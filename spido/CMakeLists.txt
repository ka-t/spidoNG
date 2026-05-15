cmake_minimum_required(VERSION 3.20)
project(spido CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(
    -Wall -Wextra -Wpedantic
    -O3 -march=native -flto
    -fno-omit-frame-pointer
)
add_link_options(-flto)

find_package(PkgConfig REQUIRED)
# Allow pointing at a locally-built liburing (≥2.5 enables multishot recv
# and the provided-buffer ring). Default falls back to system liburing.
# Auto-detect a sibling liburing install so users don't have to remember to
# set SPIDO_LIBURING_PREFIX / PKG_CONFIG_PATH on every clean build.
if(NOT DEFINED ENV{SPIDO_LIBURING_PREFIX})
    foreach(_cand
        "${CMAKE_CURRENT_SOURCE_DIR}/../liburing-prefix"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../liburing-prefix"
        "$ENV{HOME}/http/liburing-prefix"
        "/opt/liburing"
        "/usr/local")
        if(EXISTS "${_cand}/lib/pkgconfig/liburing.pc")
            set(ENV{SPIDO_LIBURING_PREFIX} "${_cand}")
            message(STATUS "spido: auto-detected liburing prefix at ${_cand}")
            break()
        endif()
    endforeach()
endif()
if(DEFINED ENV{SPIDO_LIBURING_PREFIX})
    set(ENV{PKG_CONFIG_PATH}
        "$ENV{SPIDO_LIBURING_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()
pkg_check_modules(URING REQUIRED liburing>=2.0)
if(URING_VERSION VERSION_GREATER_EQUAL 2.5)
    add_compile_definitions(SPIDO_HAVE_MULTISHOT=1 SPIDO_HAVE_BUFRING=1)
endif()
message(STATUS "spido: linking liburing ${URING_VERSION} from ${URING_LIBDIR}")

option(SPIDO_WITH_SSL "Build TLS support (requires OpenSSL)" OFF)
if(SPIDO_WITH_SSL)
    find_package(OpenSSL REQUIRED)
endif()

option(SPIDO_WITH_KTLS "Kernel TLS via OpenSSL handshake + setsockopt(SOL_TLS)" OFF)
if(SPIDO_WITH_KTLS)
    find_package(OpenSSL 3.0 REQUIRED)
endif()

option(SPIDO_WITH_JWT "JWT (HS256/RS256) bearer-token auth — needs OpenSSL" OFF)
if(SPIDO_WITH_JWT AND NOT TARGET OpenSSL::Crypto)
    find_package(OpenSSL 1.1 REQUIRED)
endif()

option(SPIDO_WITH_LUA "spido.lua config loader (needs liblua 5.3 or 5.4)" OFF)
set(SPIDO_LUA_PREFIX "" CACHE PATH "Override Lua install prefix")

option(SPIDO_WITH_XDP "Build AF_XDP kernel-bypass network layer (requires libbpf, clang)" OFF)

set(SPIDO_SOURCES
    src/server/io_uring_server.cpp
    src/server/http_parser.cpp
    src/server/router.cpp
    src/server/thread_pool.cpp
    src/server/response.cpp
    src/server/server.cpp
    src/server/access_log.cpp
    src/memory/pool_allocator.cpp
    src/utils/cpu_affinity.cpp
)
if(SPIDO_WITH_KTLS)
    list(APPEND SPIDO_SOURCES src/server/tls.cpp)
endif()
if(SPIDO_WITH_JWT)
    list(APPEND SPIDO_SOURCES src/server/jwt.cpp)
endif()
if(SPIDO_WITH_LUA)
    list(APPEND SPIDO_SOURCES src/server/lua_config.cpp)
endif()

if(SPIDO_WITH_XDP)
    pkg_check_modules(LIBBPF REQUIRED libbpf>=0.5)
    find_program(CLANG_EXECUTABLE clang REQUIRED)
    find_program(XXD_EXECUTABLE xxd REQUIRED)

    set(BPF_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/network/bpf/spido_xdp.bpf.c)
    set(BPF_OBJ ${CMAKE_CURRENT_BINARY_DIR}/spido_xdp.bpf.o)
    set(BPF_EMBED ${CMAKE_CURRENT_BINARY_DIR}/spido_xdp_bpf_embedded.c)

    # Compile the eBPF source to a BPF object.
    add_custom_command(
        OUTPUT  ${BPF_OBJ}
        COMMAND ${CLANG_EXECUTABLE} -O2 -g -Wall -Wno-unused-value
                -target bpf -D__TARGET_ARCH_x86
                -I/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}
                -c ${BPF_SRC} -o ${BPF_OBJ}
        DEPENDS ${BPF_SRC}
        VERBATIM)

    # Embed the BPF object as a C byte array. The shell wrapper is so
    # xxd's symbol naming uses the bare filename (spido_xdp_bpf_o) rather
    # than the full build path.
    add_custom_command(
        OUTPUT  ${BPF_EMBED}
        COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_BINARY_DIR}
                ${XXD_EXECUTABLE} -i spido_xdp.bpf.o ${BPF_EMBED}
        DEPENDS ${BPF_OBJ}
        VERBATIM)

    list(APPEND SPIDO_SOURCES
        src/network/umem.cpp
        src/network/xdp_socket.cpp
        src/network/xdp_loader.cpp
        src/network/xdp_engine.cpp
        src/network/xdp_program.cpp
        ${BPF_EMBED}
    )
endif()

add_library(spido STATIC ${SPIDO_SOURCES})
target_include_directories(spido
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
target_include_directories(spido SYSTEM PUBLIC ${URING_INCLUDE_DIRS})
target_link_libraries(spido PUBLIC ${URING_LIBRARIES} pthread)
target_link_directories(spido PUBLIC ${URING_LIBRARY_DIRS})

if(SPIDO_WITH_SSL)
    target_compile_definitions(spido PUBLIC SPIDO_WITH_SSL=1)
    target_link_libraries(spido PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

if(SPIDO_WITH_XDP)
    target_compile_definitions(spido PUBLIC SPIDO_WITH_XDP=1)
    target_include_directories(spido SYSTEM PUBLIC ${LIBBPF_INCLUDE_DIRS})
    target_link_libraries(spido PUBLIC ${LIBBPF_LIBRARIES})
    target_link_directories(spido PUBLIC ${LIBBPF_LIBRARY_DIRS})
endif()

if(SPIDO_WITH_KTLS)
    target_compile_definitions(spido PUBLIC SPIDO_WITH_KTLS=1)
    target_link_libraries(spido PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

if(SPIDO_WITH_JWT)
    target_compile_definitions(spido PUBLIC SPIDO_WITH_JWT=1)
    target_link_libraries(spido PUBLIC OpenSSL::Crypto)
endif()

if(SPIDO_WITH_LUA)
    target_compile_definitions(spido PUBLIC SPIDO_WITH_LUA=1)
    if(SPIDO_LUA_PREFIX)
        target_include_directories(spido SYSTEM PUBLIC "${SPIDO_LUA_PREFIX}/include")
        target_link_libraries(spido PUBLIC
            "${SPIDO_LUA_PREFIX}/lib/liblua.a"
            ${CMAKE_DL_LIBS} m)
    else()
        pkg_check_modules(LUA REQUIRED lua5.4 lua5.3)
        target_include_directories(spido SYSTEM PUBLIC ${LUA_INCLUDE_DIRS})
        target_link_libraries(spido PUBLIC ${LUA_LIBRARIES})
        target_link_directories(spido PUBLIC ${LUA_LIBRARY_DIRS})
    endif()
endif()

add_executable(spido-example src/main.cpp)
target_link_libraries(spido-example PRIVATE spido)

install(TARGETS spido ARCHIVE DESTINATION lib)
install(DIRECTORY include/spido DESTINATION include)

option(SPIDO_BUILD_TESTS "Build the spido unit-test runner" ON)
if(SPIDO_BUILD_TESTS)
    add_executable(spido-tests tests/test_parser.cpp)
    target_link_libraries(spido-tests PRIVATE spido)
    enable_testing()
    add_test(NAME parser COMMAND spido-tests)
endif()
