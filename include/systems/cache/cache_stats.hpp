#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace systems::cache {

/**
 * @brief Performance statistics and telemetry for cache instances.
 *
 * Tracks cumulative hits, misses, evictions, and TTL expirations to allow cache operators
 * to monitor cache efficiency and hit ratio in high-throughput production environments.
 */
struct CacheStats {
    uint64_t hits{0};
    uint64_t misses{0};
    uint64_t evictions{0};
    uint64_t expirations{0};

    /**
     * @brief Computes the hit ratio as a floating point between 0.0 and 1.0.
     * @return Cache hit ratio, or 0.0 if no requests have been recorded.
     */
    [[nodiscard]] double hit_ratio() const noexcept {
        const uint64_t total = hits + misses;
        if (total == 0) {
            return 0.0;
        }
        return static_cast<double>(hits) / static_cast<double>(total);
    }

    /**
     * @brief Resets all statistical counters to zero.
     */
    void reset() noexcept {
        hits = 0;
        misses = 0;
        evictions = 0;
        expirations = 0;
    }

    /**
     * @brief Formats the cache metrics into a human-readable string.
     */
    [[nodiscard]] std::string to_string() const {
        std::ostringstream oss;
        oss << "Hits: " << hits << ", Misses: " << misses
            << ", Evictions: " << evictions
            << ", Expirations: " << expirations
            << ", Hit Ratio: " << std::fixed << std::setprecision(2)
            << (hit_ratio() * 100.0) << "%";
        return oss.str();
    }

    /**
     * @brief Aggregates statistics from another CacheStats instance.
     */
    CacheStats& operator+=(const CacheStats& other) noexcept {
        hits += other.hits;
        misses += other.misses;
        evictions += other.evictions;
        expirations += other.expirations;
        return *this;
    }
};

} // namespace systems::cache
