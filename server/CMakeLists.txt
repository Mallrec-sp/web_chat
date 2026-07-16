cmake_minimum_required(VERSION 3.16)
project(server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ===== 源文件列表 =====
set(SOURCES
    src/main.cpp
    src/utils/log.cpp
    src/utils/config.cpp
    src/utils/auth.cpp
    src/core/inetaddress.cpp
    src/core/socket.cpp
    src/core/channel.cpp
    src/core/eventloop.cpp
    src/core/buffer.cpp
    src/core/acceptor.cpp
    src/core/tcp_connection.cpp
    src/core/server.cpp
    src/core/eventloop_thread.cpp
    src/core/eventloop_thread_pool.cpp
    src/core/timer_queue.cpp
    src/core/signal_handler.cpp
    src/core/ssl_context.cpp
    src/protocol/websocket.cpp
    src/service/session_mgr.cpp
    src/db/database.cpp
    src/db/migrations.cpp
)

# ===== 可执行目标 =====
add_executable(server ${SOURCES})

# ===== 链接 pthread =====
find_package(Threads REQUIRED)
target_link_libraries(server PRIVATE Threads::Threads)

# ===== 链接 OpenSSL (SHA1, Base64) =====
find_package(OpenSSL REQUIRED)
target_link_libraries(server PRIVATE OpenSSL::Crypto OpenSSL::SSL)

# ===== 链接 SQLite3 =====
find_package(SQLite3 REQUIRED)
target_link_libraries(server PRIVATE SQLite::SQLite3)

# ===== 头文件路径 =====
target_include_directories(server PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>
    $<INSTALL_INTERFACE:include>
)

# ===== 编译选项 =====
target_compile_options(server PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:RELEASE>:-O3 -march=native -DNDEBUG>
    $<$<CONFIG:DEBUG>:-g -O0 -fsanitize=address -fno-omit-frame-pointer>
)

target_link_options(server PRIVATE
    $<$<CONFIG:DEBUG>:-fsanitize=address>
)

# ===== 第三方库 (后续 Phase 按需开启) =====
# find_package(SQLite3 REQUIRED)
# target_link_libraries(netchat_server PRIVATE SQLite::SQLite)
# find_package(OpenSSL REQUIRED)
# target_link_libraries(netchat_server PRIVATE OpenSSL::SSL OpenSSL::Crypto)
