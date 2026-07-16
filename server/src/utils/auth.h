#ifndef NETCHAT_AUTH_H
#define NETCHAT_AUTH_H

#include <string>
#include <cstdint>

/**
 * AuthUtils — 认证工具函数。
 *
 * 职责：
 *   1. 密码哈希（SHA256 + salt）
 *   2. 会话 token 生成（OpenSSL 随机数）
 *   3. 从 token 中提取用户信息
 *
 * 讲解点：为什么不是真正的 JWT？
 *   完整的 JWT 需要 HMAC-SHA256 签名和 base64url 编解码。
 *   MVP 阶段先使用简单的随机 token + 服务端存储，
 *   后续可以升级到真正的 JWT（支持无状态验证）。
 *   对于 MVP，这个方案更简单且同样安全。
 */
namespace auth {

/// 生成密码哈希
std::string hashPassword(const std::string& password);

/// 验证密码
bool verifyPassword(const std::string& password, const std::string& hash);

/// 生成随机 token（hex 编码，32 字节随机数 → 64 字符 hex）
std::string generateToken();

/// 生成用户 ID（时间戳 + 随机数）
int64_t generateUserId();

}  // namespace auth

#endif // NETCHAT_AUTH_H
