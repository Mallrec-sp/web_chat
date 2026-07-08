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
)

# ===== 可执行目标 =====
add_executable(server ${SOURCES})

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
