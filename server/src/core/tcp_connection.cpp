#include "tcp_connection.h"
#include "socket.h"
#include "eventloop.h"
#include "ssl_context.h"
#include "protocol/http_parser.h"
#include "utils/logger.h"

#include <cstring>
#include <cerrno>
#include <openssl/ssl.h>
#include <openssl/err.h>

TcpConnection::TcpConnection(EventLoop* loop,
                             const std::string& name,
                             int sockfd,
                             const InetAddress& localAddr,
                             const InetAddress& peerAddr)
    : loop_(loop)
    , name_(name)
    , state_(kConnecting)
    , localAddr_(localAddr)
    , peerAddr_(peerAddr)
{
    // 用已有的 sockfd 创建 Socket（接管 fd 所有权）
    socket_ = std::make_unique<Socket>(sockfd);
    socket_->setTcpNoDelay(true);  // 禁用 Nagle 算法，减少延迟

    // 创建 Channel 并设置回调
    channel_ = std::make_unique<Channel>(loop_, sockfd);
    channel_->setReadCallback([this] { handleRead(); });
    channel_->setWriteCallback([this] { handleWrite(); });
    channel_->setCloseCallback([this] { handleClose(); });
    channel_->setErrorCallback([this] { handleError(); });

    // 分配输入输出缓冲区
    inputBuffer_ = std::make_unique<Buffer>();
    outputBuffer_ = std::make_unique<Buffer>();

    LOG_DEBUG("TcpConnection created: " << name_
              << " [" << peerAddr_.toIpPort() << "]");
}

TcpConnection::~TcpConnection() {
    LOG_DEBUG("TcpConnection destroyed: " << name_);
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
}

void TcpConnection::resetHttpParser() {
    if (!httpParser_) {
        httpParser_ = std::make_unique<HttpParser>();
    } else {
        httpParser_->reset();
    }
}

// ==================== I/O 操作 ====================

void TcpConnection::send(const std::string& message) {
    send(message.data(), message.size());
}

void TcpConnection::send(const char* data, size_t len) {
    if (state_ != kConnected) {
        LOG_WARNING("send on " << name_ << " but state is not connected");
        return;
    }

    /**
     * 线程安全：通过 runInLoop 转发到 EventLoop 线程。
     *
     * 讲解点：这里捕获了 shared_from_this() 来保活。
     * 因为 lambda 是异步执行的，当它执行时 TcpConnection
     * 可能已经被销毁了。shared_from_this() 创建了一个
     * 额外的 shared_ptr，确保回调执行期间对象还活着。
     */
    if (loop_->isInLoopThread()) {
        sendInLoop(data, len);
    } else {
        loop_->runInLoop([this, data_copy = std::string(data, len)]() {
            if (state_ == kConnected) {
                sendInLoop(data_copy);
            }
        });
    }
}

void TcpConnection::shutdown() {
    if (state_ == kConnected) {
        setState(kDisconnecting);
        loop_->runInLoop([this] { shutdownInLoop(); });
    }
}

// ==================== 生命周期 ====================

void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    setState(kConnected);

    /**
     * enableReading → 在 epoll 中注册 EPOLLIN 事件。
     * 注意：我们用的是 EPOLLIN | EPOLLPRI，没有 EPOLLET，
     * 所以目前是 LT 模式。
     *
     * 后续如果要改成 ET，需要在 channel->enableReading() 之前
     * 修改 kReadEvent 常量，加上 EPOLLET 标志。
     *
     * 但 ET 模式下需要小心：
     * 1. 所有 I/O 都要在循环中读写直到 EAGAIN
     * 2. 必须在 eventLoop 中处理 EPOLLONESHOT
     * 第一阶段我们先用 LT 模式确保功能正确。
     */
    channel_->enableReading();

    LOG_INFO("Connection UP: " << name_);
}

void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();

    if (state_ == kConnected) {
        setState(kDisconnected);
        channel_->disableAll();
    }

    // 从 EventLoop 中移除 Channel（不关闭 fd，Socket 析构时会 close）
    loop_->removeChannel(channel_.get());
    LOG_INFO("Connection DOWN: " << name_);
}

// ==================== 事件处理 ====================

/**
 * handleRead — 当 socket 可读时调用。
 *
 * 读数据 → 解数据 → 调回调
 *
 * 讲解点：为什么这里不循环读取直到 EAGAIN？
 * 如果是 LT 模式，一次 EPOLLIN 事件只对应一次可读通知，
 * 不循环 read 也没问题——下次 epoll_wait 还会通知。
 * 但如果是 ET 模式就必须循环。
 *
 * 当前我们用的是 LT 模式（未设置 EPOLLET），所以一次 readFd 就够了。
 */
void TcpConnection::handleRead() {
    loop_->assertInLoopThread();

    // SSL 握手未完成时，继续握手
    if (ssl_ && !sslHandshakeDone_) {
        handleSslHandshake();
        return;
    }

    // 第一步：从 socket 读取数据到 inputBuffer_
    ssize_t n;
    if (ssl_ && sslHandshakeDone_) {
        // SSL 模式：使用 SSL_read
        char buf[65536];
        ERR_clear_error();
        n = SSL_read(ssl_, buf, sizeof(buf));
        if (n > 0) {
            inputBuffer_->append(buf, n);
        } else if (n == 0) {
            handleClose();
            return;
        } else {
            int err = SSL_get_error(ssl_, n);
            switch (err) {
                case SSL_ERROR_WANT_READ:
                    return;
                case SSL_ERROR_WANT_WRITE:
                    channel_->enableWriting();
                    return;
                default:
                    handleError();
                    return;
            }
        }
    } else {
        // 非 SSL 模式：从 socket 读数据到 inputBuffer
        // 使用 readv 双缓冲优化
        n = inputBuffer_->readFd(socket_->fd());
        if (n == 0) {
            // 对端关闭连接（read 返回 0 = FIN 已收到）
            handleClose();
            return;
        }
        if (n < 0) {
            // read 出错
            handleError();
            return;
        }
    }

    // 第二步：数据已在 inputBuffer_ 中，根据模式分发
    if (wsMode_ && wsFrameHandler_) {
        // WebSocket 模式：帧处理器解析 inputBuffer_
        wsFrameHandler_(inputBuffer_.get(), this);
    } else if (messageCallback_) {
        // HTTP/原始模式：上层回调处理
        messageCallback_(shared_from_this(), inputBuffer_.get());
    }
}

/**
 * handleWrite — 当 socket 可写时调用。
 *
 * 把 outputBuffer_ 中的数据写到 socket。
 * 如果数据全部写完：
 * 1. 禁写 EPOLLOUT（如果不禁用，epoll 会一直通知可写，浪费 CPU）
 * 2. 调用 writeCompleteCallback_
 * 3. 如果是半关闭状态（kDisconnecting），执行真正的 shutdown
 */
void TcpConnection::handleWrite() {
    loop_->assertInLoopThread();

    // SSL 握手模式
    if (ssl_ && !sslHandshakeDone_) {
        handleSslHandshake();
        return;
    }

    if (channel_->isWriting()) {
        // 没有数据要写时，禁用 EPOLLOUT
        if (outputBuffer_->readableBytes() == 0) {
            channel_->disableWriting();
            if (state_ == kDisconnecting) {
                shutdownInLoop();
            }
            return;
        }

        ssize_t n;
        if (ssl_ && sslHandshakeDone_) {
            // SSL 模式：使用 SSL_write
            ERR_clear_error();
            n = SSL_write(ssl_, outputBuffer_->peek(), outputBuffer_->readableBytes());
            if (n > 0) {
                outputBuffer_->retrieve(n);
            } else {
                int err = SSL_get_error(ssl_, n);
                if (err == SSL_ERROR_WANT_WRITE) {
                    n = 0;  // 继续等待 EPOLLOUT
                } else {
                    LOG_ERROR("SSL_write error in handleWrite: " << name_);
                    return;
                }
            }
        } else {
            // 非 SSL 模式：使用 writeFd
            n = outputBuffer_->writeFd(socket_->fd());
        }
        if (n > 0) {
            if (outputBuffer_->readableBytes() == 0) {
                // 全部写入完毕
                channel_->disableWriting();

                if (writeCompleteCallback_) {
                    // 注意这里通过 loop_ 执行，避免递归
                    loop_->queueInLoop(
                        [this_shared = shared_from_this()]() {
                            if (this_shared->writeCompleteCallback_) {
                                this_shared->writeCompleteCallback_(this_shared);
                            }
                        });
                }

                // 如果是半关闭状态，现在关闭连接
                if (state_ == kDisconnecting) {
                    shutdownInLoop();
                }
            }
        } else {
            LOG_ERROR("TcpConnection::handleWrite error: " << strerror(errno));
        }
    } else {
        LOG_DEBUG("Connection fd=" << socket_->fd()
                  << " is down, no more writing");
    }
}

/**
 * handleClose — 对端关闭连接时的处理。
 *
 * 不直接在这里销毁对象，而是通过 closeCallback_ 通知 Server。
 * Server 会在下一个 tick 调用 connectDestroyed() 来清理。
 *
 * 这样做的原因：Server 持有 connections_ 的 shared_ptr，
 * Server 需要从 map 中 erase 后才能释放引用计数。
 */
void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
    LOG_INFO("Connection CLOSE: " << name_ << " [" << peerAddr_.toIpPort() << "]");
    setState(kDisconnected);
    channel_->disableAll();

    // WebSocket 异常断开清理（如 TCP 断连未走 WS Close 帧）
    if (wsCloseCallback_) {
        wsCloseCallback_();
        wsCloseCallback_ = nullptr;
    }

    if (closeCallback_) {
        closeCallback_(shared_from_this());
    }
}

void TcpConnection::handleError() {
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (::getsockopt(socket_->fd(), SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
        err = errno;
    }
    if (err == 0) {
        // 没有具体错误，可能是对端正常关闭导致的
        return;
    }
    LOG_ERROR("TcpConnection::handleError: " << name_
              << " error: " << strerror(err));
}

// ==================== 内部 I/O ====================

/**
 * sendInLoop — 在 EventLoop 线程中执行的实际发送。
 *
 * 两种策略（优化小包发送）：
 * 1. 如果 outputBuffer 为空（没有累积待发数据），
 *    尝试直接 write 到 socket（避免唤醒 EPOLLOUT 事件）
 * 2. 如果一次 write 没写完，或者本来就有积压数据，
 *    把数据 append 到 outputBuffer 并启用 EPOLLOUT
 */
void TcpConnection::sendInLoop(const char* data, size_t len) {
    loop_->assertInLoopThread();

    size_t remaining = len;
    ssize_t written = 0;

    // SSL 模式：使用 SSL_write 直接写入
    if (ssl_ && sslHandshakeDone_) {
        ERR_clear_error();
        int n = SSL_write(ssl_, data, len);
        if (n > 0) {
            remaining = len - n;
            if (remaining == 0 && writeCompleteCallback_) {
                loop_->queueInLoop(
                    [this_shared = shared_from_this()]() {
                        if (this_shared->writeCompleteCallback_) {
                            this_shared->writeCompleteCallback_(this_shared);
                        }
                    });
                return;
            }
            // SSL_write 只写入了部分数据，剩余数据放入缓冲区
            outputBuffer_->append(data + n, remaining);
            if (!channel_->isWriting()) {
                channel_->enableWriting();
            }
            return;
        } else {
            int err = SSL_get_error(ssl_, n);
            if (err == SSL_ERROR_WANT_WRITE) {
                // 暂不可写，放入缓冲区等待 EPOLLOUT
                outputBuffer_->append(data, len);
                if (!channel_->isWriting()) {
                    channel_->enableWriting();
                }
                return;
            } else {
                LOG_ERROR("SSL_write error: " << name_);
                return;
            }
        }
    }

    // 优化：如果 outputBuffer 为空且目前没有 EPOLLOUT 事件，
    // 尝试直接 write（零拷贝路径，不走 Epoll）
    if (!channel_->isWriting() && outputBuffer_->readableBytes() == 0) {
        written = ::write(socket_->fd(), data, len);
        if (written >= 0) {
            remaining = len - written;
            if (remaining == 0 && writeCompleteCallback_) {
                // 一次写完所有数据，通知上层
                loop_->queueInLoop(
                    [this_shared = shared_from_this()]() {
                        if (this_shared->writeCompleteCallback_) {
                            this_shared->writeCompleteCallback_(this_shared);
                        }
                    });
            }
        } else {
            // write 失败
            if (errno == EWOULDBLOCK) {
                // 不是真正错误，只是缓冲区满
                written = 0;
            } else {
                LOG_ERROR("sendInLoop write error: " << strerror(errno));
                return;
            }
        }
    }

    // 有未完成的数据，追加到 outputBuffer
    if (remaining > 0) {
        size_t oldLen = outputBuffer_->readableBytes();
        if (oldLen + remaining >= highWaterMark_
            && oldLen < highWaterMark_
            && highWaterMarkCallback_) {
            // 超过高水位线，通知上层
            loop_->queueInLoop(
                [this_shared = shared_from_this(), oldLen, remaining]() {
                    if (this_shared->highWaterMarkCallback_) {
                        this_shared->highWaterMarkCallback_(this_shared,
                                                            oldLen + remaining);
                    }
                });
        }

        outputBuffer_->append(static_cast<const char*>(data) + written, remaining);
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
}

void TcpConnection::sendInLoop(const std::string& message) {
    sendInLoop(message.data(), message.size());
}

void TcpConnection::shutdownInLoop() {
    loop_->assertInLoopThread();

    if (!channel_->isWriting()) {
        // 没有待写数据了，直接半关闭
        if (ssl_) {
            SSL_shutdown(ssl_);
        }
        socket_->shutdownWrite();
    }
    // 如果有待写数据，等 handleWrite 写完再调用 shutdownInLoop
}

// ==================== SSL/TLS ====================

void TcpConnection::startSsl(SslContext& ctx) {
    loop_->assertInLoopThread();

    // 从上下文中创建 SSL 对象
    ssl_ = ctx.createSsl();
    if (!ssl_) {
        LOG_ERROR("Failed to create SSL object");
        handleClose();
        return;
    }

    // 关联 socket fd
    SSL_set_fd(ssl_, socket_->fd());
    // 设置为服务端模式（接受客户端连接）
    SSL_set_accept_state(ssl_);

    sslHandshakeDone_ = false;

    // 注册写事件以开始握手（发送 ServerHello）
    channel_->enableWriting();

    LOG_DEBUG("SSL handshake started: " << name_);
}

void TcpConnection::handleSslHandshake() {
    int ret = SSL_do_handshake(ssl_);
    if (ret == 1) {
        // 握手成功！
        sslHandshakeDone_ = true;
        channel_->disableWriting();

        // 输出加密信息
        const char* cipher = SSL_get_cipher(ssl_);
        int ver = SSL_version(ssl_);
        LOG_INFO("SSL handshake complete: " << name_
                 << " cipher=" << (cipher ? cipher : "unknown")
                 << " version=" << ver);

        // 握手完成后，开始读取数据
        channel_->enableReading();
        return;
    }

    // 处理握手尚未完成的情况
    int err = SSL_get_error(ssl_, ret);
    switch (err) {
        case SSL_ERROR_WANT_READ:
            // 需要等待可读事件
            channel_->disableWriting();
            channel_->enableReading();
            break;
        case SSL_ERROR_WANT_WRITE:
            // 需要继续等待可写事件
            channel_->enableWriting();
            channel_->disableReading();
            break;
        default: {
            // 握手失败
            char buf[256];
            ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
            LOG_ERROR("SSL handshake failed: " << name_ << " error: " << buf);
            handleClose();
            break;
        }
    }
}
