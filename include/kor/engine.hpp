#pragma once

#include <filesystem>
#include <memory>

#include "kor/storage.hpp"

namespace kor {

class Engine {
public:
    Engine();
    explicit Engine(const std::filesystem::path& database_path,
                    std::uint64_t max_memory_bytes = 64ULL * 1024ULL * 1024ULL,
                    bool read_only = false);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;

    void start();
    void close();

    bool is_started() const noexcept;
    const std::filesystem::path& path() const noexcept;
    Storage& storage();
    const Storage& storage() const;

private:
    struct State;
    std::unique_ptr<State> state_;
    std::unique_ptr<Storage> storage_;
    std::filesystem::path database_path_;
};

}  // namespace kor
