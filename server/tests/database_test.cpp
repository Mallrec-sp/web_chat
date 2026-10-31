#include "db/database.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace {
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::string env(const char* key, const std::string& fallback = {}) {
    const char* value = std::getenv(key);
    return value ? value : fallback;
}
unsigned envUnsigned(const char* key, unsigned fallback) {
    const auto value = env(key);
    if (value.empty()) return fallback;
    size_t used = 0;
    const auto parsed = std::stoul(value, &used);
    if (used != value.size() || parsed > std::numeric_limits<unsigned>::max())
        throw std::runtime_error("Invalid numeric test environment variable");
    return static_cast<unsigned>(parsed);
}
DatabaseOptions optionsFromEnvironment() {
    DatabaseOptions options;
    options.host = env("MYSQL_TEST_HOST", options.host);
    options.port = envUnsigned("MYSQL_TEST_PORT", options.port);
    options.database = env("MYSQL_TEST_DATABASE");
    options.user = env("MYSQL_TEST_USER", options.user);
    options.password = env("MYSQL_TEST_PASSWORD");
    options.sslMode = env("MYSQL_TEST_SSL_MODE", options.sslMode);
    options.sslCa = env("MYSQL_TEST_SSL_CA");
    options.poolSize = 2;
    return options;
}
std::string randomTable() {
    std::random_device random;
    return "test_database_" + std::to_string(random()) + "_" + std::to_string(random());
}
struct TestTable {
    Database& database;
    std::string name = randomTable();
    explicit TestTable(Database& db) : database(db) {
        database.execute("CREATE TABLE `" + name + "` ("
            "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY, marker BIGINT NOT NULL UNIQUE, "
            "n TEXT NULL, txt LONGTEXT NOT NULL, bytes LONGBLOB NOT NULL, "
            "big BIGINT NOT NULL, real_value DOUBLE NOT NULL, "
            "secret VARCHAR(128) NOT NULL UNIQUE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    }
    ~TestTable() {
        // 只删除当前进程刚刚创建的随机 test_ 测试表。
        try { database.execute("DROP TABLE `" + name + "`"); }
        catch (...) { std::cerr << "Warning: generated test table cleanup failed\n"; }
    }
};

template<class Function>
DbError expectDbError(Function&& action) {
    try { action(); }
    catch (const DbError& error) { return error; }
    throw std::runtime_error("Expected DbError was not thrown");
}

void valueAndTransactionTests(Database& db, const std::string& table) {
    const std::string insert = "INSERT INTO `" + table +
        "` (marker,n,txt,bytes,big,real_value,secret) VALUES (?,?,?,?,?,?,?)";
    const std::string unicode = "中文消息 / emoji 😀🌍";
    const std::string longText = unicode + std::string(256 * 1024, 'x') + unicode;
    const std::string binary("prefix\0middle\0suffix", 20);
    const std::string secret = "test-private-password-token-do-not-echo";
    const auto minimum = std::numeric_limits<int64_t>::min();
    const auto maximum = std::numeric_limits<int64_t>::max();
    const auto inserted = db.exec(insert, {
        {7, secret}, {3, longText}, {2, nullptr}, {1, int64_t{1}},
        {4, binary}, {6, 3.125}, {5, minimum}});
    require(inserted.affectedRows == 1 && inserted.insertId != 0, "Insert result mismatch");
    const auto row = db.queryOne("SELECT n,txt,bytes,big,real_value,id FROM `" + table +
        "` WHERE marker=?", {{1, int64_t{1}}});
    require(!row.empty() && row.isNull(0) && row.get(0).empty(), "NULL round trip failed");
    require(row.get(1) == longText, "Long Unicode text round trip failed");
    require(row.get(2) == binary, "Embedded NUL round trip failed");
    require(row.getInt(3) == minimum, "BIGINT minimum round trip failed");
    require(std::abs(std::stod(row.get(4)) - 3.125) < 1e-12, "DOUBLE round trip failed");
    require(static_cast<uint64_t>(row.getInt(5)) == inserted.insertId, "Insert ID mismatch");
    require(row.isNull(-1) && row.get(-1).empty() && row.getInt(-1) == 0,
            "Negative column index is not safe");
    require(row.isNull(100) && row.get(100).empty(), "Missing column index is not safe");
    require(db.queryOne("SELECT id FROM `" + table + "` WHERE marker=999").empty(),
            "Empty query result is not empty");

    const auto updated = db.exec("UPDATE `" + table + "` SET big=?,txt=? WHERE marker=?",
                                 {{1, maximum}, {2, std::string{}}, {3, int64_t{1}}});
    require(updated.affectedRows == 1, "UPDATE affected rows mismatch");
    const auto changed = db.queryOne("SELECT big,txt FROM `" + table + "` WHERE marker=1");
    require(changed.getInt(0) == maximum, "BIGINT maximum round trip failed");
    require(!changed.isNull(1) && changed.get(1).empty(), "Empty string became NULL");

    const auto duplicate = expectDbError([&] {
        db.exec(insert, {{1, int64_t{2}}, {2, nullptr}, {3, unicode}, {4, binary},
                         {5, int64_t{0}}, {6, 0.0}, {7, secret}});
    });
    require(duplicate.code() == 1062 && duplicate.sqlState() == "23000",
            "Constraint error code/SQLSTATE mismatch");
    require(std::string(duplicate.what()).find(secret) == std::string::npos,
            "Error leaked bound secret");
    expectDbError([&] { db.query("SELECT ?,?", {{1, int64_t{1}}, {1, int64_t{2}}}); });
    expectDbError([&] { db.query("SELECT ?", {{0, int64_t{1}}}); });
    expectDbError([&] { db.query("SELECT ?"); });
    require(db.queryOne("SELECT 1").getInt(0) == 1, "Pool did not recover after errors");

    auto insertMarker = [&](Database::Transaction& transaction, int64_t marker) {
        return transaction.exec(insert, {{1, marker}, {2, nullptr}, {3, unicode}, {4, binary},
            {5, maximum}, {6, 1.5}, {7, std::string("marker-") + std::to_string(marker)}});
    };
    auto count = [&] {
        return db.queryOne("SELECT COUNT(*) FROM `" + table + "`").getInt(0);
    };
    {
        auto transaction = db.transaction();
        const auto connection = transaction.queryOne("SELECT CONNECTION_ID()").getInt(0);
        require(insertMarker(transaction, 3).affectedRows == 1, "Transaction insert failed");
        require(transaction.queryOne("SELECT CONNECTION_ID()").getInt(0) == connection,
                "Transaction changed physical connection");
        auto moved = std::move(transaction);
        expectDbError([&] { transaction.queryOne("SELECT 1"); });
        require(moved.queryOne("SELECT COUNT(*) FROM `" + table + "`").getInt(0) == 2,
                "Transaction cannot see its write");
        require(count() == 1, "Uncommitted write escaped transaction");
    }
    require(count() == 1, "Destructor did not roll back");
    {
        auto transaction = db.transaction();
        insertMarker(transaction, 4);
        transaction.rollback();
    }
    require(count() == 1, "Explicit rollback did not roll back");
    {
        auto transaction = db.transaction();
        insertMarker(transaction, 5);
        transaction.commit();
        expectDbError([&] { transaction.commit(); });
    }
    require(count() == 2, "Commit did not persist");
    {
        auto transaction = db.transaction();
        insertMarker(transaction, 6);
        transaction.discard();
        transaction.discard();
        expectDbError([&] { transaction.queryOne("SELECT 1"); });
    }
    require(count() == 2, "Discard did not roll back");
    {
        auto first = db.transaction();
        insertMarker(first, 7);
        auto second = db.transaction();
        second = std::move(first);
        second.rollback();
    }
    require(count() == 2, "Move assignment lost transaction cleanup");
    require(db.query("SELECT id FROM `" + table + "` ORDER BY id").size() == 2,
            "Multiple-row result mismatch");
}

void poolTests(const DatabaseOptions& base) {
    auto options = base;
    options.poolSize = 1;
    options.acquireTimeoutMs = 150;
    Database db(options);
    {
        auto held = db.transaction();
        const auto started = std::chrono::steady_clock::now();
        const auto error = expectDbError([&] { db.queryOne("SELECT 1"); });
        require(error.sqlState() == "HYT00", "Pool exhaustion did not report timeout");
        require(std::chrono::steady_clock::now() - started >= 100ms,
                "Pool timeout returned too early");
        held.commit();
        require(db.queryOne("SELECT 1").getInt(0) == 1, "Commit retained pool lease");
    }
    const auto lockName = randomTable();
    {
        auto held = db.transaction();
        require(held.queryOne("SELECT GET_LOCK(?,1)", {{1, lockName}}).getInt(0) == 1,
                "Cannot acquire named test lock");
        held.discard();
    }
    {
        auto replacement = db.transaction();
        require(replacement.queryOne("SELECT GET_LOCK(?,5)", {{1, lockName}}).getInt(0) == 1,
                "Discard did not release named lock");
        require(replacement.queryOne("SELECT RELEASE_LOCK(?)", {{1, lockName}}).getInt(0) == 1,
                "Cannot release named test lock");
        replacement.commit();
    }

    options.acquireTimeoutMs = 5000;
    Database shutdownDb(options);
    auto held = shutdownDb.transaction();
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    auto waiter = std::async(std::launch::async, [&] {
        entered.set_value();
        return expectDbError([&] { shutdownDb.queryOne("SELECT 1"); }).sqlState();
    });
    enteredFuture.wait();
    require(waiter.wait_for(50ms) == std::future_status::timeout,
            "Waiter unexpectedly acquired an occupied pool connection");
    shutdownDb.shutdown();
    require(waiter.wait_for(2s) == std::future_status::ready, "Shutdown failed to wake waiter");
    require(waiter.get() == "08003", "Shutdown waiter error mismatch");
    require(held.queryOne("SELECT 42").getInt(0) == 42, "Shutdown killed active transaction");
    held.commit();
    require(expectDbError([&] { shutdownDb.transaction(); }).sqlState() == "08003",
            "Shutdown accepted a new lease");
    shutdownDb.shutdown();

    // 仍有活跃事务时，共享连接池状态必须比 Database 对象存活得更久。
    auto detached = [&] {
        Database temporary(options);
        return temporary.transaction();
    }();
    require(detached.queryOne("SELECT 7").getInt(0) == 7, "Database destruction killed lease");
    detached.rollback();
    require(db.queryOne("SELECT 1").getInt(0) == 1,
            "Destroying another Database deinitialized the client library");
}

void concurrencyTests(const DatabaseOptions& options) {
    Database db(options);
    std::vector<std::future<void>> workers;
    for (int64_t worker = 0; worker < 8; ++worker) {
        workers.push_back(std::async(std::launch::async, [&, worker] {
            for (int64_t iteration = 0; iteration < 20; ++iteration) {
                const int64_t value = worker * 100 + iteration;
                const auto text = std::to_string(value);
                const auto row = db.queryOne("SELECT ?,?,SLEEP(0.001)",
                                             {{1, value}, {2, text}});
                require(row.getInt(0) == value && row.get(1) == text,
                        "Concurrent queries crossed connection/result ownership");
            }
        }));
    }
    for (auto& worker : workers) worker.get();
    auto transaction = db.transaction();
    const auto connectionId = transaction.queryOne("SELECT CONNECTION_ID()").getInt(0);
    auto moved = std::async(std::launch::async,
        [transaction = std::move(transaction), connectionId]() mutable {
            require(transaction.queryOne("SELECT CONNECTION_ID()").getInt(0) == connectionId,
                    "Moving a transaction between threads changed its connection");
            transaction.commit();
        });
    moved.get();
}

void timeoutRecoveryTest(const DatabaseOptions& base) {
    auto options = base;
    options.poolSize = 1;
    options.readTimeoutSec = 1;
    Database db(options);
    expectDbError([&] { db.queryOne("SELECT SLEEP(4)"); });
    require(db.queryOne("SELECT 9").getInt(0) == 9,
            "Pool did not replace connection after read timeout");
}
} // 匿名命名空间

int main() {
    static_assert(!std::is_copy_constructible_v<Database>);
    static_assert(!std::is_copy_constructible_v<Database::Transaction>);
    static_assert(std::is_nothrow_destructible_v<Database::Transaction>);
    static_assert(std::is_nothrow_move_constructible_v<Database::Transaction>);
    if (env("MYSQL_TEST_DATABASE").empty()) {
        std::cout << "SKIP: set MYSQL_TEST_DATABASE to an isolated test database\n";
        return 77;
    }
    try {
        const auto options = optionsFromEnvironment();
        Database db(options);
        TestTable table(db);
        valueAndTransactionTests(db, table.name);
        poolTests(options);
        concurrencyTests(options);
        timeoutRecoveryTest(options);
        std::cout << "Database integration tests passed\n";
        return 0;
    } catch (const DbError& error) {
        std::cerr << "Database integration test failed: " << error.what()
                  << "; SQLSTATE=" << error.sqlState() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Database integration test failed: " << error.what() << '\n';
    }
    return 1;
}
