cmake_minimum_required(VERSION 3.22)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/python/kor/_version.py"
    KOR_VERSION_SOURCE
    REGEX "^__version__ = \"[0-9]+\\.[0-9]+\\.[0-9]+\"$"
)
if(NOT KOR_VERSION_SOURCE)
    message(FATAL_ERROR
        "Unable to read the package version from python/kor/_version.py."
    )
endif()
string(REGEX REPLACE "^__version__ = \"([^\"]+)\"$" "\\1"
    KOR_PACKAGE_VERSION "${KOR_VERSION_SOURCE}"
)

project(Kor VERSION ${KOR_PACKAGE_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(CTest)

option(KOR_BUILD_PYTHON_EXTENSION "Build the Python extension" OFF)
if(SKBUILD OR DEFINED ENV{KOR_BUILD_PYTHON_EXTENSION})
    set(KOR_BUILD_PYTHON_EXTENSION ON CACHE BOOL "Build the Python extension" FORCE)
endif()

add_library(kor_engine STATIC
    src/version.cpp
    src/engine.cpp
    src/exceptions.cpp
    src/config.cpp
    src/value.cpp
    src/record.cpp
    src/hash_index.cpp
    src/secondary_index.cpp
    src/memory_tracker.cpp
    src/serializer.cpp
    src/storage.cpp
    src/database_file.cpp
    src/wal.cpp
    src/snapshot.cpp
    src/transaction.cpp
    src/observability.cpp
)

target_include_directories(kor_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(kor_engine PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(kor_engine PRIVATE /W4 /wd4514)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(kor_engine PRIVATE -Wall -Wextra -Wpedantic)
else()
    message(FATAL_ERROR
        "Unsupported C++ compiler '${CMAKE_CXX_COMPILER_ID}'. "
        "Kor requires MSVC, GCC, or Clang with C++20 support."
    )
endif()

target_compile_definitions(kor_engine PRIVATE "KOR_VERSION=\"${PROJECT_VERSION}\"")
if(BUILD_TESTING)
    target_compile_definitions(kor_engine PUBLIC KOR_ENABLE_TEST_HOOKS)
endif()

if(KOR_BUILD_PYTHON_EXTENSION)
    find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)

    execute_process(
        COMMAND "${Python_EXECUTABLE}" -c "import pybind11; print(pybind11.get_cmake_dir())"
        RESULT_VARIABLE PYBIND11_RESULT
        OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(NOT PYBIND11_RESULT EQUAL 0 OR "${PYBIND11_CMAKE_DIR}" STREQUAL "")
        message(FATAL_ERROR
            "pybind11 is required to build the Python extension. Install it in the active Python environment with: python -m pip install pybind11"
        )
    endif()

    list(APPEND CMAKE_PREFIX_PATH "${PYBIND11_CMAKE_DIR}")
    find_package(pybind11 CONFIG REQUIRED)

    pybind11_add_module(_kor src/bindings/module.cpp)
    target_link_libraries(_kor PRIVATE kor_engine)
    target_include_directories(_kor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_subdirectory(tests)
endif()

if(NOT SKBUILD)
    install(TARGETS kor_engine
        EXPORT KorTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(KOR_BUILD_PYTHON_EXTENSION)
    if(SKBUILD)
        file(GLOB KOR_PYTHON_PACKAGE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/python/kor/*.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/python/kor/*.pyi"
            "${CMAKE_CURRENT_SOURCE_DIR}/python/kor/py.typed"
        )
        install(FILES ${KOR_PYTHON_PACKAGE_FILES} DESTINATION kor)
    endif()
    install(TARGETS _kor
        LIBRARY DESTINATION kor
        RUNTIME DESTINATION kor
    )
endif()
