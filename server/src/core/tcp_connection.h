#ifndef NETCHAT_TCP_CONNECTION_H
#define NETCHAT_TCP_CONNECTION_H

#include "inetaddress.h"
#include "channel.h"
#include "buffer.h"

#include <memory>
#include <string>
#include <functional>
#include <atomic>

// HttpParser 前向声明（避免所有 TcpConnection 都包含 http_parser.h）
// 如果需要 HTTP 解析，调用 conn->getHttpParser() 来获取
class HttpParser;

class EventLoop;
class Socket;

struct ssl_st;
typedef struct ssl_st SSL;
class SslContext;

/**
 * TcpConnection — 一个已建立的 TCP 连接。
 *
 * 每个连接对应一个 TcpConnection 对象，生命周期由 shared_ptr 管理。
 * 它持有：
 *   - Socket：管理连接 fd 的生命周期
 *   - Channel：处理这个 fd 的 I/O 事件
 *   - inputBuffer_：从对端收到的数据缓冲
 *   - outputBuffer_：发往对端的数据缓冲
 *
 * 设计思路：
 *   一个连接即一个 fd -> Socket(RAII) + Channel(事件分发) + 双 Buffer
 *
 * 状态机：
 *   kConnecting      → connectEstablished() → kConnected
 *   kConnected       → handleClose()         → [调用 closeCallback_]
 *   kConnected       → shutdown()             → kDisconnecting
 *   kDisconnecting   → handleWrite() 写完     → shutdownInLoop()
 *   [任意状态]       → connectDestroyed()     → kDisconnected
 *
 * 讲解点：为什么继承 std::enable_shared_from_this？
 *   因为在回调中，我们需要保证 TcpConnection 不会被意外析构。
 *   比如 handleRead 时，如果在 messageCallback 中主动调用了 shutdown，
 *   TcpConnection 可能被从 Server 的 connections_ map 中删除，
 *   shared_ptr 引用计数归零而析构。
 *   但 handleRead 中还持有 this，一旦析构就野指针了。
 *   使用 shared_from_this() 可以在回调中"保活"自己。
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    /// 连接状态
    enum State {
        kConnecting,     ///< 正在连接（尚未完全建立）
        kConnected,      ///< 已建立
        kDisconnecting,  ///< 正在关闭（半关闭状态）
        kDisconnected    ///< 已断开
    };

    /// 消息回调（收到的消息 + Buffer）
    using MessageCallback = std::function<void(const std::shared_ptr<TcpConnection>&, Buffer*)>;

    /// 连接事件回调（建立/关闭）
    using ConnectionCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;

    /// 连接关闭回调
    using CloseCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;

    /// 写完成回调（outputBuffer 全部写完时调用）
    using WriteCompleteCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;

    /// 高水位回调（outputBuffer 超过阈值时调用，用于流控）
    using HighWaterMarkCallback = std::function<void(const std::shared_ptr<TcpConnection>&, size_t)>;

    TcpConnection(EventLoop* loop,
                  const std::string& name,
                  int sockfd,
                  const InetAddress& localAddr,
                  const InetAddress& peerAddr);
    ~TcpConnection();

    // ---- 访问器 ----

    EventLoop* getLoop() const { return loop_; }
    const std::string& name() const { return name_; }
    State state() const { return state_; }
    const InetAddress& localAddr() const { return localAddr_; }
    const InetAddress& peerAddr() const { return peerAddr_; }

    // ---- I/O 操作 ----

    /**
     * send — 发送数据。
     *
     * 线程安全：从任意线程调用都可以。
     * 实现逻辑：
     * - 如果在 EventLoop 线程，直接 sendInLoop
     * - 如果不在 EventLoop 线程，queueInLoop 转发到 EventLoop 线程
     *
     * 讲解点：为什么要跨线程转发？
     *   如果允许在任意线程写 outputBuffer_，就需要加锁保护。
     *   而加锁会影响 EventLoop 线程的性能（每次写事件都要抢锁）。
     *   所以我们把所有的 I/O 操作都集中到 EventLoop 线程执行。
     *   不是 EventLoop 线程的调用者，通过 queueInLoop 把写操作"快递"过去。
     *   这是"One Loop Per Thread"的核心理念——一个循环里啥也别抢。
     */
    void send(const std::string& message);
    void send(const char* data, size_t len);

    /// 半关闭写端（发送 FIN 后仍可读）
    void shutdown();

    /// 启动 SSL/TLS 握手（服务端模式）
    /// 必须在 connectEstablished() 之后调用
    void startSsl(SslContext& ctx);

    // ---- 缓冲区与 HTTP 解析 ----

    /// 获取 HttpParser 指针（HTTP 协议解析用，可为 null）
    HttpParser* getHttpParser() const { return httpParser_.get(); }

    /// 重置/创建 HttpParser（在 .cpp 中实现，避免 forward-decl 问题）
    void resetHttpParser();

    // ---- WebSocket 模式 ----

    /// 切换到 WebSocket 模式（后续数据解析为 WebSocket 帧）
    void switchToWebSocket(std::function<void(Buffer*, TcpConnection*)> frameHandler) {
        wsMode_ = true;
        wsFrameHandler_ = std::move(frameHandler);
    }

    /// 设置 WebSocket 连接关闭回调（异常断开时清理在线状态）
    void setWsCloseCallback(std::function<void()> cb) {
        wsCloseCallback_ = std::move(cb);
    }

    /// 是否处于 WebSocket 模式
    bool isWebSocketMode() const { return wsMode_; }

    Buffer* inputBuffer() { return inputBuffer_.get(); }
    Buffer* outputBuffer() { return outputBuffer_.get(); }

    // ---- 回调设置 ----

    void setMessageCallback(MessageCallback cb) { messageCallback_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { closeCallback_ = std::move(cb); }
    void setWriteCompleteCallback(WriteCompleteCallback cb) { writeCompleteCallback_ = std::move(cb); }
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, size_t highWaterMark) {
        highWaterMarkCallback_ = std::move(cb);
        highWaterMark_ = highWaterMark;
    }

    // ---- 生命周期（由 Server 调用） ----

    /// 连接建立：设置状态为 Connected，enableReading
    void connectEstablished();

    /// 连接销毁：清理所有资源，从 epoll 移除，关闭 fd
    void connectDestroyed();

private:
    // ---- 事件处理 ----

    /**
     * handleRead — 读事件处理。
     *
     * 1. 调用 inputBuffer_->readFd() 从 socket 读取数据
     * 2. 根据返回值处理：
     *    - n > 0：调用 messageCallback_
     *    - n == 0：对端关闭，调用 handleClose
     *    - n < 0：错误，调用 handleError
     *
     * 注意：Buffer::readFd 内部使用 readv 双缓冲，
     * 但这里和 ET 模式仍然需要在外部循环读？
     * 不，Buffer::readFd 已经在一个 readv 调用中尽量多地读取了数据。
     * 如果使用 ET，我们需要确保 readFd 读到 EAGAIN。
     * 而我们的 readFd 会在 readv 返回 EAGAIN 时返回 0，
     * 所以这里只需要调用一次 readFd。
     *
     * 等一下——上面这个说法不对。ET 要求循环读直到 EAGAIN。
     * 但我们的 Buffer::readFd 只调用了一次 readv。
     * 如果要严格遵循 ET，我们应在 handleRead 中循环 readFd。
     */
    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();
    void handleSslHandshake();

    // ---- 内部 I/O ----

    void setState(State s) { state_ = s; }

    void sendInLoop(const std::string& message);
    void sendInLoop(const char* data, size_t len);
    void shutdownInLoop();

    // ---- 成员变量 ----

    EventLoop* loop_;
    std::string name_;
    std::atomic<State> state_{kConnecting};

    std::unique_ptr<Socket> socket_;        ///< 管理 fd 生命周期
    std::unique_ptr<Channel> channel_;      ///< 在 EventLoop 中注册 fd 事件
    std::unique_ptr<Buffer> inputBuffer_;   ///< 读缓冲
    std::unique_ptr<Buffer> outputBuffer_;  ///< 写缓冲
    std::unique_ptr<HttpParser> httpParser_; ///< HTTP 解析器（可选）
    bool wsMode_ = false;                    ///< WebSocket 模式标志
    std::function<void(Buffer*, TcpConnection*)> wsFrameHandler_; ///< WebSocket 帧回调
    std::function<void()> wsCloseCallback_;   ///< WebSocket 断开回调（异常断连时清理在线状态）
    SSL* ssl_ = nullptr;                     ///< SSL 对象（可选）
    bool sslHandshakeDone_ = false;          ///< SSL 握手是否完成

    InetAddress localAddr_;                 ///< 本端地址
    InetAddress peerAddr_;                  ///< 对端地址

    // ---- 回调 ----

    MessageCallback messageCallback_;
    CloseCallback closeCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    size_t highWaterMark_ = 0;
};

#endif // NETCHAT_TCP_CONNECTION_H
