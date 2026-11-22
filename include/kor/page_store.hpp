#pragma once

// ---------------------------------------------------------------------------
// Phase 1 — PageStore
//
// Disk-primary key-value store built on top of:
//   • ChunkManager  — multi-file on-disk page management
//   • BufferPool    — CLOCK-eviction in-memory page cache
//   • In-memory key directory (string → PageAddress) for O(1) key lookup
//
// This replaces the pure-RAM HashIndex as the primary data store.
// The in-memory key directory stays entirely in RAM (it's just 32-ish bytes
// per key for the PageAddress), while the actual record payloads live on
// disk pages and are only paged into the buffer pool when accessed.
//
// Thread safety: all public methods acquire an internal mutex.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kor/buffer_pool.hpp"
#include "kor/chunk_manager.hpp"
#include "kor/page.hpp"
#include "kor/record.hpp"
#include "kor/value.hpp"

namespace kor {

class PageStore {
public:
    // database_dir   : path to the .kor/<name>/ directory
    // buffer_frames  : number of 8 KB frames in the buffer pool (min 32)
    explicit PageStore(std::filesystem::path database_dir,
                       std::size_t           buffer_frames = 512U);
    ~PageStore();

    PageStore(const PageStore&)            = delete;
    PageStore& operator=(const PageStore&) = delete;

    // Open / recover existing database or create fresh files
    void open();
    void close();
    bool is_open() const noexcept;

    // -----------------------------------------------------------------------
    // Core key-value operations (same contract as HashIndex + Storage)
    // -----------------------------------------------------------------------

    // Insert or overwrite
    bool set(const std::string& key, const Value& value);

    // Read
    std::optional<Value> get(const std::string& key) const;

    // Existence check (no I/O if key directory is in RAM)
    bool contains(const std::string& key) const;

    // Delete; returns true if key existed
    bool erase(const std::string& key);

    // Record count (live records only)
    std::size_t size() const noexcept;

    bool empty() const noexcept;

    // Clear all records
    void clear();

    // Iterate all live records (loads each from disk — use sparingly)
    std::vector<std::pair<std::string, Value>> list_all() const;

    // Prefix scan (sorted by key)
    std::vector<std::pair<std::string, Value>> scan_prefix(
        const std::string& prefix,
        const std::optional<std::string>& after = std::nullopt,
        std::size_t limit = 1000U) const;

    // -----------------------------------------------------------------------
    // Persistence helpers (called by Storage / WAL layer)
    // -----------------------------------------------------------------------

    // Flush all dirty buffer frames to disk
    void flush();

    // Flush and fsync all files
    void checkpoint();

    // Bytes of data on disk (sum of chunk file sizes)
    std::uint64_t disk_bytes() const;

    // Approximate memory used by the in-memory key directory
    std::uint64_t directory_memory_bytes() const;

private:
    std::filesystem::path database_dir_;
    std::size_t           buffer_frames_;

    // Components
    ChunkManager chunks_;
    BufferPool   pool_;

    // Key directory: key → (PageAddress, estimated record size)
    struct KeyEntry {
        PageAddress addr;
        std::uint32_t record_size = 0; // bytes on disk for memory accounting
    };
    mutable std::mutex                           mutex_;
    std::unordered_map<std::string, KeyEntry>    directory_;
    bool                                         open_ = false;

    // Allocate a page with enough free space for `needed_bytes`
    PinnedPage get_or_alloc_page(std::size_t needed_bytes, bool& is_new_page);

    // Currently being written to (the "current page" pointer)
    std::uint8_t  current_file_id_ = 0;
    std::uint32_t current_page_id_ = 0;
    bool          has_current_page_ = false;
};

} // namespace kor
