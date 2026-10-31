#ifndef NETCHAT_DATABASE_H
#define NETCHAT_DATABASE_H

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

struct DatabaseOptions {
    std::string host = "127.0.0.1";
    unsigned port = 3306;
    std::string database = "netchat";
    std::string user = "netchat_app";
    std::string password;
    size_t poolSize = 4;
    unsigned connectTimeoutSec = 5;
    unsigned readTimeoutSec = 5;
    unsigned writeTimeoutSec = 5;
    unsigned acquireTimeoutMs = 3000;
    std::string sslMode = "VERIFY_IDENTITY";
    std::string sslCa;
};

// 错误消息刻意排除服务端错误原文、SQL 和绑定值，避免泄露敏感信息。
class DbError : public std::runtime_error {
public:
    DbError(int code, std::string sqlState, const std::string& message);
    int code() const noexcept { return code_; }
    const std::string& sqlState() const noexcept { return sqlState_; }
private:
    int code_;
    std::string sqlState_;
};

using DbValue = std::variant<std::nullptr_t, int64_t, double, std::string>;
using Params = std::initializer_list<std::pair<int, DbValue>>;
struct ExecResult {
    uint64_t affectedRows = 0;
    uint64_t insertId = 0;
};

class Database {
public:
    using Params = ::Params;
    using ExecResult = ::ExecResult;
    struct Row {
        std::vector<std::optional<std::string>> columns;
        bool empty() const noexcept { return columns.empty(); }
        bool isNull(int i) const noexcept;
        std::string get(int i) const;
        int64_t getInt(int i) const;
    };

    // 事务独占一个连接租约，直到提交、回滚或析构时才释放。
    // 只允许移动，不允许复制；同一事务不能被多个线程并发使用。
    class Transaction {
    public:
        ~Transaction() noexcept;
        Transaction(Transaction&&) noexcept;
        Transaction& operator=(Transaction&&) noexcept;
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
        ExecResult execute(const std::string& sql);
        ExecResult exec(const std::string& sql, Params params = {});
        std::vector<Row> query(const std::string& sql, Params params = {});
        Row queryOne(const std::string& sql, Params params = {});
        void commit();
        void rollback();
        // 关闭事务固定使用的连接，同时释放会话级命名锁。
        void discard() noexcept;
    private:
        struct Impl;
        explicit Transaction(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
        friend class Database;
    };

    explicit Database(const DatabaseOptions& options);
    ~Database() noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ExecResult execute(const std::string& sql);
    ExecResult exec(const std::string& sql, Params params = {});
    std::vector<Row> query(const std::string& sql, Params params = {});
    Row queryOne(const std::string& sql, Params params = {});
    Transaction transaction();
    // 唤醒等待者并拒绝新租约，但允许已有事务继续完成。
    void shutdown() noexcept;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

#endif // NETCHAT_DATABASE_H
