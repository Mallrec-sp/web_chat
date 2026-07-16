#include "eventloop.h"
#include "channel.h"
#include "timer_queue.h"
#include "utils/logger.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cassert>

/**
 * __thread — 线程局部存储（Thread Local Storage）。
 *
 * 每个 EventLoop 所属线程都有一份独立的 t_loopOfThisThread 变量。
 * 这样可以快速判断当前线程是否持有某个 EventLoop。
 *
 * 讲解点：这是"One Loop Per Thread"模式的关键标识。
 * 每个线程最多只能有一个 EventLoop，且 EventLoop 必须在它的
 * 创建线程中运行。t_loopOfThisThread 确保了这个约束。
 */
static thread_local EventLoop* t_loopOfThisThread = nullptr;

EventLoop::EventLoop(const std::string& name)
    : threadId_(std::this_thread::get_id())
    , name_(name)
    , epollFd_(createEpollFd())
    , wakeupFd_(createEventFd())
{
    LOG_DEBUG("EventLoop created: " << name_ << " on thread " << threadId_);

    if (t_loopOfThisThread) {
        LOG_ERROR("Another EventLoop exists on this thread: " << name_);
    } else {
        t_loopOfThisThread = this;
    }

    // 创建 wakeup Channel，检测 eventfd 的可读事件
    wakeupChannel_ = std::make_unique<Channel>(this, wakeupFd_);
    wakeupChannel_->setReadCallback([this] { handleWakeup(); });
    wakeupChannel_->enableReading();

    // 创建 TimerQueue
    timerQueue_ = std::make_unique<TimerQueue>(this);
}

EventLoop::~EventLoop() {
    LOG_DEBUG("EventLoop destructing");
    wakeupChannel_->disableAll();
    removeChannel(wakeupChannel_.get());
    ::close(wakeupFd_);
    ::close(epollFd_);
    t_loopOfThisThread = nullptr;
}

/**
 * loop — 主事件循环。
 *
 * 核心流程：
 * 1. epoll_wait 等待事件（可被 eventfd 唤醒）
 * 2. 遍历 activeChannels_，对每个 Channel 调用 handleEvent
 * 3. 执行 doPendingFunctors() 处理跨线程投递的任务
 *
 * doPendingFunctors 放在最后执行，但每次循环都会跑一次，
 * 所以跨线程消息的延迟最多是一次 epoll_wait 的超时时间。
 * 但因为我们用 eventfd 唤醒了 epoll_wait，实际上几乎是立即执行的。
 */
void EventLoop::loop() {
    LOG_INFO("EventLoop started, thread: " << threadId_);
    assert(!quit_.load());
    assert(t_loopOfThisThread == this);

    while (!quit_.load()) {
        activeChannels_.clear();

        /**
         * epoll_wait 阻塞等待事件。
         * 参数 -1 表示无限超时（直到有事件发生或被 eventfd 唤醒）。
         *
         * 返回就绪事件数量，填满 events_ 数组。
         * 详解：epoll_event.data.ptr 指向 Channel 对象（在 updateChannel 时设置）
         */
        int numEvents = ::epoll_wait(epollFd_, events_, kMaxEvents, -1);

        if (numEvents < 0) {
            if (errno == EINTR) {
                // 被信号中断，重试
                continue;
            }
            LOG_ERROR("epoll_wait error: " << strerror(errno));
            continue;
        }

        // 填充 activeChannels_
        for (int i = 0; i < numEvents; ++i) {
            Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
            channel->setRevents(events_[i].events);
            activeChannels_.push_back(channel);
        }

        // 处理所有活跃 Channel
        for (Channel* channel : activeChannels_) {
            channel->handleEvent();
        }

        // 处理跨线程投递的任务
        doPendingFunctors();
    }

    LOG_INFO("EventLoop stopped");
}

void EventLoop::quit() {
    quit_.store(true);
    // 可能在其他线程调用 quit()，唤醒 epoll_wait 让 loop 看到 quit_
    if (!isInLoopThread()) {
        wakeup();
    }
}

/**
 * updateChannel — 更新 Channel 在 epoll 中的注册状态。
 *
 * 根据 Channel::index_ 判断是 ADD 还是 MOD：
 * - index_ == -1：还未注册 → EPOLL_CTL_ADD
 * - index_ >= 0：已注册 → EPOLL_CTL_MOD
 * - events_ == kNoneEvent：不感兴趣 → EPOLL_CTL_DEL（暂不处理，由 removeChannel 负责）
 */
void EventLoop::updateChannel(Channel* channel) {
    assert(channel->ownerLoop() == this);
    assertInLoopThread();

    struct epoll_event event;
    event.events = channel->events();
    event.data.ptr = channel;

    int op;
    if (channel->index() < 0) {
        // 还没注册 → 添加
        op = EPOLL_CTL_ADD;
        channel->setIndex(1);
    } else {
        // 已注册 → 修改
        op = EPOLL_CTL_MOD;
    }

    if (::epoll_ctl(epollFd_, op, channel->fd(), &event) < 0) {
        // EBADF 在析构时是正常的（epollFd 可能已被关闭）
        if (errno != EBADF) {
            LOG_ERROR("epoll_ctl error: " << strerror(errno)
                      << " op=" << op << " fd=" << channel->fd());
        }
    }
}

void EventLoop::removeChannel(Channel* channel) {
    assert(channel->ownerLoop() == this);
    assertInLoopThread();

    if (channel->index() >= 0) {
        if (::epoll_ctl(epollFd_, EPOLL_CTL_DEL, channel->fd(), nullptr) < 0) {
            if (errno != EBADF) {
                LOG_ERROR("epoll_ctl DEL error: " << strerror(errno)
                          << " fd=" << channel->fd());
            }
        }
        channel->setIndex(-1);
    }
}

void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        // 如果当前就是 EventLoop 线程，直接执行
        cb();
    } else {
        // 否则加入队列，唤醒 EventLoop 线程
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors_.push_back(std::move(cb));
    }

    /**
     * 什么时候需要 wakeup？
     * 1. 调用者不是 EventLoop 线程（需要唤醒 epoll_wait 去处理任务）
     * 2. 调用者是 EventLoop 线程，但当前正在处理 pending functors
     *    （说明这次 add 不会被当前 doPendingFunctors 处理到，
     *     需要再来一次 loop 迭代才会处理，所以要唤醒）
     */
    if (!isInLoopThread() || callingPendingFunctors_.load()) {
        wakeup();
    }
}

/**
 * wakeup — 向 eventfd 写入 8 字节以唤醒 epoll_wait。
 *
 * eventfd 是一个内核维护的 64 位计数器。
 * write() 把它的值增加指定数量，read() 读取并清空。
 * 当 eventfd > 0 时，epoll 会返回 EPOLLIN 事件。
 * 我们的 handleWakeup() 负责读取它，清空事件标志。
 *
 * 为什么不直接用 pipe？
 * pipe 需要两个 fd（一个读一个写），而 eventfd 只需要一个，
 * 且不消耗宝贵的 pipe buffer，内核实现也更轻量。
 */
void EventLoop::wakeup() {
    uint64_t one = 1;
    ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        LOG_ERROR("eventfd write error: " << strerror(errno));
    }
}

void EventLoop::handleWakeup() {
    uint64_t one = 1;
    ssize_t n = ::read(wakeupFd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        LOG_ERROR("eventfd read error: " << strerror(errno));
    }
}

void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_.store(true);

    {
        /**
         * 把 pendingFunctors_ 放到局部变量中执行。
         * 核心优化：尽量减少锁的持有时间。
         * 只在 swap 时加锁，执行回调时不加锁。
         * 这样回调函数中如果再调 queueInLoop，不会死锁。
         */
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }

    for (const auto& cb : functors) {
        cb();
    }

    callingPendingFunctors_.store(false);
}

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) {
        LOG_ERROR("EventLoop not in loop thread! eventloop thread="
                  << threadId_ << ", current=" << std::this_thread::get_id());
        assert(isInLoopThread());
    }
}

/**
 * createEpollFd — 创建 epoll 实例。
 *
 * epoll_create1(EPOLL_CLOEXEC) 替代老式的 epoll_create(int size)。
 * EPOLL_CLOEXEC 确保子进程不继承 epoll fd（执行 exec 时自动关闭）。
 * 这是安全的默认做法，避免 fd 泄漏到子进程。
 */
int EventLoop::createEpollFd() {
    int fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR("epoll_create1 failed: " << strerror(errno));
        std::abort();
    }
    return fd;
}

int EventLoop::createEventFd() {
    int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR("eventfd failed: " << strerror(errno));
        std::abort();
    }
    return fd;
}

// ==================== 定时器接口 ====================

EventLoop::TimerId EventLoop::runAfter(double delay, std::function<void()> cb) {
    auto realId = timerQueue_->addTimer(std::move(cb), delay, 0);
    return {realId.timer, realId.sequence};
}

EventLoop::TimerId EventLoop::runEvery(double interval, std::function<void()> cb) {
    auto realId = timerQueue_->addTimer(std::move(cb), interval, interval);
    return {realId.timer, realId.sequence};
}

void EventLoop::cancel(EventLoop::TimerId id) {
    ::TimerId realId;
    realId.timer = static_cast<struct Timer*>(id.timer);
    realId.sequence = id.sequence;
    timerQueue_->cancel(realId);
}
