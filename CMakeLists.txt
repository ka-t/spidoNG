# SpidoNG — config.json → production C++ REST backend
#
# Top-level build glues two static libraries together:
#   spido      — io_uring HTTP server (TLS, JWT, AF_XDP, kTLS)
#   spido_pg   — PostgreSQL client + tiered cache + batch writer +
#                WAL + entity cache + pressure controller
# plus optional test / example / benchmark targets.
#
# Quick build:
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j$(nproc)
#
# After building, the Python generator wires this stack against a config:
#   python3 generator/generator.py examples/ecommerce/config.json -o out/

cmake_minimum_required(VERSION 3.16)
project(SpidoNG CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Options forwarded into the libraries.
# ---------------------------------------------------------------------------
option(SPIDONG_WITH_TESTS      "Build doctest unit tests"            ON)
option(SPIDONG_WITH_EXAMPLES   "Build example/main.cpp's"            ON)
option(SPIDONG_WITH_BENCHMARKS "Build the bench_burst micro-bench"   ON)
option(SPIDONG_ASAN            "Build with AddressSanitizer"         OFF)
option(SPIDONG_WERROR          "Treat warnings as errors"            ON)

# The libraries each accept their own option names. Translate ours so
# the child CMakeLists don't need to learn the parent's vocabulary.
set(SPIDO_WITH_TESTS    ${SPIDONG_WITH_TESTS}      CACHE BOOL "" FORCE)
set(SPIDO_PG_WITH_TESTS ${SPIDONG_WITH_TESTS}      CACHE BOOL "" FORCE)
set(SPIDO_PG_WITH_EXAMPLES   ${SPIDONG_WITH_EXAMPLES}   CACHE BOOL "" FORCE)
set(SPIDO_PG_WITH_BENCHMARKS ${SPIDONG_WITH_BENCHMARKS} CACHE BOOL "" FORCE)
set(SPIDO_PG_ASAN   ${SPIDONG_ASAN}   CACHE BOOL "" FORCE)
set(SPIDO_PG_WERROR ${SPIDONG_WERROR} CACHE BOOL "" FORCE)

# The spido-pg example links against libspido.a, which spido produces in
# its own build dir. Point spido-pg at the spido subtree explicitly so
# its find_library() resolves without environment fiddling.
set(SPIDO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spido" CACHE PATH
    "Path to the spido source tree (header + built libspido.a)")

# ---------------------------------------------------------------------------
# Build order matters: spido first (defines libspido.a), then spido-pg
# (which links it for the example binary).
# ---------------------------------------------------------------------------
add_subdirectory(spido)
add_subdirectory(spido-pg)

# ---------------------------------------------------------------------------
# Generator pytest wiring — `cmake --build . --target generator_tests` runs
# `pytest tests/test_generator.py` against the in-tree generator. CI-friendly.
# ---------------------------------------------------------------------------
find_program(PYTEST_EXE pytest)
if(PYTEST_EXE)
  add_custom_target(generator_tests
    COMMAND ${PYTEST_EXE} -q ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_generator.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running generator regression tests")
endif()

message(STATUS "SpidoNG build configured:")
message(STATUS "  build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests          : ${SPIDONG_WITH_TESTS}")
message(STATUS "  examples       : ${SPIDONG_WITH_EXAMPLES}")
message(STATUS "  benchmarks     : ${SPIDONG_WITH_BENCHMARKS}")
message(STATUS "  asan / werror  : ${SPIDONG_ASAN} / ${SPIDONG_WERROR}")
