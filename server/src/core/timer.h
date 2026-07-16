#ifndef NETCHAT_TIMER_H
#define NETCHAT_TIMER_H

#include <functional>
#include <chrono>

/**
 * Timer — 定时器数据类。
 *
 * 由 TimerQueue 创建和管理。
 * 包含到期时间、回调函数、间隔和序列号。
 *
 * 讲解点：绝对时间 vs 相对时间
 *   所有内部存储都使用绝对时间（CLOCK_MONOTONIC 时间戳）。
 *   这样在比较两个定时器时，直接比较时间戳即可。
 *   用户调用 runAfter(3.0) 时，我们在内部转换为
 *   "当前时间 + 3.0 秒" 的绝对时间存储。
 *
 *   使用 CLOCK_MONOTONIC（单调时钟）而非 CLOCK_REALTIME（实时时钟），
 *   可以避免系统时间调整（NTP、夏令时）导致的误触发。
 */
class Timer {
public:
    using TimerCallback = std::function<void()>;

    Timer(TimerCallback cb, std::chrono::steady_clock::time_point expiration,
          double interval, int64_t seq)
        : callback_(std::move(cb))
        , expiration_(expiration)
        , interval_(interval)
        , sequence_(seq)
        , repeat_(interval > 0.0)
    {
    }

    /// 执行回调
    void run() const {
        if (callback_) callback_();
    }

    /// 获取到期时间
    std::chrono::steady_clock::time_point expiration() const { return expiration_; }

    /// 是否重复定时器
    bool repeat() const { return repeat_; }

    /// 序列号（唯一标识）
    int64_t sequence() const { return sequence_; }

    /// 重新计算下一次到期时间（仅对重复定时器有效）
    void restart() {
        if (repeat_) {
            auto now = std::chrono::steady_clock::now();
            auto interval_us = static_cast<int64_t>(interval_ * 1000000);
            expiration_ = now + std::chrono::microseconds(interval_us);
        }
    }

private:
    TimerCallback callback_;
    std::chrono::steady_clock::time_point expiration_;
    const double interval_;      ///< 间隔（秒），0 表示一次性定时器
    const int64_t sequence_;     ///< 序列号，用于唯一标识
    const bool repeat_;          ///< 是否重复
};

#endif // NETCHAT_TIMER_H
