#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "kor/value.hpp"
#include "kor/wal.hpp"

TEST(KorWal, SerializesAndDeserializesRecords) {
    const kor::Value value = kor::Value(std::string("planet"));
    const auto encoded = kor::serialize_wal_record(17ULL, kor::WalOperationType::kSet, "user", value);
    const auto decoded = kor::deserialize_wal_record(encoded);

    EXPECT_EQ(decoded.lsn, 17ULL);
    EXPECT_EQ(decoded.operation, kor::WalOperationType::kSet);
    EXPECT_EQ(decoded.key, "user");
    EXPECT_EQ(decoded.value, value);
}

TEST(KorWal, WritesAndReadsHeaderMetadata) {
    const auto path = std::filesystem::temp_directory_path() / "kor_wal_test.kr";
    std::filesystem::remove(path);

    kor::WalLog log(path);
    EXPECT_TRUE(log.is_open());

    const auto lsn = log.append_set("alpha", kor::Value(std::int64_t{42}));
    EXPECT_EQ(lsn, 1ULL);
    EXPECT_EQ(log.next_lsn(), 2ULL);

    log.close();

    kor::WalLog reopened(path);
    EXPECT_TRUE(reopened.is_open());
    EXPECT_EQ(reopened.next_lsn(), 2ULL);

    reopened.close();
    std::filesystem::remove(path);
}

TEST(KorWal, RecoversWhenTheOlderMetadataSlotVersionIsDamaged) {
    const auto path = std::filesystem::temp_directory_path() / "kor_wal_damaged_slot.kr";
    std::filesystem::remove(path);

    {
        kor::WalLog log(path);
        log.append_set("first", kor::Value(std::int64_t{1}));
        log.append_set("second", kor::Value(std::int64_t{2}));
        log.close();
    }

    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.seekp(8, std::ios::beg);
        const char legacy_version[4] = {1, 0, 0, 0};
        file.write(legacy_version, sizeof(legacy_version));
        file.flush();
        ASSERT_TRUE(file.good());
    }

    kor::WalLog reopened(path);
    const auto records = reopened.replay();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].key, "first");
    EXPECT_EQ(records[1].key, "second");
    reopened.close();
    std::filesystem::remove(path);
}
