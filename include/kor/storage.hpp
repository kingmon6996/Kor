#pragma once

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kor/hash_index.hpp"
#include "kor/memory_tracker.hpp"
#include "kor/observability.hpp"
#include "kor/record.hpp"
#include "kor/secondary_index.hpp"
#include "kor/snapshot.hpp"
#include "kor/value.hpp"
#include "kor/wal.hpp"

namespace kor {

class Transaction;

struct StorageStats {
    std::size_t record_count = 0;
    std::uint64_t configured_limit_bytes = 0;
    std::uint64_t key_bytes = 0;
    std::uint64_t value_bytes = 0;
    std::uint64_t record_overhead_bytes = 0;
    std::uint64_t index_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t file_size_bytes = 0;
    std::uint64_t logical_live_data_bytes_estimate = 0;
    std::uint64_t wal_bytes_required = 0;
    std::uint64_t snapshot_bytes_retained = 0;
    std::uint64_t allocated_region_bytes = 0;
    std::uint64_t reusable_free_space_bytes = 0;
    std::uint64_t obsolete_bytes_estimate = 0;
    std::uint64_t internal_fragmentation_estimate = 0;
    std::uint64_t last_checkpoint_lsn = 0;
    std::uint64_t compaction_generation = 0;
    std::string last_auto_snapshot_error;
    bool physical_shrinking_supported = false;
    bool closed = false;
};

struct CheckpointStatus {
    std::uint64_t last_attempt_lsn = 0U;
    std::uint64_t last_success_lsn = 0U;
    std::uint64_t last_attempt_duration_ns = 0U;
    std::uint64_t attempt_count = 0U;
    std::uint64_t success_count = 0U;
    std::uint64_t failure_count = 0U;
    bool in_progress = false;
    std::string last_error;
};

struct ScanPage {
    std::vector<std::pair<std::string, Value>> records;
    std::optional<std::string> next_cursor;
    bool has_more = false;
};

struct CompactionResult {
    std::uint64_t file_size_before_bytes = 0U;
    std::uint64_t file_size_after_bytes = 0U;
    std::uint64_t logical_wal_bytes_reclaimed = 0U;
    std::uint64_t file_length_reduction_bytes = 0U;
    std::uint64_t checkpoint_staging_memory_estimate_bytes = 0U;
    std::uint64_t checkpoint_lsn = 0U;
    std::uint64_t compaction_generation = 0U;
    bool checkpoint_performed = false;
    bool file_physically_shrunk = false;
    std::string limitation;
};

class Storage {
public:
    class WriterLease {
    public:
        WriterLease() noexcept = default;
        ~WriterLease();

        WriterLease(const WriterLease&) = delete;
        WriterLease& operator=(const WriterLease&) = delete;
        WriterLease(WriterLease&& other) noexcept;
        WriterLease& operator=(WriterLease&& other) noexcept;

    private:
        friend class Storage;
        explicit WriterLease(Storage* storage) noexcept;
        Storage* storage_ = nullptr;
    };

    explicit Storage(std::uint64_t max_memory_bytes = 64ULL * 1024ULL * 1024ULL,
                    const std::filesystem::path& database_path = std::filesystem::path(),
                    bool read_only = false);
    ~Storage();

    bool set(const std::string& key, const Value& value);
    std::optional<Value> get(const std::string& key) const;
    std::vector<std::optional<Value>> get_many(const std::vector<std::string>& keys) const;
    std::vector<bool> contains_many(const std::vector<std::string>& keys) const;
    bool exists(const std::string& key) const;
    bool erase(const std::string& key);
    std::size_t count() const;
    std::vector<std::pair<std::string, Value>> list_records() const;
    bool is_empty() const;
    void clear();
    void close();
    bool is_closed() const noexcept;

    MemoryStats memory_stats() const;
    StorageStats stats() const;
    RuntimeMetricsSnapshot runtime_metrics() const;
    CompactionResult compact();
    CompactionResult checkpoint();
    CheckpointStatus checkpoint_status() const;
    bool is_read_only() const noexcept;
#if defined(KOR_ENABLE_TEST_HOOKS)
    void fail_next_auto_checkpoint_for_testing();
#endif

    bool set_if_absent(const std::string& key, const Value& value);
    bool replace(const std::string& key, const Value& value);
    bool compare_and_set(const std::string& key,
                        const Value& expected,
                        const Value& desired);
    std::optional<Value> get_and_set(const std::string& key, const Value& value);
    std::optional<Value> get_and_delete(const std::string& key);
    void set_many(const std::vector<std::pair<std::string, Value>>& entries);
    void set_many_unique(const std::vector<std::pair<std::string, Value>>& entries);
    void delete_many(const std::vector<std::string>& keys);
    void delete_many_unique(const std::vector<std::string>& keys);
    std::unique_ptr<Transaction> transaction();
    void create_index(const std::string& name,
                      const std::string& field,
                      SecondaryIndexType type = SecondaryIndexType::kHash,
                      bool unique = false);
    void create_composite_index(const std::string& name,
                                const std::vector<std::string>& fields,
                                bool unique = false);
    void drop_index(const std::string& name);
    std::vector<IndexInfo> list_indexes() const;
    std::vector<std::pair<std::string, Value>> find(const std::string& field, const Value& value) const;
    std::vector<std::pair<std::string, Value>> find_range(const std::string& field,
                                                          const std::optional<Value>& minimum,
                                                          const std::optional<Value>& maximum,
                                                          bool include_minimum = true,
                                                          bool include_maximum = true) const;
    std::vector<std::pair<std::string, Value>> find_composite(
        const std::vector<std::string>& fields,
        const std::vector<Value>& values) const;
    std::vector<std::pair<std::string, Value>> find_composite_range(
        const std::vector<std::string>& fields,
        const std::optional<std::vector<Value>>& minimum,
        const std::optional<std::vector<Value>>& maximum,
        bool include_minimum = true,
        bool include_maximum = true) const;
    ScanPage scan(const std::string& prefix,
                  const std::optional<std::string>& after,
                  std::size_t limit) const;
    void stop_auto_snapshot_worker() noexcept;

private:
    friend class Transaction;

    WriterLease acquire_writer();
    void release_writer() noexcept;
    void start_auto_snapshot_worker();
    void auto_snapshot_loop() noexcept;
    void record_auto_snapshot_error(const std::string& error);
    CompactionResult checkpoint_internal(bool require_shrink);
    void ensure_usable() const;
    void ensure_writable() const;
    void publish_mutations_locked(const std::vector<WalMutation>& mutations,
                                  bool clear_first,
                                  bool persist,
                                  bool transaction_record,
                                  bool unique_keys = false);
    void commit_transaction(const std::vector<WalMutation>& mutations,
                            bool clear_first);
    void ensure_not_closed() const;
    static std::uint64_t record_overhead_bytes(const std::string& key, const Value& value);

    mutable std::shared_mutex mutex_;
    mutable std::mutex writer_gate_mutex_;
    std::condition_variable writer_gate_condition_;
    bool writer_active_ = false;
    std::thread::id writer_owner_;
    bool closed_ = false;
    bool recovery_required_ = false;
    bool read_only_ = false;
    HashIndex index_;
    SecondaryIndexManager secondary_indexes_;
    MemoryTracker tracker_;
    std::unique_ptr<WalLog> wal_;
    mutable RuntimeMetrics runtime_metrics_;
    mutable std::mutex auto_snapshot_mutex_;
    std::condition_variable auto_snapshot_condition_;
    std::thread auto_snapshot_worker_;
    bool stop_auto_snapshot_worker_ = false;
    std::string last_auto_snapshot_error_;
    CheckpointStatus checkpoint_status_;
#if defined(KOR_ENABLE_TEST_HOOKS)
    bool fail_next_auto_checkpoint_for_testing_ = false;
#endif
};

}  // namespace kor
