#ifndef NETCHAT_HTTP_PARSER_H
#define NETCHAT_HTTP_PARSER_H

#include "http_request.h"
#include "core/buffer.h"

#include <string>
#include <cstdlib>

/**
 * HttpParser — HTTP/1.1 请求解析器（有限状态机）。
 *
 * 状态迁移：
 *   kExpectRequestLine → kExpectHeaders → kExpectBody → kGotAll
 *                          ↓ (解析错误)
 *                        kError
 *
 * 设计思路：
 *   这是一个"行级"解析器，不是"字符级"FSM。
 *   它利用 Buffer::findCRLF() 快速定位行边界，
 *   然后用字符串操作解析每一行的内容。
 *
 * 讲解点：为什么要做成状态机？
 *   HTTP 是一个"流式"协议。数据可能分批到达：
 *   第一次 read： "GET /api/hello HTTP/1.1\r\nHost:"
 *   第二次 read： " localhost\r\n\r\n"
 *   状态机可以在收到部分数据时暂停，等更多数据到达后继续。
 *   这就是"流式解析"（streaming parsing）的核心思想。
 *   如果假设数据一次性到齐，遇到大包或慢连接就会出错。
 */
class HttpParser {
public:
    enum class State {
        kExpectRequestLine,  ///< 等待请求行："GET /path HTTP/1.1\r\n"
        kExpectHeaders,      ///< 等待头部行："Key: Value\r\n"
        kExpectBody,         ///< 等待请求体
        kGotAll,             ///< 完整请求已解析完毕
        kError               ///< 解析出错
    };

    HttpParser() { reset(); }

    /// 重置解析器（Keep-Alive 复用时调用）
    void reset() {
        state_ = State::kExpectRequestLine;
        request_.reset();
        contentLength_ = 0;
        bodyReceived_ = 0;
        errorMessage_.clear();
    }

    State state() const { return state_; }
    const std::string& errorMessage() const { return errorMessage_; }

    /// 获取已解析的请求（parse 返回 true 后调用）
    const HttpRequest& request() const { return request_; }
    HttpRequest& request() { return request_; }

    /**
     * parse — 从缓冲区解析 HTTP 请求。
     *
     * @param buf 输入缓冲区（未消费的数据）
     * @return true 表示一个完整的请求已就绪（kGotAll），
     *         false 表示需要更多数据或出错。
     *
     * 调用方式：
     *   while (parser.parse(&inputBuffer)) {
     *       handleRequest(parser.request());
     *       parser.reset();  // 准备解析下一个请求（Pipeline）
     *   }
     *   if (parser.state() == HttpParser::State::kError) {
     *       // 返回 400 Bad Request 并关闭连接
     *   }
     */
    bool parse(Buffer* buf) {
        while (buf->readableBytes() > 0 && state_ != State::kGotAll
               && state_ != State::kError) {
            switch (state_) {
                case State::kExpectRequestLine:
                    if (!parseRequestLine(buf)) return false;
                    break;
                case State::kExpectHeaders:
                    // 讲解点：parseHeaders 返回 false 时有两种情况：
                    // 1. 还没找到空行（需要更多数据）→ state 仍然是 kExpectHeaders
                    // 2. 找到了空行，Content-Length > 0，state 变成了 kExpectBody
                    //    此时不能 return false，要继续循环让 parseBody 处理
                    parseHeaders(buf);
                    if (state_ == State::kExpectHeaders) {
                        // 还在等待头部更多数据
                        return false;
                    }
                    break;
                case State::kExpectBody:
                    if (!parseBody(buf)) return false;
                    break;
                default:
                    return state_ == State::kGotAll;
            }
        }
        return state_ == State::kGotAll;
    }

private:
    /**
     * parseRequestLine — 解析请求行。
     *
     * 请求行格式：METHOD URL VERSION\r\n
     * 示例：     GET /api/hello HTTP/1.1\r\n
     *
     * 用空格分割：第一个空格前是 Method，
     *            第二个空格前是 URL，
     *            第二个空格后是 Version。
     *
     * 限制：URL 长度不超过 4096 字节，超过返回错误。
     */
    bool parseRequestLine(Buffer* buf) {
        const char* crlf = buf->findCRLF();
        if (!crlf) {
            // 行太长或者数据还没到齐
            if (buf->readableBytes() > 4096) {
                setError("Request line too long");
            }
            return false;
        }

        // 提取行内容（不含 \r\n）
        std::string line(buf->peek(), crlf - buf->peek());
        buf->retrieve(crlf + 2 - buf->peek());  // 消费行 + \r\n

        // 用空格分割
        auto firstSpace = line.find(' ');
        if (firstSpace == std::string::npos) {
            setError("Invalid request line (no space)");
            return false;
        }

        auto secondSpace = line.find(' ', firstSpace + 1);
        if (secondSpace == std::string::npos) {
            setError("Invalid request line (no version)");
            return false;
        }

        // Method
        std::string methodStr = line.substr(0, firstSpace);
        request_.setMethod(strToMethod(methodStr));
        if (request_.method() == HttpRequest::kInvalid) {
            setError("Unknown method: " + methodStr);
            return false;
        }

        // URL
        request_.setUrl(line.substr(firstSpace + 1, secondSpace - firstSpace - 1));

        // Version
        std::string versionStr = line.substr(secondSpace + 1);
        if (versionStr == "HTTP/1.1") {
            request_.setVersion(HttpRequest::kHttp11);
        } else if (versionStr == "HTTP/1.0") {
            request_.setVersion(HttpRequest::kHttp10);
        } else {
            setError("Unknown version: " + versionStr);
            return false;
        }

        state_ = State::kExpectHeaders;
        return true;
    }

    /**
     * parseHeaders — 解析头部行。
     *
     * 格式：Key: Value\r\n
     * 空行（只有 \r\n）表示头部结束。
     *
     * 逐个解析头部行：
     * - 第一行空 → 没有头部，直接检查 Content-Length
     * - 找到 : → key:value 分割，trim 空格
     * - 没找到 : → 格式错误
     *
     * 头部结束后：
     * - 读取 Content-Length 确定 body 大小
     * - 如果 Content-Length == 0 → kGotAll（无 body）
     * - 否则 → kExpectBody
     */
    bool parseHeaders(Buffer* buf) {
        while (buf->readableBytes() > 0) {
            const char* crlf = buf->findCRLF();
            if (!crlf) return false;  // 等更多数据

            std::string line(buf->peek(), crlf - buf->peek());
            buf->retrieve(crlf + 2 - buf->peek());

            if (line.empty()) {
                // 空行 → 头部结束
                return onHeadersComplete();
            }

            // 解析 "Key: Value"
            auto colon = line.find(':');
            if (colon == std::string::npos) {
                setError("Invalid header (no colon): " + line);
                return false;
            }

            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);

            // Trim 前导空格
            auto firstNonSpace = value.find_first_not_of(" \t");
            if (firstNonSpace != std::string::npos) {
                value = value.substr(firstNonSpace);
            }

            request_.setHeader(key, value);
        }
        return false;
    }

    bool onHeadersComplete() {
        std::string contentLenStr = request_.header("Content-Length");
        if (!contentLenStr.empty()) {
            try {
                contentLength_ = std::stoul(contentLenStr);
            } catch (...) {
                setError("Invalid Content-Length: " + contentLenStr);
                return false;
            }
            if (contentLength_ > 0) {
                state_ = State::kExpectBody;
                return false;
            }
        }
        state_ = State::kGotAll;
        return true;
    }

    /**
     * parseBody — 解析请求体。
     *
     * 根据 Content-Length 收齐指定字节数的 body 数据。
     * 可能分多次到达，所以用 bodyReceived_ 记录已收到的字节数。
     */
    bool parseBody(Buffer* buf) {
        size_t available = buf->readableBytes();
        size_t needed = contentLength_ - bodyReceived_;
        size_t toRead = std::min(available, needed);

        if (toRead > 0) {
            std::string bodyData(buf->peek(), toRead);
            request_.setBody(request_.body() + bodyData);
            buf->retrieve(toRead);
            bodyReceived_ += toRead;
        }

        if (bodyReceived_ >= contentLength_) {
            state_ = State::kGotAll;
            return true;
        }
        return false;
    }

    // ---- 工具 ----

    static HttpRequest::Method strToMethod(const std::string& s) {
        if (s == "GET")    return HttpRequest::kGet;
        if (s == "POST")   return HttpRequest::kPost;
        if (s == "PUT")    return HttpRequest::kPut;
        if (s == "DELETE") return HttpRequest::kDelete;
        if (s == "HEAD")   return HttpRequest::kHead;
        if (s == "OPTIONS") return HttpRequest::kOptions;
        return HttpRequest::kInvalid;
    }

    void setError(const std::string& msg) {
        state_ = State::kError;
        errorMessage_ = msg;
    }

    State state_;
    HttpRequest request_;
    size_t contentLength_ = 0;
    size_t bodyReceived_ = 0;
    std::string errorMessage_;
};

#endif // NETCHAT_HTTP_PARSER_H
