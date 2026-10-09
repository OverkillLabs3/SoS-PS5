#include "prx/common/StderrLog.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <mutex>
#include <array>
#include <chrono>
#include <atomic>
#include <unordered_map>
#include <algorithm>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fstream>
#include <pthread.h>
#include <sstream>
#endif

namespace AgcDriver::GuestMemory {
namespace {
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("AGC driver: ") + reason);
}

unsigned long long ModuleOffset(const void* address) {
#ifdef _WIN32
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module);
    return reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module);
#else
    return reinterpret_cast<std::uintptr_t>(address);
#endif
}

#ifdef _WIN32
bool readableProtection(DWORD protection) {
    return protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool writableProtection(DWORD protection) {
    return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}
#endif

}

namespace {
std::atomic<std::uint64_t> forgetCalls{0};

std::atomic<std::uint64_t> forgetSerial{0};
std::atomic<std::uint64_t> forgetBytes{0};
std::atomic<std::uint64_t> collectMemoHits{0};
std::atomic<std::uint64_t> collectEpochBumps{0};
std::atomic<std::uint64_t> unwatchSerial{0};

std::atomic<std::uint64_t> collectDirty{0};
#ifndef _WIN32
std::atomic<std::uint64_t> collectDirtyRuns{0};
#endif
std::atomic<std::uint64_t> trackerWaits{0};
std::atomic<std::uint64_t> trackerAcquisitions{0};
std::uintptr_t PagesBase();
std::size_t PagesSize();
std::uintptr_t ImagePagesBase();
std::size_t ImagePagesSize();
}

enum MemoryCounterKind { CounterRead, CounterCompare, CounterWrite, CounterChangedWrite, CounterCollect, CounterVerify, CounterQuery, MemoryCounterCount };
constexpr const char* MemoryCounterNames[MemoryCounterCount] = {"read", "compare", "write", "write-changed", "collect", "verify", "query"};

struct MemoryCounter {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> nanoseconds{0};

    void Add(std::uint64_t moreBytes, std::uint64_t moreNanoseconds, bool shared) {
        if (shared) {
            calls.fetch_add(1, std::memory_order_relaxed);
            bytes.fetch_add(moreBytes, std::memory_order_relaxed);
            nanoseconds.fetch_add(moreNanoseconds, std::memory_order_relaxed);
            return;
        }
        calls.store(calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        bytes.store(bytes.load(std::memory_order_relaxed) + moreBytes, std::memory_order_relaxed);
        nanoseconds.store(nanoseconds.load(std::memory_order_relaxed) + moreNanoseconds, std::memory_order_relaxed);
    }
};

struct MemoryCounters {
    std::array<MemoryCounter, MemoryCounterCount> counters;

    void FoldInto(std::array<std::array<std::uint64_t, 3>, MemoryCounterCount>& totals) const {
        for (std::size_t i = 0; i < MemoryCounterCount; ++i) {
            totals[i][0] += counters[i].calls.load(std::memory_order_relaxed);
            totals[i][1] += counters[i].bytes.load(std::memory_order_relaxed);
            totals[i][2] += counters[i].nanoseconds.load(std::memory_order_relaxed);
        }
    }

    void AddFrom(const MemoryCounters& other) {
        for (std::size_t i = 0; i < MemoryCounterCount; ++i) {
            counters[i].calls.store(counters[i].calls.load(std::memory_order_relaxed) + other.counters[i].calls.load(std::memory_order_relaxed), std::memory_order_relaxed);
            counters[i].bytes.store(counters[i].bytes.load(std::memory_order_relaxed) + other.counters[i].bytes.load(std::memory_order_relaxed), std::memory_order_relaxed);
            counters[i].nanoseconds.store(counters[i].nanoseconds.load(std::memory_order_relaxed) + other.counters[i].nanoseconds.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
    }
};

struct MemoryProfile {
    std::mutex threadsMutex;
    std::vector<const MemoryCounters*> threads;
    MemoryCounters retired;
    MemoryCounters shared;
    std::atomic<std::int64_t> lastReport{0};

    std::mutex callersMutex;
    std::array<std::pair<unsigned long long, std::uint64_t>, 16> callers{};
    std::array<std::pair<unsigned long long, std::uint64_t>, 16> writeCallers{};
};

MemoryProfile& Profile() {
    static MemoryProfile& profile = *new MemoryProfile();
    return profile;
}

bool SharedMemoryCounters() {
    static const bool shared = std::getenv("APS5_SHARED_MEMORY_COUNTERS") != nullptr;
    return shared;
}

struct ThreadMemoryCounters {
    MemoryCounters counters;

    ThreadMemoryCounters() {
        auto& profile = Profile();
        std::lock_guard lock(profile.threadsMutex);
        profile.threads.push_back(&counters);
    }
    ~ThreadMemoryCounters() {
        auto& profile = Profile();
        std::lock_guard lock(profile.threadsMutex);
        std::erase(profile.threads, &counters);
        profile.retired.AddFrom(counters);
    }
};

MemoryCounter& CounterFor(MemoryCounterKind kind) {
    if (SharedMemoryCounters()) return Profile().shared.counters[kind];
    thread_local ThreadMemoryCounters thread;
    return thread.counters.counters[kind];
}

std::array<std::array<std::uint64_t, 3>, MemoryCounterCount> FoldMemoryCounters() {
    auto& profile = Profile();
    std::array<std::array<std::uint64_t, 3>, MemoryCounterCount> totals{};
    std::lock_guard lock(profile.threadsMutex);
    for (const auto* thread : profile.threads) thread->FoldInto(totals);
    profile.retired.FoldInto(totals);
    profile.shared.FoldInto(totals);
    return totals;
}

void CountCaller(std::array<std::pair<unsigned long long, std::uint64_t>, 16>& callers, std::uint32_t& sampled, std::uint32_t every, const void* returnAddress) {
    if (++sampled % every != 0) return;
    auto& state = Profile();
    const auto offset = ModuleOffset(returnAddress);
    std::lock_guard lock(state.callersMutex);
    for (auto& [caller, count] : callers) {
        if (caller == offset || count == 0) {
            caller = offset;
            count += every;
            return;
        }
    }
}

thread_local PacketTag currentPacket{NoPacket, 0xffffffffu};
thread_local ReadSite currentReadSite = ReadSite::Unknown;

std::atomic<std::uint64_t> readSiteSamples[static_cast<std::size_t>(ReadSite::Count)] = {};

void SetCurrentPacket(std::uint32_t opcode, std::uint32_t queue) {
    currentPacket = {opcode, queue};
}

PacketTag CurrentPacket() {
    return currentPacket;
}

const char* ReadSiteName(ReadSite site) {
    static const char* const names[static_cast<std::size_t>(ReadSite::Count)] = {"unknown", "capture", "dispatch-cache", "texture-compare", "texture-read", "buffer-upload", "index-buffer", "vertex-buffer", "registers", "indirect-args", "wait", "label", "scanout", "store", "mirror-refresh", "draw-cache"};
    const auto index = static_cast<std::size_t>(site);
    return index < static_cast<std::size_t>(ReadSite::Count) ? names[index] : "?";
}

ReadSite SetReadSite(ReadSite site) {
    return std::exchange(currentReadSite, site);
}

ReadSite CurrentReadSite() {
    return currentReadSite;
}

std::size_t CaptureCallerOffsets(std::span<unsigned long long> frames, unsigned skip) {
    if (frames.empty()) return 0;
#ifdef _WIN32

    void* raw[16];
    const auto wanted = static_cast<DWORD>(std::min<std::size_t>(frames.size(), std::size(raw)));
    const auto captured = RtlCaptureStackBackTrace(1 + skip, wanted, raw, nullptr);

    static const auto module = [] {
        HMODULE handle = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&CaptureCallerOffsets), &handle);
        std::uintptr_t size = 0;
        if (handle != nullptr) {

            const auto* base = reinterpret_cast<const std::byte*>(handle);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            size = nt->OptionalHeader.SizeOfImage;
        }
        return std::pair(reinterpret_cast<std::uintptr_t>(handle), size);
    }();
    for (std::size_t i = 0; i < captured; ++i) {
        const auto address = reinterpret_cast<std::uintptr_t>(raw[i]);
        frames[i] = module.first != 0 && address >= module.first && address - module.first < module.second ? address - module.first : 0;
    }
    return captured;
#else
    static_cast<void>(skip);
    frames[0] = ModuleOffset(__builtin_return_address(0));
    return 1;
#endif
}

void CountReadCaller(const void* returnAddress) {
    thread_local std::uint32_t sampled = 0;

    if ((sampled + 1) % 256 == 0) readSiteSamples[static_cast<std::size_t>(currentReadSite)].fetch_add(256, std::memory_order_relaxed);
    CountCaller(Profile().callers, sampled, 256, returnAddress);
}

void CountWriteCaller(const void* returnAddress) {
    thread_local std::uint32_t sampled = 0;
    CountCaller(Profile().writeCallers, sampled, 16, returnAddress);
}

bool MemoryProfiled() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

class TimedAccess {
public:
    TimedAccess(MemoryCounterKind kind, std::uint64_t bytes) : kind(kind), bytes(bytes), start(MemoryProfiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~TimedAccess() {
        if (!MemoryProfiled()) return;
        const auto now = std::chrono::steady_clock::now();
        CounterFor(kind).Add(bytes, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count()), SharedMemoryCounters());
        auto& state = Profile();
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        auto last = state.lastReport.load();
        if (nowMs - last < 10000 || !state.lastReport.compare_exchange_strong(last, nowMs)) return;
        const auto totals = FoldMemoryCounters();
        std::fprintf(stderr, "[guestmem]");
        for (std::size_t i = 0; i < MemoryCounterCount; ++i) {
            std::fprintf(stderr, " %s %llu calls %.0f MiB %.1f s", MemoryCounterNames[i], static_cast<unsigned long long>(totals[i][0]), totals[i][1] / 1048576.0, totals[i][2] / 1e9);
        }
        std::fprintf(stderr, " collect-memo hits %llu, collect epochs %llu", static_cast<unsigned long long>(collectMemoHits.load()), static_cast<unsigned long long>(collectEpochBumps.load()));
        std::fprintf(stderr, " collect-dirty %llu tracker waits %llu / %llu", static_cast<unsigned long long>(collectDirty.load()), static_cast<unsigned long long>(trackerWaits.load()), static_cast<unsigned long long>(trackerAcquisitions.load()));
        std::fprintf(stderr, " | arena 0x%llx+0x%llx image 0x%llx+0x%llx forgets %llu (%.0f MiB)", static_cast<unsigned long long>(PagesBase()), static_cast<unsigned long long>(PagesSize()), static_cast<unsigned long long>(ImagePagesBase()), static_cast<unsigned long long>(ImagePagesSize()), static_cast<unsigned long long>(forgetCalls.load()), forgetBytes.load() / 1048576.0);
        {
            std::lock_guard lock(state.callersMutex);
            std::fprintf(stderr, " | read callers:");
            for (const auto& [caller, count] : state.callers) {
                if (count != 0) std::fprintf(stderr, " +0x%llx=%llu", caller, static_cast<unsigned long long>(count));
            }
            std::fprintf(stderr, " | write callers:");
            for (const auto& [caller, count] : state.writeCallers) {
                if (count != 0) std::fprintf(stderr, " +0x%llx=%llu", caller, static_cast<unsigned long long>(count));
            }
        }
        std::fprintf(stderr, " | read sites:");
        for (std::size_t site = 0; site < static_cast<std::size_t>(ReadSite::Count); ++site) {
            const auto count = readSiteSamples[site].load(std::memory_order_relaxed);
            if (count != 0) std::fprintf(stderr, " %s=%llu", ReadSiteName(static_cast<ReadSite>(site)), static_cast<unsigned long long>(count));
        }
        std::fprintf(stderr, "\n");
    }
private:
    MemoryCounterKind kind;
    std::uint64_t bytes;
    std::chrono::steady_clock::time_point start;
};

namespace {

constexpr std::size_t PageBytes = 4096;
constexpr std::uint8_t PageReadable = 1;
constexpr std::uint8_t PageWritable = 2;

struct PageSpan {
    std::uintptr_t base = 0;
    std::size_t size = 0;

    std::uint8_t* pages = nullptr;

    bool allocate() {
        if (size == 0) return false;
        const auto count = (size / PageBytes + 8) & ~static_cast<std::size_t>(7);
#ifdef _WIN32
        pages = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, count, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        pages = static_cast<std::uint8_t*>(std::calloc(count, 1));
#endif
        if (pages == nullptr) size = 0;
        return pages != nullptr;
    }

    bool covers(std::uintptr_t address) const {
        return size != 0 && address >= base && address - base < size;
    }

    std::uint8_t load(std::uintptr_t address) const {
        return std::atomic_ref<const std::uint8_t>(pages[(address - base) / PageBytes]).load(std::memory_order_relaxed);
    }

    void store(std::uintptr_t address, std::uint8_t value) {
        std::atomic_ref<std::uint8_t>(pages[(address - base) / PageBytes]).store(value, std::memory_order_relaxed);
    }

    std::uintptr_t runEnd(std::uintptr_t address, std::uint8_t value, std::uintptr_t limit) const {
        auto index = (address - base) / PageBytes;
        const auto stop = std::min<std::uintptr_t>((std::min(limit, base + size) - base + PageBytes - 1) / PageBytes, size / PageBytes);
        const std::uint64_t wide = 0x0101010101010101ull * value;
        while (index < stop) {
            if (index % 8 == 0 && index + 8 <= stop && std::atomic_ref<const std::uint64_t>(*reinterpret_cast<const std::uint64_t*>(pages + index)).load(std::memory_order_relaxed) == wide) {
                index += 8;
                continue;
            }
            if (std::atomic_ref<const std::uint8_t>(pages[index]).load(std::memory_order_relaxed) != value) break;
            ++index;
        }
        return std::min(limit, base + index * PageBytes);
    }

    void forget(std::uintptr_t address, std::size_t bytes) {
        if (size == 0 || bytes == 0 || address >= base + size || address + bytes <= base) return;
        const auto first = (std::max(address, base) - base) / PageBytes;
        const auto last = (std::min(address + bytes, base + size) - 1 - base) / PageBytes;
        for (auto page = first; page <= last; ++page) std::atomic_ref<std::uint8_t>(pages[page]).store(0, std::memory_order_relaxed);
    }
};

struct PageStates {
    std::once_flag once;
    PageSpan arena;
    PageSpan image;

    void initialize();

    PageSpan* spanOf(std::uintptr_t address) {
        if (arena.covers(address)) return &arena;
        if (image.covers(address)) return &image;
        return nullptr;
    }

    void forget(std::uintptr_t address, std::size_t bytes) {
        arena.forget(address, bytes);
        image.forget(address, bytes);
    }
};

PageStates& Pages() {
    static PageStates pages;
    return pages;
}

std::uintptr_t PagesBase() {
    return Pages().arena.base;
}

std::size_t PagesSize() {
    return Pages().arena.size;
}

std::uintptr_t ImagePagesBase() {
    return Pages().image.base;
}

std::size_t ImagePagesSize() {
    return Pages().image.size;
}

void ForgetPages(std::uintptr_t address, std::size_t bytes) {
    forgetCalls.fetch_add(1, std::memory_order_relaxed);
    forgetBytes.fetch_add(bytes, std::memory_order_relaxed);
    forgetSerial.fetch_add(1, std::memory_order_release);
    Pages().forget(address, bytes);
    forgetSerial.fetch_add(1, std::memory_order_release);
}

void PageStates::initialize() {
    std::call_once(once, [&] {
        GuestArena::GuestArenaRange_nid_postfix(&arena.base, &arena.size);
        const bool arenaCached = arena.allocate();
        bool imageCached = false;
#ifdef _WIN32

        static const bool noImageCache = std::getenv("APS5_NO_IMAGE_PAGE_CACHE") != nullptr;
        if (const auto module = noImageCache ? nullptr : GetModuleHandleW(nullptr)) {
            const auto start = reinterpret_cast<std::uintptr_t>(module);
            auto cursor = start;
            for (;;) {
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory) || memory.AllocationBase != module || memory.RegionSize == 0 || memory.RegionSize > std::numeric_limits<std::uintptr_t>::max() - cursor) break;
                cursor += memory.RegionSize;
            }
            image.base = start;
            image.size = cursor - start;
            imageCached = image.allocate();
        }
#endif
        if (arenaCached || imageCached) GuestAllocations::GuestAllocationsSetInvalidator_nid_postfix(&ForgetPages);
    });
}

struct PageRun {
    std::uintptr_t begin;
    std::uintptr_t end;
    bool readable;
    bool writable;
};

template <class Emit>
bool describePages(std::uintptr_t address, std::size_t bytes, Emit&& emit) {
    auto& pages = Pages();
    pages.initialize();
    const auto end = address + bytes;
    auto cursor = address;
    while (cursor < end) {
        if (const auto* span = pages.spanOf(cursor)) {
            const auto value = span->load(cursor);
            if ((value & 3u) != 0) {
                const auto next = span->runEnd(cursor, value, end);
                if (!emit(PageRun{cursor, next, true, (value & PageWritable) != 0})) return true;
                cursor = next;
                continue;
            }
        }
#ifdef _WIN32
        const TimedAccess timed(CounterQuery, 0);
        MEMORY_BASIC_INFORMATION memory{};
        const auto queryStart = std::chrono::steady_clock::now();

        const auto generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory)) return false;

        static const bool traceQuery = std::getenv("APS5_TRACE_QUERY") != nullptr;
        if (traceQuery) {
            static std::atomic<std::uint32_t> queries{0};
            if (queries.fetch_add(1) % 16 == 0) {
                const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - queryStart).count();
                std::fprintf(stderr, "[query] 0x%llx range 0x%zx: base 0x%llx size 0x%llx state 0x%lx protect 0x%lx type 0x%lx %.0f us gen %llu from +0x%llx\n", static_cast<unsigned long long>(cursor), bytes, reinterpret_cast<unsigned long long>(memory.BaseAddress), static_cast<unsigned long long>(memory.RegionSize), memory.State, memory.Protect, memory.Type, us, static_cast<unsigned long long>(GuestAllocations::GuestAllocationsGeneration_nid_postfix()), ModuleOffset(__builtin_return_address(0)));
            }
        }
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base || base + memory.RegionSize <= cursor) return false;
        const auto regionEnd = base + memory.RegionSize;
        std::uint32_t logicalProtection = memory.Protect;
        GuestArena::GuestArenaProtection_nid_postfix(cursor, &logicalProtection);
        const auto protection = logicalProtection & 0xffu;
        const bool committed = memory.State == MEM_COMMIT && (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
        const bool readable = committed && readableProtection(protection);
        const bool writable = readable && writableProtection(protection);
        for (PageSpan* span : {&pages.arena, &pages.image}) {
            if (!readable || span->size == 0 || regionEnd <= span->base || base >= span->base + span->size) continue;
            const std::uint8_t value = PageReadable | (writable ? PageWritable : 0u);
            const auto first = std::max(base, span->base);
            const auto last = std::min(regionEnd, span->base + span->size);
            for (auto at = first; at < last; at += PageBytes) span->store(at, value);
            if (GuestAllocations::GuestAllocationsGeneration_nid_postfix() != generation) span->forget(first, static_cast<std::size_t>(last - first));
        }
        const auto next = std::min(end, regionEnd);
        if (!emit(PageRun{cursor, next, readable, writable})) return true;
        cursor = next;
#else
        std::ifstream maps("/proc/self/maps");
        if (!maps.is_open()) return false;
        std::string line;
        bool found = false;
        while (std::getline(maps, line)) {
            std::istringstream fields(line);
            std::uintptr_t first = 0;
            std::uintptr_t last = 0;
            char separator = 0;
            std::string permissions;
            if (!(fields >> std::hex >> first >> separator >> last >> permissions) || separator != '-' || first >= last || permissions.size() < 2) return false;
            if (last <= cursor || first > cursor) continue;
            const auto next = std::min(end, last);
            if (!emit(PageRun{cursor, next, permissions[0] == 'r', permissions[0] == 'r' && permissions[1] == 'w'})) return true;
            cursor = next;
            found = true;
            break;
        }
        if (!found) {
            static_cast<void>(emit(PageRun{cursor, end, false, false}));
            return true;
        }
#endif
    }
    return true;
}

bool onOwnLiveStack(std::uintptr_t address, std::size_t bytes) {
    struct Bounds {
        std::uintptr_t low = 0;
        std::uintptr_t high = 0;
        Bounds() {
#ifdef _WIN32
            ULONG_PTR lowLimit = 0;
            ULONG_PTR highLimit = 0;
            GetCurrentThreadStackLimits(&lowLimit, &highLimit);
            low = static_cast<std::uintptr_t>(lowLimit);
            high = static_cast<std::uintptr_t>(highLimit);
#else
            pthread_attr_t attributes;
            if (pthread_getattr_np(pthread_self(), &attributes) != 0) return;
            void* base = nullptr;
            std::size_t size = 0;
            if (pthread_attr_getstack(&attributes, &base, &size) == 0 && base != nullptr) {
                low = reinterpret_cast<std::uintptr_t>(base);
                high = low + size;
            }
            pthread_attr_destroy(&attributes);
#endif
        }
    };
    thread_local const Bounds bounds;
    const volatile unsigned char marker = 0;
    const auto frame = reinterpret_cast<std::uintptr_t>(&marker);
    return bounds.high != 0 && frame >= bounds.low && frame < bounds.high && address >= frame && address < bounds.high && bytes <= bounds.high - address;
}

std::string verify(std::uintptr_t address, std::size_t bytes, bool writable) {
    const TimedAccess timed(CounterVerify, bytes);
    if (onOwnLiveStack(address, bytes)) return {};
    std::string reason;
    const bool queried = describePages(address, bytes, [&](const PageRun& run) {
        if (!run.readable) {
            char text[128];
            std::snprintf(text, sizeof(text), "guest memory is not readable at 0x%llx (range 0x%llx+0x%zx)", static_cast<unsigned long long>(run.begin), static_cast<unsigned long long>(address), bytes);
            reason = text;
        } else if (writable && !run.writable) {
            reason = "guest memory has no write permission";
        }
        return reason.empty();
    });
    if (!queried && reason.empty()) reason = "cannot query guest memory";
    return reason;
}

}

void CheckRange(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable) {
    require(alignment != 0, "zero guest memory alignment");
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    require(address != 0, "null address");

    static std::atomic<int> reported{0};
    if (address % alignment != 0 && reported.fetch_add(1) < 8)
        aps5::LogErr( "[agc] misaligned address 0x%llx bytes=0x%zx align=%zu from +0x%llx (accepted)\n", static_cast<unsigned long long>(address), bytes, alignment, ModuleOffset(__builtin_return_address(0)));
    require(bytes <= std::numeric_limits<std::uintptr_t>::max() - address, "address range overflow");
    const auto reason = verify(address, bytes, writable);
    if (!reason.empty()) {

        static const bool trace = std::getenv("APS5_TRACE_UNREADABLE") != nullptr;
        if (trace) std::fprintf(stderr, "[unreadable] %s from +0x%llx\n", reason.c_str(), ModuleOffset(__builtin_return_address(0)));
        require(false, reason.c_str());
    }
}

bool Accessible(const void* pointer, std::size_t bytes, bool writable) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return address != 0 && bytes <= std::numeric_limits<std::uintptr_t>::max() - address && verify(address, bytes, writable).empty();
}

std::uint64_t ForgetSerial() {
    return forgetSerial.load(std::memory_order_acquire);
}

namespace {

struct CommitKey {
    std::uint64_t address;
    std::size_t bytes;
    bool writable;
    bool operator==(const CommitKey&) const = default;
};
struct CommitKeyHash {
    std::size_t operator()(const CommitKey& key) const {
        return std::hash<std::uint64_t>()(key.address * 0x9e3779b97f4a7c15ull ^ (static_cast<std::uint64_t>(key.bytes) << 1) ^ (key.writable ? 0x5bd1e995ull : 0ull));
    }
};
struct CommitEntry {
    std::uint64_t generation;
    std::uint64_t serial;
    Commitment value;
};
std::mutex commitMutex;
std::unordered_map<CommitKey, CommitEntry, CommitKeyHash> commitCache;
}

Commitment DescribeCommittedUncached(std::uint64_t address, std::size_t bytes, bool writable);

Commitment DescribeCommitted(std::uint64_t address, std::size_t bytes, bool writable) {
    static const bool cached = std::getenv("APS5_NO_COMMIT_CACHE") == nullptr;
    if (!cached || bytes < 65536) return DescribeCommittedUncached(address, bytes, writable);
    const CommitKey key{address, bytes, writable};
    const auto generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    const auto serial = ForgetSerial();
    {
        std::lock_guard lock(commitMutex);
        const auto found = commitCache.find(key);
        if (found != commitCache.end() && found->second.generation == generation && found->second.serial == serial) return found->second.value;
    }
    auto result = DescribeCommittedUncached(address, bytes, writable);
    if (GuestAllocations::GuestAllocationsGeneration_nid_postfix() == generation && ForgetSerial() == serial) {
        std::lock_guard lock(commitMutex);
        if (commitCache.size() >= 1024) commitCache.clear();
        commitCache[key] = CommitEntry{generation, serial, result};
    }
    return result;
}

Commitment DescribeCommittedUncached(std::uint64_t address, std::size_t bytes, bool writable) {
    require(bytes <= std::numeric_limits<std::uint64_t>::max() - address, "address range overflow");
    const TimedAccess timed(CounterVerify, bytes);
    Commitment result;
    const bool queried = describePages(static_cast<std::uintptr_t>(address), bytes, [&](const PageRun& run) {
        if (run.readable && (!writable || run.writable)) {
            if (!result.ranges.empty() && result.ranges.back().second == run.begin) result.ranges.back().second = run.end;
            else result.ranges.emplace_back(run.begin, run.end);
        }
        return true;
    });
    require(queried, "cannot query guest memory");
    result.whole = bytes == 0 || (result.ranges.size() == 1 && result.ranges.front().first == address && result.ranges.front().second == address + bytes);
    return result;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> CommittedRanges(std::uint64_t address, std::size_t bytes, bool writable) {
    return DescribeCommitted(address, bytes, writable).ranges;
}

namespace {

constexpr std::size_t WriteBlockBytes = 65536;

enum class StampKind : std::uint8_t { Cpu, Driver, ImportWindow };

struct WriteTracker {
    std::mutex mutex;
    bool initialized = false;
    bool watched = false;
#ifdef _WIN32
    std::uintptr_t base = 0;
    std::size_t size = 0;

    std::vector<std::uint32_t> blocks;

    std::vector<std::uint32_t> cpuBlocks;
    std::vector<std::uint32_t> writtenBlocks;
#else
    static constexpr std::size_t LeafBlocks = std::size_t{1} << 16;
    static constexpr std::size_t LeafCount = std::size_t{1} << 15;
    struct Leaf {
        std::array<std::uint32_t, LeafBlocks> blocks{};
        std::array<std::uint32_t, LeafBlocks> cpuBlocks{};
        std::array<std::uint32_t, LeafBlocks> writtenBlocks{};
    };
    std::vector<std::unique_ptr<Leaf>> leaves;
#endif

    std::atomic<std::uint32_t> generation{1};
    std::vector<void*> pages;

    struct Memo {
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        std::uint64_t epoch = 0;
        std::uint64_t unwatched = 0;
    };
    std::array<Memo, 256> memo{};
    std::size_t nextMemo = 0;

    std::array<Memo, 512> cleanMemo{};
    std::size_t nextCleanMemo = 0;

    void initialize() {
        if (initialized) return;
        initialized = true;
#ifdef _WIN32
        GuestArena::GuestArenaRange_nid_postfix(&base, &size);
        watched = size != 0 && GuestArena::GuestArenaWriteWatched_nid_postfix();
        if (!watched) return;
        blocks.assign(size / WriteBlockBytes + 1, 0);
        cpuBlocks.assign(size / WriteBlockBytes + 1, 0);
        writtenBlocks.assign(size / WriteBlockBytes + 1, 0);
        pages.resize(1u << 16);
#else
        watched = GuestWriteWatch::GuestWriteWatchAvailable_nid_postfix();
        if (watched) leaves.resize(LeafCount);
#endif
    }

    bool covers(std::uint64_t address, std::size_t bytes) const {
#ifdef _WIN32
        return address >= base && address - base <= size - bytes;
#else
        return address + bytes <= LeafCount * LeafBlocks * WriteBlockBytes && GuestWriteWatch::GuestWriteWatchCovers_nid_postfix(static_cast<std::uintptr_t>(address), bytes);
#endif
    }

    std::uint64_t blockOf(std::uint64_t address) const {
#ifdef _WIN32
        return (address - base) / WriteBlockBytes;
#else
        return address / WriteBlockBytes;
#endif
    }

    std::uint32_t stampOf(std::uint64_t block) const {
#ifdef _WIN32
        return blocks[block];
#else
        const auto& leaf = leaves[block / LeafBlocks];
        return leaf != nullptr ? leaf->blocks[block % LeafBlocks] : 0;
#endif
    }

    std::uint32_t cpuStampOf(std::uint64_t block) const {
#ifdef _WIN32
        return cpuBlocks[block];
#else
        const auto& leaf = leaves[block / LeafBlocks];
        return leaf != nullptr ? leaf->cpuBlocks[block % LeafBlocks] : 0;
#endif
    }

    std::uint32_t writtenStampOf(std::uint64_t block) const {
#ifdef _WIN32
        return writtenBlocks[block];
#else
        const auto& leaf = leaves[block / LeafBlocks];
        return leaf != nullptr ? leaf->writtenBlocks[block % LeafBlocks] : 0;
#endif
    }

    void stamp(std::uint64_t block, std::uint32_t stampGeneration, StampKind kind) {
        const bool cpu = kind != StampKind::Driver;
        const bool written = kind != StampKind::ImportWindow;
#ifdef _WIN32
        blocks[block] = stampGeneration;
        if (cpu) cpuBlocks[block] = stampGeneration;
        if (written) writtenBlocks[block] = stampGeneration;
#else
        auto& leaf = leaves[block / LeafBlocks];
        if (leaf == nullptr) leaf = std::make_unique<Leaf>();
        leaf->blocks[block % LeafBlocks] = stampGeneration;
        if (cpu) leaf->cpuBlocks[block % LeafBlocks] = stampGeneration;
        if (written) leaf->writtenBlocks[block % LeafBlocks] = stampGeneration;
#endif
    }
};

WriteTracker& Tracker() {
    static WriteTracker tracker;
    return tracker;
}

std::unique_lock<std::mutex> lockTracker(WriteTracker& tracker) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return std::unique_lock(tracker.mutex);
    std::unique_lock lock(tracker.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        trackerWaits.fetch_add(1, std::memory_order_relaxed);
        lock.lock();
    }
    trackerAcquisitions.fetch_add(1, std::memory_order_relaxed);
    return lock;
}

std::atomic<std::uint64_t> nextCollectEpoch{1};
std::atomic<std::uint64_t> collectFrameCounter{1};
thread_local bool lastWalkDirty = false;

struct CleanTrust {
    std::uint64_t streak = 0, lastWalkFrame = 0;
    std::uintptr_t end = 0;
};
std::unordered_map<std::uintptr_t, CleanTrust> cleanTrust;

std::unordered_map<std::uint64_t, std::uint64_t> widenFailures;
constexpr std::size_t TrustMinBytes = 1u << 20;
constexpr std::uint64_t TrustStreak = 48;
constexpr std::uint64_t TrustPeriod = 8;

bool frameCleanMemoEnabled() {
    static const bool enabled = std::getenv("APS5_NO_FRAME_COLLECT_MEMO") == nullptr;
    return enabled;
}
thread_local std::uint64_t threadCollectEpoch = 0;

std::uint64_t currentCollectEpoch() {
    if (threadCollectEpoch != 0) return threadCollectEpoch;
    return nextCollectEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool collectMemoEnabled() {
    static const bool disabled = std::getenv("APS5_NO_COLLECT_MEMO") != nullptr;
    return !disabled;
}

struct ThreadCollectMemo {
    std::array<WriteTracker::Memo, 64> entries{};
    std::size_t next = 0;
};
thread_local ThreadCollectMemo threadCollectMemo;

bool sharedCollectMemo() {
    static const bool shared = std::getenv("APS5_SHARED_COLLECT_MEMO") != nullptr;
    return shared;
}

#ifndef _WIN32
struct StampRuns {
    WriteTracker& tracker;
    StampKind kind;
    std::uint32_t stampGeneration;
};

void stampWrittenRun(void* context, std::uintptr_t begin, std::uintptr_t end) {
    auto& [tracker, kind, generation] = *static_cast<StampRuns*>(context);
    if (end <= begin) return;
    for (auto block = tracker.blockOf(begin); block <= tracker.blockOf(end - 1); ++block) tracker.stamp(block, generation, kind);
    collectDirtyRuns.fetch_add(1, std::memory_order_relaxed);
}
#endif

bool walkWritesStamped(WriteTracker& tracker, std::uint64_t first, std::uint64_t stop, StampKind kind, std::uint32_t stampGen) {
#ifdef _WIN32
    constexpr std::uint64_t page = 4096;

    static const bool singlePass = std::getenv("APS5_NO_SINGLE_PASS_COLLECT") == nullptr;
    auto cursor = first;
    bool dirty = false;
    while (cursor < stop) {
        std::size_t count = tracker.pages.size();
        DWORD granularity = 4096;
        if (!GuestArena::GuestArenaCollectWrites_nid_postfix(cursor, static_cast<std::size_t>(stop - cursor), tracker.pages.data(), &count, singlePass)) {

            return false;
        }
        if (count != 0) dirty = true;
        for (ULONG_PTR i = 0; i < count; ++i) tracker.stamp(tracker.blockOf(reinterpret_cast<std::uintptr_t>(tracker.pages[i])), stampGen, kind);
        if (count < tracker.pages.size()) break;
        cursor = reinterpret_cast<std::uintptr_t>(tracker.pages[count - 1]) + (granularity != 0 ? granularity : page);
    }
    if (dirty) collectDirty.fetch_add(1, std::memory_order_relaxed);
    lastWalkDirty = dirty;
    if (dirty && !singlePass) {

        cursor = first;
        while (cursor < stop) {
            std::size_t count = tracker.pages.size();
            DWORD granularity = 4096;
            if (!GuestArena::GuestArenaCollectWrites_nid_postfix(cursor, static_cast<std::size_t>(stop - cursor), tracker.pages.data(), &count, true)) return false;
            for (ULONG_PTR i = 0; i < count; ++i) tracker.stamp(tracker.blockOf(reinterpret_cast<std::uintptr_t>(tracker.pages[i])), stampGen, kind);
            if (count < tracker.pages.size()) break;
            cursor = reinterpret_cast<std::uintptr_t>(tracker.pages[count - 1]) + (granularity != 0 ? granularity : page);
        }
    }
    return true;
#else
    const auto dirtyRuns = collectDirtyRuns.load(std::memory_order_relaxed);
    StampRuns runs{tracker, kind, stampGen};
    const bool complete = GuestWriteWatch::GuestWriteWatchCollect_nid_postfix(static_cast<std::uintptr_t>(first), static_cast<std::size_t>(stop - first), &stampWrittenRun, &runs);
    lastWalkDirty = collectDirtyRuns.load(std::memory_order_relaxed) != dirtyRuns;
    if (lastWalkDirty) collectDirty.fetch_add(1, std::memory_order_relaxed);
    return complete;
#endif
}

bool walkWrites(WriteTracker& tracker, std::uint64_t first, std::uint64_t stop, StampKind kind) {
    const std::uint32_t stampGen = tracker.generation.load(std::memory_order_relaxed) + 1;
    const bool complete = walkWritesStamped(tracker, first, stop, kind, stampGen);
    tracker.generation.store(stampGen, std::memory_order_release);
    return complete;
}

std::uint64_t collectWrites(std::uint64_t address, std::size_t bytes, bool memoized) {
    auto& tracker = Tracker();

    constexpr std::uint64_t page = 4096;
    const auto first = address & ~(page - 1);
    const auto stop = (address + bytes + page - 1) & ~(page - 1);

    const auto epoch = currentCollectEpoch();
    const auto unwatched = unwatchSerial.load(std::memory_order_acquire);
    const bool useMemo = memoized && collectMemoEnabled() && bytes != 0;
    if (useMemo && !sharedCollectMemo()) {

        for (const auto& entry : threadCollectMemo.entries) {
            if (entry.epoch == epoch && entry.unwatched == unwatched && entry.begin <= first && stop <= entry.end) {

                collectMemoHits.fetch_add(1, std::memory_order_relaxed);
                return tracker.generation.load(std::memory_order_relaxed);
            }
        }
    }
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address || !tracker.covers(address, bytes)) return 0;
    auto cursor = first;
    const auto frame = collectFrameCounter.load(std::memory_order_relaxed);
    const bool frameMemo = useMemo && frameCleanMemoEnabled();
    if (frameMemo) {
        const auto serial = unwatchSerial.load(std::memory_order_relaxed);
        for (const auto& entry : tracker.cleanMemo) {
            if (entry.epoch == frame && entry.unwatched == serial && entry.begin <= first && stop <= entry.end) {
                collectMemoHits.fetch_add(1, std::memory_order_relaxed);
                return tracker.generation;
            }
        }
    }
    if (useMemo && sharedCollectMemo()) {
        for (const auto& entry : tracker.memo) {
            if (entry.epoch == epoch && entry.unwatched == unwatchSerial.load(std::memory_order_relaxed) && entry.begin <= cursor && stop <= entry.end) {
                collectMemoHits.fetch_add(1, std::memory_order_relaxed);
                return tracker.generation;
            }
        }
    }
    static const bool trustClean = std::getenv("APS5_NO_TRUST_CLEAN") == nullptr;
    const bool trustRange = useMemo && trustClean && stop - first >= TrustMinBytes;
    if (trustRange) {
        const auto found = cleanTrust.find(first);
        if (found != cleanTrust.end() && found->second.streak >= TrustStreak && stop <= found->second.end && frame - found->second.lastWalkFrame < TrustPeriod) {
            collectMemoHits.fetch_add(1, std::memory_order_relaxed);
            return tracker.generation;
        }
    }
    const TimedAccess timed(CounterCollect, bytes);

    static const bool widenWalks = std::getenv("APS5_NO_WIDEN_WALK") == nullptr;
    auto walkFirst = first, walkStop = stop;
    bool widened = false;
#ifdef _WIN32
    if (widenWalks && stop - first < WriteBlockBytes) {
        const auto firstBlock = tracker.blockOf(first), lastBlock = tracker.blockOf(stop - 1);
        if (lastBlock - firstBlock < 2) {
            const std::uint64_t windowFirst = tracker.base + firstBlock * WriteBlockBytes;
            const std::uint64_t windowStop = std::min<std::uint64_t>(tracker.base + tracker.size, tracker.base + (lastBlock + 1) * WriteBlockBytes);
            const auto allocations = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
            const auto failed = widenFailures.find(windowFirst);
            if (windowStop >= stop && (failed == widenFailures.end() || failed->second != allocations)) {
                walkFirst = windowFirst;
                walkStop = windowStop;
                widened = true;
            }
        }
    }
#endif
    if (!walkWrites(tracker, walkFirst, walkStop, StampKind::Cpu)) {
        if (!widened) return 0;
        if (widenFailures.size() >= 4096) widenFailures.clear();
        widenFailures[walkFirst] = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        walkFirst = first;
        walkStop = stop;
        if (!walkWrites(tracker, walkFirst, walkStop, StampKind::Cpu)) return 0;
    }
    if (trustRange) {
        auto& trust = cleanTrust[first];
        if (lastWalkDirty) trust.streak = 0;
        else ++trust.streak;
        trust.lastWalkFrame = frame;
        trust.end = stop;
    }

    if (frameMemo && !lastWalkDirty) tracker.cleanMemo[tracker.nextCleanMemo++ % tracker.cleanMemo.size()] = {walkFirst, walkStop, frame, unwatchSerial.load(std::memory_order_relaxed)};

    if (collectMemoEnabled()) {
        const auto serial = unwatchSerial.load(std::memory_order_relaxed);
        if (sharedCollectMemo()) tracker.memo[tracker.nextMemo++ % tracker.memo.size()] = {walkFirst, walkStop, epoch, serial};
        else threadCollectMemo.entries[threadCollectMemo.next++ % threadCollectMemo.entries.size()] = {walkFirst, walkStop, epoch, serial};
    }
    return tracker.generation;
}

}

namespace {

struct WalkTrace {
    std::mutex mutex;
    std::unordered_map<unsigned long long, std::uint64_t> callers;
    std::unordered_map<unsigned long long, std::uint64_t> ranges;
    std::unordered_map<unsigned long long, std::uint64_t> dirty;
    std::uint64_t total = 0, walked = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
WalkTrace& walkTrace() {
    static WalkTrace trace;
    return trace;
}
}

std::uint64_t CollectWrites(std::uint64_t address, std::size_t bytes) {
    static const bool trace = std::getenv("APS5_TRACE_WALKS") != nullptr;
    if (!trace) return collectWrites(address, bytes, true);
    const auto caller = ModuleOffset(__builtin_return_address(0));
    const auto before = collectDirty.load(std::memory_order_relaxed);
    const auto result = collectWrites(address, bytes, true);
    auto& state = walkTrace();
    std::lock_guard lock(state.mutex);
    ++state.total;
    ++state.callers[caller];
    ++state.ranges[(address << 1) ^ (static_cast<unsigned long long>(bytes) << 40)];
    if (collectDirty.load(std::memory_order_relaxed) != before) ++state.dirty[caller];
    const auto now = std::chrono::steady_clock::now();
    if (now - state.last > std::chrono::seconds(10)) {
        state.last = now;
        std::vector<std::pair<std::uint64_t, unsigned long long>> top;
        for (const auto& [key, count] : state.callers) top.emplace_back(count, key);
        std::sort(top.rbegin(), top.rend());
        std::string text;
        for (std::size_t i = 0; i < top.size() && i < 10; ++i) text += " +0x" + [&] { char b[32]; std::snprintf(b, sizeof(b), "%llx", top[i].second); return std::string(b); }() + "=" + std::to_string(top[i].first) + "/dirty" + std::to_string(state.dirty[top[i].second]);
        aps5::LogErr("[walks] %llu CollectWrites calls, %zu distinct ranges; callers:%s\n", static_cast<unsigned long long>(state.total), state.ranges.size(), text.c_str());
        state.callers.clear(); state.ranges.clear(); state.dirty.clear(); state.total = 0;
    }
    return result;
}

std::uint64_t CollectWritesUncached(std::uint64_t address, std::size_t bytes) {
    return collectWrites(address, bytes, false);
}

void BumpCollectEpoch() {
    threadCollectEpoch = nextCollectEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
    collectEpochBumps.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t CollectEpochBumps() {
    return collectEpochBumps.load(std::memory_order_relaxed);
}

namespace {

void unwatchLocked(const WriteTracker& tracker, std::uint64_t address, std::size_t bytes) {
    if (!tracker.watched) return;
#ifdef _WIN32
    static_cast<void>(address);
    static_cast<void>(bytes);
#else
    if (GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(reinterpret_cast<const void*>(address), bytes)) unwatchSerial.fetch_add(1, std::memory_order_release);
#endif
}

}

void Unwatch(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return;
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    unwatchLocked(tracker, address, bytes);
}

bool ImportWatched(std::uint64_t address, std::size_t bytes, const std::function<bool()>& import) {
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return import();
    auto& tracker = Tracker();
    auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched) {
        lock.unlock();
        return import();
    }
    constexpr std::uint64_t page = 4096;
    const auto first = address & ~(page - 1);
    const auto stop = (address + bytes + page - 1) & ~(page - 1);
    const bool covered = tracker.covers(first, static_cast<std::size_t>(stop - first));
    const bool before = covered && walkWrites(tracker, first, stop, StampKind::Cpu);
    if (!import()) return false;
    if (!before || !walkWrites(tracker, first, stop, StampKind::ImportWindow)) unwatchLocked(tracker, first, static_cast<std::size_t>(stop - first));
    return true;
}

bool WriteWatched() {
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    return tracker.watched;
}

bool Watched(std::uint64_t address, std::size_t bytes) {
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    return tracker.watched && bytes != 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address && tracker.covers(address, bytes);
}

bool UnchangedSince(std::uint64_t address, std::size_t bytes, std::uint64_t generation) {
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched || generation == 0 || bytes == 0 || !tracker.covers(address, bytes)) return false;
    const auto first = tracker.blockOf(address);
    const auto last = tracker.blockOf(address + bytes - 1);
    for (auto block = first; block <= last; ++block) {
        if (tracker.stampOf(block) > generation) return false;
    }
    return true;
}

bool UnchangedSinceAll(std::span<const UnchangedQuery> queries) {
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched) return false;
    for (const auto& [address, bytes, generation] : queries) {
        if (generation == 0 || bytes == 0 || !tracker.covers(address, bytes)) return false;
        const auto first = tracker.blockOf(address);
        const auto last = tracker.blockOf(address + bytes - 1);
        for (auto block = first; block <= last; ++block) {
            if (tracker.stampOf(block) > generation) return false;
        }
    }
    return true;
}

std::uint64_t MarkWritten(std::uint64_t address, std::size_t bytes) {
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched || bytes == 0 || !tracker.covers(address, bytes)) return 0;
    const auto first = tracker.blockOf(address);
    const auto last = tracker.blockOf(address + bytes - 1);
    const std::uint32_t stampGen = tracker.generation.load(std::memory_order_relaxed) + 1;
    for (auto block = first; block <= last; ++block) tracker.stamp(block, stampGen, StampKind::Driver);
    tracker.generation.store(stampGen, std::memory_order_release);
    return stampGen;
}

std::uint64_t TrackerGeneration() {
    auto& tracker = Tracker();

    const auto lock = lockTracker(tracker);
    return tracker.generation;
}

bool UnchangedSinceCollected(std::uint64_t address, std::size_t bytes, std::uint64_t generation) {
    if (CollectWritesUncached(address, bytes) == 0) return false;
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    if (!tracker.watched || generation == 0 || bytes == 0 || !tracker.covers(address, bytes)) return false;
    const auto first = tracker.blockOf(address);
    const auto last = tracker.blockOf(address + bytes - 1);
    for (auto block = first; block <= last; ++block) {
        if (tracker.cpuStampOf(block) > generation) return false;
    }
    return true;
}

bool WrittenSince(std::uint64_t address, std::size_t bytes, std::uint64_t generation) {
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched || generation == 0 || bytes == 0 || !tracker.covers(address, bytes)) return false;
    const auto first = tracker.blockOf(address);
    const auto last = tracker.blockOf(address + bytes - 1);
    for (auto block = first; block <= last; ++block) {
        if (tracker.writtenStampOf(block) > generation) return true;
    }
    return false;
}

bool ChangedBlocks(std::uint64_t address, std::size_t bytes, std::span<const std::uint64_t> generations, std::span<std::uint8_t> changed, std::span<std::uint8_t> cpu) {
    std::fill(changed.begin(), changed.end(), BlockWritten);
    std::fill(cpu.begin(), cpu.end(), std::uint8_t{1});
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched || bytes == 0 || !tracker.covers(address, bytes)) return false;
    const auto first = tracker.blockOf(address);
    const auto last = tracker.blockOf(address + bytes - 1);
    for (auto block = first; block <= last; ++block) {
        const auto k = block - first;
        if (k >= generations.size() || k >= changed.size()) break;
        const auto generation = generations[k];
        changed[k] = generation == 0 || tracker.writtenStampOf(block) > generation ? BlockWritten : tracker.stampOf(block) > generation ? BlockMaybeWritten : BlockUnchanged;
        if (k < cpu.size()) cpu[k] = generation == 0 || tracker.cpuStampOf(block) > generation ? 1 : 0;
    }
    return true;
}

namespace {

constexpr std::size_t GpuLockSiteCount = static_cast<std::size_t>(GpuLockSite::Count);
constexpr const char* GpuLockSiteNames[GpuLockSiteCount] = {"other", "dispatch", "indirect", "draw", "hook", "wait", "label", "flush", "present", "fill", "copy", "end", "try"};

constexpr std::size_t HolderColumns = 12;
constexpr std::uint32_t UnassignedHolder = 0xfffffffeu;
constexpr std::uint32_t PresenterHolderTag = 0xfffffffdu;
constexpr int PresenterColumn = 1;
using HolderMatrixValues = std::array<std::array<std::uint64_t, GpuLockSiteCount>, HolderColumns>;
struct HolderMatrix {
    std::array<std::atomic<std::uint32_t>, HolderColumns> tags{};
    std::array<std::array<std::atomic<std::uint64_t>, GpuLockSiteCount>, HolderColumns> heldNs{};
    HolderMatrix() {
        for (auto& tag : tags) tag.store(UnassignedHolder, std::memory_order_relaxed);
        tags[0].store(0xffffffffu, std::memory_order_relaxed);
        tags[PresenterColumn].store(PresenterHolderTag, std::memory_order_relaxed);
    }
    void Snapshot(HolderMatrixValues& into) const {
        for (std::size_t h = 0; h < HolderColumns; ++h) {
            for (std::size_t s = 0; s < GpuLockSiteCount; ++s) into[h][s] = heldNs[h][s].load(std::memory_order_relaxed);
        }
    }
};

HolderMatrix& Holders() {
    static HolderMatrix matrix;
    return matrix;
}

std::size_t HolderColumnFor(std::uint32_t tag) {
    if (tag == 0xffffffffu) return 0;
    auto& holders = Holders();
    for (std::size_t i = 1; i < HolderColumns; ++i) {
        auto expected = UnassignedHolder;
        if (holders.tags[i].load(std::memory_order_acquire) == tag) return i;
        if (holders.tags[i].compare_exchange_strong(expected, tag, std::memory_order_acq_rel)) return i;
        if (expected == tag) return i;
    }
    return 0;
}

struct GpuLockStats {
    std::uint32_t tag = 0xffffffffu;
    std::uint64_t acquisitions = 0;
    std::uint64_t waits = 0;
    double waitedMs = 0;

    std::array<std::uint64_t, GpuLockSiteCount> siteAcquisitions{};
    std::array<std::uint64_t, GpuLockSiteCount> siteWaits{};
    std::array<double, GpuLockSiteCount> siteWaitedMs{};
    GpuLockSite nextSite = GpuLockSite::Other;

    std::array<std::uint64_t, GpuLockSiteCount> siteHolds{};
    std::array<double, GpuLockSiteCount> siteHeldMs{};
    std::array<double, GpuLockSiteCount> siteMaxHeldMs{};
    std::size_t holdSite = 0;
    std::chrono::steady_clock::time_point holdStart;

    int holderColumn = -1;
    HolderMatrixValues waitedBehindNs{};
    std::uint64_t tries = 0;
    std::uint64_t triesFailed = 0;
    std::uint64_t lockedGpuWaits = 0;
    double lockedGpuWaitMs = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

GpuLockStats& LockStats() {
    thread_local GpuLockStats stats;
    return stats;
}

bool GpuLockProfiled() {
    static const bool profiled = std::getenv("APS5_PROFILE_DRAW") != nullptr && std::getenv("APS5_NO_LOCK_PROFILE") == nullptr;
    return profiled;
}

bool GpuHoldProfiled() {
    static const bool profiled = GpuLockProfiled() && std::getenv("APS5_NO_HOLD_PROFILE") == nullptr;
    return profiled;
}

void BeginHold(std::size_t site) {
    auto& stats = LockStats();
    stats.holdSite = site;
    stats.holdStart = std::chrono::steady_clock::now();
}

void EndHold() {
    auto& stats = LockStats();
    const auto heldNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - stats.holdStart).count();
    const auto heldMs = static_cast<double>(heldNs) / 1e6;
    ++stats.siteHolds[stats.holdSite];
    stats.siteHeldMs[stats.holdSite] += heldMs;
    stats.siteMaxHeldMs[stats.holdSite] = std::max(stats.siteMaxHeldMs[stats.holdSite], heldMs);
    if (stats.holderColumn < 0) stats.holderColumn = static_cast<int>(HolderColumnFor(stats.tag));
    Holders().heldNs[static_cast<std::size_t>(stats.holderColumn)][stats.holdSite].fetch_add(static_cast<std::uint64_t>(heldNs), std::memory_order_relaxed);
}

const char* HolderName(std::size_t column, char* text, std::size_t size) {
    const auto tag = Holders().tags[column].load(std::memory_order_relaxed);
    if (tag == 0xffffffffu) std::snprintf(text, size, "untagged");
    else if (tag == PresenterHolderTag) std::snprintf(text, size, "presenter");
    else std::snprintf(text, size, "q0x%x", tag);
    return text;
}

std::string WaitedBehindReport(const HolderMatrixValues& waited) {
    std::vector<std::tuple<std::uint64_t, std::size_t, std::size_t>> entries;
    for (std::size_t h = 0; h < HolderColumns; ++h) {
        for (std::size_t s = 0; s < GpuLockSiteCount; ++s) {
            if (waited[h][s] >= 1000000) entries.emplace_back(waited[h][s], h, s);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
    std::string report;
    char text[96];
    char holder[24];
    for (std::size_t i = 0; i < entries.size() && i < 16; ++i) {
        const auto& [ns, h, s] = entries[i];
        std::snprintf(text, sizeof(text), " %s/%s %.0f", HolderName(h, holder, sizeof(holder)), GpuLockSiteNames[s], static_cast<double>(ns) / 1e6);
        report += text;
    }
    return report;
}

}

namespace {
const void* ThreadToken() {
    thread_local char token;
    return &token;
}
}

void GpuMutexType::acquired() {
    owner.store(ThreadToken(), std::memory_order_relaxed);
    ++depth;
}

bool GpuMutexType::try_lock() {
    if (!GpuLockProfiled()) {
        if (!mutex.try_lock()) return false;
        acquired();
        return true;
    }

    auto& stats = LockStats();
    const auto tagged = std::exchange(stats.nextSite, GpuLockSite::Other);
    ++stats.tries;
    if (!mutex.try_lock()) {
        ++stats.triesFailed;
        return false;
    }
    acquired();
    if (depth == 1 && GpuHoldProfiled()) BeginHold(static_cast<std::size_t>(tagged == GpuLockSite::Other ? GpuLockSite::Try : tagged));
    return true;
}

namespace {
std::atomic<void (*)()> gpuUnlockHook{nullptr};
}

void SetGpuUnlockHook(void (*hook)()) {
    gpuUnlockHook.store(hook, std::memory_order_release);
}

void GpuMutexType::unlock() {
    const bool outermost = --depth == 0;
    if (outermost) {
        owner.store(nullptr, std::memory_order_relaxed);

        if (GpuHoldProfiled()) EndHold();
    }
    mutex.unlock();

    if (outermost) {
        if (auto* hook = gpuUnlockHook.load(std::memory_order_acquire); hook != nullptr) hook();
    }
}

bool GpuMutexType::HeldByThisThread() const {
    return owner.load(std::memory_order_relaxed) == ThreadToken();
}

std::uint32_t GpuMutexType::DepthOnThisThread() const {

    return HeldByThisThread() ? depth : 0;
}

void AssertGpuLockHeld(const char* where) {
    static const bool enabled = std::getenv("APS5_ASSERT_GPU_LOCK") != nullptr;
    if (!enabled || GpuMutex().HeldByThisThread()) return;
    std::fprintf(stderr, "[lock] ASSERT: %s reached without GuestMemory::GpuMutex on thread tagged 0x%x\n", where, GpuLockThreadTag());
    std::abort();
}

void GpuMutexType::lock() {
    if (!GpuLockProfiled()) {
        mutex.lock();
        acquired();
        return;
    }
    auto& stats = LockStats();
    ++stats.acquisitions;
    const auto site = static_cast<std::size_t>(std::exchange(stats.nextSite, GpuLockSite::Other));
    ++stats.siteAcquisitions[site];

    if (!mutex.try_lock()) {
        const bool holders = GpuHoldProfiled();
        HolderMatrixValues before{};
        if (holders) Holders().Snapshot(before);
        const auto start = std::chrono::steady_clock::now();
        mutex.lock();
        acquired();
        const auto now = std::chrono::steady_clock::now();
        if (holders) {
            HolderMatrixValues after{};
            Holders().Snapshot(after);
            for (std::size_t h = 0; h < HolderColumns; ++h) {
                for (std::size_t s = 0; s < GpuLockSiteCount; ++s) stats.waitedBehindNs[h][s] += after[h][s] - before[h][s];
            }
        }
        if (depth == 1 && holders) {
            stats.holdSite = site;
            stats.holdStart = now;
        }
        const auto waitedMs = std::chrono::duration<double, std::milli>(now - start).count();
        stats.waitedMs += waitedMs;
        ++stats.waits;
        stats.siteWaitedMs[site] += waitedMs;
        ++stats.siteWaits[site];
        if (now - stats.lastReport < std::chrono::seconds(10)) return;

        const auto interval = std::chrono::duration<double>(now - stats.lastReport).count();
        stats.lastReport = now;
        std::string sites;
        for (std::size_t i = 0; i < GpuLockSiteCount; ++i) {
            if (stats.siteAcquisitions[i] == 0) continue;
            char text[96];
            std::snprintf(text, sizeof(text), " %s %llu/%llu %.0f ms", GpuLockSiteNames[i], static_cast<unsigned long long>(stats.siteAcquisitions[i]), static_cast<unsigned long long>(stats.siteWaits[i]), stats.siteWaitedMs[i]);
            sites += text;
        }

        std::string holds;
        if (GpuHoldProfiled()) {
            std::array<std::size_t, GpuLockSiteCount> order{};
            for (std::size_t i = 0; i < GpuLockSiteCount; ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return stats.siteHeldMs[a] > stats.siteHeldMs[b]; });
            double heldMs = 0;
            for (std::size_t i = 0; i < GpuLockSiteCount; ++i) heldMs += stats.siteHeldMs[i];
            char text[112];
            std::snprintf(text, sizeof(text), "; held %.0f ms in total, by site (holds, held, max):", heldMs);
            holds += text;
            for (const auto i : order) {
                if (stats.siteHolds[i] == 0) continue;
                std::snprintf(text, sizeof(text), " %s %llu %.0f ms (max %.1f)", GpuLockSiteNames[i], static_cast<unsigned long long>(stats.siteHolds[i]), stats.siteHeldMs[i], stats.siteMaxHeldMs[i]);
                holds += text;
            }
            holds += "; waited behind (holder/site ms):" + WaitedBehindReport(stats.waitedBehindNs);
        }
        char counts[160];
        std::snprintf(counts, sizeof(counts), "; lock() %llu / try %llu / try failed %llu; locked GPU waits %llu / %.0f ms", static_cast<unsigned long long>(stats.acquisitions), static_cast<unsigned long long>(stats.tries), static_cast<unsigned long long>(stats.triesFailed), static_cast<unsigned long long>(stats.lockedGpuWaits), stats.lockedGpuWaitMs);
        holds += counts;
        if (stats.tag == 0xffffffffu) std::fprintf(stderr, "[lock] %s waited %.0f ms for the GPU mutex in %llu of %llu acquisitions (%.0f s); by site (acquisitions/waits, waited):%s%s\n", stats.holderColumn == PresenterColumn ? "presenter" : "untagged thread", stats.waitedMs, static_cast<unsigned long long>(stats.waits), static_cast<unsigned long long>(stats.acquisitions), interval, sites.c_str(), holds.c_str());
        else std::fprintf(stderr, "[lock] queue 0x%x waited %.0f ms for the GPU mutex in %llu of %llu acquisitions (%.0f s); by site (acquisitions/waits, waited):%s%s\n", stats.tag, stats.waitedMs, static_cast<unsigned long long>(stats.waits), static_cast<unsigned long long>(stats.acquisitions), interval, sites.c_str(), holds.c_str());
        stats.waitedMs = 0;
        stats.waits = 0;
        stats.acquisitions = 0;
        stats.siteAcquisitions.fill(0);
        stats.siteWaits.fill(0);
        stats.siteWaitedMs.fill(0);
        stats.siteHolds.fill(0);
        stats.siteHeldMs.fill(0);
        stats.siteMaxHeldMs.fill(0);
        for (auto& row : stats.waitedBehindNs) row.fill(0);
        stats.tries = 0;
        stats.triesFailed = 0;
        stats.lockedGpuWaits = 0;
        stats.lockedGpuWaitMs = 0;
        return;
    }
    acquired();
    if (depth == 1 && GpuHoldProfiled()) BeginHold(site);
}

void TagGpuLockSite(GpuLockSite site) {

    if (!GpuLockProfiled()) return;
    LockStats().nextSite = site;
}

void MarkPresenterThread() {
    LockStats().holderColumn = PresenterColumn;
}

void NoteLockedGpuWait(double ms) {
    if (!GpuLockProfiled() || GpuMutex().DepthOnThisThread() == 0) return;
    auto& stats = LockStats();
    ++stats.lockedGpuWaits;
    stats.lockedGpuWaitMs += ms;
}

std::uint64_t ThreadCollectedBytes() {
    if (!MemoryProfiled()) return 0;
    return CounterFor(CounterCollect).bytes.load(std::memory_order_relaxed);
}

std::uint32_t GpuLockThreadTag() {
    return LockStats().tag;
}

GpuMutexType& GpuMutex() {
    static GpuMutexType mutex;
    return mutex;
}

void TagGpuLockThread(std::uint32_t queue) {
    LockStats().tag = queue;
}

unsigned long long CodeOffset(const void* address) {
    return ModuleOffset(address);
}

namespace {
std::atomic<void (*)(std::uint64_t, std::size_t)> flushHook{nullptr};
}

void SetFlushHook(void (*hook)(std::uint64_t, std::size_t)) {
    flushHook.store(hook, std::memory_order_release);
}

void FlushGpuWrites(std::uint64_t address, std::size_t bytes) {
    if (const auto hook = flushHook.load(std::memory_order_acquire)) hook(address, bytes);
}

void Read(std::uint64_t address, std::span<std::byte> destination, std::size_t alignment) {
    if (destination.empty()) return;
    FlushGpuWrites(address, destination.size());
    const TimedAccess timed(CounterRead, destination.size());
    CountReadCaller(__builtin_return_address(0));
    const auto* source = reinterpret_cast<const void*>(address);
    try {
        CheckRange(source, destination.size(), alignment);
    } catch (const std::runtime_error&) {
        static const bool trace = std::getenv("APS5_TRACE_UNREADABLE") != nullptr;
        if (trace) std::fprintf(stderr, "[unreadable]   read from +0x%llx\n", ModuleOffset(__builtin_return_address(0)));
        throw;
    }
    std::memcpy(destination.data(), source, destination.size());
}

void ReadCommitted(std::uint64_t address, std::span<std::byte> destination) {
    if (destination.empty()) return;
    FlushGpuWrites(address, destination.size());
    const TimedAccess timed(CounterRead, destination.size());
    CountReadCaller(__builtin_return_address(0));
    if (Accessible(reinterpret_cast<const void*>(address), destination.size())) {
        std::memcpy(destination.data(), reinterpret_cast<const void*>(address), destination.size());
        return;
    }
    std::memset(destination.data(), 0, destination.size());
    for (const auto& [begin, end] : CommittedRanges(address, destination.size())) std::memcpy(destination.data() + (begin - address), reinterpret_cast<const void*>(begin), static_cast<std::size_t>(end - begin));
}

bool EqualsCommitted(std::uint64_t address, std::span<const std::byte> bytes) {
    if (bytes.empty()) return true;
    FlushGpuWrites(address, bytes.size());
    return EqualsCommittedUnsynced(address, bytes);
}

bool EqualsCommittedUnsynced(std::uint64_t address, std::span<const std::byte> bytes) {
    if (bytes.empty()) return true;
    const TimedAccess timed(CounterCompare, bytes.size());
    if (Accessible(reinterpret_cast<const void*>(address), bytes.size())) return std::memcmp(reinterpret_cast<const void*>(address), bytes.data(), bytes.size()) == 0;
    for (const auto& [begin, end] : CommittedRanges(address, bytes.size())) {
        if (std::memcmp(reinterpret_cast<const void*>(begin), bytes.data() + (begin - address), static_cast<std::size_t>(end - begin)) != 0) return false;
    }
    return true;
}

Compare CompareMapped(std::uint64_t address, std::span<const std::byte> bytes) {
    if (bytes.empty()) return Compare::Equal;
    if (address == 0 || bytes.size() > std::numeric_limits<std::uintptr_t>::max() - address) return Compare::Unmapped;
    auto outcome = Compare::Equal;

    const bool queried = describePages(static_cast<std::uintptr_t>(address), bytes.size(), [&](const PageRun& run) {
        if (!run.readable) outcome = Compare::Unmapped;
        else if (std::memcmp(reinterpret_cast<const void*>(run.begin), bytes.data() + (run.begin - address), static_cast<std::size_t>(run.end - run.begin)) != 0) outcome = Compare::Differs;
        return outcome == Compare::Equal;
    });
    return queried ? outcome : Compare::Unmapped;
}

Compare CopyMapped(std::uint64_t address, std::span<std::byte> out) {
    if (out.empty()) return Compare::Equal;
    if (address == 0 || out.size() > std::numeric_limits<std::uintptr_t>::max() - address) return Compare::Unmapped;
    auto outcome = Compare::Equal;
    const bool queried = describePages(static_cast<std::uintptr_t>(address), out.size(), [&](const PageRun& run) {
        if (!run.readable) outcome = Compare::Unmapped;
        else std::memcpy(out.data() + (run.begin - address), reinterpret_cast<const void*>(run.begin), static_cast<std::size_t>(run.end - run.begin));
        return outcome == Compare::Equal;
    });
    return queried ? outcome : Compare::Unmapped;
}

void WriteChangedCommitted(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original) {
    require(current.size() == original.size(), "write-back snapshot sizes differ");
    if (Accessible(reinterpret_cast<const void*>(address), current.size(), true)) {
        WriteChanged(address, current, original);
        return;
    }
    for (const auto& [begin, end] : CommittedRanges(address, current.size(), true)) {
        const auto offset = static_cast<std::size_t>(begin - address);
        const auto length = static_cast<std::size_t>(end - begin);
        WriteChanged(begin, current.subspan(offset, length), original.subspan(offset, length));
    }
}

void WriteChanged(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original) {
    require(current.size() == original.size(), "write-back snapshot sizes differ");
    if (current.empty()) return;

    const ReadSiteScope site(ReadSite::Store);
    FlushGpuWrites(address, current.size());
    const TimedAccess timed(CounterChangedWrite, current.size());
    auto* destination = reinterpret_cast<std::byte*>(address);
    CheckRange(destination, current.size(), 1, true);
    constexpr std::size_t block = 256;
    const auto size = current.size();
    const auto differs = [&](std::size_t at) {
        const auto length = std::min(block, size - at);
        return std::memcmp(current.data() + at, original.data() + at, length) != 0;
    };
    std::size_t firstChanged = size;
    std::size_t lastChanged = 0;
    for (std::size_t at = 0; at < size; at += block) {
        if (!differs(at)) continue;
        const auto blockEnd = std::min(at + block, size);
        for (std::size_t run = at; run < blockEnd;) {
            if (current[run] == original[run]) { ++run; continue; }
            auto runEnd = run + 1;
            while (runEnd < blockEnd && current[runEnd] != original[runEnd]) ++runEnd;
            std::memcpy(destination + run, current.data() + run, runEnd - run);
            firstChanged = std::min(firstChanged, run);
            lastChanged = std::max(lastChanged, runEnd);
            run = runEnd;
        }
    }

    if (firstChanged < lastChanged) MarkWritten(address + firstChanged, lastChanged - firstChanged);
}

void Write(std::uint64_t address, std::span<const std::byte> source, std::size_t alignment) {
    if (source.empty()) return;

    const ReadSiteScope site(ReadSite::Store);
    FlushGpuWrites(address, source.size());
    const TimedAccess timed(CounterWrite, source.size());
    CountWriteCaller(__builtin_return_address(0));
    auto* destination = reinterpret_cast<void*>(address);
    CheckRange(destination, source.size(), alignment, true);
    std::memcpy(destination, source.data(), source.size());

    MarkWritten(address, source.size());
}

}

extern "C" void AgcDriverCheckGuestMemory_nid_postfix(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable) {
    AgcDriver::GuestMemory::CheckRange(pointer, bytes, alignment, writable);
}

namespace AgcDriver::GuestMemory {
namespace {
thread_local std::uint64_t threadTraceFrame = 0;
}

void BeginCollectFrame() {
    collectFrameCounter.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t ThreadCollectEpoch() {
    return threadCollectEpoch;
}

std::uint64_t CurrentCollectFrame() {
    return collectFrameCounter.load(std::memory_order_relaxed);
}

void SetTraceFrame(std::uint64_t frame) {
    threadTraceFrame = frame;
}

std::uint64_t TraceFrame() {
    return threadTraceFrame;
}
}
