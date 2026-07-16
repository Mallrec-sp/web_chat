#ifndef NETCHAT_TIMER_QUEUE_H
#define NETCHAT_TIMER_QUEUE_H

#include <memory>
#include <set>
#include <vector>
#include <functional>
#include <chrono>

class EventLoop;
class Channel;
class Timer;

/**
 * TimerId — 定时器的唯一标识。
 *
 * 由 TimerQueue 在创建时分配，用于后续取消。
 * 包含指向 Timer 对象的指针和序列号（用于判断 Timer 是否已被重置）。
 */
struct TimerId {
    Timer* timer = nullptr;
    int64_t sequence = 0;
    bool operator<(const TimerId& other) const {
        return std::make_pair(timer, sequence) < std::make_pair(other.timer, other.sequence);
    }
};

/**
 * TimerQueue — 基于 timerfd 的定时器队列。
 *
 * 不依赖额外线程，完全集成到 EventLoop 的 epoll 中。
 * 使用 Linux timerfd 实现，当最早到期的定时器触发时，
 * timerfd 变为可读，EventLoop 唤醒并处理所有已到期的定时器。
 *
 * 讲解点：为什么用 timerfd 而不是信号？
 *   传统做法是用 setitimer + SIGALRM，但信号处理是全局的，
 *   在多线程环境下极其麻烦（必须用 sigwait 或者
 *   在 signal handler 中写自管道）。
 *
 *   timerfd 是 Linux 3.10+ 的完美解决方案：
 *   1. 它和普通 fd 一样，可以放到 epoll 里
 *   2. 到期时定时器 fd 可读，EventLoop 自然醒来
 *   3. 不需要处理信号，完全和事件循环融合
 *   4. 精度到纳秒级别
 *
 *   这也是 muduo、libevent、nginx 等现代框架的做法。
 */
class TimerQueue {
public:
    using TimerCallback = std::function<void()>;

    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    /// 添加一个定时器，在 delay 秒后执行
    /// @return TimerId 可用于取消定时器
    TimerId addTimer(TimerCallback cb, double delay, double interval);

    /// 取消定时器
    void cancel(TimerId timerId);

private:
    /// 添加定时器的实际逻辑（在 EventLoop 线程中执行）
    void addTimerInLoop(Timer* timer);

    /// 取消定时器的实际逻辑（在 EventLoop 线程中执行）
    void cancelInLoop(TimerId timerId);

    /// timerfd 可读时的处理函数
    void handleRead();

    /// 获取所有已到期的定时器
    std::vector<Timer*> getExpired();

    /// 重置已到期的重复定时器（重新加入队列）
    void reset(const std::vector<Timer*>& expired);

    /// 创建 timerfd
    int createTimerfd();

    /// 读取 timerfd（清空通知）
    void readTimerfd(int timerfd);

    /// 重置 timerfd 的到期时间
    void resetTimerfd(int timerfd, std::chrono::steady_clock::time_point expiration);

    EventLoop* loop_;
    const int timerfd_;
    std::unique_ptr<Channel> timerfdChannel_;

    /**
     * Timer 比较器：按到期时间排序。
     * 最早到期的排在前面（std::set 中最左）。
     * 如果到期时间相同，按 sequence 排序（保证 FIFO 顺序）。
     */
    struct TimerComparator {
        bool operator()(const Timer* lhs, const Timer* rhs) const;
    };

    /// 所有定时器（按到期时间排序）
    std::set<Timer*, TimerComparator> timers_;

    /// 已取消但还在队列中的定时器（延迟删除，防止重入）
    std::set<Timer*, TimerComparator> cancelingTimers_;

    /// 序列号生成器
    int64_t nextSeq_ = 1;

    /// 是否正在处理到期定时器（防止取消操作的递归问题）
    bool callingExpiredTimers_ = false;
};

#endif // NETCHAT_TIMER_QUEUE_H
