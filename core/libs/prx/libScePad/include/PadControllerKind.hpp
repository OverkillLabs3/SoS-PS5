#ifndef CORE_LIBS_PRX_LIBSCEPAD_PADCONTROLLERKIND_HPP
#define CORE_LIBS_PRX_LIBSCEPAD_PADCONTROLLERKIND_HPP

#include <cstdint>

enum class PadControllerKind : std::uint8_t {
    None,
    PlayStation5,
    PlayStation4,
    Other,
    Xbox,
};

#endif
