# ============================================
# NetChat Docker 化 — 多阶段构建
# ============================================
# 使用方式：
#   docker compose build
#   docker compose up -d
# ============================================

# ---- Stage 1: 编译 C++ 后端 ----
FROM ubuntu:22.04 AS builder

LABEL stage=builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    libssl-dev \
    libsqlite3-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# 先复制 CMakeLists.txt 和第三方库（利用 Docker 缓存）
COPY server/CMakeLists.txt server/third_party/ ./server/
COPY server/third_party/ ./server/third_party/

# 复制所有源码
COPY server/src ./server/src

# 编译
RUN cd server && mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc)

# ---- Stage 2: 运行环境 ----
FROM ubuntu:22.04 AS runtime

# 安装运行时依赖（OpenSSL、SQLite3）
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libsqlite3-0 \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# 创建非 root 用户
RUN groupadd -r netchat && useradd -r -g netchat -d /app -s /sbin/nologin netchat

WORKDIR /app

# 从构建阶段复制编译好的二进制
COPY --from=builder /build/server/build/server .

# 复制前端静态文件
COPY client/dist ./dist/

# 复制配置文件
COPY server/config.json .

# 创建数据目录
RUN mkdir -p /app/data && chown -R netchat:netchat /app

USER netchat

EXPOSE 8080

# 启动服务器
CMD ["./server"]
