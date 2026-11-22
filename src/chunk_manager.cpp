#include "kor/chunk_manager.hpp"

#include <cassert>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace kor {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

std::string chunk_suffix(std::uint8_t file_id) {
    std::ostringstream oss;
    oss << "-" << std::setw(3) << std::setfill('0') << static_cast<int>(file_id) << ".chunk";
    return oss.str();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ChunkManager
// ---------------------------------------------------------------------------

ChunkManager::ChunkManager(std::filesystem::path database_dir,
                            std::uint64_t         chunk_limit)
    : database_dir_(std::move(database_dir)),
      chunk_limit_(chunk_limit) {}

ChunkManager::~ChunkManager() {
    close();
}

std::filesystem::path ChunkManager::file_path(std::uint8_t file_id) const {
    if (file_id == INDEX_FILE_ID)
        return database_dir_ / "index.kr";
    if (file_id == 0)
        return database_dir_ / "data.kr";
    return database_dir_ / ("data.kr" + chunk_suffix(file_id));
}

void ChunkManager::open_file(std::uint8_t file_id,
                              const std::filesystem::path& path,
                              bool create) {
    FileHandle fh;
    fh.path = path;

    auto mode = std::ios::in | std::ios::out | std::ios::binary;
    if (create) mode |= std::ios::trunc;

    fh.stream.open(path, mode);
    if (!fh.stream.is_open()) {
        // Try creating the file first if it does not exist
        if (!std::filesystem::exists(path)) {
            std::fstream create_stream(path, std::ios::out | std::ios::binary);
            create_stream.close();
            fh.stream.open(path, mode & ~std::ios::trunc);
        }
        if (!fh.stream.is_open())
            throw std::runtime_error("ChunkManager: cannot open " + path.string());
    }

    // Determine existing page count
    fh.stream.seekg(0, std::ios::end);
    const auto file_size = static_cast<std::uint64_t>(fh.stream.tellg());
    fh.page_count = static_cast<std::uint32_t>(file_size / PAGE_SIZE);

    files_.emplace(file_id, std::move(fh));
}

void ChunkManager::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_) return;

    std::filesystem::create_directories(database_dir_);

    // Open data.kr (file_id = 0); create if missing
    open_file(0, file_path(0), false);

    // Open any existing .chunk files (file_id 1..254)
    for (std::uint8_t id = 1; id < INDEX_FILE_ID; ++id) {
        const auto p = file_path(id);
        if (std::filesystem::exists(p))
            open_file(id, p, false);
        else
            break;
    }

    // Open index.kr (file_id = 255); create if missing
    open_file(INDEX_FILE_ID, file_path(INDEX_FILE_ID), false);

    open_ = true;
}

void ChunkManager::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return;
    for (auto& [id, fh] : files_)
        fh.stream.close();
    files_.clear();
    open_ = false;
}

bool ChunkManager::is_open() const noexcept {
    return open_;
}

bool ChunkManager::read_page(std::uint8_t file_id,
                              std::uint32_t page_id,
                              Page& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = files_.find(file_id);
    if (it == files_.end()) return false;

    FileHandle& fh = it->second;
    if (page_id >= fh.page_count) return false;

    const std::uint64_t byte_off = static_cast<std::uint64_t>(page_id) * PAGE_SIZE;
    fh.stream.seekg(static_cast<std::streamoff>(byte_off), std::ios::beg);
    if (!fh.stream)
        return false;

    fh.stream.read(reinterpret_cast<char*>(out.bytes().data()), PAGE_SIZE);
    return fh.stream.gcount() == PAGE_SIZE;
}

bool ChunkManager::write_page(std::uint8_t file_id,
                               std::uint32_t page_id,
                               const Page& page) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = files_.find(file_id);
    if (it == files_.end()) return false;

    FileHandle& fh = it->second;
    const std::uint64_t byte_off = static_cast<std::uint64_t>(page_id) * PAGE_SIZE;

    fh.stream.seekp(static_cast<std::streamoff>(byte_off), std::ios::beg);
    fh.stream.write(reinterpret_cast<const char*>(page.bytes().data()), PAGE_SIZE);
    if (!fh.stream) return false;

    if (page_id >= fh.page_count)
        fh.page_count = page_id + 1;

    fh.stream.flush();
    return true;
}

std::uint8_t ChunkManager::active_data_file_id() const noexcept {
    // The highest file_id that is not INDEX_FILE_ID
    std::uint8_t max_id = 0;
    for (const auto& [id, fh] : files_) {
        if (id != INDEX_FILE_ID && id > max_id)
            max_id = id;
    }
    return max_id;
}

std::pair<std::uint8_t, std::uint32_t> ChunkManager::allocate_data_page() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint8_t active = active_data_file_id();

    // Check if we need to roll to a new chunk
    auto& fh = files_.at(active);
    const std::uint64_t current_size =
        static_cast<std::uint64_t>(fh.page_count) * PAGE_SIZE;

    if (current_size >= chunk_limit_ && active < INDEX_FILE_ID - 1) {
        // Create next chunk file
        const std::uint8_t new_id = static_cast<std::uint8_t>(active + 1);
        // unlock before calling open_file which also locks... actually we hold the lock
        // since open_file uses the same mutex_, call it without re-locking
        const auto path = file_path(new_id);
        FileHandle nfh;
        nfh.path = path;
        auto mode = std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc;
        nfh.stream.open(path, mode);
        if (!nfh.stream.is_open()) {
            // Create then reopen
            { std::fstream c(path, std::ios::out | std::ios::binary); }
            nfh.stream.open(path, std::ios::in | std::ios::out | std::ios::binary);
        }
        nfh.page_count = 0;
        files_.emplace(new_id, std::move(nfh));
        active = new_id;
    }

    const std::uint32_t new_page_id = files_.at(active).page_count;
    // Write a blank page to extend the file
    Page blank(new_page_id, active);
    const std::uint64_t byte_off = static_cast<std::uint64_t>(new_page_id) * PAGE_SIZE;
    auto& af = files_.at(active);
    af.stream.seekp(static_cast<std::streamoff>(byte_off), std::ios::beg);
    af.stream.write(reinterpret_cast<const char*>(blank.bytes().data()), PAGE_SIZE);
    af.stream.flush();
    af.page_count = new_page_id + 1;

    return {active, new_page_id};
}

std::uint32_t ChunkManager::allocate_index_page() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& fh = files_.at(INDEX_FILE_ID);
    const std::uint32_t new_page_id = fh.page_count;

    Page blank(new_page_id, INDEX_FILE_ID, PageType::kIndex);
    const std::uint64_t byte_off = static_cast<std::uint64_t>(new_page_id) * PAGE_SIZE;
    fh.stream.seekp(static_cast<std::streamoff>(byte_off), std::ios::beg);
    fh.stream.write(reinterpret_cast<const char*>(blank.bytes().data()), PAGE_SIZE);
    fh.stream.flush();
    fh.page_count = new_page_id + 1;

    return new_page_id;
}

void ChunkManager::sync(std::uint8_t file_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = files_.find(file_id);
    if (it != files_.end())
        it->second.stream.flush();
}

void ChunkManager::sync_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, fh] : files_)
        fh.stream.flush();
}

std::uint32_t ChunkManager::page_count(std::uint8_t file_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = files_.find(file_id);
    if (it == files_.end()) return 0;
    return it->second.page_count;
}

std::size_t ChunkManager::chunk_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& [id, fh] : files_)
        if (id != INDEX_FILE_ID)
            ++count;
    return count;
}

} // namespace kor
