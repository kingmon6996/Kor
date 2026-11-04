#include <gtest/gtest.h>

#include "kor/engine.hpp"
#include "kor/exceptions.hpp"

TEST(KorEngineLifecycle, StartsAndStops) {
    kor::Engine engine;
    EXPECT_FALSE(engine.is_started());

    engine.start();
    EXPECT_TRUE(engine.is_started());

    engine.start();
    EXPECT_TRUE(engine.is_started());

    engine.close();
    EXPECT_FALSE(engine.is_started());

    engine.close();
    EXPECT_FALSE(engine.is_started());
}

TEST(KorEngineLifecycle, MoveSemanticsDoNotDuplicateOwnership) {
    kor::Engine source;
    source.start();

    kor::Engine moved(std::move(source));
    EXPECT_FALSE(source.is_started());
    EXPECT_TRUE(moved.is_started());

    moved.close();
    EXPECT_FALSE(moved.is_started());
}

TEST(KorEngineLifecycle, ExceptionsRemainInHierarchy) {
    try {
        throw kor::InitializationError("boot failed");
    } catch (const kor::InitializationError& error) {
        EXPECT_STREQ(error.what(), "boot failed");
    } catch (...) {
        FAIL() << "InitializationError was not caught as the expected exception type.";
    }

    try {
        throw kor::InitializationError("boot failed");
    } catch (const kor::Error& error) {
        EXPECT_STREQ(error.what(), "boot failed");
    } catch (...) {
        FAIL() << "InitializationError was not caught as a kor::Error.";
    }
}
