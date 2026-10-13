#pragma once

#include "cache_stats.hpp"

#include <chrono>
#include <concepts>
#include <cstddef>
#include <functional>
#include <list>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace systems::cache {

/**
 * @brief High-performance, single-threaded Least Recently Used (LRU) Cache
 *        with optional Time-To-Live (TTL) expiration support.
 *
 * DESIGN RATIONALE:
 * -----------------
 * An LRU Cache must support three primary operations in strictly O(1) average time complexity:
 *   1. Lookup (GET)
 *   2. Insertion / Update (PUT)
 *   3. Eviction of the oldest entry when capacity is exceeded
 *
 * To achieve true O(1) operations, we combine two fundamental data structures:
 *
 * 1. std::list<CacheEntry> (Doubly Linked List):
 *    - Maintains the access ordering of entries.
 *    - Front of list represents the Most Recently Used (MRU) item.
 *    - Back of list represents the Least Recently Used (LRU) item.
 *    - Why std::list?
 *      * Node Stability: Iterators and references to std::list nodes remain valid
 *        indefinitely upon insertions and removals of other nodes (unlike std::vector
 *        or std::deque, which reallocate or shift memory).
 *      * O(1) Splicing: std::list::splice allows detaching a node from any arbitrary
 *        position and re-linking it to the head in O(1) time without allocating/deallocating
 *        memory or moving Key/Value payloads.
 *
 * 2. std::unordered_map<Key, list_iterator, Hash, KeyEqual> (Hash Table):
 *    - Maps each Key directly to an iterator pointing to its node in the doubly linked list.
 *    - Provides average O(1) lookup to find the element and its position in the list.
 *
 * 3. Lazy TTL Expiration:
 *    - Each entry can optionally store a time_point for expiration.
 *    - On access, expired entries are lazily evicted in O(1) without requiring expensive
 *      background scanning threads. An explicit `evict_expired()` cleanup method is also provided.
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
class LRUCache {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::milliseconds;

    struct CacheEntry {
        Key key;
        Value value;
        TimePoint expiry;

        [[nodiscard]] bool is_expired(TimePoint now) const noexcept {
            return expiry != TimePoint::max() && now >= expiry;
        }
    };

    using ListType = std::list<CacheEntry>;
    using ListIterator = typename ListType::iterator;
    using ConstListIterator = typename ListType::const_iterator;
    using MapType = std::unordered_map<Key, ListIterator, Hash, KeyEqual>;

    /**
     * @brief Constructs an LRU cache with the specified positive capacity.
     * @param capacity Maximum number of key-value pairs the cache can hold.
     * @throws std::invalid_argument if capacity is 0.
     */
    explicit LRUCache(size_t capacity)
        : m_capacity(capacity) {
        if (m_capacity == 0) {
            throw std::invalid_argument("Cache capacity must be greater than zero.");
        }
    }

    ~LRUCache() = default;

    // Non-copyable for clear ownership semantics, but movable
    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;
    LRUCache(LRUCache&&) noexcept = default;
    LRUCache& operator=(LRUCache&&) noexcept = default;

    /**
     * @brief Inserts or updates a key-value pair in the cache with optional TTL.
     *
     * If the key already exists:
     *   - Its value and expiration time are updated.
     *   - The entry is promoted to the front (MRU) via O(1) splice.
     *   - Returns true (indicating an update).
     *
     * If the key does not exist:
     *   - If the cache is at capacity, the back element (LRU) is evicted in O(1).
     *   - The new element is inserted at the front (MRU).
     *   - Its list iterator is stored in the hash map.
     *   - Returns false (indicating a new insertion).
     *
     * @param key Key to store (moved).
     * @param value Value associated with the key (moved).
     * @param ttl Optional Time-To-Live duration. 0ms means no expiration.
     * @return true if an existing key was updated, false if newly inserted.
     */
    bool put(Key key, Value value, Duration ttl = Duration::zero()) {
        const TimePoint expiry = calculate_expiry(ttl);
        auto map_it = m_lookup_map.find(key);

        if (map_it != m_lookup_map.end()) {
            // Key exists: update value and expiry, promote to MRU (front)
            map_it->second->value = std::move(value);
            map_it->second->expiry = expiry;
            m_access_list.splice(m_access_list.begin(), m_access_list, map_it->second);
            return true;
        }

        // Key is new: check if capacity threshold reached
        if (m_access_list.size() >= m_capacity) {
            evict_lru_internal();
        }

        // Emplace new node at the front of the list (MRU)
        m_access_list.push_front(CacheEntry{key, std::move(value), expiry});
        m_lookup_map.emplace(std::move(key), m_access_list.begin());
        return false;
    }

    /**
     * @brief Retrieves the value for a key and marks it as Most Recently Used.
     *
     * If found and not expired:
     *   - Promotes the node to the front (MRU) via O(1) splice.
     *   - Increments the hit counter.
     *   - Returns std::optional containing a copy of the value.
     * If expired:
     *   - Evicts the entry in O(1).
     *   - Increments expiration and miss counters.
     *   - Returns std::nullopt.
     * If not found:
     *   - Increments the miss counter.
     *   - Returns std::nullopt.
     *
     * @param key Key to query.
     * @return std::optional<Value> containing the value if found and valid, std::nullopt otherwise.
     */
    std::optional<Value> get(const Key& key) {
        auto map_it = m_lookup_map.find(key);
        if (map_it == m_lookup_map.end()) {
            ++m_stats.misses;
            return std::nullopt;
        }

        const auto now = Clock::now();
        if (map_it->second->is_expired(now)) {
            // Lazy TTL expiration
            m_access_list.erase(map_it->second);
            m_lookup_map.erase(map_it);
            ++m_stats.expirations;
            ++m_stats.misses;
            return std::nullopt;
        }

        // Cache Hit: splice node to front (MRU position) in O(1) time
        m_access_list.splice(m_access_list.begin(), m_access_list, map_it->second);
        ++m_stats.hits;
        return map_it->second->value;
    }

    /**
     * @brief Cache-aside pattern: retrieves an item, or computes and caches it if missing/expired.
     *
     * @tparam ComputeFn Callable with signature: Value()
     * @param key Key to query.
     * @param compute Callable returning Value if key is not present.
     * @param ttl Optional TTL duration for the computed item.
     * @return Value either from cache or newly computed.
     */
    template <typename ComputeFn>
        requires std::invocable<ComputeFn> && std::convertible_to<std::invoke_result_t<ComputeFn>, Value>
    Value get_or_compute(const Key& key, ComputeFn&& compute, Duration ttl = Duration::zero()) {
        if (auto cached = get(key)) {
            return std::move(*cached);
        }

        Value computed = std::invoke(std::forward<ComputeFn>(compute));
        Value result = computed;
        put(key, std::move(computed), ttl);
        return result;
    }

    /**
     * @brief Inspects a value without altering the LRU access order.
     *
     * Useful for telemetry, debugging, or non-invasive read-only inspection.
     * Does NOT update cache statistics or LRU recency.
     *
     * @param key Key to inspect.
     * @return std::optional<Value> if present and not expired, std::nullopt otherwise.
     */
    [[nodiscard]] std::optional<Value> peek(const Key& key) const {
        auto map_it = m_lookup_map.find(key);
        if (map_it == m_lookup_map.end()) {
            return std::nullopt;
        }

        const auto now = Clock::now();
        if (map_it->second->is_expired(now)) {
            return std::nullopt;
        }

        return map_it->second->value;
    }

    /**
     * @brief Checks if a non-expired key exists in the cache without altering LRU order.
     */
    [[nodiscard]] bool contains(const Key& key) const {
        auto map_it = m_lookup_map.find(key);
        if (map_it == m_lookup_map.end()) {
            return false;
        }
        return !map_it->second->is_expired(Clock::now());
    }

    /**
     * @brief Removes a key and its associated value from the cache.
     * @return true if key was found and removed, false if not found.
     */
    bool remove(const Key& key) {
        auto map_it = m_lookup_map.find(key);
        if (map_it == m_lookup_map.end()) {
            return false;
        }

        m_access_list.erase(map_it->second);
        m_lookup_map.erase(map_it);
        return true;
    }

    /**
     * @brief Proactively scans and evicts all expired items from the cache.
     * @return Number of expired entries purged.
     */
    size_t evict_expired() {
        const auto now = Clock::now();
        size_t purged = 0;

        for (auto it = m_access_list.begin(); it != m_access_list.end();) {
            if (it->is_expired(now)) {
                m_lookup_map.erase(it->key);
                it = m_access_list.erase(it);
                ++purged;
                ++m_stats.expirations;
            } else {
                ++it;
            }
        }
        return purged;
    }

    /**
     * @brief Clears all entries and resets the cache.
     */
    void clear() {
        m_lookup_map.clear();
        m_access_list.clear();
    }

    [[nodiscard]] size_t size() const noexcept { return m_access_list.size(); }
    [[nodiscard]] size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool empty() const noexcept { return m_access_list.empty(); }
    [[nodiscard]] const CacheStats& stats() const noexcept { return m_stats; }
    void reset_stats() noexcept { m_stats.reset(); }

private:
    [[nodiscard]] static TimePoint calculate_expiry(Duration ttl) noexcept {
        if (ttl <= Duration::zero()) {
            return TimePoint::max();
        }
        return Clock::now() + ttl;
    }

    void evict_lru_internal() {
        if (m_access_list.empty()) {
            return;
        }
        const Key& lru_key = m_access_list.back().key;
        m_lookup_map.erase(lru_key);
        m_access_list.pop_back();
        ++m_stats.evictions;
    }

    size_t m_capacity;
    ListType m_access_list;   ///< Front = MRU, Back = LRU
    MapType m_lookup_map;     ///< Maps Key -> ListIterator
    CacheStats m_stats;       ///< Telemetry metrics
};

} // namespace systems::cache
