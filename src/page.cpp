#include "kor/page.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace kor {

// ---------------------------------------------------------------------------
// CRC32C — software table-based implementation (Castagnoli polynomial)
// ---------------------------------------------------------------------------

static std::uint32_t build_crc32c_table(std::uint32_t (&table)[256]) {
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (crc & 1U ? 0x82F63B78U : 0U);
        table[i] = crc;
    }
    return 0U;
}

std::uint32_t crc32c_compute(const std::uint8_t* data, std::size_t length) noexcept {
    static std::uint32_t table[256];
    static std::uint32_t init = build_crc32c_table(table);
    (void)init;

    std::uint32_t crc = 0xFFFF'FFFFU;
    for (std::size_t i = 0; i < length; ++i)
        crc = (crc >> 8U) ^ table[(crc ^ data[i]) & 0xFFU];
    return crc ^ 0xFFFF'FFFFU;
}

// ---------------------------------------------------------------------------
// Page implementation
// ---------------------------------------------------------------------------

Page::Page() {
    bytes_.fill(0);
    // Write default header
    PageHeader hdr{};
    std::memcpy(bytes_.data(), &hdr, sizeof(hdr));
}

Page::Page(std::uint32_t page_id, std::uint8_t file_id, PageType type) {
    bytes_.fill(0);
    PageHeader hdr{};
    hdr.page_id    = page_id;
    hdr.file_id    = file_id;
    hdr.type       = static_cast<std::uint8_t>(type);
    hdr.free_top   = PAGE_HEADER_SIZE;
    hdr.free_offset = PAGE_SIZE;
    hdr.slot_count = 0;
    hdr.lsn        = 0;
    std::memcpy(bytes_.data(), &hdr, sizeof(hdr));
    update_crc();
}

PageHeader& Page::header() noexcept {
    return *reinterpret_cast<PageHeader*>(bytes_.data());
}

const PageHeader& Page::header() const noexcept {
    return *reinterpret_cast<const PageHeader*>(bytes_.data());
}

std::uint16_t Page::slot_count() const noexcept {
    return header().slot_count;
}

SlotDescriptor Page::slot(std::uint16_t slot_id) const {
    const std::uint32_t byte_off = slot_dir_byte_offset(slot_id);
    if (byte_off + 4U > PAGE_SIZE)
        return {};
    SlotDescriptor desc;
    std::memcpy(&desc, bytes_.data() + byte_off, sizeof(desc));
    return desc;
}

void Page::set_slot(std::uint16_t slot_id, SlotDescriptor desc) {
    const std::uint32_t byte_off = slot_dir_byte_offset(slot_id);
    if (byte_off + 4U > PAGE_SIZE)
        return;
    std::memcpy(bytes_.data() + byte_off, &desc, sizeof(desc));
}

std::uint16_t Page::free_bytes() const noexcept {
    const auto& hdr = header();
    // Free space = gap between bottom of slot directory and top of record data
    if (hdr.free_offset <= hdr.free_top)
        return 0;
    return static_cast<std::uint16_t>(hdr.free_offset - hdr.free_top);
}

std::vector<std::uint8_t> Page::read_record(std::uint16_t slot_id) const {
    const auto desc = slot(slot_id);
    if (desc.length == 0)
        return {};  // tombstone or empty
    if (desc.offset + desc.length > PAGE_SIZE)
        return {};  // corrupt
    return std::vector<std::uint8_t>(
        bytes_.data() + desc.offset,
        bytes_.data() + desc.offset + desc.length
    );
}

std::optional<std::uint16_t> Page::insert(const std::vector<std::uint8_t>& data) {
    if (data.empty())
        return std::nullopt;

    auto& hdr = header();

    // Space needed: sizeof(SlotDescriptor) for dir entry + data.size() for record
    const std::uint32_t needed = 4U + static_cast<std::uint32_t>(data.size());
    if (free_bytes() < needed)
        return std::nullopt;

    // Assign record space at the top of the record region (grows upward)
    hdr.free_offset = static_cast<std::uint16_t>(hdr.free_offset - data.size());
    std::memcpy(bytes_.data() + hdr.free_offset, data.data(), data.size());

    // Assign slot descriptor at the bottom of the slot directory (grows downward)
    const std::uint16_t new_slot = hdr.slot_count;
    SlotDescriptor desc;
    desc.offset = hdr.free_offset;
    desc.length = static_cast<std::uint16_t>(data.size());
    hdr.free_top = static_cast<std::uint16_t>(hdr.free_top + 4U);
    set_slot(new_slot, desc);
    hdr.slot_count = static_cast<std::uint16_t>(hdr.slot_count + 1U);

    update_crc();
    return new_slot;
}

bool Page::erase(std::uint16_t slot_id) {
    if (slot_id >= slot_count())
        return false;
    auto desc = slot(slot_id);
    if (desc.length == 0)
        return false; // already tombstone
    // Zero the record bytes for security
    if (desc.offset + desc.length <= PAGE_SIZE)
        std::memset(bytes_.data() + desc.offset, 0, desc.length);
    desc.length = 0;
    desc.offset = 0;
    set_slot(slot_id, desc);
    update_crc();
    return true;
}

std::optional<std::uint16_t> Page::update(std::uint16_t slot_id,
                                           const std::vector<std::uint8_t>& data) {
    if (slot_id >= slot_count())
        return std::nullopt;
    auto desc = slot(slot_id);

    if (desc.length == static_cast<std::uint16_t>(data.size()) && desc.length > 0) {
        // In-place update — same size
        std::memcpy(bytes_.data() + desc.offset, data.data(), data.size());
        update_crc();
        return slot_id;
    }

    // Different size: tombstone old slot and try to insert
    erase(slot_id);
    return insert(data);
}

void Page::compact() {
    // Collect all live slots
    struct Live {
        std::vector<std::uint8_t> data;
        std::uint16_t             original_slot;
    };
    std::vector<Live> live;
    live.reserve(slot_count());
    for (std::uint16_t i = 0; i < slot_count(); ++i) {
        auto rec = read_record(i);
        if (!rec.empty())
            live.push_back({std::move(rec), i});
    }

    // Reset page, keeping header metadata
    auto& hdr = header();
    const std::uint32_t pg_id  = hdr.page_id;
    const std::uint8_t  fl_id  = hdr.file_id;
    const std::uint8_t  pg_type= hdr.type;
    const std::uint64_t lsn    = hdr.lsn;

    bytes_.fill(0);
    PageHeader fresh{};
    fresh.page_id    = pg_id;
    fresh.file_id    = fl_id;
    fresh.type       = pg_type;
    fresh.lsn        = lsn;
    fresh.free_top   = PAGE_HEADER_SIZE;
    fresh.free_offset= PAGE_SIZE;
    std::memcpy(bytes_.data(), &fresh, sizeof(fresh));

    // Re-insert live records in order
    for (auto& l : live)
        insert(l.data); // slot_id may differ — caller must rebuild key directory
    update_crc();
}

std::uint16_t Page::live_slot_count() const noexcept {
    std::uint16_t count = 0;
    for (std::uint16_t i = 0; i < slot_count(); ++i) {
        if (slot(i).length > 0)
            ++count;
    }
    return count;
}

void Page::update_crc() noexcept {
    // CRC covers bytes [0..27] (all header fields before crc32c at offset 28)
    auto& hdr = header();
    hdr.crc32c = crc32c_compute(bytes_.data(), 28U);
}

bool Page::verify_crc() const noexcept {
    const auto& hdr = header();
    return crc32c_compute(bytes_.data(), 28U) == hdr.crc32c;
}

} // namespace kor
