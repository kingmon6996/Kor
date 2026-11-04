#include "kor/storage.hpp"

#include "kor/exceptions.hpp"
#include "kor/serializer.hpp"
#include "kor/transaction.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

namespace kor {
namespace {

constexpr auto kAutomaticSnapshotCheckInterval = std::chrono::milliseconds(100);

class MetricTimer {
public:
    MetricTimer(RuntimeMetrics& metrics, LatencyKind kind) noexcept
        : metrics_(metrics), kind_(kind), started_(std::chrono::steady_clock::now()) {}

    ~MetricTimer() {
        const auto elapsed = std::chrono::steady_clock::now() - started_;
        const auto duration_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        metrics_.observe(kind_, duration_ns);
        if (!succeeded_) {
            metrics_.increment(MetricCounter::kFailedOperations);
            if (kind_ == LatencyKind::kQuery) {
                metrics_.increment(MetricCounter::kQueryFailures);
            } else if (kind_ == LatencyKind::kIndexBuild) {
                metrics_.increment(MetricCounter::kIndexBuildFailures);
            } else if (kind_ == LatencyKind::kCompaction) {
                metrics_.increment(MetricCounter::kCompactionFailures);
            }
        }
    }

    void success() noexcept { succeeded_ = true; }

private:
    RuntimeMetrics& metrics_;
    LatencyKind kind_;
    std::chrono::steady_clock::time_point started_;
    bool succeeded_ = false;
};

void apply_mutation(HashIndex& index, MemoryTracker& tracker, const WalMutation& mutation) {
    switch (mutation.operation) {
        case WalOperationType::kSet: {
            validate_key(mutation.key);
            validate_value(mutation.value);
            const auto current = index.get(mutation.key);
            if (current.has_value()) {
                tracker.replace_record(mutation.key, current->value, mutation.value);
            } else {
                tracker.add_record(mutation.key, mutation.value);
            }
            index.set(mutation.key, Record(mutation.value, estimate_value_bytes(mutation.value)));
            return;
        }
        case WalOperationType::kDelete: {
            validate_key(mutation.key);
            const auto current = index.get(mutation.key);
            if (current.has_value()) {
                tracker.remove_record(mutation.key, current->value);
                index.erase(mutation.key);
            }
            return;
        }
        case WalOperationType::kClear:
            if (!mutation.key.empty() || !mutation.value.is_null()) {
                throw RecoveryError("Malformed CLEAR operation in the WAL.");
            }
            tracker.clear();
            index.clear();
            return;
        case WalOperationType::kTransaction:
            break;
    }
    throw RecoveryError("Nested or unsupported transaction WAL operation.");
}

std::vector<WalMutation> decode_transaction(const Value& value) {
    if (!value.is_list()) {
        throw RecoveryError("Transaction WAL payload is not a list.");
    }
    const auto& entries = std::get<ValueList>(value.data);
    if (entries.empty()) {
        throw RecoveryError("Transaction WAL payload must contain at least one operation.");
    }

    std::vector<WalMutation> mutations;
    mutations.reserve(entries.size());
    for (const auto& entry : entries) {
        if (!entry.is_list()) {
            throw RecoveryError("Transaction WAL operation is malformed.");
        }
        const auto& fields = std::get<ValueList>(entry.data);
        if (fields.size() != 3U || !fields[0].is_uint64() || !fields[1].is_string()) {
            throw RecoveryError("Transaction WAL operation fields are malformed.");
        }
        const auto code = std::get<std::uint64_t>(fields[0].data);
        if (code < static_cast<std::uint64_t>(WalOperationType::kSet) ||
            code > static_cast<std::uint64_t>(WalOperationType::kClear)) {
            throw RecoveryError("Transaction WAL operation type is invalid.");
        }
        WalMutation mutation;
        mutation.operation = static_cast<WalOperationType>(code);
        mutation.key = std::get<std::string>(fields[1].data);
        try {
            if (mutation.operation == WalOperationType::kSet) {
                validate_key(mutation.key);
                if (!fields[2].is_bytes()) {
                    throw RecoveryError("SET operation has no serialized value.");
                }
                const auto& bytes = std::get<Bytes>(fields[2].data);
                mutation.value = deserialize_value(bytes);
                validate_value(mutation.value);
            } else if (mutation.operation == WalOperationType::kDelete) {
                validate_key(mutation.key);
                if (!fields[2].is_null()) {
                    throw RecoveryError("DELETE operation must not contain a value.");
                }
                mutation.value = Value(nullptr);
            } else {
                if (!mutation.key.empty() || !fields[2].is_null()) {
                    throw RecoveryError("CLEAR operation fields are invalid.");
                }
                mutation.value = Value(nullptr);
            }
        } catch (const Error& error) {
            throw RecoveryError(std::string("Invalid transaction WAL operation: ") + error.what());
        }
        mutations.push_back(std::move(mutation));
    }
    return mutations;
}

IndexDefinition decode_index_definition(const WalRecord& record) {
    if (!record.value.is_map() || record.key.empty()) {
        throw RecoveryError("Index definition WAL record is malformed.");
    }
    const auto& fields = std::get<ValueMap>(record.value.data);
    const auto field = fields.find("field");
    const auto type = fields.find("type");
    const auto unique = fields.find("unique");
    const auto generation = fields.find("generation");
    const auto indexed_fields = fields.find("fields");
    if ((fields.size() != 4U && fields.size() != 5U) ||
        (indexed_fields == fields.end() && fields.size() != 4U) ||
        (indexed_fields != fields.end() && fields.size() != 5U) ||
        field == fields.end() || type == fields.end() ||
        unique == fields.end() || generation == fields.end() || !field->second.is_string() ||
        !type->second.is_string() || !unique->second.is_bool() || !generation->second.is_uint64()) {
        throw RecoveryError("Index definition WAL fields are invalid.");
    }
    IndexDefinition definition;
    definition.name = record.key;
    definition.field = std::get<std::string>(field->second.data);
    if (indexed_fields == fields.end()) {
        definition.fields.push_back(definition.field);
    } else {
        if (!indexed_fields->second.is_list()) {
            throw RecoveryError("Index definition WAL fields list is invalid.");
        }
        for (const auto& indexed_field : std::get<ValueList>(indexed_fields->second.data)) {
            if (!indexed_field.is_string()) {
                throw RecoveryError("Index definition WAL field entry is invalid.");
            }
            definition.fields.push_back(std::get<std::string>(indexed_field.data));
        }
    }
    const auto& type_name = std::get<std::string>(type->second.data);
    if (type_name == "hash") {
        definition.type = SecondaryIndexType::kHash;
    } else if (type_name == "ordered") {
        definition.type = SecondaryIndexType::kOrdered;
    } else {
        throw RecoveryError("Index definition WAL type is unsupported.");
    }
    definition.unique = std::get<bool>(unique->second.data);
    definition.generation = std::get<std::uint64_t>(generation->second.data);
    try {
        SecondaryIndexManager::validate_definition(definition);
    } catch (const Error& error) {
        throw RecoveryError(std::string("Index definition WAL record is invalid: ") + error.what());
    }
    return definition;
}

void apply_recovered_batch(HashIndex& index,
                           MemoryTracker& tracker,
                           SecondaryIndexManager& secondary_indexes,
                           const std::vector<WalMutation>& mutations,
                           bool clear_first) {
    tracker.ensure_additional_bytes(secondary_indexes.estimated_memory_bytes());
    HashIndex staged_index = index;
    MemoryTracker staged_tracker = tracker;
    SecondaryIndexManager staged_secondary = secondary_indexes;
    staged_tracker.set_index_bytes(0U);
    if (clear_first) {
        apply_mutation(staged_index, staged_tracker,
                       WalMutation{WalOperationType::kClear, "", Value(nullptr)});
    }
    for (const auto& mutation : mutations) {
        apply_mutation(staged_index, staged_tracker, mutation);
    }
    staged_secondary.reconcile(index, staged_index, mutations, clear_first);
    const auto staged_index_bytes = staged_secondary.estimated_memory_bytes();
    tracker.ensure_additional_bytes(staged_index_bytes);
    staged_tracker.set_index_bytes(staged_index_bytes);
    index.swap(staged_index);
    tracker = staged_tracker;
    secondary_indexes.swap(staged_secondary);
}

void apply_record(HashIndex& index,
                  MemoryTracker& tracker,
                  SecondaryIndexManager& secondary_indexes,
                  const WalRecord& record) {
    if (record.operation == WalOperationType::kTransaction) {
        if (!record.key.empty()) {
            throw RecoveryError("Transaction WAL record has an unexpected key.");
        }
        apply_recovered_batch(index, tracker, secondary_indexes, decode_transaction(record.value), false);
        return;
    }
    if (record.operation == WalOperationType::kIndexCreate) {
        try {
            auto definition = decode_index_definition(record);
            const auto memory = tracker.stats();
            const auto primary_bytes = memory.total_bytes - memory.index_bytes;
            const auto build_budget = memory.limit_bytes == 0U
                                          ? std::numeric_limits<std::uint64_t>::max()
                                          : (primary_bytes >= memory.limit_bytes
                                                 ? 0U
                                                 : memory.limit_bytes - primary_bytes);
            secondary_indexes.create(std::move(definition), index, build_budget);
            tracker.set_index_bytes(secondary_indexes.estimated_memory_bytes());
        } catch (const Error& error) {
            throw RecoveryError(std::string("Unable to rebuild a persisted secondary index: ") + error.what());
        }
        return;
    }
    if (record.operation == WalOperationType::kIndexDrop) {
        if (record.key.empty() || !record.value.is_null()) {
            throw RecoveryError("Index drop WAL record is malformed.");
        }
        try {
            secondary_indexes.drop(record.key);
            tracker.set_index_bytes(secondary_indexes.estimated_memory_bytes());
        } catch (const Error& error) {
            throw RecoveryError(std::string("Unable to apply a persisted index drop: ") + error.what());
        }
        return;
    }
    if (record.operation == WalOperationType::kClear) {
        apply_recovered_batch(index,
                              tracker,
                              secondary_indexes,
                              {WalMutation{record.operation, record.key, record.value}},
                              false);
        return;
    }
    const auto before = index.get(record.key);
    apply_mutation(index, tracker, WalMutation{record.operation, record.key, record.value});
    const auto after = index.get(record.key);
    secondary_indexes.apply_change(record.key,
                                   before.has_value() ? std::optional<Value>(before->value) : std::nullopt,
                                   after.has_value() ? std::optional<Value>(after->value) : std::nullopt);
    tracker.set_index_bytes(secondary_indexes.estimated_memory_bytes());
}

void restore_checkpoint(HashIndex& index,
                        MemoryTracker& tracker,
                        SecondaryIndexManager& secondary_indexes,
                        CheckpointData& checkpoint) {
    while (!checkpoint.records.empty()) {
        auto record = checkpoint.records.extract(checkpoint.records.begin());
        apply_mutation(index,
                       tracker,
                       WalMutation{WalOperationType::kSet, record.key(), record.mapped()});
    }
    for (const auto& stored : checkpoint.indexes) {
        IndexDefinition definition;
        definition.name = stored.name;
        definition.field = stored.field;
        definition.fields = stored.fields.empty() ? std::vector<std::string>{stored.field}
                                                  : stored.fields;
        definition.type = static_cast<SecondaryIndexType>(stored.type);
        definition.unique = stored.unique;
        definition.generation = stored.generation;
        try {
            const auto memory = tracker.stats();
            const auto remaining = memory.limit_bytes == 0U
                                       ? std::numeric_limits<std::uint64_t>::max()
                                       : (memory.total_bytes >= memory.limit_bytes
                                              ? 0U
                                              : memory.limit_bytes - memory.total_bytes);
            secondary_indexes.create(std::move(definition), index, remaining);
        } catch (const Error& error) {
            throw RecoveryError(std::string("Unable to restore a checkpointed secondary index: ") +
                                error.what());
        }
    }
    tracker.set_index_bytes(secondary_indexes.estimated_memory_bytes());
}

std::uint64_t checked_add(std::uint64_t current, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - current) {
        throw MemoryLimitExceededError("Compaction size estimate overflowed.");
    }
    return current + value;
}

}  // namespace

Storage::Storage(std::uint64_t max_memory_bytes,
                 const std::filesystem::path& database_path,
                 bool read_only)
    : tracker_(max_memory_bytes), read_only_(read_only) {
    if (database_path.empty()) {
        return;
    }

    wal_ = std::make_unique<WalLog>(database_path, max_memory_bytes, read_only_);
    HashIndex recovered_index;
    MemoryTracker recovered_tracker(max_memory_bytes);
    SecondaryIndexManager recovered_secondary_indexes;
    if (auto checkpoint = wal_->read_checkpoint(); checkpoint.has_value()) {
        restore_checkpoint(recovered_index,
                           recovered_tracker,
                           recovered_secondary_indexes,
                           *checkpoint);
    }
    wal_->replay_each([&](const WalRecord& record) {
        apply_record(recovered_index, recovered_tracker, recovered_secondary_indexes, record);
    });
    index_.swap(recovered_index);
    tracker_ = recovered_tracker;
    secondary_indexes_.swap(recovered_secondary_indexes);
    runtime_metrics_.increment(MetricCounter::kRecoveryCount);
    if (!read_only_) {
        start_auto_snapshot_worker();
    }
}

Storage::~Storage() {
    stop_auto_snapshot_worker();
}

void Storage::start_auto_snapshot_worker() {
    if (!wal_) {
        return;
    }
    auto_snapshot_worker_ = std::thread(&Storage::auto_snapshot_loop, this);
}

void Storage::stop_auto_snapshot_worker() noexcept {
    {
        std::lock_guard<std::mutex> lock(auto_snapshot_mutex_);
        stop_auto_snapshot_worker_ = true;
    }
    auto_snapshot_condition_.notify_all();
    if (auto_snapshot_worker_.joinable()) {
        auto_snapshot_worker_.join();
    }
}

void Storage::record_auto_snapshot_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(auto_snapshot_mutex_);
    last_auto_snapshot_error_ = error;
    checkpoint_status_.last_error = error;
}

void Storage::auto_snapshot_loop() noexcept {
    std::uint64_t last_attempted_lsn = 0U;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(auto_snapshot_mutex_);
            if (auto_snapshot_condition_.wait_for(
                    lock,
                    kAutomaticSnapshotCheckInterval,
                    [this] { return stop_auto_snapshot_worker_; })) {
                return;
            }
        }

        WalStats current;
        try {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            if (closed_ || recovery_required_ || !wal_) {
                continue;
            }
            current = wal_->stats();
        } catch (const std::exception& error) {
            record_auto_snapshot_error(error.what());
            continue;
        } catch (...) {
            record_auto_snapshot_error("Unknown automatic snapshot status failure.");
            continue;
        }

        if (!current.compaction_supported ||
            current.committed_lsn <= current.checkpoint_lsn ||
            current.committed_lsn <= last_attempted_lsn) {
            continue;
        }

        const auto attempted_lsn = current.committed_lsn;
        const auto started = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(auto_snapshot_mutex_);
            checkpoint_status_.in_progress = true;
            checkpoint_status_.last_attempt_lsn = attempted_lsn;
            ++checkpoint_status_.attempt_count;
        }
        try {
#if defined(KOR_ENABLE_TEST_HOOKS)
            {
                std::lock_guard<std::mutex> lock(auto_snapshot_mutex_);
                if (fail_next_auto_checkpoint_for_testing_) {
                    fail_next_auto_checkpoint_for_testing_ = false;
                    throw PersistenceError("Injected automatic checkpoint failure.");
                }
            }
#endif
            const auto result = compact();
            {
                std::lock_guard<std::mutex> lock(auto_snapshot_mutex_);
                checkpoint_status_.in_progress = false;
                checkpoint_status_.last_attempt_duration_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - started).count());
                if (result.checkpoint_performed) {
                    checkpoint_status_.last_success_lsn = result.checkpoint_lsn;
                    ++checkpoint_status_.success_count;
                    checkpoint_status_.last_error.clear();
                    last_auto_snapshot_error_.clear();
                } else {
                    checkpoint_status_.last_error = result.limitation;
                    last_auto_snapshot_error_ = result.limitation;
                    ++checkpoint_status_.failure_count;
                }
            }
            last_attempted_lsn = attempted_lsn;
        } catch (const std::exception& error) {
            record_auto_snapshot_error(error.what());
            {
                std::lock_guard<std::mutex> lock(auto_snapshot_mutex_);
                checkpoint_status_.in_progress = false;
                checkpoint_status_.last_attempt_duration_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - started).count());
                ++checkpoint_status_.failure_count;
            }
            last_attempted_lsn = attempted_lsn;
        } catch (...) {
            record_auto_snapshot_error("Unknown automatic snapshot failure.");
            {
                std::lock_guard<std::mutex> lock(auto_snapshot_mutex_);
                checkpoint_status_.in_progress = false;
                checkpoint_status_.last_attempt_duration_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - started).count());
                ++checkpoint_status_.failure_count;
            }
            last_attempted_lsn = attempted_lsn;
        }
    }
}

Storage::WriterLease::WriterLease(Storage* storage) noexcept : storage_(storage) {}

Storage::WriterLease::~WriterLease() {
    if (storage_ != nullptr) {
        storage_->release_writer();
    }
}

Storage::WriterLease::WriterLease(WriterLease&& other) noexcept
    : storage_(std::exchange(other.storage_, nullptr)) {}

Storage::WriterLease& Storage::WriterLease::operator=(WriterLease&& other) noexcept {
    if (this != &other) {
        if (storage_ != nullptr) {
            storage_->release_writer();
        }
        storage_ = std::exchange(other.storage_, nullptr);
    }
    return *this;
}

bool Storage::set(const std::string& key, const Value& value) {
    MetricTimer timer(runtime_metrics_, LatencyKind::kWrite);
    validate_key(key);
    validate_value(value);
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    const bool inserted = !index_.contains(key);
    publish_mutations_locked({WalMutation{WalOperationType::kSet, key, value}}, false, true, false);
    timer.success();
    return inserted;
}

std::optional<Value> Storage::get(const std::string& key) const {
    MetricTimer timer(runtime_metrics_, LatencyKind::kRead);
    validate_key(key);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    const auto existing = index_.get(key);
    if (!existing.has_value()) {
        runtime_metrics_.increment(MetricCounter::kReads);
        timer.success();
        return std::nullopt;
    }
    runtime_metrics_.increment(MetricCounter::kReads);
    timer.success();
    return existing->value;
}

std::vector<std::optional<Value>> Storage::get_many(const std::vector<std::string>& keys) const {
    constexpr std::size_t kMaximumBatchReadKeys = 10'000U;
    if (keys.size() > kMaximumBatchReadKeys) {
        throw InvalidConfigurationError("get_many accepts at most 10000 keys.");
    }
    for (const auto& key : keys) {
        validate_key(key);
    }
    MetricTimer timer(runtime_metrics_, LatencyKind::kRead);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    std::vector<std::optional<Value>> results;
    results.reserve(keys.size());
    for (const auto& key : keys) {
        const auto record = index_.get(key);
        results.emplace_back(record.has_value()
                                 ? std::optional<Value>(record->value)
                                 : std::nullopt);
    }
    runtime_metrics_.increment(MetricCounter::kReads, static_cast<std::uint64_t>(keys.size()));
    timer.success();
    return results;
}

std::vector<bool> Storage::contains_many(const std::vector<std::string>& keys) const {
    constexpr std::size_t kMaximumBatchReadKeys = 10'000U;
    if (keys.size() > kMaximumBatchReadKeys) {
        throw InvalidConfigurationError("contains_many accepts at most 10000 keys.");
    }
    for (const auto& key : keys) {
        validate_key(key);
    }
    MetricTimer timer(runtime_metrics_, LatencyKind::kRead);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    std::vector<bool> results;
    results.reserve(keys.size());
    for (const auto& key : keys) {
        results.push_back(index_.contains(key));
    }
    runtime_metrics_.increment(MetricCounter::kReads, static_cast<std::uint64_t>(keys.size()));
    timer.success();
    return results;
}

bool Storage::exists(const std::string& key) const {
    MetricTimer timer(runtime_metrics_, LatencyKind::kRead);
    validate_key(key);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    const bool result = index_.contains(key);
    runtime_metrics_.increment(MetricCounter::kReads);
    timer.success();
    return result;
}

bool Storage::erase(const std::string& key) {
    MetricTimer timer(runtime_metrics_, LatencyKind::kDelete);
    validate_key(key);
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    ensure_writable();
    const bool existed = index_.contains(key);
    publish_mutations_locked({WalMutation{WalOperationType::kDelete, key, Value(nullptr)}}, false, true, false);
    timer.success();
    return existed;
}

std::size_t Storage::count() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    return index_.size();
}

std::vector<std::pair<std::string, Value>> Storage::list_records() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    std::vector<std::pair<std::string, Value>> rows;
    rows.reserve(index_.size());
    for (const auto& [key, record] : index_.entries()) {
        rows.emplace_back(key, record.value);
    }
    std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    return rows;
}

bool Storage::is_empty() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    return index_.empty();
}

void Storage::clear() {
    MetricTimer timer(runtime_metrics_, LatencyKind::kClear);
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    publish_mutations_locked({WalMutation{WalOperationType::kClear, "", Value(nullptr)}}, false, true, false);
    timer.success();
}

void Storage::close() {
    stop_auto_snapshot_worker();
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    closed_ = true;
    if (wal_) {
        wal_->close();
    }
}

bool Storage::is_closed() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return closed_;
}

bool Storage::is_read_only() const noexcept {
    return read_only_;
}

CheckpointStatus Storage::checkpoint_status() const {
    std::lock_guard<std::mutex> lock(auto_snapshot_mutex_);
    return checkpoint_status_;
}

#if defined(KOR_ENABLE_TEST_HOOKS)
void Storage::fail_next_auto_checkpoint_for_testing() {
    std::lock_guard<std::mutex> lock(auto_snapshot_mutex_);
    fail_next_auto_checkpoint_for_testing_ = true;
}
#endif

MemoryStats Storage::memory_stats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (recovery_required_) {
        throw RecoveryError("The database requires close and reopen after an ambiguous commit outcome.");
    }
    return tracker_.stats();
}

StorageStats Storage::stats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (recovery_required_) {
        throw RecoveryError("The database requires close and reopen after an ambiguous commit outcome.");
    }
    const auto stats = tracker_.stats();
    StorageStats result;
    result.record_count = index_.size();
    result.configured_limit_bytes = stats.limit_bytes;
    result.key_bytes = stats.key_bytes;
    result.value_bytes = stats.value_bytes;
    result.record_overhead_bytes = stats.record_overhead_bytes;
    result.index_bytes = stats.index_bytes;
    result.total_bytes = stats.total_bytes;
    result.logical_live_data_bytes_estimate = stats.total_bytes;
    result.closed = closed_;
    if (wal_) {
        result.physical_shrinking_supported = true;
        const auto file_stats = wal_->stats();
        result.file_size_bytes = file_stats.file_size_bytes;
        result.wal_bytes_required = file_stats.committed_wal_bytes;
        result.snapshot_bytes_retained = file_stats.checkpoint_bytes;
        result.allocated_region_bytes =
            file_stats.file_size_bytes > file_stats.wal_offset
                ? file_stats.file_size_bytes - file_stats.wal_offset
                : 0U;
        result.obsolete_bytes_estimate = file_stats.uncommitted_tail_bytes;
        result.internal_fragmentation_estimate = 0U;
        result.last_checkpoint_lsn = file_stats.checkpoint_lsn;
        result.compaction_generation = file_stats.checkpoint_generation;
    }
    {
        std::lock_guard<std::mutex> status_lock(auto_snapshot_mutex_);
        result.last_auto_snapshot_error = last_auto_snapshot_error_;
    }
    return result;
}

RuntimeMetricsSnapshot Storage::runtime_metrics() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto result = runtime_metrics_.snapshot();
    result.closed = closed_;
    result.recovery_required = recovery_required_;
    result.active_index_count = static_cast<std::uint64_t>(secondary_indexes_.list().size());
    if (wal_) {
        const auto wal_stats = wal_->stats();
        result.current_committed_lsn = wal_stats.committed_lsn;
        result.last_checkpoint_lsn = wal_stats.checkpoint_lsn;
        result.counters[static_cast<std::size_t>(MetricCounter::kWalRecordsWritten)] =
            wal_stats.records_written;
        result.counters[static_cast<std::size_t>(MetricCounter::kWalBytesWritten)] =
            wal_stats.bytes_written;
    }
    return result;
}

CompactionResult Storage::compact() {
    return checkpoint_internal(true);
}

CompactionResult Storage::checkpoint() {
    return checkpoint_internal(false);
}

CompactionResult Storage::checkpoint_internal(bool require_shrink) {
    MetricTimer timer(runtime_metrics_, LatencyKind::kCompaction);
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    ensure_writable();
    if (!wal_) {
        throw PersistenceError("Compaction requires a persistent .kr database.");
    }

    const auto before = wal_->stats();
    CompactionResult result;
    result.file_size_before_bytes = before.file_size_bytes;
    result.checkpoint_lsn = wal_->committed_lsn();
    if (!require_shrink && result.checkpoint_lsn <= before.checkpoint_lsn) {
        result.file_size_after_bytes = before.file_size_bytes;
        result.compaction_generation = before.checkpoint_generation;
        result.limitation = "No committed WAL records require a new checkpoint.";
        timer.success();
        return result;
    }
    result.compaction_generation = before.checkpoint_generation + 1U;
    if (result.compaction_generation == 0U) {
        throw PersistenceError("Compaction generation overflowed.");
    }

    std::vector<CheckpointRecordRef> records;
    if (index_.size() > std::numeric_limits<std::uint64_t>::max() / sizeof(CheckpointRecordRef)) {
        throw MemoryLimitExceededError("Compaction record-reference estimate overflowed.");
    }
    tracker_.ensure_additional_bytes(
        static_cast<std::uint64_t>(index_.size()) * sizeof(CheckpointRecordRef));
    records.reserve(index_.size());
    for (const auto& [key, record] : index_.entries()) {
        records.push_back(CheckpointRecordRef{&key, &record.value});
    }
    std::sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
        return *lhs.key < *rhs.key;
    });
    std::vector<CheckpointIndexDefinition> definitions;
    const auto index_list = secondary_indexes_.list();
    definitions.reserve(index_list.size());
    for (const auto& entry : index_list) {
        CheckpointIndexDefinition definition;
        definition.name = entry.name;
        definition.field = entry.field;
        definition.fields = entry.fields;
        definition.type = entry.index_type == "ordered" ? 2U : 1U;
        definition.unique = entry.unique;
        definition.generation = entry.generation;
        definitions.push_back(std::move(definition));
    }

    std::uint64_t checkpoint_size_estimate = 56U;
    for (const auto& record : records) {
        const auto encoded_value = serialize_value(*record.value);
        checkpoint_size_estimate = checked_add(checkpoint_size_estimate, 16U);
        checkpoint_size_estimate = checked_add(checkpoint_size_estimate,
                                               static_cast<std::uint64_t>(record.key->size()));
        checkpoint_size_estimate = checked_add(checkpoint_size_estimate,
                                               static_cast<std::uint64_t>(encoded_value.size()));
    }
    checkpoint_size_estimate = checked_add(checkpoint_size_estimate, 8U);
    for (const auto& definition : definitions) {
        checkpoint_size_estimate = checked_add(checkpoint_size_estimate, 26U);
        checkpoint_size_estimate = checked_add(checkpoint_size_estimate,
                                               static_cast<std::uint64_t>(definition.name.size()));
        checkpoint_size_estimate = checked_add(checkpoint_size_estimate,
                                               static_cast<std::uint64_t>(definition.field.size()));
        checkpoint_size_estimate = checked_add(checkpoint_size_estimate, 8U);
        for (const auto& indexed_field : definition.fields) {
            checkpoint_size_estimate = checked_add(checkpoint_size_estimate,
                                                   8U + static_cast<std::uint64_t>(indexed_field.size()));
        }
    }
    std::uint64_t temporary_memory_estimate = checked_add(
        checkpoint_size_estimate,
        checkpoint_size_estimate);
    temporary_memory_estimate = checked_add(
        temporary_memory_estimate,
        static_cast<std::uint64_t>(records.size()) * sizeof(CheckpointRecordRef));
    for (const auto& definition : definitions) {
        temporary_memory_estimate = checked_add(
            temporary_memory_estimate,
            static_cast<std::uint64_t>(definition.name.size() + definition.field.size()) + 64U);
        for (const auto& indexed_field : definition.fields) {
            temporary_memory_estimate = checked_add(
                temporary_memory_estimate,
                static_cast<std::uint64_t>(indexed_field.size()));
        }
    }
    tracker_.ensure_additional_bytes(temporary_memory_estimate);
    result.checkpoint_staging_memory_estimate_bytes = temporary_memory_estimate;

    const auto checkpoint = serialize_checkpoint_refs(
        result.compaction_generation,
        result.checkpoint_lsn,
        records,
        definitions);
    bool compacted = false;
    try {
        compacted = wal_->publish_checkpoint(checkpoint, result.checkpoint_lsn, require_shrink);
    } catch (const PersistenceError&) {
        runtime_metrics_.increment(MetricCounter::kCheckpointFailures);
        if (!wal_->is_open()) {
            recovery_required_ = true;
        }
        throw;
    }
    if (compacted) {
        runtime_metrics_.increment(MetricCounter::kCheckpointCount);
        result.checkpoint_performed = true;
        result.file_physically_shrunk = require_shrink;
        result.logical_wal_bytes_reclaimed = before.committed_wal_bytes;
        result.compaction_generation = wal_->stats().checkpoint_generation;
        result.file_size_after_bytes = wal_->stats().file_size_bytes;
        result.file_length_reduction_bytes =
            result.file_size_before_bytes > result.file_size_after_bytes
                ? result.file_size_before_bytes - result.file_size_after_bytes
                : 0U;
        if (require_shrink && result.file_size_after_bytes >= result.file_size_before_bytes) {
            recovery_required_ = true;
            throw PersistenceError("Compaction published but did not reduce the physical file size.");
        }
    } else {
        result.file_size_after_bytes = wal_->stats().file_size_bytes;
        result.limitation = "The validated checkpoint would not reduce the current file size.";
    }
    runtime_metrics_.increment(MetricCounter::kCompactionCount);
    if (compacted && !require_shrink) {
        result.limitation.clear();
    }
    timer.success();
    return result;
}

bool Storage::set_if_absent(const std::string& key, const Value& value) {
    validate_key(key);
    validate_value(value);
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    ensure_writable();
    if (index_.contains(key)) {
        return false;
    }
    publish_mutations_locked({WalMutation{WalOperationType::kSet, key, value}}, false, true, false);
    return true;
}

bool Storage::replace(const std::string& key, const Value& value) {
    validate_key(key);
    validate_value(value);
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    ensure_writable();
    if (!index_.contains(key)) {
        return false;
    }
    publish_mutations_locked({WalMutation{WalOperationType::kSet, key, value}}, false, true, false);
    return true;
}

bool Storage::compare_and_set(const std::string& key,
                             const Value& expected,
                             const Value& desired) {
    validate_key(key);
    validate_value(expected);
    validate_value(desired);
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    ensure_writable();
    const auto current = index_.get(key);
    if (!current.has_value() || current->value != expected) {
        return false;
    }
    publish_mutations_locked({WalMutation{WalOperationType::kSet, key, desired}}, false, true, false);
    return true;
}

std::optional<Value> Storage::get_and_set(const std::string& key, const Value& value) {
    validate_key(key);
    validate_value(value);
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    ensure_writable();
    const auto current = index_.get(key);
    publish_mutations_locked({WalMutation{WalOperationType::kSet, key, value}}, false, true, false);
    return current.has_value() ? std::optional<Value>(current->value) : std::nullopt;
}

std::optional<Value> Storage::get_and_delete(const std::string& key) {
    validate_key(key);
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    ensure_writable();
    const auto current = index_.get(key);
    if (current.has_value()) {
        publish_mutations_locked({WalMutation{WalOperationType::kDelete, key, Value(nullptr)}}, false, true, false);
    }
    return current.has_value() ? std::optional<Value>(current->value) : std::nullopt;
}

void Storage::set_many(const std::vector<std::pair<std::string, Value>>& entries) {
    ensure_writable();
    if (entries.empty()) {
        return;
    }
    std::vector<WalMutation> mutations;
    mutations.reserve(entries.size());
    for (const auto& [key, value] : entries) {
        mutations.push_back(WalMutation{WalOperationType::kSet, key, value});
    }
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    ensure_writable();
    publish_mutations_locked(mutations, false, true, true);
}

void Storage::set_many_unique(const std::vector<std::pair<std::string, Value>>& entries) {
    ensure_writable();
    if (entries.empty()) {
        return;
    }
    std::vector<WalMutation> mutations;
    mutations.reserve(entries.size());
    for (const auto& [key, value] : entries) {
        mutations.push_back(WalMutation{WalOperationType::kSet, key, value});
    }
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    ensure_writable();
    publish_mutations_locked(mutations, false, true, true, true);
}

void Storage::delete_many(const std::vector<std::string>& keys) {
    ensure_writable();
    if (keys.empty()) {
        return;
    }
    std::vector<WalMutation> mutations;
    mutations.reserve(keys.size());
    for (const auto& key : keys) {
        mutations.push_back(WalMutation{WalOperationType::kDelete, key, Value(nullptr)});
    }
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    ensure_writable();
    publish_mutations_locked(mutations, false, true, true);
}

void Storage::delete_many_unique(const std::vector<std::string>& keys) {
    ensure_writable();
    if (keys.empty()) {
        return;
    }
    std::vector<WalMutation> mutations;
    mutations.reserve(keys.size());
    for (const auto& key : keys) {
        mutations.push_back(WalMutation{WalOperationType::kDelete, key, Value(nullptr)});
    }
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    ensure_writable();
    publish_mutations_locked(mutations, false, true, true, true);
}

std::unique_ptr<Transaction> Storage::transaction() {
    if (read_only_) {
        throw ReadOnlyError("Transactions are not available on a read-only database.");
    }
    return std::make_unique<Transaction>(*this);
}

Storage::WriterLease Storage::acquire_writer() {
    std::unique_lock<std::mutex> lock(writer_gate_mutex_);
    if (writer_active_ && writer_owner_ == std::this_thread::get_id()) {
        throw TransactionStateError("A writer operation cannot be nested inside an active transaction.");
    }
    writer_gate_condition_.wait(lock, [this] { return !writer_active_; });
    writer_active_ = true;
    writer_owner_ = std::this_thread::get_id();
    return WriterLease(this);
}

void Storage::release_writer() noexcept {
    {
        std::lock_guard<std::mutex> lock(writer_gate_mutex_);
        writer_active_ = false;
        writer_owner_ = std::thread::id();
    }
    writer_gate_condition_.notify_one();
}

void Storage::ensure_usable() const {
    ensure_not_closed();
    if (recovery_required_) {
        throw RecoveryError("The database requires close and reopen after an ambiguous commit outcome.");
    }
}

void Storage::ensure_writable() const {
    if (read_only_) {
        throw ReadOnlyError("The database was opened in read-only mode.");
    }
}

void Storage::publish_mutations_locked(const std::vector<WalMutation>& mutations,
                                      bool clear_first,
                                      bool persist,
                                      bool transaction_record,
                                      bool unique_keys) {
    ensure_writable();
    tracker_.ensure_additional_bytes(secondary_indexes_.estimated_memory_bytes());

    // Bulk writes and deletes with no secondary indexes can apply directly.
    // Copying the complete primary index on every batch made repeated bulk
    // operations quadratic even though the staged change was immutable.
    const bool direct_fast_path = !clear_first && secondary_indexes_.estimated_memory_bytes() == 0U;
    if (direct_fast_path) {
        MemoryTracker preflight_tracker = tracker_;
        std::unordered_map<std::string, std::optional<Value>> simulated_records;
        simulated_records.reserve(mutations.size());
        if (unique_keys) {
            index_.reserve(mutations.size());
            for (const auto& mutation : mutations) {
                const auto record = index_.get(mutation.key);
                if (record.has_value()) {
                    preflight_tracker.replace_record(mutation.key, record->value, mutation.value);
                } else {
                    preflight_tracker.add_record(mutation.key, mutation.value);
                }
            }
        } else {
            for (const auto& mutation : mutations) {
                const auto simulated = simulated_records.find(mutation.key);
                std::optional<Value> current;
                if (simulated != simulated_records.end()) {
                    current = simulated->second;
                } else {
                    const auto record = index_.get(mutation.key);
                    if (record.has_value()) {
                        current = record->value;
                    }
                }
                if (mutation.operation == WalOperationType::kSet) {
                    if (current.has_value()) {
                        preflight_tracker.replace_record(mutation.key, *current, mutation.value);
                    } else {
                        preflight_tracker.add_record(mutation.key, mutation.value);
                    }
                    simulated_records[mutation.key] = mutation.value;
                } else if (mutation.operation == WalOperationType::kDelete && current.has_value()) {
                    preflight_tracker.remove_record(mutation.key, *current);
                    simulated_records[mutation.key] = std::nullopt;
                }
            }
        }

        if (persist && wal_) {
            try {
                if (transaction_record) {
                    wal_->append_transaction(mutations);
                } else if (mutations.size() == 1U) {
                    const auto& mutation = mutations.front();
                    if (mutation.operation == WalOperationType::kSet) {
                        wal_->append_set(mutation.key, mutation.value);
                    } else if (mutation.operation == WalOperationType::kDelete) {
                        wal_->append_delete(mutation.key);
                    } else if (mutation.operation == WalOperationType::kClear) {
                        wal_->append_clear();
                    }
                }
            } catch (const TransactionCommitError&) {
                recovery_required_ = true;
                throw;
            } catch (const PersistenceError&) {
                recovery_required_ = true;
                throw;
            }
        }

        index_.reserve(mutations.size());
        for (const auto& mutation : mutations) {
            apply_mutation(index_, tracker_, mutation);
        }
        for (const auto& mutation : mutations) {
            if (mutation.operation == WalOperationType::kSet) {
                runtime_metrics_.increment(MetricCounter::kWrites);
            } else if (mutation.operation == WalOperationType::kDelete) {
                runtime_metrics_.increment(MetricCounter::kDeletes);
            }
        }
        return;
    }

    HashIndex staged_index = index_;
    MemoryTracker staged_tracker = tracker_;
    SecondaryIndexManager staged_secondary_indexes = secondary_indexes_;
    staged_tracker.set_index_bytes(0U);
    if (clear_first) {
        apply_mutation(staged_index, staged_tracker,
                       WalMutation{WalOperationType::kClear, "", Value(nullptr)});
    }
    for (const auto& mutation : mutations) {
        apply_mutation(staged_index, staged_tracker, mutation);
    }
    staged_secondary_indexes.reconcile(index_, staged_index, mutations, clear_first);
    const auto staged_index_bytes = staged_secondary_indexes.estimated_memory_bytes();
    tracker_.ensure_additional_bytes(staged_index_bytes);
    staged_tracker.set_index_bytes(staged_index_bytes);

    if (persist && wal_) {
        try {
            if (transaction_record) {
                wal_->append_transaction(mutations);
            } else if (mutations.size() == 1U) {
                const auto& mutation = mutations.front();
                if (mutation.operation == WalOperationType::kSet) {
                    wal_->append_set(mutation.key, mutation.value);
                } else if (mutation.operation == WalOperationType::kDelete) {
                    wal_->append_delete(mutation.key);
                } else if (mutation.operation == WalOperationType::kClear) {
                    wal_->append_clear();
                }
            } else if (clear_first || !mutations.empty()) {
                throw PersistenceError("Invalid standalone WAL mutation batch.");
            }
        } catch (const TransactionCommitError&) {
            recovery_required_ = true;
            throw;
        } catch (const PersistenceError&) {
            recovery_required_ = true;
            throw;
        }
    }

    index_.swap(staged_index);
    tracker_ = staged_tracker;
    secondary_indexes_.swap(staged_secondary_indexes);
    for (const auto& mutation : mutations) {
        if (mutation.operation == WalOperationType::kSet) {
            runtime_metrics_.increment(MetricCounter::kWrites);
        } else if (mutation.operation == WalOperationType::kDelete) {
            runtime_metrics_.increment(MetricCounter::kDeletes);
        } else if (mutation.operation == WalOperationType::kClear) {
            runtime_metrics_.increment(MetricCounter::kClearOperations);
        }
    }
}

void Storage::commit_transaction(const std::vector<WalMutation>& mutations, bool clear_first) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    std::vector<WalMutation> complete;
    complete.reserve(mutations.size() + (clear_first ? 1U : 0U));
    if (clear_first) {
        complete.push_back(WalMutation{WalOperationType::kClear, "", Value(nullptr)});
    }
    complete.insert(complete.end(), mutations.begin(), mutations.end());
    if (complete.empty()) {
        return;
    }
    publish_mutations_locked(complete, false, true, true);
}

void Storage::ensure_not_closed() const {
    if (closed_) {
        throw StorageClosedError("Storage engine is closed.");
    }
}

std::uint64_t Storage::record_overhead_bytes(const std::string& key, const Value& value) {
    (void)key;
    (void)value;
    return 32ULL;
}

void Storage::create_index(const std::string& name,
                           const std::string& field,
                           SecondaryIndexType type,
                           bool unique) {
    MetricTimer timer(runtime_metrics_, LatencyKind::kIndexBuild);
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    ensure_writable();

    IndexDefinition definition;
    definition.name = name;
    definition.field = field;
    definition.fields = {field};
    definition.type = type;
    definition.unique = unique;
    definition.generation = wal_ ? wal_->next_lsn() : 1U;
    SecondaryIndexManager::validate_definition(definition);
    if (secondary_indexes_.contains(name)) {
        throw IndexExistsError("An index with this name already exists.");
    }
    tracker_.ensure_additional_bytes(secondary_indexes_.estimated_memory_bytes());
    const auto memory = tracker_.stats();
    const auto stage_budget = memory.limit_bytes == 0U
                                  ? std::numeric_limits<std::uint64_t>::max()
                                  : (memory.total_bytes >= memory.limit_bytes
                                         ? 0U
                                         : memory.limit_bytes - memory.total_bytes);
    auto staged_indexes = secondary_indexes_;
    auto staged_tracker = tracker_;
    staged_indexes.create(definition, index_, stage_budget);
    const auto staged_index_bytes = staged_indexes.estimated_memory_bytes();
    tracker_.ensure_additional_bytes(staged_index_bytes);
    staged_tracker.set_index_bytes(staged_index_bytes);

    if (wal_) {
        try {
            wal_->append_index_create(name,
                                      definition.fields,
                                      type == SecondaryIndexType::kHash ? "hash" : "ordered",
                                      unique,
                                      definition.generation);
        } catch (const TransactionCommitError&) {
            recovery_required_ = true;
            throw;
        } catch (const PersistenceError&) {
            recovery_required_ = true;
            throw;
        }
    }
    secondary_indexes_.swap(staged_indexes);
    tracker_ = staged_tracker;
    runtime_metrics_.increment(MetricCounter::kIndexBuildCount);
    timer.success();
}

void Storage::create_composite_index(const std::string& name,
                                     const std::vector<std::string>& fields,
                                     bool unique) {
    MetricTimer timer(runtime_metrics_, LatencyKind::kIndexBuild);
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    ensure_writable();
    if (fields.size() < 2U || fields.size() > 4U) {
        throw InvalidIndexDefinitionError("Composite indexes require between 2 and 4 fields.");
    }

    IndexDefinition definition;
    definition.name = name;
    definition.fields = fields;
    definition.field = fields.empty() ? std::string() : fields.front();
    definition.type = SecondaryIndexType::kOrdered;
    definition.unique = unique;
    definition.generation = wal_ ? wal_->next_lsn() : 1U;
    SecondaryIndexManager::validate_definition(definition);
    if (secondary_indexes_.contains(name)) {
        throw IndexExistsError("An index with this name already exists.");
    }

    tracker_.ensure_additional_bytes(secondary_indexes_.estimated_memory_bytes());
    const auto memory = tracker_.stats();
    const auto stage_budget = memory.limit_bytes == 0U
                                  ? std::numeric_limits<std::uint64_t>::max()
                                  : (memory.total_bytes >= memory.limit_bytes
                                         ? 0U
                                         : memory.limit_bytes - memory.total_bytes);
    auto staged_indexes = secondary_indexes_;
    auto staged_tracker = tracker_;
    staged_indexes.create(definition, index_, stage_budget);
    const auto staged_index_bytes = staged_indexes.estimated_memory_bytes();
    tracker_.ensure_additional_bytes(staged_index_bytes);
    staged_tracker.set_index_bytes(staged_index_bytes);

    if (wal_) {
        try {
            wal_->append_index_create(name, fields, "ordered", unique, definition.generation);
        } catch (const TransactionCommitError&) {
            recovery_required_ = true;
            throw;
        } catch (const PersistenceError&) {
            recovery_required_ = true;
            throw;
        }
    }
    secondary_indexes_.swap(staged_indexes);
    tracker_ = staged_tracker;
    runtime_metrics_.increment(MetricCounter::kIndexBuildCount);
    timer.success();
}

void Storage::drop_index(const std::string& name) {
    auto writer = acquire_writer();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    ensure_writable();

    tracker_.ensure_additional_bytes(secondary_indexes_.estimated_memory_bytes());
    auto staged_indexes = secondary_indexes_;
    auto staged_tracker = tracker_;
    staged_indexes.drop(name);
    const auto staged_index_bytes = staged_indexes.estimated_memory_bytes();
    staged_tracker.set_index_bytes(staged_index_bytes);
    if (wal_) {
        try {
            wal_->append_index_drop(name);
        } catch (const TransactionCommitError&) {
            recovery_required_ = true;
            throw;
        } catch (const PersistenceError&) {
            recovery_required_ = true;
            throw;
        }
    }
    secondary_indexes_.swap(staged_indexes);
    tracker_ = staged_tracker;
}

std::vector<IndexInfo> Storage::list_indexes() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    return secondary_indexes_.list();
}

std::vector<std::pair<std::string, Value>> Storage::find(const std::string& field, const Value& value) const {
    MetricTimer timer(runtime_metrics_, LatencyKind::kQuery);
    if (field.empty() || field.find('\0') != std::string::npos || !is_valid_utf8(field)) {
        throw InvalidIndexDefinitionError("Query fields must be non-empty valid UTF-8 top-level field names.");
    }
    if (!value.is_null() && !SecondaryIndexManager::is_supported_scalar(value)) {
        throw InvalidIndexDefinitionError("Equality queries require strings, booleans, integers, or finite floats.");
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    if (value.is_null()) {
        timer.success();
        return {};
    }

    std::vector<std::string> candidates;
    const auto indexed = secondary_indexes_.find_equal(field, value);
    if (indexed.has_value()) {
        runtime_metrics_.increment(MetricCounter::kIndexedQueries);
        candidates = *indexed;
    } else {
        runtime_metrics_.increment(MetricCounter::kFullScanQueries);
        candidates.reserve(index_.size());
        for (const auto& [key, record] : index_.entries()) {
            if (!record.value.is_map()) {
                continue;
            }
            const auto& fields = std::get<ValueMap>(record.value.data);
            const auto found = fields.find(field);
            if (found != fields.end() && !found->second.is_null() && found->second == value) {
                candidates.push_back(key);
            }
        }
    }
    std::sort(candidates.begin(), candidates.end());
    std::vector<std::pair<std::string, Value>> results;
    results.reserve(candidates.size());
    for (const auto& key : candidates) {
        const auto record = index_.get(key);
        if (!record.has_value() || !record->value.is_map()) {
            continue;
        }
        const auto& fields = std::get<ValueMap>(record->value.data);
        const auto found = fields.find(field);
        if (found != fields.end() && found->second == value) {
            results.emplace_back(key, record->value);
        }
    }
    timer.success();
    return results;
}

std::vector<std::pair<std::string, Value>> Storage::find_range(const std::string& field,
                                                               const std::optional<Value>& minimum,
                                                               const std::optional<Value>& maximum,
                                                               bool include_minimum,
                                                               bool include_maximum) const {
    MetricTimer timer(runtime_metrics_, LatencyKind::kQuery);
    if (field.empty() || field.find('\0') != std::string::npos || !is_valid_utf8(field)) {
        throw InvalidIndexDefinitionError("Query fields must be non-empty valid UTF-8 top-level field names.");
    }
    SecondaryIndexManager::validate_range_bounds(minimum, maximum);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();

    std::vector<std::string> candidates;
    const auto indexed = secondary_indexes_.find_range(field, minimum, maximum, include_minimum, include_maximum);
    if (indexed.has_value()) {
        runtime_metrics_.increment(MetricCounter::kIndexedQueries);
        candidates = *indexed;
    } else {
        runtime_metrics_.increment(MetricCounter::kFullScanQueries);
        candidates.reserve(index_.size());
        for (const auto& [key, record] : index_.entries()) {
            if (!record.value.is_map()) {
                continue;
            }
            const auto& fields = std::get<ValueMap>(record.value.data);
            const auto found = fields.find(field);
            if (found != fields.end() &&
                SecondaryIndexManager::range_matches(found->second, minimum, maximum,
                                                     include_minimum, include_maximum)) {
                candidates.push_back(key);
            }
        }
    }
    std::sort(candidates.begin(), candidates.end());
    std::vector<std::pair<std::string, Value>> results;
    results.reserve(candidates.size());
    for (const auto& key : candidates) {
        const auto record = index_.get(key);
        if (!record.has_value() || !record->value.is_map()) {
            continue;
        }
        const auto& fields = std::get<ValueMap>(record->value.data);
        const auto found = fields.find(field);
        if (found != fields.end() &&
            SecondaryIndexManager::range_matches(found->second, minimum, maximum,
                                                 include_minimum, include_maximum)) {
            results.emplace_back(key, record->value);
        }
    }
    timer.success();
    return results;
}

std::vector<std::pair<std::string, Value>> Storage::find_composite(
    const std::vector<std::string>& fields,
    const std::vector<Value>& values) const {
    if (values.size() != fields.size()) {
        throw InvalidIndexDefinitionError("Composite lookups require matching 2-to-4 field and value lists.");
    }
    IndexDefinition lookup_definition;
    lookup_definition.name = "lookup";
    lookup_definition.fields = fields;
    lookup_definition.field = fields.empty() ? std::string() : fields.front();
    lookup_definition.type = SecondaryIndexType::kOrdered;
    SecondaryIndexManager::validate_definition(lookup_definition);
    for (const auto& value : values) {
        if (!SecondaryIndexManager::is_supported_scalar(value)) {
            throw UnsupportedIndexValueError("Composite lookup values must be supported non-null scalars.");
        }
    }
    MetricTimer timer(runtime_metrics_, LatencyKind::kQuery);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    const auto indexed = secondary_indexes_.find_composite(fields, values);
    std::vector<std::pair<std::string, Value>> results;
    if (indexed.has_value()) {
        results.reserve(indexed->size());
        for (const auto& key : *indexed) {
            const auto record = index_.get(key);
            if (record.has_value()) {
                results.emplace_back(key, record->value);
            }
        }
    } else {
        for (const auto& [key, record] : index_.entries()) {
            if (!record.value.is_map()) {
                continue;
            }
            const auto& map = std::get<ValueMap>(record.value.data);
            bool matches = true;
            for (std::size_t position = 0; position < fields.size(); ++position) {
                const auto found = map.find(fields[position]);
                if (found == map.end() || found->second.data.index() != values[position].data.index() ||
                    !SecondaryIndexManager::is_supported_scalar(found->second) ||
                    found->second != values[position]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                results.emplace_back(key, record.value);
            }
        }
        std::sort(results.begin(), results.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });
    }
    runtime_metrics_.increment(indexed.has_value() ? MetricCounter::kIndexedQueries
                                                   : MetricCounter::kFullScanQueries);
    timer.success();
    return results;
}

std::vector<std::pair<std::string, Value>> Storage::find_composite_range(
    const std::vector<std::string>& fields,
    const std::optional<std::vector<Value>>& minimum,
    const std::optional<std::vector<Value>>& maximum,
    bool include_minimum,
    bool include_maximum) const {
    IndexDefinition lookup_definition;
    lookup_definition.name = "lookup";
    lookup_definition.fields = fields;
    lookup_definition.field = fields.empty() ? std::string() : fields.front();
    lookup_definition.type = SecondaryIndexType::kOrdered;
    SecondaryIndexManager::validate_definition(lookup_definition);
    MetricTimer timer(runtime_metrics_, LatencyKind::kQuery);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    const auto indexed = secondary_indexes_.find_composite_range(
        fields, minimum, maximum, include_minimum, include_maximum);
    if (!indexed.has_value()) {
        throw InvalidIndexDefinitionError("Composite range lookup requires a matching ordered index.");
    }
    std::vector<std::pair<std::string, Value>> results;
    results.reserve(indexed->size());
    for (const auto& key : *indexed) {
        const auto record = index_.get(key);
        if (record.has_value()) {
            results.emplace_back(key, record->value);
        }
    }
    runtime_metrics_.increment(MetricCounter::kIndexedQueries);
    timer.success();
    return results;
}

ScanPage Storage::scan(const std::string& prefix,
                       const std::optional<std::string>& after,
                       std::size_t limit) const {
    MetricTimer timer(runtime_metrics_, LatencyKind::kQuery);
    if (limit == 0U || limit > 1000U) {
        throw InvalidConfigurationError("Scan limit must be between 1 and 1000.");
    }
    if (prefix.find('\0') != std::string::npos || !is_valid_utf8(prefix)) {
        throw InvalidKeyError("Scan prefix must be valid UTF-8 and must not contain NUL.");
    }
    if (after.has_value()) {
        validate_key(*after);
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ensure_usable();
    runtime_metrics_.increment(MetricCounter::kFullScanQueries);
    ScanPage page;
    page.records.reserve(limit + 1U);
    for (const auto& [key, record] : index_.entries()) {
        if ((!prefix.empty() && key.compare(0U, prefix.size(), prefix) != 0) ||
            (after.has_value() && key <= *after)) {
            continue;
        }
        const auto position = std::lower_bound(
            page.records.begin(),
            page.records.end(),
            key,
            [](const auto& entry, const std::string& candidate) {
                return entry.first < candidate;
            });
        page.records.insert(position, std::make_pair(key, record.value));
        if (page.records.size() > limit + 1U) {
            page.records.pop_back();
        }
    }
    page.has_more = page.records.size() > limit;
    if (page.has_more) {
        page.records.pop_back();
        if (!page.records.empty()) {
            page.next_cursor = page.records.back().first;
        }
    }
    timer.success();
    return page;
}

}  // namespace kor
