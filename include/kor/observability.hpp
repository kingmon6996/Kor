#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace kor {

enum class MetricCounter : std::size_t {
    kReads,
    kWrites,
    kDeletes,
    kClearOperations,
    kFailedOperations,
    kTransactionCommits,
    kTransactionRollbacks,
    kWalRecordsWritten,
    kWalBytesWritten,
    kCheckpointCount,
    kCheckpointFailures,
    kRecoveryCount,
    kIndexBuildCount,
    kIndexBuildFailures,
    kIndexedQueries,
    kFullScanQueries,
    kQueryFailures,
    kCompactionCount,
    kCompactionFailures,
    kActiveTransactions,
    kCount,
};

enum class LatencyKind : std::size_t {
    kRead,
    kWrite,
    kDelete,
    kClear,
    kTransactionCommit,
    kIndexBuild,
    kQuery,
    kCompaction,
    kCount,
};

inline constexpr std::array<std::uint64_t, 8> kLatencyBucketUpperBoundsNs = {
    1'000ULL,
    10'000ULL,
    100'000ULL,
    1'000'000ULL,
    10'000'000ULL,
    100'000'000ULL,
    1'000'000'000ULL,
    UINT64_MAX,
};

struct LatencySummary {
    std::uint64_t count = 0U;
    std::uint64_t minimum_ns = 0U;
    std::uint64_t maximum_ns = 0U;
    std::uint64_t mean_ns = 0U;
    std::array<std::uint64_t, kLatencyBucketUpperBoundsNs.size()> buckets{};
};

struct RuntimeMetricsSnapshot {
    std::array<std::uint64_t, static_cast<std::size_t>(MetricCounter::kCount)> counters{};
    std::array<LatencySummary, static_cast<std::size_t>(LatencyKind::kCount)> latencies{};
    std::uint64_t current_committed_lsn = 0U;
    std::uint64_t last_checkpoint_lsn = 0U;
    std::uint64_t active_index_count = 0U;
    bool closed = false;
    bool recovery_required = false;
};

class RuntimeMetrics {
public:
    void increment(MetricCounter counter, std::uint64_t amount = 1U) noexcept;
    void decrement(MetricCounter counter) noexcept;
    void observe(LatencyKind kind, std::uint64_t duration_ns) noexcept;
    RuntimeMetricsSnapshot snapshot() const noexcept;

private:
    struct AtomicLatency {
        std::atomic<std::uint64_t> count{0U};
        std::atomic<std::uint64_t> total_ns{0U};
        std::atomic<std::uint64_t> minimum_ns{UINT64_MAX};
        std::atomic<std::uint64_t> maximum_ns{0U};
        std::array<std::atomic<std::uint64_t>, kLatencyBucketUpperBoundsNs.size()> buckets{};
    };

    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(MetricCounter::kCount)> counters_{};
    std::array<AtomicLatency, static_cast<std::size_t>(LatencyKind::kCount)> latencies_{};
};

}  // namespace kor
