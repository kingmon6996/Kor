#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "kor/storage.hpp"
#include "kor/value.hpp"

namespace kor {

class Transaction {
public:
    explicit Transaction(Storage& storage);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) noexcept = delete;
    Transaction& operator=(Transaction&&) noexcept = delete;

    bool active() const noexcept;
    bool committed() const noexcept;
    bool rolled_back() const noexcept;

    std::optional<Value> get(const std::string& key) const;
    bool contains(const std::string& key) const;
    void set(const std::string& key, const Value& value);
    void erase(const std::string& key);
    void clear();
    void commit();
    void rollback();

private:
    friend class Storage;

    void ensure_active() const;
    void release_writer() noexcept;
    Storage* storage_ = nullptr;
    Storage::WriterLease writer_lease_;
    std::map<std::string, std::optional<Value>> writes_;
    mutable std::mutex mutex_;
    std::uint64_t write_set_bytes_ = 0;
    std::uint64_t max_write_set_bytes_ = 0;
    bool cleared_ = false;
    bool active_ = true;
    bool committing_ = false;
    bool committed_ = false;
    bool rolled_back_ = false;
    bool failed_ = false;
};

}  // namespace kor
