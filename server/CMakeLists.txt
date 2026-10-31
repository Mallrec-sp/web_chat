cmake_minimum_required(VERSION 3.16)
project(server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED IMPORTED_TARGET mysqlclient)

# 数据库实现只编译一次，服务和原生集成测试共用。
add_library(netchat_db STATIC src/db/database.cpp)
target_include_directories(netchat_db PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(netchat_db PUBLIC PkgConfig::MYSQL Threads::Threads)

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
    src/db/migrations.cpp
)

add_executable(server ${SOURCES})
target_link_libraries(server PRIVATE netchat_db OpenSSL::Crypto OpenSSL::SSL)
target_include_directories(server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

foreach(target server netchat_db)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<CONFIG:Release>:-O3;-DNDEBUG>
        $<$<CONFIG:Debug>:-g;-O0;-fsanitize=address;-fno-omit-frame-pointer>
    )
    target_link_options(${target} PUBLIC $<$<CONFIG:Debug>:-fsanitize=address>)
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_executable(database_test tests/database_test.cpp)
    target_link_libraries(database_test PRIVATE netchat_db)
    add_test(NAME database_test COMMAND database_test)
    set_tests_properties(database_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()
