#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace spido_pg {

// Heterogeneous-lookup hasher for std::unordered_map keyed by std::string.
// Lets find()/erase()/count()/contains() accept std::string_view (or const
// char*) directly — no temporary std::string allocation on the hot path.
// Pair with std::equal_to<> (transparent) to enable lookup.
struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
    size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
    size_t operator()(const char* s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

template <typename V>
using StringMap = std::unordered_map<std::string, V, StringHash, std::equal_to<>>;

} // namespace spido_pg
