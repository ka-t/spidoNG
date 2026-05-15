#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "spido/request.h"
#include "spido/server.h"

namespace spido {

// Route entry holds either a dynamic handler or a pre-serialized response
// (or both — precomputed takes priority on the hot path). Workers do a
// pointer check on `cached` and skip the handler/serialize chain entirely
// when it's set.
struct RouteEntry {
    Handler                                     handler;
    std::shared_ptr<const std::string>          cached;
    // When true the request must carry a valid JWT (when JWT is enabled
    // server-wide). Default false so existing routes keep working.
    bool                                        require_auth = false;
};

// Hybrid router: hash-table for static-segment routes, prefix trie for
// patterns that contain ':param' or '*rest' wildcards. Hot path tries the
// hash first (one O(1) hop, no alloc thanks to transparent hashing); only
// requests that miss the table walk the trie.
class Router {
public:
    // Static path: stored in the hash. ':' in `path` switches to the trie.
    void add(Method m, std::string_view path, Handler h,
             bool require_auth = false);
    void add_static(Method m, std::string_view path,
                    std::shared_ptr<const std::string> precomputed,
                    bool require_auth = false);

    // Find with optional param capture. When the request matches a trie
    // route, `out_params` receives {name, view-into-path} pairs.
    const RouteEntry* find(Method m, std::string_view path,
                           std::vector<std::pair<std::string_view,
                                                 std::string_view>>* out_params
                           = nullptr) const noexcept;

    void set_not_found(Handler h) { not_found_.handler = std::move(h); }
    const RouteEntry* not_found() const noexcept {
        return not_found_.handler ? &not_found_ : nullptr;
    }

private:
    struct SvHash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
        size_t operator()(const std::string& s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };

    using Table = std::unordered_map<std::string, RouteEntry,
                                     SvHash, std::equal_to<>>;

    // Trie node. Each node either has static children (literal segment),
    // a single ':param' child, or a terminal entry. Construction is one-
    // shot at server boot; lookup is read-only across worker threads.
    struct TrieNode {
        // Transparent hash so segment lookup uses string_view directly.
        std::unordered_map<std::string, std::unique_ptr<TrieNode>,
                           SvHash, std::equal_to<>> kids;
        std::unique_ptr<TrieNode> wildcard;
        std::string param_name;
        RouteEntry entry;
        bool       is_endpoint = false;
    };

    void insert_trie(Method m, std::string_view path, RouteEntry entry);
    const RouteEntry* match_trie(
        Method m, std::string_view path,
        std::vector<std::pair<std::string_view, std::string_view>>* out_params
    ) const noexcept;

    std::array<Table, 8>    tables_;
    std::array<TrieNode, 8> trie_roots_;
    RouteEntry              not_found_;
};

} // namespace spido
