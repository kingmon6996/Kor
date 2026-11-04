#include "kor/secondary_index.hpp"

#include "kor/exceptions.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace kor {
namespace {

bool same_scalar_order(const Value& lhs, const Value& rhs) noexcept {
    if (lhs.data.index() != rhs.data.index()) {
        return false;
    }
    if (lhs.is_bool()) {
        return std::get<bool>(lhs.data) == std::get<bool>(rhs.data);
    }
    if (lhs.is_int64()) {
        return std::get<std::int64_t>(lhs.data) == std::get<std::int64_t>(rhs.data);
    }
    if (lhs.is_uint64()) {
        return std::get<std::uint64_t>(lhs.data) == std::get<std::uint64_t>(rhs.data);
    }
    if (lhs.is_double()) {
        return std::get<double>(lhs.data) == std::get<double>(rhs.data);
    }
    return lhs.is_string() && std::get<std::string>(lhs.data) == std::get<std::string>(rhs.data);
}

std::vector<std::string> definition_fields(const IndexDefinition& definition) {
    return definition.fields.empty() ? std::vector<std::string>{definition.field}
                                     : definition.fields;
}

int compare_scalar(const Value& lhs, const Value& rhs) noexcept {
    if (lhs.data.index() != rhs.data.index()) {
        return lhs.data.index() < rhs.data.index() ? -1 : 1;
    }
    if (lhs.is_bool()) {
        const auto a = std::get<bool>(lhs.data);
        const auto b = std::get<bool>(rhs.data);
        return a == b ? 0 : (a ? 1 : -1);
    }
    if (lhs.is_int64()) {
        const auto a = std::get<std::int64_t>(lhs.data);
        const auto b = std::get<std::int64_t>(rhs.data);
        return a == b ? 0 : (a < b ? -1 : 1);
    }
    if (lhs.is_uint64()) {
        const auto a = std::get<std::uint64_t>(lhs.data);
        const auto b = std::get<std::uint64_t>(rhs.data);
        return a == b ? 0 : (a < b ? -1 : 1);
    }
    if (lhs.is_double()) {
        const auto a = std::get<double>(lhs.data);
        const auto b = std::get<double>(rhs.data);
        return a == b ? 0 : (a < b ? -1 : 1);
    }
    const auto& a = std::get<std::string>(lhs.data);
    const auto& b = std::get<std::string>(rhs.data);
    return a == b ? 0 : (a < b ? -1 : 1);
}

Value lowest_value_of_type(const Value& value) {
    if (value.is_string()) {
        return Value(std::string());
    }
    if (value.is_bool()) {
        return Value(false);
    }
    if (value.is_int64()) {
        return Value(std::numeric_limits<std::int64_t>::lowest());
    }
    if (value.is_uint64()) {
        return Value(std::uint64_t{0});
    }
    return Value(std::numeric_limits<double>::lowest());
}

std::uint64_t add_bytes(std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
        throw MemoryLimitExceededError("Secondary-index memory accounting overflowed.");
    }
    return lhs + rhs;
}

std::size_t hash_scalar(const Value& value) noexcept {
    std::size_t seed = value.data.index();
    if (value.is_bool()) {
        return seed ^ (std::hash<bool>{}(std::get<bool>(value.data)) + 0x9e3779b9U);
    }
    if (value.is_int64()) {
        return seed ^ (std::hash<std::int64_t>{}(std::get<std::int64_t>(value.data)) + 0x9e3779b9U);
    }
    if (value.is_uint64()) {
        return seed ^ (std::hash<std::uint64_t>{}(std::get<std::uint64_t>(value.data)) + 0x9e3779b9U);
    }
    if (value.is_double()) {
        const auto number = std::get<double>(value.data);
        return seed ^ (std::hash<double>{}(number == 0.0 ? 0.0 : number) + 0x9e3779b9U);
    }
    return seed ^ (std::hash<std::string>{}(std::get<std::string>(value.data)) + 0x9e3779b9U);
}

}  // namespace

void SecondaryIndexManager::validate_definition(const IndexDefinition& definition) {
    if (definition.name.empty() || definition.name.size() > 64U ||
        !((definition.name[0] >= 'A' && definition.name[0] <= 'Z') ||
          (definition.name[0] >= 'a' && definition.name[0] <= 'z'))) {
        throw InvalidIndexDefinitionError("Index names must start with an ASCII letter and contain at most 64 characters.");
    }
    for (const char ch : definition.name) {
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
            throw InvalidIndexDefinitionError("Index names may contain only ASCII letters, digits, underscores, and hyphens.");
        }
    }
    const auto fields = definition_fields(definition);
    if (fields.empty() || fields.size() > 4U) {
        throw InvalidIndexDefinitionError("Composite indexes must contain between 2 and 4 fields.");
    }
    if (definition.field.empty() || definition.field != fields.front()) {
        throw InvalidIndexDefinitionError("The index field must match the first indexed field.");
    }
    std::set<std::string> unique_fields;
    for (const auto& field : fields) {
        if (field.empty() || field.size() > 1024U || field.find('\0') != std::string::npos ||
            !is_valid_utf8(field) || !unique_fields.insert(field).second) {
            throw InvalidIndexDefinitionError(
                "Indexed fields must be unique, non-empty valid UTF-8 top-level names of at most 1024 bytes.");
        }
    }
    if (definition.type != SecondaryIndexType::kHash && definition.type != SecondaryIndexType::kOrdered) {
        throw InvalidIndexDefinitionError("Index type must be 'hash' or 'ordered'.");
    }
    if (fields.size() > 1U && definition.type != SecondaryIndexType::kOrdered) {
        throw InvalidIndexDefinitionError("Composite indexes must use the ordered index type.");
    }
}

bool SecondaryIndexManager::is_supported_scalar(const Value& value) noexcept {
    return value.is_string() || value.is_bool() || value.is_int64() || value.is_uint64() ||
           (value.is_double() && std::isfinite(std::get<double>(value.data)));
}

void SecondaryIndexManager::create(IndexDefinition definition,
                                   const HashIndex& records,
                                   std::uint64_t maximum_staged_bytes) {
    validate_definition(definition);
    if (indexes_.contains(definition.name)) {
        throw IndexExistsError("An index with this name already exists.");
    }

    SecondaryIndex staged;
    staged.definition = std::move(definition);
    const auto fields = definition_fields(staged.definition);
    std::uint64_t definition_bytes = 128ULL + static_cast<std::uint64_t>(staged.definition.name.size());
    for (const auto& field : fields) {
        definition_bytes = add_bytes(definition_bytes, static_cast<std::uint64_t>(field.size()));
    }
    auto projected_bytes = add_bytes(estimated_memory_bytes(), definition_bytes);
    if (projected_bytes > maximum_staged_bytes) {
        throw MemoryLimitExceededError("Secondary-index staging would exceed the configured memory limit.");
    }
    for (const auto& [key, record] : records.entries()) {
        if (fields.size() > 1U) {
            const auto values = extract_composite(record.value, fields);
            if (!values.has_value()) {
                continue;
            }
            auto entry_bytes = 48ULL + static_cast<std::uint64_t>(key.size());
            for (const auto& value : *values) {
                entry_bytes = add_bytes(entry_bytes, estimate_value_bytes(value) + 16ULL);
            }
            projected_bytes = add_bytes(projected_bytes, entry_bytes);
            if (projected_bytes > maximum_staged_bytes) {
                throw MemoryLimitExceededError("Secondary-index staging would exceed the configured memory limit.");
            }
            insert_composite_unique_checked(staged, key, *values);
            continue;
        }
        const auto value = extract(record.value, fields.front());
        if (value.has_value()) {
            const bool new_value = staged.definition.type == SecondaryIndexType::kHash
                                       ? !staged.hash_entries.contains(*value)
                                       : !staged.ordered_entries.contains(*value);
            auto entry_bytes = add_bytes(48ULL, static_cast<std::uint64_t>(key.size()));
            if (new_value) {
                entry_bytes = add_bytes(entry_bytes, add_bytes(64ULL, estimate_value_bytes(*value)));
            }
            projected_bytes = add_bytes(projected_bytes, entry_bytes);
            if (projected_bytes > maximum_staged_bytes) {
                throw MemoryLimitExceededError("Secondary-index staging would exceed the configured memory limit.");
            }
            insert_unique_checked(staged, key, *value);
        }
    }
    indexes_.emplace(staged.definition.name, std::move(staged));
}

void SecondaryIndexManager::drop(const std::string& name) {
    if (indexes_.erase(name) == 0U) {
        throw IndexNotFoundError("No index with this name exists.");
    }
}

bool SecondaryIndexManager::contains(const std::string& name) const {
    return indexes_.contains(name);
}

std::vector<IndexInfo> SecondaryIndexManager::list() const {
    std::vector<IndexInfo> result;
    result.reserve(indexes_.size());
    for (const auto& [name, index] : indexes_) {
        result.push_back(IndexInfo{name,
                                   index.definition.field,
                                   definition_fields(index.definition),
                                   index.definition.type == SecondaryIndexType::kHash ? "hash" : "ordered",
                                   index.definition.unique,
                                   index.definition.generation,
                                   index.entry_count});
    }
    return result;
}

void SecondaryIndexManager::reconcile(const HashIndex& before,
                                      const HashIndex& after,
                                      const std::vector<WalMutation>& mutations,
                                      bool clear_first) {
    clear_first = clear_first ||
                  std::any_of(mutations.begin(), mutations.end(), [](const WalMutation& mutation) {
                      return mutation.operation == WalOperationType::kClear;
                  });
    std::set<std::string> changed_keys;
    for (const auto& mutation : mutations) {
        if (mutation.operation == WalOperationType::kSet || mutation.operation == WalOperationType::kDelete) {
            changed_keys.insert(mutation.key);
        }
    }

    if (clear_first) {
        for (auto& [name, index] : indexes_) {
            (void)name;
            decltype(index.hash_entries) empty_hash;
            index.hash_entries.swap(empty_hash);
            index.ordered_entries.clear();
            index.composite_entries.clear();
            index.entry_count = 0;
        }
    } else {
        for (auto& [name, index] : indexes_) {
            (void)name;
            for (const auto& key : changed_keys) {
                const auto old_record = before.get(key);
                if (old_record.has_value()) {
                    const auto fields = definition_fields(index.definition);
                    if (fields.size() > 1U) {
                        const auto old_values = extract_composite(old_record->value, fields);
                        if (old_values.has_value()) {
                            remove_composite_entry(index, key, *old_values);
                        }
                    } else {
                        const auto old_value = extract(old_record->value, fields.front());
                        if (old_value.has_value()) {
                            remove_entry(index, key, *old_value);
                        }
                    }
                }
            }
        }
    }

    for (const auto& key : changed_keys) {
        const auto new_record = after.get(key);
        if (!new_record.has_value()) {
            continue;
        }
        for (auto& [name, index] : indexes_) {
            (void)name;
            const auto fields = definition_fields(index.definition);
            if (fields.size() > 1U) {
                const auto values = extract_composite(new_record->value, fields);
                if (values.has_value()) {
                    insert_composite_unique_checked(index, key, *values);
                }
            } else {
                const auto value = extract(new_record->value, fields.front());
                if (value.has_value()) {
                    insert_unique_checked(index, key, *value);
                }
            }
        }
    }
}

void SecondaryIndexManager::apply_change(const std::string& key,
                                         const std::optional<Value>& old_record,
                                         const std::optional<Value>& new_record) {
    for (auto& [name, index] : indexes_) {
        (void)name;
        if (old_record.has_value()) {
            const auto fields = definition_fields(index.definition);
            if (fields.size() > 1U) {
                const auto values = extract_composite(*old_record, fields);
                if (values.has_value()) {
                    remove_composite_entry(index, key, *values);
                }
            } else {
                const auto value = extract(*old_record, fields.front());
                if (value.has_value()) {
                    remove_entry(index, key, *value);
                }
            }
        }
    }
    for (auto& [name, index] : indexes_) {
        (void)name;
        if (new_record.has_value()) {
            const auto fields = definition_fields(index.definition);
            if (fields.size() > 1U) {
                const auto values = extract_composite(*new_record, fields);
                if (values.has_value()) {
                    insert_composite_unique_checked(index, key, *values);
                }
            } else {
                const auto value = extract(*new_record, fields.front());
                if (value.has_value()) {
                    insert_unique_checked(index, key, *value);
                }
            }
        }
    }
}

void SecondaryIndexManager::clear_entries() noexcept {
    for (auto& [name, index] : indexes_) {
        (void)name;
        decltype(index.hash_entries) empty_hash;
        index.hash_entries.swap(empty_hash);
        index.ordered_entries.clear();
        index.composite_entries.clear();
        index.entry_count = 0;
    }
}

void SecondaryIndexManager::swap(SecondaryIndexManager& other) noexcept {
    indexes_.swap(other.indexes_);
}

std::uint64_t SecondaryIndexManager::estimated_memory_bytes() const {
    std::uint64_t total = 0;
    for (const auto& [name, index] : indexes_) {
        total = add_bytes(total, 128ULL + static_cast<std::uint64_t>(name.size()));
        for (const auto& field : definition_fields(index.definition)) {
            total = add_bytes(total, static_cast<std::uint64_t>(field.size()));
        }
        const auto add_map = [&total](const auto& entries) {
            for (const auto& [value, keys] : entries) {
                total = add_bytes(total, 64ULL + estimate_value_bytes(value));
                for (const auto& key : keys) {
                    total = add_bytes(total, 48ULL + static_cast<std::uint64_t>(key.size()));
                }
            }
        };
        add_map(index.hash_entries);
        add_map(index.ordered_entries);
        for (const auto& [values, keys] : index.composite_entries) {
            for (const auto& value : values) {
                total = add_bytes(total, 16ULL + estimate_value_bytes(value));
            }
            for (const auto& key : keys) {
                total = add_bytes(total, 48ULL + static_cast<std::uint64_t>(key.size()));
            }
        }
    }
    return total;
}

std::optional<std::vector<std::string>> SecondaryIndexManager::find_equal(const std::string& field,
                                                                           const Value& value) const {
    if (!is_supported_scalar(value)) {
        return std::vector<std::string>{};
    }
    for (const auto& [name, index] : indexes_) {
        (void)name;
        if (index.definition.field != field) {
            continue;
        }
        if (index.definition.type == SecondaryIndexType::kHash) {
            const auto found = index.hash_entries.find(value);
            return found == index.hash_entries.end()
                       ? std::optional<std::vector<std::string>>(std::vector<std::string>{})
                       : std::optional<std::vector<std::string>>(std::vector<std::string>(found->second.begin(), found->second.end()));
        }
        const auto found = index.ordered_entries.find(value);
        return found == index.ordered_entries.end()
                   ? std::optional<std::vector<std::string>>(std::vector<std::string>{})
                   : std::optional<std::vector<std::string>>(std::vector<std::string>(found->second.begin(), found->second.end()));
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> SecondaryIndexManager::find_range(
    const std::string& field,
    const std::optional<Value>& minimum,
    const std::optional<Value>& maximum,
    bool include_minimum,
    bool include_maximum) const {
    validate_range_bounds(minimum, maximum);
    if (minimum.has_value() && maximum.has_value()) {
        const auto order = compare_scalar(*minimum, *maximum);
        if (order > 0 || (order == 0 && (!include_minimum || !include_maximum))) {
            return std::vector<std::string>{};
        }
    }
    for (const auto& [name, index] : indexes_) {
        (void)name;
        if (index.definition.field != field || index.definition.type != SecondaryIndexType::kOrdered) {
            continue;
        }
        std::set<std::string> result;
        const auto& anchor = minimum.has_value() ? *minimum : *maximum;
        auto current = index.ordered_entries.begin();
        if (minimum.has_value()) {
            current = include_minimum ? index.ordered_entries.lower_bound(*minimum)
                                      : index.ordered_entries.upper_bound(*minimum);
        } else {
            current = index.ordered_entries.lower_bound(lowest_value_of_type(anchor));
        }
        auto finish = index.ordered_entries.end();
        if (maximum.has_value()) {
            finish = include_maximum ? index.ordered_entries.upper_bound(*maximum)
                                     : index.ordered_entries.lower_bound(*maximum);
        }
        for (; current != finish; ++current) {
            if (current->first.data.index() != anchor.data.index()) {
                break;
            }
            if (range_matches(current->first, minimum, maximum, include_minimum, include_maximum)) {
                result.insert(current->second.begin(), current->second.end());
            }
        }
        return std::vector<std::string>(result.begin(), result.end());
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> SecondaryIndexManager::find_composite(
    const std::vector<std::string>& fields,
    const std::vector<Value>& values) const {
    if (fields.size() < 2U || fields.size() > 4U || values.size() != fields.size() ||
        std::any_of(values.begin(), values.end(), [](const Value& value) {
            return !is_supported_scalar(value);
        })) {
        return std::vector<std::string>{};
    }
    for (const auto& [name, index] : indexes_) {
        (void)name;
        if (definition_fields(index.definition) != fields || fields.size() < 2U) {
            continue;
        }
        const auto found = index.composite_entries.find(values);
        return found == index.composite_entries.end()
                   ? std::optional<std::vector<std::string>>(std::vector<std::string>{})
                   : std::optional<std::vector<std::string>>(
                         std::vector<std::string>(found->second.begin(), found->second.end()));
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> SecondaryIndexManager::find_composite_range(
    const std::vector<std::string>& fields,
    const std::optional<std::vector<Value>>& minimum,
    const std::optional<std::vector<Value>>& maximum,
    bool include_minimum,
    bool include_maximum) const {
    if (fields.size() < 2U || fields.size() > 4U || (!minimum.has_value() && !maximum.has_value())) {
        throw InvalidIndexDefinitionError(
            "Composite range lookups require 2-to-4 fields and at least one bound.");
    }
    const auto validate_bound = [&fields](const std::optional<std::vector<Value>>& bound) {
        if (!bound.has_value()) {
            return;
        }
        if (bound->size() != fields.size()) {
            throw InvalidIndexDefinitionError("Composite range bounds must match the indexed field count.");
        }
        if (std::any_of(bound->begin(), bound->end(), [](const Value& value) {
                return !is_supported_scalar(value);
            })) {
            throw UnsupportedIndexValueError("Composite range bounds must contain supported non-null scalars.");
        }
    };
    validate_bound(minimum);
    validate_bound(maximum);
    const CompositeLess less;
    if (minimum.has_value() && maximum.has_value() &&
        (less(*maximum, *minimum) ||
         (!less(*minimum, *maximum) && (!include_minimum || !include_maximum)))) {
        return std::vector<std::string>{};
    }

    for (const auto& [name, index] : indexes_) {
        (void)name;
        if (definition_fields(index.definition) != fields || fields.size() < 2U) {
            continue;
        }
        auto current = index.composite_entries.begin();
        if (minimum.has_value()) {
            current = include_minimum ? index.composite_entries.lower_bound(*minimum)
                                      : index.composite_entries.upper_bound(*minimum);
        }
        auto finish = index.composite_entries.end();
        if (maximum.has_value()) {
            finish = include_maximum ? index.composite_entries.upper_bound(*maximum)
                                     : index.composite_entries.lower_bound(*maximum);
        }
        std::set<std::string> keys;
        for (; current != finish; ++current) {
            keys.insert(current->second.begin(), current->second.end());
        }
        return std::vector<std::string>(keys.begin(), keys.end());
    }
    return std::nullopt;
}

void SecondaryIndexManager::validate_range_bounds(const std::optional<Value>& minimum,
                                                  const std::optional<Value>& maximum) {
    if (!minimum.has_value() && !maximum.has_value()) {
        throw InvalidIndexDefinitionError("A range query requires at least one bound.");
    }
    if ((minimum.has_value() && !is_supported_scalar(*minimum)) ||
        (maximum.has_value() && !is_supported_scalar(*maximum))) {
        throw InvalidIndexDefinitionError("Range bounds must be supported scalar values.");
    }
    if (minimum.has_value() && maximum.has_value() &&
        minimum->data.index() != maximum->data.index()) {
        throw InvalidIndexDefinitionError("Range bounds must have the same exact value type.");
    }
}

bool SecondaryIndexManager::range_matches(const Value& value,
                                          const std::optional<Value>& minimum,
                                          const std::optional<Value>& maximum,
                                          bool include_minimum,
                                          bool include_maximum) noexcept {
    if (!is_supported_scalar(value)) {
        return false;
    }
    if (minimum.has_value()) {
        if (value.data.index() != minimum->data.index()) {
            return false;
        }
        const auto order = compare_scalar(value, *minimum);
        if (order < 0 || (order == 0 && !include_minimum)) {
            return false;
        }
    }
    if (maximum.has_value()) {
        if (value.data.index() != maximum->data.index()) {
            return false;
        }
        const auto order = compare_scalar(value, *maximum);
        if (order > 0 || (order == 0 && !include_maximum)) {
            return false;
        }
    }
    return true;
}

std::optional<Value> SecondaryIndexManager::extract(const Value& record, const std::string& field) {
    if (!record.is_map()) {
        return std::nullopt;
    }
    const auto& values = std::get<ValueMap>(record.data);
    const auto found = values.find(field);
    if (found == values.end() || found->second.is_null()) {
        return std::nullopt;
    }
    if (!is_supported_scalar(found->second)) {
        throw UnsupportedIndexValueError("Indexed fields may contain only strings, booleans, integers, and finite floats.");
    }
    return found->second;
}

std::optional<std::vector<Value>> SecondaryIndexManager::extract_composite(
    const Value& record,
    const std::vector<std::string>& fields) {
    if (!record.is_map()) {
        return std::nullopt;
    }
    const auto& values = std::get<ValueMap>(record.data);
    std::vector<Value> extracted;
    extracted.reserve(fields.size());
    for (const auto& field : fields) {
        const auto found = values.find(field);
        if (found == values.end() || found->second.is_null()) {
            return std::nullopt;
        }
        if (!is_supported_scalar(found->second)) {
            throw UnsupportedIndexValueError(
                "Indexed fields may contain only strings, booleans, integers, and finite floats.");
        }
        extracted.push_back(found->second);
    }
    return extracted;
}

void SecondaryIndexManager::add_entry(SecondaryIndex& index, const std::string& key, const Value& value) {
    if (index.definition.type == SecondaryIndexType::kHash) {
        index.hash_entries[value].insert(key);
    } else {
        index.ordered_entries[value].insert(key);
    }
    ++index.entry_count;
}

void SecondaryIndexManager::remove_entry(SecondaryIndex& index,
                                         const std::string& key,
                                         const Value& value) noexcept {
    if (index.definition.type == SecondaryIndexType::kHash) {
        const auto entry = index.hash_entries.find(value);
        if (entry != index.hash_entries.end() && entry->second.erase(key) != 0U) {
            --index.entry_count;
            if (entry->second.empty()) {
                index.hash_entries.erase(entry);
            }
        }
    } else {
        const auto entry = index.ordered_entries.find(value);
        if (entry != index.ordered_entries.end() && entry->second.erase(key) != 0U) {
            --index.entry_count;
            if (entry->second.empty()) {
                index.ordered_entries.erase(entry);
            }
        }
    }
}

void SecondaryIndexManager::insert_unique_checked(SecondaryIndex& index,
                                                   const std::string& key,
                                                   const Value& value) {
    if (index.definition.unique) {
        if (index.definition.type == SecondaryIndexType::kHash) {
            const auto found = index.hash_entries.find(value);
            if (found != index.hash_entries.end() && !found->second.empty() && !found->second.contains(key)) {
                throw UniqueConstraintError("A unique secondary index cannot contain duplicate values.");
            }
        } else {
            const auto found = index.ordered_entries.find(value);
            if (found != index.ordered_entries.end() && !found->second.empty() && !found->second.contains(key)) {
                throw UniqueConstraintError("A unique secondary index cannot contain duplicate values.");
            }
        }
    }
    add_entry(index, key, value);
}

void SecondaryIndexManager::add_composite_entry(SecondaryIndex& index,
                                                const std::string& key,
                                                const std::vector<Value>& values) {
    index.composite_entries[values].insert(key);
    ++index.entry_count;
}

void SecondaryIndexManager::remove_composite_entry(SecondaryIndex& index,
                                                   const std::string& key,
                                                   const std::vector<Value>& values) noexcept {
    const auto entry = index.composite_entries.find(values);
    if (entry != index.composite_entries.end() && entry->second.erase(key) != 0U) {
        --index.entry_count;
        if (entry->second.empty()) {
            index.composite_entries.erase(entry);
        }
    }
}

void SecondaryIndexManager::insert_composite_unique_checked(
    SecondaryIndex& index,
    const std::string& key,
    const std::vector<Value>& values) {
    if (index.definition.unique) {
        const auto found = index.composite_entries.find(values);
        if (found != index.composite_entries.end() && !found->second.empty() &&
            !found->second.contains(key)) {
            throw UniqueConstraintError("A unique composite index cannot contain duplicate tuples.");
        }
    }
    add_composite_entry(index, key, values);
}

std::size_t SecondaryIndexManager::ValueHash::operator()(const Value& value) const noexcept {
    return hash_scalar(value);
}

bool SecondaryIndexManager::ValueEqual::operator()(const Value& lhs, const Value& rhs) const noexcept {
    return same_scalar_order(lhs, rhs);
}

bool SecondaryIndexManager::ValueLess::operator()(const Value& lhs, const Value& rhs) const noexcept {
    return compare_scalar(lhs, rhs) < 0;
}

bool SecondaryIndexManager::CompositeLess::operator()(const std::vector<Value>& lhs,
                                                       const std::vector<Value>& rhs) const noexcept {
    const auto common = std::min(lhs.size(), rhs.size());
    for (std::size_t index = 0; index < common; ++index) {
        const auto order = compare_scalar(lhs[index], rhs[index]);
        if (order != 0) {
            return order < 0;
        }
    }
    return lhs.size() < rhs.size();
}

}  // namespace kor
