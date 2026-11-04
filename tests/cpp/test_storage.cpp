#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "kor/database_file.hpp"
#include "kor/exceptions.hpp"
#include "kor/storage.hpp"
#include "kor/transaction.hpp"
#include "kor/value.hpp"

TEST(KorStorage, StoresAndRetrievesValues) {
    kor::Storage storage(1024ULL * 1024ULL);

    EXPECT_TRUE(storage.set("user", kor::Value(std::string("Kingmon"))));
    EXPECT_TRUE(storage.exists("user"));

    const auto value = storage.get("user");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(std::get<std::string>(value->data), "Kingmon");
    EXPECT_EQ(storage.count(), 1U);
    EXPECT_FALSE(storage.is_empty());
}

TEST(KorStorage, HandlesNullAndDelete) {
    kor::Storage storage(1024ULL * 1024ULL);

    EXPECT_TRUE(storage.set("alpha", kor::Value(nullptr)));
    EXPECT_TRUE(storage.exists("alpha"));
    EXPECT_TRUE(storage.get("alpha").has_value());
    EXPECT_TRUE(storage.erase("alpha"));
    EXPECT_FALSE(storage.exists("alpha"));
    EXPECT_FALSE(storage.get("alpha").has_value());
}

TEST(KorStorage, CompareAndSetAndGetAndDelete) {
    kor::Storage storage(1024ULL * 1024ULL);
    EXPECT_TRUE(storage.set("count", kor::Value(std::int64_t{1})));

    EXPECT_TRUE(storage.compare_and_set("count", kor::Value(std::int64_t{1}), kor::Value(std::int64_t{2})));
    auto value = storage.get("count");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(value->data), 2LL);

    auto previous = storage.get_and_delete("count");
    ASSERT_TRUE(previous.has_value());
    EXPECT_EQ(std::get<std::int64_t>(previous->data), 2LL);
    EXPECT_FALSE(storage.exists("count"));
}

TEST(KorStorage, MemoryStatsRemainConsistent) {
    kor::Storage storage(64ULL * 1024ULL * 1024ULL);
    storage.set("one", kor::Value(std::int64_t{7}));
    storage.set("two", kor::Value(std::string("hello")));

    const auto stats = storage.memory_stats();
    EXPECT_EQ(stats.records, 2ULL);
    EXPECT_GT(stats.total_bytes, 0ULL);
    EXPECT_GE(stats.limit_bytes, stats.total_bytes);
}

TEST(KorStorage, RecoversStateAfterReopen) {
    const auto path = std::filesystem::temp_directory_path() / "kor_storage_recovery.kr";
    std::filesystem::remove(path);

    {
        kor::Storage storage(64ULL * 1024ULL * 1024ULL, path);
        EXPECT_TRUE(storage.set("alpha", kor::Value(std::int64_t{9})));
        EXPECT_TRUE(storage.set("beta", kor::Value(std::string("hello"))));
        EXPECT_TRUE(storage.erase("beta"));
        EXPECT_TRUE(storage.set("gamma", kor::Value(true)));
        storage.close();
    }

    kor::Storage reopened(64ULL * 1024ULL * 1024ULL, path);
    EXPECT_EQ(reopened.count(), 2U);
    EXPECT_TRUE(reopened.exists("alpha"));
    EXPECT_TRUE(reopened.exists("gamma"));
    EXPECT_FALSE(reopened.exists("beta"));
    EXPECT_EQ(std::get<std::int64_t>(reopened.get("alpha")->data), 9LL);
    EXPECT_EQ(std::get<bool>(reopened.get("gamma")->data), true);

    reopened.close();
    std::filesystem::remove(path);
}

TEST(KorStorage, CompactionReclaimsWalAndPreservesCheckpointedIndexes) {
    const auto path = std::filesystem::temp_directory_path() / "kor_storage_compaction.kr";
    std::filesystem::remove(path);
    ASSERT_TRUE(kor::initialize_database_file(path, "compact", 64U, 60U));

    {
        kor::Storage storage(64ULL * 1024ULL * 1024ULL, path);
        storage.set("account:1", kor::Value(kor::ValueMap{
            {"email", kor::Value(std::string("one@example.test"))},
            {"score", kor::Value(std::int64_t{7})},
        }));
        storage.set("account:2", kor::Value(kor::ValueMap{
            {"email", kor::Value(std::string("two@example.test"))},
            {"score", kor::Value(std::int64_t{9})},
        }));
        storage.create_index("email_unique", "email", kor::SecondaryIndexType::kHash, true);
        storage.create_index("score_order", "score", kor::SecondaryIndexType::kOrdered, false);
        for (int iteration = 0; iteration < 80; ++iteration) {
            storage.set("temporary", kor::Value(std::string(4096U, 'x')));
        }
        storage.erase("temporary");

        const auto before = storage.stats();
        const auto compacted = storage.compact();
        const auto after_compaction = storage.stats();
        EXPECT_TRUE(compacted.checkpoint_performed || after_compaction.last_checkpoint_lsn > 0U);
        if (compacted.checkpoint_performed) {
            EXPECT_EQ(after_compaction.last_checkpoint_lsn, compacted.checkpoint_lsn);
            EXPECT_LT(after_compaction.file_size_bytes, before.file_size_bytes);
        } else {
            EXPECT_GT(after_compaction.last_checkpoint_lsn, 0U);
            EXPECT_GT(after_compaction.snapshot_bytes_retained, 0U);
        }
        EXPECT_EQ(storage.find("email", kor::Value(std::string("one@example.test"))).size(), 1U);
        EXPECT_EQ(storage.find_range("score", kor::Value(std::int64_t{7}), std::nullopt).size(), 2U);
        EXPECT_EQ(storage.count(), before.record_count);
        storage.set("after_compaction", kor::Value(true));
        storage.close();
    }

    {
        kor::Storage reopened(64ULL * 1024ULL * 1024ULL, path);
        EXPECT_EQ(reopened.count(), 3U);
        EXPECT_TRUE(reopened.exists("after_compaction"));
        EXPECT_EQ(reopened.list_indexes().size(), 2U);
        EXPECT_EQ(reopened.find("email", kor::Value(std::string("one@example.test"))).size(), 1U);
        EXPECT_GT(reopened.stats().last_checkpoint_lsn, 0U);
        reopened.close();
    }
    EXPECT_FALSE(std::filesystem::exists(path.parent_path() / "kor_storage_compaction.wal"));
    EXPECT_FALSE(std::filesystem::exists(path.parent_path() / "kor_storage_compaction.tmp"));
    std::filesystem::remove(path);
}

TEST(KorStorage, AutomaticallyCheckpointsDirtyPersistentStorage) {
    const auto path = std::filesystem::temp_directory_path() / "kor_storage_auto_checkpoint.kr";
    std::filesystem::remove(path);
    ASSERT_TRUE(kor::initialize_database_file(path, "auto_checkpoint", 64U, 60U));

    {
        kor::Storage storage(64ULL * 1024ULL * 1024ULL, path);
        const auto value = kor::Value(std::string(4096U, 'x'));
        for (std::size_t index = 0; index < 24U; ++index) {
            storage.set("latest", value);
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (storage.stats().last_checkpoint_lsn == 0U &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_GT(storage.stats().last_checkpoint_lsn, 0U);
        EXPECT_TRUE(storage.stats().last_auto_snapshot_error.empty());
        storage.close();
    }

    kor::Storage reopened(64ULL * 1024ULL * 1024ULL, path);
    ASSERT_TRUE(reopened.get("latest").has_value());
    EXPECT_EQ(std::get<std::string>(reopened.get("latest")->data).size(), 4096U);
    reopened.close();
    std::filesystem::remove(path);
}

TEST(KorStorage, AutomaticCheckpointStatusReportsInjectedFailureAndRecovery) {
    const auto path = std::filesystem::temp_directory_path() / "kor_storage_checkpoint_status.kr";
    std::filesystem::remove(path);
    ASSERT_TRUE(kor::initialize_database_file(path, "checkpoint_status", 64U, 60U));

    {
        kor::Storage storage(64ULL * 1024ULL * 1024ULL, path);
        storage.fail_next_auto_checkpoint_for_testing();
        storage.set("first", kor::Value(std::string(4096U, 'x')));
        const auto failure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto status = storage.checkpoint_status();
        while (status.failure_count == 0U &&
               std::chrono::steady_clock::now() < failure_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            status = storage.checkpoint_status();
        }
        EXPECT_EQ(status.failure_count, 1U);
        EXPECT_NE(status.last_error.find("Injected automatic checkpoint failure."),
                  std::string::npos);
        EXPECT_GT(status.last_attempt_duration_ns, 0U);

        for (int iteration = 0; iteration < 20; ++iteration) {
            storage.set("second", kor::Value(std::string(4096U, 'y')));
        }
        const auto success_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        status = storage.checkpoint_status();
        while ((status.success_count == 0U || status.in_progress || !status.last_error.empty()) &&
               std::chrono::steady_clock::now() < success_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            status = storage.checkpoint_status();
        }
        EXPECT_GE(status.success_count, 1U);
        EXPECT_EQ(status.last_error, "");
        EXPECT_FALSE(status.in_progress);
        storage.close();
    }

    {
        kor::Storage reopened(64ULL * 1024ULL * 1024ULL, path);
        EXPECT_TRUE(reopened.exists("first"));
        EXPECT_TRUE(reopened.exists("second"));
        reopened.close();
    }

    std::filesystem::remove(path);
}

TEST(KorStorage, ScanReturnsSortedBoundedPages) {
    kor::Storage storage(1024ULL * 1024ULL);
    storage.set("item:03", kor::Value(std::string("three")));
    storage.set("item:01", kor::Value(std::string("one")));
    storage.set("other:01", kor::Value(std::string("other")));
    storage.set("item:02", kor::Value(std::string("two")));

    const auto first = storage.scan("item:", std::nullopt, 2U);
    ASSERT_EQ(first.records.size(), 2U);
    EXPECT_EQ(first.records[0].first, "item:01");
    EXPECT_EQ(first.records[1].first, "item:02");
    ASSERT_TRUE(first.next_cursor.has_value());
    EXPECT_EQ(*first.next_cursor, "item:02");
    EXPECT_TRUE(first.has_more);

    const auto second = storage.scan("item:", first.next_cursor, 2U);
    ASSERT_EQ(second.records.size(), 1U);
    EXPECT_EQ(second.records[0].first, "item:03");
    EXPECT_FALSE(second.has_more);
    EXPECT_FALSE(second.next_cursor.has_value());
}

TEST(KorStorage, MultiKeyReadsPreserveOrderAndMissingSlots) {
    kor::Storage storage(1024ULL * 1024ULL);
    storage.set("first", kor::Value(std::int64_t{1}));
    storage.set("second", kor::Value(std::int64_t{2}));

    const auto values = storage.get_many({"second", "missing", "first"});
    ASSERT_EQ(values.size(), 3U);
    ASSERT_TRUE(values[0].has_value());
    EXPECT_EQ(std::get<std::int64_t>(values[0]->data), 2);
    EXPECT_FALSE(values[1].has_value());
    ASSERT_TRUE(values[2].has_value());
    EXPECT_EQ(std::get<std::int64_t>(values[2]->data), 1);
    EXPECT_EQ(storage.contains_many({"first", "missing", "second"}),
              (std::vector<bool>{true, false, true}));
    EXPECT_THROW(storage.get_many(std::vector<std::string>(10'001U, "key")),
                 kor::InvalidConfigurationError);
}

TEST(KorStorage, BatchWritesAreAtomicAndRespectIndexes) {
    kor::Storage storage(1024ULL * 1024ULL);
    storage.set("existing", kor::Value(kor::ValueMap{
        {"email", kor::Value(std::string("used@example.test"))},
    }));
    storage.create_index("unique_email", "email", kor::SecondaryIndexType::kHash, true);

    const std::vector<std::pair<std::string, kor::Value>> conflict{
        {"first", kor::Value(kor::ValueMap{{"email", kor::Value(std::string("first@example.test"))}})},
        {"conflict", kor::Value(kor::ValueMap{{"email", kor::Value(std::string("used@example.test"))}})},
    };
    EXPECT_THROW(storage.set_many(conflict), kor::UniqueConstraintError);
    EXPECT_FALSE(storage.exists("first"));
    EXPECT_FALSE(storage.exists("conflict"));

    storage.set_many({{"first", kor::Value(std::string("one"))},
                      {"second", kor::Value(std::string("two"))}});
    storage.delete_many({"first", "missing"});
    EXPECT_FALSE(storage.exists("first"));
    EXPECT_TRUE(storage.exists("second"));
}

TEST(KorStorage, ExplicitCheckpointDoesNotRequirePhysicalShrinking) {
    const auto path = std::filesystem::temp_directory_path() / "kor_storage_checkpoint.kr";
    std::filesystem::remove(path);
    ASSERT_TRUE(kor::initialize_database_file(path, "checkpoint", 64U, 60U));

    {
        kor::Storage storage(64ULL * 1024ULL * 1024ULL, path);
        storage.set("alpha", kor::Value(std::int64_t{1}));
        const auto checkpoint = storage.checkpoint();
        const auto checkpoint_stats = storage.stats();
        EXPECT_TRUE(checkpoint.checkpoint_performed || checkpoint_stats.last_checkpoint_lsn > 0U);
        if (checkpoint.checkpoint_performed) {
            EXPECT_FALSE(checkpoint.file_physically_shrunk);
            EXPECT_GT(checkpoint.file_size_after_bytes, checkpoint.file_size_before_bytes);
        }
        EXPECT_GT(checkpoint_stats.last_checkpoint_lsn, 0U);
        storage.close();
    }

    kor::Storage reopened(64ULL * 1024ULL * 1024ULL, path);
    EXPECT_EQ(std::get<std::int64_t>(reopened.get("alpha")->data), 1);
    reopened.close();
    std::filesystem::remove(path);
}

TEST(KorStorage, ReadOnlyStorageRejectsMutationsWithoutChangingFile) {
    const auto path = std::filesystem::temp_directory_path() / "kor_storage_read_only.kr";
    std::filesystem::remove(path);
    ASSERT_TRUE(kor::initialize_database_file(path, "read_only", 64U, 60U));
    {
        kor::Storage writer(64ULL * 1024ULL * 1024ULL, path);
        writer.set("saved", kor::Value(std::string("value")));
        writer.close();
    }
    std::ifstream before_stream(path, std::ios::binary);
    const std::vector<char> before{std::istreambuf_iterator<char>(before_stream),
                                   std::istreambuf_iterator<char>()};
    before_stream.close();

    {
        kor::Storage reader(64ULL * 1024ULL * 1024ULL, path, true);
        EXPECT_TRUE(reader.is_read_only());
        EXPECT_TRUE(reader.exists("saved"));
        EXPECT_THROW(reader.set("new", kor::Value(std::string("value"))), kor::ReadOnlyError);
        EXPECT_THROW(reader.erase("missing"), kor::ReadOnlyError);
        EXPECT_THROW(reader.clear(), kor::ReadOnlyError);
        EXPECT_THROW(reader.transaction(), kor::ReadOnlyError);
        EXPECT_THROW(reader.checkpoint(), kor::ReadOnlyError);
        reader.close();
    }

    std::ifstream after_stream(path, std::ios::binary);
    const std::vector<char> after{std::istreambuf_iterator<char>(after_stream),
                                  std::istreambuf_iterator<char>()};
    after_stream.close();
    EXPECT_EQ(after, before);
    std::filesystem::remove(path);
}

TEST(KorTransaction, CommitsOrderedWritesAtomicallyAndKeepsAccounting) {
    kor::Storage storage(1024ULL * 1024ULL);
    storage.set("keep", kor::Value(std::int64_t{1}));

    auto transaction = storage.transaction();
    transaction->set("created", kor::Value(std::string("temporary")));
    EXPECT_TRUE(transaction->contains("created"));
    transaction->erase("created");
    EXPECT_FALSE(transaction->contains("created"));
    transaction->set("left", kor::Value(std::int64_t{2}));
    transaction->set("right", kor::Value(std::int64_t{3}));
    EXPECT_EQ(std::get<std::int64_t>(transaction->get("left")->data), 2LL);
    transaction->commit();

    EXPECT_FALSE(storage.exists("created"));
    EXPECT_TRUE(storage.exists("keep"));
    EXPECT_TRUE(storage.exists("left"));
    EXPECT_TRUE(storage.exists("right"));
    EXPECT_EQ(storage.memory_stats().records, 3ULL);
    EXPECT_THROW(transaction->commit(), kor::TransactionStateError);
    EXPECT_THROW(transaction->set("late", kor::Value(true)), kor::TransactionStateError);
}

TEST(KorTransaction, ClearThenSetPublishesOnlyTheNewState) {
    kor::Storage storage(1024ULL * 1024ULL);
    storage.set("old", kor::Value(std::int64_t{1}));

    auto transaction = storage.transaction();
    transaction->clear();
    EXPECT_FALSE(transaction->contains("old"));
    transaction->set("new", kor::Value(std::int64_t{2}));
    transaction->commit();

    EXPECT_FALSE(storage.exists("old"));
    ASSERT_TRUE(storage.get("new").has_value());
    EXPECT_EQ(std::get<std::int64_t>(storage.get("new")->data), 2LL);
    EXPECT_EQ(storage.count(), 1U);
}

TEST(KorTransaction, RollbackAndLifecycleSemantics) {
    kor::Storage storage(1024ULL * 1024ULL);
    storage.set("key", kor::Value(std::int64_t{1}));

    auto transaction = storage.transaction();
    transaction->set("key", kor::Value(std::int64_t{2}));
    transaction->rollback();
    transaction->rollback();
    EXPECT_EQ(std::get<std::int64_t>(storage.get("key")->data), 1LL);
    EXPECT_THROW(transaction->commit(), kor::TransactionStateError);
    EXPECT_THROW(transaction->set("key", kor::Value(std::int64_t{3})), kor::TransactionStateError);

    auto empty = storage.transaction();
    empty->commit();
    EXPECT_TRUE(empty->committed());

    auto committed = storage.transaction();
    committed->commit();
    EXPECT_THROW(committed->rollback(), kor::TransactionStateError);
}

TEST(KorTransaction, PersistsCommittedTransactionAndIgnoresAbandonedWriteSet) {
    const auto path = std::filesystem::temp_directory_path() / "kor_transaction_recovery.kr";
    std::filesystem::remove(path);

    {
        kor::Storage storage(1024ULL * 1024ULL, path);
        storage.set("base", kor::Value(std::int64_t{5}));
        {
            auto abandoned = storage.transaction();
            abandoned->set("uncommitted", kor::Value(true));
        }
        auto transaction = storage.transaction();
        transaction->set("one", kor::Value(std::string("alpha")));
        transaction->set("two", kor::Value(std::int64_t{2}));
        transaction->commit();
        storage.close();
    }

    {
        kor::Storage reopened(1024ULL * 1024ULL, path);
        EXPECT_TRUE(reopened.exists("base"));
        EXPECT_FALSE(reopened.exists("uncommitted"));
        EXPECT_EQ(std::get<std::string>(reopened.get("one")->data), "alpha");
        EXPECT_EQ(std::get<std::int64_t>(reopened.get("two")->data), 2LL);
        EXPECT_EQ(reopened.memory_stats().records, 3ULL);
        reopened.close();
    }

    std::filesystem::remove(path);
}

TEST(KorTransaction, PersistsValuesAtTheMaximumSupportedNestingDepth) {
    const auto path = std::filesystem::temp_directory_path() / "kor_transaction_deep_value.kr";
    std::filesystem::remove(path);

    kor::Value value(std::string("deep"));
    for (std::size_t depth = 0; depth < kor::kMaxValueNestingDepth; ++depth) {
        value = kor::Value(kor::ValueList{std::move(value)});
    }
    kor::validate_value(value);

    {
        kor::Storage storage(4ULL * 1024ULL * 1024ULL, path);
        auto transaction = storage.transaction();
        transaction->set("deep", value);
        transaction->commit();
        storage.close();
    }

    {
        kor::Storage reopened(4ULL * 1024ULL * 1024ULL, path);
        auto recovered = reopened.get("deep");
        ASSERT_TRUE(recovered.has_value());
        for (std::size_t depth = 0; depth < kor::kMaxValueNestingDepth; ++depth) {
            ASSERT_TRUE(recovered->is_list());
            const auto& list = std::get<kor::ValueList>(recovered->data);
            ASSERT_EQ(list.size(), 1U);
            recovered = list.front();
        }
        ASSERT_TRUE(recovered->is_string());
        EXPECT_EQ(std::get<std::string>(recovered->data), "deep");
        reopened.close();
    }

    std::filesystem::remove(path);
}

TEST(KorTransaction, EmbedsWalAndRecoveryInsideTheDatabaseFile) {
    const auto directory = std::filesystem::temp_directory_path() / "kor_transaction_embedded";
    const auto path = directory / "store.kr";
    std::filesystem::remove_all(directory);
    ASSERT_TRUE(kor::initialize_database_file(path, "store", 1ULL, 60U));

    {
        kor::Storage storage(1024ULL * 1024ULL, path);
        auto transaction = storage.transaction();
        transaction->set("first", kor::Value(std::int64_t{1}));
        transaction->set("second", kor::Value(std::int64_t{2}));
        transaction->commit();
        storage.close();
    }

    ASSERT_TRUE(kor::database_file_is_valid(path));
    {
        kor::Storage reopened(1024ULL * 1024ULL, path);
        EXPECT_EQ(std::get<std::int64_t>(reopened.get("first")->data), 1LL);
        EXPECT_EQ(std::get<std::int64_t>(reopened.get("second")->data), 2LL);
        reopened.close();
    }

    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                            std::filesystem::directory_iterator{}),
              1);
    std::filesystem::remove_all(directory);
}

TEST(KorTransaction, ReadersNeverObservePartiallyPublishedState) {
    kor::Storage storage(1024ULL * 1024ULL);
    storage.set("old", kor::Value(true));
    auto transaction = storage.transaction();
    transaction->clear();
    for (std::size_t index = 0; index < 500U; ++index) {
        transaction->set("key:" + std::to_string(index), kor::Value(static_cast<std::uint64_t>(index)));
    }

    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> saw_partial{false};
    std::thread reader([&] {
        started.store(true, std::memory_order_release);
        while (!finished.load(std::memory_order_acquire)) {
            const auto count = storage.count();
            if (count != 1U && count != 500U) {
                saw_partial.store(true, std::memory_order_release);
            }
        }
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    transaction->commit();
    finished.store(true, std::memory_order_release);
    reader.join();

    EXPECT_FALSE(saw_partial.load(std::memory_order_acquire));
    EXPECT_EQ(storage.count(), 500U);
}

TEST(KorTransaction, CorruptionInsideCommittedRecordFailsRecovery) {
    const auto path = std::filesystem::temp_directory_path() / "kor_transaction_corrupt.kr";
    std::filesystem::remove(path);
    {
        kor::Storage storage(1024ULL * 1024ULL, path);
        storage.set("committed", kor::Value(std::int64_t{1}));
        storage.close();
    }

    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.seekg(-1, std::ios::end);
        char byte = 0;
        file.read(&byte, 1);
        ASSERT_EQ(file.gcount(), 1);
        byte ^= 0x01;
        file.seekp(-1, std::ios::end);
        file.write(&byte, 1);
        file.flush();
    }

    EXPECT_THROW((kor::Storage(1024ULL * 1024ULL, path)), kor::RecoveryError);
    std::filesystem::remove(path);
}
