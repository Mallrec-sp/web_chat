#ifndef NETCHAT_ACCEPTOR_H
#define NETCHAT_ACCEPTOR_H

#include <functional>
#include <memory>

class EventLoop;
class Channel;
class Socket;
class InetAddress;

/**
 * Acceptor — TCP 监听器。
 *
 * 职责：
 *   1. 创建一个 listen fd（Socket），绑定到指定地址+端口
 *   2. 把 listen fd 注册到 EventLoop 的 epoll 中
 *   3. 当 listen fd 可读时（有新连接），调用 accept() 接受连接
 *   4. 通过 NewConnectionCallback 把新连接 fd 传给上层（Server）
 *
 * 设计要点：
 *   - Acceptor 本身就是一个"特殊"的 Channel
 *   - 它的 Channel 只关心 EPOLLIN 事件
 *   - EPOLLIN 触发时，循环 accept 直到 EAGAIN（ET 模式的铁律）
 *
 * 讲解点：监听套接字的 ET 处理
 *   对于 listen fd，ET 模式意味着：
 *   一次可读事件可能对应多个新连接。必须一直 accept() 直到返回 EAGAIN。
 *   如果在循环中 accept() 返回 EMFILE（进程 fd 耗尽），
 *   我们需要特殊处理——经典做法是预留一个"idle fd"，
 *   遇到 EMFILE 时关闭 idle fd，accept()，再立刻 close 掉这个新连接，
 *   最后 reopen idle fd。这样可以避免因为 fd 满了而漏掉新连接。
 *   我们第一阶段先不实现这个优化，记录日志即可。
 */
class Acceptor {
public:
    using NewConnectionCallback = std::function<void(int connfd, const InetAddress& peerAddr)>;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reusePort = false);
    ~Acceptor();

    /// 设置新连接回调（由 Server 设置）
    void setNewConnectionCallback(NewConnectionCallback cb) {
        newConnectionCallback_ = std::move(cb);
    }

    /// 是否正在监听
    bool listening() const { return listening_; }

    /// 开始监听——bind + listen + 在 EventLoop 中注册可读事件
    void listen();

private:
    /**
     * handleRead — 监听 fd 可读时的处理函数。
     *
     * ET 模式下的 accept 循环伪代码：
     *   while (connfd = accept4(listenFd, ...) > 0) {
     *     newConnectionCallback_(connfd, peerAddr);
     *   }
     *   if (errno == EAGAIN) break;  // 没有更多连接了
     *   if (errno == EMFILE) { ... }  // fd 耗尽，特殊处理
     */
    void handleRead();

    EventLoop* loop_;                        ///< 所属 EventLoop
    std::unique_ptr<Socket> acceptSocket_;   ///< 监听套接字（RAII 管理 listen fd）
    std::unique_ptr<Channel> acceptChannel_; ///< 监听套接字的 Channel
    NewConnectionCallback newConnectionCallback_;
    bool listening_ = false;
};

#endif // NETCHAT_ACCEPTOR_H
