#pragma once

// ---------------------------------------------------------------------------
// Phase 1 — CLOCK Buffer Pool
//
// Manages a fixed pool of 8 KB frames.  Pages are addressed by (file_id,
// page_id).  Eviction uses the Second-Chance CLOCK algorithm:
//   • walk circular frame array
//   • ref_bit == 1  → clear to 0, advance hand
//   • ref_bit == 0 and pin_count == 0  → evict (fsync if dirty), load new page
//
// All public methods are thread-safe.  Callers must unpin pages after use.
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "kor/page.hpp"

namespace kor {

// ---------------------------------------------------------------------------
// Frame state (one per buffer pool slot)
// ---------------------------------------------------------------------------
struct Frame {
    Page             page;
    std::atomic<int> pin_count{0};   // >0 → cannot evict
    std::atomic<bool> ref_bit{false}; // second-chance clock bit
    std::atomic<bool> dirty{false};   // true → must write before evict
    bool             occupied = false; // true → holds a valid page
    std::uint8_t     file_id  = 0;
    std::uint32_t    page_id  = 0;
    mutable std::shared_mutex latch;  // Phase 3.2: per-page buffer pool latch
};

// ---------------------------------------------------------------------------
// PinGuard — RAII wrapper that unpins on destruction
// ---------------------------------------------------------------------------
class PinnedPage {
public:
    PinnedPage() noexcept = default;
    PinnedPage(Frame* frame, std::size_t frame_idx,
               class BufferPool* pool) noexcept;
    ~PinnedPage();

    PinnedPage(const PinnedPage&)            = delete;
    PinnedPage& operator=(const PinnedPage&) = delete;
    PinnedPage(PinnedPage&& other) noexcept;
    PinnedPage& operator=(PinnedPage&& other) noexcept;

    bool valid() const noexcept { return frame_ != nullptr; }

    Page&       page()       noexcept;
    const Page& page() const noexcept;

    // Mark dirty so the pool knows to write before eviction
    void mark_dirty() noexcept;

    // Phase 3.2: Per-page buffer pool latches
    void lock_shared() const;
    void unlock_shared() const;
    void lock_exclusive();
    void unlock_exclusive();
    [[nodiscard]] std::shared_lock<std::shared_mutex> shared_latch() const;
    [[nodiscard]] std::unique_lock<std::shared_mutex> exclusive_latch();

private:
    Frame*      frame_     = nullptr;
    std::size_t frame_idx_ = 0;
    BufferPool* pool_      = nullptr;
};

// ---------------------------------------------------------------------------
// BufferPool
// ---------------------------------------------------------------------------
class BufferPool {
public:
    // read_page  : called when a page is not in the pool → load from disk
    // write_page : called when a dirty page is evicted   → flush to disk
    using ReadFn  = std::function<bool(std::uint8_t file_id,
                                       std::uint32_t page_id,
                                       Page& out)>;
    using WriteFn = std::function<bool(std::uint8_t file_id,
                                       std::uint32_t page_id,
                                       const Page& page)>;

    // frame_count: number of 8 KB frames to allocate (minimum 32)
    explicit BufferPool(std::size_t frame_count,
                        ReadFn  read_page,
                        WriteFn write_page);

    ~BufferPool();

    BufferPool(const BufferPool&)            = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // Fetch a page (creates new entry if not present in pool).
    // Returns a PinnedPage; caller must keep it alive while reading/writing.
    // If is_new == true the page is returned dirty (caller fills it in).
    PinnedPage fetch(std::uint8_t file_id, std::uint32_t page_id,
                     bool is_new = false);

    // Explicitly flush all dirty frames to disk (used by checkpoint).
    void flush_all();

    // Flush a single page if it is in the pool and dirty.
    void flush_page(std::uint8_t file_id, std::uint32_t page_id);

    // Evict (and optionally flush) a page from the pool.
    void evict_page(std::uint8_t file_id, std::uint32_t page_id);

    // Statistics
    std::size_t frame_count()  const noexcept { return frames_.size(); }
    std::size_t pinned_count() const;
    std::size_t dirty_count()  const;

private:
    friend class PinnedPage;

    // Find or load a page into a frame.  Returns frame index.
    std::size_t fetch_frame(std::uint8_t file_id, std::uint32_t page_id,
                            bool is_new);

    // CLOCK eviction: find a frame that can be evicted.
    std::size_t evict_one();

    void unpin(std::size_t frame_idx) noexcept;
    void write_frame(std::size_t frame_idx);

    // Page table: (file_id << 32 | page_id) → frame_idx
    using PageKey = std::uint64_t;
    static PageKey make_key(std::uint8_t file_id, std::uint32_t page_id) noexcept {
        return (static_cast<std::uint64_t>(file_id) << 32) |
               static_cast<std::uint64_t>(page_id);
    }

    mutable std::mutex                              mutex_;
    std::vector<std::unique_ptr<Frame>>             frames_;  // heap-allocated, stable addresses
    std::unordered_map<PageKey, std::size_t>        page_table_; // key → frame_idx
    std::size_t                                     clock_hand_ = 0;
    ReadFn                                          read_fn_;
    WriteFn                                         write_fn_;
};

} // namespace kor
