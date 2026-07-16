#ifndef NETCHAT_SOCKET_H
#define NETCHAT_SOCKET_H

#include "inetaddress.h"
#include <memory>
#include <functional>

/**
 * Socket — RAII 封装 socket 文件描述符。
 *
 * 设计思路：
 *   核心思想是 "资源获取即初始化"（RAII）。
 *   构造函数接管 fd 所有权，析构函数调用 ::close()。
 *   因为是 move-only（不支持拷贝），同一 fd 不会被关闭两次。
 *
 * 讲解点：为什么不用拷贝而用移动？
 *   Unix fd 是整数，拷贝语义下两个 Socket 对象指向同一个 fd，
 *   第一个析构时会把 fd 关掉，第二个就成了野指针（dangling fd）。
 *   移动语义把源对象的 fd 置为 -1，确保所有权唯一。
 */
class Socket {
public:
    /// 从已有 fd 构造（接管所有权）
    explicit Socket(int fd) : fd_(fd) {}

    /// 析构时关闭 fd — RAII 的核心
    ~Socket() { ::close(fd_); }

    // 禁止拷贝
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    /// 移动构造：转移所有权，源对象 fd 置 -1
    Socket(Socket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    /// 创建一个新的 TCP socket fd（SOCK_NONBLOCK | SOCK_CLOEXEC）
    static Socket create();

    void bind(const InetAddress& addr);
    void listen(int backlog = SOMAXCONN);

    /**
     * 接受新连接。
     * 使用 accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC) 替代 accept() + fcntl()。
     *
     * 讲解点：accept4 相比 accept 的好处：
     * 1. 一次系统调用代替两次（accept + fcntl 设置 O_NONBLOCK）
     * 2. 避免了竞态条件：多线程下，accept 返回后在 fcntl 设置之前，
     *    另一个线程可能已经对这个 fd 做了 epoll_ctl，而我们还没来得及设置非阻塞。
     *    accept4 在创建时就设好了标志，原子操作。
     *
     * @return 新的已连接 fd（非阻塞模式），-1 表示失败
     */
    int accept(InetAddress* peerAddr) const;

    /// 获取 fd
    int fd() const { return fd_; }

    // ---- 套接字选项 ----
    void setTcpNoDelay(bool on);
    void setReuseAddr(bool on);
    void setReusePort(bool on);
    void setKeepAlive(bool on);

    /// 半关闭写端（发送 FIN）
    void shutdownWrite();

private:
    int fd_;
};

#endif // NETCHAT_SOCKET_H
