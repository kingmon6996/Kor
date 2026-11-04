#include "kor/observability.hpp"

namespace kor {

void RuntimeMetrics::increment(MetricCounter counter, std::uint64_t amount) noexcept {
    counters_[static_cast<std::size_t>(counter)].fetch_add(amount, std::memory_order_relaxed);
}

void RuntimeMetrics::decrement(MetricCounter counter) noexcept {
    counters_[static_cast<std::size_t>(counter)].fetch_sub(1U, std::memory_order_relaxed);
}

void RuntimeMetrics::observe(LatencyKind kind, std::uint64_t duration_ns) noexcept {
    auto& latency = latencies_[static_cast<std::size_t>(kind)];
    latency.count.fetch_add(1U, std::memory_order_relaxed);
    latency.total_ns.fetch_add(duration_ns, std::memory_order_relaxed);

    auto minimum = latency.minimum_ns.load(std::memory_order_relaxed);
    while (duration_ns < minimum &&
           !latency.minimum_ns.compare_exchange_weak(minimum, duration_ns, std::memory_order_relaxed)) {
    }

    auto maximum = latency.maximum_ns.load(std::memory_order_relaxed);
    while (duration_ns > maximum &&
           !latency.maximum_ns.compare_exchange_weak(maximum, duration_ns, std::memory_order_relaxed)) {
    }

    for (std::size_t index = 0U; index < kLatencyBucketUpperBoundsNs.size(); ++index) {
        if (duration_ns <= kLatencyBucketUpperBoundsNs[index]) {
            latency.buckets[index].fetch_add(1U, std::memory_order_relaxed);
            break;
        }
    }
}

RuntimeMetricsSnapshot RuntimeMetrics::snapshot() const noexcept {
    RuntimeMetricsSnapshot result;
    for (std::size_t index = 0U; index < counters_.size(); ++index) {
        result.counters[index] = counters_[index].load(std::memory_order_relaxed);
    }
    for (std::size_t index = 0U; index < latencies_.size(); ++index) {
        const auto& source = latencies_[index];
        auto& destination = result.latencies[index];
        destination.count = source.count.load(std::memory_order_relaxed);
        const auto minimum = source.minimum_ns.load(std::memory_order_relaxed);
        destination.minimum_ns = minimum == UINT64_MAX ? 0U : minimum;
        destination.maximum_ns = source.maximum_ns.load(std::memory_order_relaxed);
        const auto total = source.total_ns.load(std::memory_order_relaxed);
        destination.mean_ns = destination.count == 0U ? 0U : total / destination.count;
        for (std::size_t bucket = 0U; bucket < source.buckets.size(); ++bucket) {
            destination.buckets[bucket] = source.buckets[bucket].load(std::memory_order_relaxed);
        }
    }
    return result;
}

}  // namespace kor
