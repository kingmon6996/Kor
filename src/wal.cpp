#include "kor/wal.hpp"

#include "kor/exceptions.hpp"
#include "kor/database_file.hpp"
#include "kor/serializer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace kor {
namespace {

constexpr std::uint32_t kWalRecordMagic = 0x57414F4BU;
constexpr std::uint32_t kWalRecordFormatVersion = 1U;
constexpr std::uint32_t kWalIndexRecordFormatVersion = 2U;
constexpr std::uint32_t kLegacyWalFormatVersion = 1U;
constexpr std::uint32_t kWalFormatVersion = 2U;
constexpr std::uint32_t kCheckpointWalFormatVersion = 3U;
constexpr std::uint64_t kWalHeaderSize = 64ULL;
constexpr std::uint64_t kDualWalHeaderSize = 2ULL * kWalHeaderSize;
constexpr std::uint64_t kMaxWalRecordSize = 16ULL * 1024ULL * 1024ULL;
constexpr std::array<char, 8> kHeaderMagic = {'K', 'O', 'R', 'F', 'I', 'L', 'E', '\0'};

std::uint32_t read_u32_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + sizeof(std::uint32_t) > bytes.size()) {
        throw SerializationError("Malformed WAL record encountered.");
    }
    return static_cast<std::uint32_t>(
        static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U));
}

std::uint64_t read_u64_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + sizeof(std::uint64_t) > bytes.size()) {
        throw SerializationError("Malformed WAL record encountered.");
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (8U * index);
    }
    return value;
}

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

std::uint32_t crc32_value_impl(const std::uint8_t* data, std::size_t length) noexcept {
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

void read_header_from_stream(std::istream& stream, std::vector<std::uint8_t>& header, std::uint64_t offset) {
    header.resize(kWalHeaderSize, 0U);
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (stream.gcount() != static_cast<std::streamsize>(header.size())) {
        throw RecoveryError("Database WAL header is incomplete or corrupted.");
    }
}

void write_header_to_stream(std::ostream& stream,
                            std::uint64_t next_lsn,
                            std::uint64_t committed_lsn,
                            std::uint64_t generation,
                            std::uint64_t offset,
                            std::uint32_t format_version = kWalFormatVersion,
                            std::uint64_t checkpoint_offset = 0U) {
    std::vector<std::uint8_t> header(kWalHeaderSize, 0U);
    std::copy(kHeaderMagic.begin(), kHeaderMagic.end(), header.begin());

    write_u32_le(header, 8, format_version);
    write_u64_le(header, 12, kWalHeaderSize);
    write_u64_le(header, 20, next_lsn);
    write_u64_le(header, 28, committed_lsn);
    write_u64_le(header, 36, generation);
    if (format_version == kCheckpointWalFormatVersion) {
        write_u64_le(header, 44, checkpoint_offset);
    }

    const auto checksum = crc32_value_impl(header.data(), 56U);
    write_u32_le(header, 56, checksum);

    stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
}

std::uint64_t read_file_size(std::fstream& stream) {
    stream.clear();
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size < 0) {
        throw PersistenceError("Unable to determine the database file size.");
    }
    return static_cast<std::uint64_t>(size);
}

void read_header_from_bytes(const std::vector<std::uint8_t>& header,
                           std::uint64_t& next_lsn,
                           std::uint64_t& committed_lsn,
                           std::uint64_t& generation,
                           std::uint32_t& version,
                           std::uint64_t& checkpoint_offset) {
    if (header.size() != kWalHeaderSize) {
        throw RecoveryError("Database WAL header size is invalid.");
    }

    if (std::memcmp(header.data(), kHeaderMagic.data(), kHeaderMagic.size()) != 0) {
        throw RecoveryError("Database WAL header magic is invalid.");
    }

    const auto expected_checksum = read_u32_le(header, 56U);
    const auto actual_checksum = crc32_value_impl(header.data(), 56U);
    if (expected_checksum != actual_checksum) {
        throw RecoveryError("Database WAL header checksum mismatch.");
    }

    const auto stored_version = read_u32_le(header, 8U);
    const auto wal_offset = read_u64_le(header, 12U);
    next_lsn = read_u64_le(header, 20U);
    committed_lsn = read_u64_le(header, 28U);

    version = stored_version;
    generation = read_u64_le(header, 36U);
    checkpoint_offset = version == kCheckpointWalFormatVersion ? read_u64_le(header, 44U) : 0U;
    if ((version != kLegacyWalFormatVersion && version != kWalFormatVersion &&
         version != kCheckpointWalFormatVersion) ||
        wal_offset != kWalHeaderSize || next_lsn == 0U ||
        committed_lsn == std::numeric_limits<std::uint64_t>::max() ||
        next_lsn != committed_lsn + 1U) {
        throw RecoveryError("Database WAL header version is unsupported.");
    }
}

void sync_database_file(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw PersistenceError("Unable to open the database file for durable synchronization.");
    }
    const BOOL flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (flushed == 0) {
        throw PersistenceError("The operating system could not durably synchronize the database file.");
    }
#else
    const int descriptor = ::open(path.c_str(), O_RDWR);
    if (descriptor < 0) {
        throw PersistenceError("Unable to open the database file for durable synchronization.");
    }
    const int result = ::fsync(descriptor);
    ::close(descriptor);
    if (result != 0) {
        throw PersistenceError("The operating system could not durably synchronize the database file.");
    }
#endif
}

std::uint64_t record_size(std::uint64_t key_length, std::uint64_t value_length) {
    constexpr std::uint64_t header_size = 40ULL;
    if (key_length > kMaxWalRecordSize || value_length > kMaxWalRecordSize ||
        key_length > kMaxWalRecordSize - value_length ||
        header_size + key_length + value_length + 4ULL > kMaxWalRecordSize) {
        throw TransactionTooLargeError("A WAL record exceeds the 16 MiB internal limit.");
    }
    return header_size + key_length + value_length + 4ULL;
}

std::uint64_t database_wal_offset(const std::filesystem::path& path, bool& embedded) {
    std::ifstream stream(path, std::ios::binary);
    std::array<std::uint8_t, 64> prefix{};
    stream.read(reinterpret_cast<char*>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
    if (stream.gcount() < 8) {
        embedded = false;
        return 0ULL;
    }

    static constexpr std::array<std::uint8_t, 8> database_magic = {'K', 'O', 'R', 'D', 'B', 0, 0, 0};
    if (!std::equal(database_magic.begin(), database_magic.end(), prefix.begin())) {
        embedded = false;
        return 0ULL;
    }
    if (!database_file_is_valid(path)) {
        throw RecoveryError("The embedded WAL belongs to an invalid Kor database file.");
    }

    const auto config_offset = read_u64_le(std::vector<std::uint8_t>(prefix.begin(), prefix.end()), 44U);
    const auto config_length = read_u32_le(std::vector<std::uint8_t>(prefix.begin(), prefix.end()), 52U);
    embedded = true;
    return config_offset + static_cast<std::uint64_t>(config_length);
}

}  // namespace

std::uint32_t crc32_value(const std::uint8_t* data, std::size_t length) noexcept {
    return crc32_value_impl(data, length);
}

std::vector<std::uint8_t> serialize_wal_record(std::uint64_t lsn,
                                              WalOperationType operation,
                                              const std::string& key,
                                              const Value& value,
                                              std::uint32_t format_version) {
    if (lsn == 0U) {
        throw SerializationError("WAL record LSN must be positive.");
    }
    if ((format_version != kWalRecordFormatVersion && format_version != kWalIndexRecordFormatVersion) ||
        (format_version == kWalRecordFormatVersion &&
         static_cast<std::uint32_t>(operation) > static_cast<std::uint32_t>(WalOperationType::kTransaction)) ||
        (format_version == kWalIndexRecordFormatVersion &&
         static_cast<std::uint32_t>(operation) > static_cast<std::uint32_t>(WalOperationType::kIndexDrop))) {
        throw SerializationError("WAL operation is not valid for the requested record format.");
    }
    const auto value_bytes = serialize_value(value);
    const std::uint64_t key_length = static_cast<std::uint64_t>(key.size());
    const std::uint64_t value_length = static_cast<std::uint64_t>(value_bytes.size());
    std::uint64_t total_size = 0;
    try {
        total_size = record_size(key_length, value_length);
    } catch (const TransactionTooLargeError& error) {
        throw RecoveryError(std::string("A committed WAL record exceeds the supported size: ") + error.what());
    }

    std::vector<std::uint8_t> record;
    record.resize(static_cast<std::size_t>(total_size), 0U);
    write_u32_le(record, 0U, kWalRecordMagic);
    write_u32_le(record, 4U, format_version);
    write_u32_le(record, 8U, static_cast<std::uint32_t>(operation));
    write_u32_le(record, 12U, 0U);
    write_u64_le(record, 16U, lsn);
    write_u64_le(record, 24U, key_length);
    write_u64_le(record, 32U, value_length);

    std::copy(key.begin(), key.end(), record.begin() + 40);
    std::copy(value_bytes.begin(), value_bytes.end(), record.begin() + static_cast<std::ptrdiff_t>(40 + key_length));

    const auto checksum = crc32_value_impl(record.data(), record.size() - 4U);
    write_u32_le(record, record.size() - 4U, checksum);
    return record;
}

WalRecord deserialize_wal_record(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 4U + 4U + 4U + 4U + 8U + 8U + 8U + 4U) {
        throw SerializationError("WAL record is too short to be valid.");
    }

    if (read_u32_le(bytes, 0U) != kWalRecordMagic) {
        throw SerializationError("WAL record magic is invalid.");
    }

    const auto version = read_u32_le(bytes, 4U);
    if (version != kWalRecordFormatVersion && version != kWalIndexRecordFormatVersion) {
        throw SerializationError("Unsupported WAL version.");
    }

    const auto op_code = read_u32_le(bytes, 8U);
    if (op_code < static_cast<std::uint32_t>(WalOperationType::kSet) ||
        op_code > static_cast<std::uint32_t>(version == kWalRecordFormatVersion
                                                 ? WalOperationType::kTransaction
                                                 : WalOperationType::kIndexDrop)) {
        throw SerializationError("WAL record operation type is invalid.");
    }
    const auto op_value = static_cast<WalOperationType>(op_code);
    const auto lsn = read_u64_le(bytes, 16U);
    const auto key_length = read_u64_le(bytes, 24U);
    const auto value_length = read_u64_le(bytes, 32U);
    const auto total_size = record_size(key_length, value_length);

    if (lsn == 0U || bytes.size() != total_size) {
        throw SerializationError("WAL record length does not match the payload dimensions.");
    }

    const auto expected_checksum = read_u32_le(bytes, static_cast<std::size_t>(bytes.size() - 4U));
    const auto actual_checksum = crc32_value_impl(bytes.data(), bytes.size() - 4U);
    if (expected_checksum != actual_checksum) {
        throw SerializationError("WAL record checksum mismatch.");
    }

    const auto key_start = 40U;
    const auto value_start = key_start + static_cast<std::size_t>(key_length);
    const std::string key(reinterpret_cast<const char*>(bytes.data() + key_start), static_cast<std::size_t>(key_length));
    std::vector<std::uint8_t> value_bytes(bytes.begin() + static_cast<std::ptrdiff_t>(value_start),
                                         bytes.begin() + static_cast<std::ptrdiff_t>(value_start + value_length));

    WalRecord record;
    record.lsn = lsn;
    record.operation = op_value;
    record.key = key;
    record.value = deserialize_value(value_bytes);
    return record;
}

WalLog::WalLog(std::filesystem::path database_path,
               std::uint64_t max_memory_bytes,
               bool read_only)
    : database_path_(std::move(database_path)),
      max_memory_bytes_(max_memory_bytes),
      read_only_(read_only) {
    if (!database_path_.empty()) {
        open();
    }
}

WalLog::~WalLog() {
    close();
}

void WalLog::open() {
    if (database_path_.empty()) {
        return;
    }
    metadata_degraded_ = false;
    checkpoint_offset_ = 0U;
    checkpoint_size_ = 0U;
    checkpoint_lsn_ = 0U;
    checkpoint_generation_ = 0U;
    if (!read_only_ && !database_path_.parent_path().empty()) {
        std::filesystem::create_directories(database_path_.parent_path());
    }

    const bool existed = std::filesystem::exists(database_path_);
    if (read_only_ && !existed) {
        throw PersistenceError("Read-only database file does not exist.");
    }
    if (read_only_ &&
        (std::filesystem::is_symlink(database_path_) ||
         !std::filesystem::is_regular_file(database_path_))) {
        throw PersistenceError("Read-only mode requires an existing regular database file.");
    }
    if (!existed) {
        std::ofstream create(database_path_, std::ios::binary);
        if (!create.is_open()) {
            throw PersistenceError("Unable to create WAL database file.");
        }
    }
    stream_.open(database_path_,
                 read_only_ ? (std::ios::binary | std::ios::in)
                            : (std::ios::binary | std::ios::in | std::ios::out));
    if (!stream_.is_open()) {
        if (existed) {
            throw PersistenceError("Unable to open the existing database file without modifying it.");
        }
        stream_.open(database_path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    }
    if (!stream_.is_open()) {
        throw PersistenceError("Unable to open WAL database file.");
    }

    wal_offset_ = database_wal_offset(database_path_, embedded_database_);
    const auto size = read_file_size(stream_);
    if (size == wal_offset_) {
        initialize_file_if_missing();
        open_ = true;
        return;
    }
    if (size < wal_offset_ + kWalHeaderSize) {
        throw RecoveryError("The database file ends before its WAL metadata header.");
    }

    std::vector<std::uint8_t> first_header;
    read_header_from_stream(stream_, first_header, wal_offset_);
    const bool has_second_slot = size >= wal_offset_ + kDualWalHeaderSize;
    std::vector<std::uint8_t> second_header;
    if (has_second_slot) {
        read_header_from_stream(stream_, second_header, wal_offset_ + kWalHeaderSize);
    }
    const auto is_dual_slot_version = [](const std::vector<std::uint8_t>& header) {
        if (header.size() != kWalHeaderSize ||
            std::memcmp(header.data(), kHeaderMagic.data(), kHeaderMagic.size()) != 0) {
            return false;
        }
        const auto version = read_u32_le(header, 8U);
        return version == kWalFormatVersion || version == kCheckpointWalFormatVersion;
    };
    if (is_dual_slot_version(first_header) || (has_second_slot && is_dual_slot_version(second_header))) {
        if (!has_second_slot) {
            throw RecoveryError("The versioned WAL metadata slots are truncated.");
        }

        bool first_valid = false;
        bool second_valid = false;
        std::uint64_t first_next = 0;
        std::uint64_t first_committed = 0;
        std::uint64_t first_generation = 0;
        std::uint64_t first_checkpoint_offset = 0;
        std::uint32_t first_format = 0;
        std::uint64_t second_next = 0;
        std::uint64_t second_committed = 0;
        std::uint64_t second_generation = 0;
        std::uint64_t second_checkpoint_offset = 0;
        std::uint32_t second_format = 0;
        try {
            read_header_from_bytes(first_header, first_next, first_committed, first_generation,
                                   first_format, first_checkpoint_offset);
            first_valid = first_format == kWalFormatVersion ||
                          first_format == kCheckpointWalFormatVersion;
        } catch (const Error&) {
        }
        try {
            read_header_from_bytes(second_header, second_next, second_committed, second_generation,
                                   second_format, second_checkpoint_offset);
            second_valid = second_format == kWalFormatVersion ||
                           second_format == kCheckpointWalFormatVersion;
        } catch (const Error&) {
        }
        if (!first_valid && !second_valid) {
            throw RecoveryError("Both WAL metadata slots are invalid.");
        }
        if (first_valid && second_valid) {
            const auto generation_gap = first_generation > second_generation
                                            ? first_generation - second_generation
                                            : second_generation - first_generation;
            if (generation_gap != 1U) {
                throw RecoveryError("WAL metadata slot generations are inconsistent.");
            }
        } else {
            metadata_degraded_ = true;
        }
        if (second_valid && (!first_valid || second_generation > first_generation)) {
            next_lsn_ = second_next;
            committed_lsn_ = second_committed;
            header_generation_ = second_generation;
            format_version_ = second_format;
            checkpoint_offset_ = second_checkpoint_offset;
        } else {
            next_lsn_ = first_next;
            committed_lsn_ = first_committed;
            header_generation_ = first_generation;
            format_version_ = first_format;
            checkpoint_offset_ = first_checkpoint_offset;
        }
        records_offset_ = wal_offset_ + kDualWalHeaderSize;
        if (format_version_ == kCheckpointWalFormatVersion) {
            if (checkpoint_offset_ < records_offset_ || checkpoint_offset_ >= size) {
                throw RecoveryError("Checkpoint region offset is outside the database file.");
            }
            std::vector<std::uint8_t> checkpoint_header(48U, 0U);
            stream_.clear();
            stream_.seekg(static_cast<std::streamoff>(checkpoint_offset_), std::ios::beg);
            stream_.read(reinterpret_cast<char*>(checkpoint_header.data()),
                         static_cast<std::streamsize>(checkpoint_header.size()));
            if (stream_.gcount() != static_cast<std::streamsize>(checkpoint_header.size())) {
                throw RecoveryError("Checkpoint header is truncated.");
            }
            checkpoint_size_ = checkpoint_encoded_size(checkpoint_header);
            if (checkpoint_size_ > size - checkpoint_offset_) {
                throw RecoveryError("Checkpoint region extends beyond the database file.");
            }
            checkpoint_generation_ = read_u64_le(checkpoint_header, 12U);
            checkpoint_lsn_ = read_u64_le(checkpoint_header, 20U);
            if (checkpoint_lsn_ > committed_lsn_) {
                throw RecoveryError("Checkpoint LSN exceeds the committed WAL boundary.");
            }
            records_offset_ = checkpoint_offset_ + checkpoint_size_;
        }
    } else {
        std::uint64_t ignored_checkpoint_offset = 0U;
        read_header_from_bytes(first_header, next_lsn_, committed_lsn_, header_generation_,
                               format_version_, ignored_checkpoint_offset);
        if (format_version_ != kLegacyWalFormatVersion) {
            throw RecoveryError("The WAL metadata format is unsupported.");
        }
        records_offset_ = wal_offset_ + kWalHeaderSize;
    }
    open_ = true;
    replay_each([](const WalRecord&) {});
}

void WalLog::close() {
    if (stream_.is_open()) {
        if (!read_only_) {
            stream_.flush();
        }
        stream_.close();
    }
    open_ = false;
}

bool WalLog::is_open() const noexcept {
    return open_ && stream_.is_open();
}

std::uint64_t WalLog::next_lsn() const noexcept {
    return next_lsn_;
}

std::uint64_t WalLog::committed_lsn() const noexcept {
    return committed_lsn_;
}

std::optional<CheckpointData> WalLog::read_checkpoint() {
    ensure_open();
    if (format_version_ != kCheckpointWalFormatVersion) {
        return std::nullopt;
    }
    if (checkpoint_size_ > std::numeric_limits<std::size_t>::max() ||
        checkpoint_size_ > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw RecoveryError("Checkpoint is too large for this process.");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(checkpoint_size_), 0U);
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(checkpoint_offset_), std::ios::beg);
    stream_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream_.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw RecoveryError("Checkpoint region is truncated.");
    }
    const auto metadata = validate_checkpoint(bytes);
    if (metadata.checkpoint_lsn != checkpoint_lsn_) {
        throw RecoveryError("Checkpoint LSN does not match its published metadata.");
    }
    if (max_memory_bytes_ != 0U &&
        (checkpoint_size_ > max_memory_bytes_ ||
         metadata.estimated_memory_bytes > max_memory_bytes_ - checkpoint_size_)) {
        throw MemoryLimitExceededError(
            "Checkpoint recovery staging exceeds the configured memory budget.");
    }
    return deserialize_checkpoint(bytes);
}

WalStats WalLog::stats() const {
    WalStats result;
    result.file_size_bytes = std::filesystem::file_size(database_path_);
    result.wal_offset = wal_offset_;
    result.committed_wal_bytes = committed_end_offset_ >= records_offset_
                                     ? committed_end_offset_ - records_offset_
                                     : 0U;
    const auto committed_end = committed_end_offset_;
    const auto file_end = result.file_size_bytes;
    result.uncommitted_tail_bytes = file_end > committed_end ? file_end - committed_end : 0U;
    result.checkpoint_bytes = checkpoint_size_;
    result.checkpoint_lsn = checkpoint_lsn_;
    result.committed_lsn = committed_lsn_;
    result.metadata_generation = header_generation_;
    result.checkpoint_present = format_version_ == kCheckpointWalFormatVersion;
    result.compaction_supported = embedded_database_ && format_version_ != kLegacyWalFormatVersion;
    result.checkpoint_generation = checkpoint_generation_;
    result.records_written = records_written_;
    result.bytes_written = bytes_written_;
    return result;
}

bool WalLog::compact_checkpoint(const std::vector<std::uint8_t>& checkpoint,
                               std::uint64_t checkpoint_lsn) {
    return publish_checkpoint(checkpoint, checkpoint_lsn, true);
}

bool WalLog::publish_checkpoint(const std::vector<std::uint8_t>& checkpoint,
                                std::uint64_t checkpoint_lsn,
                                bool require_shrink) {
    ensure_open();
    if (read_only_) {
        throw PersistenceError("Cannot publish a checkpoint through a read-only WAL.");
    }
    if (!embedded_database_) {
        throw PersistenceError("Compaction is supported only for an initialized embedded .kr database.");
    }
    if (format_version_ == kLegacyWalFormatVersion) {
        throw PersistenceError("Safe compaction requires dual-slot WAL metadata; legacy WAL files are not rewritten.");
    }
    if (checkpoint_lsn != committed_lsn_) {
        throw PersistenceError("A compaction checkpoint must cover the current committed LSN.");
    }
    if (header_generation_ > std::numeric_limits<std::uint64_t>::max() - 4U) {
        throw PersistenceError("WAL metadata generation is exhausted.");
    }
    const auto decoded = validate_checkpoint(checkpoint);
    if (decoded.checkpoint_lsn != checkpoint_lsn) {
        throw PersistenceError("Compaction checkpoint LSN does not match the current WAL boundary.");
    }
    const std::uint64_t compacted_offset = wal_offset_ + kDualWalHeaderSize;
    if (checkpoint.size() > std::numeric_limits<std::uint64_t>::max() - compacted_offset) {
        throw PersistenceError("Compacted database file size would overflow.");
    }
    const auto target_size = compacted_offset + static_cast<std::uint64_t>(checkpoint.size());
    const auto file_size = read_file_size(stream_);
    if (require_shrink && target_size >= file_size) {
        return false;
    }
    if (checkpoint.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) ||
        file_size > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw PersistenceError("Compaction region exceeds supported file-offset limits.");
    }

    auto write_bytes = [this](std::uint64_t offset, const std::vector<std::uint8_t>& bytes) {
        stream_.clear();
        stream_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        stream_.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        stream_.flush();
        if (!stream_.good()) {
            throw PersistenceError("Compaction checkpoint write failed.");
        }
        sync_database_file(database_path_);
    };
    auto publish_header = [this]() {
        write_header();
        stream_.flush();
        if (!stream_.good()) {
            throw PersistenceError("Compaction metadata publication failed.");
        }
        sync_database_file(database_path_);
    };

    try {
        const auto append_offset = file_size;
        if (checkpoint.size() > std::numeric_limits<std::uint64_t>::max() - append_offset) {
            throw PersistenceError("Appended checkpoint offset would overflow.");
        }
        write_bytes(append_offset, checkpoint);
        format_version_ = kCheckpointWalFormatVersion;
        checkpoint_offset_ = append_offset;
        checkpoint_size_ = static_cast<std::uint64_t>(checkpoint.size());
        checkpoint_lsn_ = checkpoint_lsn;
        checkpoint_generation_ = decoded.generation;
        records_offset_ = checkpoint_offset_ + checkpoint_size_;
        committed_end_offset_ = records_offset_;
        publish_header();
        publish_header();

        if (!require_shrink) {
            return true;
        }

        write_bytes(compacted_offset, checkpoint);
        checkpoint_offset_ = compacted_offset;
        records_offset_ = compacted_offset + checkpoint_size_;
        committed_end_offset_ = records_offset_;
        publish_header();
        publish_header();

        stream_.close();
        std::filesystem::resize_file(database_path_, records_offset_);
        sync_database_file(database_path_);
        stream_.open(database_path_, std::ios::binary | std::ios::in | std::ios::out);
        if (!stream_.is_open()) {
            throw PersistenceError("Unable to reopen the compacted database file.");
        }
        open_ = true;
        return true;
    } catch (const PersistenceError&) {
        open_ = false;
        if (stream_.is_open()) {
            stream_.close();
        }
        throw;
    } catch (const std::filesystem::filesystem_error& error) {
        open_ = false;
        if (stream_.is_open()) {
            stream_.close();
        }
        throw PersistenceError(std::string("Physical database-file shrinking failed: ") + error.what());
    }
}

std::uint64_t WalLog::append_set(const std::string& key, const Value& value) {
    if (key.empty()) {
        throw InvalidKeyError("WAL records require a non-empty key.");
    }
    validate_key(key);
    validate_value(value);
    return append_record(WalOperationType::kSet, key, value);
}

std::uint64_t WalLog::append_delete(const std::string& key) {
    if (key.empty()) {
        throw InvalidKeyError("WAL delete operations require a non-empty key.");
    }
    validate_key(key);
    return append_record(WalOperationType::kDelete, key, Value(nullptr));
}

std::uint64_t WalLog::append_clear() {
    return append_record(WalOperationType::kClear, "", Value(nullptr));
}

std::uint64_t WalLog::append_transaction(const std::vector<WalMutation>& mutations) {
    if (mutations.empty()) {
        throw TransactionError("An empty transaction must not generate a WAL record.");
    }
    ValueList payload;
    payload.reserve(mutations.size());
    for (const auto& mutation : mutations) {
        if (mutation.operation == WalOperationType::kSet) {
            validate_key(mutation.key);
            validate_value(mutation.value);
        } else if (mutation.operation == WalOperationType::kDelete) {
            validate_key(mutation.key);
            if (!mutation.value.is_null()) {
                throw TransactionError("A transaction DELETE operation cannot contain a value.");
            }
        } else if (mutation.operation == WalOperationType::kClear) {
            if (!mutation.key.empty() || !mutation.value.is_null()) {
                throw TransactionError("A transaction CLEAR operation must have an empty key and null value.");
            }
        } else {
            throw TransactionError("A transaction cannot contain nested transaction operations.");
        }
        ValueList item;
        item.emplace_back(static_cast<std::uint64_t>(mutation.operation));
        item.emplace_back(mutation.key);
        if (mutation.operation == WalOperationType::kSet) {
            const auto encoded_value = serialize_value(mutation.value);
            item.emplace_back(Bytes(encoded_value.begin(), encoded_value.end()));
        } else {
            item.emplace_back(nullptr);
        }
        payload.emplace_back(std::move(item));
    }
    return append_record(WalOperationType::kTransaction, "", Value(std::move(payload)));
}

std::uint64_t WalLog::append_index_create(const std::string& name,
                                         const std::string& field,
                                         const std::string& index_type,
                                         bool unique,
                                         std::uint64_t generation) {
    return append_index_create(name, std::vector<std::string>{field}, index_type, unique, generation);
}

std::uint64_t WalLog::append_index_create(const std::string& name,
                                         const std::vector<std::string>& fields,
                                         const std::string& index_type,
                                         bool unique,
                                         std::uint64_t generation) {
    if (fields.empty()) {
        throw PersistenceError("An index WAL definition must contain at least one field.");
    }
    ValueMap definition;
    definition.emplace("field", Value(fields.front()));
    definition.emplace("type", Value(index_type));
    definition.emplace("unique", Value(unique));
    definition.emplace("generation", Value(generation));
    ValueList encoded_fields;
    encoded_fields.reserve(fields.size());
    for (const auto& field : fields) {
        encoded_fields.emplace_back(field);
    }
    definition.emplace("fields", Value(std::move(encoded_fields)));
    return append_record(WalOperationType::kIndexCreate, name, Value(std::move(definition)),
                         kWalIndexRecordFormatVersion);
}

std::uint64_t WalLog::append_index_drop(const std::string& name) {
    return append_record(WalOperationType::kIndexDrop, name, Value(nullptr), kWalIndexRecordFormatVersion);
}

std::vector<WalRecord> WalLog::replay() {
    std::vector<WalRecord> records;
    replay_each([&records](const WalRecord& record) { records.push_back(record); });
    return records;
}

void WalLog::replay_each(const std::function<void(const WalRecord&)>& visitor) {
    ensure_open();
    const auto file_size = read_file_size(stream_);
    const auto available = file_size >= records_offset_ ? file_size - records_offset_ : 0ULL;
    std::uint64_t offset = 0ULL;
    committed_end_offset_ = records_offset_;
    std::uint64_t expected_lsn = checkpoint_lsn_ + 1U;
    while (expected_lsn <= committed_lsn_) {
        constexpr std::uint64_t minimum_record_size = 44ULL;
        if (available - offset < minimum_record_size) {
            throw RecoveryError("The WAL ends before its committed LSN boundary.");
        }

        std::vector<std::uint8_t> prefix(40U, 0U);
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(records_offset_ + offset), std::ios::beg);
        stream_.read(reinterpret_cast<char*>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
        if (stream_.gcount() != static_cast<std::streamsize>(prefix.size())) {
            throw RecoveryError("A committed WAL record header is truncated.");
        }
        const auto record_version = read_u32_le(prefix, 4U);
        if (read_u32_le(prefix, 0U) != kWalRecordMagic ||
            (record_version != kWalRecordFormatVersion && record_version != kWalIndexRecordFormatVersion)) {
            throw RecoveryError("A committed WAL record has an invalid frame header.");
        }

        const auto key_length = read_u64_le(prefix, 24U);
        const auto value_length = read_u64_le(prefix, 32U);
        const auto total_size = record_size(key_length, value_length);
        if (total_size > available - offset) {
            throw RecoveryError("A committed WAL record is truncated.");
        }

        std::vector<std::uint8_t> record_bytes(static_cast<std::size_t>(total_size), 0U);
        std::copy(prefix.begin(), prefix.end(), record_bytes.begin());
        const auto remainder = static_cast<std::size_t>(total_size - prefix.size());
        stream_.read(reinterpret_cast<char*>(record_bytes.data() + prefix.size()),
                     static_cast<std::streamsize>(remainder));
        if (stream_.gcount() != static_cast<std::streamsize>(remainder)) {
            throw RecoveryError("A committed WAL record payload is truncated.");
        }
        try {
            const auto record = deserialize_wal_record(record_bytes);
            if (record.lsn != expected_lsn) {
                throw RecoveryError("WAL LSN ordering is invalid.");
            }
            visitor(record);
        } catch (const SerializationError& error) {
            throw RecoveryError(std::string("A committed WAL record is invalid: ") + error.what());
        }
        offset += total_size;
        committed_end_offset_ = records_offset_ + offset;
        ++expected_lsn;
    }
    if (metadata_degraded_ && available > offset) {
        throw RecoveryError("WAL commit status is ambiguous because metadata is damaged and an uncommitted tail exists.");
    }
}

void WalLog::initialize_file_if_missing() {
    format_version_ = kWalFormatVersion;
    next_lsn_ = 1ULL;
    committed_lsn_ = 0ULL;
    header_generation_ = 1ULL;
    write_header_to_stream(stream_, next_lsn_, committed_lsn_, 0ULL, wal_offset_);
    write_header_to_stream(stream_, next_lsn_, committed_lsn_, header_generation_, wal_offset_ + kWalHeaderSize);
    records_offset_ = wal_offset_ + kDualWalHeaderSize;
    committed_end_offset_ = records_offset_;
    stream_.flush();
    if (!stream_.good()) {
        throw PersistenceError("Unable to initialize WAL metadata.");
    }
    sync_database_file(database_path_);
    open_ = true;
}

void WalLog::write_header() {
    if (format_version_ == kLegacyWalFormatVersion) {
        std::vector<std::uint8_t> header(kWalHeaderSize, 0U);
        std::copy(kHeaderMagic.begin(), kHeaderMagic.end(), header.begin());
        write_u32_le(header, 8U, kLegacyWalFormatVersion);
        write_u64_le(header, 12U, kWalHeaderSize);
        write_u64_le(header, 20U, next_lsn_);
        write_u64_le(header, 28U, committed_lsn_);
        write_u32_le(header, 56U, crc32_value_impl(header.data(), 56U));
        stream_.clear();
        stream_.seekp(static_cast<std::streamoff>(wal_offset_), std::ios::beg);
        stream_.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    } else {
        if (header_generation_ == std::numeric_limits<std::uint64_t>::max()) {
            throw PersistenceError("WAL metadata generation overflowed.");
        }
        const auto generation = header_generation_ + 1U;
        const auto slot = generation % 2U;
        write_header_to_stream(stream_,
                               next_lsn_,
                               committed_lsn_,
                               generation,
                               wal_offset_ + (slot * kWalHeaderSize),
                               format_version_,
                               checkpoint_offset_);
        header_generation_ = generation;
    }
}

std::uint64_t WalLog::append_record(WalOperationType operation,
                                    const std::string& key,
                                    const Value& value,
                                    std::uint32_t record_format_version) {
    ensure_open();
    if (read_only_) {
        throw PersistenceError("Cannot append to a read-only WAL.");
    }
    if (next_lsn_ == std::numeric_limits<std::uint64_t>::max()) {
        throw PersistenceError("WAL LSN sequence is exhausted.");
    }

    const auto record = serialize_wal_record(next_lsn_, operation, key, value, record_format_version);
    stream_.clear();
    stream_.seekp(static_cast<std::streamoff>(committed_end_offset_), std::ios::beg);
    stream_.write(reinterpret_cast<const char*>(record.data()), static_cast<std::streamsize>(record.size()));
    stream_.flush();
    if (!stream_.good()) {
        open_ = false;
        throw PersistenceError("WAL record write failed before commit metadata publication.");
    }

    try {
        sync_database_file(database_path_);
    } catch (const PersistenceError&) {
        open_ = false;
        stream_.close();
        throw;
    }

    const auto lsn = next_lsn_;
    committed_lsn_ = lsn;
    next_lsn_ = lsn + 1U;
    try {
        write_header();
        stream_.flush();
        if (!stream_.good()) {
            throw PersistenceError("WAL commit metadata write failed.");
        }
        sync_database_file(database_path_);
    } catch (const PersistenceError& error) {
        open_ = false;
        stream_.close();
        throw TransactionCommitError(std::string("WAL commit outcome is ambiguous; reopen the database to recover: ") +
                                    error.what());
    }

    committed_end_offset_ += static_cast<std::uint64_t>(record.size());
    ++records_written_;
    bytes_written_ += static_cast<std::uint64_t>(record.size());
    return lsn;
}

void WalLog::ensure_open() {
    if (!open_ || !stream_.is_open()) {
        throw PersistenceError("The WAL log is not open.");
    }
}

}  // namespace kor
