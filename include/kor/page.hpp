#pragma once

// ---------------------------------------------------------------------------
// Phase 1 — 8 KB Slotted Page layout
//
// Page layout (PAGE_SIZE = 8192 bytes):
//   [0  .. 31]   PAGE HEADER  (32 bytes, see PageHeader)
//   [32 .. ...]  SLOT DIRECTORY, grows down: slot[i] = {offset u16, length u16}
//   [... .. end] RECORD DATA, grows up from the end of the page
//   FREE SPACE sits between slot directory tail and record data bottom.
//
// A slot with length == 0 is a tombstone (deleted).
// Records are addressed by stable (file_id u8, page_id u32, slot_id u16).
// ---------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace kor {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t PAGE_SIZE        = 8192U;
inline constexpr std::uint32_t PAGE_HEADER_SIZE = 32U;
inline constexpr std::uint32_t PAGE_MAGIC       = 0x4B425047U; // "KBPG"

enum class PageType : std::uint8_t {
    kFree  = 0,
    kData  = 1,
    kIndex = 2,
};

// ---------------------------------------------------------------------------
// SlotDescriptor — 4 bytes in the slot directory
// ---------------------------------------------------------------------------
struct SlotDescriptor {
    std::uint16_t offset = 0; // byte offset from page start to record data
    std::uint16_t length = 0; // length in bytes; 0 = tombstone
};
static_assert(sizeof(SlotDescriptor) == 4);

// ---------------------------------------------------------------------------
// PageHeader — 32 bytes at offset 0 of every page
// ---------------------------------------------------------------------------
struct PageHeader {
    std::uint32_t magic       = PAGE_MAGIC;
    std::uint32_t page_id     = 0;
    std::uint8_t  file_id     = 0;
    std::uint8_t  type        = static_cast<std::uint8_t>(PageType::kData);
    std::uint16_t slot_count  = 0;         // total slots (including tombstones)
    std::uint16_t free_top    = PAGE_HEADER_SIZE; // bottom edge of slot directory
    std::uint16_t free_offset = PAGE_SIZE; // top edge of record data (grows ↑)
    std::uint64_t lsn         = 0;         // LSN of last write
    std::uint32_t crc32c      = 0;         // CRC32C of bytes [0..27]
    std::uint32_t reserved    = 0;
};
static_assert(sizeof(PageHeader) == 32);

// ---------------------------------------------------------------------------
// PageAddress — stable locator for a record
// ---------------------------------------------------------------------------
struct PageAddress {
    std::uint8_t  file_id = 0;
    std::uint32_t page_id = 0;
    std::uint16_t slot_id = 0;

    bool operator==(const PageAddress& o) const noexcept {
        return file_id == o.file_id && page_id == o.page_id && slot_id == o.slot_id;
    }
};

// ---------------------------------------------------------------------------
// CRC32C (software table-based, no ISA extension required)
// ---------------------------------------------------------------------------
std::uint32_t crc32c_compute(const std::uint8_t* data, std::size_t length) noexcept;

// ---------------------------------------------------------------------------
// Page — one 8 KB in-memory frame mirroring one on-disk page
// ---------------------------------------------------------------------------
class Page {
public:
    using Bytes = std::array<std::uint8_t, PAGE_SIZE>;

    Page();
    explicit Page(std::uint32_t page_id, std::uint8_t file_id = 0,
                  PageType type = PageType::kData);

    // Header
    PageHeader&       header()       noexcept;
    const PageHeader& header() const noexcept;

    // Raw bytes (for I/O)
    Bytes&       bytes()       noexcept { return bytes_; }
    const Bytes& bytes() const noexcept { return bytes_; }

    // Slot directory
    std::uint16_t  slot_count()  const noexcept;
    SlotDescriptor slot(std::uint16_t slot_id) const;
    void           set_slot(std::uint16_t slot_id, SlotDescriptor desc);

    // Free space in bytes available for new data + slot descriptor
    std::uint16_t free_bytes() const noexcept;

    // Record I/O
    std::vector<std::uint8_t> read_record(std::uint16_t slot_id) const;

    // Insert record → returns slot_id; nullopt if page is full
    std::optional<std::uint16_t> insert(const std::vector<std::uint8_t>& data);

    // Tombstone a slot → true if found
    bool erase(std::uint16_t slot_id);

    // In-place update if same size, else tombstone + insert
    // Returns new slot_id (unchanged if in-place)
    std::optional<std::uint16_t> update(std::uint16_t slot_id,
                                        const std::vector<std::uint8_t>& data);

    // Reclaim fragmented space (rewrites all live records compactly)
    void compact();

    // CRC32C over bytes [0..27] (all header fields except crc32c itself)
    void update_crc() noexcept;
    bool verify_crc() const noexcept;

    // Count live (non-tombstone) slots
    std::uint16_t live_slot_count() const noexcept;

private:
    Bytes bytes_{};

    static std::uint32_t slot_dir_byte_offset(std::uint16_t slot_id) noexcept {
        return PAGE_HEADER_SIZE + static_cast<std::uint32_t>(slot_id) * 4U;
    }
};

} // namespace kor
