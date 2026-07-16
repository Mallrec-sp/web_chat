#ifndef NETCHAT_EVENTLOOP_THREAD_H
#define NETCHAT_EVENTLOOP_THREAD_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <functional>

class EventLoop;

/**
 * EventLoopThread — 一个 EventLoop + 它所在的线程。
 *
 * 职责：
 *   创建一个新线程，在该线程中运行一个 EventLoop。
 *   确保 EventLoop 完全初始化后再返回给调用者。
 *
 * 讲解点：std::promise/std::future 同步模式
 *   子线程创建 EventLoop 对象后，通过 promise 把指针传回父线程。
 *   父线程调用 future.get() 阻塞等待，直到子线程初始化完成。
 *   这确保了在父线程使用 EventLoop 指针时，它已经在子线程中
 *   完成了构造和 epoll 实例创建。
 *
 *   如果没有这个同步机制，父线程拿到指针后立即调用 loop()->runInLoop()
 *   可能会在子线程还没准备好 epoll 时就操作它，导致崩溃。
 */
class EventLoopThread {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    EventLoopThread(const std::string& name = "EventLoopThread",
                    ThreadInitCallback cb = nullptr);
    ~EventLoopThread();

    /// 启动线程并返回该线程中的 EventLoop 指针
    EventLoop* startLoop();

    /// 获取线程名称
    const std::string& name() const { return name_; }

private:
    /// 线程入口函数
    void threadFunc();

    EventLoop* loop_ = nullptr;          ///< 本线程管理的 EventLoop
    std::thread thread_;                 ///< 线程对象
    std::mutex mutex_;
    std::condition_variable cv_;         ///< 等待 EventLoop 初始化完成
    std::string name_;
    ThreadInitCallback callback_;        ///< 初始化回调
    bool exiting_ = false;               ///< 退出标志
};

#endif // NETCHAT_EVENTLOOP_THREAD_H
