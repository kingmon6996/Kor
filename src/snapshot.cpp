#include "kor/snapshot.hpp"

#include "kor/exceptions.hpp"
#include "kor/serializer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace kor {
namespace {

constexpr std::uint32_t kSnapshotMagic = 0x4B53504FU;
constexpr std::uint64_t kSnapshotVersion = 1ULL;
constexpr std::uint64_t kLegacyCheckpointVersion = 2ULL;
constexpr std::uint64_t kCheckpointVersion = 3ULL;
constexpr std::size_t kSnapshotHeaderSize = 48U;

void write_u32_le(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    if (offset + sizeof(std::uint32_t) > bytes.size()) {
        bytes.resize(offset + sizeof(std::uint32_t), 0U);
    }
    for (std::size_t index = 0; index < sizeof(std::uint32_t); ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU);
    }
}

void write_u64_le(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    if (offset + sizeof(std::uint64_t) > bytes.size()) {
        bytes.resize(offset + sizeof(std::uint64_t), 0U);
    }
    for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU);
    }
}

std::uint32_t read_u32_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + sizeof(std::uint32_t) > bytes.size()) {
        throw SerializationError("Snapshot metadata is truncated.");
    }
    return static_cast<std::uint32_t>(
        static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U));
}

std::uint64_t read_u64_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + sizeof(std::uint64_t) > bytes.size()) {
        throw SerializationError("Snapshot payload is truncated.");
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (8U * index);
    }
    return value;
}

std::uint32_t crc32_value(const std::uint8_t* data, std::size_t length) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (std::size_t bit = 0; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320U;
            } else {
                crc >>= 1U;
            }
        }
    }
    return ~crc;
}

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        throw InitializationError("Unable to open the database file for snapshot access.");
    }

    const auto size = static_cast<std::size_t>(stream.tellg());
    stream.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(size, 0U);
    if (size > 0U) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    }
    return bytes;
}

std::uint32_t crc32_checkpoint(const std::vector<std::uint8_t>& bytes) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index >= 44U && index < kSnapshotHeaderSize) {
            continue;
        }
        crc ^= bytes[index];
        for (std::size_t bit = 0; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320U;
            } else {
                crc >>= 1U;
            }
        }
    }
    return ~crc;
}

void append_u8(std::vector<std::uint8_t>& buffer, std::uint8_t value) {
    buffer.push_back(value);
}

void append_bytes(std::vector<std::uint8_t>& buffer, const std::vector<std::uint8_t>& value) {
    buffer.insert(buffer.end(), value.begin(), value.end());
}

void append_u64(std::vector<std::uint8_t>& buffer, std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
        buffer.push_back(static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU));
    }
}

}  // namespace

std::vector<std::uint8_t> serialize_snapshot(std::uint64_t generation,
                                             std::uint64_t snapshot_lsn,
                                             const std::map<std::string, Value>& records) {
    std::map<std::string, Value> ordered(records.begin(), records.end());
    std::vector<std::uint8_t> payload;
    for (const auto& [key, value] : ordered) {
        validate_key(key);
        validate_value(value);

        const auto value_bytes = serialize_value(value);
        append_u64(payload, static_cast<std::uint64_t>(key.size()));
        payload.insert(payload.end(), key.begin(), key.end());
        append_u64(payload, static_cast<std::uint64_t>(value_bytes.size()));
        append_bytes(payload, value_bytes);
    }

    std::vector<std::uint8_t> encoded(kSnapshotHeaderSize + payload.size(), 0U);
    write_u32_le(encoded, 0U, kSnapshotMagic);
    write_u64_le(encoded, 4U, kSnapshotVersion);
    write_u64_le(encoded, 12U, generation);
    write_u64_le(encoded, 20U, snapshot_lsn);
    write_u64_le(encoded, 28U, static_cast<std::uint64_t>(ordered.size()));
    write_u64_le(encoded, 36U, static_cast<std::uint64_t>(payload.size()));

    const auto checksum = crc32_value(encoded.data(), 44U);
    write_u32_le(encoded, 44U, checksum);
    std::copy(payload.begin(), payload.end(), encoded.begin() + static_cast<std::ptrdiff_t>(kSnapshotHeaderSize));
    return encoded;
}

std::map<std::string, Value> deserialize_snapshot(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kSnapshotHeaderSize) {
        throw SerializationError("Snapshot is too small to contain a valid snapshot header.");
    }

    const auto magic = read_u32_le(bytes, 0U);
    if (magic != kSnapshotMagic) {
        throw DatabaseFormatError("Snapshot magic mismatch.");
    }

    const auto version = read_u64_le(bytes, 4U);
    if (version == kCheckpointVersion) {
        return deserialize_checkpoint(bytes).records;
    }
    if (version != kSnapshotVersion) {
        throw UnsupportedFormatVersionError("Snapshot format version is unsupported.");
    }

    const auto generation = read_u64_le(bytes, 12U);
    const auto snapshot_lsn = read_u64_le(bytes, 20U);
    const auto record_count = read_u64_le(bytes, 28U);
    const auto payload_size = read_u64_le(bytes, 36U);
    const auto checksum = read_u32_le(bytes, 44U);
    if (checksum != crc32_value(bytes.data(), 44U)) {
        throw DatabaseFormatError("Snapshot checksum mismatch.");
    }

    if (bytes.size() != kSnapshotHeaderSize + payload_size) {
        throw DatabaseFormatError("Snapshot payload size mismatch.");
    }

    std::map<std::string, Value> values;
    std::size_t offset = kSnapshotHeaderSize;
    for (std::uint64_t index = 0; index < record_count; ++index) {
        if (offset + sizeof(std::uint64_t) > bytes.size()) {
            throw SerializationError("Snapshot record length is truncated.");
        }
        const auto key_length = read_u64_le(bytes, offset);
        offset += sizeof(std::uint64_t);
        if (offset + key_length > bytes.size()) {
            throw SerializationError("Snapshot key length exceeds available bytes.");
        }
        std::string key(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::size_t>(key_length));
        offset += static_cast<std::size_t>(key_length);

        if (offset + sizeof(std::uint64_t) > bytes.size()) {
            throw SerializationError("Snapshot value length is truncated.");
        }
        const auto value_length = read_u64_le(bytes, offset);
        offset += sizeof(std::uint64_t);
        if (offset + value_length > bytes.size()) {
            throw SerializationError("Snapshot value payload exceeds available bytes.");
        }

        std::vector<std::uint8_t> value_bytes(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                             bytes.begin() + static_cast<std::ptrdiff_t>(offset + value_length));
        offset += static_cast<std::size_t>(value_length);
        values.emplace(key, deserialize_value(value_bytes));
    }

    if (offset != bytes.size()) {
        throw SerializationError("Snapshot trailing bytes remain after decoding all records.");
    }

    (void)generation;
    (void)snapshot_lsn;
    return values;
}

std::vector<std::uint8_t> serialize_checkpoint(
    std::uint64_t generation,
    std::uint64_t checkpoint_lsn,
    const std::map<std::string, Value>& records,
    const std::vector<CheckpointIndexDefinition>& indexes) {
    std::vector<CheckpointRecordRef> references;
    references.reserve(records.size());
    for (const auto& [key, value] : records) {
        references.push_back(CheckpointRecordRef{&key, &value});
    }
    return serialize_checkpoint_refs(generation, checkpoint_lsn, references, indexes);
}

std::vector<std::uint8_t> serialize_checkpoint_refs(
    std::uint64_t generation,
    std::uint64_t checkpoint_lsn,
    const std::vector<CheckpointRecordRef>& records,
    const std::vector<CheckpointIndexDefinition>& indexes) {
    std::vector<std::uint8_t> encoded(kSnapshotHeaderSize, 0U);
    const std::string* previous_key = nullptr;
    for (const auto& record : records) {
        if (record.key == nullptr || record.value == nullptr ||
            (previous_key != nullptr && *previous_key >= *record.key)) {
            throw SerializationError("Checkpoint records must be non-null and ordered by unique key.");
        }
        validate_key(*record.key);
        validate_value(*record.value);
        const auto value_bytes = serialize_value(*record.value);
        append_u64(encoded, static_cast<std::uint64_t>(record.key->size()));
        encoded.insert(encoded.end(), record.key->begin(), record.key->end());
        append_u64(encoded, static_cast<std::uint64_t>(value_bytes.size()));
        append_bytes(encoded, value_bytes);
        previous_key = record.key;
    }

    append_u64(encoded, static_cast<std::uint64_t>(indexes.size()));
    for (const auto& index : indexes) {
        const auto fields = index.fields.empty() ? std::vector<std::string>{index.field}
                                                : index.fields;
        if (index.name.empty() || index.field.empty() || fields.empty() || fields.size() > 4U ||
            fields.front() != index.field || index.generation == 0U ||
            (index.type != 1U && index.type != 2U)) {
            throw SerializationError("Checkpoint contains an invalid secondary-index definition.");
        }
        append_u64(encoded, static_cast<std::uint64_t>(index.name.size()));
        encoded.insert(encoded.end(), index.name.begin(), index.name.end());
        append_u64(encoded, static_cast<std::uint64_t>(index.field.size()));
        encoded.insert(encoded.end(), index.field.begin(), index.field.end());
        append_u64(encoded, static_cast<std::uint64_t>(fields.size()));
        for (const auto& field : fields) {
            append_u64(encoded, static_cast<std::uint64_t>(field.size()));
            encoded.insert(encoded.end(), field.begin(), field.end());
        }
        append_u8(encoded, index.type);
        append_u8(encoded, index.unique ? 1U : 0U);
        append_u64(encoded, index.generation);
    }

    const auto payload_size = encoded.size() - kSnapshotHeaderSize;
    if (payload_size > std::numeric_limits<std::uint64_t>::max()) {
        throw SerializationError("Checkpoint payload size overflowed.");
    }
    write_u32_le(encoded, 0U, kSnapshotMagic);
    write_u64_le(encoded, 4U, kCheckpointVersion);
    write_u64_le(encoded, 12U, generation);
    write_u64_le(encoded, 20U, checkpoint_lsn);
    write_u64_le(encoded, 28U, static_cast<std::uint64_t>(records.size()));
    write_u64_le(encoded, 36U, static_cast<std::uint64_t>(payload_size));
    write_u32_le(encoded, 44U, crc32_checkpoint(encoded));
    return encoded;
}

CheckpointData deserialize_checkpoint(const std::vector<std::uint8_t>& bytes) {
    const auto checkpoint_version = bytes.size() >= kSnapshotHeaderSize
                                       ? read_u64_le(bytes, 4U)
                                       : 0U;
    if (bytes.size() < kSnapshotHeaderSize ||
        read_u32_le(bytes, 0U) != kSnapshotMagic ||
        (checkpoint_version != kLegacyCheckpointVersion && checkpoint_version != kCheckpointVersion)) {
        throw RecoveryError("Checkpoint header is invalid or unsupported.");
    }
    const auto payload_size = read_u64_le(bytes, 36U);
    if (payload_size > std::numeric_limits<std::size_t>::max() - kSnapshotHeaderSize ||
        bytes.size() != kSnapshotHeaderSize + static_cast<std::size_t>(payload_size)) {
        throw RecoveryError("Checkpoint payload length is invalid.");
    }
    if (read_u32_le(bytes, 44U) != crc32_checkpoint(bytes)) {
        throw RecoveryError("Checkpoint checksum mismatch.");
    }

    CheckpointData checkpoint;
    checkpoint.generation = read_u64_le(bytes, 12U);
    checkpoint.checkpoint_lsn = read_u64_le(bytes, 20U);
    const auto record_count = read_u64_le(bytes, 28U);
    std::size_t offset = kSnapshotHeaderSize;
    for (std::uint64_t index = 0; index < record_count; ++index) {
        const auto key_length = read_u64_le(bytes, offset);
        offset += sizeof(std::uint64_t);
        if (key_length > bytes.size() - offset) {
            throw RecoveryError("Checkpoint key exceeds the validated payload.");
        }
        std::string key(reinterpret_cast<const char*>(bytes.data() + offset),
                        static_cast<std::size_t>(key_length));
        offset += static_cast<std::size_t>(key_length);
        if (bytes.size() - offset < sizeof(std::uint64_t)) {
            throw RecoveryError("Checkpoint value length is truncated.");
        }
        const auto value_length = read_u64_le(bytes, offset);
        offset += sizeof(std::uint64_t);
        if (value_length > bytes.size() - offset) {
            throw RecoveryError("Checkpoint value exceeds the validated payload.");
        }
        std::vector<std::uint8_t> value_bytes(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + static_cast<std::size_t>(value_length)));
        offset += static_cast<std::size_t>(value_length);
        try {
            validate_key(key);
            auto value = deserialize_value(value_bytes);
            validate_value(value);
            if (!checkpoint.records.emplace(std::move(key), std::move(value)).second) {
                throw RecoveryError("Checkpoint contains duplicate primary keys.");
            }
        } catch (const Error& error) {
            throw RecoveryError(std::string("Checkpoint record is invalid: ") + error.what());
        }
    }
    if (bytes.size() - offset < sizeof(std::uint64_t)) {
        throw RecoveryError("Checkpoint index-definition count is truncated.");
    }
    const auto index_count = read_u64_le(bytes, offset);
    offset += sizeof(std::uint64_t);
    for (std::uint64_t index = 0; index < index_count; ++index) {
        if (bytes.size() - offset < sizeof(std::uint64_t)) {
            throw RecoveryError("Checkpoint index name length is truncated.");
        }
        const auto name_length = read_u64_le(bytes, offset);
        offset += sizeof(std::uint64_t);
        if (name_length > bytes.size() - offset) {
            throw RecoveryError("Checkpoint index name exceeds the validated payload.");
        }
        CheckpointIndexDefinition definition;
        definition.name.assign(reinterpret_cast<const char*>(bytes.data() + offset),
                               static_cast<std::size_t>(name_length));
        offset += static_cast<std::size_t>(name_length);
        if (bytes.size() - offset < sizeof(std::uint64_t)) {
            throw RecoveryError("Checkpoint index field length is truncated.");
        }
        const auto field_length = read_u64_le(bytes, offset);
        offset += sizeof(std::uint64_t);
        if (field_length > bytes.size() - offset ||
            bytes.size() - offset - static_cast<std::size_t>(field_length) < 10U) {
            throw RecoveryError("Checkpoint index definition is truncated.");
        }
        definition.field.assign(reinterpret_cast<const char*>(bytes.data() + offset),
                                static_cast<std::size_t>(field_length));
        offset += static_cast<std::size_t>(field_length);
        if (checkpoint_version >= kCheckpointVersion) {
            if (bytes.size() - offset < sizeof(std::uint64_t)) {
                throw RecoveryError("Checkpoint composite-index field count is truncated.");
            }
            const auto field_count = read_u64_le(bytes, offset);
            offset += sizeof(std::uint64_t);
            if (field_count == 0U || field_count > 4U) {
                throw RecoveryError("Checkpoint composite-index field count is invalid.");
            }
            for (std::uint64_t field_index = 0; field_index < field_count; ++field_index) {
                if (bytes.size() - offset < sizeof(std::uint64_t)) {
                    throw RecoveryError("Checkpoint composite-index field length is truncated.");
                }
                const auto composite_field_length = read_u64_le(bytes, offset);
                offset += sizeof(std::uint64_t);
                if (composite_field_length > bytes.size() - offset) {
                    throw RecoveryError("Checkpoint composite-index field exceeds the validated payload.");
                }
                definition.fields.emplace_back(
                    reinterpret_cast<const char*>(bytes.data() + offset),
                    static_cast<std::size_t>(composite_field_length));
                offset += static_cast<std::size_t>(composite_field_length);
            }
        } else {
            definition.fields.push_back(definition.field);
        }
        if (bytes.size() - offset < 10U) {
            throw RecoveryError("Checkpoint index definition is truncated.");
        }
        definition.type = bytes[offset++];
        const auto unique = bytes[offset++];
        definition.unique = unique == 1U;
        definition.generation = read_u64_le(bytes, offset);
        offset += sizeof(std::uint64_t);
        if (definition.name.empty() || definition.field.empty() || definition.fields.empty() ||
            definition.fields.front() != definition.field || definition.generation == 0U ||
            (definition.type != 1U && definition.type != 2U) || unique > 1U) {
            throw RecoveryError("Checkpoint contains an invalid secondary-index definition.");
        }
        checkpoint.indexes.push_back(std::move(definition));
    }
    if (offset != bytes.size()) {
        throw RecoveryError("Checkpoint contains trailing bytes.");
    }
    return checkpoint;
}

CheckpointMetadata validate_checkpoint(const std::vector<std::uint8_t>& bytes) {
    const auto checkpoint_version = bytes.size() >= kSnapshotHeaderSize
                                       ? read_u64_le(bytes, 4U)
                                       : 0U;
    if (bytes.size() < kSnapshotHeaderSize ||
        read_u32_le(bytes, 0U) != kSnapshotMagic ||
        (checkpoint_version != kLegacyCheckpointVersion && checkpoint_version != kCheckpointVersion)) {
        throw RecoveryError("Checkpoint header is invalid or unsupported.");
    }
    const auto payload_size = read_u64_le(bytes, 36U);
    if (payload_size > std::numeric_limits<std::size_t>::max() - kSnapshotHeaderSize ||
        bytes.size() != kSnapshotHeaderSize + static_cast<std::size_t>(payload_size)) {
        throw RecoveryError("Checkpoint payload length is invalid.");
    }
    if (read_u32_le(bytes, 44U) != crc32_checkpoint(bytes)) {
        throw RecoveryError("Checkpoint checksum mismatch.");
    }

    CheckpointMetadata metadata;
    metadata.generation = read_u64_le(bytes, 12U);
    metadata.checkpoint_lsn = read_u64_le(bytes, 20U);
    metadata.record_count = read_u64_le(bytes, 28U);
    if (metadata.generation == 0U) {
        throw RecoveryError("Checkpoint generation must be positive.");
    }
    metadata.encoded_size = static_cast<std::uint64_t>(bytes.size());
    std::uint64_t estimated_memory = 0U;
    std::size_t offset = kSnapshotHeaderSize;
    std::string previous_key;
    for (std::uint64_t index = 0; index < metadata.record_count; ++index) {
        const auto key_length = read_u64_le(bytes, offset);
        offset += sizeof(std::uint64_t);
        if (key_length > bytes.size() - offset) {
            throw RecoveryError("Checkpoint key exceeds the validated payload.");
        }
        std::string key(reinterpret_cast<const char*>(bytes.data() + offset),
                        static_cast<std::size_t>(key_length));
        offset += static_cast<std::size_t>(key_length);
        if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint64_t)) {
            throw RecoveryError("Checkpoint value length is truncated.");
        }
        const auto value_length = read_u64_le(bytes, offset);
        offset += sizeof(std::uint64_t);
        if (value_length > bytes.size() - offset) {
            throw RecoveryError("Checkpoint value exceeds the validated payload.");
        }
        std::vector<std::uint8_t> value_bytes(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + static_cast<std::size_t>(value_length)));
        offset += static_cast<std::size_t>(value_length);
        try {
            validate_key(key);
            if (!previous_key.empty() && previous_key >= key) {
                throw RecoveryError("Checkpoint primary keys are not unique and ordered.");
            }
            previous_key = key;
            const auto value = deserialize_value(value_bytes);
            validate_value(value);
            const auto key_size = static_cast<std::uint64_t>(key.size());
            const auto value_size = estimate_value_bytes(value);
            constexpr std::uint64_t record_overhead = 32U;
            if (key_size > std::numeric_limits<std::uint64_t>::max() - value_size ||
                key_size + value_size > std::numeric_limits<std::uint64_t>::max() - record_overhead ||
                estimated_memory > std::numeric_limits<std::uint64_t>::max() -
                                       (key_size + value_size + record_overhead)) {
                throw RecoveryError("Checkpoint memory estimate overflowed.");
            }
            estimated_memory += key_size + value_size + record_overhead;
        } catch (const Error& error) {
            throw RecoveryError(std::string("Checkpoint record is invalid: ") + error.what());
        }
    }
    if (bytes.size() - offset < sizeof(std::uint64_t)) {
        throw RecoveryError("Checkpoint index-definition count is truncated.");
    }
    const auto index_count = read_u64_le(bytes, offset);
    offset += sizeof(std::uint64_t);
    std::set<std::string> names;
    for (std::uint64_t index = 0; index < index_count; ++index) {
        const auto name_length = read_u64_le(bytes, offset);
        offset += sizeof(std::uint64_t);
        if (name_length > bytes.size() - offset) {
            throw RecoveryError("Checkpoint index name exceeds the validated payload.");
        }
        std::string name(reinterpret_cast<const char*>(bytes.data() + offset),
                         static_cast<std::size_t>(name_length));
        offset += static_cast<std::size_t>(name_length);
        if (bytes.size() - offset < sizeof(std::uint64_t)) {
            throw RecoveryError("Checkpoint index field length is truncated.");
        }
        const auto field_length = read_u64_le(bytes, offset);
        offset += sizeof(std::uint64_t);
        if (field_length > bytes.size() - offset ||
            bytes.size() - offset - static_cast<std::size_t>(field_length) < 10U) {
            throw RecoveryError("Checkpoint index definition is truncated.");
        }
        std::string field(reinterpret_cast<const char*>(bytes.data() + offset),
                          static_cast<std::size_t>(field_length));
        offset += static_cast<std::size_t>(field_length);
        std::vector<std::string> fields;
        if (checkpoint_version >= kCheckpointVersion) {
            if (bytes.size() - offset < sizeof(std::uint64_t)) {
                throw RecoveryError("Checkpoint composite-index field count is truncated.");
            }
            const auto field_count = read_u64_le(bytes, offset);
            offset += sizeof(std::uint64_t);
            if (field_count == 0U || field_count > 4U) {
                throw RecoveryError("Checkpoint composite-index field count is invalid.");
            }
            fields.reserve(static_cast<std::size_t>(field_count));
            for (std::uint64_t field_index = 0; field_index < field_count; ++field_index) {
                if (bytes.size() - offset < sizeof(std::uint64_t)) {
                    throw RecoveryError("Checkpoint composite-index field length is truncated.");
                }
                const auto composite_field_length = read_u64_le(bytes, offset);
                offset += sizeof(std::uint64_t);
                if (composite_field_length > bytes.size() - offset) {
                    throw RecoveryError("Checkpoint composite-index field exceeds the validated payload.");
                }
                fields.emplace_back(reinterpret_cast<const char*>(bytes.data() + offset),
                                    static_cast<std::size_t>(composite_field_length));
                offset += static_cast<std::size_t>(composite_field_length);
            }
        } else {
            fields.push_back(field);
        }
        if (bytes.size() - offset < 10U) {
            throw RecoveryError("Checkpoint index definition is truncated.");
        }
        const auto type = bytes[offset++];
        const auto unique = bytes[offset++];
        const auto generation = read_u64_le(bytes, offset);
        offset += sizeof(std::uint64_t);
        if (name.empty() || field.empty() || fields.empty() || fields.front() != field ||
            !std::all_of(fields.begin(), fields.end(), [](const std::string& value) {
                return !value.empty() && is_valid_utf8(value);
            }) ||
            (type != 1U && type != 2U) || unique > 1U || generation == 0U ||
            !names.insert(name).second) {
            throw RecoveryError("Checkpoint contains an invalid secondary-index definition.");
        }
        constexpr std::uint64_t index_overhead = 64U;
        std::uint64_t index_bytes = static_cast<std::uint64_t>(name.size()) + index_overhead;
        for (const auto& indexed_field : fields) {
            if (index_bytes > std::numeric_limits<std::uint64_t>::max() - indexed_field.size()) {
                throw RecoveryError("Checkpoint memory estimate overflowed.");
            }
            index_bytes += static_cast<std::uint64_t>(indexed_field.size());
        }
        if (estimated_memory > std::numeric_limits<std::uint64_t>::max() - index_bytes) {
            throw RecoveryError("Checkpoint memory estimate overflowed.");
        }
        estimated_memory += index_bytes;
    }
    if (offset != bytes.size()) {
        throw RecoveryError("Checkpoint contains trailing bytes.");
    }
    metadata.estimated_memory_bytes = estimated_memory;
    return metadata;
}

std::uint64_t checkpoint_encoded_size(const std::vector<std::uint8_t>& header) {
    const auto version = header.size() >= kSnapshotHeaderSize
                             ? read_u64_le(header, 4U)
                             : 0U;
    if (header.size() < kSnapshotHeaderSize ||
        read_u32_le(header, 0U) != kSnapshotMagic ||
        (version != kLegacyCheckpointVersion && version != kCheckpointVersion)) {
        throw RecoveryError("Checkpoint header is invalid or unsupported.");
    }
    const auto payload_size = read_u64_le(header, 36U);
    if (payload_size > std::numeric_limits<std::uint64_t>::max() - kSnapshotHeaderSize) {
        throw RecoveryError("Checkpoint length overflowed.");
    }
    return kSnapshotHeaderSize + payload_size;
}

bool write_snapshot_to_database_file(const std::filesystem::path& path,
                                     std::uint64_t generation,
                                     std::uint64_t snapshot_lsn,
                                     const std::map<std::string, Value>& records) {
    const auto resolved = std::filesystem::absolute(path);
    if (std::filesystem::exists(resolved) && std::filesystem::file_size(resolved) != 0U) {
        throw PersistenceError(
            "Standalone snapshot serialization cannot overwrite an existing file; publish checkpoints through the WAL.");
    }
    const auto parent = resolved.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream stream(resolved, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        throw InitializationError("Unable to open the database file for snapshot publication.");
    }

    const auto snapshot_bytes = serialize_snapshot(generation, snapshot_lsn, records);
    stream.write(reinterpret_cast<const char*>(snapshot_bytes.data()),
                 static_cast<std::streamsize>(snapshot_bytes.size()));
    if (!stream.good()) {
        throw PersistenceError("Database snapshot write failed before it was fully published.");
    }
    stream.close();
    return true;
}

std::map<std::string, Value> read_snapshot_from_database_file(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return {};
    }
    const auto bytes = read_file_bytes(path);
    if (bytes.empty()) {
        return {};
    }
    return deserialize_snapshot(bytes);
}

std::optional<SnapshotMetadata> read_snapshot_metadata(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }

    const auto bytes = read_file_bytes(path);
    if (bytes.size() < kSnapshotHeaderSize) {
        return std::nullopt;
    }

    try {
        const auto magic = read_u32_le(bytes, 0U);
        if (magic != kSnapshotMagic) {
            return std::nullopt;
        }

        const auto version = read_u64_le(bytes, 4U);
        if (version != kSnapshotVersion && version != kCheckpointVersion) {
            return std::nullopt;
        }

        const auto generation = read_u64_le(bytes, 12U);
        const auto snapshot_lsn = read_u64_le(bytes, 20U);
        const auto record_count = read_u64_le(bytes, 28U);
        const auto payload_size = read_u64_le(bytes, 36U);
        const auto checksum = read_u32_le(bytes, 44U);
        if ((version == kSnapshotVersion && checksum != crc32_value(bytes.data(), 44U)) ||
            (version == kCheckpointVersion && checksum != crc32_checkpoint(bytes))) {
            return std::nullopt;
        }
        if (bytes.size() != kSnapshotHeaderSize + payload_size) {
            return std::nullopt;
        }

        SnapshotMetadata metadata;
        metadata.version = static_cast<std::uint32_t>(version);
        metadata.generation = generation;
        metadata.snapshot_lsn = snapshot_lsn;
        metadata.record_count = record_count;
        metadata.payload_size = payload_size;
        metadata.checksum = checksum;
        return metadata;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace kor
