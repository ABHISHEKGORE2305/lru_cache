#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "systems/cache/lru_cache.hpp"

#include <chrono>
#include <string>
#include <thread>

using systems::cache::LRUCache;

TEST_CASE("LRUCache: Basic Operations and Value Retrieval", "[LRUCache]") {
    LRUCache<std::string, int> cache(3);

    REQUIRE(cache.capacity() == 3);
    REQUIRE(cache.size() == 0);
    REQUIRE(cache.empty());

    SECTION("Put and Get single item") {
        REQUIRE_FALSE(cache.put("alpha", 100)); // new insert returns false
        REQUIRE(cache.size() == 1);
        REQUIRE_FALSE(cache.empty());

        auto val = cache.get("alpha");
        REQUIRE(val.has_value());
        REQUIRE(*val == 100);

        // Telemetry check
        REQUIRE(cache.stats().hits == 1);
        REQUIRE(cache.stats().misses == 0);
    }

    SECTION("Get non-existent key returns nullopt") {
        auto val = cache.get("unknown");
        REQUIRE_FALSE(val.has_value());
        REQUIRE(cache.stats().misses == 1);
    }

    SECTION("Put existing key updates value and returns true") {
        cache.put("alpha", 100);
        REQUIRE(cache.put("alpha", 200)); // update returns true
        REQUIRE(cache.size() == 1);

        auto val = cache.get("alpha");
        REQUIRE(val.has_value());
        REQUIRE(*val == 200);
    }
}

TEST_CASE("LRUCache: O(1) Eviction Lifecycle and Recency Ordering", "[LRUCache]") {
    LRUCache<int, std::string> cache(3);

    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");
    REQUIRE(cache.size() == 3);

    SECTION("Evicts Least Recently Used when capacity is exceeded") {
        // Insert key 4 -> key 1 is LRU and should be evicted
        cache.put(4, "four");
        REQUIRE(cache.size() == 3);
        REQUIRE(cache.stats().evictions == 1);

        REQUIRE_FALSE(cache.get(1).has_value()); // evicted
        REQUIRE(cache.get(2).has_value());
        REQUIRE(cache.get(3).has_value());
        REQUIRE(cache.get(4).has_value());
    }

    SECTION("GET access promotes item to Most Recently Used") {
        // Access key 1 to promote it to MRU
        REQUIRE(cache.get(1).has_value());

        // Now LRU is key 2! Inserting key 4 should evict key 2, NOT key 1
        cache.put(4, "four");
        REQUIRE(cache.size() == 3);

        REQUIRE(cache.get(1).has_value()); // preserved because it was accessed!
        REQUIRE_FALSE(cache.get(2).has_value()); // evicted
        REQUIRE(cache.get(3).has_value());
        REQUIRE(cache.get(4).has_value());
    }

    SECTION("PUT update promotes existing item to Most Recently Used") {
        // Updating key 1 promotes it to MRU
        cache.put(1, "one_updated");

        // Now LRU is key 2. Inserting key 4 evicts key 2
        cache.put(4, "four");
        REQUIRE_FALSE(cache.get(2).has_value());
        REQUIRE(cache.get(1).has_value());
        REQUIRE(*cache.get(1) == "one_updated");
    }
}

TEST_CASE("LRUCache: Time-To-Live (TTL) Expiration", "[LRUCache][TTL]") {
    LRUCache<std::string, std::string> cache(5);

    SECTION("Item expires after TTL duration") {
        cache.put("session_token", "jwt_abc123", std::chrono::milliseconds(50));
        REQUIRE(cache.contains("session_token"));

        auto val_immediate = cache.get("session_token");
        REQUIRE(val_immediate.has_value());
        REQUIRE(*val_immediate == "jwt_abc123");

        // Wait for expiration
        std::this_thread::sleep_for(std::chrono::milliseconds(60));

        // Lazy eviction on get()
        auto val_expired = cache.get("session_token");
        REQUIRE_FALSE(val_expired.has_value());
        REQUIRE_FALSE(cache.contains("session_token"));
        REQUIRE(cache.stats().expirations == 1);
        REQUIRE(cache.size() == 0);
    }

    SECTION("Proactive eviction of expired entries") {
        cache.put("item1", "v1", std::chrono::milliseconds(30));
        cache.put("item2", "v2", std::chrono::milliseconds(200));
        cache.put("item3", "v3"); // no expiration

        REQUIRE(cache.size() == 3);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        size_t purged = cache.evict_expired();
        REQUIRE(purged == 1);
        REQUIRE(cache.size() == 2);
        REQUIRE_FALSE(cache.contains("item1"));
        REQUIRE(cache.contains("item2"));
        REQUIRE(cache.contains("item3"));
    }
}

TEST_CASE("LRUCache: Get Or Compute (Cache-Aside)", "[LRUCache][Compute]") {
    LRUCache<std::string, int> cache(5);
    int compute_invocations = 0;

    auto compute_fn = [&]() {
        ++compute_invocations;
        return 42;
    };

    // First call computes
    int v1 = cache.get_or_compute("expensive_key", compute_fn);
    REQUIRE(v1 == 42);
    REQUIRE(compute_invocations == 1);

    // Second call serves from cache without recomputing
    int v2 = cache.get_or_compute("expensive_key", compute_fn);
    REQUIRE(v2 == 42);
    REQUIRE(compute_invocations == 1);
}

TEST_CASE("LRUCache: Peek and Removal Semantics", "[LRUCache]") {
    LRUCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20);

    SECTION("Peek does not alter LRU order") {
        auto val = cache.peek(1);
        REQUIRE(val.has_value());
        REQUIRE(*val == 10);

        // Inserting key 3 should evict key 1 (not key 2)
        cache.put(3, 30);
        REQUIRE_FALSE(cache.contains(1)); // evicted
        REQUIRE(cache.contains(2));
        REQUIRE(cache.contains(3));
    }

    SECTION("Explicit key removal") {
        REQUIRE(cache.remove(1));
        REQUIRE(cache.size() == 1);
        REQUIRE_FALSE(cache.contains(1));
        REQUIRE_FALSE(cache.remove(1));
    }

    SECTION("Clear resets everything") {
        cache.clear();
        REQUIRE(cache.size() == 0);
        REQUIRE(cache.empty());
        REQUIRE_FALSE(cache.contains(1));
        REQUIRE_FALSE(cache.contains(2));
    }
}

TEST_CASE("LRUCache: Invalid capacity throws exception", "[LRUCache]") {
    REQUIRE_THROWS_AS(LRUCache<int, int>(0), std::invalid_argument);
}
