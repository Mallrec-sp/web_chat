# MySQL 部署与开发

## 范围与运行模型

当前版本只使用 MySQL 8.4，不再链接 SQLite，也不提供旧 SQLite 数据自动导入；新库需要重新注册。用户 ID 与毫秒时间保持 BIGINT，HTTP/WebSocket 中的用户 ID 仍为字符串。

- `Database` 封装官方 MySQL C API，使用预编译参数、线程安全有界连接池和 RAII 事务。
- 单条操作借用一个连接；一个事务始终使用同一连接，未提交会回滚。
- 数据库失败抛出 `DbError`，不会被误当成空查询结果。HTTP 返回 503；WebSocket 保存失败返回带消息 ID 的 error，不确认发送成功。
- 断线连接会被淘汰，下次借用可新建连接；不会自动重放写入或 COMMIT。提交期间断线可能无法确认是否提交，不能直接重试相同操作。
- 连接/读写/租约等待有超时，锁等待上限为 5 秒；这些不构成整个请求的硬截止时间。
- **SQL 仍是同步调用**，会阻塞当前网络 worker。连接池不是异步数据库线程池；高并发、消息幂等和多实例会话路由仍需要后续设计。

## 本地 Docker 开发

前置：Docker Engine、Compose v2、可访问 Docker daemon 的账号，以及 Node.js 22+。

```bash
# 项目根目录；只创建不存在的 .env，权限为 0600
node scripts/init-env.mjs
npm --prefix client ci
npm --prefix client run build

docker compose up -d --build
docker compose ps
docker compose logs --tail=100 migrate netchat
curl --fail http://127.0.0.1:8081/api/health
```

启动顺序为：MySQL 健康 → `migrate` 运行 `server --migrate` 成功退出 → 应用启动并检查 schema 版本。

- MySQL 端口仅绑定 `127.0.0.1:3306`，不会对局域网开放。应用默认监听 `8081`。
- 原服务占用 8081 时，使用 `NETCHAT_HTTP_PORT=8082 docker compose up -d --build` 并访问 8082，先验证新服务。
- `netchat_app` 只有目标库 SELECT/INSERT/UPDATE/DELETE 权限；`netchat_migrator` 有目标库 DDL 权限。应用不使用 root。
- Docker 开发库使用自动生成的证书；`REQUIRED` 保证加密，但**不验证服务器身份，仅适用于这个隔离开发环境**。线上用 `VERIFY_IDENTITY`。
- 密码仅在空数据卷初始化时生效。修改 `.env` 不会修改已有数据库账号的密码，需执行数据库密码轮换并更新配置；不要为改密码删除数据卷。
- `docker compose down` 保留数据卷。**不要随意加 `--volumes` 或运行全局 prune**，那会删除数据。
- Docker socket 权限不足时，由管理员按环境策略处理；不要把 socket 改成全员可写。加入 docker 组通常等同于授予主机高权限。

## 本机 CMake 构建，Docker 只运行数据库

使用官方 MySQL 客户端开发库（不是 SQLite，也不保证 MariaDB 客户端头文件兼容）：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libssl-dev libmysqlclient-dev

# 创建本地 .env 后启动数据库；已有 .env 时不要重复生成
node scripts/init-env.mjs
docker compose up -d --wait mysql
cmake -S server -B server/build-mysql -DCMAKE_BUILD_TYPE=Release
cmake --build server/build-mysql -j2

# 仅加载自己生成的本地文件；不要 source 不可信文件
set -a
. ./.env
set +a
export NETCHAT_DB_HOST=127.0.0.1
export NETCHAT_DB_PORT="${NETCHAT_MYSQL_PORT:-3306}"
export NETCHAT_DB_NAME=netchat
export NETCHAT_DB_SSL_MODE=REQUIRED

cd server/build-mysql
NETCHAT_DB_USER=netchat_migrator NETCHAT_DB_PASSWORD="$NETCHAT_MIGRATION_PASSWORD" ./server --migrate
NETCHAT_DB_USER=netchat_app NETCHAT_DB_PASSWORD="$NETCHAT_APP_PASSWORD" ./server
```

工作目录保持在 `server/build-mysql`，默认静态文件位于 `../../client/dist`，上传目录为 `../uploads`。需要自定义时可通过 `NETCHAT_CONFIG_PATH` 和 `NETCHAT_STATIC_DIR` 指定绝对路径。编辑器可使用 CMake 生成的 `compile_commands.json` 获取真实 include 路径。

## 连接配置

`server/config.json` 的 `database` 配置由环境变量覆盖；配置文件不存在时，环境变量仍生效。示例 JSON 不存密码，启动缺少密码会明确失败。

| 环境变量 | JSON 字段 | 默认值 |
|---|---|---|
| `NETCHAT_DB_HOST` | `host` | `127.0.0.1` |
| `NETCHAT_DB_PORT` | `port` | `3306` |
| `NETCHAT_DB_NAME` | `name` | `netchat` |
| `NETCHAT_DB_USER` | `user` | `netchat_app` |
| `NETCHAT_DB_PASSWORD` | `password` | 无，必须提供 |
| `NETCHAT_DB_POOL_SIZE` | `pool_size` | `4`，允许 1–128 |
| `NETCHAT_DB_CONNECT_TIMEOUT` | `connect_timeout_seconds` | `5` |
| `NETCHAT_DB_READ_TIMEOUT` | `read_timeout_seconds` | `5` |
| `NETCHAT_DB_WRITE_TIMEOUT` | `write_timeout_seconds` | `5` |
| `NETCHAT_DB_ACQUIRE_TIMEOUT_MS` | `acquire_timeout_ms` | `3000` |
| `NETCHAT_DB_SSL_MODE` | `ssl_mode` | `VERIFY_IDENTITY` |
| `NETCHAT_DB_SSL_CA` | `ssl_ca` | 默认信任配置，可显式指定 CA |

TLS 模式仅接受 `DISABLED`、`REQUIRED`、`VERIFY_IDENTITY`。默认不自动降级 TLS，不自动获取不可信服务器的 RSA 公钥。应用示例运行账号要求 SSL，不允许以 DISABLED 连接。

数据库会话强制严格 SQL 模式，避免静默截断。消息使用 LONGTEXT，但实际大小仍受 `max_allowed_packet` 和应用/协议资源限制约束；开发 MySQL 配置为 16 MiB，不代表应用可以安全承载任意大小的消息。

## 线上独立或托管 MySQL

使用独立部署文件，不启动本地 MySQL：

1. 在私有网络准备 MySQL 8.4、专用库、业务与迁移账号，限制数据库入站来源。
2. 业务账号只需要目标库 DML/SELECT；迁移账号限定该库 DDL，不授予全局 root 权限。
3. 获取数据库提供方 CA，证书名称必须匹配连接主机名。把外部连接参数放在未提交的 `.env.production` 或由密钥服务注入。
4. 先执行迁移，再启动/升级应用；迁移失败时停止部署，不让应用带着不匹配的 schema 启动。

`.env.production` 所需变量见 `.env.example` 底部，主要为：

```text
NETCHAT_DB_HOST=<数据库证书匹配的主机名>
NETCHAT_DB_PORT=3306
NETCHAT_DB_NAME=netchat
NETCHAT_DB_USER=netchat_app
NETCHAT_DB_PASSWORD=<运行账号密码>
NETCHAT_MIGRATION_USER=netchat_migrator
NETCHAT_MIGRATION_PASSWORD=<迁移账号密码>
NETCHAT_DB_CA_FILE=<宿主机CA文件绝对路径>
NETCHAT_HTTP_BIND=127.0.0.1
NETCHAT_HTTP_PORT=8081
```

```bash
docker compose --env-file .env.production -f compose.external.yml build
docker compose --env-file .env.production -f compose.external.yml run --rm migrate
# 只有上一条成功后才能执行下一条
docker compose --env-file .env.production -f compose.external.yml up -d netchat
```

该部署文件强制 `VERIFY_IDENTITY`。生产镜像建议在发布流水线中固定到验证过的镜像摘要，不依赖可变的 `mysql:8.4` 或本地 `netchat:mysql` 标签。`.env*`、私钥、数据库和上传文件都已排除出 Docker 构建上下文。

### Schema 管理

`server --migrate` 使用专用连接取得库级命名锁，运行版本化建表并记录版本。MySQL DDL 会隐式提交，**不能把整个迁移当成可回滚事务**；首次迁移的幂等建表允许中断后重跑，版本只在所有 DDL 成功后记录。异常会丢弃连接，防止命名锁残留在连接池里。新版本结构变更需新增迁移，不要手工修改版本记录冒充升级。

### 运维责任与仍需加固的部分

- 配置备份与 binlog/PITR、备份加密和保留策略，并定期验证实际恢复。
- 根据可用性目标选择托管故障切换/副本；监控连接池等待、查询延迟、慢查询、死锁、容量和错误率。
- 当前退出流程先关闭池的新租约，再停止接入并 join worker，最后释放连接及业务对象；在途操作受超时约束。
- `/api/health` 会访问数据库，失败返回 503。容器健康状态不等同于自动故障恢复，需由部署平台制定恢复策略。
- 数据库迁移不等于整个应用生产就绪。HTTP/WSS TLS、密码哈希升级、token 过期、消息幂等、限流、上传元数据持久化和多实例在线会话同步都需单独设计与验证。
- 当前每个账号只记录一个最新 WebSocket 连接；生产水平扩容不能仅启动多个进程而不处理会话路由。

## 隔离测试

```bash
npm --prefix client test
node server/tests/run-mysql-tests.mjs
```

MySQL 测试入口为每轮创建随机 Compose 项目和 `netchat_test_*` 库，密码写入临时 0600 文件。只清理本轮创建的项目资源，不复用 `.env` 开发密码，不运行全局 Docker 清理。

包括两次迁移幂等检查、运行账号 DDL 拒绝、C++ 数据库类型/事务/连接池测试，以及真实 HTTP/WebSocket 的注册、双向消息、历史、好友、并发注册、Unicode 长消息、重启持久化和只读账号写入失败测试。没有数据库或 Docker 权限时，不能把语法检查或 CTest 的 `Skipped` 当作 MySQL 测试通过。

原生 `database_test` 必须显式设置 `MYSQL_TEST_DATABASE` 及相关 `MYSQL_TEST_*` 连接变量；未设置时退出码 77，CTest 标记为跳过。协议脚本仅允许测试专用库，推荐始终通过隔离 runner 运行。
