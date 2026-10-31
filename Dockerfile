# MySQL 客户端构建环境；不需要在宿主机安装开发库。
FROM ubuntu:24.04 AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config libssl-dev libmysqlclient-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY server/ ./server/
RUN cmake -S server -B out -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    && cmake --build out -j2

# 原生数据库测试镜像，与服务共用同一适配层。
FROM builder AS db-test
ENTRYPOINT ["/build/out/database_test"]

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3t64 libmysqlclient21 curl ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -r netchat \
    && useradd -r -g netchat -d /app -s /usr/sbin/nologin netchat
WORKDIR /app
COPY --from=builder /build/out/server ./server
COPY client/dist/ ./dist/
COPY server/config.json ./config.json
RUN mkdir -p /app/uploads /app/run && chown -R netchat:netchat /app
USER netchat
WORKDIR /app/run
ENV NETCHAT_CONFIG_PATH=/app/config.json NETCHAT_STATIC_DIR=/app/dist
EXPOSE 8081
ENTRYPOINT ["/app/server"]

# 协议回归测试专用；测试脚本不能放进 /app/server（二进制文件）目录。
FROM runtime AS protocol-test
COPY --from=node:22-bookworm-slim /usr/local/bin/node /usr/local/bin/node
COPY server/tests/chat-protocol.mjs /app/tests/server/chat-protocol.mjs
ENTRYPOINT ["node", "/app/tests/server/chat-protocol.mjs"]

# 默认 docker build 产物仍然是服务，不携带 Node 或测试脚本。
FROM runtime AS production
