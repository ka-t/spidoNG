#include "spido_pg/endpoint_registry.h"

#include <algorithm>
#include <stdexcept>

namespace spido_pg {

EndpointRegistry::EndpointRegistry() = default;

void EndpointRegistry::register_policy(EndpointPolicy policy) {
    if (policy.endpoint_id == kEndpointUnknown ||
        policy.endpoint_id > kMaxEndpoints) {
        // Refuse silently for IDs out of range. The generator always
        // assigns sequential IDs from 1, so an out-of-range here means a
        // programming error in the caller — better to ignore than crash
        // the service at startup; the missing policy will just fall back
        // to "Sync / Strict" defaults inside the handlers.
        return;
    }
    std::lock_guard lk(register_mtx_);
    auto& slot = slots_[index_of(policy.endpoint_id)];
    bool was_occupied = slot.occupied.load(std::memory_order_relaxed);
    slot.policy = std::make_unique<EndpointPolicy>(std::move(policy));
    if (!slot.state) {
        slot.state = std::make_unique<EndpointRuntimeState>();
        slot.state->endpoint_id = slot.policy->endpoint_id;
    }
    slot.occupied.store(true, std::memory_order_release);
    if (!was_occupied) registered_.fetch_add(1, std::memory_order_relaxed);
}

const EndpointPolicy* EndpointRegistry::policy(EndpointId id) const noexcept {
    if (id == kEndpointUnknown || id > kMaxEndpoints) return nullptr;
    const auto& slot = slots_[index_of(id)];
    if (!slot.occupied.load(std::memory_order_acquire)) return nullptr;
    return slot.policy.get();
}

EndpointRuntimeState& EndpointRegistry::state(EndpointId id) {
    // We need a stable reference for unknown ids too — handlers can call
    // record_read() before the registry has any policy registered (e.g.
    // a route the generator missed). Lazily allocate a state slot.
    if (id == kEndpointUnknown || id > kMaxEndpoints) {
        // Fallback: a singleton "unknown" state. Counters all funnel here.
        static EndpointRuntimeState fallback;
        return fallback;
    }
    auto& slot = slots_[index_of(id)];
    if (!slot.occupied.load(std::memory_order_acquire)) {
        std::lock_guard lk(register_mtx_);
        if (!slot.occupied.load(std::memory_order_relaxed)) {
            slot.state = std::make_unique<EndpointRuntimeState>();
            slot.state->endpoint_id = id;
            slot.occupied.store(true, std::memory_order_release);
            registered_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return *slot.state;
}

std::vector<EndpointId> EndpointRegistry::all_ids() const {
    std::vector<EndpointId> out;
    out.reserve(registered_.load(std::memory_order_relaxed));
    for (size_t i = 0; i < kMaxEndpoints; ++i) {
        if (slots_[i].occupied.load(std::memory_order_acquire)) {
            out.push_back(static_cast<EndpointId>(i + 1));
        }
    }
    return out;
}

size_t EndpointRegistry::count() const noexcept {
    return registered_.load(std::memory_order_relaxed);
}

// Top-K via partial sort. N is typically a few hundred to a few thousand,
// K is single-digit — partial_sort is O(N log K) and avoids the heap
// allocator pressure of a priority_queue. The metric we sort on is
// already aggregated into the state by the pressure controller; here we
// just snapshot.
std::vector<EndpointRegistry::HotEntry>
EndpointRegistry::top_k_writers(size_t k) const {
    std::vector<HotEntry> all;
    all.reserve(registered_.load(std::memory_order_relaxed));
    for (size_t i = 0; i < kMaxEndpoints; ++i) {
        const auto& slot = slots_[i];
        if (!slot.occupied.load(std::memory_order_acquire)) continue;
        double r = slot.state->write_rps.load(std::memory_order_relaxed);
        if (r > 0) all.push_back({slot.state->endpoint_id, r});
    }
    if (k > all.size()) k = all.size();
    std::partial_sort(all.begin(), all.begin() + k, all.end(),
                      [](const HotEntry& a, const HotEntry& b){ return a.rps > b.rps; });
    all.resize(k);
    return all;
}

std::vector<EndpointRegistry::HotEntry>
EndpointRegistry::top_k_readers(size_t k) const {
    std::vector<HotEntry> all;
    all.reserve(registered_.load(std::memory_order_relaxed));
    for (size_t i = 0; i < kMaxEndpoints; ++i) {
        const auto& slot = slots_[i];
        if (!slot.occupied.load(std::memory_order_acquire)) continue;
        double r = slot.state->read_rps.load(std::memory_order_relaxed);
        if (r > 0) all.push_back({slot.state->endpoint_id, r});
    }
    if (k > all.size()) k = all.size();
    std::partial_sort(all.begin(), all.begin() + k, all.end(),
                      [](const HotEntry& a, const HotEntry& b){ return a.rps > b.rps; });
    all.resize(k);
    return all;
}

} // namespace spido_pg
