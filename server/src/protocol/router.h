#ifndef NETCHAT_ROUTER_H
#define NETCHAT_ROUTER_H

#include "http_request.h"
#include "http_response.h"

#include <functional>
#include <string>
#include <vector>

/**
 * Router — HTTP 路由分发器。
 *
 * 把 HTTP 方法 + URL 路径映射到对应的处理函数。
 * 当前版本只支持精确匹配（exact match），
 * 不支持路径参数（如 /users/:id）和通配符。
 *
 * 使用方式：
 *   Router router;
 *   router.get("/", handler);
 *   router.post("/api/echo", handler);
 *
 *   HttpResponse resp;
 *   if (router.dispatch(req, &resp)) { ... }
 *
 * 讲解点：精确匹配 vs 模式匹配
 * 精确匹配实现简单、性能高（O(n) 线性扫表）。
 * 后续可以优化为哈希表（O(1)）或者 Trie 树。
 * 对于聊天服务器的 REST API（通常只有十几个端点），
 * 线性扫描几毫秒都不到，完全不用担心性能。
 */
class Router {
public:
    /// 路由处理函数：接收 HttpRequest，返回 HttpResponse
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    Router() {
        // 默认 404 handler
        defaultHandler_ = [](const HttpRequest&) {
            return HttpResponse::notFound();
        };
    }

    // ---- 路由注册 ----

    void get(const std::string& path, Handler handler) {
        addRoute("GET", path, std::move(handler));
    }

    void post(const std::string& path, Handler handler) {
        addRoute("POST", path, std::move(handler));
    }

    void put(const std::string& path, Handler handler) {
        addRoute("PUT", path, std::move(handler));
    }

    void del(const std::string& path, Handler handler) {
        addRoute("DELETE", path, std::move(handler));
    }

    void addRoute(const std::string& method, const std::string& path, Handler handler) {
        routes_.push_back({method, path, std::move(handler)});
    }

    void setDefaultHandler(Handler handler) {
        defaultHandler_ = std::move(handler);
    }

    /**
     * dispatch — 分发请求。
     *
     * 遍历所有路由表，找到 method + path 都匹配的 handler。
     * 如果找不到，用 defaultHandler_ 返回 404。
     *
     * @return true 表示成功分发，false 表示出错
     */
    bool dispatch(const HttpRequest& req, HttpResponse* resp) const {
        const std::string& method = req.methodString();
        std::string path = req.url();

        // 去掉查询参数（?后面的部分），只匹配路径
        auto qpos = path.find('?');
        if (qpos != std::string::npos) {
            path = path.substr(0, qpos);
        }

        for (const auto& route : routes_) {
            if (route.method == method && route.path == path) {
                *resp = route.handler(req);
                // 根据请求的 Connection 设置响应的 Keep-Alive
                if (!req.isKeepAlive()) {
                    resp->setKeepAlive(false);
                }
                return true;
            }
        }

        // 没有匹配的路由 → 404
        *resp = defaultHandler_(req);
        return false;  // 返回 false 表示走到了 404
    }

private:
    struct Route {
        std::string method;
        std::string path;
        Handler handler;
    };

    std::vector<Route> routes_;
    Handler defaultHandler_;
};

#endif // NETCHAT_ROUTER_H
