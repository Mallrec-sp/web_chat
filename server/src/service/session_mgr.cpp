#include "session_mgr.h"
#include "db/database.h"
#include "utils/auth.h"
#include "utils/logger.h"
#include "protocol/websocket.h"
#include "core/tcp_connection.h"
#include "nlohmann/json.hpp"

#include <chrono>
#include <sstream>
#include <algorithm>

SessionManager::SessionManager(Database& db)
    : db_(db)
{
    // 启动时从数据库加载用户名缓存
    loadUsernameCache();
    LOG_INFO("SessionManager initialized with database, "
             << usernameCache_.size() << " users loaded");
}

void SessionManager::loadUsernameCache() {
    auto rows = db_.query("SELECT id, username FROM users");
    for (const auto& row : rows) {
        int64_t uid = row.getInt(0);
        usernameCache_[uid] = row.get(1);
    }
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

    // 检查用户名是否已存在
    auto existing = db_.queryOne(
        "SELECT id FROM users WHERE username = ?", {{1, username}});
    if (!existing.empty()) {
        result.error = "Username already exists";
        return result;
    }

    // 生成用户 ID（时间戳 + 随机数）
    int64_t userId = auth::generateUserId();

    // 密码哈希
    std::string passwordHash = auth::hashPassword(password);

    // 时间戳
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // INSERT 用户
    bool ok = db_.exec(
        "INSERT INTO users (id, username, password_hash, created_at) VALUES (?, ?, ?, ?)",
        {
            {1, std::to_string(userId)},
            {2, username},
            {3, passwordHash},
            {4, std::to_string(now)}
        }
    );
    if (!ok) {
        result.error = "Database error";
        return result;
    }

    // 生成 token
    std::string token = auth::generateToken();
    db_.exec(
        "INSERT INTO tokens (token, user_id, created_at) VALUES (?, ?, ?)",
        {
            {1, token},
            {2, std::to_string(userId)},
            {3, std::to_string(now)}
        }
    );

    // 更新缓存
    usernameCache_[userId] = username;

    LOG_INFO("User registered: " << username << " (id=" << userId << ")");
    result.userId = userId;
    result.token = token;
    return result;
}

SessionManager::LoginResult SessionManager::loginUser(
    const std::string& username, const std::string& password) {

    LoginResult result;

    auto row = db_.queryOne(
        "SELECT id, password_hash FROM users WHERE username = ?",
        {{1, username}}
    );

    if (row.empty()) {
        result.error = "Invalid username or password";
        return result;
    }

    int64_t userId = row.getInt(0);
    std::string passwordHash = row.get(1);

    if (!auth::verifyPassword(password, passwordHash)) {
        result.error = "Invalid username or password";
        return result;
    }

    // 生成 token
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string token = auth::generateToken();
    db_.exec(
        "INSERT INTO tokens (token, user_id, created_at) VALUES (?, ?, ?)",
        {
            {1, token},
            {2, std::to_string(userId)},
            {3, std::to_string(now)}
        }
    );

    LOG_INFO("User logged in: " << username << " (id=" << userId << ")");
    result.userId = userId;
    result.username = username;
    result.token = token;
    return result;
}

int64_t SessionManager::verifyToken(const std::string& token) {
    auto row = db_.queryOne(
        "SELECT user_id FROM tokens WHERE token = ?",
        {{1, token}}
    );
    return row.empty() ? -1 : row.getInt(0);
}

void SessionManager::invalidateToken(const std::string& token) {
    db_.exec("DELETE FROM tokens WHERE token = ?", {{1, token}});
    LOG_DEBUG("Token invalidated");
}

std::string SessionManager::getUsername(int64_t userId) {
    // 从缓存查
    auto it = usernameCache_.find(userId);
    if (it != usernameCache_.end()) return it->second;

    // 缓存未命中，查数据库
    auto row = db_.queryOne(
        "SELECT username FROM users WHERE id = ?",
        {{1, std::to_string(userId)}}
    );
    if (!row.empty()) {
        usernameCache_[userId] = row.get(0);
        return row.get(0);
    }
    return "";
}

std::string SessionManager::getUsernameByToken(const std::string& token) {
    auto row = db_.queryOne(
        "SELECT u.username FROM tokens t JOIN users u ON t.user_id = u.id WHERE t.token = ?",
        {{1, token}}
    );
    return row.empty() ? "" : row.get(0);
}

// ==================== 用户列表 ====================

std::vector<SessionManager::UserInfo> SessionManager::getAllUsers() {
    auto rows = db_.query("SELECT id, username FROM users ORDER BY username");
    std::vector<UserInfo> result;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& row : rows) {
        int64_t uid = row.getInt(0);
        UserInfo info;
        info.userId = uid;
        info.username = row.get(1);
        auto it = onlineSessions_.find(uid);
        info.online = (it != onlineSessions_.end() && !it->second.expired());
        result.push_back(info);
    }
    return result;
}

// ==================== 好友系统 ====================

std::string SessionManager::sendFriendRequest(int64_t fromUserId, int64_t toUserId) {
    if (fromUserId == toUserId) return "Cannot add yourself";
    if (areFriends(fromUserId, toUserId)) return "Already friends";

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // 检查是否已存在请求
    auto existing = db_.queryOne(
        "SELECT status FROM friendships WHERE user_id=? AND friend_id=?",
        {{1, std::to_string(fromUserId)}, {2, std::to_string(toUserId)}}
    );
    if (!existing.empty()) {
        auto status = existing.get(0);
        if (status == "1") return "Already friends";
        if (status == "0") {
            // 如果是反向的请求（对方请求了我），自动接受
            auto reverse = db_.queryOne(
                "SELECT status FROM friendships WHERE user_id=? AND friend_id=?",
                {{1, std::to_string(toUserId)}, {2, std::to_string(fromUserId)}}
            );
            if (!reverse.empty() && reverse.get(0) == "0") {
                acceptFriendRequest(toUserId, fromUserId);
                return "accepted";
            }
            return "Request already sent";
        }
    }

    db_.exec("INSERT INTO friendships (user_id, friend_id, status, created_at) VALUES (?,?,?,?)",
        {
            {1, std::to_string(fromUserId)},
            {2, std::to_string(toUserId)},
            {3, "0"},
            {4, std::to_string(now)}
        }
    );

    LOG_INFO("Friend request: " << fromUserId << " -> " << toUserId);
    return "";
}

bool SessionManager::acceptFriendRequest(int64_t userId, int64_t requesterId) {
    auto row = db_.queryOne(
        "SELECT status FROM friendships WHERE user_id=? AND friend_id=?",
        {{1, std::to_string(requesterId)}, {2, std::to_string(userId)}}
    );
    if (row.empty() || row.get(0) != "0") return false;

    // 更新状态为已接受
    db_.exec("UPDATE friendships SET status=1 WHERE user_id=? AND friend_id=?",
        {{1, std::to_string(requesterId)}, {2, std::to_string(userId)}}
    );

    // 添加反向关系
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    db_.exec("INSERT OR IGNORE INTO friendships (user_id, friend_id, status, created_at) VALUES (?,?,?,?)",
        {
            {1, std::to_string(userId)},
            {2, std::to_string(requesterId)},
            {3, "1"},
            {4, std::to_string(now)}
        }
    );

    LOG_INFO("Friend accepted: " << requesterId << " <-> " << userId);
    return true;
}

bool SessionManager::rejectFriendRequest(int64_t userId, int64_t requesterId) {
    db_.exec("DELETE FROM friendships WHERE user_id=? AND friend_id=?",
        {{1, std::to_string(requesterId)}, {2, std::to_string(userId)}}
    );
    LOG_INFO("Friend request rejected: " << requesterId << " -> " << userId);
    return true;
}

bool SessionManager::removeFriend(int64_t userId, int64_t friendId) {
    db_.exec("DELETE FROM friendships WHERE (user_id=? AND friend_id=?) OR (user_id=? AND friend_id=?)",
        {
            {1, std::to_string(userId)}, {2, std::to_string(friendId)},
            {3, std::to_string(friendId)}, {4, std::to_string(userId)}
        }
    );
    LOG_INFO("Friend removed: " << userId << " <-> " << friendId);
    return true;
}

std::vector<SessionManager::UserInfo> SessionManager::getFriends(int64_t userId) {
    auto rows = db_.query(
        "SELECT u.id, u.username FROM users u "
        "JOIN friendships f ON (u.id = f.friend_id) "
        "WHERE f.user_id=? AND f.status=1 "
        "UNION "
        "SELECT u.id, u.username FROM users u "
        "JOIN friendships f ON (u.id = f.user_id) "
        "WHERE f.friend_id=? AND f.status=1 "
        "ORDER BY username",
        {{1, std::to_string(userId)}, {2, std::to_string(userId)}}
    );

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<UserInfo> result;
    for (const auto& row : rows) {
        int64_t uid = row.getInt(0);
        UserInfo info;
        info.userId = uid;
        info.username = row.get(1);
        auto it = onlineSessions_.find(uid);
        info.online = (it != onlineSessions_.end() && !it->second.expired());
        result.push_back(info);
    }
    return result;
}

std::vector<SessionManager::UserInfo> SessionManager::getPendingRequests(int64_t userId) {
    auto rows = db_.query(
        "SELECT u.id, u.username FROM users u "
        "JOIN friendships f ON (u.id = f.user_id) "
        "WHERE f.friend_id=? AND f.status=0 "
        "ORDER BY f.created_at DESC",
        {{1, std::to_string(userId)}}
    );

    std::vector<UserInfo> result;
    for (const auto& row : rows) {
        UserInfo info;
        info.userId = row.getInt(0);
        info.username = row.get(1);
        info.online = false;
        result.push_back(info);
    }
    return result;
}

bool SessionManager::areFriends(int64_t userId1, int64_t userId2) {
    auto row = db_.queryOne(
        "SELECT status FROM friendships WHERE user_id=? AND friend_id=? AND status=1",
        {{1, std::to_string(userId1)}, {2, std::to_string(userId2)}}
    );
    return !row.empty();
}

// ==================== WebSocket 会话管理 ====================

void SessionManager::userOnline(int64_t userId,
                                 const std::shared_ptr<TcpConnection>& conn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        onlineSessions_[userId] = conn;
    }

    std::string username = getUsername(userId);
    LOG_INFO("User online: " << username << " (id=" << userId << ")");

    // 广播上线通知
    nlohmann::json msg;
    msg["type"] = "user_online";
    msg["userId"] = userId;
    msg["username"] = username;
    broadcast(msg.dump());
    broadcastOnlineList();
}

void SessionManager::userOffline(int64_t userId) {
    std::string username;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        onlineSessions_.erase(userId);
        username = getUsername(userId);
    }

    LOG_INFO("User offline: " << username << " (id=" << userId << ")");
    nlohmann::json msg;
    msg["type"] = "user_offline";
    msg["userId"] = userId;
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
            if (it != usernameCache_.end())
                result.push_back(it->second);
        }
    }
    return result;
}

std::shared_ptr<TcpConnection> SessionManager::getConnection(int64_t userId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = onlineSessions_.find(userId);
    if (it == onlineSessions_.end()) return nullptr;
    return it->second.lock();
}

bool SessionManager::sendToUser(int64_t toUserId, const std::string& message) {
    auto conn = getConnection(toUserId);
    if (!conn) return false;
    auto frame = websocket::encodeFrame(ws::kText, message);
    conn->send(frame);
    return true;
}

void SessionManager::broadcast(const std::string& message) {
    auto frame = websocket::encodeFrame(ws::kText, message);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [uid, wptr] : onlineSessions_) {
        auto conn = wptr.lock();
        if (conn) conn->send(frame);
    }
}

void SessionManager::broadcastOnlineList() {
    nlohmann::json list;
    list["type"] = "online_list";

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [uid, wptr] : onlineSessions_) {
        if (!wptr.expired()) {
            nlohmann::json user;
            user["userId"] = uid;
            user["username"] = getUsername(uid);
            list["users"].push_back(user);
        }
    }

    std::string msg = list.dump();
    auto frame = websocket::encodeFrame(ws::kText, msg);
    for (auto& [uid, wptr] : onlineSessions_) {
        auto conn = wptr.lock();
        if (conn) conn->send(frame);
    }
}

// ==================== 消息存储 ====================

void SessionManager::saveMessage(int64_t from, int64_t to, const std::string& content) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    db_.exec(
        "INSERT INTO messages (from_user_id, to_user_id, content, created_at) VALUES (?, ?, ?, ?)",
        {
            {1, std::to_string(from)},
            {2, std::to_string(to)},
            {3, content},
            {4, std::to_string(now)}
        }
    );
}

std::vector<SessionManager::StoredMessage> SessionManager::getHistory(
    int64_t userId1, int64_t userId2, int limit) {

    auto rows = db_.query(
        "SELECT from_user_id, to_user_id, content, created_at FROM messages "
        "WHERE (from_user_id = ? AND to_user_id = ?) OR (from_user_id = ? AND to_user_id = ?) "
        "ORDER BY created_at ASC LIMIT ?",
        {
            {1, std::to_string(userId1)},
            {2, std::to_string(userId2)},
            {3, std::to_string(userId2)},
            {4, std::to_string(userId1)},
            {5, std::to_string(limit)}
        }
    );

    std::vector<StoredMessage> result;
    for (const auto& row : rows) {
        StoredMessage msg;
        msg.fromUserId = row.getInt(0);
        msg.toUserId = row.getInt(1);
        msg.content = row.get(2);
        msg.timestamp = row.getInt(3);
        result.push_back(msg);
    }
    return result;
}
