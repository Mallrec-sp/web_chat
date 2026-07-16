#include "ssl_context.h"
#include "utils/logger.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

SslContext::SslContext(const std::string& certFile, const std::string& keyFile) {
    // 初始化 OpenSSL（只需要做一次）
    static bool sslInitDone = false;
    if (!sslInitDone) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        sslInitDone = true;
    }

    // 创建 SSL_CTX（TLS 服务端方法）
    ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ctx_) {
        error_ = "Failed to create SSL_CTX";
        LOG_ERROR(error_);
        return;
    }

    // 设置最低 TLS 版本（TLS 1.2 以上，禁用不安全的 SSLv2/SSLv3/TLSv1.0/TLSv1.1）
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

    // 加载证书
    if (SSL_CTX_use_certificate_file(ctx_, certFile.c_str(), SSL_FILETYPE_PEM) <= 0) {
        error_ = "Failed to load certificate: " + certFile;
        LOG_ERROR(error_ << " - " << ERR_error_string(ERR_get_error(), nullptr));
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        return;
    }

    // 加载私钥
    if (SSL_CTX_use_PrivateKey_file(ctx_, keyFile.c_str(), SSL_FILETYPE_PEM) <= 0) {
        error_ = "Failed to load private key: " + keyFile;
        LOG_ERROR(error_ << " - " << ERR_error_string(ERR_get_error(), nullptr));
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        return;
    }

    // 验证私钥是否与证书匹配
    if (!SSL_CTX_check_private_key(ctx_)) {
        error_ = "Private key does not match certificate";
        LOG_ERROR(error_);
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        return;
    }

    LOG_INFO("SSL context initialized: " << certFile);
}

SslContext::~SslContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
    }
}

SSL* SslContext::createSsl() {
    if (!ctx_) return nullptr;
    return SSL_new(ctx_);
}
