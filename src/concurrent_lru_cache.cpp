#include "systems/cache/concurrent_lru_cache.hpp"

#include <cstdint>
#include <string>

namespace systems::cache {

// Explicit template instantiations for common production systems types
template class ConcurrentLRUCache<std::string, std::string>;
template class ConcurrentLRUCache<std::string, int>;
template class ConcurrentLRUCache<int, std::string>;
template class ConcurrentLRUCache<int, int>;
template class ConcurrentLRUCache<uint64_t, std::string>;

} // namespace systems::cache
