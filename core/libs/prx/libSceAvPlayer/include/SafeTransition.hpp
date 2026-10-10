#pragma once

#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

// Serializes AvPlayer source opening while Movement Speed 2x or Jump Height is on. libSceSystemService sets the state through a
// process-local named event, so neither library links to the other.
namespace SafeTransition {

#ifdef _WIN32

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

#else

// The two libraries are separate host modules and are linked with -Bsymbolic, so a function-local static would be
// duplicated per module instead of shared. A named shared-memory byte is the POSIX equivalent of the Windows named
// event: both modules map the same object, and neither links to the other.
inline volatile unsigned char* Flag() {
    static volatile unsigned char* mapped = [] {
        const std::string name = "/SoS-SafeAvPlayerTransition-" + std::to_string(static_cast<long>(::getpid()));
        const int fd = ::shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd < 0) return static_cast<volatile unsigned char*>(nullptr);
        if (::ftruncate(fd, 1) != 0) {
            ::close(fd);
            ::shm_unlink(name.c_str());
            return static_cast<volatile unsigned char*>(nullptr);
        }
        void* const page = ::mmap(nullptr, 1, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        // The mapping survives the unlink, so the object cannot outlive the process.
        ::shm_unlink(name.c_str());
        if (page == MAP_FAILED) return static_cast<volatile unsigned char*>(nullptr);
        auto* const flag = static_cast<volatile unsigned char*>(page);
        *flag = 0;
        return flag;
    }();
    return mapped;
}

inline bool Enabled() {
    const volatile unsigned char* const flag = Flag();
    return flag != nullptr && *flag != 0;
}

inline void Set(bool on) {
    if (volatile unsigned char* const flag = Flag()) *flag = on ? 1 : 0;
}

#endif

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
