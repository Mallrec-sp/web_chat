#include "database.h"
#include "utils/logger.h"

#include <sqlite3.h>
#include <cstring>

Database::Database(const std::string& dbPath) {
    int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc != SQLITE_OK) {
        LOG_ERROR("Failed to open database: " << dbPath
                  << " error: " << sqlite3_errmsg(db_));
        db_ = nullptr;
        return;
    }

    // 启用 WAL 模式 — 读写不阻塞
    execute("PRAGMA journal_mode=WAL");
    // 启用外键约束
    execute("PRAGMA foreign_keys=ON");
    // 同步模式设为 NORMAL（WAL 模式下 crash-safe 且性能好）
    execute("PRAGMA synchronous=NORMAL");

    LOG_INFO("Database opened: " << dbPath << " (WAL mode)");
}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Database::execute(const std::string& sql) {
    if (!db_) return false;

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR("SQL error: " << (errMsg ? errMsg : "unknown")
                  << " SQL: " << sql.substr(0, 100));
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

Database::Row Database::queryOne(
    const std::string& sql,
    std::initializer_list<std::pair<int, std::string>> params) {

    auto rows = query(sql, params);
    return rows.empty() ? Row{} : rows[0];
}

std::vector<Database::Row> Database::query(
    const std::string& sql,
    std::initializer_list<std::pair<int, std::string>> params) {

    std::vector<Row> result;
    if (!db_) return result;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR("SQL prepare error: " << sqlite3_errmsg(db_)
                  << " SQL: " << sql.substr(0, 80));
        return result;
    }

    // 绑定参数
    for (const auto& [index, value] : params) {
        sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }

    // 执行并读取结果
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Row row;
        int colCount = sqlite3_column_count(stmt);
        for (int i = 0; i < colCount; ++i) {
            const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
            row.columns.push_back(text ? text : "");
        }
        result.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        LOG_ERROR("SQL step error: " << sqlite3_errmsg(db_));
    }

    return result;
}

bool Database::exec(
    const std::string& sql,
    std::initializer_list<std::pair<int, std::string>> params) {

    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR("SQL exec prepare error: " << sqlite3_errmsg(db_)
                  << " SQL: " << sql.substr(0, 80));
        return false;
    }

    for (const auto& [index, value] : params) {
        sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        LOG_ERROR("SQL exec error: " << sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool Database::beginTransaction() {
    return execute("BEGIN TRANSACTION");
}

bool Database::commit() {
    return execute("COMMIT");
}

bool Database::rollback() {
    return execute("ROLLBACK");
}

int64_t Database::lastInsertId() {
    if (!db_) return 0;
    return sqlite3_last_insert_rowid(db_);
}

std::string Database::error() const {
    return db_ ? (sqlite3_errmsg(db_) ? sqlite3_errmsg(db_) : "") : "database not open";
}
