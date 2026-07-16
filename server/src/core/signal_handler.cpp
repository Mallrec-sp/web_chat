#include "signal_handler.h"
#include "eventloop.h"
#include "channel.h"
#include "utils/logger.h"

#include <sys/eventfd.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

/**
 * 全局 eventfd — 信号处理函数和 EventLoop 之间的桥梁。
 *
 * 因为信号处理函数必须是 async-signal-safe（不能拿锁、不能分配内存），
 * 我们用一个全局 int 来传递 fd。信号处理函数向这个 fd 写入 1 字节，
 * EventLoop 从 fd 读出并触发回调。
 *
 * 讲解点：为什么 write() 到 eventfd 是 async-signal-safe 的？
 *   write() 是一个系统调用，不涉及用户态的锁或内存分配。
 *   信号处理函数中可以做以下事情：
 *   - 读写全局 volatile sig_atomic_t 变量
 *   - 调用 write() / read() 系统调用
 *   - 调用 _exit()
 *   不能做：
 *   - malloc/free（涉及锁）
 *   - printf/fprintf（涉及锁和缓冲区）
 *   - 大部分 C++ 标准库函数
 *
 * eventfd 比 pipe 更好：只需一个 fd，内核实现更轻量。
 */
static int g_signalEventFd = -1;

/// 通用的信号处理函数：把信号编号写入 eventfd
static void signalHandler(int signum) {
    (void)signum;  // 参数未使用，但我们不需要知道具体是哪个信号
    if (g_signalEventFd >= 0) {
        uint64_t one = 1;
        ::write(g_signalEventFd, &one, sizeof(one));
    }
}

SignalHandler::SignalHandler(EventLoop* loop,
                             const std::initializer_list<int>& siglist,
                             SignalCallback cb)
    : loop_(loop)
    , callback_(std::move(cb))
{
    // 1. 创建 eventfd（信号处理函数将向它写入）
    eventFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventFd_ < 0) {
        LOG_ERROR("eventfd failed: " << strerror(errno));
        std::abort();
    }

    // 2. 注册到 EventLoop 的 epoll 中
    eventFdChannel_ = std::make_unique<Channel>(loop_, eventFd_);
    eventFdChannel_->setReadCallback([this] { handleRead(); });
    eventFdChannel_->enableReading();

    // 3. 设置全局变量（信号处理函数用）
    g_signalEventFd = eventFd_;

    // 4. 为每个信号安装处理函数
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    ::sigemptyset(&sa.sa_mask);

    for (int sig : siglist) {
        // 添加当前信号到 sa_mask（处理期间屏蔽此信号，防止重入）
        ::sigaddset(&sa.sa_mask, sig);
        if (::sigaction(sig, &sa, nullptr) < 0) {
            LOG_ERROR("sigaction(" << sig << ") failed: " << strerror(errno));
            std::abort();
        }
    }

    LOG_INFO("SignalHandler created, monitoring " << siglist.size()
              << " signals via self-pipe");
}

SignalHandler::~SignalHandler() {
    // 恢复默认信号处理
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    ::sigemptyset(&sa.sa_mask);

    // SIGINT 和 SIGTERM 恢复默认
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    g_signalEventFd = -1;
    eventFdChannel_->disableAll();
    loop_->removeChannel(eventFdChannel_.get());
    ::close(eventFd_);
}

void SignalHandler::handleRead() {
    loop_->assertInLoopThread();

    uint64_t one = 0;
    ssize_t n = ::read(eventFd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        LOG_ERROR("signal eventfd read error: " << strerror(errno));
        return;
    }

    // eventfd 的值包含了信号编号（写入时的 signum）
    // 但我们只关心信号到达，不关心是哪个信号
    LOG_INFO("Signal received, initiating graceful shutdown...");

    if (callback_) {
        // 简化处理：传递 SIGINT（实际信号编号不重要，都是 shutdown）
        callback_(SIGINT);
    }
}
