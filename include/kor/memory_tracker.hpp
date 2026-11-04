#pragma once

#include <cstdint>

#include "kor/value.hpp"

namespace kor {

struct MemoryStats {
    std::uint64_t limit_bytes = 0;
    std::uint64_t key_bytes = 0;
    std::uint64_t value_bytes = 0;
    std::uint64_t record_overhead_bytes = 0;
    std::uint64_t index_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t records = 0;
};

class MemoryTracker {
public:
    explicit MemoryTracker(std::uint64_t limit_bytes = 64ULL * 1024ULL * 1024ULL);

    std::uint64_t limit_bytes() const noexcept;
    void set_limit(std::uint64_t limit_bytes);

    void add_record(const std::string& key, const Value& value);
    void replace_record(const std::string& key, const Value& old_value, const Value& new_value);
    void remove_record(const std::string& key, const Value& value);
    void clear();
    void set_index_bytes(std::uint64_t bytes);

    bool would_exceed_limit(std::uint64_t extra_bytes) const noexcept;
    void ensure_additional_bytes(std::uint64_t bytes) const;
    MemoryStats stats() const noexcept;

private:
    std::uint64_t limit_bytes_ = 0;
    std::uint64_t key_bytes_ = 0;
    std::uint64_t value_bytes_ = 0;
    std::uint64_t record_overhead_bytes_ = 0;
    std::uint64_t total_bytes_ = 0;
    std::uint64_t index_bytes_ = 0;
    std::uint64_t records_ = 0;
};

}  // namespace kor
