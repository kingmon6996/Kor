include(FetchContent)
include(GoogleTest)

if(NOT TARGET GTest::gtest_main)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(kor_cpp_tests
    cpp/test_version.cpp
    cpp/test_engine.cpp
    cpp/test_config.cpp
    cpp/test_storage.cpp
    cpp/test_secondary_index.cpp
    cpp/test_wal.cpp
    cpp/test_snapshot.cpp
)

target_link_libraries(kor_cpp_tests PRIVATE GTest::gtest_main kor_engine)

gtest_discover_tests(kor_cpp_tests)
