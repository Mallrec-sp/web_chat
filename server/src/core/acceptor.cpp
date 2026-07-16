#include "acceptor.h"
#include "eventloop.h"
#include "socket.h"
#include "inetaddress.h"
#include "channel.h"
#include "utils/logger.h"

#include <sys/socket.h>
#include <cerrno>
#include <cstring>

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reusePort)
    : loop_(loop)
{
    // 创建 listen fd
    acceptSocket_ = std::make_unique<Socket>(Socket::create());

    // 设置 REUSEADDR，避免 "Address already in use"
    acceptSocket_->setReuseAddr(true);
    acceptSocket_->setReusePort(reusePort);

    // bind + listen（先 bind 再 listen）
    acceptSocket_->bind(listenAddr);

    // 创建 Channel 关联 listen fd
    acceptChannel_ = std::make_unique<Channel>(loop_, acceptSocket_->fd());
    acceptChannel_->setReadCallback([this] { handleRead(); });

    LOG_INFO("Acceptor created on " << listenAddr.toIpPort());
}

Acceptor::~Acceptor() {
    acceptChannel_->disableAll();
    loop_->removeChannel(acceptChannel_.get());
    LOG_DEBUG("Acceptor destroyed");
}

void Acceptor::listen() {
    loop_->assertInLoopThread();
    acceptSocket_->listen();
    listening_ = true;

    /**
     * enableReading → 注册 EPOLLIN 事件到 EventLoop 的 epoll。
     * 注意：这里没有启用 EPOLLET（边缘触发）。
     *
     * 讲解点：listen fd 用 LT 还是 ET？
     * 对于 listen fd，用 LT 也没有关系——因为 accept() 本身是轻量操作。
     * 但在本项目中我们统一使用 ET 模式以保持一致。
     * 实际上我们的 Channel 默认注册的就是 EPOLLIN（不是 EPOLLET），
     * 所以监听套接字目前是 LT 模式。
     *
     * 要不要改成 ET？对于 listen fd 来说区别不大。
     * LT：只要有未 accept 的连接，epoll_wait 就会一直返回
     * ET：只有新连接到来时才返回，必须循环 accept
     * 两种模式在功能上没有区别，只是 ET 稍微高效一点（少几次 epoll 通知）。
     * 后续如果想改成 ET，在 kReadEvent 中加上 EPOLLET 即可。
     */
    acceptChannel_->enableReading();

    LOG_INFO("Acceptor listening on " << acceptSocket_->fd());
}

void Acceptor::handleRead() {
    loop_->assertInLoopThread();
    InetAddress peerAddr;

    /**
     * accept 循环（ET 一致性）。
     *
     * 虽然 listen fd 是 LT 模式，但还是用循环 accept 更安全。
     * 如果后续改成 ET，循环就是必需的，现在先写对逻辑。
     *
     * 注意：当 accept4 返回 -1 时有两种情况：
     * - EAGAIN / EWOULDBLOCK：没有更多连接了，正常退出循环
     * - EMFILE：进程 fd 耗尽，需要特殊处理
     */
    while (true) {
        int connfd = acceptSocket_->accept(&peerAddr);
        if (connfd >= 0) {
            if (newConnectionCallback_) {
                newConnectionCallback_(connfd, peerAddr);
            } else {
                // 没人处理这个连接，直接关闭
                ::close(connfd);
            }
        } else {
            if (errno == EAGAIN) {
                // 没有更多待处理的连接了
                break;
            } else if (errno == EMFILE || errno == ENFILE) {
                /**
                 * fd 耗尽的情况。
                 * 经典处理方式是用一个 idle fd：
                 * 1. 预留一个 fd 在程序启动时打开
                 * 2. 遇到 EMFILE 时关闭 idle fd→腾出一个位置
                 * 3. accept() 得到新连接
                 * 4. 立即 close 这个新连接（因为服务不了）
                 * 5. 重新打开 idle fd 占住那个位置
                 * 这样可以避免因为 fd 满而导致 accept 永远失败。
                 * 我们第一阶段暂时不实现，记录日志即可。
                 */
                LOG_ERROR("accept failed: " << strerror(errno)
                          << " (too many open files)");
                break;
            } else {
                LOG_ERROR("accept failed: " << strerror(errno));
                break;
            }
        }
    }
}
