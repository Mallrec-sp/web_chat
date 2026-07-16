#ifndef NETCHAT_WEBSOCKET_H
#define NETCHAT_WEBSOCKET_H

#include <string>
#include <cstdint>
#include <functional>
#include <memory>
#include <cstring>

class Buffer;

/**
 * WebSocket 协议常量 (RFC 6455)
 */
namespace ws {
    /// WebSocket 魔术 GUID（用于握手）
    constexpr const char* kMagicGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    /// Opcode 定义
    enum Opcode : uint8_t {
        kContinuation = 0x0,
        kText         = 0x1,
        kBinary       = 0x2,
        kClose        = 0x8,
        kPing         = 0x9,
        kPong         = 0xA
    };

    /// 关闭帧状态码
    enum CloseCode : uint16_t {
        kNormalClosure    = 1000,
        kGoingAway        = 1001,
        kProtocolError    = 1002,
        kUnsupportedData  = 1003,
        kNoStatusRcvd     = 1005,
        kAbnormalClosure  = 1006,
        kInvalidPayload   = 1007,
        kPolicyViolation  = 1008,
        kMessageTooBig    = 1009,
        kMandatoryExt     = 1010,
        kInternalError    = 1011
    };
}

/**
 * WebSocketFrame — 已解析的 WebSocket 数据帧。
 *
 * 由 WebSocketParser 解析后生成。
 * 包含帧头部信息和未屏蔽的 payload 数据。
 */
struct WebSocketFrame {
    bool fin     = true;       ///< 是否为最终帧
    uint8_t opcode = 0;        ///< 帧类型（文本/二进制/关闭/Ping/Pong）
    std::string payload;       ///< 已解码的 payload 数据

    /// 是否为数据帧（文本或二进制）
    bool isDataFrame() const {
        return opcode == ws::kText || opcode == ws::kBinary;
    }

    /// 是否为控制帧
    bool isControlFrame() const {
        return opcode >= 0x8;
    }
};

/**
 * WebSocketParser — WebSocket 帧解析器（有限状态机）。
 *
 * WebSocket 帧是二进制协议，逐字节解析。
 * 状态迁移：
 *   kExpectHeader → kExpectExtLen (if needed)
 *                 → kExpectMask (if masked)
 *                 → kExpectPayload
 *                 → kGotFrame
 *
 * 讲解点：为什么不用行级解析（像 HTTP 那样）？
 *   WebSocket 帧是二进制协议，没有 "行" 的概念。
 *   每个字节都有特定含义（bit 级别的标志位）。
 *   所以只能用逐字节的 FSM 解析。
 *   而且 payload 长度可能很大（> 2^63 bytes），
 *   我们需要支持 Extended Payload Length。
 */
class WebSocketParser {
public:
    enum class State {
        kExpectHeader,       ///< 等待前 2 字节（Fin/Opcode/Mask/Length）
        kExpectExtLen16,     ///< 等待 2 字节扩展长度
        kExpectExtLen64,     ///< 等待 8 字节扩展长度
        kExpectMask,         ///< 等待 4 字节 Masking Key
        kExpectPayload,      ///< 等待 payload 数据
        kGotFrame,           ///< 完整帧已就绪
        kError               ///< 解析出错
    };

    WebSocketParser() { reset(); }

    /// 重置解析器
    void reset() {
        state_ = State::kExpectHeader;
        frame_ = WebSocketFrame();
        payloadLen_ = 0;
        bytesRead_ = 0;
        std::memset(mask_, 0, sizeof(mask_));
        masked_ = false;
    }

    State state() const { return state_; }
    const WebSocketFrame& frame() const { return frame_; }
    bool isComplete() const { return state_ == State::kGotFrame; }

    /**
     * parse — 从缓冲区解析 WebSocket 帧。
     *
     * @param buf 输入缓冲区
     * @return true 表示一个完整帧已就绪，false 表示需要更多数据
     */
    bool parse(Buffer* buf);

private:
    // 内部状态
    State state_ = State::kExpectHeader;
    WebSocketFrame frame_;
    uint64_t payloadLen_ = 0;
    uint8_t mask_[4];
    bool masked_ = false;
    size_t bytesRead_ = 0;  ///< payload 已读字节数

    void unmaskPayload();
};

// =====================================================
// WebSocket 工具函数
// =====================================================

namespace websocket {

/**
 * computeAccept — 计算 Sec-WebSocket-Accept 值。
 *
 * 算法：base64(sha1(clientKey + magicGUID))
 *
 * 讲解点：为什么要有这个计算？
 *   WebSocket 握手需要验证客户端确实是一个 WebSocket 客户端
 *   （而不是一个普通的 HTTP 请求伪造的 Upgrade）。
 *   Sec-WebSocket-Key 是客户端发送的随机 base64 值，
 *   服务器拼接魔术 GUID 后做 SHA1 + base64，返回 Accept。
 *   客户端验证 Accept 值确保服务器确实实现了 WebSocket。
 *   这种机制称为 "Challenge-Response Authentication"。
 *
 * @param clientKey 客户端发送的 Sec-WebSocket-Key
 * @return 服务器应返回的 Sec-WebSocket-Accept 值
 */
std::string computeAccept(const std::string& clientKey);

/**
 * encodeFrame — 编码一个 WebSocket 发送帧。
 *
 * 服务器 → 客户端：不设置 MASK 位（RFC 6455 规定只有客户端需要 masking）
 *
 * @param opcode 帧类型（ws::kText / ws::kBinary / ws::kClose / ws::kPing / ws::kPong）
 * @param payload 数据内容
 * @return 编码后的完整帧（可直接写入 socket）
 */
std::string encodeFrame(uint8_t opcode, const std::string& payload);

/**
 * encodeCloseFrame — 编码关闭帧。
 *
 * @param code 关闭状态码（如 ws::kNormalClosure）
 * @param reason 关闭原因（可空）
 * @return 编码后的关闭帧
 */
std::string encodeCloseFrame(uint16_t code, const std::string& reason = "");

}  // namespace websocket

#endif // NETCHAT_WEBSOCKET_H
