#include "kor/record.hpp"

namespace kor {

Record::Record() : value(nullptr), revision(0), estimated_memory(0), valid(true) {}

Record::Record(Value value_, std::uint64_t estimated_memory_)
    : value(std::move(value_)), revision(0), estimated_memory(estimated_memory_), valid(true) {}

std::uint64_t Record::memory_usage() const noexcept {
    return estimated_memory;
}

}  // namespace kor
