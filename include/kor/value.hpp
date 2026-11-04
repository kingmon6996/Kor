#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace kor {

using Bytes = std::vector<std::uint8_t>;
struct Value;
using ValueList = std::vector<Value>;
using ValueMap = std::map<std::string, Value>;

constexpr std::size_t kMaxValueNestingDepth = 128;

struct Value {
    using StorageType = std::variant<std::monostate,
                                     std::nullptr_t,
                                     bool,
                                     std::int64_t,
                                     std::uint64_t,
                                     double,
                                     std::string,
                                     Bytes,
                                     ValueList,
                                     ValueMap>;

    StorageType data;

    Value();
    Value(std::nullptr_t);
    Value(bool);
    Value(std::int64_t);
    Value(std::uint64_t);
    Value(double);
    Value(const std::string&);
    Value(std::string&&);
    Value(const Bytes&);
    Value(Bytes&&);
    Value(const ValueList&);
    Value(ValueList&&);
    Value(const ValueMap&);
    Value(ValueMap&&);

    bool is_null() const noexcept;
    bool is_bool() const noexcept;
    bool is_int64() const noexcept;
    bool is_uint64() const noexcept;
    bool is_double() const noexcept;
    bool is_string() const noexcept;
    bool is_bytes() const noexcept;
    bool is_list() const noexcept;
    bool is_map() const noexcept;

    bool operator==(const Value& other) const noexcept;
    bool operator!=(const Value& other) const noexcept;
};

bool is_valid_utf8(const std::string& value) noexcept;
void validate_key(const std::string& key);
void validate_value(const Value& value, std::size_t depth = 0);
std::uint64_t estimate_value_bytes(const Value& value);
std::uint64_t estimate_key_bytes(const std::string& key);

}  // namespace kor

namespace std {

template <>
struct hash<kor::Value> {
    std::size_t operator()(const kor::Value& value) const noexcept;
};

}  // namespace std
