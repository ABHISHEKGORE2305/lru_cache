#pragma once

#include "lru_cache.hpp"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace systems::cache {

/**
 * @brief High-throughput, multi-threaded LRU Cache using key-space sharding
 *        with optional Time-To-Live (TTL) expiration support.
 *
 * SYSTEMS ENGINEERING RATIONALE & CONCURRENCY ARCHITECTURE:
 * ---------------------------------------------------------
 * In high-performance backend and edge proxy systems (such as Zscaler, Envoy, or Memcached),
 * caching layers handle tens of thousands of concurrent requests across multi-core processors.
 *
 * 1. The Global Lock Bottleneck:
 *    A naive thread-safe cache uses a single global mutex over the entire data structure.
 *    Under heavy multi-threaded concurrency (e.g., 16+ worker threads), all threads serialize
 *    on that single mutex, causing severe lock contention, thread convoying, and context switches.
 *    Throughput collapses regardless of how many CPU cores are available.
 *
 * 2. Sharded / Striped Concurrency:
 *    To eliminate global lock contention, ConcurrentLRUCache partitions the key-space across
 *    N independent shards (sub-caches).
 *      - Shard Selection: Shard index = Hash(Key) % N.
 *      - Each shard encapsulates its own independent LRUCache instance protected by its own
 *        std::shared_mutex.
 *      - Threads querying or updating different keys operate concurrently on disjoint shards
 *        with ZERO lock contention. Lock conflict probability drops from 100% to ~ 1 / N.
 *
 * 3. Lock Granularity & Reader-Writer Synchronization:
 *    - Exclusive (Write) Lock (std::unique_lock<std::shared_mutex>):
 *      Used for `put()`, `remove()`, and `get()`.
 *      *Note on `get()`*: Because LRU policy mutates recency order (promoting accessed nodes to MRU)
 *      and lazily evicts expired nodes, `get()` mutates internal state and requires an exclusive lock on that shard.
 *    - Shared (Read) Lock (std::shared_lock<std::shared_mutex>):
 *      Used for `peek()` and `contains()`. Multiple concurrent reader threads can inspect
 *      the same shard simultaneously without blocking each other.
 *
 * 4. False Sharing Mitigation:
 *    Each Shard structure is aligned to 64 bytes (the standard L1/L2 cache line size) to prevent
 *    false sharing between adjacent CPU cores when updating shard locks.
 *
 * @tparam Key Key type. Must be movable, hashable, and equality-comparable.
 * @tparam Value Value type. Must be movable.
 * @tparam Hash Hash functor for Key. Defaults to std::hash<Key>.
 * @tparam KeyEqual Equality functor for Key. Defaults to std::equal_to<Key>.
 */
template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
    requires std::movable<Key> && std::movable<Value>
class ConcurrentLRUCache {
public:
    using Duration = std::chrono::milliseconds;

    static size_t default_shard_count() noexcept {
        const unsigned int hw = std::thread::hardware_concurrency();
        const size_t base = (hw == 0) ? 8 : hw;
        return std::max<size_t>(16, base * 4);
    }

    explicit ConcurrentLRUCache(size_t total_capacity,
                                size_t shard_count = default_shard_count())
        : m_shard_count(shard_count) {
        if (total_capacity == 0) {
            throw std::invalid_argument("Total capacity must be greater than zero.");
        }
        if (m_shard_count == 0) {
            throw std::invalid_argument("Shard count must be greater than zero.");
        }

        m_shards.reserve(m_shard_count);

        const size_t capacity_per_shard = std::max<size_t>(1, total_capacity / m_shard_count);
        size_t remainder = total_capacity % m_shard_count;

        for (size_t i = 0; i < m_shard_count; ++i) {
            size_t shard_cap = capacity_per_shard + (remainder > 0 ? 1 : 0);
            if (remainder > 0) {
                --remainder;
            }
            m_shards.push_back(std::make_unique<CacheShard>(shard_cap));
        }
    }

    ~ConcurrentLRUCache() = default;

    ConcurrentLRUCache(const ConcurrentLRUCache&) = delete;
    ConcurrentLRUCache& operator=(const ConcurrentLRUCache&) = delete;
    ConcurrentLRUCache(ConcurrentLRUCache&&) noexcept = default;
    ConcurrentLRUCache& operator=(ConcurrentLRUCache&&) noexcept = default;

    /**
     * @brief Concurrently inserts or updates a key-value pair with optional TTL.
     */
    bool put(Key key, Value value, Duration ttl = Duration::zero()) {
        CacheShard& shard = get_shard(key);
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        return shard.cache.put(std::move(key), std::move(value), ttl);
    }

    /**
     * @brief Concurrently retrieves a value and updates LRU recency.
     */
    std::optional<Value> get(const Key& key) {
        CacheShard& shard = get_shard(key);
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        return shard.cache.get(key);
    }

    /**
     * @brief Cache-aside pattern: retrieves key or computes and inserts it atomically within the shard.
     */
    template <typename ComputeFn>
        requires std::invocable<ComputeFn> && std::convertible_to<std::invoke_result_t<ComputeFn>, Value>
    Value get_or_compute(const Key& key, ComputeFn&& compute, Duration ttl = Duration::zero()) {
        CacheShard& shard = get_shard(key);
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        return shard.cache.get_or_compute(key, std::forward<ComputeFn>(compute), ttl);
    }

    /**
     * @brief Concurrently inspects a key without altering LRU recency order.
     */
    [[nodiscard]] std::optional<Value> peek(const Key& key) const {
        const CacheShard& shard = get_shard(key);
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        return shard.cache.peek(key);
    }

    /**
     * @brief Concurrently checks if key exists and is non-expired using a shared (read) lock.
     */
    [[nodiscard]] bool contains(const Key& key) const {
        const CacheShard& shard = get_shard(key);
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        return shard.cache.contains(key);
    }

    /**
     * @brief Concurrently removes an item from its designated shard.
     */
    bool remove(const Key& key) {
        CacheShard& shard = get_shard(key);
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        return shard.cache.remove(key);
    }

    /**
     * @brief Proactively evicts all expired items across all shards.
     */
    size_t evict_expired() {
        size_t total_purged = 0;
        for (auto& shard_ptr : m_shards) {
            std::unique_lock<std::shared_mutex> lock(shard_ptr->mutex);
            total_purged += shard_ptr->cache.evict_expired();
        }
        return total_purged;
    }

    /**
     * @brief Clears all shards across the entire cache.
     */
    void clear() {
        for (auto& shard_ptr : m_shards) {
            std::unique_lock<std::shared_mutex> lock(shard_ptr->mutex);
            shard_ptr->cache.clear();
        }
    }

    [[nodiscard]] size_t size() const {
        size_t total = 0;
        for (const auto& shard_ptr : m_shards) {
            std::shared_lock<std::shared_mutex> lock(shard_ptr->mutex);
            total += shard_ptr->cache.size();
        }
        return total;
    }

    [[nodiscard]] size_t capacity() const noexcept {
        size_t total = 0;
        for (const auto& shard_ptr : m_shards) {
            total += shard_ptr->cache.capacity();
        }
        return total;
    }

    [[nodiscard]] size_t shard_count() const noexcept { return m_shard_count; }

    [[nodiscard]] CacheStats stats() const {
        CacheStats total_stats;
        for (const auto& shard_ptr : m_shards) {
            std::shared_lock<std::shared_mutex> lock(shard_ptr->mutex);
            total_stats += shard_ptr->cache.stats();
        }
        return total_stats;
    }

    void reset_stats() {
        for (auto& shard_ptr : m_shards) {
            std::unique_lock<std::shared_mutex> lock(shard_ptr->mutex);
            shard_ptr->cache.reset_stats();
        }
    }

private:
    struct alignas(64) CacheShard {
        mutable std::shared_mutex mutex;
        LRUCache<Key, Value, Hash, KeyEqual> cache;

        explicit CacheShard(size_t capacity)
            : cache(capacity) {}
    };

    [[nodiscard]] size_t shard_index_for(const Key& key) const noexcept {
        return m_hasher(key) % m_shard_count;
    }

    [[nodiscard]] CacheShard& get_shard(const Key& key) {
        return *m_shards[shard_index_for(key)];
    }

    [[nodiscard]] const CacheShard& get_shard(const Key& key) const {
        return *m_shards[shard_index_for(key)];
    }

    size_t m_shard_count;
    Hash m_hasher{};
    std::vector<std::unique_ptr<CacheShard>> m_shards;
};

} // namespace systems::cache
