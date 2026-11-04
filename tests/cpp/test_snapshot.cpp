#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>

#include "kor/exceptions.hpp"
#include "kor/snapshot.hpp"

TEST(KorSnapshot, SerializesAndDeserializesRecords) {
    const std::map<std::string, kor::Value> records = {
        {"answer", kor::Value(static_cast<std::int64_t>(42))},
        {"enabled", kor::Value(true)},
        {"message", kor::Value(std::string("hello"))},
    };

    const auto encoded = kor::serialize_snapshot(7ULL, 99ULL, records);
    const auto decoded = kor::deserialize_snapshot(encoded);

    EXPECT_EQ(decoded, records);
}

TEST(KorSnapshot, CheckpointRoundTripsRecordsAndIndexDefinitions) {
    const std::map<std::string, kor::Value> records = {
        {"account:1", kor::Value(kor::ValueMap{{"email", kor::Value(std::string("one@example.test"))},
                                                {"score", kor::Value(std::int64_t{7})}})},
        {"account:2", kor::Value(kor::ValueMap{{"email", kor::Value(std::string("two@example.test"))},
                                                {"score", kor::Value(std::int64_t{9})}})},
    };
    const std::vector<kor::CheckpointIndexDefinition> indexes = {
        {"email_unique", "email", {"email"}, 1U, true, 4U},
        {"score_order", "score", {"score"}, 2U, false, 5U},
        {"tenant_email", "tenant", {"tenant", "email"}, 2U, true, 6U},
    };

    const auto encoded = kor::serialize_checkpoint(12U, 91U, records, indexes);
    const auto decoded = kor::deserialize_checkpoint(encoded);

    EXPECT_EQ(decoded.generation, 12U);
    EXPECT_EQ(decoded.checkpoint_lsn, 91U);
    EXPECT_EQ(decoded.records, records);
    ASSERT_EQ(decoded.indexes.size(), indexes.size());
    EXPECT_EQ(decoded.indexes[0].name, indexes[0].name);
    EXPECT_EQ(decoded.indexes[0].field, indexes[0].field);
    EXPECT_EQ(decoded.indexes[0].unique, indexes[0].unique);
    EXPECT_EQ(decoded.indexes[1].type, indexes[1].type);
    EXPECT_EQ(decoded.indexes[2].fields, indexes[2].fields);
    EXPECT_EQ(kor::checkpoint_encoded_size(encoded), encoded.size());
}

TEST(KorSnapshot, PersistsToDatabaseFileAndReadsMetadata) {
    const auto path = std::filesystem::temp_directory_path() / "kor_snapshot_test.kr";
    std::filesystem::remove(path);

    const std::map<std::string, kor::Value> records = {
        {"count", kor::Value(static_cast<std::int64_t>(3))},
        {"status", kor::Value(std::string("ok"))},
    };

    EXPECT_TRUE(kor::write_snapshot_to_database_file(path, 11ULL, 21ULL, records));

    const auto restored = kor::read_snapshot_from_database_file(path);
    EXPECT_EQ(restored, records);

    const auto metadata = kor::read_snapshot_metadata(path);
    ASSERT_TRUE(metadata.has_value());
    EXPECT_EQ(metadata->generation, 11ULL);
    EXPECT_EQ(metadata->snapshot_lsn, 21ULL);
    EXPECT_EQ(metadata->record_count, records.size());

    const auto original_size = std::filesystem::file_size(path);
    EXPECT_THROW(kor::write_snapshot_to_database_file(path, 12ULL, 22ULL, records), kor::PersistenceError);
    EXPECT_EQ(std::filesystem::file_size(path), original_size);
    std::filesystem::remove(path);
}
