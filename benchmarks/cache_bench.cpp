#include "systems/cache/concurrent_lru_cache.hpp"
#include "systems/cache/lru_cache.hpp"

#include <benchmark/benchmark.h>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace {

// Naive global-mutex LRU cache for comparative baseline benchmarking
template <typename Key, typename Value>
class NaiveThreadSafeLRU {
public:
    explicit NaiveThreadSafeLRU(size_t capacity)
        : m_cache(capacity) {}

    bool put(const Key& key, Value value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cache.put(key, std::move(value));
    }

    std::optional<Value> get(const Key& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cache.get(key);
    }

private:
    std::mutex m_mutex;
    systems::cache::LRUCache<Key, Value> m_cache;
};

// Generate synthetic key working set
std::vector<std::string> generate_keys(size_t count) {
    std::vector<std::string> keys;
    keys.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        keys.push_back("key_" + std::to_string(i));
    }
    return keys;
}

// Global datasets for benchmarks
const size_t WORKING_SET_SIZE = 100'000;
const size_t CACHE_CAPACITY = 20'000;

// ============================================================================
// Single-Threaded Core Benchmarks
// ============================================================================

static void BM_SingleThread_LRU_Put(benchmark::State& state) {
    systems::cache::LRUCache<std::string, std::string> cache(CACHE_CAPACITY);
    const auto keys = generate_keys(WORKING_SET_SIZE);
    size_t idx = 0;

    for (auto _ : state) {
        cache.put(keys[idx % WORKING_SET_SIZE], "payload_data");
        ++idx;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SingleThread_LRU_Put);

static void BM_SingleThread_LRU_Get_Hit(benchmark::State& state) {
    systems::cache::LRUCache<std::string, std::string> cache(CACHE_CAPACITY);
    const auto keys = generate_keys(CACHE_CAPACITY);
    for (const auto& k : keys) {
        cache.put(k, "payload_data");
    }

    size_t idx = 0;
    for (auto _ : state) {
        auto val = cache.get(keys[idx % CACHE_CAPACITY]);
        benchmark::DoNotOptimize(val);
        ++idx;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SingleThread_LRU_Get_Hit);

// ============================================================================
// Concurrent Multi-Threaded Benchmarks: Sharded vs. Naive Global Mutex
// ============================================================================

// Fixture for Sharded Concurrent LRU Cache
class ShardedCacheFixture : public benchmark::Fixture {
public:
    std::unique_ptr<systems::cache::ConcurrentLRUCache<std::string, std::string>> cache;
    std::vector<std::string> keys;

    void SetUp(const ::benchmark::State&) override {
        cache = std::make_unique<systems::cache::ConcurrentLRUCache<std::string, std::string>>(
            CACHE_CAPACITY, 32);
        keys = generate_keys(WORKING_SET_SIZE);
        // Pre-populate cache to simulate warm cache state
        for (size_t i = 0; i < CACHE_CAPACITY; ++i) {
            cache->put(keys[i], "warm_data");
        }
    }

    void TearDown(const ::benchmark::State&) override {
        cache.reset();
    }
};

// Fixture for Naive Global Mutex Cache
class NaiveCacheFixture : public benchmark::Fixture {
public:
    std::unique_ptr<NaiveThreadSafeLRU<std::string, std::string>> cache;
    std::vector<std::string> keys;

    void SetUp(const ::benchmark::State&) override {
        cache = std::make_unique<NaiveThreadSafeLRU<std::string, std::string>>(CACHE_CAPACITY);
        keys = generate_keys(WORKING_SET_SIZE);
        for (size_t i = 0; i < CACHE_CAPACITY; ++i) {
            cache->put(keys[i], "warm_data");
        }
    }

    void TearDown(const ::benchmark::State&) override {
        cache.reset();
    }
};

// Read-Heavy Concurrent Workload (90% GET, 10% PUT) - Sharded
BENCHMARK_DEFINE_F(ShardedCacheFixture, BM_Concurrent_ReadHeavy_Sharded)(benchmark::State& state) {
    std::mt19937_64 rng(1337 + state.thread_index());
    std::uniform_int_distribution<size_t> dist_key(0, WORKING_SET_SIZE - 1);
    std::uniform_int_distribution<int> dist_op(0, 99);

    for (auto _ : state) {
        const size_t key_idx = dist_key(rng);
        if (dist_op(rng) < 90) {
            auto val = cache->get(keys[key_idx]);
            benchmark::DoNotOptimize(val);
        } else {
            cache->put(keys[key_idx], "updated_data");
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK_REGISTER_F(ShardedCacheFixture, BM_Concurrent_ReadHeavy_Sharded)
    ->ThreadRange(1, 16)
    ->UseRealTime();

// Read-Heavy Concurrent Workload (90% GET, 10% PUT) - Naive Global Mutex
BENCHMARK_DEFINE_F(NaiveCacheFixture, BM_Concurrent_ReadHeavy_NaiveMutex)(benchmark::State& state) {
    std::mt19937_64 rng(1337 + state.thread_index());
    std::uniform_int_distribution<size_t> dist_key(0, WORKING_SET_SIZE - 1);
    std::uniform_int_distribution<int> dist_op(0, 99);

    for (auto _ : state) {
        const size_t key_idx = dist_key(rng);
        if (dist_op(rng) < 90) {
            auto val = cache->get(keys[key_idx]);
            benchmark::DoNotOptimize(val);
        } else {
            cache->put(keys[key_idx], "updated_data");
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK_REGISTER_F(NaiveCacheFixture, BM_Concurrent_ReadHeavy_NaiveMutex)
    ->ThreadRange(1, 16)
    ->UseRealTime();

// Write-Heavy Concurrent Workload (50% GET, 50% PUT) - Sharded
BENCHMARK_DEFINE_F(ShardedCacheFixture, BM_Concurrent_WriteHeavy_Sharded)(benchmark::State& state) {
    std::mt19937_64 rng(42 + state.thread_index());
    std::uniform_int_distribution<size_t> dist_key(0, WORKING_SET_SIZE - 1);
    std::uniform_int_distribution<int> dist_op(0, 1);

    for (auto _ : state) {
        const size_t key_idx = dist_key(rng);
        if (dist_op(rng) == 0) {
            auto val = cache->get(keys[key_idx]);
            benchmark::DoNotOptimize(val);
        } else {
            cache->put(keys[key_idx], "new_payload");
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK_REGISTER_F(ShardedCacheFixture, BM_Concurrent_WriteHeavy_Sharded)
    ->ThreadRange(1, 16)
    ->UseRealTime();

} // namespace

BENCHMARK_MAIN();
