#include "server.h"
#include "eventloop.h"
#include "utils/logger.h"

#include <sys/socket.h>

Server::Server(EventLoop* loop, const InetAddress& listenAddr, const std::string& name)
    : loop_(loop)
    , name_(name)
{
    // Acceptor 负责监听新连接
    acceptor_ = std::make_unique<Acceptor>(loop_, listenAddr);
    acceptor_->setNewConnectionCallback(
        [this](int connfd, const InetAddress& peerAddr) {
            newConnection(connfd, peerAddr);
        });

    // 线程池初始化（默认单线程，兼容旧行为）
    threadPool_ = std::make_unique<EventLoopThreadPool>(loop_, name_);

    LOG_INFO("Server created: " << name_
              << " listening on " << listenAddr.toIpPort());
}

Server::~Server() {
    LOG_DEBUG("Server destructing: " << name_);

    // 在优雅关闭时，worker EventLoops 可能已经被销毁，
    // 所以不能调用 connectDestroyed()（会尝试 removeChannel 在已关闭的 epoll fd 上）。
    // 直接释放所有连接，由 ~TcpConnection 和 ~Socket 关闭真实 fd。
    // 内核会在 close(epollFd_) 时自动清理所有注册。
    connections_.clear();
}

void Server::start() {
    loop_->assertInLoopThread();

    // 启动线程池（先启动 worker loops）
    threadPool_->start(threadInitCallback_);

    // 开始监听
    acceptor_->listen();

    LOG_INFO("Server started: " << name_
              << " with " << threadPool_->threadCount() << " workers");
}

void Server::newConnection(int connfd, const InetAddress& peerAddr) {
    loop_->assertInLoopThread();

    // 生成连接名
    std::string connName = name_ + "-" + std::to_string(nextConnId_++);

    // 获取本地地址
    struct sockaddr_in local;
    socklen_t addrlen = sizeof(local);
    InetAddress localAddr;
    if (::getsockname(connfd, reinterpret_cast<struct sockaddr*>(&local), &addrlen) == 0) {
        localAddr = InetAddress(local);
    }

    /**
     * 多 Reactor 分发：
     * 从线程池中选取一个 EventLoop 来处理这个连接。
     * 此后这个连接上的所有 I/O 事件都在 selectedLoop 中处理。
     *
     * round-robin 策略确保了连接的均匀分布。
     */
    EventLoop* ioLoop = threadPool_->getNextLoop();

    LOG_INFO("New connection: " << connName << " from " << peerAddr.toIpPort()
              << " assigned to " << ioLoop->name());

    // 在 ioLoop 中创建 TcpConnection
    ioLoop->runInLoop([this, connfd, connName, localAddr, peerAddr, ioLoop]() {
        auto conn = std::make_shared<TcpConnection>(
            ioLoop, connName, connfd, localAddr, peerAddr);

        conn->setMessageCallback(messageCallback_);
        conn->setWriteCompleteCallback(writeCompleteCallback_);
        conn->setCloseCallback([this](const std::shared_ptr<TcpConnection>& c) {
            removeConnection(c);
        });

        // 保存并建立连接
        connections_[connName] = conn;
        conn->connectEstablished();

        // SSL 握手（如果配置了 SSL）
        if (sslCtx_) {
            conn->startSsl(*sslCtx_);
        }

        numConnected_.store(connections_.size());

        if (connectionCallback_) {
            connectionCallback_(conn);
        }
    });
}

void Server::removeConnection(const std::shared_ptr<TcpConnection>& conn) {
    // 用 connection 所属的 EventLoop 来执行移除
    EventLoop* ioLoop = conn->getLoop();
    ioLoop->runInLoop([this, conn]() {
        removeConnectionInLoop(conn);
    });
}

void Server::removeConnectionInLoop(const std::shared_ptr<TcpConnection>& conn) {
    size_t n = connections_.erase(conn->name());
    conn->connectDestroyed();

    numConnected_.store(connections_.size());

    if (n == 1 && connectionCallback_) {
        connectionCallback_(conn);
    }
}
