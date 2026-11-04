#pragma once

#include <cstddef>
#include <cstdint>

#include "kor/value.hpp"

namespace kor {

struct Record {
    Value value;
    std::uint64_t revision = 0;
    std::uint64_t estimated_memory = 0;
    bool valid = true;

    Record();
    explicit Record(Value value_, std::uint64_t estimated_memory_ = 0);

    std::uint64_t memory_usage() const noexcept;
};

}  // namespace kor
