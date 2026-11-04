#include <gtest/gtest.h>

#include "kor/version.hpp"

TEST(KorVersion, ReturnsStableNonEmptyString) {
    const char* version = kor::version();
    EXPECT_NE(version, nullptr);
    EXPECT_STRNE(version, "");
    EXPECT_STREQ(version, "0.1.0");
}
