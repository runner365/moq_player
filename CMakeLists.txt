# ============================================================
# moq_player — Media over QUIC 播放器
#
# 第 1 步（当前）：拉流层
#   upstream/moq_transport_client  (moq-transport draft-22 C SDK)
#     └─ webtransport_client       (QUIC / HTTP3 / WebTransport 客户端栈)
#         └─ libuv + OpenSSL
#
# 第 2 步：FFmpeg 解码 H.264 + AAC
# 第 3 步：SDL3 渲染与播放
# ============================================================
cmake_minimum_required(VERSION 3.16)
project(moq_player LANGUAGES C CXX)

# 本工程统一 C++14；C 侧按上游要求 C11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(MOQ_UPSTREAM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/upstream"
    CACHE PATH "moqlivecast 源码目录（默认指向 upstream submodule）")

if(NOT EXISTS "${MOQ_UPSTREAM_DIR}/moq_transport_client/src/moq_transport_client.c")
    message(FATAL_ERROR
        "upstream submodule 未初始化。请先执行：\n"
        "  git submodule update --init --recursive")
endif()

# ============================================================
# OpenSSL
#
# 上游默认用 ExternalProject 现编自带的 3.5.8（很慢）。这里优先用系统 OpenSSL，
# 前提是它带 QUIC TLS API（SSL_set_quic_tls_cbs，OpenSSL 3.5+）。
# ============================================================
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
    foreach(_cand /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
        if(EXISTS "${_cand}/include/openssl/ssl.h")
            set(OPENSSL_ROOT_DIR "${_cand}")
            break()
        endif()
    endforeach()
endif()

find_package(OpenSSL REQUIRED)

file(READ "${OPENSSL_INCLUDE_DIR}/openssl/ssl.h" _ssl_h)
if(NOT _ssl_h MATCHES "SSL_set_quic_tls_cbs")
    message(FATAL_ERROR
        "找到一个 OpenSSL（${OPENSSL_INCLUDE_DIR}），但它没有 QUIC TLS API。\n"
        "本工程需要 OpenSSL 3.5+。macOS 可执行：brew install openssl@3\n"
        "或显式指定：cmake -B build -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3")
endif()
message(STATUS "OpenSSL ${OPENSSL_VERSION} -> ${OPENSSL_INCLUDE_DIR}")

# ============================================================
# libuv（上游自带）
# ============================================================
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${MOQ_UPSTREAM_DIR}/3rdparty/libuv" libuv_build)

# ============================================================
# webtransport_client — WebTransport 客户端协议栈
#
# 注意：这里没有 add_subdirectory(upstream)，而是按上游 CMakeLists.txt 的
# 目标定义在本地重建。原因：上游多处硬编码 ${CMAKE_SOURCE_DIR}，一旦作为
# 子目录引入，该变量会指向本项目根目录，导致源文件路径全部失效
# （表现为 "No SOURCES given to target"）。用绝对路径重建可绕开该问题。
#
# 只包含客户端路径：不含 http3_server / webtransport_session（服务端）。
# ============================================================
set(WT_SOURCES
    "${MOQ_UPSTREAM_DIR}/http3/webtransport_client_api.c"
    "${MOQ_UPSTREAM_DIR}/http3/http3_frame.c"
    "${MOQ_UPSTREAM_DIR}/http3/qpack.c"
    "${MOQ_UPSTREAM_DIR}/http3/huffman.c"
    "${MOQ_UPSTREAM_DIR}/udp/udp_client.c"
    "${MOQ_UPSTREAM_DIR}/udp/udp_server.c"
    "${MOQ_UPSTREAM_DIR}/tls/tls_common.c"
    "${MOQ_UPSTREAM_DIR}/tls/tls_client.c"
    "${MOQ_UPSTREAM_DIR}/tls/tls_server.c"
    "${MOQ_UPSTREAM_DIR}/quic/quic_packet.c"
    "${MOQ_UPSTREAM_DIR}/quic/quic_crypto.c"
    "${MOQ_UPSTREAM_DIR}/quic/quic_connection.c"
    "${MOQ_UPSTREAM_DIR}/quic/quic_listener.c"
    "${MOQ_UPSTREAM_DIR}/quic/quic_stream.c"
    "${MOQ_UPSTREAM_DIR}/quic/quic_recovery.c"
    "${MOQ_UPSTREAM_DIR}/quic/quic_timer.c"
    "${MOQ_UPSTREAM_DIR}/quic/pacing_send.c"
    "${MOQ_UPSTREAM_DIR}/quic/quic_client_api.c"
    "${MOQ_UPSTREAM_DIR}/quic/congestion_control/quic_cc_newreno.c"
    "${MOQ_UPSTREAM_DIR}/quic/congestion_control/quic_cc_bbr.c"
    "${MOQ_UPSTREAM_DIR}/quic/congestion_control/quic_cc_cubic.c"
    "${MOQ_UPSTREAM_DIR}/logger/logger.c"
)

add_library(webtransport_client STATIC ${WT_SOURCES})

set_target_properties(webtransport_client PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(webtransport_client PUBLIC
    "${MOQ_UPSTREAM_DIR}/3rdparty/libuv/include"
    "${MOQ_UPSTREAM_DIR}/udp"
    "${MOQ_UPSTREAM_DIR}/tls"
    "${MOQ_UPSTREAM_DIR}/quic"
    "${MOQ_UPSTREAM_DIR}/quic/congestion_control"
    "${MOQ_UPSTREAM_DIR}/http3"
    "${MOQ_UPSTREAM_DIR}/logger"
)

# PUBLIC：链接方自动继承 uv_a / OpenSSL 和上面的 include 目录
target_link_libraries(webtransport_client PUBLIC
    uv_a
    OpenSSL::SSL
    OpenSSL::Crypto
)

# ============================================================
# moq_transport_client — moq-transport draft-22 C SDK
#
# 这个子目录只用到自己的 CMAKE_CURRENT_SOURCE_DIR，可以安全地用
# add_subdirectory 引入，保持与上游同步。
# ============================================================
add_subdirectory("${MOQ_UPSTREAM_DIR}/moq_transport_client" moq_transport_client_build)

# ============================================================
# FFmpeg（第 2 步：H.264 / AAC 解码 + avfilter 音频转换）
#
# 3rdparty/ffmpeg.tar.gz 是 autotools 工程（没有 CMakeLists），只能靠
# ExternalProject 驱动 ./configure && make。只编解码所需的最小集合：
#   h264/aac 解码器、h264 parser、avfilter(aresample/aformat)、swresample
# ============================================================
include(ExternalProject)

set(FFMPEG_TARBALL     "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ffmpeg.tar.gz")
set(FFMPEG_PREFIX      "${CMAKE_BINARY_DIR}/ffmpeg")
set(FFMPEG_SRC_DIR     "${FFMPEG_PREFIX}/src")
set(FFMPEG_INSTALL_DIR "${FFMPEG_PREFIX}/install")

if(NOT EXISTS "${FFMPEG_TARBALL}")
    message(FATAL_ERROR "缺少 FFmpeg 源码包：${FFMPEG_TARBALL}")
endif()

# 并行度在 configure 阶段算好，避免 BUILD_COMMAND 里嵌套 shell 子命令
include(ProcessorCount)
ProcessorCount(FFMPEG_NPROC)
if(FFMPEG_NPROC EQUAL 0)
    set(FFMPEG_NPROC 4)
endif()

ExternalProject_Add(ffmpeg_ext
    URL                    "${FFMPEG_TARBALL}"
    SOURCE_DIR             "${FFMPEG_SRC_DIR}"
    CONFIGURE_COMMAND      "${FFMPEG_SRC_DIR}/configure"
                               --prefix=${FFMPEG_INSTALL_DIR}
                               --disable-programs
                               --disable-doc
                               --disable-network
                               --disable-everything
                               --enable-decoder=h264,aac
                               --enable-parser=h264
                               --enable-avfilter
                               --enable-filter=aresample,aformat,anull
                               --enable-swresample
                               --disable-shared
                               --enable-static
    BUILD_COMMAND          ${CMAKE_MAKE_PROGRAM} -j${FFMPEG_NPROC}
    INSTALL_COMMAND        ${CMAKE_MAKE_PROGRAM} install
    BUILD_IN_SOURCE        OFF
    UPDATE_DISCONNECTED    ON
    LOG_CONFIGURE          ON
    LOG_BUILD              ON
    LOG_INSTALL            ON
    BUILD_BYPRODUCTS
        "${FFMPEG_INSTALL_DIR}/lib/libavcodec.a"
        "${FFMPEG_INSTALL_DIR}/lib/libavfilter.a"
        "${FFMPEG_INSTALL_DIR}/lib/libswresample.a"
        "${FFMPEG_INSTALL_DIR}/lib/libswscale.a"
        "${FFMPEG_INSTALL_DIR}/lib/libavutil.a"
)

# 头文件目录需要提前存在，否则 CMake 会因 IMPORTED target 的路径不存在而报错
file(MAKE_DIRECTORY "${FFMPEG_INSTALL_DIR}/include")

# ---- IMPORTED 静态库 ----
foreach(_lib avcodec avfilter swresample swscale avutil)
    add_library(ff_${_lib} STATIC IMPORTED GLOBAL)
    set_target_properties(ff_${_lib} PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_INSTALL_DIR}/lib/lib${_lib}.a"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INSTALL_DIR}/include"
    )
    add_dependencies(ff_${_lib} ffmpeg_ext)
endforeach()

# FFmpeg 静态库的链接依赖：avfilter 依赖 avcodec/swresample/swscale，
# avcodec 依赖 swresample/avutil，顺序不能乱。
target_link_libraries(ff_avfilter INTERFACE
    ff_avcodec ff_swresample ff_swscale ff_avutil)
target_link_libraries(ff_avcodec INTERFACE
    ff_swresample ff_swscale ff_avutil)
target_link_libraries(ff_swresample INTERFACE ff_avutil)
target_link_libraries(ff_swscale INTERFACE ff_avutil)

# macOS 上 avutil 静态库引用了系统框架符号
if(APPLE)
    target_link_libraries(ff_avutil INTERFACE
        "-framework VideoToolbox"
        "-framework CoreFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreServices"
        "-liconv"
    )
endif()
target_link_libraries(ff_avutil INTERFACE z m pthread)

# ============================================================
# SDL3（第 3 步：渲染与播放）
#
# 3rdparty/SDL3-3.4.20.tar.gz 是标准 CMake 工程，解压后 add_subdirectory
# 即可。构建静态库，避免运行时依赖 dylib 路径。
# ============================================================
set(SDL3_TARBALL "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/SDL3-3.4.20.tar.gz")
set(SDL3_EXTRACT_DIR "${CMAKE_BINARY_DIR}/sdl3-src")
set(SDL3_SRC_DIR "${SDL3_EXTRACT_DIR}/SDL3-3.4.20")

if(NOT EXISTS "${SDL3_TARBALL}")
    message(FATAL_ERROR "缺少 SDL3 源码包：${SDL3_TARBALL}")
endif()

if(NOT EXISTS "${SDL3_SRC_DIR}/CMakeLists.txt")
    message(STATUS "解压 SDL3 源码包...")
    file(MAKE_DIRECTORY "${SDL3_EXTRACT_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${SDL3_TARBALL}" DESTINATION "${SDL3_EXTRACT_DIR}")
endif()

set(SDL_SHARED       OFF CACHE BOOL "" FORCE)
set(SDL_STATIC       ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS        OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL      OFF CACHE BOOL "" FORCE)

add_subdirectory("${SDL3_SRC_DIR}" sdl3_build)

# ============================================================
# moq_player
# ============================================================
add_executable(moq_player moq_player.cpp moq_decoder.cpp moq_render.cpp)

target_link_libraries(moq_player PRIVATE
    moq_transport_client
    ff_avfilter          # INTERFACE 链已带上 avcodec/swresample/swscale/avutil
    SDL3::SDL3
)

# 日志统一走 moqlivecast 的 logger（logger.c 已随 webtransport_client 编译进
# 静态库；这里只补头文件路径，保证 #include "logger.h" 可用）
target_include_directories(moq_player PRIVATE
    "${MOQ_UPSTREAM_DIR}/logger"
)

set_target_properties(moq_player PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
