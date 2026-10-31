#include "config.h"
#include "nlohmann/json.hpp"
#include "utils/logger.h"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace {
void envString(const char* name, std::string& value) {
    if (const char* env = std::getenv(name)) value = env;
}

unsigned envNumber(const char* name, unsigned fallback, unsigned maximum) {
    const char* env = std::getenv(name);
    if (!env) return fallback;
    std::string_view text(env);
    unsigned value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value == 0 || value > maximum) {
        throw std::runtime_error(std::string("Invalid numeric environment variable: ") + name);
    }
    return value;
}
}

Config Config::load(const char* json_path) {
    Config config;
    nlohmann::json json = nlohmann::json::object();
    std::ifstream file(json_path);
    if (file.is_open()) {
        file >> json;
        if (!json.is_object()) throw std::runtime_error("Configuration must be a JSON object");
    } else {
        LOG_WARNING("Config file unavailable; using defaults and environment variables");
    }

    if (json.contains("server")) {
        const auto& server = json.at("server");
        config.server_config_.host_ip_ = server.value("host", config.server_config_.host_ip_);
        config.server_config_.port_ = server.value("port", config.server_config_.port_);
    }
    config.workers_ = json.value("workers", config.workers_);
    auto& db = config.database_config_;
    if (json.contains("database")) {
        const auto& database = json.at("database");
        db.host = database.value("host", db.host);
        db.port = database.value("port", db.port);
        db.database = database.value("name", db.database);
        db.user = database.value("user", db.user);
        db.password = database.value("password", db.password);
        db.poolSize = database.value("pool_size", db.poolSize);
        db.connectTimeoutSec = database.value("connect_timeout_seconds", db.connectTimeoutSec);
        db.readTimeoutSec = database.value("read_timeout_seconds", db.readTimeoutSec);
        db.writeTimeoutSec = database.value("write_timeout_seconds", db.writeTimeoutSec);
        db.acquireTimeoutMs = database.value("acquire_timeout_ms", db.acquireTimeoutMs);
        db.sslMode = database.value("ssl_mode", db.sslMode);
        db.sslCa = database.value("ssl_ca", db.sslCa);
    }
    // 即使配置文件不存在，环境变量也必须生效。密码不写入示例JSON。
    envString("NETCHAT_DB_HOST", db.host);
    envString("NETCHAT_DB_NAME", db.database);
    envString("NETCHAT_DB_USER", db.user);
    envString("NETCHAT_DB_PASSWORD", db.password);
    envString("NETCHAT_DB_SSL_MODE", db.sslMode);
    envString("NETCHAT_DB_SSL_CA", db.sslCa);
    db.port = envNumber("NETCHAT_DB_PORT", db.port, 65535);
    db.poolSize = envNumber("NETCHAT_DB_POOL_SIZE", static_cast<unsigned>(db.poolSize), 128);
    db.connectTimeoutSec = envNumber("NETCHAT_DB_CONNECT_TIMEOUT", db.connectTimeoutSec, 300);
    db.readTimeoutSec = envNumber("NETCHAT_DB_READ_TIMEOUT", db.readTimeoutSec, 300);
    db.writeTimeoutSec = envNumber("NETCHAT_DB_WRITE_TIMEOUT", db.writeTimeoutSec, 300);
    db.acquireTimeoutMs = envNumber("NETCHAT_DB_ACQUIRE_TIMEOUT_MS", db.acquireTimeoutMs, 300000);
    if (db.host.empty() || db.database.empty() || db.user.empty() || db.password.empty()) {
        throw std::runtime_error("MySQL host/name/user/password required; set NETCHAT_DB_PASSWORD");
    }
    if (db.port == 0 || db.port > 65535 || db.poolSize == 0 || db.poolSize > 128 ||
        db.connectTimeoutSec == 0 || db.connectTimeoutSec > 300 ||
        db.readTimeoutSec == 0 || db.readTimeoutSec > 300 ||
        db.writeTimeoutSec == 0 || db.writeTimeoutSec > 300 ||
        db.acquireTimeoutMs == 0 || db.acquireTimeoutMs > 300000) {
        throw std::runtime_error("MySQL port, pool size or timeout out of range");
    }
    if (db.sslMode != "DISABLED" && db.sslMode != "REQUIRED" && db.sslMode != "VERIFY_IDENTITY") {
        throw std::runtime_error("MySQL ssl_mode must be DISABLED, REQUIRED or VERIFY_IDENTITY");
    }
    if (config.server_config_.port_ == 0 || config.server_config_.port_ > 65535 || config.workers_ < 0) {
        throw std::runtime_error("Invalid server port or worker count");
    }
    if (json.contains("ssl")) {
        const auto& ssl = json.at("ssl");
        config.ssl_config_.enabled_ = ssl.value("enabled", false);
        config.ssl_config_.cert_path_ = ssl.value("cert_path", config.ssl_config_.cert_path_);
        config.ssl_config_.key_path_ = ssl.value("key_path", config.ssl_config_.key_path_);
    }
    return config;
}
