#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "kor/value.hpp"
#include "kor/snapshot.hpp"

namespace kor {

enum class WalOperationType : std::uint8_t {
    kSet = 1,
    kDelete = 2,
    kClear = 3,
    kTransaction = 4,
    kIndexCreate = 5,
    kIndexDrop = 6,
};

struct WalMutation {
    WalOperationType operation = WalOperationType::kSet;
    std::string key;
    Value value;
};

struct WalRecord {
    std::uint64_t lsn = 0;
    WalOperationType operation = WalOperationType::kSet;
    std::string key;
    Value value;
};

struct WalStats {
    std::uint64_t file_size_bytes = 0U;
    std::uint64_t wal_offset = 0U;
    std::uint64_t committed_wal_bytes = 0U;
    std::uint64_t uncommitted_tail_bytes = 0U;
    std::uint64_t checkpoint_bytes = 0U;
    std::uint64_t checkpoint_lsn = 0U;
    std::uint64_t committed_lsn = 0U;
    std::uint64_t metadata_generation = 0U;
    std::uint64_t checkpoint_generation = 0U;
    std::uint64_t records_written = 0U;
    std::uint64_t bytes_written = 0U;
    bool checkpoint_present = false;
    bool compaction_supported = false;
};

std::uint32_t crc32_value(const std::uint8_t* data, std::size_t length) noexcept;
std::vector<std::uint8_t> serialize_wal_record(std::uint64_t lsn,
                                              WalOperationType operation,
                                              const std::string& key,
                                              const Value& value,
                                              std::uint32_t format_version = 1U);
WalRecord deserialize_wal_record(const std::vector<std::uint8_t>& bytes);

class WalLog {
public:
    explicit WalLog(std::filesystem::path database_path = {},
                    std::uint64_t max_memory_bytes = 64ULL * 1024ULL * 1024ULL,
                    bool read_only = false);
    ~WalLog();

    WalLog(const WalLog&) = delete;
    WalLog& operator=(const WalLog&) = delete;
    WalLog(WalLog&&) noexcept = delete;
    WalLog& operator=(WalLog&&) noexcept = delete;

    void open();
    void close();
    bool is_open() const noexcept;
    std::uint64_t next_lsn() const noexcept;
    std::uint64_t committed_lsn() const noexcept;
    std::optional<CheckpointData> read_checkpoint();
    WalStats stats() const;
    bool compact_checkpoint(const std::vector<std::uint8_t>& checkpoint,
                            std::uint64_t checkpoint_lsn);
    bool publish_checkpoint(const std::vector<std::uint8_t>& checkpoint,
                            std::uint64_t checkpoint_lsn,
                            bool require_shrink);
    std::uint64_t append_set(const std::string& key, const Value& value);
    std::uint64_t append_delete(const std::string& key);
    std::uint64_t append_clear();
    std::uint64_t append_transaction(const std::vector<WalMutation>& mutations);
    std::uint64_t append_index_create(const std::string& name,
                                      const std::string& field,
                                      const std::string& index_type,
                                      bool unique,
                                      std::uint64_t generation);
    std::uint64_t append_index_create(const std::string& name,
                                      const std::vector<std::string>& fields,
                                      const std::string& index_type,
                                      bool unique,
                                      std::uint64_t generation);
    std::uint64_t append_index_drop(const std::string& name);
    std::vector<WalRecord> replay();
    void replay_each(const std::function<void(const WalRecord&)>& visitor);

private:
    void initialize_file_if_missing();
    void read_header();
    void write_header();
    void ensure_open();
    std::uint64_t append_record(WalOperationType operation,
                                const std::string& key,
                                const Value& value,
                                std::uint32_t record_format_version = 1U);
    std::filesystem::path database_path_;
    std::fstream stream_;
    std::uint64_t wal_offset_ = 0;
    std::uint64_t records_offset_ = 0;
    std::uint64_t committed_end_offset_ = 0;
    std::uint64_t checkpoint_offset_ = 0U;
    std::uint64_t checkpoint_size_ = 0U;
    std::uint64_t checkpoint_lsn_ = 0U;
    std::uint64_t checkpoint_generation_ = 0U;
    std::uint64_t header_generation_ = 0;
    std::uint64_t next_lsn_ = 1;
    std::uint64_t committed_lsn_ = 0;
    std::uint64_t records_written_ = 0U;
    std::uint64_t bytes_written_ = 0U;
    std::uint64_t max_memory_bytes_ = 64ULL * 1024ULL * 1024ULL;
    std::uint32_t format_version_ = 2U;
    bool embedded_database_ = false;
    bool metadata_degraded_ = false;
    bool read_only_ = false;
    bool open_ = false;
};

}  // namespace kor
