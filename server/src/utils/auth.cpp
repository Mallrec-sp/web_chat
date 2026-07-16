#include "auth.h"

#include <openssl/sha.h>
#include <openssl/rand.h>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <chrono>

namespace auth {

/**
 * hashPassword — SHA256(password + salt)。
 *
 * 真正的生产环境应该用 bcrypt/scrypt/Argon2，
 * 但需要额外的库。使用 SHA256 + salt 作为 MVP 方案。
 *
 * 格式：salt(32 hex) + hash(64 hex)
 * 总长度：96 字符
 */
std::string hashPassword(const std::string& password) {
    // 生成 16 字节随机 salt
    unsigned char salt[16];
    RAND_bytes(salt, sizeof(salt));

    // 拼接 salt + password
    std::string data(reinterpret_cast<const char*>(salt), sizeof(salt));
    data += password;

    // SHA256 哈希
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);

    // 编码为 hex 字符串: salt + hash
    std::ostringstream oss;
    for (int i = 0; i < 16; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(salt[i]);
    }
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(hash[i]);
    }
    return oss.str();
}

bool verifyPassword(const std::string& password, const std::string& hashStr) {
    if (hashStr.size() != 96) return false;  // 32 + 64 hex chars

    // 提取 salt（前 32 hex 字符 = 16 字节）
    unsigned char salt[16];
    for (int i = 0; i < 16; ++i) {
        std::string byteStr = hashStr.substr(i * 2, 2);
        salt[i] = static_cast<unsigned char>(std::stoul(byteStr, nullptr, 16));
    }

    // 重新计算哈希
    std::string data(reinterpret_cast<const char*>(salt), sizeof(salt));
    data += password;

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);

    // 比较哈希部分（后 64 hex 字符）
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        std::string byteStr = hashStr.substr(32 + i * 2, 2);
        if (static_cast<unsigned char>(std::stoul(byteStr, nullptr, 16)) != hash[i]) {
            return false;
        }
    }
    return true;
}

std::string generateToken() {
    // 生成 32 字节随机数
    unsigned char buf[32];
    RAND_bytes(buf, sizeof(buf));

    // Hex 编码
    std::ostringstream oss;
    for (int i = 0; i < 32; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(buf[i]);
    }
    return oss.str();
}

int64_t generateUserId() {
    // 时间戳（毫秒）+ 随机数
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    unsigned short randVal = 0;
    RAND_bytes(reinterpret_cast<unsigned char*>(&randVal), sizeof(randVal));

    return (ms << 16) | (randVal & 0xFFFF);
}

}  // namespace auth
