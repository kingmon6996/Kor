#pragma once

#include <cstdint>

namespace kor {

using byte = std::uint8_t;

enum class EngineState : std::uint8_t {
    Stopped = 0,
    Started = 1,
    Closed = 2,
};

}  // namespace kor
