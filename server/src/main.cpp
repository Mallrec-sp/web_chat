/**
 * NetChat Server — 企业级入口。
 *
 * 功能：
 *   1. HTTP API: 注册、登录、静态文件
 *   2. WebSocket: 实时聊天
 *   3. 多 Reactor + 优雅关闭
 *
 * 全部自研，不依赖 Nginx 等外部组件。
 * 静态文件和 API 都通过同一个端口提供。
 */

#include "utils/logger.h"
#include "utils/config.h"
#include "utils/auth.h"
#include "core/eventloop.h"
#include "core/server.h"
#include "core/inetaddress.h"
#include "core/signal_handler.h"
#include "core/ssl_context.h"
#include "protocol/http_parser.h"
#include "protocol/http_response.h"
#include "protocol/router.h"
#include "protocol/websocket.h"
#include "service/session_mgr.h"
#include "db/database.h"
#include "db/migrations.h"
#include "nlohmann/json.hpp"

#include <csignal>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <iomanip>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

// ============================================================
// 文件管理器
// ============================================================

static std::string createUploadsDir() {
    // 创建 uploads/ 目录（如果不存在）
    struct stat st;
    std::string dir = "../uploads";
    if (stat(dir.c_str(), &st) != 0) {
        mkdir(dir.c_str(), 0755);
        LOG_INFO("Created uploads directory: " << dir);
    }
    return dir;
}

static std::string generateFileId() {
    unsigned char buf[16];
    RAND_bytes(buf, sizeof(buf));
    std::ostringstream oss;
    for (int i = 0; i < 16; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(buf[i]);
    }
    return oss.str();
}

static std::string getMimeFromExtension(const std::string& filename) {
    auto dot = filename.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = filename.substr(dot + 1);
    for (auto& c : ext) c = std::tolower(c);
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png") return "image/png";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "pdf") return "application/pdf";
    if (ext == "doc" || ext == "docx") return "application/msword";
    if (ext == "zip") return "application/zip";
    if (ext == "mp4") return "video/mp4";
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "txt") return "text/plain";
    return "application/octet-stream";
}

/// Global file metadata map
static struct FileStore {
    std::mutex mutex;
    std::map<std::string, std::string> files; // fileId -> filePath
    std::map<std::string, std::string> names; // fileId -> originalName
    std::map<std::string, int64_t> sizes;    // fileId -> fileSize
    std::map<std::string, int64_t> owners;   // fileId -> userId
} g_fileStore;

using json = nlohmann::json;

// ============================================================
// 静态文件处理
// ============================================================

/** 获取静态文件 MIME 类型 */
static std::string getMimeType(const std::string& path) {
    auto ext = path.substr(path.find_last_of('.') + 1);
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "js")   return "application/javascript; charset=utf-8";
    if (ext == "css")  return "text/css; charset=utf-8";
    if (ext == "json") return "application/json";
    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "ico")  return "image/x-icon";
    if (ext == "woff2") return "font/woff2";
    return "text/plain";
}

/**
 * serveStaticFile — 从 dist/ 目录读取静态文件。
 *
 * 如果是 SPA 路由（非真实文件），返回 index.html。
 * 讲解点：SPA 路由处理
 *   前端使用 React Router 的 BrowserRouter，URL 如 /chat 在服务端
 *   没有对应的文件。所以所有不存在的路径都返回 index.html，
 *   让前端 JS 自己处理路由。
 */
static HttpResponse serveStaticFile(const std::string& url, const std::string& staticDir) {
    // 移除查询参数
    std::string path = url.substr(0, url.find('?'));

    // 默认首页
    if (path == "/") path = "/index.html";

    // 构建文件路径
    std::string filePath = staticDir + path;

    // 尝试读取文件
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        // SPA fallback: 返回 index.html
        std::ifstream indexFile(staticDir + "/index.html");
        if (!indexFile.is_open()) {
            return HttpResponse::notFound("File not found");
        }
        std::stringstream ss;
        ss << indexFile.rdbuf();
        return HttpResponse::ok(ss.str(), "text/html; charset=utf-8");
    }

    std::stringstream ss;
    ss << file.rdbuf();
    return HttpResponse::ok(ss.str(), getMimeType(path));
}

// ============================================================
// WebSocket 消息处理（JSON over WebSocket）
// ============================================================

/**
 * handleWebSocketFrame — 处理 WebSocket 文本帧。
 *
 * 消息协议：
 *   客户端 → 服务端: {type:"chat", to:userId, msg:"text"}
 *   客户端 → 服务端: {type:"ping"}
 *   服务端 → 客户端: {type:"chat", from:userId, username, msg, time}
 *   服务端 → 客户端: {type:"online_list", users:[...]}
 *   服务端 → 客户端: {type:"user_online", userId, username}
 *   服务端 → 客户端: {type:"user_offline", userId}
 */
static void handleWebSocketMessage(
    const std::string& payload,
    int64_t userId,
    SessionManager& sessionMgr) {

    try {
        auto msg = json::parse(payload);
        std::string type = msg.value("type", "");

        if (type == "chat") {
            int64_t to = msg.value("to", (int64_t)0);
            std::string text = msg.value("msg", "");
            if (to <= 0 || text.empty()) return;

            sessionMgr.saveMessage(userId, to, text);

            json response;
            response["type"] = "chat";
            response["from"] = userId;
            response["username"] = sessionMgr.getUsername(userId);
            response["msg"] = text;
            // 转发文件附件（如果有）
            if (msg.contains("file") && msg["file"].is_object()) {
                response["file"] = msg["file"];
            }
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            response["time"] = now;

            // 客户端消息 ID（用于回执跟踪）
            std::string msgId = msg.value("id", "");
            if (!msgId.empty()) response["id"] = msgId;

            std::string responseStr = response.dump();

            // 发送给目标用户
            bool delivered = sessionMgr.sendToUser(to, responseStr);

            // 发送回执
            json ack;
            if (!msgId.empty()) {
                ack["id"] = msgId;
            }
            ack["time"] = now;

            if (delivered) {
                // 已送达回执
                ack["type"] = "delivered";
            } else {
                // 用户离线
                ack["type"] = "error";
                ack["msg"] = "User is offline";
            }
            sessionMgr.sendToUser(userId, ack.dump());

            // 同时也发给自己（确认消息已发送）
            sessionMgr.sendToUser(userId, responseStr);

        } else if (type == "read") {
            // 已读回执：userId 已读 fromUser 发来的消息
            int64_t fromUser = msg.value("from", (int64_t)0);
            if (fromUser <= 0) return;

            json readMsg;
            readMsg["type"] = "read";
            readMsg["from"] = userId;
            readMsg["username"] = sessionMgr.getUsername(userId);

            // 转发已读通知给消息发送者
            sessionMgr.sendToUser(fromUser, readMsg.dump());
        } else if (type == "friend_add") {
            // 通过 WebSocket 添加好友
            int64_t targetId = msg.value("userId", (int64_t)0);
            if (targetId <= 0) return;

            std::string err = sessionMgr.sendFriendRequest(userId, targetId);
            if (err.empty() || err == "accepted") {
                // 通知目标用户
                json notify;
                notify["type"] = "friend_request";
                notify["from"] = userId;
                notify["username"] = sessionMgr.getUsername(userId);
                sessionMgr.sendToUser(targetId, notify.dump());

                if (err == "accepted") {
                    json accepted;
                    accepted["type"] = "friend_accepted";
                    accepted["by"] = targetId;
                    accepted["username"] = sessionMgr.getUsername(targetId);
                    sessionMgr.sendToUser(userId, accepted.dump());
                }
            }
        } else if (type == "friend_accept") {
            // 通过 WebSocket 接受好友请求
            int64_t requesterId = msg.value("userId", (int64_t)0);
            if (sessionMgr.acceptFriendRequest(userId, requesterId)) {
                json notify;
                notify["type"] = "friend_accepted";
                notify["by"] = userId;
                notify["username"] = sessionMgr.getUsername(userId);
                sessionMgr.sendToUser(requesterId, notify.dump());
            }
        }
        // ping 不需要回复（WebSocket 协议层的 Pong 由浏览器自动处理）
    } catch (const json::parse_error&) {
        LOG_WARNING("Invalid JSON from WebSocket");
    }
}

// ============================================================
// main
// ============================================================

int main() {
    LOG_INFO("===========================================");
    LOG_INFO("  NetChat Enterprise Chat Server");
    LOG_INFO("===========================================");

    // 加载配置（可通过 NETCHAT_CONFIG_PATH 环境变量覆盖）
    std::string configPath = "../config.json";
    const char* envCfg = std::getenv("NETCHAT_CONFIG_PATH");
    if (envCfg && envCfg[0]) configPath = envCfg;
    Config config = Config::load(configPath.c_str());
    const auto& srvCfg = config.server_config_;

    // 线程数
    int workerCount = config.workers_;
    if (workerCount <= 0) {
        workerCount = static_cast<int>(std::thread::hardware_concurrency());
        if (workerCount <= 0) workerCount = 2;
    }

    // CWD for static files
    // 静态文件目录（可通过环境变量覆盖）
    std::string staticDir = "../../client/dist";
    const char* envDir = std::getenv("NETCHAT_STATIC_DIR");
    if (envDir && envDir[0] != '\0') {
        staticDir = envDir;
    }

    LOG_INFO("Listen on " << srvCfg.host_ip_ << ":" << srvCfg.port_
              << " with " << workerCount << " workers"
              << ", static: " << staticDir);

    EventLoop mainLoop("MainLoop");
    InetAddress listenAddr(srvCfg.host_ip_, srvCfg.port_);
    Server server(&mainLoop, listenAddr);

    server.setThreadNum(workerCount);

    // ---- SSL/TLS 加密 ----
    std::unique_ptr<SslContext> sslContext;
    if (config.ssl_config_.enabled_) {
        LOG_INFO("SSL enabled, loading certificate...");
        auto ctx = std::make_unique<SslContext>(
            config.ssl_config_.cert_path_,
            config.ssl_config_.key_path_
        );
        if (ctx->valid()) {
            server.setSslContext(ctx.get());
            sslContext = std::move(ctx);
            LOG_INFO("SSL/TLS ready for connections");
        } else {
            LOG_WARNING("SSL init failed: " << ctx->error()
                       << ", falling back to unencrypted");
        }
    } else {
        LOG_DEBUG("SSL not enabled");
    }

    server.setThreadInitCallback([](EventLoop* loop) {
        LOG_INFO("Worker started: " << loop->name());
    });

    // ---- 数据库和业务对象 ----
    std::string dbPath = "../data/netchat.db";
    Database db(dbPath);
    if (!db.isOpen()) {
        LOG_ERROR("Failed to open database, exiting");
        return 1;
    }
    if (!runMigrations(db)) {
        LOG_ERROR("Migration failed, exiting");
        return 1;
    }
    SessionManager sessionMgr(db);

    // ---- Router ----
    Router router;

    // Auth: 注册
    router.post("/api/auth/register", [&](const HttpRequest& req) -> HttpResponse {
        try {
            auto body = json::parse(req.body());
            std::string username = body.value("username", "");
            std::string password = body.value("password", "");

            auto result = sessionMgr.registerUser(username, password);
            if (result.success()) {
                json resp;
                resp["userId"] = result.userId;
                resp["token"] = result.token;
                return HttpResponse::ok(resp.dump(), "application/json");
            } else {
                json err;
                err["error"] = result.error;
                return HttpResponse::badRequest(err.dump());
            }
        } catch (...) {
            json err; err["error"] = "Invalid JSON";
            return HttpResponse::badRequest(err.dump());
        }
    });

    // Auth: 登录
    router.post("/api/auth/login", [&](const HttpRequest& req) -> HttpResponse {
        try {
            auto body = json::parse(req.body());
            std::string username = body.value("username", "");
            std::string password = body.value("password", "");

            auto result = sessionMgr.loginUser(username, password);
            if (result.success()) {
                json resp;
                resp["userId"] = result.userId;
                resp["token"] = result.token;
                return HttpResponse::ok(resp.dump(), "application/json");
            } else {
                json err;
                err["error"] = result.error;
                return HttpResponse::badRequest(err.dump());
            }
        } catch (...) {
            json err; err["error"] = "Invalid JSON";
            return HttpResponse::badRequest(err.dump());
        }
    });

    // Auth: 登出
    router.post("/api/auth/logout", [&](const HttpRequest& req) -> HttpResponse {
        try {
            std::string token;
            // 检查请求体中是否携带 token
            auto body = json::parse(req.body());
            token = body.value("token", "");
            if (!token.empty()) {
                sessionMgr.invalidateToken(token);
            }
            json resp;
            resp["message"] = "Logged out";
            return HttpResponse::ok(resp.dump(), "application/json");
        } catch (...) {
            json err; err["error"] = "Invalid JSON";
            return HttpResponse::badRequest(err.dump());
        }
    });

    // ---- 查询参数解析辅助函数 ----
    auto getQueryParam = [](const std::string& url, const std::string& name) -> std::string {
        auto qpos = url.find('?');
        if (qpos == std::string::npos) return "";
        std::string query = url.substr(qpos + 1);
        auto p = query.find(name + "=");
        if (p == std::string::npos) return "";
        auto val = query.substr(p + name.size() + 1);
        auto amp = val.find('&');
        return (amp != std::string::npos) ? val.substr(0, amp) : val;
    };

    // ---- 文件上传目录 ----
    std::string uploadsDir = createUploadsDir();

    // ---- API: 文件上传（base64 编码） ----
    router.post("/api/files/upload", [&](const HttpRequest& req) -> HttpResponse {
        std::string token = getQueryParam(req.url(), "token");
        int64_t userId = sessionMgr.verifyToken(token);
        if (userId < 0) {
            json err; err["error"] = "Unauthorized";
            return HttpResponse::ok(err.dump(), "application/json");
        }

        try {
            auto body = json::parse(req.body());
            std::string name = body.value("name", "unnamed");
            std::string data = body.value("data", "");  // base64 编码的文件数据
            int64_t size = body.value("size", (int64_t)0);

            if (data.empty() || size <= 0) {
                json err; err["error"] = "Invalid file data";
                return HttpResponse::badRequest(err.dump());
            }

            // 生成文件 ID
            std::string fileId = generateFileId();
            std::string ext = name.substr(name.find_last_of('.') + 1);

            // 解码 base64
            std::string decoded;
            decoded.resize(data.size() * 3 / 4 + 64);

            // OpenSSL base64 解码
            int decodedLen = EVP_DecodeBlock(
                reinterpret_cast<unsigned char*>(&decoded[0]),
                reinterpret_cast<const unsigned char*>(data.data()),
                data.size());
            if (decodedLen < 0) {
                json err; err["error"] = "Base64 decode failed";
                return HttpResponse::badRequest(err.dump());
            }
            decoded.resize(decodedLen);

            // 写入文件
            std::string filePath = uploadsDir + "/" + fileId + "_" + name;
            std::ofstream file(filePath, std::ios::binary);
            if (!file.is_open()) {
                json err; err["error"] = "Failed to save file";
                return HttpResponse::internalError(err.dump());
            }
            file.write(decoded.data(), decoded.size());
            file.close();

            // 存储元数据
            {
                std::lock_guard<std::mutex> lock(g_fileStore.mutex);
                g_fileStore.files[fileId] = filePath;
                g_fileStore.names[fileId] = name;
                g_fileStore.sizes[fileId] = size;
                g_fileStore.owners[fileId] = userId;
            }

            LOG_INFO("File uploaded: " << name << " (" << size << " bytes) by user "
                     << userId << " id=" << fileId);

            json resp;
            resp["fileId"] = fileId;
            resp["name"] = name;
            resp["size"] = size;
            resp["url"] = "/api/files/download?id=" + fileId;
            resp["mime"] = getMimeFromExtension(name);
            return HttpResponse::ok(resp.dump(), "application/json");

        } catch (const json::parse_error&) {
            json err; err["error"] = "Invalid JSON";
            return HttpResponse::badRequest(err.dump());
        }
    });

    // ---- API: 文件下载 ----
    router.get("/api/files/download", [&](const HttpRequest& req) -> HttpResponse {
        std::string token = getQueryParam(req.url(), "token");
        if (sessionMgr.verifyToken(token) < 0) {
            json err; err["error"] = "Unauthorized";
            return HttpResponse::ok(err.dump(), "application/json");
        }

        std::string fileId = getQueryParam(req.url(), "id");
        if (fileId.empty()) {
            json err; err["error"] = "Missing file id";
            return HttpResponse::badRequest(err.dump());
        }

        std::string filePath, fileName;
        {
            std::lock_guard<std::mutex> lock(g_fileStore.mutex);
            auto it = g_fileStore.files.find(fileId);
            if (it == g_fileStore.files.end()) {
                json err; err["error"] = "File not found";
                return HttpResponse::notFound(err.dump());
            }
            filePath = it->second;
            fileName = g_fileStore.names[fileId];
        }

        // 读取文件
        std::ifstream f(filePath, std::ios::binary | std::ios::ate);
        if (!f.is_open()) {
            json err; err["error"] = "File not found on disk";
            return HttpResponse::notFound(err.dump());
        }

        auto fileSize = f.tellg();
        std::string content(fileSize, '\0');
        f.seekg(0);
        f.read(&content[0], fileSize);
        f.close();

        HttpResponse resp;
        resp.setStatusCode(HttpResponse::k200);
        resp.setStatusMessage("OK");
        resp.setContentType(getMimeFromExtension(fileName));
        resp.setHeader("Content-Disposition", "attachment; filename=\"" + fileName + "\"");
        resp.setBody(content);
        resp.setKeepAlive(false);  // 大文件下载后关闭连接
        return resp;
    });

    // ---- API: 好友管理 ----
    router.post("/api/friends/add", [&](const HttpRequest& req) -> HttpResponse {
        std::string token = getQueryParam(req.url(), "token");
        int64_t userId = sessionMgr.verifyToken(token);
        if (userId < 0) { json e; e["error"]="Unauthorized"; return HttpResponse::ok(e.dump(),"application/json"); }
        try {
            auto body = json::parse(req.body());
            int64_t targetId = body.value("userId", (int64_t)0);
            if (targetId <= 0) { json e; e["error"]="Invalid userId"; return HttpResponse::badRequest(e.dump()); }
            std::string err = sessionMgr.sendFriendRequest(userId, targetId);
            json resp;
            if (err.empty() || err == "accepted") {
                resp["success"] = true;
                resp["accepted"] = (err == "accepted");
                // 通知目标用户
                json notify;
                notify["type"] = "friend_request";
                notify["from"] = userId;
                notify["username"] = sessionMgr.getUsername(userId);
                sessionMgr.sendToUser(targetId, notify.dump());
                return HttpResponse::ok(resp.dump(), "application/json");
            }
            resp["error"] = err;
            return HttpResponse::badRequest(resp.dump());
        } catch (...) {
            json e; e["error"]="Invalid JSON"; return HttpResponse::badRequest(e.dump());
        }
    });

    router.post("/api/friends/accept", [&](const HttpRequest& req) -> HttpResponse {
        std::string token = getQueryParam(req.url(), "token");
        int64_t userId = sessionMgr.verifyToken(token);
        if (userId < 0) { json e; e["error"]="Unauthorized"; return HttpResponse::ok(e.dump(),"application/json"); }
        try {
            auto body = json::parse(req.body());
            int64_t requesterId = body.value("userId", (int64_t)0);
            if (sessionMgr.acceptFriendRequest(userId, requesterId)) {
                json resp; resp["success"] = true;
                // 通知对方
                json notify;
                notify["type"] = "friend_accepted";
                notify["by"] = userId;
                notify["username"] = sessionMgr.getUsername(userId);
                sessionMgr.sendToUser(requesterId, notify.dump());
                return HttpResponse::ok(resp.dump(), "application/json");
            }
            json e; e["error"]="No pending request"; return HttpResponse::badRequest(e.dump());
        } catch (...) {
            json e; e["error"]="Invalid JSON"; return HttpResponse::badRequest(e.dump());
        }
    });

    router.post("/api/friends/reject", [&](const HttpRequest& req) -> HttpResponse {
        std::string token = getQueryParam(req.url(), "token");
        int64_t userId = sessionMgr.verifyToken(token);
        if (userId < 0) { json e; e["error"]="Unauthorized"; return HttpResponse::ok(e.dump(),"application/json"); }
        try {
            auto body = json::parse(req.body());
            int64_t requesterId = body.value("userId", (int64_t)0);
            sessionMgr.rejectFriendRequest(userId, requesterId);
            json resp; resp["success"] = true;
            return HttpResponse::ok(resp.dump(), "application/json");
        } catch (...) {
            json e; e["error"]="Invalid JSON"; return HttpResponse::badRequest(e.dump());
        }
    });

    router.post("/api/friends/remove", [&](const HttpRequest& req) -> HttpResponse {
        std::string token = getQueryParam(req.url(), "token");
        int64_t userId = sessionMgr.verifyToken(token);
        if (userId < 0) { json e; e["error"]="Unauthorized"; return HttpResponse::ok(e.dump(),"application/json"); }
        try {
            auto body = json::parse(req.body());
            int64_t friendId = body.value("userId", (int64_t)0);
            sessionMgr.removeFriend(userId, friendId);
            json resp; resp["success"] = true;
            return HttpResponse::ok(resp.dump(), "application/json");
        } catch (...) {
            json e; e["error"]="Invalid JSON"; return HttpResponse::badRequest(e.dump());
        }
    });

    router.get("/api/friends", [&](const HttpRequest& req) -> HttpResponse {
        std::string token = getQueryParam(req.url(), "token");
        int64_t userId = sessionMgr.verifyToken(token);
        if (userId < 0) { json e; e["error"]="Unauthorized"; return HttpResponse::ok(e.dump(),"application/json"); }
        auto friends = sessionMgr.getFriends(userId);
        json list = json::array();
        for (const auto& f : friends) {
            json j; j["userId"]=f.userId; j["username"]=f.username; j["online"]=f.online;
            list.push_back(j);
        }
        return HttpResponse::ok(list.dump(), "application/json");
    });

    router.get("/api/friends/requests", [&](const HttpRequest& req) -> HttpResponse {
        std::string token = getQueryParam(req.url(), "token");
        int64_t userId = sessionMgr.verifyToken(token);
        if (userId < 0) { json e; e["error"]="Unauthorized"; return HttpResponse::ok(e.dump(),"application/json"); }
        auto requests = sessionMgr.getPendingRequests(userId);
        json list = json::array();
        for (const auto& r : requests) {
            json j; j["userId"]=r.userId; j["username"]=r.username;
            list.push_back(j);
        }
        return HttpResponse::ok(list.dump(), "application/json");
    });

    // ---- API: 获取用户列表 ----
    router.get("/api/users", [&](const HttpRequest& req) -> HttpResponse {
        std::string token = getQueryParam(req.url(), "token");
        int64_t userId = sessionMgr.verifyToken(token);
        if (userId < 0) {
            json err; err["error"] = "Unauthorized";
            return HttpResponse::ok(err.dump(), "application/json");
        }

        auto users = sessionMgr.getAllUsers();
        json list = json::array();
        for (const auto& u : users) {
            json j;
            j["userId"] = u.userId;
            j["username"] = u.username;
            j["online"] = u.online;
            list.push_back(j);
        }
        return HttpResponse::ok(list.dump(), "application/json");
    });

    // ---- API: 获取消息历史 ----
    router.get("/api/messages/history", [&](const HttpRequest& req) -> HttpResponse {
        std::string token = getQueryParam(req.url(), "token");
        int64_t userId = sessionMgr.verifyToken(token);
        if (userId < 0) {
            json err; err["error"] = "Unauthorized";
            return HttpResponse::ok(err.dump(), "application/json");
        }

        int64_t with = 0;
        try { with = std::stoll(getQueryParam(req.url(), "with")); } catch (...) {}
        int limit = 50;
        try { limit = std::stoi(getQueryParam(req.url(), "limit")); } catch (...) {}

        auto msgs = sessionMgr.getHistory(userId, with, limit);
        json jarray = json::array();
        for (const auto& m : msgs) {
            json j;
            j["fromUserId"] = m.fromUserId;
            j["toUserId"] = m.toUserId;
            j["content"] = m.content;
            j["created_at"] = m.timestamp;
            jarray.push_back(j);
        }
        return HttpResponse::ok(jarray.dump(), "application/json");
    });

    // ---- 连接回调 ----
    server.setConnectionCallback([&](const std::shared_ptr<TcpConnection>& conn) {
        if (conn->state() == TcpConnection::kConnected) {
            LOG_INFO("Connection: " << conn->name()
                     << " [" << conn->peerAddr().toIpPort() << "]");
            conn->resetHttpParser();
        } else if (conn->state() == TcpConnection::kDisconnected) {
            LOG_INFO("Disconnected: " << conn->name());
        }
    });

    // ---- HTTP / WebSocket 消息回调 ----
    server.setMessageCallback([&](const std::shared_ptr<TcpConnection>& conn,
                                  Buffer* buffer) {
        // WebSocket 模式：使用独立的帧处理器（在升级时已设置）
        if (conn->isWebSocketMode()) {
            return;
        }

        // HTTP 模式
        HttpParser* parser = conn->getHttpParser();
        if (!parser) return;

        while (parser->parse(buffer)) {
            const auto& req = parser->request();
            const std::string& url = req.url();

            // ---- 检测 WebSocket 升级请求 ----
            std::string upgrade = req.header("Upgrade");
            if (upgrade == "websocket" || upgrade == "WebSocket") {
                // 验证 token
                std::string token;
                auto qpos = url.find("token=");
                if (qpos != std::string::npos) {
                    token = url.substr(qpos + 6);
                    // 去掉可能的后缀
                    auto amp = token.find('&');
                    if (amp != std::string::npos) token = token.substr(0, amp);
                }

                int64_t userId = sessionMgr.verifyToken(token);
                if (userId < 0) {
                    LOG_WARNING("WebSocket auth failed, invalid token");
                    conn->send("HTTP/1.1 401 Unauthorized\r\n\r\n");
                    conn->shutdown();
                    parser->reset();
                    return;
                }

                // 计算 WebSocket Accept
                std::string key = req.header("Sec-WebSocket-Key");
                std::string accept = websocket::computeAccept(key);

                // 发送 101 Switching Protocols
                std::string response =
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " + accept + "\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "\r\n";
                conn->send(response);

                LOG_INFO("WebSocket upgrade: " << conn->name()
                         << " user=" << sessionMgr.getUsername(userId));

                // 切换 WebSocket 模式
                auto wsParser = std::make_shared<WebSocketParser>();
                conn->switchToWebSocket([wsParser, userId, &sessionMgr](Buffer* buf, TcpConnection* c) {
                    while (wsParser->parse(buf)) {
                        const auto& frame = wsParser->frame();
                        if (frame.opcode == ws::kText) {
                            handleWebSocketMessage(frame.payload, userId, sessionMgr);
                        } else if (frame.opcode == ws::kPing) {
                            c->send(websocket::encodeFrame(ws::kPong, ""));
                        } else if (frame.opcode == ws::kClose) {
                            sessionMgr.userOffline(userId);
                            c->send(websocket::encodeCloseFrame(ws::kNormalClosure, "bye"));
                            c->shutdown();
                        }
                        wsParser->reset();
                    }
                });

                // 设置 WebSocket 断开回调（处理 TCP 异常断开场景）
                conn->setWsCloseCallback([userId, &sessionMgr]() {
                    sessionMgr.userOffline(userId);
                });

                // 注册在线
                sessionMgr.userOnline(userId, conn);

                parser->reset();
                return;
            }

            // ---- 普通 HTTP 请求 ----
            LOG_INFO(req.methodString() << " " << url);

            // 先尝试 API 路由
            HttpResponse response;
            bool routed = router.dispatch(req, &response);

            if (!routed) {
                // 不是 API 路由，尝试静态文件
                response = serveStaticFile(url, staticDir);
            }

            conn->send(response.toString());

            bool keepAlive = req.isKeepAlive() && response.keepAlive();
            if (!keepAlive) {
                conn->shutdown();
                return;
            }
            parser->reset();
        }

        if (parser->state() == HttpParser::State::kError) {
            LOG_WARNING("HTTP parse error: " << parser->errorMessage());
            conn->send(HttpResponse::badRequest().toString());
            conn->shutdown();
        }
    });

    // ---- 信号处理 ----
    SignalHandler signalHandler(&mainLoop, {SIGINT, SIGTERM},
        [&](int signum) {
            LOG_INFO("Signal " << signum << ", shutting down...");
            mainLoop.quit();
        });

    // ---- 定时状态 ----
    mainLoop.runEvery(60.0, [&server]() {
        LOG_INFO("Status - Connections: " << server.connectionCount());
    });

    // ---- 启动！ ----
    server.start();
    LOG_INFO("Server running on http://" << srvCfg.host_ip_ << ":" << srvCfg.port_);
    mainLoop.loop();

    // ---- 清理 ----
    LOG_INFO("Stopping workers...");
    auto* pool = server.threadPool();
    if (pool) {
        for (auto* loop : pool->getAllLoops()) {
            loop->quit();
        }
    }
    LOG_INFO("Shutdown complete.");
    return 0;
}
