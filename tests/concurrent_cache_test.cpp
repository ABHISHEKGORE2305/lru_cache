#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "systems/cache/concurrent_lru_cache.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using systems::cache::ConcurrentLRUCache;

TEST_CASE("ConcurrentLRUCache: Basic Functional Verification", "[ConcurrentLRU]") {
    ConcurrentLRUCache<std::string, std::string> cache(100, 8);

    REQUIRE(cache.capacity() >= 100);
    REQUIRE(cache.shard_count() == 8);
    REQUIRE(cache.size() == 0);

    SECTION("Put, Get, Peek, Contains") {
        cache.put("user_101", "session_abc");
        REQUIRE(cache.size() == 1);
        REQUIRE(cache.contains("user_101"));

        auto peek_val = cache.peek("user_101");
        REQUIRE(peek_val.has_value());
        REQUIRE(*peek_val == "session_abc");

        auto get_val = cache.get("user_101");
        REQUIRE(get_val.has_value());
        REQUIRE(*get_val == "session_abc");

        REQUIRE(cache.remove("user_101"));
        REQUIRE(cache.size() == 0);
        REQUIRE_FALSE(cache.contains("user_101"));
    }
}

TEST_CASE("ConcurrentLRUCache: Multi-Threaded Stress & Contention", "[ConcurrentLRU]") {
    const size_t total_capacity = 1000;
    const size_t shard_count = 16;
    ConcurrentLRUCache<int, int> cache(total_capacity, shard_count);

    const int num_threads = 8;
    const int ops_per_thread = 5000;

    SECTION("Concurrent Writes and Reads without Race Conditions") {
        std::vector<std::thread> workers;
        workers.reserve(num_threads);

        std::atomic<bool> start_flag{false};

        for (int t = 0; t < num_threads; ++t) {
            workers.emplace_back([&, t]() {
                // Wait for coordinated start
                while (!start_flag.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                for (int i = 0; i < ops_per_thread; ++i) {
                    const int key = (t * 1000) + (i % 200);
                    cache.put(key, i);

                    auto val = cache.get(key);
                    if (val.has_value()) {
                        CHECK(*val >= 0);
                    }
                }
            });
        }

        start_flag.store(true, std::memory_order_release);

        for (auto& w : workers) {
            w.join();
        }

        REQUIRE(cache.size() <= cache.capacity());
        REQUIRE(cache.stats().hits > 0);
    }

    SECTION("Concurrent Readers with Single Writer") {
        std::atomic<bool> running{true};
        std::vector<std::thread> readers;
        const int reader_count = 6;
        readers.reserve(reader_count);

        // Prepopulate
        for (int i = 0; i < 200; ++i) {
            cache.put(i, i * 10);
        }

        // Reader threads using peek and contains (shared lock)
        for (int r = 0; r < reader_count; ++r) {
            readers.emplace_back([&]() {
                int local_key = 0;
                while (running.load(std::memory_order_relaxed)) {
                    cache.peek(local_key % 200);
                    cache.contains((local_key + 1) % 200);
                    ++local_key;
                }
            });
        }

        // Writer thread concurrently mutating keys (unique lock)
        std::thread writer([&]() {
            for (int w = 0; w < 3000; ++w) {
                cache.put(w % 200, w);
            }
        });

        writer.join();
        running.store(false, std::memory_order_relaxed);

        for (auto& r : readers) {
            r.join();
        }

        REQUIRE(cache.size() <= cache.capacity());
    }
}
