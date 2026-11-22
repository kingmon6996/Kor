#include "kor/buffer_pool.hpp"

#include <stdexcept>

namespace kor {

// ---------------------------------------------------------------------------
// PinnedPage
// ---------------------------------------------------------------------------

PinnedPage::PinnedPage(Frame* frame, std::size_t frame_idx,
                       BufferPool* pool) noexcept
    : frame_(frame), frame_idx_(frame_idx), pool_(pool) {}

PinnedPage::~PinnedPage() {
    if (frame_)
        pool_->unpin(frame_idx_);
}

PinnedPage::PinnedPage(PinnedPage&& other) noexcept
    : frame_(other.frame_),
      frame_idx_(other.frame_idx_),
      pool_(other.pool_) {
    other.frame_ = nullptr;
}

PinnedPage& PinnedPage::operator=(PinnedPage&& other) noexcept {
    if (this != &other) {
        if (frame_)
            pool_->unpin(frame_idx_);
        frame_     = other.frame_;
        frame_idx_ = other.frame_idx_;
        pool_      = other.pool_;
        other.frame_ = nullptr;
    }
    return *this;
}

Page& PinnedPage::page() noexcept       { return frame_->page; }
const Page& PinnedPage::page() const noexcept { return frame_->page; }

void PinnedPage::mark_dirty() noexcept {
    if (frame_)
        frame_->dirty.store(true, std::memory_order_relaxed);
}

void PinnedPage::lock_shared() const {
    if (frame_)
        frame_->latch.lock_shared();
}

void PinnedPage::unlock_shared() const {
    if (frame_)
        frame_->latch.unlock_shared();
}

void PinnedPage::lock_exclusive() {
    if (frame_)
        frame_->latch.lock();
}

void PinnedPage::unlock_exclusive() {
    if (frame_)
        frame_->latch.unlock();
}

std::shared_lock<std::shared_mutex> PinnedPage::shared_latch() const {
    if (frame_)
        return std::shared_lock<std::shared_mutex>(frame_->latch);
    return {};
}

std::unique_lock<std::shared_mutex> PinnedPage::exclusive_latch() {
    if (frame_)
        return std::unique_lock<std::shared_mutex>(frame_->latch);
    return {};
}

// ---------------------------------------------------------------------------
// BufferPool
// ---------------------------------------------------------------------------

BufferPool::BufferPool(std::size_t frame_count,
                       ReadFn      read_page,
                       WriteFn     write_page)
    : read_fn_(std::move(read_page)),
      write_fn_(std::move(write_page)) {
    if (frame_count < 32)
        frame_count = 32;
    frames_.reserve(frame_count);
    for (std::size_t i = 0; i < frame_count; ++i)
        frames_.push_back(std::make_unique<Frame>());
    page_table_.reserve(frame_count);
}

BufferPool::~BufferPool() {
    try { flush_all(); } catch (...) {}
}

PinnedPage BufferPool::fetch(std::uint8_t file_id, std::uint32_t page_id,
                              bool is_new) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t idx = fetch_frame(file_id, page_id, is_new);
    frames_[idx]->pin_count.fetch_add(1, std::memory_order_relaxed);
    frames_[idx]->ref_bit.store(true, std::memory_order_relaxed);
    if (is_new)
        frames_[idx]->dirty.store(true, std::memory_order_relaxed);
    return PinnedPage(frames_[idx].get(), idx, this);
}

std::size_t BufferPool::fetch_frame(std::uint8_t file_id,
                                     std::uint32_t page_id,
                                     bool is_new) {
    const PageKey key = make_key(file_id, page_id);
    auto it = page_table_.find(key);
    if (it != page_table_.end())
        return it->second;

    std::size_t idx = evict_one();
    Frame& f = *frames_[idx];

    f.page     = Page(page_id, file_id);
    f.file_id  = file_id;
    f.page_id  = page_id;
    f.occupied = true;
    f.dirty.store(false, std::memory_order_relaxed);
    f.ref_bit.store(false, std::memory_order_relaxed);
    f.pin_count.store(0, std::memory_order_relaxed);

    if (!is_new)
        read_fn_(file_id, page_id, f.page);

    page_table_[key] = idx;
    return idx;
}

std::size_t BufferPool::evict_one() {
    const std::size_t n = frames_.size();
    for (std::size_t tries = 0; tries < 2 * n; ++tries) {
        std::size_t idx = clock_hand_;
        clock_hand_ = (clock_hand_ + 1) % n;

        Frame& f = *frames_[idx];

        if (!f.occupied)
            return idx;

        if (f.pin_count.load(std::memory_order_relaxed) > 0)
            continue;

        if (f.ref_bit.load(std::memory_order_relaxed)) {
            f.ref_bit.store(false, std::memory_order_relaxed);
            continue;
        }

        // Evict
        if (f.dirty.load(std::memory_order_relaxed))
            write_frame(idx);

        page_table_.erase(make_key(f.file_id, f.page_id));
        f.occupied = false;
        return idx;
    }
    throw std::runtime_error("BufferPool: all frames pinned — cannot evict");
}

void BufferPool::write_frame(std::size_t idx) {
    Frame& f = *frames_[idx];
    if (!f.occupied || !f.dirty.load(std::memory_order_relaxed))
        return;
    write_fn_(f.file_id, f.page_id, f.page);
    f.dirty.store(false, std::memory_order_relaxed);
}

void BufferPool::unpin(std::size_t frame_idx) noexcept {
    frames_[frame_idx]->pin_count.fetch_sub(1, std::memory_order_relaxed);
}

void BufferPool::flush_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < frames_.size(); ++i)
        if (frames_[i]->occupied && frames_[i]->dirty.load(std::memory_order_relaxed))
            write_frame(i);
}

void BufferPool::flush_page(std::uint8_t file_id, std::uint32_t page_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = page_table_.find(make_key(file_id, page_id));
    if (it != page_table_.end())
        write_frame(it->second);
}

void BufferPool::evict_page(std::uint8_t file_id, std::uint32_t page_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = page_table_.find(make_key(file_id, page_id));
    if (it == page_table_.end()) return;
    std::size_t idx = it->second;
    if (frames_[idx]->pin_count.load(std::memory_order_relaxed) > 0) return;
    if (frames_[idx]->dirty.load(std::memory_order_relaxed))
        write_frame(idx);
    frames_[idx]->occupied = false;
    page_table_.erase(it);
}

std::size_t BufferPool::pinned_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t c = 0;
    for (const auto& f : frames_)
        if (f->occupied && f->pin_count.load(std::memory_order_relaxed) > 0) ++c;
    return c;
}

std::size_t BufferPool::dirty_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t c = 0;
    for (const auto& f : frames_)
        if (f->occupied && f->dirty.load(std::memory_order_relaxed)) ++c;
    return c;
}

} // namespace kor
