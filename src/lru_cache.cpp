#include "systems/cache/lru_cache.hpp"

#include <cstdint>
#include <string>

namespace systems::cache {

// Explicit template instantiations for common production systems types
template class LRUCache<std::string, std::string>;
template class LRUCache<std::string, int>;
template class LRUCache<int, std::string>;
template class LRUCache<int, int>;
template class LRUCache<uint64_t, std::string>;

} // namespace systems::cache
