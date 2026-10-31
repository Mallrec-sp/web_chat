#include "session_mgr.h"
#include "db/database.h"
#include "utils/auth.h"
#include "utils/logger.h"
#include "protocol/websocket.h"
#include "core/tcp_connection.h"
#include "nlohmann/json.hpp"

#include <chrono>

namespace {
int64_t nowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

SessionManager::SessionManager(Database& db) : db_(db) {
    loadUsernameCache();
    size_t count;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        count = usernameCache_.size();
    }
    LOG_INFO("SessionManager initialized with database, " << count << " users loaded");
}

void SessionManager::loadUsernameCache() {
    auto rows = db_.query("SELECT id, username FROM users");
    std::map<int64_t, std::string> cache;
    for (const auto& row : rows) cache.emplace(row.getInt(0), row.get(1));
    std::lock_guard<std::mutex> lock(mutex_);
    usernameCache_ = std::move(cache);
}

// ==================== 用户管理 ====================

SessionManager::RegisterResult SessionManager::registerUser(
    const std::string& username, const std::string& password) {
    RegisterResult result;
    if (username.empty() || password.empty()) {
        result.error = "Username and password required";
        return result;
    }
    if (username.size() < 2 || username.size() > 32) {
        result.error = "Username must be 2-32 characters";
        return result;
    }
    if (password.size() < 4) {
        result.error = "Password must be at least 4 characters";
        return result;
    }

    const int64_t userId = auth::generateUserId();
    const auto passwordHash = auth::hashPassword(password);
    const auto token = auth::generateToken();
    const int64_t now = nowMilliseconds();
    try {
        auto tx = db_.transaction();
        tx.exec("INSERT INTO users (id, username, password_hash, created_at) VALUES (?, ?, ?, ?)",
                {{1, userId}, {2, username}, {3, passwordHash}, {4, now}});
        tx.exec("INSERT INTO tokens (token, user_id, created_at) VALUES (?, ?, ?)",
                {{1, token}, {2, userId}, {3, now}});
        tx.commit();
    } catch (const DbError& error) {
        // 唯一约束是并发注册的最终裁判。仅将真实用户名冲突翻译成业务错误；
        // 极罕见的随机 ID/token 冲突仍然作为数据库故障向上传播。
        if (error.code() == 1062 && !db_.queryOne(
                "SELECT id FROM users WHERE username = ?", {{1, username}}).empty()) {
            result.error = "Username already exists";
            return result;
        }
        throw;
    }
    // 事务提交之前，绝不让其他线程看到尚未持久化的账号。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        usernameCache_[userId] = username;
    }
    LOG_INFO("User registered: " << username << " (id=" << userId << ")");
    result.userId = userId;
    result.token = token;
    return result;
}

SessionManager::LoginResult SessionManager::loginUser(
    const std::string& username, const std::string& password) {
    LoginResult result;
    auto row = db_.queryOne("SELECT id, password_hash FROM users WHERE username = ?",
                            {{1, username}});
    if (row.empty() || !auth::verifyPassword(password, row.get(1))) {
        result.error = "Invalid username or password";
        return result;
    }
    const int64_t userId = row.getInt(0);
    const auto token = auth::generateToken();
    db_.exec("INSERT INTO tokens (token, user_id, created_at) VALUES (?, ?, ?)",
             {{1, token}, {2, userId}, {3, nowMilliseconds()}});
    // exec 失败抛异常，不会构造一个实际上不能使用的登录结果。
    LOG_INFO("User logged in: " << username << " (id=" << userId << ")");
    result.userId = userId;
    result.username = username;
    result.token = token;
    return result;
}

int64_t SessionManager::verifyToken(const std::string& token) {
    // ascii_bin 是 PAD SPACE；二进制参数比较还须区分 token 的尾空格。
    auto row = db_.queryOne("SELECT user_id FROM tokens WHERE token = CAST(? AS BINARY)",
                            {{1, token}});
    return row.empty() ? -1 : row.getInt(0);
}

void SessionManager::invalidateToken(const std::string& token) {
    db_.exec("DELETE FROM tokens WHERE token = CAST(? AS BINARY)", {{1, token}});
    LOG_DEBUG("Token invalidated");
}

std::string SessionManager::getUsername(int64_t userId) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = usernameCache_.find(userId);
        if (it != usernameCache_.end()) return it->second;
    }
    // 缓存 miss 的数据库 I/O 不占用会话互斥锁。
    auto row = db_.queryOne("SELECT username FROM users WHERE id = ?", {{1, userId}});
    if (row.empty()) return "";
    auto username = row.get(0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        usernameCache_[userId] = username;
    }
    return username;
}

std::string SessionManager::getUsernameByToken(const std::string& token) {
    auto row = db_.queryOne(
        "SELECT u.username FROM tokens t JOIN users u ON t.user_id = u.id "
        "WHERE t.token = CAST(? AS BINARY)", {{1, token}});
    return row.empty() ? "" : row.get(0);
}

// ==================== 用户列表 ====================

std::vector<SessionManager::UserInfo> SessionManager::getAllUsers() {
    const auto online = getOnlineUsers();
    const std::set<int64_t> onlineIds(online.begin(), online.end());
    auto rows = db_.query("SELECT id, username FROM users ORDER BY username");
    std::vector<UserInfo> result;
    for (const auto& row : rows) {
        const int64_t uid = row.getInt(0);
        result.push_back({uid, row.get(1), onlineIds.count(uid) != 0});
    }
    return result;
}

// ==================== 好友系统 ====================

std::string SessionManager::sendFriendRequest(int64_t fromUserId, int64_t toUserId) {
    if (fromUserId == toUserId) return "Cannot add yourself";
    if (areFriends(fromUserId, toUserId)) return "Already friends";

    auto existing = db_.queryOne(
        "SELECT status FROM friendships WHERE user_id=? AND friend_id=?",
        {{1, fromUserId}, {2, toUserId}});
    if (!existing.empty()) {
        if (existing.getInt(0) == 1) return "Already friends";
        if (existing.getInt(0) == 0) {
            auto reverse = db_.queryOne(
                "SELECT status FROM friendships WHERE user_id=? AND friend_id=?",
                {{1, toUserId}, {2, fromUserId}});
            if (!reverse.empty() && reverse.getInt(0) == 0 &&
                    acceptFriendRequest(toUserId, fromUserId)) {
                return "accepted";
            }
            return "Request already sent";
        }
    }
    try {
        db_.exec("INSERT INTO friendships (user_id, friend_id, status, created_at) VALUES (?,?,0,?)",
                 {{1, fromUserId}, {2, toUserId}, {3, nowMilliseconds()}});
    } catch (const DbError& error) {
        if (error.code() != 1062) throw;
        // 与另一个请求竞争插入时，返回实际状态，不忽略其他 SQL 故障。
        auto current = db_.queryOne(
            "SELECT status FROM friendships WHERE user_id=? AND friend_id=?",
            {{1, fromUserId}, {2, toUserId}});
        if (current.empty()) throw;
        return current.getInt(0) == 1 ? "Already friends" : "Request already sent";
    }
    LOG_INFO("Friend request: " << fromUserId << " -> " << toUserId);
    return "";
}

bool SessionManager::acceptFriendRequest(int64_t userId, int64_t requesterId) {
    auto tx = db_.transaction();
    auto row = tx.queryOne(
        "SELECT status FROM friendships WHERE user_id=? AND friend_id=? FOR UPDATE",
        {{1, requesterId}, {2, userId}});
    if (row.empty() || row.getInt(0) != 0) {
        tx.rollback();
        return false;
    }
    const auto updated = tx.exec(
        "UPDATE friendships SET status=1 WHERE user_id=? AND friend_id=? AND status=0",
        {{1, requesterId}, {2, userId}});
    if (updated.affectedRows != 1) {
        tx.rollback();
        return false;
    }
    // 反向关系可能已经是 pending，必须升级，不能 INSERT IGNORE 后留下半边好友。
    tx.exec("INSERT INTO friendships (user_id, friend_id, status, created_at) VALUES (?,?,1,?) "
            "ON DUPLICATE KEY UPDATE status=1",
            {{1, userId}, {2, requesterId}, {3, nowMilliseconds()}});
    tx.commit();
    LOG_INFO("Friend accepted: " << requesterId << " <-> " << userId);
    return true;
}

bool SessionManager::rejectFriendRequest(int64_t userId, int64_t requesterId) {
    const auto result = db_.exec(
        "DELETE FROM friendships WHERE user_id=? AND friend_id=? AND status=0",
        {{1, requesterId}, {2, userId}});
    if (result.affectedRows == 0) return false;
    LOG_INFO("Friend request rejected: " << requesterId << " -> " << userId);
    return true;
}

bool SessionManager::removeFriend(int64_t userId, int64_t friendId) {
    auto tx = db_.transaction();
    const auto result = tx.exec(
        "DELETE FROM friendships WHERE (user_id=? AND friend_id=?) OR (user_id=? AND friend_id=?)",
        {{1, userId}, {2, friendId}, {3, friendId}, {4, userId}});
    tx.commit();
    if (result.affectedRows == 0) return false;
    LOG_INFO("Friend removed: " << userId << " <-> " << friendId);
    return true;
}

std::vector<SessionManager::UserInfo> SessionManager::getFriends(int64_t userId) {
    const auto online = getOnlineUsers();
    const std::set<int64_t> onlineIds(online.begin(), online.end());
    auto rows = db_.query(
        "SELECT u.id, u.username FROM users u "
        "JOIN friendships f ON (u.id = f.friend_id) "
        "WHERE f.user_id=? AND f.status=1 UNION "
        "SELECT u.id, u.username FROM users u "
        "JOIN friendships f ON (u.id = f.user_id) "
        "WHERE f.friend_id=? AND f.status=1 ORDER BY username",
        {{1, userId}, {2, userId}});
    std::vector<UserInfo> result;
    for (const auto& row : rows) {
        const int64_t uid = row.getInt(0);
        result.push_back({uid, row.get(1), onlineIds.count(uid) != 0});
    }
    return result;
}

std::vector<SessionManager::UserInfo> SessionManager::getPendingRequests(int64_t userId) {
    auto rows = db_.query(
        "SELECT u.id, u.username FROM users u JOIN friendships f ON (u.id = f.user_id) "
        "WHERE f.friend_id=? AND f.status=0 ORDER BY f.created_at DESC", {{1, userId}});
    std::vector<UserInfo> result;
    for (const auto& row : rows) result.push_back({row.getInt(0), row.get(1), false});
    return result;
}

bool SessionManager::areFriends(int64_t userId1, int64_t userId2) {
    return !db_.queryOne(
        "SELECT status FROM friendships WHERE user_id=? AND friend_id=? AND status=1",
        {{1, userId1}, {2, userId2}}).empty();
}

// ==================== WebSocket 会话管理 ====================

void SessionManager::userOnline(int64_t userId, const std::shared_ptr<TcpConnection>& conn) {
    // 查询可能失败，必须先完成再修改在线状态。
    std::string username = getUsername(userId);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        onlineSessions_[userId] = conn;
    }
    LOG_INFO("User online: " << username << " (id=" << userId << ")");
    nlohmann::json msg;
    msg["type"] = "user_online";
    msg["userId"] = std::to_string(userId);
    msg["username"] = username;
    broadcast(msg.dump());
    broadcastOnlineList();
}

void SessionManager::userOffline(int64_t userId, const TcpConnection* connection) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = onlineSessions_.find(userId);
        if (it == onlineSessions_.end()) return;
        const auto current = it->second.lock();
        // 旧连接关闭不能删除同一账号的新连接；重复关闭通知也应幂等。
        if (!current || current.get() != connection) return;
        onlineSessions_.erase(it);
    }
    // 不在 mutex_ 下调用 getUsername（它自己加锁，且可能访问 MySQL）。
    const auto username = getUsername(userId);
    LOG_INFO("User offline: " << username << " (id=" << userId << ")");
    nlohmann::json msg;
    msg["type"] = "user_offline";
    msg["userId"] = std::to_string(userId);
    broadcast(msg.dump());
    broadcastOnlineList();
}

std::vector<int64_t> SessionManager::getOnlineUsers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int64_t> result;
    for (const auto& [uid, wptr] : onlineSessions_) {
        if (!wptr.expired()) result.push_back(uid);
    }
    return result;
}

std::vector<std::string> SessionManager::getOnlineUsernames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (const auto& [uid, wptr] : onlineSessions_) {
        if (!wptr.expired()) {
            auto it = usernameCache_.find(uid);
            if (it != usernameCache_.end()) result.push_back(it->second);
        }
    }
    return result;
}

std::shared_ptr<TcpConnection> SessionManager::getConnection(int64_t userId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = onlineSessions_.find(userId);
    return it == onlineSessions_.end() ? nullptr : it->second.lock();
}

bool SessionManager::sendToUser(int64_t toUserId, const std::string& message) {
    auto conn = getConnection(toUserId);
    if (!conn) return false;
    conn->send(websocket::encodeFrame(ws::kText, message));
    return true;
}

void SessionManager::broadcast(const std::string& message) {
    std::vector<std::shared_ptr<TcpConnection>> connections;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [uid, wptr] : onlineSessions_) {
            if (auto conn = wptr.lock()) connections.push_back(std::move(conn));
        }
    }
    const auto frame = websocket::encodeFrame(ws::kText, message);
    for (const auto& conn : connections) conn->send(frame);
}

void SessionManager::broadcastOnlineList() {
    std::vector<std::pair<int64_t, std::shared_ptr<TcpConnection>>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [uid, wptr] : onlineSessions_) {
            if (auto conn = wptr.lock()) sessions.emplace_back(uid, std::move(conn));
        }
    }
    nlohmann::json list;
    list["type"] = "online_list";
    list["users"] = nlohmann::json::array();
    for (const auto& [uid, conn] : sessions) {
        list["users"].push_back({{"userId", std::to_string(uid)}, {"username", getUsername(uid)}});
    }
    const auto frame = websocket::encodeFrame(ws::kText, list.dump());
    for (const auto& [uid, conn] : sessions) conn->send(frame);
}

// ==================== 消息存储 ====================

void SessionManager::saveMessage(int64_t from, int64_t to, const std::string& content) {
    db_.exec("INSERT INTO messages (from_user_id, to_user_id, content, created_at) VALUES (?, ?, ?, ?)",
             {{1, from}, {2, to}, {3, content}, {4, nowMilliseconds()}});
}

std::vector<SessionManager::StoredMessage> SessionManager::getHistory(
    int64_t userId1, int64_t userId2, int limit) {
    auto rows = db_.query(
        "SELECT from_user_id, to_user_id, content, created_at FROM messages "
        "WHERE (from_user_id = ? AND to_user_id = ?) OR (from_user_id = ? AND to_user_id = ?) "
        "ORDER BY created_at ASC LIMIT ?",
        {{1, userId1}, {2, userId2}, {3, userId2}, {4, userId1}, {5, int64_t{limit}}});
    std::vector<StoredMessage> result;
    for (const auto& row : rows) {
        result.push_back({row.getInt(0), row.getInt(1), row.get(2), row.getInt(3)});
    }
    return result;
}
