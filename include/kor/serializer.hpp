#pragma once

#include <cstdint>
#include <vector>

#include "kor/value.hpp"

namespace kor {

constexpr std::uint32_t kValueEncodingVersion = 1U;

std::vector<std::uint8_t> serialize_value(const Value& value);
Value deserialize_value(const std::vector<std::uint8_t>& bytes);

}  // namespace kor
