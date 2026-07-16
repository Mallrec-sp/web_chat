#ifndef NETCHAT_INETADDRESS_H
#define NETCHAT_INETADDRESS_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>
#include <cstring>

/**
 * InetAddress — 对 struct sockaddr_in 的 C++ 封装。
 *
 * 职责：
 *   - 持有 IP 地址和端口
 *   - 构造时可以从字符串+端口构造，也可以从 sockaddr_in 构造
 *   - 提供 toIp() / toIpPort() 等字符串转换
 *
 * 设计思路：
 *   把 C 风格的 struct sockaddr_in 藏起来，让上层代码不用手动处理
 *   htons/htonl/inet_ntop 这些细节。这是典型的"封装变化"。
 */
class InetAddress {
public:
    /// 默认构造（全零）
    InetAddress() { std::memset(&addr_, 0, sizeof(addr_)); }

    /**
     * 从 IP 字符串 + 端口构造
     * @param ip   IPv4 地址，如 "127.0.0.1" 或 "0.0.0.0"
     * @param port 端口号（主机字节序，内部自动转为网络字节序）
     *
     * 讲解点：inet_pton 把点分十进制转成 in_addr 的二进制形式，
     * htons 把端口转成网络字节序（大端）。这两个是网络编程基本功。
     */
    explicit InetAddress(const std::string& ip, uint16_t port);

    /// 仅端口构造（IP = INADDR_ANY，即 0.0.0.0）
    explicit InetAddress(uint16_t port);

    /// 从 sockaddr_in 构造（用于 accept4 返回对端地址时）
    explicit InetAddress(const struct sockaddr_in& addr) : addr_(addr) {}

    // ---- 访问器 ----

    /// 返回 IP 字符串，如 "192.168.1.1"
    std::string toIp() const;

    /// 返回 "IP:Port" 字符串，如 "192.168.1.1:8080"
    std::string toIpPort() const;

    /// 返回端口（主机字节序）
    uint16_t port() const { return ntohs(addr_.sin_port); }

    /// 返回底层 C 结构体（给 bind/connect 等系统调用使用）
    const struct sockaddr_in& getSockAddr() const { return addr_; }
    void setSockAddr(const struct sockaddr_in& addr) { addr_ = addr; }

private:
    struct sockaddr_in addr_;
};

#endif // NETCHAT_INETADDRESS_H
