#ifndef NETCHAT_EVENTLOOP_H
#define NETCHAT_EVENTLOOP_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/epoll.h>  // struct epoll_event

class Channel;
class TimerQueue;

/**
 * EventLoop — Reactor 模式的实现核心（事件循环）。
 *
 * 职责：
 *   1. 持有 epoll 实例（epollFd_），管理所有 Channel 的生命周期
 *   2. 运行事件循环：epoll_wait → 分发事件到 Channel 回调
 *   3. 支持跨线程任务投递（eventfd + pendingFunctors）
 *
 * 设计思路：
 *   本实现是"单线程 Reactor"模式——所有 I/O 事件在同一个线程中处理。
 *   这样做的好处：
 *   - 没有锁竞争（都在一个线程里跑）
 *   - 简单、易于 debug
 *   - 对于 I/O 密集型但计算轻量的场景（聊天服务器），单线程完全够用
 *   - 后续可以扩展为多 Reactor 模式（one loop per thread）
 *
 * 讲解点：eventfd 唤醒机制
 *   当 EventLoop 阻塞在 epoll_wait() 上时，如果有其他线程想往这个
 *   EventLoop 投递任务（比如跨线程调用 send()），必须能唤醒 epoll_wait。
 *   我们的做法是创建一个 eventfd（wakeupFd_），注册 EPOLLIN。
 *   投递任务时向 eventfd 写入 8 字节，epoll_wait 立即返回，
 *   在处理 wakeup Channel 的回调函数中清空 eventfd，然后执行 pending functors。
 *
 *   这个模式在 libevent 中叫"event base wakeup"，
 *   在 muduo 中叫"runInLoop"，是跨线程任务调度的标准方案。
 */
class EventLoop {
public:
    using Functor = std::function<void()>;

    EventLoop(const std::string& name = "EventLoop");
    ~EventLoop();

    // 禁止拷贝
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /**
     * loop — 主事件循环。
     *
     * while (!quit_) {
     *   1. epoll_wait（阻塞等待事件）
     *   2. 遍历所有活跃 Channel，调用 handleEvent
     *   3. 执行 pending functors（跨线程投递的任务）
     * }
     *
     * 这是一个无限循环，直到 quit() 被调用才会退出。
     */
    void loop();

    /// 退出事件循环，线程安全
    void quit();

    /// 获取 EventLoop 名称
    const std::string& name() const { return name_; }

    // ---- 定时器接口 ----

    struct TimerId {
        void* timer = nullptr;
        int64_t sequence = 0;
    };

    /// 在 delay 秒后执行回调
    TimerId runAfter(double delay, std::function<void()> cb);

    /// 每隔 interval 秒执行回调
    TimerId runEvery(double interval, std::function<void()> cb);

    /// 取消定时器
    void cancel(TimerId id);

    // ---- Channel 管理 ----

    /**
     * 更新 Channel 在 epoll 中的注册状态。
     * 如果 Channel 不在 epoll 中 → EPOLL_CTL_ADD
     * 如果 Channel 已在 epoll 中 → EPOLL_CTL_MOD
     * 如果 Channel 事件为 kNoneEvent → EPOLL_CTL_DEL
     */
    void updateChannel(Channel* channel);

    /// 从 epoll 中移除 Channel
    void removeChannel(Channel* channel);

    // ---- 跨线程任务 ----

    /**
     * runInLoop — 在 EventLoop 所在线程执行回调。
     *
     * 如果调用者已经是 EventLoop 线程，直接执行；
     * 否则将回调加入 pendingFunctors 队列，唤醒 loop 线程处理。
     *
     * 这是整个系统中"跨线程安全"的核心机制。
     */
    void runInLoop(Functor cb);

    /// 将回调加入队列（总是）并唤醒 EventLoop 线程
    void queueInLoop(Functor cb);

    // ---- 线程标识 ----

    /// 判断当前线程是否为 EventLoop 所在线程
    bool isInLoopThread() const {
        return threadId_ == std::this_thread::get_id();
    }

    void assertInLoopThread() const;

private:
    int createEpollFd();
    int createEventFd();

    /// 向 eventfd 写 8 字节以唤醒 epoll_wait
    void wakeup();

    /// 从 eventfd 读 8 字节（清空事件标志）
    void handleWakeup();

    /// 执行所有 pending functors（加锁交换队列后执行，减少锁持有时间）
    void doPendingFunctors();

    static constexpr int kMaxEvents = 4096;  ///< epoll_wait 最多返回事件数

    std::atomic<bool> quit_{false};           ///< 退出标志
    const std::thread::id threadId_;          ///< 创建此 EventLoop 的线程 ID
    const std::string name_;                  ///< EventLoop 名称

    int epollFd_;                             ///< epoll 实例 fd
    int wakeupFd_;                            ///< eventfd（用于唤醒）
    std::unique_ptr<Channel> wakeupChannel_;  ///< eventfd 对应的 Channel
    std::unique_ptr<TimerQueue> timerQueue_;  ///< 定时器队列

    /// epoll_wait 返回的事件数组
    struct epoll_event events_[kMaxEvents];

    /// 临时存储 epoll_wait 返回的活跃 Channel
    std::vector<Channel*> activeChannels_;

    /// 跨线程投递的任务队列
    std::vector<Functor> pendingFunctors_;

    std::mutex mutex_;                       ///< 保护 pendingFunctors_
    std::atomic<bool> callingPendingFunctors_{false};
};

#endif // NETCHAT_EVENTLOOP_H
