#ifndef NETCHAT_HTTP_RESPONSE_H
#define NETCHAT_HTTP_RESPONSE_H

#include <string>
#include <map>

/**
 * HttpResponse — HTTP 响应构建器。
 *
 * 使用 Builder 模式构建响应：
 *   设置 StatusCode → 设置 Header → 设置 Body → toString() 序列化
 *
 * 也提供静态工厂方法快速创建常见响应：
 *   HttpResponse::ok("body")
 *   HttpResponse::notFound()
 *   HttpResponse::badRequest()
 *   HttpResponse::internalError()
 */
class HttpResponse {
public:
    enum StatusCode {
        kUnknown   = 0,
        k200       = 200,
        k201       = 201,
        k204       = 204,
        k301       = 301,
        k302       = 302,
        k400       = 400,
        k401       = 401,
        k403       = 403,
        k404       = 404,
        k405       = 405,
        k500       = 500
    };

    HttpResponse() { reset(); }

    /// 重置所有字段
    void reset() {
        statusCode_ = kUnknown;
        statusMessage_.clear();
        headers_.clear();
        body_.clear();
        keepAlive_ = true;
    }

    // ---- Setters ----

    void setStatusCode(StatusCode code) { statusCode_ = code; }
    void setStatusMessage(const std::string& message) { statusMessage_ = message; }
    void setContentType(const std::string& type) { headers_["Content-Type"] = type; }
    void setBody(const std::string& body) { body_ = body; }

    void setHeader(const std::string& key, const std::string& value) {
        headers_[key] = value;
    }

    void setKeepAlive(bool on) { keepAlive_ = on; }

    /**
     * toString — 序列化为 HTTP 响应字符串（wire format）。
     *
     * 输出格式：
     *   HTTP/1.1 200 OK\r\n
     *   Content-Type: text/plain\r\n
     *   Content-Length: 11\r\n
     *   \r\n
     *   Hello World
     *
     * 讲解点：为什么响应头顺序不重要？
     *   HTTP 协议规定 header 顺序不影响语义。
     *   所以我们可以自由安排顺序，只需要保证：
     *   1. 状态行在第一行
     *   2. headers 每行一个
     *   3. 空行分隔 headers 和 body
     *   Content-Length 是必须的（除非用 chunked 编码），
     *   告诉客户端 body 的长度，客户端才知道何时读完。
     */
    std::string toString() const {
        std::string response;

        // 状态行
        response += "HTTP/1.1 ";
        response += std::to_string(statusCode_);
        response += " ";
        response += statusMessage_;
        response += "\r\n";

        // Content-Length（优先于其他 header）
        response += "Content-Length: " + std::to_string(body_.size()) + "\r\n";

        // Connection
        response += "Connection: ";
        response += keepAlive_ ? "Keep-Alive" : "Close";
        response += "\r\n";

        // 其他 headers
        for (const auto& [key, value] : headers_) {
            // 跳过已经在上面单独处理的 header
            if (key == "Content-Length" || key == "Connection") continue;
            response += key + ": " + value + "\r\n";
        }

        // 空行分隔 headers 和 body
        response += "\r\n";

        // Body
        response += body_;

        return response;
    }

    // ========== 静态工厂方法 ==========

    static std::string statusMessage(StatusCode code) {
        switch (code) {
            case k200: return "OK";
            case k201: return "Created";
            case k204: return "No Content";
            case k301: return "Moved Permanently";
            case k302: return "Found";
            case k400: return "Bad Request";
            case k401: return "Unauthorized";
            case k403: return "Forbidden";
            case k404: return "Not Found";
            case k405: return "Method Not Allowed";
            case k500: return "Internal Server Error";
            default:   return "Unknown";
        }
    }

    static HttpResponse ok(const std::string& body,
                           const std::string& contentType = "text/plain") {
        HttpResponse resp;
        resp.setStatusCode(k200);
        resp.setStatusMessage(statusMessage(k200));
        resp.setContentType(contentType);
        resp.setBody(body);
        return resp;
    }

    static HttpResponse notFound(const std::string& body = "404 Not Found") {
        HttpResponse resp;
        resp.setStatusCode(k404);
        resp.setStatusMessage(statusMessage(k404));
        resp.setContentType("text/plain");
        resp.setBody(body);
        resp.setKeepAlive(false);
        return resp;
    }

    static HttpResponse badRequest(const std::string& body = "400 Bad Request") {
        HttpResponse resp;
        resp.setStatusCode(k400);
        resp.setStatusMessage(statusMessage(k400));
        resp.setContentType("text/plain");
        resp.setBody(body);
        resp.setKeepAlive(false);
        return resp;
    }

    static HttpResponse internalError(const std::string& body = "500 Internal Server Error") {
        HttpResponse resp;
        resp.setStatusCode(k500);
        resp.setStatusMessage(statusMessage(k500));
        resp.setContentType("text/plain");
        resp.setBody(body);
        resp.setKeepAlive(false);
        return resp;
    }

    // ---- Accessors ----

    StatusCode statusCode() const { return statusCode_; }
    const std::string& body() const { return body_; }
    bool keepAlive() const { return keepAlive_; }

private:
    StatusCode statusCode_;
    std::string statusMessage_;
    std::map<std::string, std::string> headers_;
    std::string body_;
    bool keepAlive_ = true;
};

#endif // NETCHAT_HTTP_RESPONSE_H
