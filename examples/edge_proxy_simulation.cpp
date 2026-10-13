#include "systems/cache/concurrent_lru_cache.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main() {
    std::cout << "=========================================================\n";
    std::cout << "   Edge Proxy Concurrent LRU Cache Simulation Demo       \n";
    std::cout << "=========================================================\n\n";

    // Initialize concurrent cache: 10,000 capacity partitioned across 16 shards
    const size_t cache_capacity = 10'000;
    const size_t shard_count = 16;
    systems::cache::ConcurrentLRUCache<std::string, std::string> session_cache(cache_capacity, shard_count);

    std::cout << "[Init] Cache initialized with capacity: " << session_cache.capacity()
              << " across " << session_cache.shard_count() << " independent shards.\n\n";

    // 1. Basic put and get with TTL (Time-To-Live)
    std::cout << "[1] Inserting session token with 100ms TTL...\n";
    session_cache.put("session:usr_9981", "jwt_token_payload_xyz", 100ms);

    if (auto token = session_cache.get("session:usr_9981")) {
        std::cout << "    Immediate retrieval success: " << *token << "\n";
    }

    std::cout << "    Sleeping for 120ms to simulate token expiration...\n";
    std::this_thread::sleep_for(120ms);

    if (auto token = session_cache.get("session:usr_9981")) {
        std::cout << "    Token unexpectedly found: " << *token << "\n";
    } else {
        std::cout << "    TTL expired: Token lazily evicted as expected!\n\n";
    }

    // 2. Cache-Aside pattern via get_or_compute
    std::cout << "[2] Demonstrating Cache-Aside pattern (get_or_compute)...\n";
    int upstream_api_calls = 0;

    auto fetch_upstream_auth = [&]() -> std::string {
        ++upstream_api_calls;
        // Simulate upstream database / LDAP / OAuth roundtrip
        std::this_thread::sleep_for(5ms);
        return "upstream_auth_data_for_tenant_42";
    };

    // First call invokes fetch_upstream_auth
    std::string auth1 = session_cache.get_or_compute("tenant:42:auth", fetch_upstream_auth, 5000ms);
    std::cout << "    First call result: " << auth1 << " (Upstream calls: " << upstream_api_calls << ")\n";

    // Second call serves from cache without contacting upstream
    std::string auth2 = session_cache.get_or_compute("tenant:42:auth", fetch_upstream_auth, 5000ms);
    std::cout << "    Second call result: " << auth2 << " (Upstream calls: " << upstream_api_calls << ")\n\n";

    // 3. Multi-threaded worker simulation
    std::cout << "[3] Running multi-threaded worker simulation (4 worker threads hammering cache)...\n";
    std::vector<std::thread> workers;
    const int num_workers = 4;
    const int requests_per_worker = 2'500;

    auto start_time = std::chrono::steady_clock::now();

    for (int t = 0; t < num_workers; ++t) {
        workers.emplace_back([&, t]() {
            for (int i = 0; i < requests_per_worker; ++i) {
                std::string key = "req_key_" + std::to_string((t * 500) + (i % 200));
                if (i % 5 == 0) {
                    session_cache.put(key, "payload_data_" + std::to_string(i));
                } else {
                    auto val = session_cache.get(key);
                    (void)val;
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start_time);

    std::cout << "    Processed " << (num_workers * requests_per_worker)
              << " concurrent requests in " << (elapsed.count() / 1000.0) << " ms.\n";

    // 4. Telemetry Reporting
    systems::cache::CacheStats stats = session_cache.stats();
    std::cout << "\n[Telemetry Summary]\n";
    std::cout << "    " << stats.to_string() << "\n";
    std::cout << "    Active entries in cache: " << session_cache.size() << "\n\n";

    std::cout << "Simulation completed successfully!\n";
    return 0;
}
