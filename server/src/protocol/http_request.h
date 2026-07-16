#ifndef NETCHAT_HTTP_REQUEST_H
#define NETCHAT_HTTP_REQUEST_H

#include <string>
#include <map>
#include <algorithm>

/**
 * HttpRequest — HTTP 请求的数据容器。
 *
 * 由 HttpParser 在解析过程中填充。
 * 包含请求行（Method + URL + Version）、Headers、Body。
 *
 * 设计思路：纯粹的数据对象，不包含任何解析逻辑。
 * 解析逻辑全部在 HttpParser 中。
 * 这样的分离使 HttpRequest 可以被复用和测试。
 */
class HttpRequest {
public:
    enum Method {
        kInvalid,
        kGet,
        kPost,
        kPut,
        kDelete,
        kHead,
        kOptions
    };

    enum Version {
        kUnknown,
        kHttp10,
        kHttp11
    };

    HttpRequest() { reset(); }

    /// 重置所有字段（用于 Keep-Alive 时的请求复用）
    void reset() {
        method_ = kInvalid;
        version_ = kUnknown;
        url_.clear();
        headers_.clear();
        body_.clear();
    }

    // ---- Setters（供 HttpParser 调用） ----

    void setMethod(Method m) { method_ = m; }
    void setUrl(const std::string& url) { url_ = url; }
    void setVersion(Version v) { version_ = v; }

    void setHeader(const std::string& key, const std::string& value) {
        // HTTP header names are case-insensitive, store as lowercase
        std::string lowerKey;
        lowerKey.resize(key.size());
        std::transform(key.begin(), key.end(), lowerKey.begin(), ::tolower);
        headers_[lowerKey] = value;
    }

    void setBody(const std::string& body) { body_ = body; }

    // ---- Getters ----

    Method method() const { return method_; }

    /**
     * 返回 HTTP 方法字符串。
     * 比如 GET → "GET", POST → "POST"
     * 用于日志和错误信息。
     */
    std::string methodString() const {
        switch (method_) {
            case kGet:     return "GET";
            case kPost:    return "POST";
            case kPut:     return "PUT";
            case kDelete:  return "DELETE";
            case kHead:    return "HEAD";
            case kOptions: return "OPTIONS";
            default:       return "UNKNOWN";
        }
    }

    const std::string& url() const { return url_; }
    Version version() const { return version_; }
    std::string versionString() const {
        switch (version_) {
            case kHttp10: return "HTTP/1.0";
            case kHttp11: return "HTTP/1.1";
            default:      return "HTTP/UNKNOWN";
        }
    }

    /**
     * 获取指定 header 的值。
     * 如果 header 不存在，返回空字符串。
     * 注意：内部 key 全部存储为小写，这里会自动转换。
     */
    std::string header(const std::string& key) const {
        std::string lowerKey;
        lowerKey.resize(key.size());
        std::transform(key.begin(), key.end(), lowerKey.begin(), ::tolower);
        auto it = headers_.find(lowerKey);
        return it != headers_.end() ? it->second : "";
    }

    const std::map<std::string, std::string>& headers() const { return headers_; }
    const std::string& body() const { return body_; }

    /**
     * 判断是否为长连接。
     *
     * HTTP/1.1 默认 keep-alive，除非显式 Connection: close
     * HTTP/1.0 默认 close，除非显式 Connection: Keep-Alive
     *
     * 讲解点：Keep-Alive 是 HTTP 的重要优化。
     * 它允许在同一个 TCP 连接上发送多个请求/响应，
     * 避免了反复建立连接的开销（三次握手 + TLS 握手）。
     */
    bool isKeepAlive() const {
        std::string conn = header("Connection");
        if (!conn.empty()) {
            // 转小写比较
            std::transform(conn.begin(), conn.end(), conn.begin(), ::tolower);
            if (conn == "keep-alive") return true;
            if (conn == "close") return false;
        }
        return version_ == kHttp11;  // HTTP/1.1 默认 keep-alive
    }

private:
    Method method_;
    Version version_;
    std::string url_;
    std::map<std::string, std::string> headers_;
    std::string body_;
};

#endif // NETCHAT_HTTP_REQUEST_H
