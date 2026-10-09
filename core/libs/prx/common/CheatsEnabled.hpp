#ifndef CORE_LIBS_PRX_COMMON_CHEATSENABLED_HPP
#define CORE_LIBS_PRX_COMMON_CHEATSENABLED_HPP

#include <cstdlib>
#include <cstring>

// The launcher's master switch for the runtime cheat system. Every library that needs it reads the same environment value once.
inline bool CheatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SOS_CHEATS_ENABLED");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

#endif
