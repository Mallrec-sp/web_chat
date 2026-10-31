#include "migrations.h"
#include "database.h"

#include <chrono>

namespace {
// GET_LOCK 名称最长 64 字节；对数据库名取摘要，避免长名称截断和不同库互相阻塞。
constexpr const char* kAcquireLock =
    "SELECT GET_LOCK(SHA2(CONCAT('netchat.schema:', DATABASE()), 256), ?)";
constexpr const char* kReleaseLock =
    "SELECT RELEASE_LOCK(SHA2(CONCAT('netchat.schema:', DATABASE()), 256))";
constexpr const char* kVersionQuery =
    "SELECT COUNT(*), COALESCE(MIN(version), 0), COALESCE(MAX(version), 0) "
    "FROM schema_migrations";

bool isCurrentVersion(const Database::Row& row) {
    return !row.empty() && row.getInt(0) == 1 &&
           row.getInt(1) == kSchemaVersion && row.getInt(2) == kSchemaVersion;
}

void releaseMigrationLock(Database::Transaction& connection) {
    const auto row = connection.queryOne(kReleaseLock);
    if (row.empty() || row.isNull(0) || row.getInt(0) != 1) {
        throw DbError(0, "HY000", "Failed to release database migration lock");
    }
}
} // namespace

bool runMigrations(Database& db) {
    // 这里只借 Transaction 固定池连接，不把 DDL 误认为可回滚操作。
    // START TRANSACTION 会被首条 DDL 隐式提交，但对象仍持有同一连接租约。
    auto connection = db.transaction();
    try {
        const auto acquired = connection.queryOne(kAcquireLock, {{1, int64_t{30}}});
        if (acquired.empty() || acquired.isNull(0)) {
            throw DbError(0, "HY000", "Failed to acquire database migration lock");
        }
        if (acquired.getInt(0) != 1) {
            connection.rollback();
            return false;
        }

        connection.execute(
            "CREATE TABLE IF NOT EXISTS schema_migrations ("
            " version BIGINT NOT NULL PRIMARY KEY,"
            " applied_at BIGINT NOT NULL"
            ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin");

        const auto version = connection.queryOne(kVersionQuery);
        if (version.empty()) {
            throw DbError(0, "HY000", "Database schema version is unavailable");
        }
        if (version.getInt(0) != 0) {
            const bool current = isCurrentVersion(version);
            releaseMigrationLock(connection);
            connection.rollback();
            return current;
        }

        // 用户名校验现有语义是 std::string 的 2..32 字节，并非 Unicode 码点数量。
        // 0900_bin 为 NO PAD，精确区分大小写和尾空格；不要换成 utf8mb4_bin。
        connection.execute(
            "CREATE TABLE IF NOT EXISTS users ("
            " id BIGINT NOT NULL PRIMARY KEY,"
            " username VARCHAR(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin NOT NULL,"
            " password_hash VARCHAR(96) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
            " nickname VARCHAR(255) NOT NULL DEFAULT '',"
            " created_at BIGINT NOT NULL,"
            " UNIQUE KEY uq_users_username (username),"
            " CONSTRAINT chk_users_username_length CHECK (OCTET_LENGTH(username) BETWEEN 2 AND 32),"
            " CONSTRAINT chk_users_hash_length CHECK (OCTET_LENGTH(password_hash) = 96)"
            ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin");

        // token/hash 都是现有鉴权算法生成的固定长度 ASCII 十六进制文本。
        // ascii_bin 区分大小写；鉴权查询额外 CAST 参数为 BINARY，避免 PAD SPACE 别名。
        connection.execute(
            "CREATE TABLE IF NOT EXISTS tokens ("
            " token VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,"
            " user_id BIGINT NOT NULL,"
            " created_at BIGINT NOT NULL,"
            " KEY idx_tokens_user (user_id),"
            " CONSTRAINT chk_tokens_length CHECK (OCTET_LENGTH(token) = 64)"
            ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin");

        connection.execute(
            "CREATE TABLE IF NOT EXISTS messages ("
            " id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
            " from_user_id BIGINT NOT NULL,"
            " to_user_id BIGINT NOT NULL,"
            " content LONGTEXT NOT NULL,"
            " created_at BIGINT NOT NULL,"
            " KEY idx_messages_from (from_user_id, to_user_id, created_at),"
            " KEY idx_messages_to (to_user_id, from_user_id, created_at),"
            " KEY idx_messages_time (created_at)"
            ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin");

        connection.execute(
            "CREATE TABLE IF NOT EXISTS friendships ("
            " user_id BIGINT NOT NULL,"
            " friend_id BIGINT NOT NULL,"
            " status TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            " created_at BIGINT NOT NULL,"
            " PRIMARY KEY (user_id, friend_id),"
            " KEY idx_friendships_user (user_id, status),"
            " KEY idx_friendships_friend (friend_id, status, created_at),"
            " CONSTRAINT chk_friendships_status CHECK (status IN (0, 1))"
            ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin");

        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        connection.exec("INSERT INTO schema_migrations (version, applied_at) VALUES (?, ?)",
                        {{1, kSchemaVersion}, {2, now}});
        // DDL 已提交；明确提交版本标记且继续持有租约。不能在这里调用
        // Transaction::commit()，它会在 RELEASE_LOCK 前将连接归还池。
        connection.execute("COMMIT");
        releaseMigrationLock(connection);
        connection.rollback(); // 结束租约；不会撤销任何已完成的 DDL/版本记录。
        return true;
    } catch (...) {
        // GET_LOCK 不随 ROLLBACK 释放。发生任何异常直接关闭此租约连接，
        // 包括 RELEASE_LOCK 自身失败的情形，绝不让带命名锁的连接重返池。
        connection.discard();
        throw;
    }
}

bool verifySchema(Database& db) {
    try {
        return isCurrentVersion(db.queryOne(kVersionQuery));
    } catch (const DbError& error) {
        // 首次部署还没有版本表。其他错误（权限、断线等）必须交给启动入口处理。
        if (error.code() == 1146) return false;
        throw;
    }
}
