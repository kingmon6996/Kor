#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace kor {

struct Config {
    std::filesystem::path path;
    std::string name;
    std::uint64_t max_memory_mib = 0;
    std::uint32_t snapshot_interval_seconds = 0;
};

inline constexpr std::string_view kDefaultPersistence = "wal";
inline constexpr std::string_view kDefaultDurability = "balanced";

constexpr std::uint64_t kMiBInBytes = 1024ULL * 1024ULL;

std::uint64_t memory_mib_to_bytes(std::uint64_t max_memory_mib);

void validate_name(const std::string& name);
void validate_memory_mib(std::uint64_t max_memory_mib);
void validate_snapshot_interval(std::uint32_t snapshot_interval_seconds);

Config make_config(const std::filesystem::path& path,
                  const std::string& name,
                  std::uint64_t max_memory_mib,
                  std::uint32_t snapshot_interval_seconds);

}  // namespace kor
