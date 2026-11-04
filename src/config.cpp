#include "kor/config.hpp"

#include "kor/exceptions.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

namespace {

constexpr std::array<std::string_view, 11> kWindowsReservedNames = {
    "CON",
    "PRN",
    "AUX",
    "NUL",
    "COM1",
    "COM2",
    "COM3",
    "COM4",
    "LPT1",
    "LPT2",
    "LPT3",
};

bool is_ascii_alnum_or_special(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
}

bool is_reserved_device_name(std::string_view value) {
    std::string upper_value(value.size(), '\0');
    std::transform(value.begin(), value.end(), upper_value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });

    return std::find(kWindowsReservedNames.begin(), kWindowsReservedNames.end(), upper_value) !=
           kWindowsReservedNames.end();
}

}  // namespace

namespace kor {

std::uint64_t memory_mib_to_bytes(std::uint64_t max_memory_mib) {
    if (max_memory_mib == 0) {
        throw InvalidConfigurationError("max_memory must be a positive integer number of MiB.");
    }

    if (max_memory_mib > (std::numeric_limits<std::uint64_t>::max() / kMiBInBytes)) {
        throw InvalidConfigurationError("max_memory exceeds the representable byte budget.");
    }

    return max_memory_mib * kMiBInBytes;
}

void validate_memory_mib(std::uint64_t max_memory_mib) {
    if (max_memory_mib == 0) {
        throw InvalidConfigurationError("max_memory must be a positive integer number of MiB.");
    }
}

void validate_snapshot_interval(std::uint32_t snapshot_interval_seconds) {
    if (snapshot_interval_seconds < 5 || snapshot_interval_seconds > 500) {
        throw InvalidConfigurationError(
            "snapshot_interval must be an integer between 5 and 500 seconds, inclusive.");
    }
}

void validate_name(const std::string& name) {
    if (name.empty()) {
        throw InvalidConfigurationError("Database name must not be empty.");
    }

    if (name.find('\0') != std::string::npos) {
        throw InvalidConfigurationError("Database name must not contain null bytes.");
    }

    if (name == "." || name == "..") {
        throw InvalidConfigurationError("Database name must not be '.' or '..'.");
    }

    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
        name.find(':') != std::string::npos || name.find('.') != std::string::npos) {
        throw InvalidConfigurationError(
            "Database name must not include path separators, a colon, or a file extension.");
    }

    if (std::any_of(name.begin(), name.end(), [](unsigned char ch) { return std::isspace(ch) != 0; })) {
        throw InvalidConfigurationError("Database name must not contain whitespace.");
    }

    if (name.front() != '_' && !std::isalpha(static_cast<unsigned char>(name.front())) &&
        !std::isdigit(static_cast<unsigned char>(name.front()))) {
        throw InvalidConfigurationError(
            "Database name must start with an ASCII letter or digit.");
    }

    if (!std::all_of(name.begin(), name.end(), [](unsigned char ch) {
            return is_ascii_alnum_or_special(static_cast<char>(ch));
        })) {
        throw InvalidConfigurationError(
            "Database name must contain only ASCII letters, digits, underscores, and hyphens.");
    }

    if (is_reserved_device_name(name)) {
        throw InvalidConfigurationError(
            "Database name is a reserved Windows device name and cannot be used.");
    }
}

Config make_config(const std::filesystem::path& path,
                  const std::string& name,
                  std::uint64_t max_memory_mib,
                  std::uint32_t snapshot_interval_seconds) {
    validate_name(name);
    validate_memory_mib(max_memory_mib);
    validate_snapshot_interval(snapshot_interval_seconds);

    std::filesystem::path resolved_path = std::filesystem::absolute(path);
    if (std::filesystem::exists(resolved_path) && !std::filesystem::is_directory(resolved_path)) {
        throw InvalidConfigurationError(
            "The configured database path points to a regular file, not a directory.");
    }

    Config config;
    config.path = resolved_path;
    config.name = name;
    config.max_memory_mib = max_memory_mib;
    config.snapshot_interval_seconds = snapshot_interval_seconds;
    return config;
}

}  // namespace kor
