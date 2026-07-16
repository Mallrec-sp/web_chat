#include "eventloop_thread.h"
#include "eventloop.h"

EventLoopThread::EventLoopThread(const std::string& name, ThreadInitCallback cb)
    : name_(name)
    , callback_(std::move(cb))
{
}

EventLoopThread::~EventLoopThread() {
    exiting_ = true;
    if (loop_ != nullptr) {
        loop_->quit();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

EventLoop* EventLoopThread::startLoop() {
    thread_ = std::thread(&EventLoopThread::threadFunc, this);
    EventLoop* loop = nullptr;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return loop_ != nullptr; });
        loop = loop_;
    }

    return loop;
}

void EventLoopThread::threadFunc() {
    EventLoop loop;

    if (callback_) {
        callback_(&loop);
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);
        loop_ = &loop;
        cv_.notify_one();
    }

    loop.loop();  // 阻塞在这里，直到 quit() 被调用

    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = nullptr;
}
