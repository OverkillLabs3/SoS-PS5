#pragma once

#include <windows.h>
#include <mutex>
#include <string>

// Serializes AvPlayer source opening while Movement Speed 2x or Jump Height is on. libSceSystemService sets the state through a
// process-local named event, so neither library links to the other.
namespace SafeTransition {

inline HANDLE Event() {
    static const HANDLE event =
        CreateEventW(nullptr, TRUE, FALSE, (L"Local\\SoS-SafeAvPlayerTransition-" + std::to_wstring(GetCurrentProcessId())).c_str());
    return event;
}

inline bool Enabled() {
    const HANDLE event = Event();
    return event != nullptr && WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

inline void Set(bool on) {
    if (const HANDLE event = Event()) on ? SetEvent(event) : ResetEvent(event);
}

inline std::mutex& AddSourceMutex() {
    static std::mutex mutex;
    return mutex;
}

template <typename Open> int AddSource(Open&& open) {
    if (!Enabled()) return open();
    std::lock_guard lock(AddSourceMutex());
    return open();
}

}
