#include "kor/page_store.hpp"

#include "kor/exceptions.hpp"
#include "kor/serializer.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace kor {

// ---------------------------------------------------------------------------
// On-disk record format (stored in a page slot):
//
//   key_length  : uint32_t (4 bytes, big-endian)
//   key         : key_length bytes
//   value_bytes : rest of the slot (serialized Value via serialize_value())
//
// This reuses the existing kor::serialize_value / deserialize_value.
// ---------------------------------------------------------------------------

namespace {

std::vector<std::uint8_t> encode_record(const std::string& key,
                                         const Value& value) {
    const auto value_bytes = serialize_value(value);
    std::vector<std::uint8_t> buf;
    buf.reserve(4U + key.size() + value_bytes.size());

    // key length (uint32_t big-endian)
    const std::uint32_t klen = static_cast<std::uint32_t>(key.size());
    buf.push_back(static_cast<std::uint8_t>((klen >> 24U) & 0xFFU));
    buf.push_back(static_cast<std::uint8_t>((klen >> 16U) & 0xFFU));
    buf.push_back(static_cast<std::uint8_t>((klen >>  8U) & 0xFFU));
    buf.push_back(static_cast<std::uint8_t>( klen         & 0xFFU));

    buf.insert(buf.end(), key.begin(), key.end());
    buf.insert(buf.end(), value_bytes.begin(), value_bytes.end());
    return buf;
}

std::pair<std::string, Value> decode_record(const std::vector<std::uint8_t>& raw) {
    if (raw.size() < 4U)
        throw SerializationError("PageStore: record too short");

    const std::uint32_t klen =
        (static_cast<std::uint32_t>(raw[0]) << 24U) |
        (static_cast<std::uint32_t>(raw[1]) << 16U) |
        (static_cast<std::uint32_t>(raw[2]) <<  8U) |
         static_cast<std::uint32_t>(raw[3]);

    if (4U + klen > raw.size())
        throw SerializationError("PageStore: key extends beyond record");

    std::string key(reinterpret_cast<const char*>(raw.data() + 4U), klen);

    const std::vector<std::uint8_t> value_bytes(
        raw.begin() + 4U + static_cast<std::ptrdiff_t>(klen),
        raw.end()
    );
    Value value = deserialize_value(value_bytes);

    return {std::move(key), std::move(value)};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PageStore
// ---------------------------------------------------------------------------

PageStore::PageStore(std::filesystem::path database_dir,
                     std::size_t           buffer_frames)
    : database_dir_(std::move(database_dir)),
      buffer_frames_(buffer_frames < 32U ? 32U : buffer_frames),
      chunks_(database_dir_),
      pool_(buffer_frames_,
            // read_fn
            [this](std::uint8_t file_id, std::uint32_t page_id, Page& out) -> bool {
                return chunks_.read_page(file_id, page_id, out);
            },
            // write_fn
            [this](std::uint8_t file_id, std::uint32_t page_id, const Page& page) -> bool {
                return chunks_.write_page(file_id, page_id, page);
            }) {}

PageStore::~PageStore() {
    close();
}

void PageStore::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_) return;

    chunks_.open();

    // Rebuild the in-memory key directory by scanning all pages
    // (walk file_id=0..254 and INDEX_FILE_ID, read every page, extract records)
    const std::size_t data_chunks = chunks_.chunk_count();
    for (std::size_t ci = 0; ci < data_chunks; ++ci) {
        const std::uint8_t fid = static_cast<std::uint8_t>(ci);
        const std::uint32_t npages = chunks_.page_count(fid);
        for (std::uint32_t pid = 0; pid < npages; ++pid) {
            Page pg;
            if (!chunks_.read_page(fid, pid, pg)) continue;
            if (!pg.verify_crc()) continue;
            if (pg.header().type != static_cast<std::uint8_t>(PageType::kData)) continue;

            for (std::uint16_t sid = 0; sid < pg.slot_count(); ++sid) {
                const auto raw = pg.read_record(sid);
                if (raw.empty()) continue; // tombstone
                try {
                    auto [key, _val] = decode_record(raw);
                    KeyEntry entry;
                    entry.addr = {fid, pid, sid};
                    entry.record_size = static_cast<std::uint32_t>(raw.size());
                    directory_[std::move(key)] = entry;
                } catch (...) {
                    // Skip corrupt records
                }
            }

            // Track the last page as the current write target
            current_file_id_  = fid;
            current_page_id_  = pid;
            has_current_page_ = true;
        }
    }

    open_ = true;
}

void PageStore::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return;
    pool_.flush_all();
    chunks_.sync_all();
    chunks_.close();
    directory_.clear();
    has_current_page_ = false;
    open_ = false;
}

bool PageStore::is_open() const noexcept {
    return open_;
}

// ---------------------------------------------------------------------------
// get_or_alloc_page — find the current page or allocate a new one
// ---------------------------------------------------------------------------
PinnedPage PageStore::get_or_alloc_page(std::size_t needed_bytes, bool& is_new_page) {
    // needed_bytes: sizeof(SlotDescriptor) + encode_record size
    constexpr std::size_t slot_overhead = 4U; // SlotDescriptor

    // Try current page first
    if (has_current_page_) {
        auto pinned = pool_.fetch(current_file_id_, current_page_id_);
        const std::uint16_t free = pinned.page().free_bytes();
        if (free >= slot_overhead + needed_bytes) {
            is_new_page = false;
            return pinned;
        }
        // Current page full — allocate a new one
    }

    // Allocate new data page
    auto [new_fid, new_pid] = chunks_.allocate_data_page();
    current_file_id_  = new_fid;
    current_page_id_  = new_pid;
    has_current_page_ = true;
    is_new_page = true;

    return pool_.fetch(new_fid, new_pid, /*is_new=*/true);
}

// ---------------------------------------------------------------------------
// set
// ---------------------------------------------------------------------------
bool PageStore::set(const std::string& key, const Value& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_)
        throw std::runtime_error("PageStore: not open");

    const auto encoded = encode_record(key, value);

    // If key already exists, tombstone the old slot
    auto it = directory_.find(key);
    if (it != directory_.end()) {
        const auto& addr = it->second.addr;
        auto pinned = pool_.fetch(addr.file_id, addr.page_id);
        pinned.page().erase(addr.slot_id);
        pinned.mark_dirty();
        // Don't re-add the page to current — just tombstone in place
    }

    // Insert into a page with enough space
    if (encoded.size() > PAGE_SIZE - PAGE_HEADER_SIZE - 4U) {
        // Record larger than what fits in one page — not supported in Phase 1
        throw InvalidValueError("PageStore: record too large for a single page");
    }

    bool is_new_page = false;
    auto pinned = get_or_alloc_page(encoded.size(), is_new_page);
    auto slot_opt = pinned.page().insert(encoded);
    if (!slot_opt.has_value()) {
        // Shouldn't happen since get_or_alloc_page checked free space
        throw std::runtime_error("PageStore: insert failed unexpectedly");
    }

    pinned.mark_dirty();

    KeyEntry entry;
    entry.addr = {current_file_id_, current_page_id_, *slot_opt};
    entry.record_size = static_cast<std::uint32_t>(encoded.size());
    directory_[key] = entry;

    return true;
}

// ---------------------------------------------------------------------------
// get
// ---------------------------------------------------------------------------
std::optional<Value> PageStore::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return std::nullopt;

    auto it = directory_.find(key);
    if (it == directory_.end()) return std::nullopt;

    const auto& addr = it->second.addr;
    // const_cast safe: fetch is conceptually read-only here
    auto pinned = const_cast<BufferPool&>(pool_).fetch(addr.file_id, addr.page_id);
    const auto raw = pinned.page().read_record(addr.slot_id);
    if (raw.empty()) return std::nullopt;

    try {
        auto [_key, value] = decode_record(raw);
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// contains
// ---------------------------------------------------------------------------
bool PageStore::contains(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return directory_.count(key) > 0;
}

// ---------------------------------------------------------------------------
// erase
// ---------------------------------------------------------------------------
bool PageStore::erase(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return false;

    auto it = directory_.find(key);
    if (it == directory_.end()) return false;

    const auto& addr = it->second.addr;
    auto pinned = pool_.fetch(addr.file_id, addr.page_id);
    pinned.page().erase(addr.slot_id);
    pinned.mark_dirty();

    directory_.erase(it);
    return true;
}

// ---------------------------------------------------------------------------
// size / empty / clear
// ---------------------------------------------------------------------------
std::size_t PageStore::size() const noexcept {
    return directory_.size();
}

bool PageStore::empty() const noexcept {
    return directory_.empty();
}

void PageStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return;

    // Tombstone every live slot
    for (auto& [key, entry] : directory_) {
        const auto& addr = entry.addr;
        auto pinned = pool_.fetch(addr.file_id, addr.page_id);
        pinned.page().erase(addr.slot_id);
        pinned.mark_dirty();
    }
    directory_.clear();
    has_current_page_ = false;
}

// ---------------------------------------------------------------------------
// list_all / scan_prefix
// ---------------------------------------------------------------------------
std::vector<std::pair<std::string, Value>> PageStore::list_all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return {};

    std::vector<std::pair<std::string, Value>> results;
    results.reserve(directory_.size());

    for (const auto& [key, entry] : directory_) {
        const auto& addr = entry.addr;
        auto pinned = const_cast<BufferPool&>(pool_).fetch(addr.file_id, addr.page_id);
        const auto raw = pinned.page().read_record(addr.slot_id);
        if (raw.empty()) continue;
        try {
            auto [_k, value] = decode_record(raw);
            results.emplace_back(key, std::move(value));
        } catch (...) {}
    }

    std::sort(results.begin(), results.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return results;
}

std::vector<std::pair<std::string, Value>> PageStore::scan_prefix(
        const std::string& prefix,
        const std::optional<std::string>& after,
        std::size_t limit) const {

    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return {};

    std::vector<std::pair<std::string, Value>> results;
    results.reserve(std::min(limit, directory_.size()));

    for (const auto& [key, entry] : directory_) {
        if (!prefix.empty() && key.compare(0, prefix.size(), prefix) != 0)
            continue;
        if (after.has_value() && key <= *after)
            continue;

        const auto& addr = entry.addr;
        auto pinned = const_cast<BufferPool&>(pool_).fetch(addr.file_id, addr.page_id);
        const auto raw = pinned.page().read_record(addr.slot_id);
        if (raw.empty()) continue;
        try {
            auto [_k, value] = decode_record(raw);
            results.emplace_back(key, std::move(value));
        } catch (...) {}

        if (results.size() > limit + 1U) break;
    }

    std::sort(results.begin(), results.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    if (results.size() > limit)
        results.resize(limit);
    return results;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void PageStore::flush() {
    pool_.flush_all();
}

void PageStore::checkpoint() {
    pool_.flush_all();
    chunks_.sync_all();
}

std::uint64_t PageStore::disk_bytes() const {
    std::uint64_t total = 0;
    const std::size_t nc = chunks_.chunk_count();
    for (std::size_t i = 0; i < nc; ++i) {
        total += static_cast<std::uint64_t>(
            chunks_.page_count(static_cast<std::uint8_t>(i))) * PAGE_SIZE;
    }
    return total;
}

std::uint64_t PageStore::directory_memory_bytes() const {
    // Approximate: each entry ≈ key string + PageAddress + overhead
    std::uint64_t total = 0;
    for (const auto& [key, entry] : directory_)
        total += key.size() + sizeof(KeyEntry) + 48U; // unordered_map bucket overhead
    return total;
}

} // namespace kor
