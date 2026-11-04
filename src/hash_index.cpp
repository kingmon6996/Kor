#include "kor/hash_index.hpp"

namespace kor {

bool HashIndex::contains(const std::string& key) const {
    return entries_.find(key) != entries_.end();
}

std::optional<Record> HashIndex::get(const std::string& key) const {
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return found->second;
}

bool HashIndex::set(const std::string& key, const Record& record) {
    entries_[key] = record;
    return true;
}

void HashIndex::reserve(std::size_t additional_entries) {
    entries_.reserve(entries_.size() + additional_entries);
}

bool HashIndex::erase(const std::string& key) {
    return entries_.erase(key) > 0;
}

std::size_t HashIndex::size() const noexcept {
    return entries_.size();
}

bool HashIndex::empty() const noexcept {
    return entries_.empty();
}

void HashIndex::clear() noexcept {
    entries_.clear();
}

void HashIndex::swap(HashIndex& other) noexcept {
    entries_.swap(other.entries_);
}

const HashIndex::map_type& HashIndex::entries() const noexcept {
    return entries_;
}

}  // namespace kor
