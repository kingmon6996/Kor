#include "kor/version.hpp"

#ifndef KOR_VERSION
#error "KOR_VERSION must be provided by the CMake build."
#endif

namespace kor {

const char* version() noexcept {
    return KOR_VERSION;
}

}  // namespace kor
