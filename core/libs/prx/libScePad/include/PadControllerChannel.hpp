#ifndef CORE_LIBS_PRX_LIBSCEPAD_PADCONTROLLERCHANNEL_HPP
#define CORE_LIBS_PRX_LIBSCEPAD_PADCONTROLLERCHANNEL_HPP

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include "prx/libScePad/include/PadControllerKind.hpp"

// The physical controller kind is written by libScePad under its state lock and read by libSceSystemService hooks on the game thread,
// through process-local shared memory so neither library links to the other.
namespace PadControllerChannel {

struct Shared {
    std::uint32_t kind;
};

inline Shared* View() {
    static Shared* const view = [] {
        const std::wstring name = L"Local\\SoS-PadControllerKind-" + std::to_wstring(GetCurrentProcessId());
        const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name.c_str());
        return mapping != nullptr ? static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
    }();
    return view;
}

inline void Publish(PadControllerKind kind) {
    if (Shared* shared = View()) std::atomic_ref(shared->kind).store(static_cast<std::uint32_t>(kind), std::memory_order_release);
}

inline PadControllerKind Current() {
    Shared* const shared = View();
    return shared != nullptr ? static_cast<PadControllerKind>(std::atomic_ref(shared->kind).load(std::memory_order_acquire)) : PadControllerKind::None;
}

}

#endif
