#include "server/router.h"

namespace spido {

namespace {

bool path_has_pattern(std::string_view path) noexcept {
    for (char c : path) if (c == ':' || c == '*') return true;
    return false;
}

// Walk path segments calling `f(segment)` for each '/'-separated piece.
template <class F>
void for_each_segment(std::string_view path, F&& f) {
    size_t i = (!path.empty() && path[0] == '/') ? 1 : 0;
    while (i < path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string_view::npos) j = path.size();
        f(path.substr(i, j - i));
        i = j + 1;
    }
}

} // namespace

void Router::add(Method m, std::string_view path, Handler h,
                 bool require_auth) {
    auto idx = static_cast<size_t>(m);
    RouteEntry e;
    e.handler      = std::move(h);
    e.require_auth = require_auth;
    if (path_has_pattern(path)) {
        insert_trie(m, path, std::move(e));
    } else {
        tables_[idx][std::string(path)] = std::move(e);
    }
}

void Router::add_static(Method m, std::string_view path,
                        std::shared_ptr<const std::string> precomputed,
                        bool require_auth) {
    auto idx = static_cast<size_t>(m);
    RouteEntry e;
    e.cached       = std::move(precomputed);
    e.require_auth = require_auth;
    if (path_has_pattern(path)) {
        insert_trie(m, path, std::move(e));
    } else {
        tables_[idx][std::string(path)] = std::move(e);
    }
}

void Router::insert_trie(Method m, std::string_view path, RouteEntry entry) {
    auto idx = static_cast<size_t>(m);
    TrieNode* node = &trie_roots_[idx];
    for_each_segment(path, [&](std::string_view seg) {
        if (!seg.empty() && seg[0] == ':') {
            if (!node->wildcard) {
                node->wildcard = std::make_unique<TrieNode>();
                node->wildcard->param_name = std::string(seg.substr(1));
            }
            node = node->wildcard.get();
        } else {
            auto& slot = node->kids[std::string(seg)];
            if (!slot) slot = std::make_unique<TrieNode>();
            node = slot.get();
        }
    });
    node->is_endpoint = true;
    node->entry       = std::move(entry);
}

const RouteEntry* Router::match_trie(
    Method m, std::string_view path,
    std::vector<std::pair<std::string_view, std::string_view>>* out_params
) const noexcept {
    auto idx = static_cast<size_t>(m);
    if (idx >= trie_roots_.size()) return nullptr;
    const TrieNode* node = &trie_roots_[idx];

    bool matched = true;
    for_each_segment(path, [&](std::string_view seg) {
        if (!matched || !node) return;
        // Prefer literal match over wildcard.
        auto it = node->kids.find(seg);
        if (it != node->kids.end()) { node = it->second.get(); return; }
        if (node->wildcard) {
            if (out_params) out_params->push_back(
                {std::string_view(node->wildcard->param_name), seg});
            node = node->wildcard.get();
            return;
        }
        matched = false;
        node = nullptr;
    });
    if (matched && node && node->is_endpoint) return &node->entry;
    return nullptr;
}

const RouteEntry* Router::find(
    Method m, std::string_view path,
    std::vector<std::pair<std::string_view, std::string_view>>* out_params
) const noexcept {
    auto idx = static_cast<size_t>(m);
    if (idx >= tables_.size()) return nullptr;

    // Hot path: exact-match hash hit.
    const auto& t = tables_[idx];
    auto it = t.find(path);
    if (it != t.end()) return &it->second;

    // Slow path: pattern match via trie.
    return match_trie(m, path, out_params);
}

} // namespace spido
