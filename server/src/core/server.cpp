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
    stop();
}

void Server::stop() {
    loop_->assertInLoopThread();
    acceptor_.reset();
    // 必须先 join；不能在 worker 仍访问连接和数据库时释放这些对象。
    threadPool_->stop();
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    connections_.clear();
    numConnected_.store(0);
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

        // 连接对象属于各自 worker，但连接索引由多个 worker 共享。
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            connections_[connName] = conn;
            numConnected_.store(connections_.size());
        }
        conn->connectEstablished();

        // SSL 握手（如果配置了 SSL）
        if (sslCtx_) {
            conn->startSsl(*sslCtx_);
        }

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
    size_t n;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        n = connections_.erase(conn->name());
        numConnected_.store(connections_.size());
    }
    conn->connectDestroyed();

    if (n == 1 && connectionCallback_) {
        connectionCallback_(conn);
    }
}
