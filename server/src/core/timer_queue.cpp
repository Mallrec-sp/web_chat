#include "timer_queue.h"
#include "timer.h"
#include "eventloop.h"
#include "channel.h"
#include "utils/logger.h"

#include <sys/timerfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

bool TimerQueue::TimerComparator::operator()(const Timer* lhs, const Timer* rhs) const {
    return lhs->expiration() < rhs->expiration();
}

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop)
    , timerfd_(createTimerfd())
    , timerfdChannel_(std::make_unique<Channel>(loop_, timerfd_))
{
    timerfdChannel_->setReadCallback([this] { handleRead(); });
    timerfdChannel_->enableReading();
}

TimerQueue::~TimerQueue() {
    timerfdChannel_->disableAll();
    loop_->removeChannel(timerfdChannel_.get());
    ::close(timerfd_);

    // 删除所有定时器
    for (auto* timer : timers_) {
        delete timer;
    }
    timers_.clear();
}

TimerId TimerQueue::addTimer(TimerCallback cb, double delay, double interval) {
    auto now = std::chrono::steady_clock::now();
    auto delay_us = static_cast<int64_t>(delay * 1000000);
    auto expiration = now + std::chrono::microseconds(delay_us);

    auto* timer = new Timer(std::move(cb), expiration, interval, nextSeq_++);
    loop_->runInLoop([this, timer] { addTimerInLoop(timer); });
    return {timer, timer->sequence()};
}

void TimerQueue::cancel(TimerId timerId) {
    loop_->runInLoop([this, timerId] { cancelInLoop(timerId); });
}

void TimerQueue::addTimerInLoop(Timer* timer) {
    bool earliestChanged = false;
    if (timers_.empty() || (*timers_.begin())->expiration() > timer->expiration()) {
        earliestChanged = true;
    }

    timers_.insert(timer);

    if (earliestChanged) {
        resetTimerfd(timerfd_, timer->expiration());
    }
}

void TimerQueue::cancelInLoop(TimerId timerId) {
    // 从 timers_ 中删除
    auto it = timers_.find(timerId.timer);
    if (it != timers_.end() && (*it)->sequence() == timerId.sequence) {
        timers_.erase(it);
        delete timerId.timer;
    }
    // 从 cancelingTimers_ 中删除
    auto cit = cancelingTimers_.find(timerId.timer);
    if (cit != cancelingTimers_.end() && (*cit)->sequence() == timerId.sequence) {
        cancelingTimers_.erase(cit);
        delete timerId.timer;
    }
}

void TimerQueue::handleRead() {
    loop_->assertInLoopThread();
    readTimerfd(timerfd_);

    auto expired = getExpired();

    callingExpiredTimers_ = true;
    cancelingTimers_.clear();
    for (auto* timer : expired) {
        timer->run();
    }
    callingExpiredTimers_ = false;

    reset(expired);
}

std::vector<Timer*> TimerQueue::getExpired() {
    auto now = std::chrono::steady_clock::now();
    std::vector<Timer*> expired;

    // 哨兵定时器：到期时间为 now，用于找到所有 <= now 的定时器
    Timer sentinel(nullptr, now, 0, 0);

    // lower_bound 返回第一个 >= sentinel 的元素
    auto end = timers_.lower_bound(&sentinel);
    std::copy(timers_.begin(), end, std::back_inserter(expired));
    timers_.erase(timers_.begin(), end);

    return expired;
}

void TimerQueue::reset(const std::vector<Timer*>& expired) {
    for (auto* timer : expired) {
        if (timer->repeat()) {
            timer->restart();
            timers_.insert(timer);
        } else {
            delete timer;
        }
    }

    // 重置 timerfd
    if (!timers_.empty()) {
        resetTimerfd(timerfd_, (*timers_.begin())->expiration());
    }
}

int TimerQueue::createTimerfd() {
    int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR("timerfd_create failed: " << strerror(errno));
        std::abort();
    }
    return fd;
}

void TimerQueue::readTimerfd(int timerfd) {
    uint64_t howmany;
    ssize_t n = ::read(timerfd, &howmany, sizeof(howmany));
    if (n != sizeof(howmany)) {
        LOG_ERROR("timerfd read error: " << strerror(errno));
    }
}

void TimerQueue::resetTimerfd(int timerfd, std::chrono::steady_clock::time_point expiration) {
    struct itimerspec newValue;
    struct itimerspec oldValue;

    std::memset(&newValue, 0, sizeof(newValue));
    std::memset(&oldValue, 0, sizeof(oldValue));

    // 计算距离现在的纳秒数
    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(expiration - now).count();
    if (us < 0) us = 0;

    newValue.it_value.tv_sec = static_cast<time_t>(us / 1000000);
    newValue.it_value.tv_nsec = static_cast<long>((us % 1000000) * 1000);

    int ret = ::timerfd_settime(timerfd, 0, &newValue, &oldValue);
    if (ret < 0) {
        LOG_ERROR("timerfd_settime error: " << strerror(errno));
    }
}
