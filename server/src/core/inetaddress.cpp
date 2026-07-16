#include "inetaddress.h"
#include <cstring>

InetAddress::InetAddress(const std::string& ip, uint16_t port) {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);

    // inet_pton: "presentation to numeric" — 把 "192.168.1.1" 转成 in_addr
    if (::inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) <= 0) {
        // 如果失败（非法 IP），退回到 INADDR_ANY
        addr_.sin_addr.s_addr = INADDR_ANY;
    }
}

InetAddress::InetAddress(uint16_t port) {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);
    addr_.sin_addr.s_addr = INADDR_ANY;  // 监听所有网络接口
}

std::string InetAddress::toIp() const {
    char buf[64] = {0};
    // inet_ntop: "numeric to presentation" — 把 in_addr 转回字符串
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
    return buf;
}

std::string InetAddress::toIpPort() const {
    char buf[64] = {0};
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
    int end = strlen(buf);
    // 追加 ":port"
    snprintf(buf + end, sizeof(buf) - end, ":%d", ntohs(addr_.sin_port));
    return buf;
}
