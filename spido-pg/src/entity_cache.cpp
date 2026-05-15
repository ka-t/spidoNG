#include "spido_pg/entity_cache.h"

#include <algorithm>
#include <cstring>
#include <thread>

namespace spido_pg {

namespace {

inline size_t round_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

} // namespace

uint64_t EntityCache::key_hash(TableId table, std::string_view pk) noexcept {
    // Mix table id into the hash so different tables with the same pk
    // value don't collide on the same slot. We use a cheap multiplier
    // hash on the first 8 bytes of pk (typically a SHA-256 hex or an
    // integer encoded as text); FNV fallback for short pk.
    uint64_t h;
    if (pk.size() >= 8) {
        std::memcpy(&h, pk.data(), 8);
    } else {
        h = 0xcbf29ce484222325ULL;
        for (unsigned char c : pk) { h ^= c; h *= 0x100000001b3ULL; }
    }
    h ^= static_cast<uint64_t>(table) * 0x9e3779b97f4a7c15ULL;
    return h ? h : 1;
}

EntityCache::EntityCache(EntityCacheConfig cfg) : cfg_(cfg) {
    cfg_.shards          = round_pow2(std::max<size_t>(1, cfg_.shards));
    cfg_.slots_per_shard = round_pow2(std::max<size_t>(16, cfg_.slots_per_shard));
    shard_mask_ = cfg_.shards - 1;
    slot_mask_  = cfg_.slots_per_shard - 1;
    shards_.reserve(cfg_.shards);
    for (size_t i = 0; i < cfg_.shards; ++i) {
        auto sh = std::make_unique<Shard>();
        sh->slots = std::unique_ptr<Slot[]>(new Slot[cfg_.slots_per_shard]);
        sh->n_slots = cfg_.slots_per_shard;
        shards_.push_back(std::move(sh));
    }
    if (cfg_.sweep_interval_ms > 0) {
        sweep_thread_ = std::thread([this]{ sweep_loop(); });
    }
}

EntityCache::~EntityCache() {
    stop_.store(true, std::memory_order_release);
    if (sweep_thread_.joinable()) sweep_thread_.join();
}

EntityCache::Shard& EntityCache::shard_for(uint64_t h) {
    return *shards_[(h >> 32) & shard_mask_];
}
const EntityCache::Shard& EntityCache::shard_for(uint64_t h) const {
    return *shards_[(h >> 32) & shard_mask_];
}

EntityRowPtr EntityCache::get(TableId table, std::string_view pk) const {
    uint64_t h    = key_hash(table, pk);
    const auto& sh = shard_for(h);
    size_t   base = h & slot_mask_;
    for (size_t i = 0; i < cfg_.probe; ++i) {
        const Slot& s = sh.slots[(base + i) & slot_mask_];
        uint64_t slot_h = s.hash.load(std::memory_order_acquire);
        if (slot_h == 0)     { misses_.fetch_add(1, std::memory_order_relaxed); return nullptr; }
        if (slot_h != h)     continue;

        std::shared_lock lk(s.mtx);
        if (s.hash.load(std::memory_order_relaxed) != h) continue;
        if (s.table != table || s.pk != pk) continue;
        if (s.state == EntityState::Tombstone) {
            // Logical delete still in the cache; report miss to the caller
            // but bump tombstones so metrics see what's happening.
            misses_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        s.last_access.store(
            sh.counter.fetch_add(1, std::memory_order_relaxed) + 1,
            std::memory_order_relaxed);
        hits_.fetch_add(1, std::memory_order_relaxed);
        return s.row;
    }
    misses_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void EntityCache::put(TableId table, std::string_view pk, EntityRow row,
                      bool is_dirty,
                      std::chrono::seconds ttl)
{
    uint64_t h = key_hash(table, pk);
    auto&    sh = shard_for(h);
    size_t   base = h & slot_mask_;

    // Find target: existing match, empty slot, or LRU non-pinned victim.
    Slot* target     = nullptr;
    Slot* empty_slot = nullptr;
    Slot* lru        = nullptr;
    uint64_t lru_seen = UINT64_MAX;

    for (size_t i = 0; i < cfg_.probe; ++i) {
        Slot& s = sh.slots[(base + i) & slot_mask_];
        uint64_t slot_h = s.hash.load(std::memory_order_acquire);
        if (slot_h == 0) {
            if (!empty_slot) empty_slot = &s;
            continue;
        }
        if (slot_h == h) {
            std::shared_lock rlk(s.mtx);
            if (s.hash.load(std::memory_order_relaxed) == h &&
                s.table == table && s.pk == pk) {
                target = &s; break;
            }
        }
        if (!s.pinned) {
            uint64_t la = s.last_access.load(std::memory_order_relaxed);
            if (la < lru_seen) { lru_seen = la; lru = &s; }
        }
    }
    if (!target) target = empty_slot;
    if (!target) {
        // No existing slot, no empty. Evict the LRU candidate — but only
        // if it isn't dirty/flushing. Dirty entries are pinned against
        // eviction (that's the durability guarantee).
        if (lru) {
            std::shared_lock rlk(lru->mtx);
            if (lru->state == EntityState::Dirty ||
                lru->state == EntityState::Flushing ||
                lru->state == EntityState::Tombstone) {
                lru = nullptr;
            }
        }
        target = lru;
    }
    if (!target) {
        // All candidates dirty/pinned. Don't corrupt; drop the new value.
        // The caller will retry on the next put or rely on PG-direct
        // read until eviction is possible again.
        return;
    }

    auto row_ptr = std::make_shared<const EntityRow>(std::move(row));
    size_t bytes = sizeof(EntityRow) + row_ptr->data.size() + pk.size() + 64;

    std::unique_lock lk(target->mtx);
    // Clear the hash first so concurrent readers bail out without
    // reading stale fields.
    target->hash.store(0, std::memory_order_release);

    if (target->size_bytes > 0) {
        sh.bytes.fetch_sub(target->size_bytes, std::memory_order_relaxed);
        evictions_.fetch_add(1, std::memory_order_relaxed);
    }
    target->table   = table;
    target->pk.assign(pk);
    target->row     = row_ptr;
    target->version = row_ptr->version;
    target->state   = is_dirty ? EntityState::Dirty : EntityState::Clean;
    target->size_bytes = bytes;
    target->last_access.store(
        sh.counter.fetch_add(1, std::memory_order_relaxed) + 1,
        std::memory_order_relaxed);
    if (ttl.count() > 0) target->expires_at =
        std::chrono::steady_clock::now() + ttl;
    else                  target->expires_at = std::chrono::steady_clock::time_point{};
    sh.bytes.fetch_add(bytes, std::memory_order_relaxed);
    target->hash.store(h, std::memory_order_release);
}

void EntityCache::erase(TableId table, std::string_view pk, uint64_t version) {
    EntityRow t;
    t.tombstone = true;
    t.version   = version;
    put(table, pk, std::move(t), /*is_dirty=*/true);

    // Adjust state to Tombstone explicitly — put() set it to Dirty
    // because tombstones are still pending PG.
    uint64_t h = key_hash(table, pk);
    auto&    sh = shard_for(h);
    size_t   base = h & slot_mask_;
    for (size_t i = 0; i < cfg_.probe; ++i) {
        Slot& s = sh.slots[(base + i) & slot_mask_];
        if (s.hash.load(std::memory_order_acquire) != h) continue;
        std::unique_lock lk(s.mtx);
        if (s.hash.load(std::memory_order_relaxed) == h &&
            s.table == table && s.pk == pk) {
            s.state = EntityState::Tombstone;
            tombstones_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void EntityCache::mark_flushed(TableId table, std::string_view pk,
                               uint64_t version)
{
    uint64_t h = key_hash(table, pk);
    auto&    sh = shard_for(h);
    size_t   base = h & slot_mask_;
    for (size_t i = 0; i < cfg_.probe; ++i) {
        Slot& s = sh.slots[(base + i) & slot_mask_];
        if (s.hash.load(std::memory_order_acquire) != h) continue;
        std::unique_lock lk(s.mtx);
        if (s.hash.load(std::memory_order_relaxed) != h ||
            s.table != table || s.pk != pk) continue;
        // Only transition to Clean if the flush is for the version we
        // currently hold. If a newer write came in while the flush was in
        // flight, leave the dirty bit set so the next flush sees it.
        if (s.version > version) return;
        if (s.state == EntityState::Tombstone) {
            // Tombstone flushed → drop the slot entirely.
            s.hash.store(0, std::memory_order_release);
            sh.bytes.fetch_sub(s.size_bytes, std::memory_order_relaxed);
            s.size_bytes = 0;
            s.pk.clear();
            s.row.reset();
        } else {
            s.state = EntityState::Clean;
        }
        return;
    }
}

void EntityCache::notify_remote_version(TableId table, std::string_view pk,
                                        uint64_t remote_version)
{
    uint64_t h = key_hash(table, pk);
    auto&    sh = shard_for(h);
    size_t   base = h & slot_mask_;
    for (size_t i = 0; i < cfg_.probe; ++i) {
        Slot& s = sh.slots[(base + i) & slot_mask_];
        if (s.hash.load(std::memory_order_acquire) != h) continue;
        std::unique_lock lk(s.mtx);
        if (s.hash.load(std::memory_order_relaxed) != h ||
            s.table != table || s.pk != pk) continue;
        // Never overwrite a newer LOCAL state. The remote NOTIFY only
        // wins if our local copy is strictly older AND not dirty (a dirty
        // local write hasn't been to PG yet; NOTIFY can't reflect it).
        if (s.state == EntityState::Dirty || s.state == EntityState::Flushing) return;
        if (s.version >= remote_version) return;
        // We're stale. The simplest correct response is to evict the
        // entry so the next read goes to PG and refills from there.
        s.hash.store(0, std::memory_order_release);
        sh.bytes.fetch_sub(s.size_bytes, std::memory_order_relaxed);
        s.size_bytes = 0;
        s.row.reset();
        s.pk.clear();
        evictions_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

void EntityCache::pin(TableId table, std::string_view pk) {
    uint64_t h = key_hash(table, pk);
    auto&    sh = shard_for(h);
    size_t   base = h & slot_mask_;
    for (size_t i = 0; i < cfg_.probe; ++i) {
        Slot& s = sh.slots[(base + i) & slot_mask_];
        if (s.hash.load(std::memory_order_acquire) != h) continue;
        std::unique_lock lk(s.mtx);
        if (s.hash.load(std::memory_order_relaxed) == h &&
            s.table == table && s.pk == pk) {
            s.pinned = true;
            return;
        }
    }
}

void EntityCache::unpin(TableId table, std::string_view pk) {
    uint64_t h = key_hash(table, pk);
    auto&    sh = shard_for(h);
    size_t   base = h & slot_mask_;
    for (size_t i = 0; i < cfg_.probe; ++i) {
        Slot& s = sh.slots[(base + i) & slot_mask_];
        if (s.hash.load(std::memory_order_acquire) != h) continue;
        std::unique_lock lk(s.mtx);
        if (s.hash.load(std::memory_order_relaxed) == h &&
            s.table == table && s.pk == pk) {
            s.pinned = false;
            return;
        }
    }
}

EntityCache::Stats EntityCache::stats() const {
    Stats out;
    out.hits       = hits_.load(std::memory_order_relaxed);
    out.misses     = misses_.load(std::memory_order_relaxed);
    out.evictions  = evictions_.load(std::memory_order_relaxed);
    out.tombstones = tombstones_.load(std::memory_order_relaxed);
    for (const auto& shp : shards_) {
        out.bytes += shp->bytes.load(std::memory_order_relaxed);
        for (size_t i = 0; i < shp->n_slots; ++i) {
            const Slot& s = shp->slots[i];
            if (s.hash.load(std::memory_order_relaxed) == 0) continue;
            ++out.entries;
            if (s.state == EntityState::Dirty ||
                s.state == EntityState::Flushing) ++out.dirty;
        }
    }
    return out;
}

void EntityCache::sweep_loop() {
    using namespace std::chrono;
    while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(milliseconds(cfg_.sweep_interval_ms));
        if (stop_.load(std::memory_order_acquire)) return;
        auto now = steady_clock::now();

        // Two passes per shard:
        //   1. TTL-expired clean entries → evict
        //   2. Per-shard byte quota → evict LRU non-dirty until under
        size_t per_shard_quota = cfg_.max_bytes / cfg_.shards + 1;
        for (auto& shp : shards_) {
            auto& sh = *shp;
            for (size_t i = 0; i < sh.n_slots; ++i) {
                Slot& s = sh.slots[i];
                if (s.hash.load(std::memory_order_acquire) == 0) continue;
                std::unique_lock lk(s.mtx, std::try_to_lock);
                if (!lk) continue;
                if (s.hash.load(std::memory_order_relaxed) == 0) continue;
                if (s.state == EntityState::Dirty ||
                    s.state == EntityState::Flushing ||
                    s.state == EntityState::Tombstone) continue;
                if (s.pinned) continue;
                if (s.expires_at != std::chrono::steady_clock::time_point{} &&
                    s.expires_at <= now) {
                    s.hash.store(0, std::memory_order_release);
                    sh.bytes.fetch_sub(s.size_bytes, std::memory_order_relaxed);
                    s.size_bytes = 0;
                    s.pk.clear();
                    s.row.reset();
                    evictions_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // Byte quota pass — drop oldest non-dirty non-pinned slots.
            while (sh.bytes.load(std::memory_order_relaxed) > per_shard_quota) {
                Slot* victim = nullptr;
                uint64_t oldest = UINT64_MAX;
                for (size_t i = 0; i < sh.n_slots; ++i) {
                    Slot& s = sh.slots[i];
                    if (s.hash.load(std::memory_order_relaxed) == 0) continue;
                    if (s.state != EntityState::Clean) continue;
                    if (s.pinned) continue;
                    uint64_t la = s.last_access.load(std::memory_order_relaxed);
                    if (la < oldest) { oldest = la; victim = &s; }
                }
                if (!victim) break;
                std::unique_lock vlk(victim->mtx);
                if (victim->hash.load(std::memory_order_relaxed) == 0) continue;
                if (victim->state != EntityState::Clean) continue;
                victim->hash.store(0, std::memory_order_release);
                sh.bytes.fetch_sub(victim->size_bytes, std::memory_order_relaxed);
                victim->size_bytes = 0;
                victim->pk.clear();
                victim->row.reset();
                evictions_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

} // namespace spido_pg
