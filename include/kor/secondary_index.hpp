#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "kor/hash_index.hpp"
#include "kor/wal.hpp"

namespace kor {

enum class SecondaryIndexType : std::uint8_t {
    kHash = 1,
    kOrdered = 2,
};

struct IndexDefinition {
    std::string name;
    std::string field;
    std::vector<std::string> fields;
    SecondaryIndexType type = SecondaryIndexType::kHash;
    bool unique = false;
    std::uint64_t generation = 1;
};

struct IndexInfo {
    std::string name;
    std::string field;
    std::vector<std::string> fields;
    std::string index_type;
    bool unique = false;
    std::uint64_t generation = 1;
    std::uint64_t entry_count = 0;
};

class SecondaryIndexManager {
public:
    void create(IndexDefinition definition,
                const HashIndex& records,
    std::uint64_t maximum_staged_bytes = std::numeric_limits<std::uint64_t>::max());
    void drop(const std::string& name);
    bool contains(const std::string& name) const;
    std::vector<IndexInfo> list() const;
    void reconcile(const HashIndex& before,
                   const HashIndex& after,
                   const std::vector<WalMutation>& mutations,
                   bool clear_first);
    void apply_change(const std::string& key,
                      const std::optional<Value>& old_record,
                      const std::optional<Value>& new_record);
    void clear_entries() noexcept;
    void swap(SecondaryIndexManager& other) noexcept;
    std::uint64_t estimated_memory_bytes() const;
    std::optional<std::vector<std::string>> find_equal(const std::string& field,
                                                        const Value& value) const;
    std::optional<std::vector<std::string>> find_range(const std::string& field,
                                                        const std::optional<Value>& minimum,
                                                        const std::optional<Value>& maximum,
                                                        bool include_minimum,
                                                        bool include_maximum) const;
    std::optional<std::vector<std::string>> find_composite(
        const std::vector<std::string>& fields,
        const std::vector<Value>& values) const;
    std::optional<std::vector<std::string>> find_composite_range(
        const std::vector<std::string>& fields,
        const std::optional<std::vector<Value>>& minimum,
        const std::optional<std::vector<Value>>& maximum,
        bool include_minimum,
        bool include_maximum) const;

    static void validate_definition(const IndexDefinition& definition);
    static bool is_supported_scalar(const Value& value) noexcept;
    static void validate_range_bounds(const std::optional<Value>& minimum,
                                      const std::optional<Value>& maximum);
    static bool range_matches(const Value& value,
                              const std::optional<Value>& minimum,
                              const std::optional<Value>& maximum,
                              bool include_minimum,
                              bool include_maximum) noexcept;

private:
    struct ValueHash {
        std::size_t operator()(const Value& value) const noexcept;
    };
    struct ValueEqual {
        bool operator()(const Value& lhs, const Value& rhs) const noexcept;
    };
    struct ValueLess {
        bool operator()(const Value& lhs, const Value& rhs) const noexcept;
    };
    struct CompositeLess {
        bool operator()(const std::vector<Value>& lhs,
                        const std::vector<Value>& rhs) const noexcept;
    };
    struct SecondaryIndex {
        IndexDefinition definition;
        std::unordered_map<Value, std::set<std::string>, ValueHash, ValueEqual> hash_entries;
        std::map<Value, std::set<std::string>, ValueLess> ordered_entries;
        std::map<std::vector<Value>, std::set<std::string>, CompositeLess> composite_entries;
        std::uint64_t entry_count = 0;
    };

    static std::optional<Value> extract(const Value& record, const std::string& field);
    static std::optional<std::vector<Value>> extract_composite(
        const Value& record,
        const std::vector<std::string>& fields);
    static void add_entry(SecondaryIndex& index, const std::string& key, const Value& value);
    static void remove_entry(SecondaryIndex& index, const std::string& key, const Value& value) noexcept;
    static void insert_unique_checked(SecondaryIndex& index, const std::string& key, const Value& value);
    static void add_composite_entry(SecondaryIndex& index,
                                    const std::string& key,
                                    const std::vector<Value>& values);
    static void remove_composite_entry(SecondaryIndex& index,
                                       const std::string& key,
                                       const std::vector<Value>& values) noexcept;
    static void insert_composite_unique_checked(SecondaryIndex& index,
                                                const std::string& key,
                                                const std::vector<Value>& values);

    std::map<std::string, SecondaryIndex> indexes_;
};

}  // namespace kor
