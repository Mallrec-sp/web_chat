#include "channel.h"
#include "eventloop.h"
#include <sys/epoll.h>
#include <cassert>

/**
 * handleEvent — 根据 epoll 返回的事件分发到对应回调。
 *
 * revents 可能同时包含多个标志位，处理顺序很重要：
 * 1. EPOLLERR 或 EPOLLHUP → 优先处理错误
 * 2. (EPOLLIN | EPOLLPRI | EPOLLRDHUP) → 可读事件
 * 3. EPOLLOUT → 可写事件
 *
 * EPOLLRDHUP 是 Linux 2.6.17+ 的扩展：
 * 当对端关闭连接（发送 FIN）时触发，比读返回 0 更早通知。
 * 这使得我们不需要再调用 read() 来发现连接已关闭。
 *
 * EPOLLHUP 比较特殊：
 * - 如果同时有 EPOLLIN，表示对端在挂断前还有数据可读
 * - 如果只有 EPOLLHUP（没有 EPOLLIN），才触发 closeCallback
 */
void Channel::handleEvent() {
    if (revents_ & (EPOLLERR | EPOLLHUP)) {
        if (errorCallback_) errorCallback_();
    }
    if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (readCallback_) readCallback_();
    }
    if (revents_ & EPOLLOUT) {
        if (writeCallback_) writeCallback_();
    }
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
        // HUP 但没有数据可读 → 纯粹的对端关闭
        if (closeCallback_) closeCallback_();
    }
}

/**
 * update — 把本 Channel 注册或更新到 EventLoop 中。
 *
 * 为什么需要用 update() 而不是直接操作 epoll？
 * 因为 EventLoop 封装了 epoll 实例（epollFd_），
 * 所有 epoll_ctl 操作必须通过 EventLoop 完成。
 * 这是"封装变化"原则的体现。
 */
void Channel::update() {
    loop_->updateChannel(this);
}
