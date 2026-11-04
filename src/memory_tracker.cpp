#include "kor/memory_tracker.hpp"

#include "kor/exceptions.hpp"

#include <limits>

namespace kor {

namespace {

std::uint64_t checked_add(std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
        throw MemoryLimitExceededError("Memory accounting overflowed.");
    }
    return lhs + rhs;
}

std::uint64_t checked_sub(std::uint64_t lhs, std::uint64_t rhs) {
    if (rhs > lhs) {
        return 0;
    }
    return lhs - rhs;
}

}  // namespace

MemoryTracker::MemoryTracker(std::uint64_t limit_bytes)
    : limit_bytes_(limit_bytes),
      key_bytes_(0),
      value_bytes_(0),
      record_overhead_bytes_(0),
      total_bytes_(0),
      index_bytes_(0),
      records_(0) {}

std::uint64_t MemoryTracker::limit_bytes() const noexcept {
    return limit_bytes_;
}

void MemoryTracker::set_limit(std::uint64_t limit_bytes) {
    limit_bytes_ = limit_bytes;
}

void MemoryTracker::add_record(const std::string& key, const Value& value) {
    const auto key_bytes = estimate_key_bytes(key);
    const auto value_bytes = estimate_value_bytes(value);
    const auto record_overhead = 32ULL;
    const auto delta = checked_add(checked_add(key_bytes, value_bytes), record_overhead);
    if (limit_bytes_ != 0 && would_exceed_limit(delta)) {
        throw MemoryLimitExceededError("The configured memory limit would be exceeded.");
    }
    key_bytes_ = checked_add(key_bytes_, key_bytes);
    value_bytes_ = checked_add(value_bytes_, value_bytes);
    record_overhead_bytes_ = checked_add(record_overhead_bytes_, record_overhead);
    total_bytes_ = checked_add(total_bytes_, delta);
    records_ += 1ULL;
}

void MemoryTracker::replace_record(const std::string& key, const Value& old_value, const Value& new_value) {
    const auto old_key_bytes = estimate_key_bytes(key);
    const auto old_value_bytes = estimate_value_bytes(old_value);
    const auto new_key_bytes = estimate_key_bytes(key);
    const auto new_value_bytes = estimate_value_bytes(new_value);
    const auto record_overhead = 32ULL;
    const auto old_usage = checked_add(checked_add(old_key_bytes, old_value_bytes), record_overhead);
    const auto new_usage = checked_add(checked_add(new_key_bytes, new_value_bytes), record_overhead);

    if (new_usage > old_usage) {
        const auto delta = new_usage - old_usage;
        if (limit_bytes_ != 0 && would_exceed_limit(delta)) {
            throw MemoryLimitExceededError("The configured memory limit would be exceeded.");
        }
    }

    key_bytes_ = checked_sub(key_bytes_, old_key_bytes);
    value_bytes_ = checked_sub(value_bytes_, old_value_bytes);
    record_overhead_bytes_ = checked_sub(record_overhead_bytes_, record_overhead);
    total_bytes_ = checked_sub(total_bytes_, old_usage);

    key_bytes_ = checked_add(key_bytes_, new_key_bytes);
    value_bytes_ = checked_add(value_bytes_, new_value_bytes);
    record_overhead_bytes_ = checked_add(record_overhead_bytes_, record_overhead);
    total_bytes_ = checked_add(total_bytes_, new_usage);
}

void MemoryTracker::remove_record(const std::string& key, const Value& value) {
    const auto key_bytes = estimate_key_bytes(key);
    const auto value_bytes = estimate_value_bytes(value);
    const auto record_overhead = 32ULL;
    const auto delta = checked_add(checked_add(key_bytes, value_bytes), record_overhead);
    if (records_ == 0) {
        return;
    }
    key_bytes_ = checked_sub(key_bytes_, key_bytes);
    value_bytes_ = checked_sub(value_bytes_, value_bytes);
    record_overhead_bytes_ = checked_sub(record_overhead_bytes_, record_overhead);
    total_bytes_ = checked_sub(total_bytes_, delta);
    records_ -= 1ULL;
}

void MemoryTracker::clear() {
    key_bytes_ = 0;
    value_bytes_ = 0;
    record_overhead_bytes_ = 0;
    total_bytes_ = index_bytes_;
    records_ = 0;
}

void MemoryTracker::set_index_bytes(std::uint64_t bytes) {
    const auto records_total = checked_add(checked_add(key_bytes_, value_bytes_), record_overhead_bytes_);
    const auto next_total = checked_add(records_total, bytes);
    if (limit_bytes_ != 0 && next_total > limit_bytes_) {
        throw MemoryLimitExceededError("Secondary indexes would exceed the configured memory limit.");
    }
    index_bytes_ = bytes;
    total_bytes_ = next_total;
}

bool MemoryTracker::would_exceed_limit(std::uint64_t extra_bytes) const noexcept {
    if (limit_bytes_ == 0) {
        return false;
    }
    return total_bytes_ > limit_bytes_ || extra_bytes > limit_bytes_ - total_bytes_;
}

void MemoryTracker::ensure_additional_bytes(std::uint64_t bytes) const {
    if (limit_bytes_ != 0 && would_exceed_limit(bytes)) {
        throw MemoryLimitExceededError("Temporary secondary-index memory would exceed the configured limit.");
    }
}

MemoryStats MemoryTracker::stats() const noexcept {
    MemoryStats stats;
    stats.limit_bytes = limit_bytes_;
    stats.key_bytes = key_bytes_;
    stats.value_bytes = value_bytes_;
    stats.record_overhead_bytes = record_overhead_bytes_;
    stats.index_bytes = index_bytes_;
    stats.total_bytes = total_bytes_;
    stats.records = records_;
    return stats;
}

}  // namespace kor
