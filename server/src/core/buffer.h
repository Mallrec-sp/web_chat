#ifndef NETCHAT_BUFFER_H
#define NETCHAT_BUFFER_H

#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <cassert>
#include <sys/uio.h>  // readv / writev
#include <unistd.h>   // read / write

/**
 * Buffer — 可增长的 I/O 缓冲区。
 *
 * 内存布局: [prependable | readable | writable]
 *            ^            ^          ^           ^
 *            begin()     peek()     beginWrite() end()
 *
 * 设计思路：
 *   - prependable 区域 (8 字节) 给上层协议预留空间。
 *     比如 WebSocket 帧需要在前加头部，可以直接在前面写。
 *   - readable 区域是尚未被消费的数据。
 *   - writable 区域是空闲可写入空间。
 *
 * 讲解点：这个布局是 muduo 首创的经典设计。和传统环形缓冲区不同，
 * 它在头部预留了空间，使得协议层可以在不拷贝数据的情况下添加头部。
 * WebSocket 加 Mask 和 HTTP 加 chunked 编码时特别有用。
 */
class Buffer {
public:
    static constexpr size_t kInitialSize = 1024;
    static constexpr size_t kCheapPrepend = 8;

    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepend + initialSize)
        , readerIndex_(kCheapPrepend)
        , writerIndex_(kCheapPrepend) {}

    // ========== 数据访问 ==========

    /// 可读数据的起始位置
    const char* peek() const { return begin() + readerIndex_; }

    /// 可读字节数
    size_t readableBytes() const { return writerIndex_ - readerIndex_; }

    /// 可写字节数（还剩多少空间）
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }

    /// prependable 字节数（已读但尚未回收的空间）
    size_t prependableBytes() const { return readerIndex_; }

    /// 可写区域的起始地址
    char* beginWrite() { return begin() + writerIndex_; }
    const char* beginWrite() const { return begin() + writerIndex_; }

    // ========== I/O 操作 ==========

    /**
     * 从 fd 读取数据到缓冲区。
     *
     * 讲解点：这里用了 readv 双缓冲技巧。
     * iovec[0] 指向 buffer 的 writable 区域，
     * iovec[1] 指向栈上的 65536 字节临时缓冲区。
     *
     * 为什么这么设计？
     * - 如果数据量小于 writable 剩余空间，直接读入 buffer，零拷贝。
     * - 如果数据量很大（> writable 剩余），超出的部分落到栈缓冲中，
     *   然后 append 到 buffer。避免了频繁 resize buffer。
     * - 65536 是经典值，因为 TCP 的 receive buffer 通常也是 64KB。
     *
     * @param fd 已就绪可读的 socket fd
     * @return 读取的字节数，0 表示对端关闭，-1 表示错误
     */
    ssize_t readFd(int fd);

    /**
     * 把 readable 数据写入 fd。
     * 利用 writev 聚合写入，减少系统调用。
     * 如果一次写不完（部分写），剩余的还在 buffer 里。
     *
     * @return 写入的字节数
     */
    ssize_t writeFd(int fd);

    // ========== 数据操作 ==========

    /// 确保 writable 区域有至少 len 字节
    void ensureWritableBytes(size_t len);

    /// 写入数据到 writable 区域
    void append(const char* data, size_t len);
    void append(const std::string& str) { append(str.data(), str.size()); }

    /// 在 prependable 区域写入 len 字节（用于协议头部）
    void prepend(const void* data, size_t len);

    /// 消费 len 字节的可读数据（注意：不移动内存，只移动 readerIndex_）
    void retrieve(size_t len) {
        if (len < readableBytes()) {
            readerIndex_ += len;
        } else {
            retrieveAll();
        }
    }

    /// 消费全部可读数据
    void retrieveAll() {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
    }

    /// 消费全部并返回字符串
    std::string retrieveAllAsString() {
        return retrieveAsString(readableBytes());
    }

    /// 消费 len 字节并返回字符串
    std::string retrieveAsString(size_t len) {
        assert(len <= readableBytes());
        std::string result(peek(), len);
        retrieve(len);
        return result;
    }

    // ========== HTTP 工具 ==========

    /// 查找 "\r\n" 在 readable 区域中的位置（HTTP 协议用）
    const char* findCRLF() const {
        const char* crlf = std::search(peek(), beginWrite(), kCRLF, kCRLF + 2);
        return crlf == beginWrite() ? nullptr : crlf;
    }

    // ========== 调试 ==========
    ssize_t size() const { return buffer_.size(); }

private:
    char* begin() { return &*buffer_.begin(); }
    const char* begin() const { return &*buffer_.begin(); }

    /// 保证至少 len 字节 writable 空间
    void makeSpace(size_t len);

    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;

    static constexpr const char* kCRLF = "\r\n";
};

#endif // NETCHAT_BUFFER_H
