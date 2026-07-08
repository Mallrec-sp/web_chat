# NetChat 技术方案 — 自建 C++ 网络栈，极致性能

## 设计哲学

从零开始手写网络库，不依赖任何第三方网络框架（如 drogon/Boost.Asio/uWebSockets），用纯操作系统 API 实现最高性能的聊天服务器。

---

## 一、整体架构

```
┌─────────────────────────────────────────────────────────┐
│                      用户浏览器                            │
│  ┌───────────────────────────────────────────────────┐   │
│  │          React + TypeScript 前端 (Vite)            │   │
│  │    ┌─────────┐  ┌──────────┐  ┌────────────┐    │   │
│  │    │ REST    │  │WebSocket │  │  UI 组件层  │    │   │
│  │    │ Client  │  │  Client  │  │ (聊天/好友)  │    │   │
│  │    └────┬────┘  └────┬─────┘  └────────────┘    │   │
│  └─────────┼────────────┼───────────────────────────┘   │
└────────────┼────────────┼───────────────────────────────┘
             │ HTTP/1.1   │ WebSocket (RFC 6455)
             ▼            ▼
┌─────────────────────────────────────────────────────────┐
│              C++ 高性能网络服务器（自研）                    │
│                                                          │
│  ┌───────────────────────────────────────────────────┐   │
│  │               I/O 核心层                            │   │
│  │  ┌──────────┐ ┌──────────┐ ┌───────────────────┐ │   │
│  │  │ epoll    │ │ 线程池    │ │ 内存池 /  Slab    │ │   │
│  │  │(事件驱动) │ │(工作线程) │ │ 分配器            │ │   │
│  │  └──────────┘ └──────────┘ └───────────────────┘ │   │
│  └───────────────────────────────────────────────────┘   │
│                            │                              │
│  ┌───────────────────────────────────────────────────┐   │
│  │              协议解析层                              │   │
│  │  ┌──────────┐ ┌──────────┐ ┌───────────────────┐ │   │
│  │  │ HTTP/1.1 │ │WebSocket │ │ 自定义二进制协议    │ │   │
│  │  │ Parser   │ │ RFC 6455 │ │ (内部模块间通信)    │ │   │
│  │  └──────────┘ └──────────┘ └───────────────────┘ │   │
│  └───────────────────────────────────────────────────┘   │
│                            │                              │
│  ┌───────────────────────────────────────────────────┐   │
│  │              业务逻辑层                              │   │
│  │  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐    │   │
│  │  │用户系统 │ │好友关系 │ │消息路由 │ │文件管理 │    │   │
│  │  └────────┘ └────────┘ └────────┘ └────────┘    │   │
│  └───────────────────────────────────────────────────┘   │
│                            │                              │
│  ┌───────────────────────────────────────────────────┐   │
│  │               持久化层                               │   │
│  │  ┌──────────────────────────────────────────────┐ │   │
│  │  │   SQLite3 (Direct C API, 手动优化 WAL + mmap)│ │   │
│  │  └──────────────────────────────────────────────┘ │   │
│  └───────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

---

## 二、需要自研的网络组件清单

### 1. I/O 模型 — epoll（边缘触发 ET）

Linux 上最高性能的 I/O 多路复用方案，也是 nginx、Redis 等高性能服务器的共同选择。

```
epoll 核心架构:
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│    listen fd  │  │  client fd 1 │  │  client fd N │
│  (accept)     │  │  (recv/send) │  │  (recv/send) │
└──────┬───────┘  └──────┬───────┘  └──────┬───────┘
       └─────────────────┼──────────────────┘
                         ▼
                ┌────────────────┐
                │  epoll 实例     │ ← epoll_ctl() 注册 fd
                │  (1 个 fd)     │
                └────────┬───────┘
                         │ epoll_wait() 就绪事件
                         ▼
                ┌────────────────┐
                │  事件循环       │
                │  (单线程/多线程  │ ← 处理 accept/recv/send
                │   Reactor)     │
                └────────────────┘
```

关键技术点：
- **epoll 边缘触发 (ET) + 非阻塞 I/O**：只在状态变化时通知，配合非阻塞 fd 循环读取直到 EAGAIN，实现最高效率
- **EPOLLONESHOT**：配合多线程，确保一个 socket 同一时刻只被一个线程处理，避免锁竞争
- **eventfd**：用于优雅地唤醒 epoll_wait（关闭通知、任务投递）
- **accept4**：用一次系统调用完成 accept + 设置非阻塞标志

### 2. 内存管理 — 自研内存池

```
┌───────────────┐
│  Slab 分配器   │ ← 固定大小对象 (连接对象、缓冲区) 零碎片
├───────────────┤
│  Ring Buffer  │ ← 每个连接读写缓冲，无锁实现
├───────────────┤
│  内存池 (Mempool)│ ← 预分配大块内存，按需分配，减少 syscall
└───────────────┘
```

- **Slab Allocator**：为连接对象、消息对象固定大小分配，完全无碎片
- **Ring Buffer**：每个 TCP 连接一个读缓冲区 + 写缓冲区，指针操作，零拷贝
- **Memory Pool**：一次 `mmap` 申请大页内存（Huge Page 2MB 起），内部切分管理

### 3. 协议实现

**HTTP/1.1 Parser（手写）**
- 有限状态机（FSM）解析，O(n) 复杂度
- 零拷贝解析——用指针偏移代替字符串拷贝
- 支持 Pipeline

**WebSocket (RFC 6455) 协议实现**
- Frame 解析：Fin/Opcode/Mask/Payload Length 逐字节解析
- 数据帧掩码（Masking/Unmasking）
- 控制帧处理：Ping/Pong/Close
- 分片帧重组（Fragmentation）
- HTTP Upgrade 握手

### 4. 线程模型

```
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│  Worker 1    │  │  Worker 2    │  │  Worker N    │
│  epoll_wait  │  │  epoll_wait  │  │  epoll_wait  │
│  (ET oneshot)│  │  (ET oneshot)│  │  (ET oneshot)│
└──────┬───────┘  └──────┬───────┘  └──────┬───────┘
       └─────────────────┼──────────────────┘
                         ▼
               ┌─────────────────┐
               │ 就绪事件分发      │
               │ (解析 → 业务 → 回写)│
               └─────────────────┘
```

- 多 Reactor 模式：每个工作线程独立调用 `epoll_wait()`，通过 `EPOLLONESHOT` 确保每个 socket 一次只被一个线程处理
- 线程数 = `std::thread::hardware_concurrency()`（通常 2 × 物理核心数）
- 连接通过 `eventfd` 进行跨线程迁移（当需要离线消息等逻辑处理时）
- 避免线程切换和锁竞争，尽量做到 per-connection 数据无锁

### 5. 定时器系统

- 层级时间轮（Hierarchical Timing Wheel）
- 用于：WebSocket Ping/Pong 心跳检查、连接超时、速率限制

---

## 三、可以保留的第三方组件

虽然是"自己造轮子"，但有些东西没必要重复发明：

| 组件 | 保留理由 | 替代方案 |
|------|---------|---------|
| **nlohmann/json** | JSON 是数据格式不是网络协议 | 手写 JSON parser（开发量大 N 倍） |
| **SQLite3**（C 库） | 数据库引擎本身是成熟的 C 库 | 自己写 B-tree + SQL 解析器就偏离目标了 |
| **fmtlib/fmt** | 高性能格式化，C++20 std::format 的前身 | 纯手写字符串拼接 |
| **spdlog** | 异步日志，性能极高 | 自己写日志轮转 |
| **OpenSSL** | TLS/HTTPS 必须 | 完全自己实现 TLS 不可能也不安全 |

> **关于密码学**：用户密码哈希直接用现成的，自己实现密码学算法很危险。

---

## 四、自研 vs 放弃的库

| 功能 | 自研 | 放弃的库 | 原因 |
|------|------|---------|------|
| TCP 连接管理 | ✅ 基于 epoll ET 手写 | drogon, Boost.Asio, uWebSockets | 核心轮子 |
| HTTP 解析 | ✅ FSM 手写 | llhttp, http_parser | 协议不复杂，手写可控 |
| WebSocket | ✅ RFC 6455 手写 | uWebSockets, libwebsockets | 核心轮子 |
| 内存分配器 | ✅ Slab + Ring Buffer | mimalloc, tcmalloc | 要极致性能 |
| 线程池 | ✅ 基于 epoll | 标准线程池 | 事件驱动天然绑定 |
| 连接保活/心跳 | ✅ 时间轮 | — | 自研 |
| JSON 解析 | ❌ 使用 nlohmann | rapidjson | 非核心，够用就行 |
| 数据库 | ❌ 使用 SQLite3 C API | — | 数据库引擎太重 |
| 日志 | ❌ 使用 spdlog | — | 辅助设施 |
| 密码学 | ❌ 使用 crypto 库 | — | 安全不能手写 |

---

## 五、前端方案

前端走标准 TS 栈：

| 前端技术 | 方案 |
|---------|------|
| 框架 | React 18 + TypeScript |
| 构建工具 | Vite |
| 状态管理 | Zustand |
| WebSocket | 原生 WebSocket API |
| UI | Ant Design 或 自建 |
| HTTP 请求 | fetch + ky |
| 文件上传 | HTTP Multipart |
| 消息格式 | JSON（收发文本）/ 二进制（文件传输可选） |

---

## 六、推荐开发路径

因为是自研，建议分阶段迭代，每阶段都有可验证的产出：

### Phase 1：I/O 核心（约 1-2 周）
1. 实现 epoll 事件循环（ET 模式） —— 能 accept 连接
2. 实现 TCP 读写 —— 回显服务器（echo server）
3. 验证：`netcat` 或写一个简单的 Python/JS 客户端测试

### Phase 2：HTTP 协议（约 1 周）
1. 实现 HTTP/1.1 请求解析器（FSM）
2. 实现 HTTP 响应构造
3. 实现 GET/POST 路由
4. 验证：浏览器能访问服务器，返回静态页面

### Phase 3：WebSocket 协议（约 1-2 周）
1. 实现 WebSocket 握手（HTTP Upgrade）
2. 实现数据帧编解码（RFC 6455）
3. 实现 Ping/Pong/Close 控制帧
4. 验证：前端 JS WebSocket 连接成功，双向文本通信

### Phase 4：业务逻辑（约 2-3 周）
1. 用户注册/登录（SQLite3 + JWT）
2. 好友系统（CRUD API）
3. 消息路由（点对点 WebSocket 转发）
4. 消息存储与历史拉取
5. 文件上传/下载

### Phase 5：高级功能（约 1-2 周）
1. 已读回执
2. 正在输入提示
3. 离线消息
4. 在线状态广播

### Phase 6：性能优化（持续）
1. 内存池调优
2. zero-copy 路径
3. 锁竞争分析
4. Send 缓冲区合并

---

## 七、项目结构

```
E:\project\NetChat\
├── client/                    # 前端 (Vite + React/TS)
│   ├── src/
│   │   ├── components/        # UI 组件
│   │   ├── pages/             # 页面
│   │   ├── hooks/             # 自定义 Hooks
│   │   ├── store/             # Zustand
│   │   ├── api/               # HTTP + WebSocket Client
│   │   └── types/             # TS 类型
│   └── package.json
│
├── server/                    # C++ 服务端（自研）
│   ├── src/
│   │   ├── core/              # I/O 核心
│   │   │   ├── epoll.cpp/h     # epoll 事件循环
│   │   │   ├── socket.cpp/h   # 套接字封装
│   │   │   ├── buffer.cpp/h   # Ring Buffer
│   │   │   ├── mempool.cpp/h  # 内存池 / Slab 分配器
│   │   │   └── timer.cpp/h    # 时间轮定时器
│   │   │
│   │   ├── protocol/          # 协议实现
│   │   │   ├── http_parser.cpp/h   # HTTP/1.1 解析器 (FSM)
│   │   │   ├── http_response.cpp/h # HTTP 响应构建
│   │   │   ├── websocket.cpp/h     # WebSocket RFC 6455
│   │   │   └── router.cpp/h        # 路由匹配
│   │   │
│   │   ├── handler/           # 请求处理器
│   │   │   ├── user_handler.cpp/h  # 用户相关
│   │   │   ├── message_handler.cpp/h # 消息相关
│   │   │   ├── friend_handler.cpp/h # 好友相关
│   │   │   └── file_handler.cpp/h    # 文件上传
│   │   │
│   │   ├── service/           # 业务逻辑
│   │   │   ├── user_service.cpp/h
│   │   │   ├── message_service.cpp/h
│   │   │   ├── friend_service.cpp/h
│   │   │   └── session_mgr.cpp/h    # 在线会话管理
│   │   │
│   │   ├── db/                # 数据库层
│   │   │   ├── database.cpp/h       # SQLite3 封装
│   │   │   └── migrations.cpp/h     # 数据库迁移
│   │   │
│   │   ├── utils/             # 工具
│   │   │   ├── json.cpp/h          # nlohmann/json 封装
│   │   │   ├── jwt.cpp/h           # JWT 编解码
│   │   │   ├── crypto.cpp/h        # 密码哈希
│   │   │   └── config.cpp/h        # 配置读取
│   │   │
│   │   └── main.cpp           # 入口
│   │
│   ├── tests/                 # 单元测试
│   │   ├── test_http_parser.cpp
│   │   ├── test_websocket.cpp
│   │   └── test_ring_buffer.cpp
│   │
│   ├── third_party/           # 第三方库
│   │   ├── nlohmann/
│   │   └── sqlite3/
│   │
│   ├── CMakeLists.txt
│   └── config.json
│
├── uploads/                   # 文件上传存储
├── data/                      # SQLite 数据文件
└── README.md
```

---

## 八、关键性能目标

| 指标 | 目标值 |
|------|-------|
| 并发连接数 | 10,000+ (单机) |
| 消息延迟 (p99) | < 5ms (局域网) |
| 消息吞吐 | > 50,000 msg/s |
| 内存占用 (10k 空闲连接) | < 200 MB |
| 零拷贝路径 | 收发全链路零拷贝 |
| 锁竞争 | 无共享状态，per-connection 无锁 |

---

## 九、验证方案

1. **单元测试**：每个协议解析器、缓冲区的独立测试
2. **Echo 测试**：实现 echo server，用多客户端压测连接数和延迟
3. **WebSocket 测试**：用前端页面连接 WebSocket，验证双向通信
4. **集成压测**：模拟多用户同时登录、发消息、收发文件
5. **性能剖析**：Linux 上用 perf + FlameGraph 做 CPU 分析，Valgrind/Cachegrind 做内存分析

---

## 十、开发环境

| 工具 | 建议 |
|------|------|
| 编译器 | GCC 11+ 或 Clang 14+ |
| 构建系统 | CMake + Ninja |
| IDE | VS Code + C++ 插件 (在 WSL2 或远程 Linux 上开发) |
| 性能分析 | perf + FlameGraph |
| 调试 | GDB / LLDB + AddressSanitizer |
| 开发方式 | 方案 A：Windows 上用 WSL2 开发；方案 B：纯 Linux 虚拟机或远程服务器 |
| WSL2 | 可直接安装 Ubuntu 22.04 做开发环境，epoll 完全支持 |
