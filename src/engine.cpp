#include "kor/engine.hpp"

#include "kor/exceptions.hpp"

#include <mutex>

namespace kor {

struct Engine::State {
    mutable std::mutex mutex;
    bool started = false;
};

Engine::Engine() : state_(std::make_unique<State>()), storage_(std::make_unique<Storage>()) {}

Engine::Engine(const std::filesystem::path& database_path,
               std::uint64_t max_memory_bytes,
               bool read_only)
    : state_(std::make_unique<State>()),
      storage_(std::make_unique<Storage>(max_memory_bytes, database_path, read_only)),
      database_path_(database_path) {}

Engine::~Engine() = default;

Engine::Engine(Engine&& other) noexcept
    : state_(std::move(other.state_)),
      storage_(std::move(other.storage_)),
      database_path_(std::move(other.database_path_)) {}

Engine& Engine::operator=(Engine&& other) noexcept {
    if (this != &other) {
        state_ = std::move(other.state_);
        storage_ = std::move(other.storage_);
        database_path_ = std::move(other.database_path_);
    }
    return *this;
}

void Engine::start() {
    if (!state_) {
        throw InitializationError("Engine is in a moved-from state and cannot be started.");
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->started) {
        return;
    }
    state_->started = true;
}

void Engine::close() {
    if (!state_) {
        return;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (storage_) {
        storage_->close();
    }
    state_->started = false;
}

bool Engine::is_started() const noexcept {
    if (!state_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->started;
}

const std::filesystem::path& Engine::path() const noexcept {
    return database_path_;
}

Storage& Engine::storage() {
    if (!storage_) {
        throw InitializationError("Engine storage is unavailable.");
    }
    return *storage_;
}

const Storage& Engine::storage() const {
    if (!storage_) {
        throw InitializationError("Engine storage is unavailable.");
    }
    return *storage_;
}

}  // namespace kor
