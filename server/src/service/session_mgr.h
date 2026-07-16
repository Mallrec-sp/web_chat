#ifndef NETCHAT_SESSION_MGR_H
#define NETCHAT_SESSION_MGR_H

#include <memory>
#include <string>
#include <map>
#include <mutex>
#include <functional>
#include <set>
#include <vector>

class TcpConnection;
class Database;

/**
 * SessionManager — WebSocket 会话管理和聊天业务逻辑。
 *
 * 职责：
 *   1. 用户注册和登录（SQLite3 持久化）
 *   2. 会话 token 验证
 *   3. WebSocket 连接管理（上线/下线/广播）
 *   4. 消息路由（点对点发送）
 *   5. 在线用户列表
 *
 * 数据持久化：用户数据、Token、消息全部存储在 SQLite3 中。
 * 运行时状态（WebSocket 连接）保持在内存中。
 */
class SessionManager {
public:
    explicit SessionManager(Database& db);
    ~SessionManager() = default;

    // ---- 用户管理 ----

    struct RegisterResult {
        int64_t userId = 0;
        std::string token;
        std::string error;
        bool success() const { return userId > 0 && !token.empty(); }
    };
    RegisterResult registerUser(const std::string& username, const std::string& password);

    struct LoginResult {
        int64_t userId = 0;
        std::string username;
        std::string token;
        std::string error;
        bool success() const { return userId > 0 && !token.empty(); }
    };
    LoginResult loginUser(const std::string& username, const std::string& password);

    int64_t verifyToken(const std::string& token);
    void invalidateToken(const std::string& token);
    std::string getUsername(int64_t userId);
    std::string getUsernameByToken(const std::string& token);

    // ---- WebSocket 会话管理 ----

    void userOnline(int64_t userId, const std::shared_ptr<TcpConnection>& conn);
    void userOffline(int64_t userId);
    std::vector<int64_t> getOnlineUsers() const;
    std::vector<std::string> getOnlineUsernames() const;
    std::shared_ptr<TcpConnection> getConnection(int64_t userId) const;
    bool sendToUser(int64_t toUserId, const std::string& message);
    void broadcast(const std::string& message);
    void broadcastOnlineList();

    // ---- 消息存储 ----

    struct StoredMessage {
        int64_t fromUserId;
        int64_t toUserId;
        std::string content;
        int64_t timestamp;
    };
    void saveMessage(int64_t from, int64_t to, const std::string& content);
    std::vector<StoredMessage> getHistory(int64_t userId1, int64_t userId2, int limit = 50);

    /// 获取所有用户列表
    struct UserInfo {
        int64_t userId;
        std::string username;
        bool online;
    };
    std::vector<UserInfo> getAllUsers();

    // ---- 好友系统 ----

    /// 发送好友请求
    std::string sendFriendRequest(int64_t fromUserId, int64_t toUserId);

    /// 接受好友请求
    bool acceptFriendRequest(int64_t userId, int64_t requesterId);

    /// 拒绝好友请求
    bool rejectFriendRequest(int64_t userId, int64_t requesterId);

    /// 删除好友
    bool removeFriend(int64_t userId, int64_t friendId);

    /// 获取好友列表（含在线状态）
    std::vector<UserInfo> getFriends(int64_t userId);

    /// 获取待处理的好友请求（别人发给我的）
    std::vector<UserInfo> getPendingRequests(int64_t userId);

    /// 检查是否为好友
    bool areFriends(int64_t userId1, int64_t userId2);

private:
    Database& db_;
    mutable std::mutex mutex_;

    // 用户名缓存（启动时从数据库加载）
    std::map<int64_t, std::string> usernameCache_;

    // WebSocket 在线会话（运行态，不持久化）
    std::map<int64_t, std::weak_ptr<TcpConnection>> onlineSessions_;

    // 加载用户名缓存
    void loadUsernameCache();
};

#endif // NETCHAT_SESSION_MGR_H
