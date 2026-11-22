#pragma once

// ---------------------------------------------------------------------------
// Phase 1 — Multi-Chunk File Manager
//
// Presents a single logical page-address space across multiple on-disk files:
//   data.kr              file_id = 0  (created on first write)
//   data.kr-001.chunk    file_id = 1  (auto when data.kr exceeds chunk_limit)
//   data.kr-002.chunk    file_id = 2
//   index.kr             file_id = 255  (index pages only)
//
// Each file is a simple flat array of PAGE_SIZE (8192-byte) pages.
// file_id + page_id uniquely address a page across all chunks.
//
// All public methods are thread-safe.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "kor/page.hpp"

namespace kor {

inline constexpr std::uint8_t  INDEX_FILE_ID     = 255U;
inline constexpr std::uint64_t DEFAULT_CHUNK_LIMIT = 1ULL * 1024ULL * 1024ULL * 1024ULL; // 1 GiB

class ChunkManager {
public:
    // database_dir  : directory where .kr files live (e.g. .kor/mystore/)
    // chunk_limit   : max size of data.kr before creating a new .chunk file
    explicit ChunkManager(std::filesystem::path database_dir,
                          std::uint64_t chunk_limit = DEFAULT_CHUNK_LIMIT);
    ~ChunkManager();

    ChunkManager(const ChunkManager&)            = delete;
    ChunkManager& operator=(const ChunkManager&) = delete;

    // Open / create all existing chunk files
    void open();
    void close();
    bool is_open() const noexcept;

    // Read a page from disk into `out`. Returns false if page does not exist.
    bool read_page(std::uint8_t file_id, std::uint32_t page_id, Page& out);

    // Write a page to disk. Creates new chunk if necessary.
    bool write_page(std::uint8_t file_id, std::uint32_t page_id, const Page& page);

    // Allocate a new page in the data files (not index).
    // Returns (file_id, page_id) for the new page.
    std::pair<std::uint8_t, std::uint32_t> allocate_data_page();

    // Allocate a new index page (file_id = INDEX_FILE_ID).
    std::uint32_t allocate_index_page();

    // fsync a specific file
    void sync(std::uint8_t file_id);

    // fsync all open files
    void sync_all();

    // Number of pages in a given file_id
    std::uint32_t page_count(std::uint8_t file_id) const;

    // Data chunk file count (not counting index.kr)
    std::size_t chunk_count() const;

private:
    struct FileHandle {
        std::fstream      stream;
        std::uint32_t     page_count = 0; // pages already allocated
        std::filesystem::path path;
    };

    std::filesystem::path database_dir_;
    std::uint64_t         chunk_limit_;
    bool                  open_ = false;

    mutable std::mutex                              mutex_;
    std::unordered_map<std::uint8_t, FileHandle>   files_; // file_id → handle

    // Open or create a specific file
    void open_file(std::uint8_t file_id, const std::filesystem::path& path, bool create);

    // Determine path for a given file_id
    std::filesystem::path file_path(std::uint8_t file_id) const;

    // Active data file_id (the one still accepting writes)
    std::uint8_t active_data_file_id() const noexcept;
};

} // namespace kor
