#include "kor/transaction.hpp"

#include "kor/exceptions.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>
#include <vector>

namespace kor {
namespace {

constexpr std::uint64_t kMaxTransactionBytes = 16ULL * 1024ULL * 1024ULL;

std::uint64_t checked_write_size(const std::string& key, const std::optional<Value>& value) {
    const auto key_bytes = estimate_key_bytes(key);
    const auto value_bytes = value.has_value() ? estimate_value_bytes(*value) : 0ULL;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (value_bytes > maximum - 32ULL || key_bytes > maximum - value_bytes - 32ULL) {
        throw TransactionTooLargeError("Transaction write-set size overflowed.");
    }
    return key_bytes + value_bytes + 32ULL;
}

}  // namespace

Transaction::Transaction(Storage& storage)
    : storage_(&storage),
      writer_lease_(storage.acquire_writer()) {
    std::unique_lock<std::shared_mutex> lock(storage.mutex_);
    storage.ensure_usable();
    storage.runtime_metrics_.increment(MetricCounter::kActiveTransactions);
    max_write_set_bytes_ = kMaxTransactionBytes;
    const auto configured_limit = storage.tracker_.limit_bytes();
    if (configured_limit != 0U) {
        const auto used = storage.tracker_.stats().total_bytes;
        const auto available = used >= configured_limit ? 0U : configured_limit - used;
        max_write_set_bytes_ = std::min(max_write_set_bytes_, available);
    }
}

Transaction::~Transaction() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ && !committed_ && !rolled_back_) {
        active_ = false;
        writes_.clear();
        write_set_bytes_ = 0;
        storage_->runtime_metrics_.increment(MetricCounter::kTransactionRollbacks);
        storage_->runtime_metrics_.decrement(MetricCounter::kActiveTransactions);
    }
}

bool Transaction::active() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && !committing_ && !committed_ && !rolled_back_ && !failed_;
}

bool Transaction::committed() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return committed_;
}

bool Transaction::rolled_back() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return rolled_back_;
}

std::optional<Value> Transaction::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_active();
    validate_key(key);

    const auto it = writes_.find(key);
    if (it != writes_.end()) {
        return it->second;
    }
    if (cleared_) {
        return std::nullopt;
    }
    return storage_->get(key);
}

bool Transaction::contains(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_active();
    validate_key(key);
    const auto it = writes_.find(key);
    if (it != writes_.end()) {
        return it->second.has_value();
    }
    return !cleared_ && storage_->exists(key);
}

void Transaction::set(const std::string& key, const Value& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_active();
    validate_key(key);
    validate_value(value);

    const auto current = writes_.find(key);
    const auto old_size = current == writes_.end() ? 0ULL : checked_write_size(key, current->second);
    const auto new_size = checked_write_size(key, value);
    const auto base_size = write_set_bytes_ - old_size;
    if (new_size > max_write_set_bytes_ || base_size > max_write_set_bytes_ - new_size) {
        throw TransactionTooLargeError("Transaction write set exceeds the configured memory budget or 16 MiB limit.");
    }
    writes_.insert_or_assign(key, value);
    write_set_bytes_ = base_size + new_size;
}

void Transaction::erase(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_active();
    validate_key(key);

    const auto current = writes_.find(key);
    const auto old_size = current == writes_.end() ? 0ULL : checked_write_size(key, current->second);
    const auto new_size = checked_write_size(key, std::nullopt);
    const auto base_size = write_set_bytes_ - old_size;
    if (new_size > max_write_set_bytes_ || base_size > max_write_set_bytes_ - new_size) {
        throw TransactionTooLargeError("Transaction write set exceeds the configured memory budget or 16 MiB limit.");
    }
    writes_.insert_or_assign(key, std::nullopt);
    write_set_bytes_ = base_size + new_size;
}

void Transaction::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_active();
    cleared_ = true;
    writes_.clear();
    write_set_bytes_ = 0;
}

void Transaction::commit() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_active();
    const auto started = std::chrono::steady_clock::now();
    if (writes_.empty() && !cleared_) {
        active_ = false;
        committing_ = false;
        committed_ = true;
        write_set_bytes_ = 0;
        storage_->runtime_metrics_.increment(MetricCounter::kTransactionCommits);
        storage_->runtime_metrics_.decrement(MetricCounter::kActiveTransactions);
        storage_->runtime_metrics_.observe(
            LatencyKind::kTransactionCommit,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - started)
                                           .count()));
        release_writer();
        return;
    }

    std::vector<WalMutation> mutations;
    mutations.reserve(writes_.size());
    for (const auto& [key, value] : writes_) {
        if (value.has_value()) {
            mutations.push_back(WalMutation{WalOperationType::kSet, key, *value});
        } else {
            mutations.push_back(WalMutation{WalOperationType::kDelete, key, Value(nullptr)});
        }
    }

    committing_ = true;
    try {
        storage_->commit_transaction(mutations, cleared_);
    } catch (...) {
        failed_ = true;
        active_ = false;
        committing_ = false;
        writes_.clear();
        write_set_bytes_ = 0;
        storage_->runtime_metrics_.increment(MetricCounter::kFailedOperations);
        storage_->runtime_metrics_.decrement(MetricCounter::kActiveTransactions);
        storage_->runtime_metrics_.observe(
            LatencyKind::kTransactionCommit,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - started)
                                           .count()));
        release_writer();
        throw;
    }

    active_ = false;
    committing_ = false;
    committed_ = true;
    writes_.clear();
    write_set_bytes_ = 0;
    storage_->runtime_metrics_.increment(MetricCounter::kTransactionCommits);
    storage_->runtime_metrics_.decrement(MetricCounter::kActiveTransactions);
    storage_->runtime_metrics_.observe(
        LatencyKind::kTransactionCommit,
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count()));
    release_writer();
}

void Transaction::rollback() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rolled_back_) {
        return;
    }
    if (committed_) {
        throw TransactionStateError("Cannot roll back a committed transaction.");
    }
    if (failed_) {
        throw TransactionStateError("Cannot roll back a failed transaction; reopen the database if commit status is ambiguous.");
    }
    if (!active_ || committing_) {
        throw TransactionStateError("Cannot roll back an inactive or committing transaction.");
    }

    writes_.clear();
    write_set_bytes_ = 0;
    cleared_ = false;
    active_ = false;
    rolled_back_ = true;
    storage_->runtime_metrics_.increment(MetricCounter::kTransactionRollbacks);
    storage_->runtime_metrics_.increment(MetricCounter::kActiveTransactions,
                                         static_cast<std::uint64_t>(-1));
    release_writer();
}

void Transaction::ensure_active() const {
    if (!active_ || committing_ || committed_ || rolled_back_ || failed_) {
        throw TransactionStateError("Transaction is no longer active.");
    }
}

void Transaction::release_writer() noexcept {
    writer_lease_ = Storage::WriterLease();
}

}  // namespace kor
