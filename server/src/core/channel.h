#ifndef NETCHAT_CHANNEL_H
#define NETCHAT_CHANNEL_H

#include <sys/epoll.h>
#include <functional>
#include <memory>

class EventLoop;  // 前置声明，避免循环包含

/**
 * Channel — fd 事件分发的"通道"。
 *
 * 职责：
 *   把一个文件描述符和它关心的事件（EPOLLIN/EPOLLOUT）以及事件发生时的
 *   回调函数绑定在一起。是 Reactor 模式中"事件"的抽象。
 *
 * 核心概念：
 *   - fd_：监听的文件描述符
 *   - events_：注册到 epoll 的事件（如 EPOLLIN | EPOLLOUT）
 *   - revents_：epoll_wait 返回的实际发生的事件
 *   - callbacks_：事件对应的处理函数
 *
 * 讲解点：Channel vs Socket 的区别
 *   - Socket 拥有 fd（RAII 管理生命周期，析构时 close）
 *   - Channel 不拥有 fd（只是一个观察者/事件分发器）
 *   - Socket 负责创建、bind、listen、accept 等操作
 *   - Channel 负责把 fd 上发生的事件分发给对应的处理函数
 *   - 两者生命周期：Socket 持有者销毁后，fd 关闭，Channel 也必须移除
 *
 * 关于 EPOLLIN | EPOLLOUT：
 *   - EPOLLIN：fd 可读（对端发了数据，或者 listen fd 有新连接）
 *   - EPOLLOUT：fd 可写（发送缓冲区有空闲）
 *   - EPOLLRDHUP：对端关闭连接（Linux 2.6.17+）
 *   - EPOLLERR：fd 发生错误
 *   - EPOLLHUP：fd 挂起（通常表示对端关闭了连接）
 */
class Channel {
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop* loop, int fd)
        : loop_(loop), fd_(fd), events_(kNoneEvent), revents_(0), index_(-1) {}

    ~Channel() = default;

    int fd() const { return fd_; }
    int events() const { return events_; }
    int revents() const { return revents_; }
    void setRevents(int rev) { revents_ = rev; }

    // ---- 事件注册 ----

    void enableReading() {
        events_ |= kReadEvent;
        update();
    }

    void disableReading() {
        events_ &= ~kReadEvent;
        update();
    }

    void enableWriting() {
        events_ |= kWriteEvent;
        update();
    }

    void disableWriting() {
        events_ &= ~kWriteEvent;
        update();
    }

    void disableAll() {
        events_ = kNoneEvent;
        update();
    }

    bool isNoneEvent() const { return events_ == kNoneEvent; }
    bool isWriting() const { return events_ & kWriteEvent; }
    bool isReading() const { return events_ & kReadEvent; }

    // ---- 回调设置 ----

    void setReadCallback(EventCallback cb) { readCallback_ = std::move(cb); }
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

    /**
     * handleEvent — 根据 revents_ 分发到具体回调。
     *
     * 注意优先级：
     * 1. EPOLLERR 和 EPOLLHUP 优先处理（即使同时有 EPOLLIN）
     * 2. EPOLLIN 和 EPOLLPRI 触发 readCallback
     * 3. EPOLLOUT 触发 writeCallback
     * 4. EPOLLRDHUP 表示对端关闭，走 closeCallback
     * 5. EPOLLHUP（无 EPOLLIN 时）表示连接挂断
     */
    void handleEvent();

    // ---- EventLoop 管理索引 ----

    int index() const { return index_; }
    void setIndex(int idx) { index_ = idx; }

    EventLoop* ownerLoop() { return loop_; }

    /// 事件常量
    static constexpr int kReadEvent = EPOLLIN | EPOLLPRI;
    static constexpr int kWriteEvent = EPOLLOUT;
    static constexpr int kNoneEvent = 0;

private:
    void update();  // 调用 EventLoop::updateChannel

    EventLoop* loop_;
    const int fd_;
    int events_;
    int revents_;
    int index_;  ///< -1: 不在 epoll 中, 1: 在 epoll 中

    EventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};

#endif // NETCHAT_CHANNEL_H
