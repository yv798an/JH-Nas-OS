cmake_minimum_required(VERSION 3.16)
project(nas-web-cpp CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 单元测试默认开启：在 Ubuntu x86 上跑。
# 交叉编译上板时用 -DNAS_BUILD_TESTS=OFF（交叉工具链通常没有 TSan）。
option(NAS_BUILD_TESTS "Build unit tests" ON)
option(NAS_TSAN "Enable ThreadSanitizer for the test build" ON)

find_package(Threads REQUIRED)

# TSan 只在“本机测试”构建里启用（上板交叉编译 NAS_BUILD_TESTS=OFF 时不启用）。
# 关键：必须给**所有**参与链接的 target（含 nas_common 库）都加插桩，
# 否则 TSan 看不到库内部的加锁/同步，会产生 double-lock / data-race 误报。
set(NAS_USE_TSAN OFF)
if (NAS_BUILD_TESTS AND NAS_TSAN)
    set(NAS_USE_TSAN ON)
endif()

function(nas_add_tsan target)
    if (NAS_USE_TSAN)
        target_compile_options(${target} PRIVATE -fsanitize=thread)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
endfunction()

# 把 web/index.html 生成为内嵌资源头（HTML 独立成文件，但编译进二进制）。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" NAS_WEB_INDEX_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," NAS_WEB_INDEX_BYTES "${NAS_WEB_INDEX_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/web_index_generated.h"
"// 由 CMake 从 web/index.html 生成，请勿手改
#pragma once
namespace nas {
static const unsigned char kIndexHtml[] = {${NAS_WEB_INDEX_BYTES}0x00};
}
")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")

# 公共设施库：Logger 等。
add_library(nas_common STATIC
    src/common/Logger.cpp
)
target_include_directories(nas_common PUBLIC include)
target_compile_options(nas_common PRIVATE -Wall -Wextra)
target_link_libraries(nas_common PUBLIC Threads::Threads)
nas_add_tsan(nas_common)

# 配置层：/etc/nas/nas.conf 解析（优先级 CLI > 文件 > 默认）。
add_library(nas_config STATIC
    src/config/Config.cpp
)
target_include_directories(nas_config PUBLIC include)
target_compile_options(nas_config PRIVATE -Wall -Wextra)
nas_add_tsan(nas_config)

# 平台适配层：ISystemState 的 Linux 实现（非 Linux 平台编译为空）。
add_library(nas_platform STATIC
    src/platform/linux/LinuxSystemState.cpp
)
target_include_directories(nas_platform PUBLIC include PRIVATE src)
target_compile_options(nas_platform PRIVATE -Wall -Wextra)
target_link_libraries(nas_platform PUBLIC Threads::Threads)
nas_add_tsan(nas_platform)

# 系统状态消费层：SystemMonitor / ShareRegistry。
add_library(nas_system STATIC
    src/system/ShareRegistry.cpp
    src/system/SystemMonitor.cpp
)
target_include_directories(nas_system PUBLIC include)
target_compile_options(nas_system PRIVATE -Wall -Wextra)
target_link_libraries(nas_system PUBLIC nas_platform Threads::Threads)
nas_add_tsan(nas_system)

# 文件业务层：FileService（白名单 + 路径规范化 + 读写）。
add_library(nas_file STATIC
    src/file/FileService.cpp
)
target_include_directories(nas_file PUBLIC include)
target_compile_options(nas_file PRIVATE -Wall -Wextra)
nas_add_tsan(nas_file)

# io 层：BufferPool +（可选）io_uring 异步读。
# 交叉编译时在工具链 sysroot 内查找 liburing（优先静态库 .a，配合 -static）；
# 找不到则关闭 io_uring，BufferPool 仍构建。
option(NAS_WITH_IO_URING "Enable io_uring backend (needs liburing)" ON)
set(NAS_HAS_IO_URING OFF)
if (NAS_WITH_IO_URING)
    if (CMAKE_CROSSCOMPILING)
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-sysroot
            OUTPUT_VARIABLE NAS_SYSROOT OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        set(_uring_inc_hints "${NAS_SYSROOT}/usr/include" "${NAS_SYSROOT}/include")
        set(_uring_lib_hints "${NAS_SYSROOT}/usr/lib" "${NAS_SYSROOT}/usr/lib64"
                             "${NAS_SYSROOT}/lib" "${NAS_SYSROOT}/lib64")
        find_path(URING_INCLUDE_DIR NAMES liburing.h
            PATHS ${_uring_inc_hints} NO_DEFAULT_PATH)
        # 先找 liburing.a（静态链接需要），再退回 .so
        find_library(URING_LIBRARY NAMES liburing.a uring
            PATHS ${_uring_lib_hints} NO_DEFAULT_PATH)
    else()
        find_library(URING_LIBRARY NAMES uring)
        find_path(URING_INCLUDE_DIR NAMES liburing.h)
    endif()

    if (URING_LIBRARY AND URING_INCLUDE_DIR)
        set(NAS_HAS_IO_URING ON)
        message(STATUS "liburing found: io_uring enabled (${URING_LIBRARY})")
    else()
        message(STATUS "liburing not found: io_uring disabled (BufferPool still built)")
    endif()
endif()

add_library(nas_io STATIC
    src/io/BufferPool.cpp
)
target_include_directories(nas_io PUBLIC include)
target_compile_options(nas_io PRIVATE -Wall -Wextra)
target_link_libraries(nas_io PUBLIC Threads::Threads)
nas_add_tsan(nas_io)

if (NAS_HAS_IO_URING)
    target_sources(nas_io PRIVATE
        src/io/IoUring.cpp
        src/io/AsyncFileReader.cpp
    )
    target_compile_definitions(nas_io PUBLIC NAS_HAS_IO_URING=1)
    target_include_directories(nas_io PRIVATE ${URING_INCLUDE_DIR})
    target_link_libraries(nas_io PUBLIC ${URING_LIBRARY})

    # 基准仅在宿主机（非交叉）构建，交叉产物无法在本机运行。
    if (NOT CMAKE_CROSSCOMPILING)
        add_executable(io_bench bench/io_bench.cpp)
        target_link_libraries(io_bench PRIVATE nas_io)
        target_compile_options(io_bench PRIVATE -Wall -Wextra -O2)
        nas_add_tsan(io_bench)
    endif()
endif()

# 第三方：cpp-httplib（MIT，单头文件，已 vendor 到 third_party/）。
# SYSTEM 关掉对本第三方头文件的告警。
add_library(cpp_httplib INTERFACE)
target_include_directories(cpp_httplib SYSTEM INTERFACE third_party/cpp-httplib)

# HTTP 拉取压测客户端（bench/，仅宿主机）。
if (NOT CMAKE_CROSSCOMPILING)
    add_executable(http_bench bench/http_bench.cpp)
    target_link_libraries(http_bench PRIVATE cpp_httplib Threads::Threads)
    target_compile_options(http_bench PRIVATE -Wall -Wextra -O2)
    nas_add_tsan(http_bench)
endif()

# HTTP 层：抽象 + 路由器 + API + httplib 适配。
add_library(nas_http STATIC
    src/http/Router.cpp
    src/http/Api.cpp
    src/http/FileApi.cpp
    src/http/WebUi.cpp
    src/http/HttplibHttpServer.cpp
    src/http/HealthProbe.cpp
)
target_include_directories(nas_http PUBLIC include PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(nas_http PRIVATE -Wall -Wextra)
target_link_libraries(nas_http PUBLIC nas_common nas_system nas_file nas_io cpp_httplib Threads::Threads)
nas_add_tsan(nas_http)
if (WIN32)
    target_link_libraries(nas_http PUBLIC ws2_32)
endif()

add_executable(nas-server src/main.cpp)
target_link_libraries(nas-server PRIVATE
    nas_common nas_config nas_platform nas_system nas_http)
target_compile_options(nas-server PRIVATE -Wall -Wextra)
nas_add_tsan(nas-server)

if (NAS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
