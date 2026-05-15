cmake_minimum_required(VERSION 3.16)
project(spido_pg CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SPIDO_PG_WITH_TESTS      "Build unit tests"     ON)
option(SPIDO_PG_WITH_EXAMPLES   "Build example/main.cpp" ON)
option(SPIDO_PG_WITH_BENCHMARKS "Build the burst-isolation benchmark" ON)
option(SPIDO_PG_ASAN          "Build with AddressSanitizer" OFF)

# Strict by default — Werror is request from the spec. Tune locally with
# -DSPIDO_PG_WERROR=OFF if compiler diagnostics on your particular libc/uring
# version are noisier than usual.
option(SPIDO_PG_WERROR        "Treat warnings as errors" ON)

set(SPIDO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
    "Path to libspido (containing include/spido and libspido.a)")

add_compile_options(-Wall -Wextra)
if(SPIDO_PG_WERROR)
  add_compile_options(-Werror)
endif()

# Link-time optimization. Lets the cache hot path inline into the
# benchmark / generated-handler call sites; matters because cache.cpp and
# the consumer TUs are separately compiled. Disabled under ASan since the
# combination tends to hide stack traces in some toolchain versions.
if(NOT SPIDO_PG_ASAN AND CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
  if(_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    message(STATUS "spido_pg: LTO enabled")
  endif()
endif()

if(SPIDO_PG_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address)
endif()

find_package(PkgConfig REQUIRED)
# Auto-detect a sibling liburing install. spido needs ≥ 2.5 for multishot
# recv and provided-buffer rings; system liburing 2.0 lacks those symbols, so
# falling back silently would explode at link time. Search well-known
# locations and prepend the one that ships a pkgconfig file.
if(NOT DEFINED ENV{SPIDO_LIBURING_PREFIX})
    foreach(_cand
        "${CMAKE_CURRENT_SOURCE_DIR}/../liburing-prefix"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../liburing-prefix"
        "$ENV{HOME}/http/liburing-prefix"
        "/opt/liburing"
        "/usr/local")
        if(EXISTS "${_cand}/lib/pkgconfig/liburing.pc")
            set(ENV{SPIDO_LIBURING_PREFIX} "${_cand}")
            message(STATUS "spido_pg: auto-detected liburing prefix at ${_cand}")
            break()
        endif()
    endforeach()
endif()
if(DEFINED ENV{SPIDO_LIBURING_PREFIX})
    set(ENV{PKG_CONFIG_PATH}
        "$ENV{SPIDO_LIBURING_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()
pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

set(SPIDO_PG_SRCS
  src/connection.cpp
  src/pool.cpp
  src/cache.cpp
  src/batch_writer.cpp
  src/notify.cpp
  src/db.cpp
  src/endpoint_registry.cpp
  src/token_bucket.cpp
  src/pressure_controller.cpp
  src/wal.cpp
  src/entity_cache.cpp
)

add_library(spido_pg STATIC ${SPIDO_PG_SRCS})
target_include_directories(spido_pg PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(spido_pg PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${URING_INCLUDE_DIRS})
target_link_libraries(spido_pg PUBLIC
  PkgConfig::URING
  Threads::Threads)
# SCRAM-SHA-256 + cache key hashing both want libcrypto. We link PUBLIC
# so consumers (the generator-emitted service) don't have to know about
# this transitively.
target_link_libraries(spido_pg PUBLIC
  OpenSSL::Crypto)

# libspido — only needed to LINK the example. The static library itself
# doesn't depend on libspido (the HTTP server lives in user code).
#
# Two modes:
#   1. SpidoNG umbrella build (add_subdirectory(spido) happened in a
#      parent CMakeLists): the `spido` target already exists, link to it
#      directly. No file lookup needed.
#   2. Standalone build of spido-pg: probe for a pre-built libspido.a in
#      conventional spots so users can still build spido-pg in isolation.
if(TARGET spido)
  set(SPIDO_LINK_TARGET spido)
else()
  find_library(SPIDO_LIB NAMES spido
    PATHS ${SPIDO_DIR} ${SPIDO_DIR}/build "${SPIDO_DIR}/spido-pg"
    NO_DEFAULT_PATH)
  set(SPIDO_LINK_TARGET ${SPIDO_LIB})
endif()

if(SPIDO_PG_WITH_EXAMPLES AND SPIDO_LINK_TARGET)
  add_executable(spido_pg_example example/main.cpp)
  if(TARGET spido)
    # spido target carries its own include directories transitively.
    target_link_libraries(spido_pg_example PRIVATE spido_pg spido
      OpenSSL::SSL OpenSSL::Crypto)
  else()
    target_include_directories(spido_pg_example PRIVATE
      ${SPIDO_DIR}/spido/include
      ${SPIDO_DIR}/include)
    target_link_libraries(spido_pg_example PRIVATE
      spido_pg
      ${SPIDO_LINK_TARGET}
      OpenSSL::SSL OpenSSL::Crypto)
  endif()
endif()

if(SPIDO_PG_WITH_BENCHMARKS)
  add_executable(bench_burst benchmarks/bench_burst.cpp)
  target_link_libraries(bench_burst PRIVATE spido_pg)
  target_include_directories(bench_burst PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()

if(SPIDO_PG_WITH_TESTS)
  include(FetchContent)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11)
  # doctest v2.4.11 declares cmake_minimum_required(VERSION 2.8.12), which
  # CMake 4.x refuses. Bump the floor for nested projects so the policy is
  # satisfied without forking doctest.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
  FetchContent_MakeAvailable(doctest)

  enable_testing()
  set(TEST_BINS
    test_connection test_pool test_cache test_batch_writer
    test_endpoint_registry test_token_bucket test_pressure_controller
    test_wal test_wal_crash test_entity_cache
  )
  foreach(t ${TEST_BINS})
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE spido_pg doctest::doctest)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
endif()
