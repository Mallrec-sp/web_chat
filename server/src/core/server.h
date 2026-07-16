#ifndef NETCHAT_SERVER_H
#define NETCHAT_SERVER_H

#include "inetaddress.h"
#include "acceptor.h"
#include "tcp_connection.h"
#include "eventloop_thread_pool.h"

#include <memory>
#include <string>
#include <map>
#include <atomic>

class SslContext;

class EventLoop;

/**
 * Server — 企业级网络服务器。
 *
 * 职责：
 *   1. 管理 Acceptor（监听新连接）
 *   2. 管理 EventLoopThreadPool（多 Reactor 模式）
 *   3. 管理所有 TcpConnection（活跃连接）
 *   4. 提供用户回调：新连接、消息到达、连接关闭
 *
 * 多 Reactor 架构：
 *   MainLoop (accept 连接)
 *    └─ Acceptor
 *    └─ WorkerLoops (处理 I/O)
 *        ├─ WorkerLoop-0: TcpConnections
 *        ├─ WorkerLoop-1: TcpConnections
 *        └─ ...
 *
 *   新连接通过 round-robin 分配到 WorkerLoop。
 *   每个连接的所有 I/O 都在同一个 WorkerLoop 中处理，无需锁。
 */
class Server {
public:
    using ConnectionCallback = TcpConnection::ConnectionCallback;
    using MessageCallback = TcpConnection::MessageCallback;
    using CloseCallback = TcpConnection::CloseCallback;
    using WriteCompleteCallback = TcpConnection::WriteCompleteCallback;
    using ThreadInitCallback = EventLoopThreadPool::ThreadInitCallback;

    Server(EventLoop* loop, const InetAddress& listenAddr,
           const std::string& name = "NetChatServer");
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// 启动服务器（开始监听）
    void start();

    // ---- 多线程设置（必须在 start() 之前调用） ----

    /// 设置工作线程数（0 = 单线程模式，与之前兼容）
    void setThreadNum(int numThreads) {
        threadPool_->setThreadNum(numThreads);
    }

    /// 设置线程初始化回调
    void setThreadInitCallback(const ThreadInitCallback& cb) {
        threadInitCallback_ = cb;
    }

    /// 设置 SSL/TLS 上下文（启用加密）
    void setSslContext(SslContext* ctx) { sslCtx_ = ctx; }

    // ---- 用户回调 ----

    void setConnectionCallback(ConnectionCallback cb) {
        connectionCallback_ = std::move(cb);
    }

    void setMessageCallback(MessageCallback cb) {
        messageCallback_ = std::move(cb);
    }

    void setWriteCompleteCallback(WriteCompleteCallback cb) {
        writeCompleteCallback_ = std::move(cb);
    }

    // ---- 访问器 ----

    const std::string& name() const { return name_; }
    EventLoop* getLoop() const { return loop_; }
    EventLoopThreadPool* threadPool() const { return threadPool_.get(); }

    /// 获取当前连接数
    int connectionCount() const { return connections_.size(); }

private:
    void newConnection(int connfd, const InetAddress& peerAddr);
    void removeConnection(const std::shared_ptr<TcpConnection>& conn);
    void removeConnectionInLoop(const std::shared_ptr<TcpConnection>& conn);

    EventLoop* loop_;                    ///< 主 EventLoop（accept）
    std::string name_;
    std::unique_ptr<Acceptor> acceptor_; ///< 监听器

    std::unique_ptr<EventLoopThreadPool> threadPool_;  ///< 工作线程池
    ThreadInitCallback threadInitCallback_;
    SslContext* sslCtx_ = nullptr;  ///< SSL 上下文（可选）

    /// 所有活跃连接 <连接名, TcpConnection>
    std::map<std::string, std::shared_ptr<TcpConnection>> connections_;

    int nextConnId_ = 1;
    std::atomic<int> numConnected_{0};   ///< 当前连接数

    // ---- 用户回调 ----
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
};

#endif // NETCHAT_SERVER_H
