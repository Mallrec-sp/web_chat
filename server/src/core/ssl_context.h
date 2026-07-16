#ifndef NETCHAT_SSL_CONTEXT_H
#define NETCHAT_SSL_CONTEXT_H

#include <string>
#include <memory>

struct ssl_ctx_st;
typedef struct ssl_ctx_st SSL_CTX;

struct ssl_st;
typedef struct ssl_st SSL;

/**
 * SslContext — SSL/TLS 上下文。
 *
 * 职责：
 *   1. 加载证书和私钥
 *   2. 创建 SSL 对象
 *   3. 管理 SSL_CTX 生命周期
 *
 * 讲解点：为什么需要 SSL_CTX？
 *   SSL_CTX 是 OpenSSL 的全局上下文，包含证书、私钥、
 *   加密算法套件等配置。每个连接创建一个 SSL 对象（从上下文中派生）。
 *   所有 SSL 对象共享同一个 SSL_CTX 的配置。
 *   SSL_CTX 的创建和销毁是重量级操作，应该在程序启动时
 *   完成一次，在整个生命周期复用。
 */
class SslContext {
public:
    /// 加载证书和私钥文件
    /// @param certFile PEM 格式的证书文件路径
    /// @param keyFile  PEM 格式的私钥文件路径
    SslContext(const std::string& certFile, const std::string& keyFile);
    ~SslContext();

    /// 创建新的 SSL 对象（用于单个连接）
    SSL* createSsl();

    /// 上下文是否有效
    bool valid() const { return ctx_ != nullptr; }

    /// 获取错误信息
    std::string error() const { return error_; }

private:
    SSL_CTX* ctx_ = nullptr;
    std::string error_;
};

#endif // NETCHAT_SSL_CONTEXT_H
