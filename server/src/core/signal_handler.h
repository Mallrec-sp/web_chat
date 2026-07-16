#ifndef NETCHAT_SIGNAL_HANDLER_H
#define NETCHAT_SIGNAL_HANDLER_H

#include <memory>
#include <functional>

class EventLoop;
class Channel;

/**
 * SignalHandler — 基于 self-pipe trick 的异步信号处理。
 *
 * 不使用 signalfd（多线程下不可靠），而是用经典的自管道技巧：
 * 1. 创建一个 eventfd（信号安全的文件描述符）
 * 2. 在 EventLoop 中监听 eventfd 的可读事件
 * 3. 用 sigaction 注册信号处理函数
 * 4. 信号处理函数向 eventfd 写入一个字节（async-signal-safe）
 * 5. EventLoop 从 eventfd 读出字节，触发回调
 *
 * 讲解点：为什么不用 signalfd？
 *   signalfd 只能读取调用线程的信号，而 process-directed 信号
 *   （如 kill -INT）在 multi-threading 下处于"进程级待决"状态，
 *   signalfd 捕获不到。self-pipe trick 在任何线程都能工作。
 *
 *   这是 libevent、libuv、nginx 等所有高性能框架的标准做法。
 */
class SignalHandler {
public:
    using SignalCallback = std::function<void(int signum)>;

    SignalHandler(EventLoop* loop,
                  const std::initializer_list<int>& siglist,
                  SignalCallback cb);
    ~SignalHandler();

private:
    /// eventfd 可读时的处理函数（由 EventLoop 调用）
    void handleRead();

    EventLoop* loop_;
    int eventFd_;                            ///< 信号通知用的 eventfd
    std::unique_ptr<Channel> eventFdChannel_; ///< eventfd 的 Channel
    SignalCallback callback_;
};

#endif // NETCHAT_SIGNAL_HANDLER_H
