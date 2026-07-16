#include "websocket.h"
#include "core/buffer.h"

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <cstring>
#include <cstdint>
#include <arpa/inet.h>  // htons

// htobe64 的便携实现（musl/glibc 通用）
#if defined(__linux__)
#include <endian.h>
#else
static inline uint64_t htobe64(uint64_t x) {
    return ((uint64_t)htonl(x & 0xFFFFFFFF) << 32) | htonl(x >> 32);
}
#endif

// ============================================================
// WebSocketParser — 帧解析器
// ============================================================

/**
 * parse — 从缓冲区解析 WebSocket 帧。
 *
 * 状态机逐字节解析：
 * 1. 读取 2 字节头部（Fin/Opcode/Mask/PayloadLen）
 * 2. 根据 PayloadLen 判断是否需要扩展长度（126 → 2字节, 127 → 8字节）
 * 3. 如果 MASK=1，读取 4 字节 Masking Key
 * 4. 读取 Payload 数据
 * 5. 如果 masked，用 Masking Key XOR 解码
 *
 * 讲解点：WebSocket 帧设计
 *   前 2 字节包含了几乎所有元信息：
 *   - 第 1 字节: Fin(1bit) | RSV(3bit) | Opcode(4bit)
 *   - 第 2 字节: Mask(1bit) | PayloadLen(7bit)
 *   Fin=0 表示分片帧的中间片，Fin=1 表示最终片
 *   控制帧（Opcode≥8）不能分片，且可能出现在数据帧之间
 *   客户端→服务器的帧必须 masked，服务器→客户端禁止 mask
 */
bool WebSocketParser::parse(Buffer* buf) {
    while (buf->readableBytes() > 0 && state_ != State::kGotFrame && state_ != State::kError) {
        switch (state_) {
            case State::kExpectHeader: {
                if (buf->readableBytes() < 2) return false;  // 需要至少 2 字节

                uint8_t b1 = static_cast<uint8_t>(*buf->peek());
                uint8_t b2 = static_cast<uint8_t>(*(buf->peek() + 1));

                // 解析第 1 字节
                frame_.fin = (b1 & 0x80) != 0;
                frame_.opcode = b1 & 0x0F;

                // RSV 位必须为 0（我们没有协商扩展）
                if (b1 & 0x70) {
                    state_ = State::kError;
                    return false;
                }

                // 解析第 2 字节
                masked_ = (b2 & 0x80) != 0;
                payloadLen_ = b2 & 0x7F;

                buf->retrieve(2);  // 消费头部的 2 字节

                // 根据 PayloadLen 决定是否需要扩展长度
                if (payloadLen_ == 126) {
                    state_ = State::kExpectExtLen16;
                } else if (payloadLen_ == 127) {
                    state_ = State::kExpectExtLen64;
                } else if (masked_) {
                    state_ = State::kExpectMask;
                } else if (payloadLen_ == 0) {
                    state_ = State::kGotFrame;  // 空 payload 帧
                } else {
                    state_ = State::kExpectPayload;
                }
                break;
            }

            case State::kExpectExtLen16: {
                if (buf->readableBytes() < 2) return false;
                uint16_t len;
                std::memcpy(&len, buf->peek(), 2);
                payloadLen_ = ntohs(len);
                buf->retrieve(2);

                if (masked_) {
                    state_ = State::kExpectMask;
                } else if (payloadLen_ == 0) {
                    state_ = State::kGotFrame;
                } else {
                    state_ = State::kExpectPayload;
                }
                break;
            }

            case State::kExpectExtLen64: {
                if (buf->readableBytes() < 8) return false;
                uint64_t len;
                std::memcpy(&len, buf->peek(), 8);
                // 网络字节序 → 主机字节序
                payloadLen_ = ((uint64_t)ntohl(len & 0xFFFFFFFF) << 32) | ntohl(len >> 32);
                buf->retrieve(8);

                if (masked_) {
                    state_ = State::kExpectMask;
                } else if (payloadLen_ == 0) {
                    state_ = State::kGotFrame;
                } else {
                    state_ = State::kExpectPayload;
                }
                break;
            }

            case State::kExpectMask: {
                if (buf->readableBytes() < 4) return false;
                std::memcpy(mask_, buf->peek(), 4);
                buf->retrieve(4);

                if (payloadLen_ == 0) {
                    state_ = State::kGotFrame;
                } else {
                    state_ = State::kExpectPayload;
                }
                break;
            }

            case State::kExpectPayload: {
                size_t available = buf->readableBytes();
                size_t needed = payloadLen_ - bytesRead_;
                size_t toRead = std::min(available, needed);

                if (toRead > 0) {
                    std::string chunk(buf->peek(), toRead);
                    frame_.payload += chunk;
                    buf->retrieve(toRead);
                    bytesRead_ += toRead;
                }

                if (bytesRead_ >= payloadLen_) {
                    // 如果 masked，解码 payload
                    if (masked_) {
                        unmaskPayload();
                    }
                    state_ = State::kGotFrame;
                }
                break;
            }

            default:
                return state_ == State::kGotFrame;
        }
    }
    return state_ == State::kGotFrame;
}

void WebSocketParser::unmaskPayload() {
    for (size_t i = 0; i < frame_.payload.size(); ++i) {
        frame_.payload[i] ^= mask_[i % 4];
    }
}

// ============================================================
// WebSocket 工具函数
// ============================================================

namespace websocket {

std::string computeAccept(const std::string& clientKey) {
    // 拼接客户端 Key 和魔术 GUID
    std::string data = clientKey + ws::kMagicGUID;

    // SHA1 哈希
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);

    // Base64 编码
    int encodedLen = 4 * ((SHA_DIGEST_LENGTH + 2) / 3);
    std::string result(encodedLen + 1, '\0');  // +1 for null terminator
    int actualLen = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(&result[0]),
        hash, SHA_DIGEST_LENGTH);
    result.resize(actualLen);
    return result;
}

std::string encodeFrame(uint8_t opcode, const std::string& payload) {
    std::string frame;

    // 第 1 字节: Fin(1) | RSV(000) | Opcode(4)
    uint8_t b1 = 0x80 | (opcode & 0x0F);
    frame.push_back(static_cast<char>(b1));

    // 第 2+ 字节: Mask(0) | PayloadLen
    size_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        frame.push_back(126);
        uint16_t netLen = htons(static_cast<uint16_t>(len));
        frame.append(reinterpret_cast<const char*>(&netLen), 2);
    } else {
        frame.push_back(127);
        uint64_t netLen = htobe64(len);
        frame.append(reinterpret_cast<const char*>(&netLen), 8);
    }

    // Payload（服务器→客户端，不设置 Mask）
    frame += payload;

    return frame;
}

std::string encodeCloseFrame(uint16_t code, const std::string& reason) {
    std::string payload;
    uint16_t netCode = htons(code);
    payload.append(reinterpret_cast<const char*>(&netCode), 2);
    payload += reason;

    return encodeFrame(ws::kClose, payload);
}

}  // namespace websocket
