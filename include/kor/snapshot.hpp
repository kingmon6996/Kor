#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "kor/value.hpp"

namespace kor {

struct SnapshotMetadata {
    std::uint32_t version = 1U;
    std::uint64_t generation = 0;
    std::uint64_t snapshot_lsn = 0;
    std::uint64_t record_count = 0;
    std::uint64_t payload_size = 0;
    std::uint32_t checksum = 0;
};

struct CheckpointIndexDefinition {
    std::string name;
    std::string field;
    std::vector<std::string> fields;
    std::uint8_t type = 1U;
    bool unique = false;
    std::uint64_t generation = 1U;
};

struct CheckpointData {
    std::uint64_t generation = 0U;
    std::uint64_t checkpoint_lsn = 0U;
    std::map<std::string, Value> records;
    std::vector<CheckpointIndexDefinition> indexes;
};

struct CheckpointMetadata {
    std::uint64_t generation = 0U;
    std::uint64_t checkpoint_lsn = 0U;
    std::uint64_t record_count = 0U;
    std::uint64_t encoded_size = 0U;
    std::uint64_t estimated_memory_bytes = 0U;
};

struct CheckpointRecordRef {
    const std::string* key = nullptr;
    const Value* value = nullptr;
};

std::vector<std::uint8_t> serialize_snapshot(std::uint64_t generation,
                                             std::uint64_t snapshot_lsn,
                                             const std::map<std::string, Value>& records);
std::map<std::string, Value> deserialize_snapshot(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> serialize_checkpoint(
    std::uint64_t generation,
    std::uint64_t checkpoint_lsn,
    const std::map<std::string, Value>& records,
    const std::vector<CheckpointIndexDefinition>& indexes);
std::vector<std::uint8_t> serialize_checkpoint_refs(
    std::uint64_t generation,
    std::uint64_t checkpoint_lsn,
    const std::vector<CheckpointRecordRef>& records,
    const std::vector<CheckpointIndexDefinition>& indexes);
CheckpointData deserialize_checkpoint(const std::vector<std::uint8_t>& bytes);
CheckpointMetadata validate_checkpoint(const std::vector<std::uint8_t>& bytes);
std::uint64_t checkpoint_encoded_size(const std::vector<std::uint8_t>& header);

bool write_snapshot_to_database_file(const std::filesystem::path& path,
                                     std::uint64_t generation,
                                     std::uint64_t snapshot_lsn,
                                     const std::map<std::string, Value>& records);
std::map<std::string, Value> read_snapshot_from_database_file(const std::filesystem::path& path);
std::optional<SnapshotMetadata> read_snapshot_metadata(const std::filesystem::path& path);

}  // namespace kor
