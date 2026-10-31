# NetChat — Reactor 模型 WebChat 学习项目

> 从零实现一个支持多用户实时聊天的 Web 聊天应用。
> 网络层、HTTP 与 WebSocket 服务端自研；持久化使用 MySQL 8.4，不依赖 Nginx/Redis。

## 项目简介

NetChat 是一个**学习项目**，目标是深入理解：

- **网络编程**：Reactor 模式、epoll 多路复用、非阻塞 I/O
- **协议实现**：HTTP/1.1 解析、WebSocket (RFC 6455) 帧编码解码
- **并发模型**：多 Reactor (One Loop Per Thread)、线程安全消息传递
- **架构设计**：分层解耦、有限状态机、双缓冲、RAII

客户端使用 React + TypeScript + Zustand，通过 WebSocket 与 C++ 服务端通信。

---

## 快速开始

需要 Docker Engine + Compose v2，以及 Node.js 22+（更新前端和运行测试使用）。以下从项目根目录执行：

```bash
# 创建本地随机密码，不覆盖已有 .env，也不打印密码
node scripts/init-env.mjs

# 修改前端后需要重新生成静态产物
npm --prefix client ci
npm --prefix client run build

# 启动独立 MySQL → 运行一次迁移 → 启动聊天服务
# 运行账号只有 DML 权限，不使用 root、不自动建表
docker compose up -d --build
```

浏览器访问 **http://localhost:8081**。若原服务仍占用 8081，可临时使用 `NETCHAT_HTTP_PORT=8082 docker compose up -d --build`，在 8082 验证新服务，不必停止或清理旧服务。

开发前端可另开终端运行 `npm --prefix client run dev`，访问 `http://localhost:5173`；其代理默认连接 8081，使用其他后端端口时需同步调整 `client/vite.config.ts`。

**存储已切换为 MySQL，全新数据库需要重新注册。** 不再使用旧 SQLite 数据库，也不提供旧数据自动导入。连接池、预编译语句、事务及数据库错误处理见 [MySQL 部署与开发说明](docs/mysql.md)。

### 回归测试

```bash
npm --prefix client test
node server/tests/run-mysql-tests.mjs
```

前者测试前端 store 和列表渲染；后者需要 Docker 权限，为每次测试创建独立的随机项目、MySQL 库和临时密码，执行迁移、原生数据库测试以及真实双账号 HTTP/WebSocket 测试，最后只清理本轮测试资源，不操作开发库。

**用户 ID 协议：** HTTP/WebSocket 中的 `userId`、`from`、`to`、`by`、`fromUserId`、`toUserId` 均使用十进制字符串；C++ 使用 `int64_t`，MySQL 使用 `BIGINT`。不要在 JavaScript 中将它们转成 `Number`。

### 外部 MySQL / 本机构建

生产或独立数据库使用 `compose.external.yml`，默认启用 `VERIFY_IDENTITY` 并要求数据库 CA；不会启动本地 MySQL。本机 CMake 构建需要官方 MySQL 客户端开发库。完整命令、环境变量及权限说明见 [docs/mysql.md](docs/mysql.md)。

---

## 架构概览

```
┌─────────────────────────────────────────────────────┐
│                    Browser (React)                    │
│  ┌──────────────┐    ┌───────────────────────────┐  │
│  │  ChatPage.tsx │───▶│  WsClient (WebSocket)     │  │
│  │  ChatPanel    │    │  + auto-reconnect          │  │
│  │  MessageList  │    │  + heartbeat (30s ping)   │  │
│  └──────────────┘    └───────────┬───────────────┘  │
└──────────────────────────────────┼──────────────────┘
                                   │
        ┌────── HTTP/WS ───────────┤
        ▼                          ▼
┌──────────────────────────────────────────────────────┐
│                C++ Server (自研)                      │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────┐  │
│  │ Acceptor │─▶│  Reactor  │─▶│  TcpConnection    │  │
│  │(listen)  │  │ Pool     │  │  + Channel        │  │
│  └──────────┘  │(round-   │  │  + Buffer (双缓冲)│  │
│                │ robin)   │  │  + HttpParser     │  │
│                └──────────┘  │  + WebSocketParser │  │
│                              └────────┬─────────┘  │
│                                       │             │
│  ┌────────────────────────────────────┘             │
│  ▼                                                   │
│  ┌──────────────────────────────────────────────┐    │
│  │  SessionManager                               │    │
│  │  ├── 用户注册/登录 (SHA256 + salt)            │    │
│  │  ├── Token 验证                               │    │
│  │  ├── 在线用户管理                             │    │
│  │  ├── 消息路由 (点对点 + 广播)                 │    │
│  │  ├── 消息持久化 (MySQL 8.4)                     │    │
│  │  └── 好友系统                                 │    │
│  └──────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────┘
```

---

## 设计历程

### 第一阶段：Reactor 模式 —— 事件驱动的网络框架

**关键词**：`EventLoop` · `Channel` · `Acceptor` · `epoll`

网络编程的核心问题是：**如何高效地等待多个 fd 上的事件？**

最朴素的方案是每个连接开一个线程（one-thread-per-connection）。但线程多了之后：
- 上下文切换开销巨大
- 每个线程都需要一个栈（默认 8MB），C10K 下内存爆炸
- 锁竞争严重

Reactor 模式解决这个问题：**用一个线程监听所有 fd，事件来了再分发给对应的处理函数**。

```
while (true) {
    int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
    for (int i = 0; i < n; i++) {
        Channel* ch = (Channel*)events[i].data.ptr;
        ch->handleEvent();  // 分发到 readCallback_ / writeCallback_
    }
}
```

**三个核心类**：

| 类 | 职责 | 生命周期 |
|----|------|---------|
| `EventLoop` | 封装 `epoll_wait` 循环，维持线程关联 | 每个 I/O 线程一个 |
| `Channel` | fd + 事件 + 回调 的绑定，不拥有 fd | 与 TcpConnection 同寿 |
| `Acceptor` | 监听 `listen_fd`，接受新连接 | 与 Server 同寿 |

> **学到的东西**：Reactor 分离了"等待事件"和"处理事件"两个关注点。Channel 是一个轻量的"事件分发器"，不像 Socket 那样持有 fd 的所有权。这样同一个 fd 可以在不同阶段注册不同的事件（比如握手阶段只关心 EPOLLOUT，建立后关心 EPOLLIN）。

### 第二阶段：Buffer 设计 —— 双缓冲 + readv 优化

**关键词**：`Buffer` · `readv` · `prependable`

Buffer 的内存布局（muduo 经典设计）：

```
[ prependable | readable | writable ]
 ^              ^          ^           ^
 begin()       peek()     beginWrite() end()
```

**三个设计要点**：

1. **头部预留（prependable）**：8 字节预留空间，让上层协议可以直接在数据前面加头部，避免拷贝。
2. **readv 双缓冲**：`readFd()` 用一个 io_vec[2] 技巧——先尝试写入 buffer 的 writable 区域，如果数据量超大（> writable 剩余），超出的部分落到栈上的 64KB 临时缓冲区，再 append 进 buffer。这避免了每次都 resize buffer。
3. **分摊策略**：当 readable 数据较少而 prependable 区域较大时（前面已被消费），`makeSpace()` 会搬移数据而非 resize，实现分摊 O(1)。

> **学到的东西**：网络编程中，数据是"流"式的，不知道一次 read 能拿多少。双缓冲和分摊策略保证了无论是 1 字节的小包还是 64KB 的大包都能高效处理。

### 第三阶段：HTTP 协议解析 —— 有限状态机

**关键词**：`HttpParser` · `FSM` · `行级解析`

HTTP 请求格式：
```
GET /path HTTP/1.1\r\n
Host: localhost\r\n
Content-Length: 42\r\n
\r\n
<body data>
```

HttpParser 的状态迁移：

```
kExpectRequestLine → kExpectHeaders → kExpectBody → kGotAll
                       ↓ (解析错误)
                     kError
```

为什么用状态机？
- **流式解析**：数据可能分多次到达，状态机可以在"数据不够"时暂停，等更多数据到达后继续。
- **行级而非字符级**：HTTP 是文本协议，利用 `Buffer::findCRLF()` 快速定位行边界，比逐字符的 FSM 更高效。

**处理 Pipeline（HTTP 流水线）**：
```cpp
while (parser->parse(&buf)) {
    handleRequest(parser->request());
    parser->reset();  // 重置后解析下一条请求
}
```

> **学到的东西**：FSM 是网络协议解析的基本范式。每个状态明确知道"我在等什么数据"以及"还缺多少数据"，天然支持流式处理。

### 第四阶段：WebSocket 协议 —— 从握手到帧传输

**关键词**：`WebSocketParser` · `encodeFrame` · `Upgrade`

WebSocket 连接分为两个阶段：

#### 4.1 握手（HTTP Upgrade）

```
客户端 → 服务端：
  GET /ws?token=xxx HTTP/1.1
  Upgrade: websocket
  Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==

服务端 → 客户端（101 Switching Protocols）：
  HTTP/1.1 101 Switching Protocols
  Upgrade: websocket
  Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
```

`Sec-WebSocket-Accept` 的计算：`base64(sha1(clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))`。这是 Challenge-Response 机制：客户端验证这个值，确保服务端确实实现了 WebSocket 协议，而不是某个中间件伪造的 Upgrade。

#### 4.2 帧协议（RFC 6455）

WebSocket 帧设计极其精巧，前 2 字节包含所有元信息：

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len==126/127)   |
| |1|2|3|       |K|             |                               |
+-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - -+
|     Masking-Key (if MASK set)       |          Payload         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ - - - - - - - - - - - - +
```

几个关键点：
- **Fin 位 + Opcode**：控制帧（%x8-9）和数据帧（%x1-2）分开，控制帧可以插在分片的数据帧之间
- **Masking**：**客户端→服务端必须 mask，服务端→客户端禁止 mask**。这是为了防止缓存污染攻击（WebSocket 的 payload 可以被嵌入 HTTP 响应的缓存中）
- **Payload Length 扩展**：7 位不够时用 126→2 字节扩展，127→8 字节扩展

**解析器状态机**：
```
kExpectHeader → kExpectExtLen16/64 → kExpectMask → kExpectPayload → kGotFrame
```

> **学到的东西**：WebSocket 帧协议是二进制协议，"逐字节解析"和 HTTP 的"行级解析"完全不同，所以用了两个不同的解析器。Mask 机制是个安全设计——防止浏览器中恶意页面利用 WebSocket 污染 HTTP 缓存。

### 第五阶段：多 Reactor —— One Loop Per Thread

**关键词**：`EventLoopThreadPool` · `Round-Robin` · `线程安全`

Acceptor 在主 Reactor 上 accept，然后把新连接的 fd **分发**给 Worker Reactor：

```
                          ┌──────────────────┐
                          │   Main Reactor    │  ← accept 连接
                          │  (EventLoop #0)   │
                          └────────┬─────────┘
                                   │
                    round-robin 分发
                    ┌───────┼───────┐
                    ▼       ▼       ▼
              ┌────────┐┌────────┐┌────────┐
              │Worker 1││Worker 2││Worker N│  ← 处理各自连接的 I/O
              │Loop #1 ││Loop #2 ││Loop #N │
              └────────┘└────────┘└────────┘
```

**为什么比单 Reactor 好？**

单 Reactor 的问题是：一个连接的大量 I/O 会阻塞其他连接的事件处理（虽然 epoll_wait 本身不阻塞，但 callback 执行时间太长也会影响其他连接的响应性）。

多 Reactor 的核心思想：**每个 CPU 核心一个线程，每个线程一个 EventLoop，每个连接固定在一个 EventLoop 上**。一个连接的所有 I/O 操作永远在同一个线程中完成，**不需要加锁**。

**跨线程发送消息怎么处理？**

这是整个架构中最微妙的地方。当用户 A（Worker 1）发消息给用户 B（Worker 2）时：

1. `handleWebSocketMessage` 在 Worker 1 线程执行
2. 它调 `sessionMgr.sendToUser(toUserId, responseStr)` → 拿到 B 的 `TcpConnection` 的 `shared_ptr`
3. 调 `conn->send(data)` — **这个 conn 可能属于 Worker 2！**
4. `send()` 检测到 `!loop_->isInLoopThread()` → 通过 `loop_->runInLoop()` 把写操作"快递"到 Worker 2 线程

```cpp
void TcpConnection::send(const char* data, size_t len) {
    if (loop_->isInLoopThread()) {
        sendInLoop(data, len);    // 直接写
    } else {
        loop_->runInLoop([this, data_copy = std::string(data, len)]() {
            sendInLoop(data_copy);  // 转发到目标线程
        });
    }
}
```

> **学到的东西**：One Loop Per Thread + runInLoop 跨线程转发，是 C++ 网络编程中"无锁并发"的经典范式。每个线程只写自己的数据结构，跨线程通信通过消息传递而非共享内存加锁。

### 第六阶段：业务层 —— SessionManager + MySQL

最初使用 SQLite，现已替换为 MySQL：业务复用 `Database` 封装，通过 RAII 连接租约和固定连接事务管理并发访问。数据库调用仍是同步的，不代表 Reactor 已具备异步 SQL 能力。

**关键词**：`SessionManager` · `Token 认证` · `消息路由`

| 模块 | 功能 |
|------|------|
| 注册/登录 | 密码 SHA256 哈希 + salt，生成 token |
| Token 认证 | 每次 WebSocket 连接时验证 URL 中的 token |
| 在线管理 | map<userId, weak_ptr<TcpConnection>> |
| 消息路由 | `sendToUser(userId, msg)` → 查找连接 → 编码帧 → 发送 |
| 消息持久化 | MySQL 预编译 INSERT/SELECT；持久化失败不确认发送 |
| 好友系统 | friendships 表双向关系 |

**Token 认证流程**：
```
客户端                 服务端
  │                     │
  │ POST /api/auth/login│ → 验证密码，生成 token，存数据库
  │ {userId, token}     │
  │────────────────────▶│
  │◀────────────────────│
  │                     │
  │ WS /ws?token=xxx    │ → verifyToken(token) → 获取 userId
  │────────────────────▶│ → userOnline(userId, conn)
  │                     │
```

**为什么用 `weak_ptr` 存储连接？**
`TcpConnection` 的生命周期由 `Server::connections_` 的 `shared_ptr` 管理。`SessionManager` 如果也存 `shared_ptr`，就会出现**循环引用**（SessionManager 持有 Connection，Connection 回调引用 SessionManager），导致内存泄漏。`weak_ptr` 可以在 Connection 被销毁时自动失效。

### 第七阶段：SSL/TLS 支持

**关键词**：`OpenSSL` · `SslContext` · `SSL_read/SSL_write`

SSL 接入层的设计：在 TcpConnection 中插一层 SSL，对上层（HTTP 解析器、WebSocket 帧处理器）透明。

```cpp
// 非 SSL：读 socket → 解析
n = inputBuffer_->readFd(socket_->fd());

// SSL：SSL_read → 解析
n = SSL_read(ssl_, buf, sizeof(buf));
inputBuffer_->append(buf, n);
```

SSL 握手也是一个状态机——`SSL_do_handshake()` 可能返回 `SSL_ERROR_WANT_READ` 或 `SSL_ERROR_WANT_WRITE`，我们根据返回值动态调整 epoll 注册的事件。

### 第八阶段：前端架构

**关键词**：`React` · `TypeScript` · `Zustand` · `Vite`

前端架构：

```
src/
├── api/
│   ├── http.ts         # HTTP API 封装 (axios)
│   └── ws.ts           # WebSocket 客户端封装 (自动重连 + 心跳)
├── components/
│   ├── ChatPanel.tsx   # 聊天面板
│   ├── MessageInput.tsx # 消息输入框
│   ├── MessageList.tsx  # 消息列表
│   └── UserList.tsx     # 用户列表
├── pages/
│   ├── ChatPage.tsx    # 聊天页面
│   ├── LoginPage.tsx   # 登录页面
│   └── RegisterPage.tsx # 注册页面
├── store/
│   └── chatStore.ts    # Zustand 状态管理
└── types/
    └── index.ts        # TypeScript 类型定义
```

**WsClient 设计**：封装了自动重连（断开 3 秒后重连）、心跳（每 30 秒发 ping）、消息队列。

**为什么不用原生 WebSocket？** 原生 `WebSocket` 太底层——没有自动重连、没有心跳、连接断开后消息丢失。封装后上层只需调用 `send()` 和 `onMessage()`，不需要操心底层连接状态。

---

## 踩坑记录

### Bug 1：WebSocket 模式下 `handleRead` 不读 socket

**现象**：发消息对方收不到，服务器 CPU 100%

**根因**：`TcpConnection::handleRead()` 中，WebSocket 模式直接调 `wsFrameHandler_` 解析 `inputBuffer_`，但**没有先读 socket**：

```cpp
// 修复前（错误的短路逻辑）
void TcpConnection::handleRead() {
    if (wsMode_ && wsFrameHandler_) {
        wsFrameHandler_(inputBuffer_.get(), this);  // inputBuffer_ 是空的！
        return;
    }
    n = inputBuffer_->readFd(socket_->fd());  // ← 只有 HTTP 模式才执行这里
}
```

修复后，每次 `handleRead` 先读 socket 数据到 `inputBuffer_`，再根据模式分发：

```cpp
n = inputBuffer_->readFd(socket_->fd());
// ...
if (wsMode_ && wsFrameHandler_) {
    wsFrameHandler_(inputBuffer_.get(), this);
} else if (messageCallback_) {
    messageCallback_(shared_from_this(), inputBuffer_.get());
}
```

### Bug 2：TCP 异常断开时未清理在线状态

**现象**：用户掉线后仍显示在线

**根因**：`userOffline` 只在收到 WebSocket Close 帧时调用。如果 TCP 连接意外断开（断网、浏览器崩溃），不会调 `userOffline`。

**修复**：在 `TcpConnection` 中新增 `wsCloseCallback_`，在 `handleClose()` 中调用。WebSocket 升级时设置此回调为 `userOffline`。

---

## 技术栈

| 层 | 技术 | 用途 |
|----|------|------|
| 网络框架 | Reactor (epoll, LT 模式) | 事件驱动 I/O |
| 协议 | HTTP/1.1 + WebSocket (RFC 6455) | 通信协议 |
| SSL | OpenSSL | 传输加密 |
| 数据库 | MySQL 8.4 + 官方 C API | 连接池、事务与持久化 |
| 构建 | CMake + Makefile | C++ 构建 |
| 前端 | React 18 + TypeScript | UI |
| 状态管理 | Zustand | 前端状态 |
| 打包 | Vite | 前端构建 |
| 容器化 | Docker + docker-compose | 部署 |

---

## 项目结构

```
├── server/
│   ├── CMakeLists.txt
│   ├── config.json              # 运行时配置
│   ├── src/
│   │   ├── main.cpp             # 入口 + HTTP/WS 路由
│   │   ├── core/                # 网络层
│   │   │   ├── eventloop.cpp/h   # Reactor 核心
│   │   │   ├── channel.cpp/h     # 事件分发通道
│   │   │   ├── acceptor.cpp/h    # 连接接受器
│   │   │   ├── server.cpp/h      # 服务器（多 Reactor 协调）
│   │   │   ├── tcp_connection.cpp/h  # TCP 连接抽象
│   │   │   ├── buffer.cpp/h      # I/O 缓冲区
│   │   │   ├── socket.cpp/h      # RAII socket 封装
│   │   │   ├── inetaddress.cpp/h  # 网络地址
│   │   │   ├── timer.h / timer_queue.cpp/h  # 定时器
│   │   │   ├── signal_handler.cpp/h  # 信号处理
│   │   │   ├── ssl_context.cpp/h  # SSL 上下文
│   │   │   └── eventloop_thread*.cpp/h  # 线程池
│   │   ├── protocol/            # 协议层
│   │   │   ├── http_parser.h     # HTTP 请求解析
│   │   │   ├── http_request.h    # HTTP 请求体
│   │   │   ├── http_response.h   # HTTP 响应构建
│   │   │   ├── router.h          # 路由分发
│   │   │   └── websocket.cpp/h   # WebSocket 帧解析/编码
│   │   ├── service/
│   │   │   └── session_mgr.cpp/h # 会话管理和业务逻辑
│   │   ├── db/
│   │   │   ├── database.cpp/h    # MySQL 预编译、连接池和 RAII 事务
│   │   │   └── migrations.cpp/h  # 数据库迁移
│   │   └── utils/
│   │       ├── auth.cpp/h        # 认证工具
│   │       ├── config.cpp/h      # 配置
│   │       └── log.cpp/h         # 日志
│   └── third_party/
│       └── nlohmann/             # JSON 库 (single-header)
│
├── client/
│   ├── src/
│   │   ├── App.tsx              # React 主应用
│   │   ├── api/ws.ts            # WebSocket 客户端封装
│   │   ├── api/http.ts          # HTTP API 封装
│   │   ├── store/chatStore.ts   # 状态管理
│   │   └── components/          # UI 组件
│   ├── dist/                    # 预构建的前端
│   ├── index.html
│   └── vite.config.ts
│
├── docker-compose.yml
├── Dockerfile
├── PLAN.md                      # 原始设计文档
└── README.md                    # 你正在看的这个
```

---

## 设计原则总结

1. **Reactor 模式**：用事件驱动代替线程驱动，一个线程管理数千连接
2. **One Loop Per Thread**：每个连接固定在一个线程，避免锁竞争
3. **RAII**：用构造函数/析构函数管理资源（Socket、SSL、数据库连接）
4. **有限状态机**：协议解析器天然用 FSM 实现，流式处理不必等完整数据
5. **弱引用**：用 `weak_ptr` 避免 SessionManager 和 TcpConnection 的循环引用
6. **跨线程消息传递**：通过 `runInLoop` 把 I/O 操作转发到目标 EventLoop 线程
