#include "eventloop_thread_pool.h"
#include "eventloop_thread.h"
#include "eventloop.h"

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, const std::string& name)
    : baseLoop_(baseLoop)
    , name_(name)
    , numThreads_(0)
    , next_(0)
{
}

EventLoopThreadPool::~EventLoopThreadPool() {
    // EventLoopThread 的析构函数会自动 quit() 并 join 线程
}

void EventLoopThreadPool::start(const ThreadInitCallback& cb) {
    for (int i = 0; i < numThreads_; ++i) {
        std::string threadName = name_ + "-" + std::to_string(i);

        auto thread = std::make_unique<EventLoopThread>(threadName, cb);
        EventLoop* loop = thread->startLoop();

        threads_.push_back(std::move(thread));
        loops_.push_back(loop);
    }
}

EventLoop* EventLoopThreadPool::getNextLoop() {
    if (loops_.empty()) {
        // 没有 worker 线程，使用 baseLoop（主 EventLoop）
        return baseLoop_;
    }

    // round-robin 分配
    EventLoop* loop = loops_[next_];
    ++next_;
    if (next_ >= static_cast<int>(loops_.size())) {
        next_ = 0;
    }
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops() const {
    if (loops_.empty()) {
        return {baseLoop_};
    }
    return loops_;
}
