#pragma once

// Phase 3 — Concurrency: 256-shard key-space lock map
//
// Partitions the key space into 256 shards (power-of-two) using the low byte
// of a FNV-1a hash of the key.  Each shard owns a std::shared_mutex so:
//
//   - Multiple concurrent READERS within the same shard run in parallel.
//   - A single WRITER per shard; writers to DIFFERENT shards are unblocked.
//   - Every writer also holds the global Storage::mutex_ as a unique_lock
//     (upgraded to shared_lock for reads) so the page store and WAL see a
//     consistent view without a second lock hierarchy.
//
// Lock ordering (must always be acquired in this order to prevent deadlocks):
//   1. ShardLockMap shard lock  (per key-shard)  — innermost
//   2. Storage::mutex_          (global)          — outermost — REMOVED: shard
//      IS the write token; Storage::mutex_ stays shared for readers.
//
// Design note: for single-key writes the caller acquires the shard lock
// exclusively INSTEAD OF upgrading mutex_ to unique.  For structural ops
// (clear, scan, checkpoint) the caller acquires mutex_ exclusively as before,
// which acts as a full-database write fence.

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>

namespace kor {

/// Number of key-space shards. Must be a power of two.
inline constexpr std::size_t kShardCount = 256;

/// Map a key to its shard index [0, kShardCount).
/// Uses FNV-1a 64-bit hash — fast, good distribution, no dependencies.
inline std::size_t key_shard(const std::string& key) noexcept {
    // FNV-1a 64-bit
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : key) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    // Fold to shard index using low bits (power-of-two mask)
    return static_cast<std::size_t>(hash & (kShardCount - 1U));
}

/// 256-shard read-write lock map.
///
/// Acquire the shard lock for a key with:
///   auto lk = shard_map.write(key);   // exclusive within this shard
///   auto lk = shard_map.read(key);    // shared within this shard
class ShardLockMap {
public:
    ShardLockMap() = default;

    // Non-copyable, non-movable (array of mutexes).
    ShardLockMap(const ShardLockMap&)            = delete;
    ShardLockMap& operator=(const ShardLockMap&) = delete;
    ShardLockMap(ShardLockMap&&)                 = delete;
    ShardLockMap& operator=(ShardLockMap&&)      = delete;

    /// Acquire an exclusive (write) lock on the shard for `key`.
    [[nodiscard]] std::unique_lock<std::shared_mutex> write(const std::string& key) {
        return std::unique_lock<std::shared_mutex>(shards_[key_shard(key)].m);
    }

    [[nodiscard]] std::shared_lock<std::shared_mutex> read(const std::string& key) const {
        return std::shared_lock<std::shared_mutex>(
            const_cast<std::shared_mutex&>(shards_[key_shard(key)].m));
    }

    [[nodiscard]] std::pair<std::unique_lock<std::shared_mutex>,
                            std::unique_lock<std::shared_mutex>>
    write2(const std::string& key_a, const std::string& key_b) {
        const auto sa = key_shard(key_a);
        const auto sb = key_shard(key_b);
        if (sa == sb) {
            return {std::unique_lock<std::shared_mutex>(shards_[sa].m),
                    std::unique_lock<std::shared_mutex>()};
        }
        if (sa < sb) {
            auto la = std::unique_lock<std::shared_mutex>(shards_[sa].m);
            auto lb = std::unique_lock<std::shared_mutex>(shards_[sb].m);
            return {std::move(la), std::move(lb)};
        }
        auto lb = std::unique_lock<std::shared_mutex>(shards_[sb].m);
        auto la = std::unique_lock<std::shared_mutex>(shards_[sa].m);
        return {std::move(la), std::move(lb)};
    }

    std::shared_mutex& shard(std::size_t idx) noexcept { return shards_[idx].m; }

private:
    // Pad each mutex to its own cache line to avoid false sharing.
    // std::shared_mutex is at most 64 bytes on all platforms; 128-byte
    // alignment gives two cache lines of headroom.
    struct alignas(128) PaddedMutex {
        std::shared_mutex m;
    };

    std::array<PaddedMutex, kShardCount> shards_{};
};

}  // namespace kor
