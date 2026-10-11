#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>

// Triangle state shared by libScePad and libSceAudioIn through process-local shared memory; the two PRXs do not link to each other.
namespace BlowInput {

struct Flags {
    std::uint32_t active;
};

inline Flags* Shared() {
    static Flags* const flags = [] {
        const std::wstring name = L"Local\\SoS-BlowInput-" + std::to_wstring(GetCurrentProcessId());
        const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Flags), name.c_str());
        return mapping != nullptr ? static_cast<Flags*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Flags))) : nullptr;
    }();
    return flags;
}

inline void SetActive(bool active) {
    if (Flags* const flags = Shared()) std::atomic_ref(flags->active).store(active ? 1 : 0);
}

inline bool IsActive() {
    Flags* const flags = Shared();
    return flags != nullptr && std::atomic_ref(flags->active).load() != 0;
}

}
