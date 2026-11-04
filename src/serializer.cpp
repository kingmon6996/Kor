#include "kor/serializer.hpp"

#include "kor/exceptions.hpp"

#include <cstring>
#include <limits>
#include <utility>

namespace kor {
namespace {

void append_u64(std::vector<std::uint8_t>& buffer, std::uint64_t value) {
    for (std::size_t idx = 0; idx < sizeof(value); ++idx) {
        const std::uint8_t byte = static_cast<std::uint8_t>((value >> (8U * (sizeof(value) - 1U - idx))) & 0xFFU);
        buffer.push_back(byte);
    }
}

std::uint64_t read_u64(const std::vector<std::uint8_t>& buffer, std::size_t& offset) {
    if (offset + sizeof(std::uint64_t) > buffer.size()) {
        throw SerializationError("Serialized value is truncated.");
    }
    std::uint64_t value = 0;
    for (std::size_t idx = 0; idx < sizeof(std::uint64_t); ++idx) {
        value = (value << 8U) | static_cast<std::uint64_t>(buffer[offset + idx]);
    }
    offset += sizeof(std::uint64_t);
    return value;
}

void append_bytes(std::vector<std::uint8_t>& buffer, const std::vector<std::uint8_t>& bytes) {
    append_u64(buffer, static_cast<std::uint64_t>(bytes.size()));
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
}

void append_string(std::vector<std::uint8_t>& buffer, const std::string& text) {
    append_u64(buffer, static_cast<std::uint64_t>(text.size()));
    buffer.insert(buffer.end(), text.begin(), text.end());
}

std::uint8_t type_tag_for(const Value& value) {
    if (value.is_null()) {
        return 0U;
    }
    if (value.is_bool()) {
        return 1U;
    }
    if (value.is_int64()) {
        return 2U;
    }
    if (value.is_uint64()) {
        return 3U;
    }
    if (value.is_double()) {
        return 4U;
    }
    if (value.is_string()) {
        return 5U;
    }
    if (value.is_bytes()) {
        return 6U;
    }
    if (value.is_list()) {
        return 7U;
    }
    if (value.is_map()) {
        return 8U;
    }
    throw InvalidValueError("Unsupported value type cannot be serialized.");
}

void serialize_value_into(std::vector<std::uint8_t>& buffer, const Value& value, std::size_t depth) {
    if (depth > kMaxValueNestingDepth) {
        throw SerializationError("Value nesting exceeds the supported maximum depth.");
    }
    validate_value(value, depth);

    buffer.push_back(type_tag_for(value));

    if (value.is_null()) {
        return;
    }
    if (value.is_bool()) {
        buffer.push_back(static_cast<std::uint8_t>(std::get<bool>(value.data) ? 1U : 0U));
        return;
    }
    if (value.is_int64()) {
        const auto number = std::get<std::int64_t>(value.data);
        append_u64(buffer, static_cast<std::uint64_t>(number));
        return;
    }
    if (value.is_uint64()) {
        append_u64(buffer, std::get<std::uint64_t>(value.data));
        return;
    }
    if (value.is_double()) {
        double number = std::get<double>(value.data);
        std::uint64_t bits = 0;
        std::memcpy(&bits, &number, sizeof(number));
        append_u64(buffer, bits);
        return;
    }
    if (value.is_string()) {
        const auto& text = std::get<std::string>(value.data);
        append_string(buffer, text);
        return;
    }
    if (value.is_bytes()) {
        const auto& bytes = std::get<Bytes>(value.data);
        append_bytes(buffer, bytes);
        return;
    }
    if (value.is_list()) {
        const auto& list = std::get<ValueList>(value.data);
        append_u64(buffer, static_cast<std::uint64_t>(list.size()));
        for (const auto& item : list) {
            serialize_value_into(buffer, item, depth + 1U);
        }
        return;
    }
    if (value.is_map()) {
        const auto& map = std::get<ValueMap>(value.data);
        append_u64(buffer, static_cast<std::uint64_t>(map.size()));
        for (const auto& entry : map) {
            append_string(buffer, entry.first);
            serialize_value_into(buffer, entry.second, depth + 1U);
        }
        return;
    }

    throw SerializationError("Unsupported value type.");
}

Value deserialize_value_from(const std::vector<std::uint8_t>& buffer, std::size_t& offset, std::size_t depth) {
    if (depth > kMaxValueNestingDepth) {
        throw SerializationError("Value nesting exceeds the supported maximum depth.");
    }
    if (offset >= buffer.size()) {
        throw SerializationError("Missing serialized value data.");
    }

    const std::uint8_t tag = buffer[offset++];
    switch (tag) {
        case 0U:
            return Value(nullptr);
        case 1U: {
            if (offset >= buffer.size()) {
                throw SerializationError("Boolean value is truncated.");
            }
            return Value(static_cast<bool>(buffer[offset++] != 0U));
        }
        case 2U: {
            const auto number = static_cast<std::int64_t>(read_u64(buffer, offset));
            return Value(number);
        }
        case 3U: {
            return Value(read_u64(buffer, offset));
        }
        case 4U: {
            std::uint64_t bits = read_u64(buffer, offset);
            double number = 0.0;
            std::memcpy(&number, &bits, sizeof(number));
            return Value(number);
        }
        case 5U: {
            const auto length = read_u64(buffer, offset);
            if (offset + length > buffer.size()) {
                throw SerializationError("String payload is truncated.");
            }
            std::string text(reinterpret_cast<const char*>(buffer.data() + offset), length);
            offset += length;
            return Value(text);
        }
        case 6U: {
            const auto length = read_u64(buffer, offset);
            if (offset + length > buffer.size()) {
                throw SerializationError("Byte payload is truncated.");
            }
            Bytes bytes(buffer.begin() + static_cast<std::ptrdiff_t>(offset),
                        buffer.begin() + static_cast<std::ptrdiff_t>(offset + length));
            offset += length;
            return Value(std::move(bytes));
        }
        case 7U: {
            const auto count = read_u64(buffer, offset);
            if (count > buffer.size() - offset) {
                throw SerializationError("List item count exceeds the available serialized data.");
            }
            ValueList items;
            items.reserve(static_cast<std::size_t>(count));
            for (std::uint64_t index = 0; index < count; ++index) {
                items.push_back(deserialize_value_from(buffer, offset, depth + 1U));
            }
            return Value(std::move(items));
        }
        case 8U: {
            const auto count = read_u64(buffer, offset);
            if (count > (buffer.size() - offset) / 9U) {
                throw SerializationError("Dictionary entry count exceeds the available serialized data.");
            }
            ValueMap map;
            for (std::uint64_t index = 0; index < count; ++index) {
                const auto key_length = read_u64(buffer, offset);
                if (key_length > buffer.size() - offset) {
                    throw SerializationError("Dictionary key is truncated.");
                }
                std::string key(reinterpret_cast<const char*>(buffer.data() + offset),
                                static_cast<std::size_t>(key_length));
                offset += static_cast<std::size_t>(key_length);
                auto value = deserialize_value_from(buffer, offset, depth + 1U);
                if (!map.emplace(std::move(key), std::move(value)).second) {
                    throw SerializationError("Dictionary contains a duplicate key.");
                }
            }
            return Value(std::move(map));
        }
        default:
            throw SerializationError("Unsupported serialized tag.");
    }
}

}  // namespace

std::vector<std::uint8_t> serialize_value(const Value& value) {
    std::vector<std::uint8_t> buffer;
    buffer.push_back(kValueEncodingVersion);
    serialize_value_into(buffer, value, 0U);
    return buffer;
}

Value deserialize_value(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        throw SerializationError("Serialized value is empty.");
    }
    std::size_t offset = 0U;
    const auto version = bytes[offset++];
    if (version != kValueEncodingVersion) {
        throw SerializationError("Unsupported value serialization version.");
    }
    const auto value = deserialize_value_from(bytes, offset, 0U);
    if (offset != bytes.size()) {
        throw SerializationError("Serialized value contains trailing bytes.");
    }
    return value;
}

}  // namespace kor
