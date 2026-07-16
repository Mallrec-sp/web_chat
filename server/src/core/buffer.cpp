#include "buffer.h"
#include <cassert>
#include <cerrno>

/**
 * readFd 实现 — readv 双缓冲。
 *
 * 详细流程：
 * 1. 先检查 writable 剩余空间
 * 2. 准备 iovec[2]
 *    - iovec[0] 指向 buffer 的 writable 区域
 *    - iovec[1] 指向栈上的 extrabuf (64KB)
 * 3. 如果剩余空间能容纳 extrabuf，只用 iovec[0]（空间够大不需要双缓冲）
 * 4. 否则用两个 iovec，多出的数据落到 extrabuf
 * 5. readv 调用后：
 *    - 如果写入 buffer 的数据 <= 原 writable 空间 → writerIndex_ 后移即可
 *    - 如果有溢出数据写入 extrabuf → append 进 buffer
 */
ssize_t Buffer::readFd(int fd) {
    const size_t writable = writableBytes();
    char extrabuf[65536];
    struct iovec vec[2];
    const size_t iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;

    vec[0].iov_base = begin() + writerIndex_;
    vec[0].iov_len = writable;
    if (iovcnt == 2) {
        vec[1].iov_base = extrabuf;
        vec[1].iov_len = sizeof(extrabuf);
    }

    // ET 模式下这里只有一个 readv 调用
    // 调用者（TcpConnection）会在外部循环确保数据读完
    ssize_t n = ::readv(fd, vec, iovcnt);
    if (n < 0) {
        // EAGAIN 不是错误，表示当前没有更多数据可读
        if (errno != EAGAIN) {
            return -1;
        }
        return 0;
    }
    if (static_cast<size_t>(n) <= writable) {
        writerIndex_ += n;
    } else {
        writerIndex_ = buffer_.size();
        append(extrabuf, n - writable);
    }
    return n;
}

/**
 * writeFd 实现 — 使用 writev 聚合写入。
 * 一次 writev 可以把 outputBuffer 的 readable 数据全部写出。
 * 如果部分写成功，retrieve 只消费写入的字节数，剩余的等下一次 EPOLLOUT。
 */
ssize_t Buffer::writeFd(int fd) {
    ssize_t n = ::write(fd, peek(), readableBytes());
    if (n > 0) {
        readerIndex_ += n;
    }
    return n;
}

/**
 * ensureWritableBytes — 保证至少有 len 字节可写空间。
 */
void Buffer::ensureWritableBytes(size_t len) {
    if (writableBytes() >= len) return;
    makeSpace(len);
}

/**
 * makeSpace — 腾出足够空间。
 *
 * 两种策略：
 * 1. 如果 prependable + writable >= len（即已读区域+空闲区够用），
 *    把 readable 数据往前搬，腾出后面空间 → 不需要重新分配内存
 * 2. 否则 resize buffer → 可能需要重新分配和拷贝
 *
 * 讲解点：策略 1 是关键的优化。当大部分数据已被消费时，
 * prependable 区域很大但 writable 区域很小。这时候不 resize，
 * 而是 memmove 把还未读的数据往前挪，一次性腾出大量空间。
 * 这就是分摊策略（amortized O(1)）。
 */
void Buffer::makeSpace(size_t len) {
    if (prependableBytes() + writableBytes() < len) {
        // 总空间不够 → resize
        buffer_.resize(writerIndex_ + len);
    } else {
        // 空间够但布局碎片化 → 搬移
        size_t readable = readableBytes();
        std::copy(begin() + readerIndex_, begin() + writerIndex_,
                  begin() + kCheapPrepend);
        readerIndex_ = kCheapPrepend;
        writerIndex_ = readerIndex_ + readable;
        assert(writableBytes() >= len);
    }
}

void Buffer::append(const char* data, size_t len) {
    ensureWritableBytes(len);
    std::copy(data, data + len, beginWrite());
    writerIndex_ += len;
}

void Buffer::prepend(const void* data, size_t len) {
    assert(len <= prependableBytes());
    readerIndex_ -= len;
    const char* d = static_cast<const char*>(data);
    std::copy(d, d + len, begin() + readerIndex_);
}
