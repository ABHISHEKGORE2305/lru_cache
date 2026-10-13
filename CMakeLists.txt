cmake_minimum_required(VERSION 3.20)

project(SystemsLRUCache
    VERSION 1.0.0
    DESCRIPTION "High-throughput, sharded concurrent LRU cache for systems engineering"
    LANGUAGES CXX
)

# -----------------------------------------------------------------------------
# C++ Standard Configuration
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for language servers (clangd, clang-tidy)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# Project Options
# -----------------------------------------------------------------------------
option(BUILD_TESTS "Build comprehensive unit test suite" ON)
option(BUILD_BENCHMARKS "Build Google Benchmark performance suite" ON)
option(BUILD_EXAMPLES "Build example applications" ON)

# -----------------------------------------------------------------------------
# Compiler Warnings & Diagnostics
# -----------------------------------------------------------------------------
if(MSVC)
    set(SYSTEMS_CACHE_WARN_FLAGS /W4 /permissive-)
else()
    set(SYSTEMS_CACHE_WARN_FLAGS
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wnon-virtual-dtor
    )
endif()

# Threading support for multi-core synchronization
find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# Core Library Target: systems_cache (Static Library)
# -----------------------------------------------------------------------------
add_library(systems_cache STATIC
    src/lru_cache.cpp
    src/concurrent_lru_cache.cpp
)

target_include_directories(systems_cache
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(systems_cache PUBLIC cxx_std_20)
target_compile_options(systems_cache PRIVATE ${SYSTEMS_CACHE_WARN_FLAGS})
target_link_libraries(systems_cache PUBLIC Threads::Threads)

# -----------------------------------------------------------------------------
# External Dependencies Management via FetchContent
# -----------------------------------------------------------------------------
include(FetchContent)

# -----------------------------------------------------------------------------
# Unit Testing Target (Catch2 v3)
# -----------------------------------------------------------------------------
if(BUILD_TESTS)
    message(STATUS "Configuring Unit Tests with Catch2 v3...")
    include(CTest)
    enable_testing()

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.2
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(cache_tests
        tests/test_main.cpp
        tests/lru_cache_test.cpp
        tests/concurrent_cache_test.cpp
    )

    target_link_libraries(cache_tests
        PRIVATE
            systems_cache
            Catch2::Catch2WithMain
    )

    target_compile_options(cache_tests PRIVATE ${SYSTEMS_CACHE_WARN_FLAGS})

    add_test(NAME AllCacheTests COMMAND cache_tests)
endif()

# -----------------------------------------------------------------------------
# Performance Benchmarks Target (Google Benchmark)
# -----------------------------------------------------------------------------
if(BUILD_BENCHMARKS)
    message(STATUS "Configuring Performance Benchmarks with Google Benchmark...")
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        google_benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.8.3
    )
    FetchContent_MakeAvailable(google_benchmark)

    add_executable(cache_bench
        benchmarks/cache_bench.cpp
    )

    target_link_libraries(cache_bench
        PRIVATE
            systems_cache
            benchmark::benchmark
    )

    target_compile_options(cache_bench PRIVATE ${SYSTEMS_CACHE_WARN_FLAGS})
endif()

# -----------------------------------------------------------------------------
# Example Applications
# -----------------------------------------------------------------------------
if(BUILD_EXAMPLES)
    add_executable(edge_proxy_demo
        examples/edge_proxy_simulation.cpp
    )

    target_link_libraries(edge_proxy_demo
        PRIVATE
            systems_cache
    )

    target_compile_options(edge_proxy_demo PRIVATE ${SYSTEMS_CACHE_WARN_FLAGS})
endif()
