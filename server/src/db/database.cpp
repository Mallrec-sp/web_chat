#include "database.h"

#include <mysql.h>

#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <type_traits>

DbError::DbError(int code, std::string sqlState, const std::string& message)
    : std::runtime_error(message + " (code " + std::to_string(code) + ")"),
      code_(code), sqlState_(std::move(sqlState)) {}

bool Database::Row::isNull(int i) const noexcept {
    return i < 0 || static_cast<size_t>(i) >= columns.size() || !columns[i];
}
std::string Database::Row::get(int i) const {
    return isNull(i) ? std::string{} : *columns[i];
}
int64_t Database::Row::getInt(int i) const {
    return isNull(i) ? 0 : std::stoll(*columns[i]);
}

namespace {
// 进程内共享唯一的客户端库生命周期，线程局部守卫也持有其引用。
// 因此，销毁一个 Database 不会提前释放其他实例仍在使用的客户端库。
struct MysqlLibrary {
    MysqlLibrary() {
        if (mysql_library_init(0, nullptr, nullptr) != 0)
            throw DbError(0, "HY000", "MySQL library initialization failed");
    }
    ~MysqlLibrary() { mysql_library_end(); }
};
std::shared_ptr<MysqlLibrary> library() {
    static const auto instance = std::make_shared<MysqlLibrary>();
    return instance;
}
struct MysqlThread {
    std::shared_ptr<MysqlLibrary> owner = library();
    bool ready = mysql_thread_init() == 0;
    ~MysqlThread() { if (ready) mysql_thread_end(); }
};
MysqlThread& threadContext() {
    thread_local MysqlThread context;
    return context;
}
void ensureThread() {
    if (!threadContext().ready)
        throw DbError(0, "HY000", "MySQL thread initialization failed");
}

[[noreturn]] void connectionError(MYSQL* connection, const char* operation) {
    // 不使用 mysql_error/mysql_stmt_error：重复键和语法错误可能包含
    // 凭据、令牌、SQL 字面量或绑定的消息内容。
    throw DbError(static_cast<int>(mysql_errno(connection)),
                  mysql_sqlstate(connection), operation);
}
[[noreturn]] void statementError(MYSQL_STMT* statement, const char* operation) {
    throw DbError(static_cast<int>(mysql_stmt_errno(statement)),
                  mysql_stmt_sqlstate(statement), operation);
}

struct Connection {
    MYSQL* handle = nullptr;
    Connection() : handle(mysql_init(nullptr)) {
        if (!handle) throw DbError(0, "HY001", "MySQL connection allocation failed");
    }
    ~Connection() { if (handle) mysql_close(handle); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
};
using ConnectionPtr = std::unique_ptr<Connection>;

ConnectionPtr connect(const DatabaseOptions& options) {
    ensureThread();
    auto connection = std::make_unique<Connection>();
    MYSQL* mysql = connection->handle;
    const bool reconnect = false;
    const bool reportTruncation = true;
    const unsigned protocol = MYSQL_PROTOCOL_TCP;
    mysql_ssl_mode sslMode;
    if (options.sslMode == "DISABLED") sslMode = SSL_MODE_DISABLED;
    else if (options.sslMode == "REQUIRED") sslMode = SSL_MODE_REQUIRED;
    else if (options.sslMode == "VERIFY_IDENTITY") sslMode = SSL_MODE_VERIFY_IDENTITY;
    else throw DbError(0, "HY000", "Unsupported MySQL TLS mode");
    auto setOption = [&](mysql_option name, const void* value) {
        if (mysql_options(mysql, name, value) != 0)
            connectionError(mysql, "MySQL option configuration failed");
    };
    setOption(MYSQL_OPT_CONNECT_TIMEOUT, &options.connectTimeoutSec);
    setOption(MYSQL_OPT_READ_TIMEOUT, &options.readTimeoutSec);
    setOption(MYSQL_OPT_WRITE_TIMEOUT, &options.writeTimeoutSec);
    setOption(MYSQL_OPT_RECONNECT, &reconnect);
    setOption(MYSQL_REPORT_DATA_TRUNCATION, &reportTruncation);
    setOption(MYSQL_OPT_PROTOCOL, &protocol);
    setOption(MYSQL_OPT_SSL_MODE, &sslMode);
    if (!options.sslCa.empty()) setOption(MYSQL_OPT_SSL_CA, options.sslCa.c_str());
    setOption(MYSQL_SET_CHARSET_NAME, "utf8mb4");
    // 不开启 CLIENT_MULTI_STATEMENTS，也不自动获取公钥或降低 TLS 要求。
    // 认证或 TLS 失败时必须修正配置，不能静默降级。
    if (!mysql_real_connect(mysql, options.host.c_str(), options.user.c_str(),
                            options.password.c_str(), options.database.c_str(),
                            options.port, nullptr, 0))
        connectionError(mysql, "MySQL connection failed");
    // 不依赖远端全局 sql_mode，避免长文本或非法编码被静默截断；锁等待也有上限。
    if (mysql_query(mysql,
            "SET SESSION sql_mode='STRICT_ALL_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION', "
            "innodb_lock_wait_timeout=5") != 0) {
        connectionError(mysql, "MySQL session configuration failed");
    }
    return connection;
}

struct StatementDeleter {
    void operator()(MYSQL_STMT* statement) const noexcept { mysql_stmt_close(statement); }
};
struct ResultDeleter {
    void operator()(MYSQL_RES* result) const noexcept { mysql_free_result(result); }
};
using BindBool = std::remove_pointer_t<decltype(MYSQL_BIND{}.is_null)>;
struct Parameter {
    int64_t integer = 0;
    double real = 0;
    unsigned long length = 0;
    BindBool isNull = false;
};
struct Column {
    unsigned long length = 0;
    BindBool isNull = false;
    BindBool error = false;
};
struct QueryResult {
    ExecResult execution;
    std::vector<Database::Row> rows;
};

QueryResult run(MYSQL* connection, const std::string& sql, Params params, bool collect) {
    ensureThread();
    if (sql.size() > std::numeric_limits<unsigned long>::max())
        throw DbError(0, "22001", "SQL statement exceeds client length limit");
    std::unique_ptr<MYSQL_STMT, StatementDeleter> statement(mysql_stmt_init(connection));
    if (!statement) connectionError(connection, "MySQL statement allocation failed");
    MYSQL_STMT* stmt = statement.get();
    if (mysql_stmt_prepare(stmt, sql.data(), static_cast<unsigned long>(sql.size())) != 0)
        statementError(stmt, "MySQL statement preparation failed");
    const auto count = mysql_stmt_param_count(stmt);
    if (params.size() != count)
        throw DbError(0, "07001", "Incorrect SQL parameter count");
    std::vector<MYSQL_BIND> bindings(count);
    std::vector<Parameter> values(count);
    std::vector<unsigned char> seen(count, 0);
    for (const auto& [number, value] : params) {
        if (number < 1 || static_cast<unsigned long>(number) > count || seen[number - 1])
            throw DbError(0, "07009", "Invalid or duplicate SQL parameter index");
        const auto index = static_cast<size_t>(number - 1);
        seen[index] = 1;
        auto& bind = bindings[index];
        auto& storage = values[index];
        bind.is_null = &storage.isNull;
        bind.length = &storage.length;
        if (std::holds_alternative<std::nullptr_t>(value)) {
            storage.isNull = true;
            bind.buffer_type = MYSQL_TYPE_NULL;
        } else if (const auto* integer = std::get_if<int64_t>(&value)) {
            storage.integer = *integer;
            bind.buffer_type = MYSQL_TYPE_LONGLONG;
            bind.buffer = &storage.integer;
            bind.buffer_length = sizeof(storage.integer);
            bind.is_unsigned = false;
        } else if (const auto* real = std::get_if<double>(&value)) {
            storage.real = *real;
            bind.buffer_type = MYSQL_TYPE_DOUBLE;
            bind.buffer = &storage.real;
            bind.buffer_length = sizeof(storage.real);
        } else {
            const auto& text = std::get<std::string>(value);
            if (text.size() > std::numeric_limits<unsigned long>::max())
                throw DbError(0, "22001", "SQL parameter exceeds client length limit");
            storage.length = static_cast<unsigned long>(text.size());
            bind.buffer_type = MYSQL_TYPE_STRING;
            bind.buffer = const_cast<char*>(text.data());
            bind.buffer_length = storage.length;
        }
    }
    if (count && mysql_stmt_bind_param(stmt, bindings.data()) != 0)
        statementError(stmt, "MySQL parameter binding failed");
    if (mysql_stmt_execute(stmt) != 0)
        statementError(stmt, "MySQL statement execution failed");

    QueryResult result;
    const auto affected = mysql_stmt_affected_rows(stmt);
    result.execution.affectedRows = affected == static_cast<my_ulonglong>(-1) ? 0 : affected;
    result.execution.insertId = mysql_stmt_insert_id(stmt);

    // 读完所有结果，包括 CALL 末尾的状态结果，但只收集第一个结果集。
    // 连接归还池之前不能残留未读取的数据包。
    bool firstResult = true;
    for (;;) {
        const unsigned fields = mysql_stmt_field_count(stmt);
        if (fields) {
            std::unique_ptr<MYSQL_RES, ResultDeleter> metadata(mysql_stmt_result_metadata(stmt));
            if (!metadata) statementError(stmt, "MySQL result metadata failed");
            std::vector<MYSQL_BIND> outputs(fields);
            std::vector<Column> columns(fields);
            for (unsigned i = 0; i < fields; ++i) {
                outputs[i].buffer_type = MYSQL_TYPE_STRING;
                outputs[i].length = &columns[i].length;
                outputs[i].is_null = &columns[i].isNull;
                outputs[i].error = &columns[i].error;
                // 使用零长度缓冲区，让 MySQL 返回该列精确的字节长度。
                outputs[i].buffer = nullptr;
                outputs[i].buffer_length = 0;
            }
            if (mysql_stmt_bind_result(stmt, outputs.data()) != 0)
                statementError(stmt, "MySQL result binding failed");
            for (;;) {
                const int status = mysql_stmt_fetch(stmt);
                if (status == MYSQL_NO_DATA) break;
                if (status != 0 && status != MYSQL_DATA_TRUNCATED)
                    statementError(stmt, "MySQL result fetch failed");
                if (!collect || !firstResult) continue;
                Database::Row row;
                row.columns.reserve(fields);
                for (unsigned i = 0; i < fields; ++i) {
                    if (columns[i].isNull) {
                        row.columns.emplace_back(std::nullopt);
                        continue;
                    }
                    std::string text(columns[i].length, '\0');
                    if (!text.empty()) {
                        MYSQL_BIND column{};
                        unsigned long length = 0;
                        BindBool isNull = false;
                        BindBool error = false;
                        column.buffer_type = MYSQL_TYPE_STRING;
                        column.buffer = text.data();
                        column.buffer_length = static_cast<unsigned long>(text.size());
                        column.length = &length;
                        column.is_null = &isNull;
                        column.error = &error;
                        if (mysql_stmt_fetch_column(stmt, &column, i, 0) != 0)
                            statementError(stmt, "MySQL result column fetch failed");
                        if (error || isNull || length != text.size())
                            throw DbError(0, "HY000", "Inconsistent MySQL result column length");
                    }
                    row.columns.emplace_back(std::move(text));
                }
                result.rows.push_back(std::move(row));
            }
            if (mysql_stmt_free_result(stmt) != 0)
                statementError(stmt, "MySQL result cleanup failed");
        }
        const int next = mysql_stmt_next_result(stmt);
        if (next == -1) break;
        if (next != 0) statementError(stmt, "MySQL additional result failed");
        firstResult = false;
    }
    return result;
}
} // 匿名命名空间

struct Database::Impl : std::enable_shared_from_this<Database::Impl> {
    std::shared_ptr<MysqlLibrary> runtime = library();
    DatabaseOptions options;
    std::mutex mutex;
    std::condition_variable available;
    std::vector<ConnectionPtr> idle;
    size_t total = 0; // 包括已借出的连接和为新建连接预留的名额。
    bool stopping = false;

    explicit Impl(const DatabaseOptions& value) : options(value) {
        if (options.poolSize == 0 || options.port == 0 || options.port > 65535 ||
            options.connectTimeoutSec == 0 || options.readTimeoutSec == 0 ||
            options.writeTimeoutSec == 0)
            throw DbError(0, "HY000", "Invalid MySQL pool configuration");
        ensureThread();
        idle.reserve(options.poolSize);
        // 初始化失败时销毁所有已创建的连接，不留下部分可用的连接池。
        for (size_t i = 0; i < options.poolSize; ++i) idle.push_back(connect(options));
        total = idle.size();
    }

    struct Lease {
        std::shared_ptr<Impl> owner;
        ConnectionPtr connection;
        bool healthy = true;
        Lease(std::shared_ptr<Impl> pool, ConnectionPtr value)
            : owner(std::move(pool)), connection(std::move(value)) {}
        Lease(Lease&& other) noexcept = default;
        Lease& operator=(Lease&&) = delete;
        Lease(const Lease&) = delete;
        ~Lease() noexcept {
            if (!connection) return;
            // 析构所在的线程可能与借出连接的线程不同。
            try { ensureThread(); } catch (...) { healthy = false; }
            {
                std::lock_guard<std::mutex> lock(owner->mutex);
                if (healthy && !owner->stopping)
                    owner->idle.push_back(std::move(connection)); // 容量已预留。
            }
            if (connection) {
                // 关闭完成前仍占用池名额，避免重建连接时出现
                // 实际连接数暂时超过池容量上限的情况。
                connection.reset();
                std::lock_guard<std::mutex> lock(owner->mutex);
                --owner->total;
            }
            owner->available.notify_all();
        }
        void startTransaction() {
            try {
                ensureThread();
                // MySQL 不支持预编译 START TRANSACTION；这里只执行固定控制命令。
                if (mysql_query(connection->handle, "START TRANSACTION") != 0)
                    connectionError(connection->handle, "MySQL transaction start failed");
            } catch (...) {
                healthy = false;
                throw;
            }
        }
        QueryResult invoke(const std::string& sql, Params params, bool collect) {
            if (!healthy)
                throw DbError(0, "25000", "Database transaction is no longer usable");
            try { return run(connection->handle, sql, params, collect); }
            catch (...) {
                // 保守处理：任何操作失败都淘汰该连接，同时防止
                // 已被 MySQL 隐式中止的事务继续执行提交。
                healthy = false;
                throw;
            }
        }
    };

    Lease acquire() {
        ensureThread();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(options.acquireTimeoutMs);
        ConnectionPtr connection;
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!available.wait_until(lock, deadline, [&] {
                    return stopping || !idle.empty() || total < options.poolSize;
                }))
                throw DbError(0, "HYT00", "Database connection pool acquisition timed out");
            if (stopping) throw DbError(0, "08003", "Database connection pool is shut down");
            if (!idle.empty()) {
                connection = std::move(idle.back());
                idle.pop_back();
            } else {
                ++total;
            }
        }
        try {
            // 仅在交出租约前探测连接，绝不重连或重放执行中的语句、事务。
            // MySQL 的自动重连功能始终关闭。
            if (connection && mysql_ping(connection->handle) != 0) connection.reset();
            if (!connection) connection = connect(options);
        } catch (...) {
            { std::lock_guard<std::mutex> lock(mutex); --total; }
            available.notify_all();
            throw;
        }
        Lease lease(shared_from_this(), std::move(connection));
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) throw DbError(0, "08003", "Database connection pool is shut down");
        }
        return lease;
    }

    void shutdown() noexcept {
        std::vector<ConnectionPtr> closing;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            total -= idle.size();
            closing.swap(idle);
        }
        available.notify_all();
        if (!closing.empty()) {
            try { ensureThread(); } catch (...) { /* 仍需释放 C 接口资源。 */ }
        }
    }
};

struct Database::Transaction::Impl {
    Database::Impl::Lease lease;
    bool active = true;
    explicit Impl(Database::Impl::Lease value) : lease(std::move(value)) {}
    ~Impl() noexcept {
        if (!active || !lease.healthy) return;
        try { lease.invoke("ROLLBACK", {}, false); }
        catch (...) { lease.healthy = false; }
    }
    void requireActive() const {
        if (!active || !lease.healthy)
            throw DbError(0, "25000", "Database transaction is no longer active");
    }
};

Database::Database(const DatabaseOptions& options) : impl_(std::make_shared<Impl>(options)) {}
Database::~Database() noexcept { shutdown(); }
void Database::shutdown() noexcept { impl_->shutdown(); }
ExecResult Database::execute(const std::string& sql) { return exec(sql); }
ExecResult Database::exec(const std::string& sql, Params params) {
    auto lease = impl_->acquire();
    return lease.invoke(sql, params, false).execution;
}
std::vector<Database::Row> Database::query(const std::string& sql, Params params) {
    auto lease = impl_->acquire();
    return lease.invoke(sql, params, true).rows;
}
Database::Row Database::queryOne(const std::string& sql, Params params) {
    auto rows = query(sql, params);
    return rows.empty() ? Row{} : std::move(rows.front());
}
Database::Transaction Database::transaction() {
    auto transaction = std::make_unique<Transaction::Impl>(impl_->acquire());
    transaction->lease.startTransaction();
    return Transaction(std::move(transaction));
}

Database::Transaction::Transaction(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Database::Transaction::~Transaction() noexcept = default;
Database::Transaction::Transaction(Transaction&&) noexcept = default;
Database::Transaction& Database::Transaction::operator=(Transaction&&) noexcept = default;
ExecResult Database::Transaction::execute(const std::string& sql) { return exec(sql); }
ExecResult Database::Transaction::exec(const std::string& sql, Params params) {
    if (!impl_) throw DbError(0, "25000", "Database transaction is no longer active");
    impl_->requireActive();
    return impl_->lease.invoke(sql, params, false).execution;
}
std::vector<Database::Row> Database::Transaction::query(const std::string& sql, Params params) {
    if (!impl_) throw DbError(0, "25000", "Database transaction is no longer active");
    impl_->requireActive();
    return impl_->lease.invoke(sql, params, true).rows;
}
Database::Row Database::Transaction::queryOne(const std::string& sql, Params params) {
    auto rows = query(sql, params);
    return rows.empty() ? Row{} : std::move(rows.front());
}
void Database::Transaction::commit() {
    if (!impl_) throw DbError(0, "25000", "Database transaction is no longer active");
    impl_->requireActive();
    try {
        impl_->lease.invoke("COMMIT", {}, false);
    } catch (...) {
        // 传输失败时无法确定提交结果，应淘汰连接并报告错误，
        // 绝不能重试 COMMIT 或之前的写入操作。
        discard();
        throw;
    }
    impl_->active = false;
    impl_.reset(); // 立即归还租约，不等到 Transaction 析构。
}
void Database::Transaction::rollback() {
    if (!impl_) throw DbError(0, "25000", "Database transaction is no longer active");
    // 关闭失效连接会让服务端回滚该连接尚未提交的事务。
    try {
        if (impl_->lease.healthy) impl_->lease.invoke("ROLLBACK", {}, false);
    } catch (...) {
        discard();
        throw;
    }
    impl_->active = false;
    impl_.reset();
}
void Database::Transaction::discard() noexcept {
    if (!impl_) return;
    impl_->active = false;
    impl_->lease.healthy = false;
    impl_.reset();
}
