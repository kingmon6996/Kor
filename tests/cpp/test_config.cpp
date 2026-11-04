#include <gtest/gtest.h>

#include <filesystem>

#include "kor/config.hpp"
#include "kor/exceptions.hpp"

TEST(KorConfig, AcceptsValidConfiguration) {
    const auto config = kor::make_config("database", "store", 512, 300);

    EXPECT_EQ(config.name, "store");
    EXPECT_EQ(config.max_memory_mib, 512ULL);
    EXPECT_EQ(config.snapshot_interval_seconds, 300U);
    EXPECT_EQ(std::string(kor::kDefaultPersistence), "wal");
    EXPECT_EQ(std::string(kor::kDefaultDurability), "balanced");
}

TEST(KorConfig, RejectsInvalidName) {
    EXPECT_THROW(kor::make_config("database", "store.kr", 512, 300), kor::InvalidConfigurationError);
    EXPECT_THROW(kor::make_config("database", "", 512, 300), kor::InvalidConfigurationError);
    EXPECT_THROW(kor::make_config("database", "my database", 512, 300), kor::InvalidConfigurationError);
    EXPECT_THROW(kor::make_config("database", "../store", 512, 300), kor::InvalidConfigurationError);
    EXPECT_THROW(kor::make_config("database", "CON", 512, 300), kor::InvalidConfigurationError);
}

TEST(KorConfig, RejectsInvalidMemoryAndSnapshot) {
    EXPECT_THROW(kor::make_config("database", "store", 0, 300), kor::InvalidConfigurationError);
    EXPECT_THROW(kor::make_config("database", "store", 1, 4), kor::InvalidConfigurationError);
    EXPECT_THROW(kor::make_config("database", "store", 1, 501), kor::InvalidConfigurationError);
}
