#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

#include "kor/record.hpp"

namespace kor {

class HashIndex {
public:
    using map_type = std::unordered_map<std::string, Record>;

    bool contains(const std::string& key) const;
    std::optional<Record> get(const std::string& key) const;
    bool set(const std::string& key, const Record& record);
    void reserve(std::size_t additional_entries);
    bool erase(const std::string& key);
    std::size_t size() const noexcept;
    bool empty() const noexcept;
    void clear() noexcept;
    void swap(HashIndex& other) noexcept;
    const map_type& entries() const noexcept;

private:
    map_type entries_;
};

}  // namespace kor
