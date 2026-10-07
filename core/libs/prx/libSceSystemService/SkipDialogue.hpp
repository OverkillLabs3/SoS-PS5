#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

// Skip dialogue state shared by libScePad (Cross presses, input thread) and libSceSystemService (dialogue hooks, game thread) through
// process-local shared memory, so neither library links to the other.
namespace SkipDialogue {

struct Flags {
    std::uint32_t lineActive;  // a spoken line that Cross may skip
    std::uint32_t request;
};

inline Flags* Shared() {
    static Flags* const flags = [] {
        const std::wstring name = L"Local\\SoS-SkipDialogue-" + std::to_wstring(GetCurrentProcessId());
        const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Flags), name.c_str());
        return mapping != nullptr ? static_cast<Flags*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Flags))) : nullptr;
    }();
    return flags;
}

inline bool Enabled() {
    const char* value = std::getenv("SOS_SKIP_DIALOGUE");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

// A press only counts while a skippable line is active.
inline bool Request() {
    Flags* const flags = Shared();
    if (flags == nullptr || std::atomic_ref(flags->lineActive).load() == 0) return false;
    std::atomic_ref(flags->request).store(1);
    return true;
}

// A line starting or ending drops any earlier request, so a press can only skip the line it was made in.
inline void SetLineActive(bool active) {
    Flags* const flags = Shared();
    if (flags == nullptr) return;
    std::atomic_ref(flags->request).store(0);
    std::atomic_ref(flags->lineActive).store(active ? 1 : 0);
}

inline bool TakeRequest() {
    Flags* const flags = Shared();
    return flags != nullptr && std::atomic_ref(flags->request).load() != 0 && std::atomic_ref(flags->request).exchange(0) != 0;
}

}
