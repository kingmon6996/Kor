#include "kor/value.hpp"

#include "kor/exceptions.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace kor {
namespace {

bool is_ascii_control(unsigned char ch) {
    return ch < 0x20 || ch == 0x7F;
}

bool is_valid_utf8_codepoint(const std::string& value, std::size_t& index) {
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    if ((ch & 0x80U) == 0U) {
        ++index;
        return ch != 0;
    }

    if ((ch & 0xE0U) == 0xC0U) {
        if (index + 1 >= value.size()) {
            return false;
        }
        const unsigned char next = static_cast<unsigned char>(value[index + 1]);
        if ((next & 0xC0U) != 0x80U) {
            return false;
        }
        index += 2;
        return true;
    }

    if ((ch & 0xF0U) == 0xE0U) {
        if (index + 2 >= value.size()) {
            return false;
        }
        const unsigned char next1 = static_cast<unsigned char>(value[index + 1]);
        const unsigned char next2 = static_cast<unsigned char>(value[index + 2]);
        if ((next1 & 0xC0U) != 0x80U || (next2 & 0xC0U) != 0x80U) {
            return false;
        }
        index += 3;
        return true;
    }

    if ((ch & 0xF8U) == 0xF0U) {
        if (index + 3 >= value.size()) {
            return false;
        }
        const unsigned char next1 = static_cast<unsigned char>(value[index + 1]);
        const unsigned char next2 = static_cast<unsigned char>(value[index + 2]);
        const unsigned char next3 = static_cast<unsigned char>(value[index + 3]);
        if ((next1 & 0xC0U) != 0x80U || (next2 & 0xC0U) != 0x80U || (next3 & 0xC0U) != 0x80U) {
            return false;
        }
        index += 4;
        return true;
    }

    return false;
}

std::uint64_t estimate_list_bytes(const ValueList& list) {
    std::uint64_t total = 0;
    for (const auto& item : list) {
        total += estimate_value_bytes(item);
    }
    total += static_cast<std::uint64_t>(list.size() * sizeof(void*));
    return total;
}

std::uint64_t estimate_map_bytes(const ValueMap& map) {
    std::uint64_t total = 0;
    for (const auto& entry : map) {
        total += estimate_key_bytes(entry.first);
        total += estimate_value_bytes(entry.second);
    }
    total += static_cast<std::uint64_t>(map.size() * 32ULL);
    return total;
}

void validate_map_key(const std::string& key) {
    if (key.empty()) {
        throw InvalidValueError("Dictionary keys must not be empty.");
    }
    if (key.find('\0') != std::string::npos) {
        throw InvalidValueError("Dictionary keys must not contain null bytes.");
    }
    if (!is_valid_utf8(key)) {
        throw InvalidValueError("Dictionary keys must be valid UTF-8.");
    }
}

}  // namespace

Value::Value() : data(nullptr) {}
Value::Value(std::nullptr_t) : data(nullptr) {}
Value::Value(bool value) : data(value) {}
Value::Value(std::int64_t value) : data(value) {}
Value::Value(std::uint64_t value) : data(value) {}
Value::Value(double value) : data(value) {
    if (!std::isfinite(value)) {
        throw InvalidValueError("Floating-point values must be finite numbers.");
    }
}
Value::Value(const std::string& value) : data(value) {
    if (!is_valid_utf8(value)) {
        throw InvalidValueError("Strings must contain valid UTF-8.");
    }
    if (value.find('\0') != std::string::npos) {
        throw InvalidValueError("Strings must not contain embedded null bytes.");
    }
}
Value::Value(std::string&& value) : data(std::move(value)) {
    if (!is_valid_utf8(std::get<std::string>(data))) {
        throw InvalidValueError("Strings must contain valid UTF-8.");
    }
    if (std::get<std::string>(data).find('\0') != std::string::npos) {
        throw InvalidValueError("Strings must not contain embedded null bytes.");
    }
}
Value::Value(const Bytes& value) : data(value) {}
Value::Value(Bytes&& value) : data(std::move(value)) {}
Value::Value(const ValueList& value) : data(value) {
    validate_value(*this, 0);
}
Value::Value(ValueList&& value) : data(std::move(value)) {
    validate_value(*this, 0);
}
Value::Value(const ValueMap& value) : data(value) {
    validate_value(*this, 0);
}
Value::Value(ValueMap&& value) : data(std::move(value)) {
    validate_value(*this, 0);
}

bool Value::is_null() const noexcept {
    return std::holds_alternative<std::nullptr_t>(data);
}
bool Value::is_bool() const noexcept {
    return std::holds_alternative<bool>(data);
}
bool Value::is_int64() const noexcept {
    return std::holds_alternative<std::int64_t>(data);
}
bool Value::is_uint64() const noexcept {
    return std::holds_alternative<std::uint64_t>(data);
}
bool Value::is_double() const noexcept {
    return std::holds_alternative<double>(data);
}
bool Value::is_string() const noexcept {
    return std::holds_alternative<std::string>(data);
}
bool Value::is_bytes() const noexcept {
    return std::holds_alternative<Bytes>(data);
}
bool Value::is_list() const noexcept {
    return std::holds_alternative<ValueList>(data);
}
bool Value::is_map() const noexcept {
    return std::holds_alternative<ValueMap>(data);
}

bool Value::operator==(const Value& other) const noexcept {
    return data == other.data;
}

bool Value::operator!=(const Value& other) const noexcept {
    return !(*this == other);
}

bool is_valid_utf8(const std::string& value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        if (value[index] == '\0') {
            return false;
        }
        std::size_t next = index;
        if (!is_valid_utf8_codepoint(value, next)) {
            return false;
        }
        index = next;
    }
    return true;
}

void validate_key(const std::string& key) {
    if (key.empty()) {
        throw InvalidKeyError("Keys must not be empty.");
    }
    if (key.find('\0') != std::string::npos) {
        throw InvalidKeyError("Keys must not contain null bytes.");
    }
    if (!is_valid_utf8(key)) {
        throw InvalidKeyError("Keys must be valid UTF-8.");
    }
}

void validate_value(const Value& value, std::size_t depth) {
    if (depth > kMaxValueNestingDepth) {
        throw InvalidValueError("Value nesting exceeds the supported maximum depth.");
    }

    if (value.is_null() || value.is_bool() || value.is_int64() || value.is_uint64() || value.is_double() ||
        value.is_string() || value.is_bytes()) {
        if (value.is_double()) {
            const auto number = std::get<double>(value.data);
            if (!std::isfinite(number)) {
                throw InvalidValueError("Floating-point values must be finite.");
            }
        }
        if (value.is_string()) {
            const auto& text = std::get<std::string>(value.data);
            if (!::kor::is_valid_utf8(text)) {
                throw InvalidValueError("String values must be valid UTF-8.");
            }
            if (text.find('\0') != std::string::npos) {
                throw InvalidValueError("String values must not contain null bytes.");
            }
        }
        return;
    }

    if (value.is_list()) {
        for (const auto& item : std::get<ValueList>(value.data)) {
            validate_value(item, depth + 1U);
        }
        return;
    }

    if (value.is_map()) {
        for (const auto& entry : std::get<ValueMap>(value.data)) {
            validate_map_key(entry.first);
            validate_value(entry.second, depth + 1U);
        }
        return;
    }

    throw InvalidValueError("Unsupported value type.");
}

std::uint64_t estimate_value_bytes(const Value& value) {
    if (value.is_null()) {
        return 1ULL;
    }
    if (value.is_bool()) {
        return sizeof(bool);
    }
    if (value.is_int64()) {
        return sizeof(std::int64_t);
    }
    if (value.is_uint64()) {
        return sizeof(std::uint64_t);
    }
    if (value.is_double()) {
        return sizeof(double);
    }
    if (value.is_string()) {
        const auto& text = std::get<std::string>(value.data);
        return static_cast<std::uint64_t>(text.size()) + 1ULL;
    }
    if (value.is_bytes()) {
        const auto& bytes = std::get<Bytes>(value.data);
        return static_cast<std::uint64_t>(bytes.size());
    }
    if (value.is_list()) {
        return estimate_list_bytes(std::get<ValueList>(value.data));
    }
    if (value.is_map()) {
        return estimate_map_bytes(std::get<ValueMap>(value.data));
    }
    return 0ULL;
}

std::uint64_t estimate_key_bytes(const std::string& key) {
    validate_key(key);
    return static_cast<std::uint64_t>(key.size()) + 1ULL;
}

}  // namespace kor

namespace std {

std::size_t hash<kor::Value>::operator()(const kor::Value& value) const noexcept {
    std::size_t seed = 0;
    std::visit(
        [&seed](const auto& entry) {
            using T = std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                seed ^= std::hash<std::uint8_t>{}(0);
            } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
                seed ^= std::hash<std::uintptr_t>{}(0);
            } else if constexpr (std::is_same_v<T, bool>) {
                seed ^= std::hash<bool>{}(entry) << 1;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                seed ^= std::hash<std::int64_t>{}(entry) + 0x9e3779b97f4a7c15ULL;
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                seed ^= std::hash<std::uint64_t>{}(entry) + 0x9e3779b97f4a7c15ULL;
            } else if constexpr (std::is_same_v<T, double>) {
                const auto bits = std::bit_cast<std::uint64_t>(entry);
                seed ^= std::hash<std::uint64_t>{}(bits);
            } else if constexpr (std::is_same_v<T, std::string>) {
                seed ^= std::hash<std::string>{}(entry) + 0x9e3779b97f4a7c15ULL;
            } else if constexpr (std::is_same_v<T, kor::Bytes>) {
                for (auto byte : entry) {
                    seed ^= std::hash<std::uint8_t>{}(byte) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                }
            } else if constexpr (std::is_same_v<T, kor::ValueList>) {
                for (const auto& item : entry) {
                    seed ^= std::hash<kor::Value>{}(item) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                }
            } else if constexpr (std::is_same_v<T, kor::ValueMap>) {
                for (const auto& item : entry) {
                    seed ^= std::hash<std::string>{}(item.first) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                    seed ^= std::hash<kor::Value>{}(item.second) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                }
            }
        },
        value.data);
    return seed;
}

}  // namespace std
