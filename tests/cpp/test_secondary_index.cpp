#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>

#include "kor/exceptions.hpp"
#include "kor/storage.hpp"
#include "kor/transaction.hpp"
#include "kor/value.hpp"

namespace {

kor::Value document(std::string email, std::int64_t age) {
    return kor::Value(kor::ValueMap{
        {"email", kor::Value(std::move(email))},
        {"age", kor::Value(age)},
    });
}

}  // namespace

TEST(KorSecondaryIndex, EqualityUsesIndexAndScanWithDeterministicKeyOrder) {
    kor::Storage storage(1024ULL * 1024ULL);
    storage.set("user:z", document("same@example.test", 30));
    storage.set("user:a", document("same@example.test", 20));
    storage.set("user:m", document("other@example.test", 25));
    storage.create_index("email_lookup", "email");

    const auto results = storage.find("email", kor::Value(std::string("same@example.test")));
    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results[0].first, "user:a");
    EXPECT_EQ(results[1].first, "user:z");
    EXPECT_EQ(storage.find("missing", kor::Value(std::string("same@example.test"))).size(), 0U);

    storage.drop_index("email_lookup");
    const auto scanned = storage.find("email", kor::Value(std::string("same@example.test")));
    ASSERT_EQ(scanned.size(), 2U);
    EXPECT_EQ(scanned[0].first, "user:a");
    EXPECT_EQ(scanned[1].first, "user:z");
}

TEST(KorSecondaryIndex, OrderedRangeHonorsBoundsAndExactTypes) {
    kor::Storage storage(1024ULL * 1024ULL);
    storage.set("c", document("c@test", 30));
    storage.set("a", document("a@test", 10));
    storage.set("b", document("b@test", 20));
    storage.set("wrong-type", kor::Value(kor::ValueMap{{"age", kor::Value(std::uint64_t{20})}}));
    storage.create_index("age_order", "age", kor::SecondaryIndexType::kOrdered);

    auto results = storage.find_range("age", kor::Value(std::int64_t{10}), kor::Value(std::int64_t{30}));
    ASSERT_EQ(results.size(), 3U);
    EXPECT_EQ(results[0].first, "a");
    EXPECT_EQ(results[1].first, "b");
    EXPECT_EQ(results[2].first, "c");

    results = storage.find_range("age", kor::Value(std::int64_t{10}), kor::Value(std::int64_t{30}),
                                 false, false);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].first, "b");
    EXPECT_TRUE(storage.find_range("age", kor::Value(std::int64_t{30}),
                                   kor::Value(std::int64_t{10})).empty());
    EXPECT_TRUE(storage.find_range("age", kor::Value(std::int64_t{20}),
                                   kor::Value(std::int64_t{20}), false, true).empty());

    results = storage.find_range("age", kor::Value(std::uint64_t{20}), std::nullopt);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].first, "wrong-type");
}

TEST(KorSecondaryIndex, MutationsAndTransactionsKeepIndexesConsistent) {
    kor::Storage storage(1024ULL * 1024ULL);
    storage.set("a", document("a@test", 10));
    storage.set("b", document("b@test", 20));
    storage.create_index("email_unique", "email", kor::SecondaryIndexType::kHash, true);

    storage.set("a", document("updated@test", 11));
    EXPECT_TRUE(storage.find("email", kor::Value(std::string("a@test"))).empty());
    ASSERT_EQ(storage.find("email", kor::Value(std::string("updated@test"))).size(), 1U);

    {
        auto transaction = storage.transaction();
        transaction->set("a", document("b@test", 12));
        transaction->set("b", document("updated@test", 22));
        transaction->commit();
    }
    const auto swapped = storage.find("email", kor::Value(std::string("updated@test")));
    ASSERT_EQ(swapped.size(), 1U);
    EXPECT_EQ(swapped[0].first, "b");
    ASSERT_EQ(storage.find("email", kor::Value(std::string("b@test"))).size(), 1U);

    EXPECT_THROW(storage.set("c", document("b@test", 1)), kor::UniqueConstraintError);
    EXPECT_FALSE(storage.exists("c"));

    storage.erase("a");
    EXPECT_TRUE(storage.find("email", kor::Value(std::string("b@test"))).empty());
    storage.clear();
    EXPECT_EQ(storage.list_indexes().size(), 1U);
    EXPECT_TRUE(storage.find("email", kor::Value(std::string("updated@test"))).empty());
    EXPECT_EQ(storage.list_indexes()[0].entry_count, 0U);
    EXPECT_GT(storage.memory_stats().index_bytes, 0U);
}

TEST(KorSecondaryIndex, RejectsUnsupportedValuesAndDuplicateUniqueDataAtomically) {
    kor::Storage storage(1024ULL * 1024ULL);
    storage.set("one", document("duplicate@test", 1));
    storage.set("two", document("duplicate@test", 2));
    EXPECT_THROW(storage.create_index("email_unique", "email", kor::SecondaryIndexType::kHash, true),
                 kor::UniqueConstraintError);
    EXPECT_TRUE(storage.list_indexes().empty());

    EXPECT_THROW(storage.create_index("bad.index", "email"), kor::InvalidIndexDefinitionError);
    EXPECT_THROW(storage.create_index("unsupported", "email", static_cast<kor::SecondaryIndexType>(99)),
                 kor::InvalidIndexDefinitionError);

    storage.create_index("email_lookup", "email");
    EXPECT_THROW(storage.set("three", kor::Value(kor::ValueMap{
                                                {"email", kor::Value(kor::ValueList{kor::Value("x")})}})),
                 kor::UnsupportedIndexValueError);
    EXPECT_FALSE(storage.exists("three"));
}

TEST(KorSecondaryIndex, FailedIndexBuildLeavesDefinitionsAndMemoryUnchanged) {
    kor::Storage storage(256U);
    storage.set("a", kor::Value(kor::ValueMap{{"tag", kor::Value(std::string("x"))}}));
    const auto memory_before = storage.memory_stats().total_bytes;

    EXPECT_THROW(storage.create_index("tag_lookup", "tag"), kor::MemoryLimitExceededError);
    EXPECT_TRUE(storage.list_indexes().empty());
    EXPECT_EQ(storage.memory_stats().total_bytes, memory_before);
}

TEST(KorSecondaryIndex, ReadersObserveTheOldOrCommittedIndexGeneration) {
    kor::Storage storage(4ULL * 1024ULL * 1024ULL);
    storage.set("old", kor::Value(kor::ValueMap{{"group", kor::Value(std::string("old"))}}));
    storage.create_index("group_lookup", "group");

    auto transaction = storage.transaction();
    transaction->clear();
    for (std::size_t index = 0; index < 200U; ++index) {
        transaction->set("new:" + std::to_string(index),
                         kor::Value(kor::ValueMap{{"group", kor::Value(std::string("new"))}}));
    }

    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> saw_partial{false};
    std::thread reader([&] {
        started.store(true, std::memory_order_release);
        while (!finished.load(std::memory_order_acquire)) {
            const auto matches = storage.find("group", kor::Value(std::string("new")));
            if (!matches.empty() && matches.size() != 200U) {
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
    EXPECT_EQ(storage.find("group", kor::Value(std::string("new"))).size(), 200U);
}

TEST(KorSecondaryIndex, DefinitionsPersistAndRebuildFromWalInTheSingleDatabaseFile) {
    const auto directory = std::filesystem::temp_directory_path() / "kor_secondary_index_recovery";
    const auto path = directory / "db.kr";
    std::filesystem::remove_all(directory);

    {
        kor::Storage storage(1024ULL * 1024ULL, path);
        storage.set("a", document("a@test", 10));
        storage.set("b", document("b@test", 20));
        storage.create_index("email_lookup", "email");
        storage.create_index("age_order", "age", kor::SecondaryIndexType::kOrdered);
        storage.set("a", document("changed@test", 15));
        storage.close();
    }

    {
        kor::Storage reopened(1024ULL * 1024ULL, path);
        const auto indexes = reopened.list_indexes();
        ASSERT_EQ(indexes.size(), 2U);
        EXPECT_EQ(indexes[0].name, "age_order");
        EXPECT_EQ(indexes[1].name, "email_lookup");
        ASSERT_EQ(reopened.find("email", kor::Value(std::string("changed@test"))).size(), 1U);
        ASSERT_EQ(reopened.find_range("age", kor::Value(std::int64_t{15}), std::nullopt).size(), 2U);
        reopened.drop_index("email_lookup");
        reopened.close();
    }

    {
        kor::Storage reopened(1024ULL * 1024ULL, path);
        ASSERT_EQ(reopened.list_indexes().size(), 1U);
        EXPECT_EQ(reopened.list_indexes()[0].name, "age_order");
        reopened.close();
    }
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                            std::filesystem::directory_iterator{}),
              1);
    std::filesystem::remove_all(directory);
}

TEST(KorSecondaryIndex, CompositeIndexesEnforceTupleUniquenessAndSurviveWalRecovery) {
    const auto directory = std::filesystem::temp_directory_path() / "kor_composite_index_recovery";
    const auto path = directory / "db.kr";
    std::filesystem::remove_all(directory);
    {
        kor::Storage storage(1024ULL * 1024ULL, path);
        storage.set("user:z", kor::Value(kor::ValueMap{
                                  {"tenant", kor::Value(std::string("acme"))},
                                  {"email", kor::Value(std::string("z@example.test"))}}));
        storage.set("user:a", kor::Value(kor::ValueMap{
                                  {"tenant", kor::Value(std::string("acme"))},
                                  {"email", kor::Value(std::string("a@example.test"))}}));
        storage.set("user:other", kor::Value(kor::ValueMap{
                                      {"tenant", kor::Value(std::string("other"))},
                                      {"email", kor::Value(std::string("a@example.test"))}}));
        storage.create_composite_index("tenant_email", {"tenant", "email"}, true);

        const auto matches = storage.find_composite(
            {"tenant", "email"},
            {kor::Value(std::string("acme")), kor::Value(std::string("a@example.test"))});
        ASSERT_EQ(matches.size(), 1U);
        EXPECT_EQ(matches[0].first, "user:a");
        const auto range = storage.find_composite_range(
            {"tenant", "email"},
            std::vector<kor::Value>{kor::Value(std::string("acme")),
                                    kor::Value(std::string("b@example.test"))},
            std::vector<kor::Value>{kor::Value(std::string("acme")),
                                    kor::Value(std::string("z@example.test"))});
        ASSERT_EQ(range.size(), 1U);
        EXPECT_EQ(range[0].first, "user:z");
        EXPECT_EQ(storage.list_indexes()[0].fields,
                  (std::vector<std::string>{"tenant", "email"}));
        EXPECT_THROW(storage.set("duplicate", kor::Value(kor::ValueMap{
                                                  {"tenant", kor::Value(std::string("acme"))},
                                                  {"email", kor::Value(std::string("a@example.test"))}})),
                     kor::UniqueConstraintError);
        EXPECT_FALSE(storage.exists("duplicate"));
        storage.close();
    }

    {
        kor::Storage reopened(1024ULL * 1024ULL, path);
        ASSERT_EQ(reopened.list_indexes().size(), 1U);
        EXPECT_EQ(reopened.list_indexes()[0].fields,
                  (std::vector<std::string>{"tenant", "email"}));
        const auto matches = reopened.find_composite(
            {"tenant", "email"},
            {kor::Value(std::string("other")), kor::Value(std::string("a@example.test"))});
        ASSERT_EQ(matches.size(), 1U);
        EXPECT_EQ(matches[0].first, "user:other");
        reopened.close();
    }
    std::filesystem::remove_all(directory);
}
