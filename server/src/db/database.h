#ifndef NETCHAT_DATABASE_H
#define NETCHAT_DATABASE_H

#include <string>
#include <vector>
#include <memory>
#include <functional>

struct sqlite3;
struct sqlite3_stmt;

/**
 * Database — SQLite3 数据库封装。
 *
 * 职责：
 *   1. 打开/关闭 SQLite3 数据库
 *   2. 执行 SQL 语句（DDL/DML）
 *   3. 创建预编译语句（Statement）
 *   4. 事务管理
 *
 * 设计要点：
 *   - 打开时自动启用 WAL 模式（读写并发不阻塞）
 *   - 外键约束启用
 *   - RAII 管理 sqlite3 连接生命周期
 *
 * 讲解点：WAL 模式 vs 传统回滚日志
 *   传统模式下，读操作会阻塞写操作，写操作也会阻塞读操作。
 *   WAL（Write-Ahead Logging）模式下，读操作从不阻塞写操作，
 *   写操作也不阻塞读操作。这对聊天服务器非常重要——
 *   用户发消息（写）的同时，消息历史查询（读）可以正常进行。
 */
class Database {
public:
    /// 打开数据库（自动创建 WAL 模式）
    explicit Database(const std::string& dbPath);
    ~Database();

    // 禁止拷贝
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /// 执行 SQL（无参数）
    bool execute(const std::string& sql);

    /// 执行 SQL 并自动处理 bind/step/finalize（可变参数版本）
    /// 支持 int64_t, double, std::string, nullptr
    bool exec(const std::string& sql,
              std::initializer_list<std::pair<int, std::string>> params = {});

    /// 查询一行
    struct Row {
        std::vector<std::string> columns;
        bool empty() const { return columns.empty(); }
        std::string get(int i) const {
            return i < static_cast<int>(columns.size()) ? columns[i] : "";
        }
        int64_t getInt(int i) const {
            return i < static_cast<int>(columns.size()) ? std::stoll(columns[i]) : 0;
        }
    };

    /// 查询单行
    Row queryOne(const std::string& sql,
                 std::initializer_list<std::pair<int, std::string>> params = {});

    /// 查询多行
    std::vector<Row> query(const std::string& sql,
                           std::initializer_list<std::pair<int, std::string>> params = {});

    // ---- 事务 ----

    bool beginTransaction();
    bool commit();
    bool rollback();

    // ---- 工具 ----

    int64_t lastInsertId();
    std::string error() const;
    bool isOpen() const { return db_ != nullptr; }

private:
    sqlite3* db_ = nullptr;
};

#endif // NETCHAT_DATABASE_H
