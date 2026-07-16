#include "socket.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/tcp.h>  // TCP_NODELAY
#include <cerrno>
#include <cstring>
#include <system_error>

Socket Socket::create() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "socket create");
    }
    return Socket(fd);
}

void Socket::bind(const InetAddress& addr) {
    const auto& sockaddr = addr.getSockAddr();
    int ret = ::bind(fd_, reinterpret_cast<const struct sockaddr*>(&sockaddr),
                     sizeof(sockaddr));
    if (ret < 0) {
        throw std::system_error(errno, std::generic_category(), "socket bind");
    }
}

void Socket::listen(int backlog) {
    int ret = ::listen(fd_, backlog);
    if (ret < 0) {
        throw std::system_error(errno, std::generic_category(), "socket listen");
    }
}

int Socket::accept(InetAddress* peerAddr) const {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    /**
     * accept4 接受新连接。
     * SOCK_NONBLOCK | SOCK_CLOEXEC 自动设置在返回的 fd 上。
     *
     * 为什么不用普通的 accept()？
     * 如果用 accept()，还需要额外调用 fcntl() 设置 O_NONBLOCK，
     * 多了一次系统调用，而且有竞态。
     */
    int connfd = ::accept4(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                           &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (connfd >= 0 && peerAddr) {
        peerAddr->setSockAddr(addr);
    }
    return connfd;
}

void Socket::shutdownWrite() {
    /**
     * shutdown(SHUT_WR) 发送 FIN 包。
     * 和 close() 的区别：
     * - close() 会释放 fd，但如果 fd 被多个进程共享（fork），
     *   只减少引用计数，不会立刻发 FIN。
     * - shutdown() 立即发送 FIN，不管引用计数。
     * - 半关闭后还可以读（对端发来的数据还能 receive）。
     */
    if (::shutdown(fd_, SHUT_WR) < 0) {
        // 如果 fd 已经关闭，这是个无害的失败
        if (errno != ENOTCONN && errno != EBADF) {
            // 记录日志，但不抛异常——shutdown 失败不应该让程序崩溃
        }
    }
}

void Socket::setTcpNoDelay(bool on) {
    /**
     * TCP_NODELAY — 禁用 Nagle 算法。
     * Nagle 算法会把小包合并成大包再发送，以减少网络中的小包数量。
     * 但对于实时通信（聊天、游戏），我们希望数据立刻发送，
     * 不要等到凑满一个 MSS。所以关掉 Nagle 算法。
     */
    int optval = on ? 1 : 0;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
}

void Socket::setReuseAddr(bool on) {
    /**
     * SO_REUSEADDR — 允许重用 TIME_WAIT 状态的端口。
     * 服务器重启时，如果上一个实例刚关闭，端口可能还在 TIME_WAIT。
     * 不设这个选项，bind 会失败（Address already in use）。
     * 所以在开发阶段强烈建议开启。
     */
    int optval = on ? 1 : 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
}

void Socket::setReusePort(bool on) {
    /**
     * SO_REUSEPORT — 多进程/多线程共享同一端口。
     * 内核会自动在多个 listener 之间分发连接。
     * 这是 nginx 等高性能服务器的常用技巧。
     * 我们第一阶段用单线程，暂时不需要，但留着以后扩展。
     */
    int optval = on ? 1 : 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
}

void Socket::setKeepAlive(bool on) {
    /**
     * SO_KEEPALIVE — TCP 保活探测。
     * 如果连接长时间没有数据交换，内核会发送探测包。
     * 默认等待 2 小时才开始探测（太久了），
     * 后续我们会用应用层的心跳机制（WebSocket Ping/Pong）替代。
     */
    int optval = on ? 1 : 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
}
