#ifndef NETCHAT_EVENTLOOP_THREAD_POOL_H
#define NETCHAT_EVENTLOOP_THREAD_POOL_H

#include <memory>
#include <vector>
#include <functional>

class EventLoop;
class EventLoopThread;

/**
 * EventLoopThreadPool — 多 Reactor 线程池。
 *
 * 管理一组 EventLoopThread，每个线程运行一个独立的 EventLoop。
 * 新连接通过 round-robin 分配到不同的 EventLoop，实现多核负载均衡。
 *
 * 讲解点：主从 Reactor（One Loop Per Thread）
 *   - Main Reactor（acceptor 所在的 EventLoop）：只负责 accept 新连接
 *   - Sub Reactors（worker EventLoops）：负责已建立连接的 I/O 读写
 *
 *   MainLoop                       SubLoops
 *   ┌─────────┐    新连接     ┌──────────────┐
 *   │ Accept  │─────────────→│ WorkerLoop 1  │
 *   │ Channel │   round-     ├──────────────┤
 *   └─────────┘   robin      │ WorkerLoop 2  │
 *                             ├──────────────┤
 *                             │ WorkerLoop N  │
 *                             └──────────────┘
 *
 *   每个连接分配到一个固定的 WorkerLoop 后，该连接上的所有 I/O
 *   事件都在那个 WorkerLoop 中处理，不需要加锁。
 *   这就是 "One Loop Per Thread" 模式的核心理念。
 */
class EventLoopThreadPool {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    EventLoopThreadPool(EventLoop* baseLoop, const std::string& name = "ThreadPool");
    ~EventLoopThreadPool();

    /// 设置线程数量（必须在 start() 之前调用）
    void setThreadNum(int numThreads) { numThreads_ = numThreads; }

    /// 启动线程池，创建所有 EventLoopThread
    void start(const ThreadInitCallback& cb = ThreadInitCallback());

    /// 获取下一个 EventLoop（round-robin 负载均衡）
    EventLoop* getNextLoop();

    /// 获取所有 EventLoop（用于广播等场景）
    std::vector<EventLoop*> getAllLoops() const;

    /// 获取线程数量
    int threadCount() const { return numThreads_; }

    /// 线程池名称
    const std::string& name() const { return name_; }

private:
    EventLoop* baseLoop_;                               ///< 主 EventLoop（acceptor）
    std::string name_;
    int numThreads_;                                    ///< 工作线程数
    int next_;                                          ///< 下一个分配的索引
    std::vector<std::unique_ptr<EventLoopThread>> threads_;  ///< 线程列表
    std::vector<EventLoop*> loops_;                           ///< EventLoop 指针列表
};

#endif // NETCHAT_EVENTLOOP_THREAD_POOL_H
