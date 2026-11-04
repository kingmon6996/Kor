#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace kor {

struct DatabaseConfig {
    std::string name;
    std::uint64_t max_memory_mib = 0;
    std::uint32_t snapshot_interval_seconds = 0;
    std::string persistence = "wal";
    std::string durability = "balanced";
};

bool initialize_database_file(const std::filesystem::path& path,
                              const std::string& name,
                              std::uint64_t max_memory_mib,
                              std::uint32_t snapshot_interval_seconds);

bool database_file_is_valid(const std::filesystem::path& path);
DatabaseConfig read_database_config(const std::filesystem::path& path);

}  // namespace kor
