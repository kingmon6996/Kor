#include "kor/database_file.hpp"

#include "kor/exceptions.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace kor {
namespace {

constexpr std::array<std::uint8_t, 8> kDatabaseMagic = {'K', 'O', 'R', 'D', 'B', 0, 0, 0};
constexpr std::uint32_t kHeaderSize = 64U;
constexpr std::uint16_t kFormatMajorVersion = 1U;
constexpr std::uint16_t kFormatMinorVersion = 0U;
constexpr std::uint32_t kFeatureFlags = 0U;
constexpr std::uint32_t kStateMarker = 0x4B4F524DU;
constexpr std::uint32_t kConfigMagic = 0x4B4F524CU;
constexpr std::uint32_t kDefaultConfigVersion = 1U;

std::uint16_t read_u16_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + sizeof(std::uint16_t) > bytes.size()) {
        throw DatabaseFormatError("Database file is truncated.");
    }
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U));
}

std::uint32_t read_u32_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || offset + sizeof(std::uint32_t) > bytes.size()) {
        throw DatabaseFormatError("Database file is truncated.");
    }
    return static_cast<std::uint32_t>(
        static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U));
}

std::uint64_t read_u64_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || offset + sizeof(std::uint64_t) > bytes.size()) {
        throw DatabaseFormatError("Database file is truncated.");
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (8U * index);
    }
    return value;
}

void write_u16_le(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void write_u32_le(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

void write_u64_le(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU);
    }
}

std::uint32_t crc32_bytes(const std::uint8_t* data, std::size_t length) noexcept {
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
        throw InitializationError("Unable to open the database file.");
    }

    const auto size = static_cast<std::size_t>(stream.tellg());
    stream.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(size, 0U);
    if (size > 0U) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    }
    return bytes;
}

std::vector<std::uint8_t> encode_config_block(const std::string& name,
                                             std::uint64_t max_memory_mib,
                                             std::uint32_t snapshot_interval_seconds,
                                             const std::string& persistence,
                                             const std::string& durability) {
    const auto name_size = static_cast<std::uint32_t>(name.size());
    const auto persistence_size = static_cast<std::uint32_t>(persistence.size());
    const auto durability_size = static_cast<std::uint32_t>(durability.size());

    const std::size_t fixed_prefix = 4U + 4U + 4U;
    const std::size_t name_field = name_size;
    const std::size_t max_memory_field = 8U;
    const std::size_t snapshot_field = 4U;
    const std::size_t persistence_length_field = 4U;
    const std::size_t durability_length_field = 4U;
    const std::size_t checksum_field = 4U;
    const std::size_t total_size = fixed_prefix + name_field + max_memory_field + snapshot_field + persistence_length_field +
                                  persistence_size + durability_length_field + durability_size + checksum_field;
    if (total_size > std::numeric_limits<std::size_t>::max() / 2U) {
        throw DatabaseFormatError("Database configuration block exceeds supported size limits.");
    }

    std::vector<std::uint8_t> block(total_size, 0U);
    write_u32_le(block, 0U, kConfigMagic);
    write_u32_le(block, 4U, kDefaultConfigVersion);
    write_u32_le(block, 8U, name_size);
    std::memcpy(block.data() + 12, name.data(), name_size);
    write_u64_le(block, 12U + name_size, max_memory_mib);
    write_u32_le(block, 12U + name_size + 8U, snapshot_interval_seconds);
    write_u32_le(block, 12U + name_size + 8U + 4U, persistence_size);
    const auto persistence_offset = 12U + name_size + 8U + 4U + 4U;
    std::memcpy(block.data() + persistence_offset, persistence.data(), persistence_size);
    const auto durability_offset = persistence_offset + persistence_size;
    write_u32_le(block, durability_offset, durability_size);
    std::memcpy(block.data() + durability_offset + 4U, durability.data(), durability_size);
    const auto checksum_offset = block.size() - 4U;
    const std::uint32_t checksum = crc32_bytes(block.data(), checksum_offset);
    write_u32_le(block, checksum_offset, checksum);
    return block;
}

DatabaseConfig decode_config_block(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 4U + 4U + 4U + 4U) {
        throw DatabaseFormatError("Database configuration block is truncated.");
    }

    const auto magic = read_u32_le(bytes, 0U);
    if (magic != kConfigMagic) {
        throw DatabaseFormatError("Database configuration block has an invalid magic value.");
    }

    if (bytes.size() < 4U + 4U + 4U + 4U + 4U) {
        throw DatabaseFormatError("Database configuration block is too small to contain a checksum.");
    }

    const auto expected_checksum = read_u32_le(bytes, bytes.size() - 4U);
    const auto actual_checksum = crc32_bytes(bytes.data(), bytes.size() - 4U);
    if (expected_checksum != actual_checksum) {
        throw DatabaseFormatError("Database configuration block checksum mismatch.");
    }

    const auto version = read_u32_le(bytes, 4U);
    if (version != kDefaultConfigVersion) {
        throw UnsupportedFormatVersionError("Database configuration block format is not supported.");
    }

    const auto name_length = read_u32_le(bytes, 8U);
    if (name_length > bytes.size()) {
        throw DatabaseFormatError("Database name length is invalid.");
    }
    const auto name_start = 12U;
    const auto name_end = name_start + name_length;
    if (name_end > bytes.size() - 4U) {
        throw DatabaseFormatError("Database name is truncated.");
    }
    const std::string name(reinterpret_cast<const char*>(bytes.data() + name_start), name_length);

    std::size_t cursor = name_end;
    if (cursor + 8U + 4U + 4U > bytes.size()) {
        throw DatabaseFormatError("Database configuration block is truncated.");
    }
    const auto max_memory_mib = read_u64_le(bytes, cursor);
    cursor += 8U;
    const auto snapshot_interval_seconds = read_u32_le(bytes, cursor);
    cursor += 4U;

    const auto persistence_length = read_u32_le(bytes, cursor);
    cursor += 4U;
    if (cursor + persistence_length > bytes.size()) {
        throw DatabaseFormatError("Database persistence string is truncated.");
    }
    const auto persistence_start = cursor;
    const std::string persistence(reinterpret_cast<const char*>(bytes.data() + persistence_start), persistence_length);
    cursor += persistence_length;

    if (cursor + 4U > bytes.size()) {
        throw DatabaseFormatError("Database durability string is truncated.");
    }
    const auto durability_length = read_u32_le(bytes, cursor);
    cursor += 4U;
    if (cursor + durability_length > bytes.size()) {
        throw DatabaseFormatError("Database durability string is truncated.");
    }
    const auto durability_start = cursor;
    const std::string durability(reinterpret_cast<const char*>(bytes.data() + durability_start), durability_length);

    if (durability_start + durability_length + 4U != bytes.size()) {
        throw DatabaseFormatError("Database configuration block has trailing bytes or unexpected layout.");
    }

    return DatabaseConfig{name, max_memory_mib, snapshot_interval_seconds, persistence, durability};
}

std::vector<std::uint8_t> build_header(std::uint64_t config_offset,
                                      std::uint32_t config_length,
                                      std::array<std::uint8_t, 16> database_id,
                                      std::uint64_t created_at_ms,
                                      std::uint32_t state_marker) {
    std::vector<std::uint8_t> header(kHeaderSize, 0U);
    std::copy(kDatabaseMagic.begin(), kDatabaseMagic.end(), header.begin());
    write_u32_le(header, 8U, kHeaderSize);
    write_u16_le(header, 12U, kFormatMajorVersion);
    write_u16_le(header, 14U, kFormatMinorVersion);
    write_u32_le(header, 16U, kFeatureFlags);
    std::copy(database_id.begin(), database_id.end(), header.begin() + 20U);
    write_u64_le(header, 36U, created_at_ms);
    write_u64_le(header, 44U, config_offset);
    write_u32_le(header, 52U, config_length);
    write_u32_le(header, 56U, state_marker);
    const auto checksum = crc32_bytes(header.data(), 60U);
    write_u32_le(header, 60U, checksum);
    return header;
}

std::array<std::uint8_t, 16> generate_database_id() {
    std::random_device rng;
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(rng() & 0xFFU);
    }
    return bytes;
}

std::vector<std::uint8_t> read_database_bytes(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path) || std::filesystem::is_directory(path)) {
        throw DatabaseFormatError("The database path does not reference a valid Kor database file.");
    }
    return read_file_bytes(path);
}

}  // namespace

bool database_file_is_valid(const std::filesystem::path& path) {
    try {
        const auto bytes = read_database_bytes(path);
        if (bytes.size() < kHeaderSize) {
            return false;
        }

        if (std::memcmp(bytes.data(), kDatabaseMagic.data(), kDatabaseMagic.size()) != 0) {
            return false;
        }

        if (bytes.size() < kHeaderSize) {
            return false;
        }

        const auto header_size = read_u32_le(bytes, 8U);
        if (header_size != kHeaderSize) {
            return false;
        }

        const auto major = read_u16_le(bytes, 12U);
        if (major != kFormatMajorVersion) {
            throw UnsupportedFormatVersionError("Kor database file format version is unsupported.");
        }

        const auto header_checksum = read_u32_le(bytes, 60U);
        const auto actual_checksum = crc32_bytes(bytes.data(), 60U);
        if (header_checksum != actual_checksum) {
            return false;
        }

        const auto config_offset = read_u64_le(bytes, 44U);
        const auto config_length = read_u32_le(bytes, 52U);
        if (config_offset != kHeaderSize || config_offset > bytes.size() || config_length > bytes.size() - config_offset) {
            return false;
        }

        if (read_u32_le(bytes, 56U) != kStateMarker) {
            return false;
        }

        const std::vector<std::uint8_t> config_block(bytes.begin() + static_cast<std::ptrdiff_t>(config_offset),
                                                    bytes.begin() + static_cast<std::ptrdiff_t>(config_offset + config_length));
        decode_config_block(config_block);
        return true;
    } catch (const UnsupportedFormatVersionError&) {
        throw;
    } catch (...) {
        return false;
    }
}

DatabaseConfig read_database_config(const std::filesystem::path& path) {
    const auto bytes = read_database_bytes(path);
    if (bytes.size() < kHeaderSize) {
        throw DatabaseFormatError("The database file is too small to contain a valid Kor header.");
    }

    if (std::memcmp(bytes.data(), kDatabaseMagic.data(), kDatabaseMagic.size()) != 0) {
        throw DatabaseFormatError("The database file does not contain a valid Kor magic value.");
    }

    const auto header_size = read_u32_le(bytes, 8U);
    if (header_size != kHeaderSize) {
        throw DatabaseFormatError("The database file has an unsupported header size.");
    }

    const auto major = read_u16_le(bytes, 12U);
    if (major != kFormatMajorVersion) {
        throw UnsupportedFormatVersionError("Kor database format version is unsupported.");
    }

    const auto header_checksum = read_u32_le(bytes, 60U);
    const auto actual_checksum = crc32_bytes(bytes.data(), 60U);
    if (header_checksum != actual_checksum) {
        throw DatabaseFormatError("Database file header checksum mismatch.");
    }

    const auto config_offset = read_u64_le(bytes, 44U);
    const auto config_length = read_u32_le(bytes, 52U);
    if (config_offset != kHeaderSize || config_offset > bytes.size() || config_length > bytes.size() - config_offset) {
        throw DatabaseFormatError("Database file has invalid configured offsets or lengths.");
    }

    const std::vector<std::uint8_t> config_block(bytes.begin() + static_cast<std::ptrdiff_t>(config_offset),
                                                bytes.begin() + static_cast<std::ptrdiff_t>(config_offset + config_length));
    return decode_config_block(config_block);
}

bool initialize_database_file(const std::filesystem::path& path,
                              const std::string& name,
                              std::uint64_t max_memory_mib,
                              std::uint32_t snapshot_interval_seconds) {
    const auto resolved = std::filesystem::absolute(path);
    if (std::filesystem::exists(resolved) && std::filesystem::is_directory(resolved)) {
        throw InitializationError("A directory exists at the database file path. Expected a file path.");
    }

    if (std::filesystem::exists(resolved)) {
        if (!database_file_is_valid(resolved)) {
            throw DatabaseFormatError("The existing database file is not a valid Kor database file.");
        }
        return false;
    }

    const auto parent = resolved.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    const auto database_id = generate_database_id();
    const auto now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const auto config_block = encode_config_block(name, max_memory_mib, snapshot_interval_seconds, "wal", "balanced");
    const auto header = build_header(kHeaderSize, static_cast<std::uint32_t>(config_block.size()), database_id, now_ms, kStateMarker);

    std::ofstream stream(resolved, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        throw InitializationError("Unable to create the Kor database file.");
    }

    stream.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    stream.write(reinterpret_cast<const char*>(config_block.data()), static_cast<std::streamsize>(config_block.size()));
    if (!stream.good()) {
        throw InitializationError("The Kor database file could not be written to disk.");
    }
    stream.close();

    if (!database_file_is_valid(resolved)) {
        throw DatabaseFormatError("The newly created database file could not be validated.");
    }

    return true;
}

}  // namespace kor
