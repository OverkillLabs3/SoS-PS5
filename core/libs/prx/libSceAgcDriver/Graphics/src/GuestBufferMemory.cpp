#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/common/StderrLog.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BdaResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <atomic>
#include <bit>
#include <condition_variable>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <stop_token>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace AgcDriver::Graphics {

struct ImageMirror {
    std::uint64_t base = 0;
    std::uint64_t bytes = 0;
    bool writable = false;
    bool heap = false;
    std::vector<std::uint64_t> generations;
    std::weak_ptr<const GuestAllocations::Range> range;
    std::shared_ptr<Buffer> buffer;

    std::vector<std::byte> shadow;

    std::uint64_t serial = 0;
};

struct GuestBufferMemory::AddressSpace {
    std::uint64_t generation = 0;
    std::uint64_t importsEpoch = 0;
    VkDevice device = VK_NULL_HANDLE;

    std::uint64_t serial = 0;
    GuestAllocations::Lease lease;
    std::vector<Region> base;
    std::vector<CopiedRange> copied;

    std::vector<ShaderRecompiler::BdaAbi::Range> ranges;
};

namespace {

struct AddressSpaceCache {
    std::atomic<std::shared_ptr<const GuestBufferMemory::AddressSpace>> current;
    std::atomic<std::uint64_t> serials{0};

    std::atomic<bool> droppedByWaiter{false};
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> rebuiltFirst{0};
    std::atomic<std::uint64_t> rebuiltGeneration{0};
    std::atomic<std::uint64_t> rebuiltEpoch{0};
    std::atomic<std::uint64_t> rebuiltDevice{0};
    std::atomic<std::uint64_t> rebuiltWaiterDrop{0};
    std::atomic<std::uint64_t> unpublished{0};
    std::atomic<std::uint64_t> dissolvedOverlap{0};
    std::atomic<std::uint64_t> dissolvedImports{0};
    std::atomic<std::uint64_t> waiterDrops{0};
};

AddressSpaceCache& Spaces() {
    static AddressSpaceCache cache;
    return cache;
}

bool addressSpaceCacheEnabled() {
    static const bool disabled = std::getenv("APS5_NO_ADDRESS_SPACE_CACHE") != nullptr;
    return !disabled;
}

struct HostImports {
    std::mutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    std::map<std::uint64_t, HostImport> imports;
    std::set<std::uint64_t> failed;

    std::uint64_t refreshedGeneration = 0;

    std::uint64_t epoch = 1;
    VkDevice watchDevice = VK_NULL_HANDLE;
    bool unwatchImports = false;
};

HostImports& Imports() {
    static HostImports imports;
    return imports;
}

void destroyImport(const Context& context, const HostImport& entry) {
    context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, entry.buffer, nullptr);
    context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, entry.memory, nullptr);
#ifdef _WIN32
    GuestArena::GuestArenaUnmapAlias_nid_postfix(entry.alias);
#endif
}

struct RetiredImport {
    RetiredImport(const Context& context, const HostImport& entry) : context(context), entry(entry) {}
    RetiredImport(const RetiredImport&) = delete;
    RetiredImport& operator=(const RetiredImport&) = delete;
    ~RetiredImport() { destroyImport(context, entry); }
    Context context;
    HostImport entry;
};

const GuestAllocations::Range* containingRange(const GuestAllocations::Lease& lease, std::uint64_t begin, std::uint64_t end);

void retireImport(const Context& context, HostImports& state, std::map<std::uint64_t, HostImport>::iterator it, const GuestAllocations::Lease& lease) {

    RetireShadow(context, it->second, [&lease](std::uint64_t begin, std::uint64_t end) { return containingRange(lease, begin, end) != nullptr; });
    auto holder = std::make_shared<RetiredImport>(context, it->second);
    if (auto* recorder = Recorder::Active(); recorder != nullptr && !recorder->Idle()) recorder->Keep(std::move(holder));
    ++state.epoch;
    state.imports.erase(it);
}

const char* createImport(const Context& context, HostImport& entry, VkResult& failure) {
    const auto bytes = entry.bytes;
    void* const host = entry.alias != nullptr ? entry.alias : reinterpret_cast<void*>(entry.base);
    const auto failed = [&](const char* step, VkResult result) -> const char* {
        if (entry.buffer) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, entry.buffer, nullptr);
        if (entry.memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, entry.memory, nullptr);
        entry.buffer = VK_NULL_HANDLE;
        entry.memory = VK_NULL_HANDLE;
        failure = result;
        return step;
    };
    const VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
    info.size = bytes;

    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (const auto result = context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &entry.buffer); result != VK_SUCCESS) return failed("vkCreateBuffer", result);
    VkMemoryHostPointerPropertiesEXT pointer{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (const auto result = context.Function<PFN_vkGetMemoryHostPointerPropertiesEXT>("vkGetMemoryHostPointerPropertiesEXT")(context.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &pointer); result != VK_SUCCESS) return failed("vkGetMemoryHostPointerPropertiesEXT", result);
    VkMemoryRequirements requirements{};
    context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, entry.buffer, &requirements);
    const auto types = requirements.memoryTypeBits & pointer.memoryTypeBits;
    if (types == 0) return failed("memory type selection", VK_ERROR_FORMAT_NOT_SUPPORTED);
    const VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host};
    const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &import, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
    allocation.allocationSize = bytes;
    allocation.memoryTypeIndex = static_cast<std::uint32_t>(std::countr_zero(types));
    if (const auto result = context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &entry.memory); result != VK_SUCCESS) return failed("vkAllocateMemory", result);
    if (const auto result = context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, entry.buffer, entry.memory, 0); result != VK_SUCCESS) return failed("vkBindBufferMemory", result);
    const VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, entry.buffer};
    entry.address = context.Function<PFN_vkGetBufferDeviceAddressKHR>("vkGetBufferDeviceAddressKHR")(context.device, &addressInfo);
    if (entry.address == 0) return failed("vkGetBufferDeviceAddressKHR", VK_ERROR_UNKNOWN);
    return nullptr;
}

#ifndef _WIN32
enum class ImportWatchRequest : std::uint8_t { Probe, Watch, Unwatch };

ImportWatchRequest importWatchRequest() {
    static const auto request = [] {
        const char* value = std::getenv("APS5_WRITE_WATCH_IMPORTS");
        if (value == nullptr || std::strcmp(value, "probe") == 0) return ImportWatchRequest::Probe;
        if (std::strcmp(value, "watch") == 0) return ImportWatchRequest::Watch;
        if (std::strcmp(value, "unwatch") == 0) return ImportWatchRequest::Unwatch;
        throw std::runtime_error(std::string("APS5_WRITE_WATCH_IMPORTS=") + value + ": expected probe, watch or unwatch");
    }();
    return request;
}
#endif

void decideImportWatch(const Context& context, HostImports& state) {
    if (state.watchDevice == context.device) return;
#ifdef _WIN32
    state.watchDevice = context.device;
    state.unwatchImports = false;
#else
    const auto request = importWatchRequest();
    state.watchDevice = context.device;
    state.unwatchImports = false;
    if (context.hostImportAlignment == 0 || !GuestMemory::WriteWatched()) return;
    if (request == ImportWatchRequest::Watch) {
        std::fprintf(stderr, "[write-watch] host imports stay watched (APS5_WRITE_WATCH_IMPORTS=watch)\n");
        return;
    }
    if (request == ImportWatchRequest::Unwatch) {
        state.unwatchImports = true;
        std::fprintf(stderr, "[write-watch] host imports are compared, not watched (APS5_WRITE_WATCH_IMPORTS=unwatch)\n");
        return;
    }
    const auto probe = ProbeImportWriteProtection(context);
    if (probe.failure != nullptr) {
        state.unwatchImports = true;
        std::fprintf(stderr, "[write-watch] host imports resolve write protection: unknown (probe failed at %s, %d); imported ranges are compared\n", probe.failure, static_cast<int>(probe.result));
        return;
    }
    state.unwatchImports = probe.writtenAfterSubmit != 0;
    std::fprintf(stderr, "[write-watch] host imports resolve write protection: %s (%u of %u scratch pages written after a GPU read, %u after the import); imported ranges %s\n", state.unwatchImports ? "yes" : "no", probe.writtenAfterSubmit, probe.pages, probe.writtenAtImport, state.unwatchImports ? "are compared" : "stay watched");
#endif
}

const HostImport* importAllocation(const Context& context, HostImports& state, std::uint64_t base, std::uint64_t bytes, const GuestAllocations::Lease& lease) {
    if (const auto found = state.imports.find(base); found != state.imports.end()) {
        if (found->second.bytes == bytes) return &found->second;
        retireImport(context, state, found, lease);
    }
    const auto alignment = context.hostImportAlignment;
    if (alignment == 0 || base % alignment != 0 || bytes % alignment != 0 || state.failed.contains(base)) return nullptr;

    static const std::uint64_t budget = [] {
        const char* value = std::getenv("APS5_HOST_IMPORT_MIB");
        return (value ? std::strtoull(value, nullptr, 10) : 6144ull) << 20u;
    }();
    std::uint64_t live = 0;
    for (const auto& [address, existing] : state.imports) live += existing.bytes;
    if (live + bytes > budget) {

        static std::uint64_t refused = 0, refusedBytes = 0;
        static auto lastReport = std::chrono::steady_clock::now() - std::chrono::seconds(60);
        ++refused;
        refusedBytes += bytes;
        if (std::chrono::steady_clock::now() - lastReport > std::chrono::seconds(10)) {
            lastReport = std::chrono::steady_clock::now();
            aps5::LogErr( "[gpu] host import budget: %llu MiB live of %llu MiB (APS5_HOST_IMPORT_MIB); %llu imports (%llu MiB) refused so far, last 0x%llx+0x%llx\n", static_cast<unsigned long long>(live >> 20u), static_cast<unsigned long long>(budget >> 20u), static_cast<unsigned long long>(refused), static_cast<unsigned long long>(refusedBytes >> 20u), static_cast<unsigned long long>(base), static_cast<unsigned long long>(bytes));
        }
        return nullptr;
    }
    HostImport entry{base, bytes, VK_NULL_HANDLE, VK_NULL_HANDLE, 0};
#ifdef _WIN32

    bool writable = true;
    bool readOnly = true;
    MEMORY_BASIC_INFORMATION refused{};
    for (std::uint64_t cursor = base; cursor < base + bytes;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            state.failed.insert(base);
            return nullptr;
        }
        const auto protection = info.Protect & 0xffu;
        const bool pageWritable = (info.Type == MEM_PRIVATE || info.Type == MEM_IMAGE) && (protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_WRITECOPY);
        const bool pageReadOnly = info.Type != MEM_MAPPED && (protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ);
        if (!pageReadOnly) readOnly = false;
        if (info.Type != MEM_MAPPED && !pageWritable && !pageReadOnly && refused.BaseAddress == nullptr) refused = info;
        if (!pageWritable) writable = false;
        cursor = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    }
    if (!writable && readOnly) {
        state.failed.insert(base);
        return nullptr;
    }
    if (!writable) {
        if (refused.BaseAddress != nullptr) {
            char text[256];
            std::snprintf(text, sizeof(text), "AGC graphics: host import of 0x%llx+0x%llx: memory at 0x%llx (type 0x%lx, protection 0x%lx) is neither read-write, read-only nor a shared mapping", static_cast<unsigned long long>(base), static_cast<unsigned long long>(bytes), reinterpret_cast<unsigned long long>(refused.BaseAddress), refused.Type, refused.Protect);
            throw std::runtime_error(text);
        }
        entry.alias = GuestArena::GuestArenaMapAlias_nid_postfix(static_cast<std::uintptr_t>(base), static_cast<std::size_t>(bytes));
    }
#endif
    decideImportWatch(context, state);
    if (state.unwatchImports) GuestMemory::Unwatch(base, bytes);
    entry.unwatched = state.unwatchImports;
    VkResult result = VK_SUCCESS;
    const char* step = nullptr;
    GuestMemory::ImportWatched(base, bytes, [&] {
        step = createImport(context, entry, result);
        return step == nullptr;
    });
    if (step != nullptr) {
#ifdef _WIN32
        GuestArena::GuestArenaUnmapAlias_nid_postfix(entry.alias);
#endif
        state.failed.insert(base);
        aps5::LogErr( "[gpu] host import of 0x%llx+0x%llx failed at %s (%d); falling back to copies\n", static_cast<unsigned long long>(base), static_cast<unsigned long long>(bytes), step, static_cast<int>(result));
#ifdef _WIN32
        static const bool trace = std::getenv("APS5_TRACE_HOST_IMPORT") != nullptr;
        for (std::uint64_t cursor = base; trace && cursor < base + bytes;) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) break;
            const auto regionEnd = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
            aps5::LogErr( "[gpu]   0x%llx+0x%llx state 0x%lx protect 0x%lx type 0x%lx allocation 0x%llx\n", static_cast<unsigned long long>(cursor), static_cast<unsigned long long>(std::min(regionEnd, base + bytes) - cursor), info.State, info.Protect, info.Type, reinterpret_cast<unsigned long long>(info.AllocationBase));
            cursor = regionEnd;
        }
#endif
        return nullptr;
    }
    static std::uint64_t importedBytes = 0;
    importedBytes += bytes;
    static const bool trace = std::getenv("APS5_TRACE_HOST_IMPORT") != nullptr;
    std::uint64_t liveBytes = bytes;
    for (const auto& [address, existing] : state.imports) liveBytes += existing.bytes;
    if (trace) aps5::LogErr( "[gpu] host import of 0x%llx+0x%llx ok (%zu live, %.1f MiB live, %.1f MiB ever)\n", static_cast<unsigned long long>(base), static_cast<unsigned long long>(bytes), state.imports.size() + 1, liveBytes / 1048576.0, importedBytes / 1048576.0);
    return &state.imports.emplace(base, entry).first->second;
}

const GuestAllocations::Range* leasedRangeAt(const GuestAllocations::Lease& lease, std::uint64_t base) {
    const auto found = std::lower_bound(lease.begin(), lease.end(), base, [](const auto& range, std::uint64_t value) { return range->address < value; });
    return found != lease.end() && (*found)->address == base ? found->get() : nullptr;
}

const GuestAllocations::Range* containingRange(const GuestAllocations::Lease& lease, std::uint64_t begin, std::uint64_t end) {
    const auto found = std::upper_bound(lease.begin(), lease.end(), begin, [](std::uint64_t value, const auto& range) { return value < range->address; });
    if (found == lease.begin()) return nullptr;
    const auto& range = *std::prev(found);
    return range->readable && begin >= range->address && end <= range->address + range->bytes ? range.get() : nullptr;
}

void refreshImports(const Context& context, HostImports& state, const GuestAllocations::Lease& lease) {
    if (state.device != context.device) {
        for (const auto& [address, entry] : state.imports) {
            if (state.device != VK_NULL_HANDLE && state.destroyBuffer != nullptr && state.freeMemory != nullptr) {
                state.destroyBuffer(state.device, entry.buffer, nullptr);
                state.freeMemory(state.device, entry.memory, nullptr);
            }
#ifdef _WIN32
            GuestArena::GuestArenaUnmapAlias_nid_postfix(entry.alias);
#endif
        }
        state.imports.clear();
        state.failed.clear();
        state.device = context.device;
        state.destroyBuffer = context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer");
        state.freeMemory = context.Function<PFN_vkFreeMemory>("vkFreeMemory");
        state.refreshedGeneration = 0;
        ++state.epoch;
    }
    const auto generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    if (generation == state.refreshedGeneration) return;
    state.refreshedGeneration = generation;
    for (auto it = state.imports.begin(); it != state.imports.end();) {
        const auto* range = leasedRangeAt(lease, it->first);
        if (range != nullptr && range->bytes == it->second.bytes) {
            if (it->second.unwatched) GuestMemory::Unwatch(it->first, it->second.bytes);
            ++it;
            continue;
        }
        const auto next = std::next(it);
        retireImport(context, state, it, lease);
        it = next;
    }
    for (auto it = state.failed.begin(); it != state.failed.end();) it = leasedRangeAt(lease, *it) != nullptr ? std::next(it) : state.failed.erase(it);
}

const HostImport* findImport(HostImports& state, std::uint64_t begin, std::uint64_t end) {
    auto found = state.imports.upper_bound(begin);
    if (found == state.imports.begin()) return nullptr;
    --found;
    const auto& entry = found->second;
    return begin >= entry.base && end <= entry.base + entry.bytes ? &entry : nullptr;
}

bool importsStale(const Context& context, const HostImports& state) {
    return state.device != context.device || GuestAllocations::GuestAllocationsGeneration_nid_postfix() != state.refreshedGeneration;
}

struct ImageMirrors {
    std::mutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    std::map<std::uint64_t, std::shared_ptr<ImageMirror>> entries;
    std::set<std::uint64_t> failed;
    std::uint64_t heapBytes = 0;

    std::uint64_t builds = 0;
    std::uint64_t heapRefills = 0;
    std::uint64_t heapUnwatched = 0;
    std::uint64_t subranges = 0;
    std::uint64_t rebuilds = 0;
    std::uint64_t refreshes = 0;
    std::uint64_t blocksCompared = 0;
    std::uint64_t blocksCopied = 0;

    std::uint64_t syncs = 0;
    std::uint64_t serials = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

struct AddressBuildTiming {
    double leaseUs = 0;
    double importsUs = 0;
    double mirrorsUs = 0;
    double compareUs = 0;
    std::uint64_t blocksCompared = 0;
    std::uint64_t blocksCopied = 0;
};

AddressBuildTiming& ThreadAddressTiming() {
    thread_local AddressBuildTiming timing;
    return timing;
}

struct AddressBuildTotals {
    std::mutex mutex;
    std::uint64_t builds = 0;
    AddressBuildTiming sums;
    double snapshotsUs = 0;
    BdaResources::TableCacheStats tableSeen;
    AddressSpaceStats spaceSeen;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

AddressBuildTotals& AddressBuilds() {
    static AddressBuildTotals totals;
    return totals;
}

ImageMirrors& Mirrors() {
    static ImageMirrors mirrors;
    return mirrors;
}

bool mirrorsEnabled() {
    static const bool disabled = std::getenv("APS5_NO_LEASE_MIRROR") != nullptr;
    return !disabled;
}

std::uint64_t heapMirrorBudget() {
    static const std::uint64_t bytes = [] {
        const char* value = std::getenv("APS5_HEAP_MIRROR_MIB");
        return (value ? std::strtoull(value, nullptr, 10) : 24576ull) << 20u;
    }();
    return bytes;
}

[[noreturn]] void heapMirrorFatal(const GuestAllocations::Range& range, std::uint64_t held, const char* reason) {
    aps5::LogErr( "FATAL: heap mirror of 0x%llx+0x%llx: %s; heap mirrors hold %llu MiB of %llu MiB (APS5_HEAP_MIRROR_MIB)\n", static_cast<unsigned long long>(range.address), static_cast<unsigned long long>(range.bytes), reason, static_cast<unsigned long long>(held >> 20u), static_cast<unsigned long long>(heapMirrorBudget() >> 20u));
    aps5::LogFlush(aps5::LogStdErr);
    std::abort();
}

bool sameRange(const std::weak_ptr<const GuestAllocations::Range>& mirrored, const std::shared_ptr<const GuestAllocations::Range>& range) {
    return !mirrored.owner_before(range) && !range.owner_before(mirrored);
}

struct RefreshBlock {
    ImageMirror* mirror;
    std::uint64_t address;
    std::size_t length;
    std::uint64_t generation = 0;
};

bool compareBlock(const RefreshBlock& block) {
    const auto offset = static_cast<std::size_t>(block.address - block.mirror->base);
    const auto* guest = reinterpret_cast<const std::byte*>(block.address);
    if (block.mirror->heap) {
        std::memcpy(block.mirror->buffer->Bytes().data() + offset, guest, block.length);
        block.mirror->generations[static_cast<std::size_t>(block.address / 65536 - block.mirror->base / 65536)] = block.generation;
        return true;
    }
    if (std::memcmp(block.mirror->shadow.data() + offset, guest, block.length) == 0) return false;
    std::memcpy(block.mirror->buffer->Bytes().data() + offset, guest, block.length);
    std::memcpy(block.mirror->shadow.data() + offset, guest, block.length);
    return true;
}

class RefreshPool {
public:
    static RefreshPool& Get() {
        if (instance == nullptr) instance = new RefreshPool;
        return *instance;
    }

    static void Shutdown() {
        delete std::exchange(instance, nullptr);
    }

    std::uint64_t Run(std::span<const RefreshBlock> blocks) {

        if (helpers == 0 || blocks.size() < 8) {
            std::uint64_t copied = 0;
            for (const auto& block : blocks) copied += compareBlock(block) ? 1 : 0;
            return copied;
        }

        std::lock_guard running(runMutex);
        {
            std::lock_guard lock(mutex);
            job = blocks;
            next.store(0, std::memory_order_relaxed);
            copiedBlocks.store(0, std::memory_order_relaxed);
            finished = 0;
            ++serial;
        }
        wake.notify_all();
        work(blocks);
        std::unique_lock lock(mutex);
        done.wait(lock, [&] { return finished == helpers; });
        job = {};
        return copiedBlocks.load(std::memory_order_relaxed);
    }

private:
    RefreshPool() {
        static const bool disabled = std::getenv("APS5_NO_PARALLEL_MIRROR_REFRESH") != nullptr;
        const char* text = std::getenv("APS5_MIRROR_REFRESH_THREADS");
        const auto requested = text != nullptr ? std::atoi(text) : 3;
        const auto wanted = disabled ? 0u : static_cast<unsigned>(std::clamp(requested, 0, 16));
        for (unsigned i = 0; i < wanted; ++i) {
            threads.emplace_back([this](std::stop_token token) {
                CpuTopology::PinHelperThread("mirror refresh");
                helper(token);
            });
            ++helpers;
        }
    }

    void work(std::span<const RefreshBlock> blocks) {
        std::uint64_t copied = 0;
        for (auto index = next.fetch_add(1, std::memory_order_relaxed); index < blocks.size(); index = next.fetch_add(1, std::memory_order_relaxed)) copied += compareBlock(blocks[index]) ? 1 : 0;
        copiedBlocks.fetch_add(copied, std::memory_order_relaxed);
    }

    void helper(std::stop_token token) {
        std::uint64_t seen = 0;
        for (;;) {
            std::span<const RefreshBlock> blocks;
            {
                std::unique_lock lock(mutex);
                if (!wake.wait(lock, token, [&] { return serial != seen; })) return;
                seen = serial;
                blocks = job;
            }
            work(blocks);
            {
                std::lock_guard lock(mutex);
                ++finished;
            }
            done.notify_all();
        }
    }

    inline static RefreshPool* instance = nullptr;
    unsigned helpers = 0;
    std::mutex runMutex;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::condition_variable done;
    std::span<const RefreshBlock> job;
    std::uint64_t serial = 0;
    unsigned finished = 0;
    std::atomic<std::size_t> next{0};
    std::atomic<std::uint64_t> copiedBlocks{0};
    std::vector<std::jthread> threads;
};

void prepareRange(std::uint64_t address, std::uint64_t bytes) {

    Require(GuestMemory::Accessible(reinterpret_cast<const void*>(address), static_cast<std::size_t>(bytes)), "image mirror range became inaccessible");

    const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::MirrorRefresh);
    GuestMemory::FlushGpuWrites(address, static_cast<std::size_t>(bytes));
    auto& state = Mirrors();
    if (auto* recorder = Recorder::Active(); recorder != nullptr && recorder->PendingWriteOverlaps(address, static_cast<std::size_t>(bytes))) {
        Recorder::CountSync(4);
        ++state.syncs;
        recorder->Sync();
    }
    ++state.refreshes;
}

bool prepareRefresh(ImageMirror& mirror, std::uint64_t address, std::uint64_t bytes) {
    if ((!mirror.writable && !mirror.heap) || bytes == 0) return false;
    prepareRange(address, bytes);
    return true;
}

void appendBlocks(std::vector<RefreshBlock>& blocks, ImageMirror& mirror, std::uint64_t address, std::uint64_t bytes) {
    constexpr std::uint64_t block = 65536;
    const auto end = address + bytes;
    for (auto at = address; at < end;) {
        const auto next = std::min(end, (at / block + 1) * block);
        blocks.push_back({&mirror, at, static_cast<std::size_t>(next - at)});
        at = next;
    }
}

void compareBlocks(std::span<const RefreshBlock> blocks) {
    if (blocks.empty()) return;
    const auto copied = RefreshPool::Get().Run(blocks);
    auto& state = Mirrors();
    state.blocksCompared += blocks.size();
    state.blocksCopied += copied;
}

void refreshMirror(ImageMirror& mirror, std::uint64_t address, std::uint64_t bytes) {
    if (!prepareRefresh(mirror, address, bytes)) return;
    std::vector<RefreshBlock> blocks;
    appendBlocks(blocks, mirror, address, bytes);
    compareBlocks(blocks);
}

void refreshHeapMirrors(std::vector<ImageMirror*>& mirrors, std::vector<RefreshBlock>& blocks) {
    constexpr std::uint64_t block = 65536;
    std::sort(mirrors.begin(), mirrors.end(), [](const ImageMirror* left, const ImageMirror* right) { return left->base < right->base; });
    thread_local std::vector<std::uint8_t> changed;
    for (std::size_t first = 0; first < mirrors.size();) {
        auto last = first + 1;
        while (last < mirrors.size() && mirrors[last]->base == mirrors[last - 1]->base + mirrors[last - 1]->bytes) ++last;
        const auto begin = mirrors[first]->base;
        const auto bytes = mirrors[last - 1]->base + mirrors[last - 1]->bytes - begin;
        prepareRange(begin, bytes);
        const auto generation = GuestMemory::CollectWrites(begin, static_cast<std::size_t>(bytes));
        Require(generation != 0, "a heap mirror's range is no longer write-watched");
        for (auto index = first; index < last; ++index) {
            auto& mirror = *mirrors[index];
            changed.assign(mirror.generations.size(), 0);
            GuestMemory::ChangedBlocks(mirror.base, static_cast<std::size_t>(mirror.bytes), mirror.generations, changed);
            const auto aligned = mirror.base / block * block;
            const auto end = mirror.base + mirror.bytes;
            bool refilled = false;
            for (std::size_t at = 0; at < changed.size(); ++at) {
                if (changed[at] == 0) continue;
                const auto from = std::max(mirror.base, aligned + at * block);
                blocks.push_back({&mirror, from, static_cast<std::size_t>(std::min(end, aligned + (at + 1) * block) - from), generation});
                refilled = true;
            }
            if (refilled) ++Mirrors().heapRefills;
            {

                static const bool traceMirror = std::getenv("APS5_TRACE_VIDEO_MIRROR") != nullptr;
                static std::atomic<unsigned> traced{0};
                if (traceMirror && traced.fetch_add(1) < 800) {
                    unsigned count = 0;
                    for (const auto flag : changed) count += flag != 0;
                    aps5::LogErr("[vmirror] 0x%llx+0x%llx refreshed gen %llu changed %u of %zu blocks\n", static_cast<unsigned long long>(mirror.base), static_cast<unsigned long long>(mirror.bytes), static_cast<unsigned long long>(generation), count, changed.size());
                }
            }
        }
        first = last;
    }
}

std::shared_ptr<ImageMirror> acquireMirror(const Context& context, const std::shared_ptr<const GuestAllocations::Range>& range, std::vector<RefreshBlock>& blocks, bool heap) {
    auto& state = Mirrors();
    std::shared_ptr<ImageMirror> mirror;
    {
        std::lock_guard lock(state.mutex);
        if (state.device != context.device) {
            state.entries.clear();
            state.failed.clear();
            state.heapBytes = 0;
            state.device = context.device;
        }
        if (state.failed.contains(range->address)) return nullptr;
        const auto found = state.entries.find(range->address);
        if (found != state.entries.end() && sameRange(found->second->range, range) && found->second->bytes == range->bytes && found->second->writable == range->writable && found->second->heap == heap) mirror = found->second;
    }
    if (mirror != nullptr) {
        if (!heap && prepareRefresh(*mirror, range->address, range->bytes)) appendBlocks(blocks, *mirror, range->address, range->bytes);
        return mirror;
    }
    if (!GuestMemory::Accessible(reinterpret_cast<const void*>(range->address), range->bytes, range->writable && !heap)) return nullptr;
    mirror = std::make_shared<ImageMirror>();
    mirror->base = range->address;
    mirror->bytes = range->bytes;
    mirror->writable = range->writable;
    mirror->heap = heap;
    mirror->range = range;
    if (heap) {
        prepareRefresh(*mirror, range->address, range->bytes);
        const auto generation = GuestMemory::CollectWrites(range->address, range->bytes);
        constexpr std::uint64_t block = 65536;
        mirror->generations.assign(static_cast<std::size_t>(((range->address + range->bytes + block - 1) / block) - range->address / block), generation);
        std::lock_guard lock(state.mutex);
        if (generation == 0) {
            ++state.heapUnwatched;
            return nullptr;
        }
        std::uint64_t held = state.heapBytes;
        if (const auto found = state.entries.find(range->address); found != state.entries.end() && found->second->heap) held -= found->second->bytes;
        if (held + range->bytes > heapMirrorBudget()) heapMirrorFatal(*range, held, "past the budget");
    }
    try {
        mirror->buffer = std::make_shared<Buffer>(context, range->bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    } catch (const std::runtime_error& error) {
        if (heap) heapMirrorFatal(*range, state.heapBytes, error.what());
        aps5::LogErr( "[gpu] image mirror of 0x%llx+0x%llx failed: %s; falling back to copies\n", static_cast<unsigned long long>(range->address), static_cast<unsigned long long>(range->bytes), error.what());
        std::lock_guard lock(state.mutex);
        state.failed.insert(range->address);
        return nullptr;
    }

    {
        const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::MirrorRefresh);
        GuestMemory::Read(range->address, mirror->buffer->Bytes());
    }
    if (range->writable && !heap) mirror->shadow.assign(mirror->buffer->Bytes().begin(), mirror->buffer->Bytes().end());
    std::lock_guard lock(state.mutex);
    ++state.rebuilds;
    mirror->serial = (1ull << 63u) | ++state.serials;

    if (const auto found = state.entries.find(range->address); found != state.entries.end() && found->second->heap) state.heapBytes -= found->second->bytes;
    if (heap) state.heapBytes += mirror->bytes;
    state.entries[range->address] = mirror;
    return mirror;
}

std::shared_ptr<ImageMirror> findMirror(const Context& context, std::uint64_t begin, std::uint64_t end) {
    auto& state = Mirrors();
    std::lock_guard lock(state.mutex);
    if (state.device != context.device || state.entries.empty()) return nullptr;
    auto found = state.entries.upper_bound(begin);
    if (found == state.entries.begin()) return nullptr;
    --found;
    const auto& mirror = found->second;
    if (mirror->heap || begin < mirror->base || end > mirror->base + mirror->bytes || mirror->range.expired()) return nullptr;
    return mirror;
}

void sweepMirrors() {
    auto& state = Mirrors();
    std::lock_guard lock(state.mutex);
    for (auto it = state.entries.begin(); it != state.entries.end();) {
        if (!it->second->range.expired()) {
            ++it;
            continue;
        }
        if (it->second->heap) state.heapBytes -= it->second->bytes;
        it = state.entries.erase(it);
    }
}

void reportMirrors() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    auto& state = Mirrors();
    const auto now = std::chrono::steady_clock::now();
    if (now - state.lastReport < std::chrono::seconds(10)) return;
    state.lastReport = now;
    std::size_t count = 0;
    std::size_t writable = 0;
    std::size_t heaps = 0;
    std::uint64_t bytes = 0;
    std::uint64_t heapBytes = 0;
    {
        std::lock_guard lock(state.mutex);
        for (const auto& [base, mirror] : state.entries) {
            ++count;
            bytes += mirror->bytes;
            if (mirror->writable) ++writable;
            if (mirror->heap) ++heaps;
        }
        heapBytes = state.heapBytes;
    }
    aps5::LogErr( "[buffers] image mirrors: %zu ranges (%.1f MiB, %zu writable, %zu heap %.1f MiB), %llu address-based builds served, %llu descriptor sub-ranges bound, %llu rebuilds, %llu refreshes: %llu blocks compared, %llu copied, %llu refresh syncs; heap refills %llu, unwatched heaps %llu\n", count, bytes / 1048576.0, writable, heaps, heapBytes / 1048576.0, static_cast<unsigned long long>(state.builds), static_cast<unsigned long long>(state.subranges), static_cast<unsigned long long>(state.rebuilds), static_cast<unsigned long long>(state.refreshes), static_cast<unsigned long long>(state.blocksCompared), static_cast<unsigned long long>(state.blocksCopied), static_cast<unsigned long long>(state.syncs), static_cast<unsigned long long>(state.heapRefills), static_cast<unsigned long long>(state.heapUnwatched));
}

struct LeaseState {
    std::mutex mutex;
    LeaseStats stats;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();

    const Recorder* leaseRecorder = nullptr;
    std::uint64_t newestLeaseSerial = 0;
    std::uint64_t finishedLeaseSerial = 0;
};

LeaseState& Leases() {
    static LeaseState state;
    return state;
}

struct SnapshotStats {
    std::atomic<std::uint64_t> checked{0};
    std::atomic<std::uint64_t> skipped{0};
};

SnapshotStats& Snapshots() {
    static SnapshotStats stats;
    return stats;
}

bool WaitForLeases() noexcept {
    const auto start = std::chrono::steady_clock::now();
    bool synced = false;
    bool drained = false;

    if (auto dropped = Spaces().current.exchange(nullptr); dropped != nullptr) {
        Spaces().droppedByWaiter.store(true, std::memory_order_relaxed);
        Spaces().waiterDrops.fetch_add(1, std::memory_order_relaxed);
        dropped.reset();
        auto& state = Leases();
        std::lock_guard lock(state.mutex);
        ++state.stats.contentionWaits;
        ++state.stats.cacheDrops;
        state.stats.contentionMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return true;
    }
    if (GuestMemory::GpuMutex().HeldByThisThread()) {
        std::this_thread::yield();
    } else {
        try {
            std::lock_guard lock(GuestMemory::GpuMutex());
            auto& state = Leases();
            auto* recorder = Recorder::Active();
            const auto target = state.newestLeaseSerial;
            if (recorder != nullptr && recorder == state.leaseRecorder && target > state.finishedLeaseSerial) {

                Recorder::CountSync(4);
                recorder->Submit();
                recorder->FinishUpTo(target);
                state.finishedLeaseSerial = target;
                synced = true;
            } else if (recorder != nullptr && !recorder->Idle()) {
                Recorder::CountSync(4);
                recorder->Sync();
                synced = true;
                drained = true;
            } else {

                std::this_thread::yield();
            }
        } catch (const std::exception& error) {
            aps5::LogErr( "[gpu] lease wait failed: %s\n", error.what());
        }
    }
    auto& state = Leases();
    std::lock_guard lock(state.mutex);
    ++state.stats.contentionWaits;
    if (synced) ++state.stats.contentionSyncs;
    if (drained) ++state.stats.contentionDrains;
    state.stats.contentionMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return synced;
}

void ensurePinWaiter() {
    static const bool registered = [] {
        GuestAllocations::GuestAllocationsSetPinWaiter_nid_postfix(&WaitForLeases);
        return true;
    }();
    static_cast<void>(registered);
}

}

bool SyncLeaseWork() {
    static const bool sync = std::getenv("APS5_SYNC_LEASE_DISPATCH") != nullptr;
    return sync;
}

void CountLeaseOutcome(bool synced, std::uint64_t batchSerial) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& state = Leases();
    if (!synced) {

        GuestMemory::AssertGpuLockHeld("CountLeaseOutcome");
        if (const auto* recorder = Recorder::Active(); recorder != state.leaseRecorder) {
            state.leaseRecorder = recorder;
            state.newestLeaseSerial = 0;
            state.finishedLeaseSerial = 0;
        }
        state.newestLeaseSerial = std::max(state.newestLeaseSerial, batchSerial);
    }
    std::lock_guard lock(state.mutex);
    ++(synced ? state.stats.synced : state.stats.deferred);
    if (!profile) return;

    const auto now = std::chrono::steady_clock::now();
    if (now - state.lastReport < std::chrono::seconds(10)) return;
    state.lastReport = now;
    const auto& stats = state.stats;
    const auto table = BdaResources::TableCacheCounters();
    const auto& snapshots = Snapshots();
    aps5::LogErr( "[address-sync] leases: %llu released at completion, %llu synced at once; %llu pin-contention waits by guest threads (%llu finished the lease batch, %llu drained the recorder, %llu cache-only drops) %.1f s; BDA table cache %llu hits / %llu misses (%zu tables held); snapshot compares: %llu skipped (live-backed region), %llu made\n", static_cast<unsigned long long>(stats.deferred), static_cast<unsigned long long>(stats.synced), static_cast<unsigned long long>(stats.contentionWaits), static_cast<unsigned long long>(stats.contentionSyncs), static_cast<unsigned long long>(stats.contentionDrains), static_cast<unsigned long long>(stats.cacheDrops), stats.contentionMs / 1000, static_cast<unsigned long long>(table.hits), static_cast<unsigned long long>(table.misses), table.held, static_cast<unsigned long long>(snapshots.skipped.load(std::memory_order_relaxed)), static_cast<unsigned long long>(snapshots.checked.load(std::memory_order_relaxed)));
}

LeaseStats LeaseCounters() {
    auto& state = Leases();
    std::lock_guard lock(state.mutex);
    return state.stats;
}

AddressSpaceStats AddressSpaceCounters() {
    const auto& cache = Spaces();
    const auto load = [](const std::atomic<std::uint64_t>& counter) { return counter.load(std::memory_order_relaxed); };
    return {addressSpaceCacheEnabled(), load(cache.hits), load(cache.rebuiltFirst), load(cache.rebuiltGeneration), load(cache.rebuiltEpoch), load(cache.rebuiltDevice), load(cache.rebuiltWaiterDrop), load(cache.unpublished), load(cache.dissolvedOverlap), load(cache.dissolvedImports), load(cache.waiterDrops)};
}

std::string AddressCopyOverflow(std::vector<AddressCopy> copies, std::uint64_t limit) {
    std::uint64_t total = 0;
    for (const auto& copy : copies) total += copy.committed;
    if (total <= limit) return {};
    std::sort(copies.begin(), copies.end(), [](const AddressCopy& left, const AddressCopy& right) { return left.committed > right.committed; });
    char text[256];
    std::snprintf(text, sizeof(text), "an address-based build copies %llu MiB of %zu registered ranges, past %llu MiB (APS5_ADDRESS_COPY_MAX_MIB); the largest:", static_cast<unsigned long long>(total >> 20u), copies.size(), static_cast<unsigned long long>(limit >> 20u));
    std::string message = text;
    for (std::size_t index = 0; index < copies.size() && index < 8; ++index) {
        const auto& copy = copies[index];
        std::snprintf(text, sizeof(text), " 0x%llx+0x%llx (%.1f MiB committed, %s)", static_cast<unsigned long long>(copy.begin), static_cast<unsigned long long>(copy.end - copy.begin), copy.committed / 1048576.0, copy.reason);
        message += text;
    }
    return message;
}

MirrorStats MirrorCounters() {
    auto& state = Mirrors();
    std::lock_guard lock(state.mutex);
    MirrorStats stats{0, state.heapBytes, state.rebuilds, state.blocksCopied, state.heapRefills};
    for (const auto& [base, mirror] : state.entries) stats.heapMirrors += mirror->heap ? 1 : 0;
    return stats;
}

ImportProbe ProbeImportWriteProtection(const Context& context) {
    ImportProbe probe;
#ifdef _WIN32
    static_cast<void>(context);
    probe.failure = "the Linux write watch";
    return probe;
#else
    if (context.hostImportAlignment == 0) {
        probe.failure = "host import support";
        return probe;
    }
    if (!GuestWriteWatch::GuestWriteWatchAvailable_nid_postfix()) {
        probe.failure = "the write watch";
        return probe;
    }
    constexpr std::uint64_t page = 4096;
    const std::uint64_t alignment = std::max<std::uint64_t>(context.hostImportAlignment, page);
    const std::uint64_t bytes = (4 * page + alignment - 1) / alignment * alignment;
    probe.pages = static_cast<std::uint32_t>(bytes / page);
    void* raw = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        probe.failure = "mmap";
        return probe;
    }
    const auto base = (reinterpret_cast<std::uint64_t>(raw) + alignment - 1) / alignment * alignment;
    auto* scratch = reinterpret_cast<volatile std::uint8_t*>(base);
    for (std::uint64_t offset = 0; offset < bytes; offset += page) scratch[offset] = 1;
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(reinterpret_cast<const void*>(base), bytes);
    HostImport import{base, bytes, VK_NULL_HANDLE, VK_NULL_HANDLE, 0};
    VkBuffer destination = VK_NULL_HANDLE;
    VkDeviceMemory destinationMemory = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
    const auto collect = [&](std::uint32_t& pages) {
        pages = 0;
        return GuestWriteWatch::GuestWriteWatchCollect_nid_postfix(static_cast<std::uintptr_t>(base), static_cast<std::size_t>(bytes), [](void* count, std::uintptr_t begin, std::uintptr_t end) { *static_cast<std::uint32_t*>(count) += static_cast<std::uint32_t>((end - begin) / 4096); }, &pages);
    };
    const auto run = [&]() -> const char* {
        std::uint32_t quiet = 0;
        if (!collect(quiet)) return "the first collect";
        if (!collect(quiet)) return "the second collect";
        if (quiet != 0) return "an unwritten scratch range";
        if (const char* step = createImport(context, import, probe.result)) return step;
        if (!collect(probe.writtenAtImport)) return "the collect after the import";
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if ((probe.result = context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &destination)) != VK_SUCCESS) return "vkCreateBuffer";
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, destination, &requirements);
        if (requirements.memoryTypeBits == 0) return "the destination memory type";
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = static_cast<std::uint32_t>(std::countr_zero(requirements.memoryTypeBits));
        if ((probe.result = context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &destinationMemory)) != VK_SUCCESS) return "vkAllocateMemory";
        if ((probe.result = context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, destination, destinationMemory, 0)) != VK_SUCCESS) return "vkBindBufferMemory";
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = context.pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        if ((probe.result = context.Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(context.device, &allocate, &commands)) != VK_SUCCESS) return "vkAllocateCommandBuffers";
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if ((probe.result = context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin)) != VK_SUCCESS) return "vkBeginCommandBuffer";
        const VkBufferCopy region{0, 0, bytes};
        context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, import.buffer, destination, 1, &region);
        if ((probe.result = context.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands)) != VK_SUCCESS) return "vkEndCommandBuffer";
        const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if ((probe.result = context.Function<PFN_vkCreateFence>("vkCreateFence")(context.device, &fenceInfo, nullptr, &fence)) != VK_SUCCESS) return "vkCreateFence";
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        if ((probe.result = context.Function<PFN_vkQueueSubmit>("vkQueueSubmit")(context.queue, 1, &submit, fence)) != VK_SUCCESS) return "vkQueueSubmit";
        submitted = true;
        if ((probe.result = context.Function<PFN_vkWaitForFences>("vkWaitForFences")(context.device, 1, &fence, VK_TRUE, 10'000'000'000ull)) != VK_SUCCESS) return "vkWaitForFences";
        submitted = false;
        if (!collect(probe.writtenAfterSubmit)) return "the collect after the submission";
        return nullptr;
    };
    probe.failure = run();
    if (submitted) context.Function<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(context.queue);
    if (fence != VK_NULL_HANDLE) context.Function<PFN_vkDestroyFence>("vkDestroyFence")(context.device, fence, nullptr);
    if (commands != VK_NULL_HANDLE) context.Function<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(context.device, context.pool, 1, &commands);
    if (destination != VK_NULL_HANDLE) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, destination, nullptr);
    if (destinationMemory != VK_NULL_HANDLE) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, destinationMemory, nullptr);
    if (import.buffer != VK_NULL_HANDLE) destroyImport(context, import);
    GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(reinterpret_cast<const void*>(base), bytes);
    munmap(raw, bytes + alignment);
    return probe;
#endif
}

ImportWatch PrepareImportWatch(const Context& context) {
    auto& state = Imports();
    std::lock_guard lock(state.mutex);
    decideImportWatch(context, state);
    return state.unwatchImports ? ImportWatch::Unwatch : ImportWatch::Watch;
}

void SetImportWatch(const Context& context, ImportWatch watch) {
    auto& state = Imports();
    std::lock_guard lock(state.mutex);
    state.watchDevice = context.device;
    state.unwatchImports = watch == ImportWatch::Unwatch;
}

const HostImport* HostImportFor(const Context& context, std::uint64_t address, std::size_t bytes) {
    if (context.hostImportAlignment == 0 || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return nullptr;
    auto& state = Imports();
    std::lock_guard lock(state.mutex);

    if (!importsStale(context, state)) {
        if (const auto* entry = findImport(state, address, address + bytes)) return entry;
    }
    const auto lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
    refreshImports(context, state, lease);
    if (const auto* range = containingRange(lease, address, address + bytes)) return importAllocation(context, state, range->address, range->bytes, lease);
    return nullptr;
}

bool RegisteredReadableCovers(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return false;
    const auto lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
    return containingRange(lease, address, address + bytes) != nullptr;
}

bool HostImportCovers(const Context& context, std::uint64_t address, std::size_t bytes) {
    if (context.hostImportAlignment == 0 || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return false;
    auto& state = Imports();
    std::lock_guard lock(state.mutex);

    return state.device == context.device && findImport(state, address, address + bytes) != nullptr;
}

GuestBufferMemory::GuestBufferMemory(const Context& context) : context(context) {}

bool GuestBufferMemory::WritesOverlap(std::uint64_t address, std::size_t bytes) const {
    return std::any_of(writes.begin(), writes.end(), [&](const auto& range) { return address < range.second && range.first < address + bytes; });
}

void GuestBufferMemory::CountAddressBuild(double snapshotsUs) {
    auto& totals = AddressBuilds();
    auto& timing = ThreadAddressTiming();
    std::lock_guard lock(totals.mutex);
    ++totals.builds;
    totals.sums.leaseUs += timing.leaseUs;
    totals.sums.importsUs += timing.importsUs;
    totals.sums.mirrorsUs += timing.mirrorsUs;
    totals.sums.compareUs += timing.compareUs;
    totals.sums.blocksCompared += timing.blocksCompared;
    totals.sums.blocksCopied += timing.blocksCopied;
    totals.snapshotsUs += snapshotsUs;
    timing = {};
    const auto now = std::chrono::steady_clock::now();
    if (now - totals.lastReport < std::chrono::seconds(10)) return;
    totals.lastReport = now;
    const auto per = [&](double us) { return us / static_cast<double>(totals.builds); };
    const auto table = BdaResources::TableCacheCounters();
    const auto& seen = totals.tableSeen;
    const auto delta = [](std::uint64_t now, std::uint64_t before) { return static_cast<unsigned long long>(now - before); };
    const auto space = AddressSpaceCounters();
    const auto& spaceSeen = totals.spaceSeen;
    aps5::LogErr( "[address] %llu address-based builds (10 s), us per build: lease %.0f, imports pass %.0f, mirror prepare %.0f, compare %.0f (%.0f blocks, %.1f copied), snapshots %.0f; space hits %llu / rebuilds: generation %llu, epoch %llu, device %llu, waiter drop %llu, first %llu; unpublished %llu, dissolved: overlap %llu, imports %llu%s; [bda-table] hits %llu / misses %llu (space tables %llu), first entry: expired %llu, hash differs low %llu / heap %llu, same hash %llu, none held %llu\n", static_cast<unsigned long long>(totals.builds), per(totals.sums.leaseUs), per(totals.sums.importsUs), per(totals.sums.mirrorsUs), per(totals.sums.compareUs), per(static_cast<double>(totals.sums.blocksCompared)), per(static_cast<double>(totals.sums.blocksCopied)), per(totals.snapshotsUs), delta(space.hits, spaceSeen.hits), delta(space.rebuiltGeneration, spaceSeen.rebuiltGeneration), delta(space.rebuiltEpoch, spaceSeen.rebuiltEpoch), delta(space.rebuiltDevice, spaceSeen.rebuiltDevice), delta(space.rebuiltWaiterDrop, spaceSeen.rebuiltWaiterDrop), delta(space.rebuiltFirst, spaceSeen.rebuiltFirst), delta(space.unpublished, spaceSeen.unpublished), delta(space.dissolvedOverlap, spaceSeen.dissolvedOverlap), delta(space.dissolvedImports, spaceSeen.dissolvedImports), space.enabled ? "" : " (cache off)", delta(table.hits, seen.hits), delta(table.misses, seen.misses), delta(table.spaceTables, seen.spaceTables), delta(table.firstExpired, seen.firstExpired), delta(table.firstDiffersLow, seen.firstDiffersLow), delta(table.firstDiffersHeap, seen.firstDiffersHeap), delta(table.firstSameHash, seen.firstSameHash), delta(table.firstEmpty, seen.firstEmpty));
    totals.tableSeen = table;
    totals.spaceSeen = space;
    totals.builds = 0;
    totals.sums = {};
    totals.snapshotsUs = 0;
}

void GuestBufferMemory::AcquireRegistered() {
    Require(!uploaded && regions.empty() && lease.empty() && space == nullptr, "guest allocation lease must precede resource registration");
    ensurePinWaiter();
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& timing = ThreadAddressTiming();
    const auto lap = [&](double& into, std::chrono::steady_clock::time_point& from) {
        if (!profile) return;
        const auto now = std::chrono::steady_clock::now();
        into += std::chrono::duration<double, std::micro>(now - from).count();
        from = now;
    };
    auto at = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const bool importable = context.hostImportAlignment != 0;

    static const bool sortedLookup = std::getenv("APS5_NO_SORTED_SNAPSHOT_LOOKUP") == nullptr;
    std::vector<RefreshBlock> blocks;
    std::vector<ImageMirror*> heaps;
    bool mirrored = false;
    auto& spaces = Spaces();

    const auto generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    if (addressSpaceCacheEnabled()) {
        auto current = spaces.current.load();
        bool hit = false;
        {
            auto& state = Imports();
            std::lock_guard lock(state.mutex);
            if (current == nullptr) (spaces.droppedByWaiter.exchange(false, std::memory_order_relaxed) ? spaces.rebuiltWaiterDrop : spaces.rebuiltFirst).fetch_add(1, std::memory_order_relaxed);
            else if (current->device != context.device) spaces.rebuiltDevice.fetch_add(1, std::memory_order_relaxed);
            else if (current->generation != generation) spaces.rebuiltGeneration.fetch_add(1, std::memory_order_relaxed);
            else if (current->importsEpoch != state.epoch) spaces.rebuiltEpoch.fetch_add(1, std::memory_order_relaxed);
            else hit = true;
        }
        if (hit) {
            spaces.hits.fetch_add(1, std::memory_order_relaxed);
            space = std::move(current);
            lap(timing.leaseUs, at);
            for (const auto& range : space->copied) addCopiedRange(range);
            regionsSorted = sortedLookup;
            lap(timing.importsUs, at);

            for (const auto& region : space->base) {
                if (region.mirror == nullptr) continue;
                mirrored = true;
                if (region.mirror->heap) heaps.push_back(region.mirror.get());
                else if (region.mirror->writable && prepareRefresh(*region.mirror, region.begin, region.end - region.begin)) appendBlocks(blocks, *region.mirror, region.begin, region.end - region.begin);
            }
            lap(timing.mirrorsUs, at);
        }
    }
    if (space == nullptr) {

        lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
        lap(timing.leaseUs, at);
        {
            auto& state = Imports();
            std::lock_guard lock(state.mutex);
            if (importable) refreshImports(context, state, lease);

            importsEpoch = state.epoch;
        }
        lap(timing.importsUs, at);

        regions.reserve(lease.size());
        std::vector<AddressCopy> copies;
        for (const auto& range : lease) {
            if (!range->readable) continue;
            validate(range->address, range->bytes);
            Region region{range->address, range->address + range->bytes, range->writable, {}, nullptr};
            const HostImport* entry = nullptr;
            if (importable) {
                auto& state = Imports();
                std::lock_guard lock(state.mutex);
                entry = importAllocation(context, state, range->address, range->bytes, lease);
            }
            lap(timing.importsUs, at);
            if (entry != nullptr) {
                region.hostBacked = true;

                region.direct = entry;
                regions.push_back(std::move(region));
                continue;
            }
            if (mirrorsEnabled()) {
                region.mirror = acquireMirror(context, range, blocks, range->releasable);
                lap(timing.mirrorsUs, at);
                if (region.mirror != nullptr) {
                    mirrored = true;
                    if (region.mirror->heap) heaps.push_back(region.mirror.get());
                    regions.push_back(std::move(region));
                    continue;
                }
            }
            addCopiedRange({range->address, range->address + range->bytes, range->writable});
            const auto& copied = regions.back();
            std::uint64_t committed = copied.end - copied.begin;
            if (copied.sparse) {
                committed = 0;
                for (const auto& [first, last] : copied.backed) committed += last - first;
            }
            const char* reason = !mirrorsEnabled() ? "mirrors disabled by APS5_NO_LEASE_MIRROR" : copied.sparse ? "unreadable pages" : !range->releasable ? "image mirror refused" : "outside the write-watched arena";
            copies.push_back({copied.begin, copied.end, committed, reason});
        }
        regionsSorted = sortedLookup;
        static const std::uint64_t copyLimit = [] {
            const char* value = std::getenv("APS5_ADDRESS_COPY_MAX_MIB");
            return (value ? std::strtoull(value, nullptr, 10) : 64ull) << 20u;
        }();
        if (const auto overflow = AddressCopyOverflow(std::move(copies), copyLimit); !overflow.empty()) {
            aps5::LogErr( "FATAL: %s\n", overflow.c_str());
            aps5::LogFlush(aps5::LogStdErr);
            std::abort();
        }
        lap(timing.importsUs, at);
        if (addressSpaceCacheEnabled()) {

            std::vector<ShaderRecompiler::BdaAbi::Range> ranges;
            bool publish = true;
            {
                auto& state = Imports();
                std::lock_guard lock(state.mutex);
                if (state.epoch != importsEpoch) {
                    publish = false;
                    spaces.unpublished.fetch_add(1, std::memory_order_relaxed);
                } else {
                    for (const auto& region : regions) {
                        if (region.direct != nullptr || region.mirror != nullptr) ranges.push_back(addressRange(region));
                    }
                }
            }
            if (publish) {
                auto built = std::make_shared<AddressSpace>();
                built->generation = generation;
                built->importsEpoch = importsEpoch;
                built->device = context.device;
                built->serial = spaces.serials.fetch_add(1, std::memory_order_relaxed) + 1;
                built->lease = std::move(lease);
                built->ranges = std::move(ranges);
                std::vector<Region> extras;
                for (auto& region : regions) {
                    if (region.direct != nullptr || region.mirror != nullptr) {
                        built->base.push_back(std::move(region));
                    } else {
                        built->copied.push_back({region.begin, region.end, region.writable});
                        extras.push_back(std::move(region));
                    }
                }
                regions = std::move(extras);
                lease.clear();
                space = std::move(built);
                spaces.droppedByWaiter.store(false, std::memory_order_relaxed);
                spaces.current.store(space);
            }
        }
    }
    refreshHeapMirrors(heaps, blocks);
    lap(timing.mirrorsUs, at);
    const auto copiedBefore = Mirrors().blocksCopied;
    compareBlocks(blocks);
    lap(timing.compareUs, at);
    if (profile) {
        timing.blocksCompared += blocks.size();
        timing.blocksCopied += Mirrors().blocksCopied - copiedBefore;
    }
    if (mirrorsEnabled()) {
        if (mirrored) ++Mirrors().builds;
        sweepMirrors();
        reportMirrors();
    }
}

void GuestBufferMemory::validate(std::uint64_t address, std::size_t bytes) const {
    Require(!uploaded, "guest memory ownership is frozen for GPU execution");
    Require(address != 0 && bytes != 0, "empty guest memory range");
    Require(bytes <= std::numeric_limits<std::uint64_t>::max() - address, "guest memory range overflow");
}

void GuestBufferMemory::addCopiedRange(const CopiedRange& range) {
    validate(range.begin, range.end - range.begin);
    Region region{range.begin, range.end, range.writable, {}, nullptr};

    region.hostBacked = !range.writable;
    auto committed = GuestMemory::DescribeCommitted(range.begin, range.end - range.begin);
    if (!committed.whole) {
        region.sparse = true;
        region.backed = std::move(committed.ranges);
    }
    regions.push_back(std::move(region));
}

GuestBufferMemory::BaseOverlap GuestBufferMemory::baseOverlap(std::uint64_t begin, std::uint64_t end, const Region** owner) const {
    if (space == nullptr || space->base.empty()) return BaseOverlap::None;
    const auto& base = space->base;
    const auto found = std::upper_bound(base.begin(), base.end(), begin, [](std::uint64_t value, const Region& region) { return value < region.begin; });
    if (found != base.begin()) {
        const auto& previous = *std::prev(found);
        if (begin < previous.end) {
            if (end > previous.end) return BaseOverlap::Partial;
            if (owner != nullptr) *owner = &previous;
            return BaseOverlap::Inside;
        }
    }
    return found != base.end() && found->begin < end ? BaseOverlap::Partial : BaseOverlap::None;
}

void GuestBufferMemory::dissolveSpace(bool resolve) {
    Require(space != nullptr, "no address space to dissolve");
    std::vector<Region> merged;
    merged.reserve(space->base.size() + regions.size());
    if (prepared) {

        std::merge(space->base.begin(), space->base.end(), std::make_move_iterator(regions.begin()), std::make_move_iterator(regions.end()), std::back_inserter(merged), [](const Region& left, const Region& right) { return left.begin < right.begin; });
    } else {
        merged.assign(space->base.begin(), space->base.end());
        merged.insert(merged.end(), std::make_move_iterator(regions.begin()), std::make_move_iterator(regions.end()));
        regionsSorted = false;
    }
    if (resolve) {
        for (auto& region : merged) {
            if (region.direct != nullptr) region.pending = true;
        }
    }
    lease = space->lease;
    importsEpoch = space->importsEpoch;
    regions = std::move(merged);
    space.reset();
}

const GuestBufferMemory::Region* GuestBufferMemory::owner(std::uint64_t address) const {

    const auto candidate = [address](const std::vector<Region>& list) -> const Region* {
        const auto found = std::upper_bound(list.begin(), list.end(), address, [](std::uint64_t value, const Region& region) { return value < region.begin; });
        return found == list.begin() ? nullptr : &*std::prev(found);
    };
    const auto* own = candidate(regions);
    const auto* shared = space != nullptr ? candidate(space->base) : nullptr;
    if (own == nullptr || (shared != nullptr && shared->begin > own->begin)) return shared;
    return own;
}

void GuestBufferMemory::addDescriptorRegion(std::uint64_t address, std::size_t bytes, bool atomic) {
    validate(address, bytes);
    switch (baseOverlap(address, address + bytes, nullptr)) {
        case BaseOverlap::Inside:
            return;
        case BaseOverlap::Partial:
            Spaces().dissolvedOverlap.fetch_add(1, std::memory_order_relaxed);
            dissolveSpace(false);
            break;
        case BaseOverlap::None:
            break;
    }

    Region region{address, address + bytes, true, {}, nullptr};
    region.atomic = atomic;
    auto committed = GuestMemory::DescribeCommitted(address, bytes);
    if (!committed.whole) {

        region.sparse = true;
        region.backed = std::move(committed.ranges);
    }
    regions.push_back(std::move(region));
    regionsSorted = false;
}

void GuestBufferMemory::AddWritable(std::uint64_t address, std::size_t bytes, bool atomic) {
    addDescriptorRegion(address, bytes, atomic);
    writes.emplace_back(address, address + bytes);
}

void GuestBufferMemory::AddReadable(std::uint64_t address, std::size_t bytes) {

    addDescriptorRegion(address, bytes, false);
}

void GuestBufferMemory::AddSnapshot(const GuestMemorySnapshot& snapshot) {
    validate(snapshot.address, snapshot.bytes.size());
    const auto end = snapshot.address + snapshot.bytes.size();

    const Region* owner = nullptr;
    if (baseOverlap(snapshot.address, end, &owner) == BaseOverlap::Partial) {
        Spaces().dissolvedOverlap.fetch_add(1, std::memory_order_relaxed);
        dissolveSpace(false);
    }
    if (owner == nullptr && regionsSorted) {
        const auto found = std::upper_bound(regions.begin(), regions.end(), snapshot.address, [](std::uint64_t value, const Region& region) { return value < region.begin; });
        if (found != regions.begin() && end <= std::prev(found)->end) owner = &*std::prev(found);
    } else if (owner == nullptr) {
        for (const auto& region : regions) {
            if (region.begin <= snapshot.address && end <= region.end) {
                owner = &region;
                break;
            }
        }
    }
    if (owner != nullptr) {

        static const bool checkLive = std::getenv("APS5_NO_SNAPSHOT_CHECK") == nullptr;
        static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
        const bool live = owner->writable || owner->hostBacked || owner->mirror != nullptr;

        if (live && (!checkLive || AnyShadowedOverlaps(snapshot.address, snapshot.bytes.size()))) {
            if (profile) Snapshots().skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto offset = static_cast<std::size_t>(snapshot.address - owner->begin);
        const auto* source = live ? reinterpret_cast<const std::byte*>(snapshot.address) : owner->snapshot.data() + offset;
        Require(std::memcmp(source, snapshot.bytes.data(), snapshot.bytes.size()) == 0, "guest snapshot differs from registered memory");
        if (profile) Snapshots().checked.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    regions.push_back({snapshot.address, end, false, {snapshot.bytes.begin(), snapshot.bytes.end()}, nullptr});
    regionsSorted = false;
}

void GuestBufferMemory::Upload(bool addressable) {
    UploadPrepare(addressable);
    UploadFinish(addressable);
}

namespace {

std::atomic<std::uint64_t> importUs{0};
std::atomic<std::uint64_t> allocateUs{0};
std::atomic<std::uint64_t> readUs{0};
std::atomic<std::uint64_t> uploadsProfiled{0};

std::uint64_t microsecondsSince(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

bool gpuCopiesEnabled() {
    static const bool disabled = std::getenv("APS5_CPU_COPIES") != nullptr;
    return !disabled;
}

std::uint64_t gpuCopyLimit() {
    static const std::uint64_t limit = [] {
        const char* value = std::getenv("APS5_GPU_COPY_MAX_KIB");
        return (value != nullptr ? std::strtoull(value, nullptr, 10) : 1024ull) << 10u;
    }();
    return limit;
}

VkBufferUsageFlags gpuCopyUsage(bool addressable) {
    return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | (addressable ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0u);
}

bool writtenShadowEnabled() {
    static const bool disabled = std::getenv("APS5_NO_WRITTEN_SHADOW") != nullptr;
    return !disabled;
}

bool atomicStagingEnabled() {
    static const bool disabled = std::getenv("APS5_NO_ATOMIC_STAGING") != nullptr;
    return !disabled;
}

std::uint64_t kibSetting(const char* name, std::uint64_t defaultKib) {
    const char* value = std::getenv(name);
    return (value != nullptr ? std::strtoull(value, nullptr, 10) : defaultKib) << 10u;
}

std::uint64_t writtenShadowMin() {
    static const std::uint64_t bytes = kibSetting("APS5_WRITTEN_SHADOW_MIN_KIB", 16);
    return bytes;
}

std::uint64_t writtenShadowMax() {
    static const std::uint64_t bytes = kibSetting("APS5_WRITTEN_SHADOW_MAX_KIB", 2048);
    return bytes;
}

std::uint64_t atomicStageMax() {
    static const std::uint64_t bytes = kibSetting("APS5_ATOMIC_STAGE_MAX_KIB", 1024);
    return bytes;
}

VkMemoryPropertyFlags copyBufferProperties(bool deviceLocal) {
    return deviceLocal ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

constexpr auto StagingCopyInKey = Recorder::CommandClass::StagingIn;
constexpr auto StagingCopyBackKey = Recorder::CommandClass::StagingOut;

struct CopyStats {
    std::atomic<std::uint64_t> gpuCopies{0};
    std::atomic<std::uint64_t> gpuCopyBytes{0};
    std::atomic<std::uint64_t> gpuCopyBacks{0};
    std::atomic<std::uint64_t> gpuCopyBackBytes{0};

    std::atomic<std::uint64_t> stagingStores{0};

    std::atomic<std::uint64_t> staged{0};
    std::atomic<std::uint64_t> stagedAtomic{0};
    std::atomic<std::uint64_t> stagedReused{0};
    std::atomic<std::uint64_t> stagedInBytes{0};
    std::atomic<std::uint64_t> stagedOutBytes{0};
    std::atomic<std::uint64_t> stagedLost{0};
    std::atomic<std::uint64_t> stagingRefused{0};
    std::atomic<std::int64_t> lastStagingReport{0};
};

CopyStats& Copies() {
    static CopyStats stats;
    return stats;
}

std::shared_ptr<Buffer> stagingBuffer(const Context& context, std::size_t bytes, VkBufferUsageFlags usage) {
    try {
        return std::make_shared<Buffer>(context, bytes, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    } catch (const std::exception& error) {
        static std::atomic<int> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 4) aps5::LogErr( "[buffers] staging shadow of %zu bytes refused: %s\n", bytes, error.what());
        Copies().stagingRefused.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
}

void traceStaged(std::uint64_t begin, std::uint64_t end, bool atomic) {
    static const bool trace = std::getenv("APS5_TRACE_STAGING") != nullptr;
    if (!trace) return;
    static std::mutex mutex;
    static std::set<std::pair<std::uint64_t, std::uint64_t>> seen;
    std::lock_guard lock(mutex);
    if (!seen.insert({begin, end}).second) return;
    aps5::LogErr( "[staging] 0x%llx+0x%llx (%.1f KiB)%s\n", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin), (end - begin) / 1024.0, atomic ? " atomic" : "");
}

void reportStaging() {
    auto& stats = Copies();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = stats.lastStagingReport.load();
    if (nowMs - last < 10000 || !stats.lastStagingReport.compare_exchange_strong(last, nowMs)) return;

    static std::uint64_t lastStaged = 0, lastIn = 0, lastOut = 0;
    const auto staged = stats.staged.load();
    const auto in = stats.stagedInBytes.load();
    const auto out = stats.stagedOutBytes.load();
    aps5::LogErr( "[buffers] staging: %llu regions staged device-local (%llu with atomics, %llu of reused builds), %.0f KiB copied in, %.0f KiB copied back, %llu without copy-back, %llu shadows refused; last 10 s: %llu regions, %.0f KiB in, %.0f KiB back\n", static_cast<unsigned long long>(staged), static_cast<unsigned long long>(stats.stagedAtomic.load()), static_cast<unsigned long long>(stats.stagedReused.load()), in / 1024.0, out / 1024.0, static_cast<unsigned long long>(stats.stagedLost.load()), static_cast<unsigned long long>(stats.stagingRefused.load()), static_cast<unsigned long long>(staged - lastStaged), (in - lastIn) / 1024.0, (out - lastOut) / 1024.0);
    lastStaged = staged;
    lastIn = in;
    lastOut = out;
}

}

void ShutdownGuestBufferWorkers() {
    RefreshPool::Shutdown();
}

void ClearImageMirrors(VkDevice device) {
    auto& state = Mirrors();
    std::map<std::uint64_t, std::shared_ptr<ImageMirror>> entries;
    {
        std::lock_guard lock(state.mutex);
        if (state.device != device) return;
        entries.swap(state.entries);
        state.failed.clear();
        state.heapBytes = 0;
        state.device = VK_NULL_HANDLE;
    }
    Spaces().current.store(nullptr);
}

void ClearHostImports(VkDevice device) {
    auto& state = Imports();
    std::lock_guard lock(state.mutex);
    if (state.device != device) return;
    for (const auto& [address, entry] : state.imports) {
        state.destroyBuffer(state.device, entry.buffer, nullptr);
        state.freeMemory(state.device, entry.memory, nullptr);
#ifdef _WIN32
        GuestArena::GuestArenaUnmapAlias_nid_postfix(entry.alias);
#endif
    }
    state.imports.clear();
    state.failed.clear();
    state.device = VK_NULL_HANDLE;
    state.refreshedGeneration = 0;
    ++state.epoch;
}

GuestBufferMemory::~GuestBufferMemory() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;

    for (const auto& region : regions) {
        if (region.gpuCopy && region.deviceLocal && !region.copiedBack) Copies().stagedLost.fetch_add(1, std::memory_order_relaxed);
    }
}

bool GuestBufferMemory::gpuCopyEligible(const Region& region) const {
    if (!gpuCopiesEnabled() || region.sparse || !(region.hostBacked || region.writable)) return false;
    return region.end - region.begin <= gpuCopyLimit();
}

bool GuestBufferMemory::stagingEligible(const Region& region, bool addressable) const {
    if (!stagingAllowed || addressable || region.unstaged || !gpuCopiesEnabled() || region.sparse || region.mirror != nullptr) return false;
    const auto bytes = region.end - region.begin;
    if (!WritesOverlap(region.begin, static_cast<std::size_t>(bytes))) return false;
    if (region.atomic && atomicStagingEnabled() && bytes <= atomicStageMax()) return true;
    return writtenShadowEnabled() && bytes >= writtenShadowMin() && bytes <= writtenShadowMax();
}

void GuestBufferMemory::UploadPrepare(bool addressable) {
    Require(!prepared && !uploaded, "guest memory was already uploaded");
    prepared = true;
    std::sort(regions.begin(), regions.end(), [](const Region& left, const Region& right) { return left.begin < right.begin; });

    const auto mergeBacked = [](Region& into, const Region& from) {
        if (!into.sparse && !from.sparse) return;
        auto ranges = into.sparse ? into.backed : decltype(into.backed){{into.begin, into.end}};
        if (from.sparse) ranges.insert(ranges.end(), from.backed.begin(), from.backed.end());
        else ranges.emplace_back(from.begin, from.end);
        std::sort(ranges.begin(), ranges.end());
        into.backed.clear();
        for (const auto& range : ranges) {
            if (!into.backed.empty() && range.first <= into.backed.back().second) into.backed.back().second = std::max(into.backed.back().second, range.second);
            else into.backed.push_back(range);
        }
        const auto end = std::max(into.end, from.end);
        into.sparse = !(into.backed.size() == 1 && into.backed.front().first <= into.begin && into.backed.front().second >= end);
        if (!into.sparse) into.backed.clear();
    };
    std::vector<Region> merged;
    for (auto& region : regions) {
        if (!merged.empty() && region.begin < merged.back().end) {
            auto& previous = merged.back();
            if (previous.mirror != nullptr || region.mirror != nullptr) {

                if (previous.mirror == nullptr) std::swap(previous, region);
                Require(region.mirror == nullptr, "image mirrors overlap");
                if (region.begin >= previous.begin && region.end <= previous.end) continue;
                previous.mirror = nullptr;
                previous.hostBacked = true;
            }
            mergeBacked(previous, region);
            previous.atomic = previous.atomic || region.atomic;

            previous.begin = std::min(previous.begin, region.begin);
            if (previous.hostBacked || region.hostBacked) {

                previous.hostBacked = true;
                previous.writable = previous.writable || region.writable;
                previous.snapshot.clear();
                previous.end = std::max(previous.end, region.end);
                continue;
            }
            Require(previous.writable == region.writable, "writable guest memory overlaps an immutable snapshot");
            if (!region.writable) {
                const auto offset = static_cast<std::size_t>(region.begin - previous.begin);
                const auto overlap = static_cast<std::size_t>(std::min(previous.end, region.end) - region.begin);
                Require(std::memcmp(previous.snapshot.data() + offset, region.snapshot.data(), overlap) == 0, "inconsistent overlapping guest snapshots");
                if (region.end > previous.end) previous.snapshot.insert(previous.snapshot.end(), region.snapshot.begin() + overlap, region.snapshot.end());
            }
            previous.end = std::max(previous.end, region.end);
        } else {
            merged.push_back(std::move(region));
        }
    }
    regions = std::move(merged);
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto started = std::chrono::steady_clock::now();
    for (auto& region : regions) {
        Require(region.end - region.begin <= std::numeric_limits<std::size_t>::max(), "guest GPU allocation size overflow");

        if (region.mirror != nullptr || region.direct != nullptr || !mirrorsEnabled()) continue;

        auto mirror = findMirror(context, region.begin, region.end);
        if (mirror == nullptr || (region.begin - mirror->base) % context.limits.minStorageBufferOffsetAlignment != 0) continue;

        region.pending = mirror->writable;
        region.mirror = std::move(mirror);
        region.subrangeMirror = true;
    }

    if (context.hostImportAlignment != 0) {
        auto& state = Imports();
        std::lock_guard lock(state.mutex);
        const bool stale = importsStale(context, state);
        for (auto& region : regions) {
            if (region.mirror != nullptr) continue;

            const HostImport* entry = region.direct != nullptr && state.epoch == importsEpoch ? region.direct : nullptr;
            region.direct = nullptr;
            if (stale) {
                region.pending = true;
                continue;
            }
            if (entry == nullptr) entry = findImport(state, region.begin, region.end);
            if (entry == nullptr) {
                region.pending = true;
                continue;
            }

            bool staged = Recorder::Active() != nullptr && stagingEligible(region, addressable);
            const bool misaligned = (region.begin - entry->base) % context.limits.minStorageBufferOffsetAlignment != 0;
            if (staged) {
                const auto allocateStart = std::chrono::steady_clock::now();
                region.buffer = stagingBuffer(context, static_cast<std::size_t>(region.end - region.begin), gpuCopyUsage(addressable));
                if (profile) allocateUs.fetch_add(microsecondsSince(allocateStart), std::memory_order_relaxed);
                if (region.buffer == nullptr) {

                    region.unstaged = true;
                    staged = false;
                }
            }
            if (misaligned || staged) {
                if (!staged) {
                    if (!gpuCopyEligible(region)) continue;
                    const auto allocateStart = std::chrono::steady_clock::now();
                    region.buffer = std::make_shared<Buffer>(context, static_cast<std::size_t>(region.end - region.begin), gpuCopyUsage(addressable), copyBufferProperties(false));
                    if (profile) allocateUs.fetch_add(microsecondsSince(allocateStart), std::memory_order_relaxed);
                }
                region.deviceLocal = staged;
                region.direct = entry;
                region.pending = true;
                continue;
            }

            region.direct = entry;

            region.pending = true;
        }
        importsEpoch = state.epoch;
    }
    if (profile) importUs.fetch_add(microsecondsSince(started), std::memory_order_relaxed);

    for (auto& region : regions) {
        if (region.pending || region.mirror != nullptr || region.direct != nullptr) continue;
        if (region.writable && WritesOverlap(region.begin, static_cast<std::size_t>(region.end - region.begin))) {
            region.pending = true;
            continue;
        }
        copyRegion(region, addressable);
    }
}

void GuestBufferMemory::UploadFinish(bool addressable) {
    Require(prepared && !uploaded, "guest memory upload was not prepared");
    uploaded = true;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto started = std::chrono::steady_clock::now();

    GuestAllocations::Lease acquired;
    const auto importRanges = [&]() -> const GuestAllocations::Lease& {
        if (!lease.empty()) return lease;
        if (space != nullptr) return space->lease;
        if (acquired.empty()) acquired = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
        return acquired;
    };

    for (auto& region : regions) {
        if (region.mirror == nullptr || !region.subrangeMirror) continue;
        if (region.mirror->range.expired()) {
            region.mirror = nullptr;
            region.subrangeMirror = false;
            region.pending = true;
            continue;
        }
        region.snapshot.clear();
        std::lock_guard lock(Mirrors().mutex);
        ++Mirrors().subranges;
    }
    bool importsRefreshed = false;
    if (space != nullptr && context.hostImportAlignment != 0) {

        auto& state = Imports();
        std::lock_guard lock(state.mutex);
        if (importsStale(context, state)) refreshImports(context, state, importRanges());
        importsRefreshed = true;
        if (state.epoch != space->importsEpoch) {
            Spaces().dissolvedImports.fetch_add(1, std::memory_order_relaxed);
            dissolveSpace(true);
        }
    }

    auto* recorder = Recorder::Active();
    std::vector<Region*> gpuCopies;

    const auto loopStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::uint64_t refreshUs = 0;
    std::uint64_t copyUs = 0;
    for (auto& region : regions) {
        if (!region.pending) continue;
        region.pending = false;
        const auto bytes = region.end - region.begin;
        if (region.mirror != nullptr) {

            const auto refreshStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            refreshMirror(*region.mirror, region.begin, bytes);
            if (profile) refreshUs += microsecondsSince(refreshStart);
            continue;
        }
        bool copyOnGpu = false;
        if (context.hostImportAlignment != 0) {
            auto& state = Imports();
            std::lock_guard lock(state.mutex);

            if (!importsRefreshed) {
                if (importsStale(context, state)) refreshImports(context, state, importRanges());
                importsRefreshed = true;
            }

            const HostImport* entry = region.direct != nullptr && state.epoch == importsEpoch ? region.direct : nullptr;
            region.direct = nullptr;
            if (entry == nullptr) entry = findImport(state, region.begin, region.end);
            if (entry == nullptr) {
                if (const auto* range = containingRange(importRanges(), region.begin, region.end)) entry = importAllocation(context, state, range->address, range->bytes, importRanges());
            }

            const bool staged = recorder != nullptr && stagingEligible(region, addressable);
            if (entry != nullptr && !staged && (region.begin - entry->base) % context.limits.minStorageBufferOffsetAlignment == 0) {
                region.direct = entry;
                region.snapshot.clear();

                region.buffer.reset();
                region.deviceLocal = false;
            } else if (entry != nullptr && recorder != nullptr && (staged || gpuCopyEligible(region))) {

                copyOnGpu = true;
                region.deviceLocal = staged;
                region.copySource = entry->buffer;
                region.copySourceBase = entry->base;
            }
        }
        if (region.direct != nullptr) {

            static const bool bdaFlush = std::getenv("APS5_BDA_FLUSH") != nullptr;
            if (!(addressable && region.hostBacked) || bdaFlush) StorageTexture::FlushPending(region.begin, static_cast<std::size_t>(bytes), nullptr, "imported buffer region");
            continue;
        }
        if (copyOnGpu) {

            static_cast<void>(recorder->Commands());
            StorageTexture::FlushPending(region.begin, static_cast<std::size_t>(bytes), nullptr, "copied buffer region");
            region.gpuCopy = true;
            gpuCopies.push_back(&region);
            continue;
        }
        if (region.deviceLocal) {

            region.buffer.reset();
            region.deviceLocal = false;
        }
        const auto copyStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        copyRegion(region, addressable);
        if (profile) copyUs += microsecondsSince(copyStart);
    }
    if (!gpuCopies.empty()) recordGpuCopies(gpuCopies, addressable);
    takeHeapReferences();
    if (!profile) return;
    const auto loopUs = microsecondsSince(loopStart);
    readUs.fetch_add(refreshUs, std::memory_order_relaxed);
    importUs.fetch_add(loopUs > refreshUs + copyUs ? loopUs - refreshUs - copyUs : 0, std::memory_order_relaxed);
    const auto uploads = uploadsProfiled.fetch_add(1, std::memory_order_relaxed) + 1;
    if (uploads % 2000 == 0) {
        const auto& copies = Copies();
        aps5::LogErr( "[buffers] %llu uploads: import lookup %.0f ms, buffer allocation %.0f ms, guest read %.0f ms; copies on the GPU: %llu regions (%.0f KiB) copied out of imports, %llu written sub-ranges (%.0f KiB) copied back, %llu staging stores by the CPU at write-back\n", static_cast<unsigned long long>(uploads), importUs.load(std::memory_order_relaxed) / 1000.0, allocateUs.load(std::memory_order_relaxed) / 1000.0, readUs.load(std::memory_order_relaxed) / 1000.0, static_cast<unsigned long long>(copies.gpuCopies.load(std::memory_order_relaxed)), copies.gpuCopyBytes.load(std::memory_order_relaxed) / 1024.0, static_cast<unsigned long long>(copies.gpuCopyBacks.load(std::memory_order_relaxed)), copies.gpuCopyBackBytes.load(std::memory_order_relaxed) / 1024.0, static_cast<unsigned long long>(copies.stagingStores.load(std::memory_order_relaxed)));
    }
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (ms > 50) {
        std::uint64_t total = 0;
        std::uint64_t largest = 0;
        std::uint64_t copied = 0;
        for (const auto& region : regions) {
            total += region.end - region.begin;
            largest = std::max<std::uint64_t>(largest, region.end - region.begin);
            if (region.buffer != nullptr) copied += region.end - region.begin;
        }
        aps5::LogErr( "[resources] guest upload of %zu regions, %.1f MiB (largest %.1f MiB, copied %.1f MiB, addressable %d) took %.0f ms to finish\n", regions.size(), total / 1048576.0, largest / 1048576.0, copied / 1048576.0, addressable ? 1 : 0, ms);
    }

    static const bool traceBig = std::getenv("APS5_TRACE_BIGBUF") != nullptr;
    if (traceBig) {
        for (const auto& region : regions) {
            if (region.buffer != nullptr && region.end - region.begin >= (1u << 20u)) aps5::LogErr( "[bigbuf] upload 0x%llx+0x%llx writable=%d hostBacked=%d sparse=%d addressable=%d\n", static_cast<unsigned long long>(region.begin), static_cast<unsigned long long>(region.end - region.begin), region.writable ? 1 : 0, region.hostBacked ? 1 : 0, region.sparse ? 1 : 0, addressable ? 1 : 0);
        }
    }
}

void GuestBufferMemory::copyRegion(Region& region, bool addressable) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto bytes = region.end - region.begin;
    const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | (addressable ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0u);
    if (profile) {

        static std::mutex reasonsMutex;
        static std::uint64_t uploads = 0, noImport = 0, noImportBytes = 0, misaligned = 0, misalignedBytes = 0, snapshotOnly = 0, snapshotBytes = 0;
        static std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> outside;
        bool inImport = false;
        if (context.hostImportAlignment != 0) {
            auto& state = Imports();
            std::lock_guard lock(state.mutex);
            inImport = findImport(state, region.begin, region.end) != nullptr;
        }
        std::lock_guard lock(reasonsMutex);
        if (!region.hostBacked && !region.writable) { ++snapshotOnly; snapshotBytes += bytes; }
        else if (inImport) { ++misaligned; misalignedBytes += bytes; }
        else {
            ++noImport;
            noImportBytes += bytes;
            auto& bucket = outside[region.begin >> 28u];
            ++bucket.first;
            bucket.second += bytes;
        }
        if (++uploads % 1000 == 0) {

            aps5::LogErr( "[buffers] %llu copied regions on the CPU: %llu snapshots (%.0f MiB), %llu misaligned in imports (%.0f MiB), %llu outside imports (%.0f MiB):", static_cast<unsigned long long>(uploads), static_cast<unsigned long long>(snapshotOnly), snapshotBytes / 1048576.0, static_cast<unsigned long long>(misaligned), misalignedBytes / 1048576.0, static_cast<unsigned long long>(noImport), noImportBytes / 1048576.0);
            for (const auto& [granule, counts] : outside) aps5::LogErr( " 0x%llx0000000:%llu/%.0fMiB", static_cast<unsigned long long>(granule), static_cast<unsigned long long>(counts.first), counts.second / 1048576.0);
            aps5::LogErr( "\n");
        }
    }
    const auto allocateStart = std::chrono::steady_clock::now();

    if (region.buffer == nullptr) region.buffer = std::make_shared<Buffer>(context, static_cast<std::size_t>(bytes), usage);
    const auto readStart = std::chrono::steady_clock::now();
    if (profile) allocateUs.fetch_add(microsecondsSince(allocateStart), std::memory_order_relaxed);

    const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::BufferUpload);
    if (region.hostBacked || region.writable) {

        const bool written = region.writable && WritesOverlap(region.begin, static_cast<std::size_t>(bytes));
        if (!region.sparse) {
            GuestMemory::Read(region.begin, region.buffer->Bytes());
            if (written) region.uploaded.assign(region.buffer->Bytes().begin(), region.buffer->Bytes().end());
        } else {

            for (const auto& [first, last] : region.backed) {
                const auto piece = region.buffer->Bytes().subspan(static_cast<std::size_t>(first - region.begin), static_cast<std::size_t>(last - first));
                GuestMemory::Read(first, piece);
                if (written) region.uploaded.insert(region.uploaded.end(), piece.begin(), piece.end());
            }
        }
    }
    else std::memcpy(region.buffer->Bytes().data(), region.snapshot.data(), region.snapshot.size());
    region.snapshot.clear();
    if (profile) readUs.fetch_add(microsecondsSince(readStart), std::memory_order_relaxed);
}

void GuestBufferMemory::recordGpuCopies(std::span<Region* const> copies, bool addressable) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    static const bool capture = std::getenv("APS5_CAPTURE_GPU_COPIES") != nullptr;
    Require(!capture || CaptureTrace::Enabled(), "GPU copy capture requires APS5_CAPTURE_TRACE");
    auto* recorder = Recorder::Active();
    Require(recorder != nullptr, "GPU buffer copies need an active recorder");

    for (const auto* region : copies) {
        recorder->FlushKeyStoresOverlapping(region->begin, static_cast<std::size_t>(region->end - region->begin));
        recorder->FlushStoresOverlapping(region->begin, static_cast<std::size_t>(region->end - region->begin));
    }
    const auto commands = recorder->Commands();
    const auto timing = recorder->BeginGpuTiming(StagingCopyInKey);
    if (Recorder::BarrierValidate()) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
        for (const auto* region : copies) reads.emplace_back(region->begin, region->end);
        recorder->NoteAccess(Recorder::CommandClass::StagingIn, Recorder::Access{reads, {}, {}, VK_PIPELINE_STAGE_TRANSFER_BIT});
    }
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    Recorder::CountBarriers(Recorder::CommandClass::StagingIn);
    bool stagedAny = false;
    std::uint64_t copiedBytes = 0;
    for (auto* region : copies) {
        const auto bytes = region->end - region->begin;
        copiedBytes += bytes;
        if (region->buffer == nullptr) {

            const auto allocateStart = std::chrono::steady_clock::now();
            if (region->deviceLocal) region->buffer = stagingBuffer(context, static_cast<std::size_t>(bytes), gpuCopyUsage(addressable));
            if (region->buffer == nullptr) {

                region->deviceLocal = false;
                region->unstaged = true;
                region->buffer = std::make_shared<Buffer>(context, static_cast<std::size_t>(bytes), gpuCopyUsage(addressable), copyBufferProperties(false));
            }
            if (profile) allocateUs.fetch_add(microsecondsSince(allocateStart), std::memory_order_relaxed);
        }
        std::vector<std::byte> expected;
        const auto address = region->begin;
        const auto batch = static_cast<unsigned long long>(recorder->Submissions() + 1);
        const bool pendingWriter = recorder->PendingWriteOverlaps(address, static_cast<std::size_t>(bytes));
        const bool writable = WritesOverlap(address, static_cast<std::size_t>(bytes));
        std::shared_ptr<Buffer> inputSnapshot;
        auto copySource = region->copySource;
        auto copyOffset = region->begin - region->copySourceBase;
        if (!pendingWriter && !writable) {
            inputSnapshot = std::make_shared<Buffer>(context, static_cast<std::size_t>(bytes), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            std::memcpy(inputSnapshot->Bytes().data(), reinterpret_cast<const void*>(address), static_cast<std::size_t>(bytes));
            copySource = inputSnapshot->Handle();
            copyOffset = 0;
            recorder->Keep(inputSnapshot);
            CaptureTrace::Log("copy-input-snapshot batch=%llu address=%llx bytes=%llu", batch, static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
        }
        if (capture && bytes == 32) {
            CaptureTrace::Log("copy-input-candidate batch=%llu address=%llx bytes=%llu pendingWriter=%d writable=%d", batch, static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), pendingWriter, writable);
            if (inputSnapshot != nullptr) {
                const auto snapshotBytes = inputSnapshot->Bytes();
                expected.assign(snapshotBytes.begin(), snapshotBytes.end());
            }
        }
        CopyBuffer(context, commands, copySource, copyOffset, region->buffer->Handle(), 0, bytes);
        if (!expected.empty()) {
            auto readback = std::make_shared<Buffer>(context, expected.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            CopyBuffer(context, commands, region->buffer->Handle(), 0, readback->Handle(), 0, bytes);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
            recorder->OnComplete([readback, expected = std::move(expected), address, batch] {
                const auto actual = readback->Bytes();
                std::size_t changed = 0;
                for (std::size_t index = 0; index < expected.size(); ++index) {
                    if (expected[index] == actual[index]) continue;
                    ++changed;
                    CaptureTrace::Log("copy-input-byte batch=%llu address=%llx offset=%zu cpu=%02x gpu=%02x", batch, static_cast<unsigned long long>(address), index, std::to_integer<unsigned>(expected[index]), std::to_integer<unsigned>(actual[index]));
                }
                CaptureTrace::Log("copy-input-result batch=%llu address=%llx bytes=%zu changed=%zu", batch, static_cast<unsigned long long>(address), expected.size(), changed);
            });
        }
        if (inputSnapshot == nullptr) recorder->NotePendingRead(region->begin, static_cast<std::size_t>(bytes), Recorder::ReadKind::GpuCopy);

        recorder->Keep(region->buffer);
        region->snapshot.clear();
        if (region->deviceLocal) traceStaged(region->begin, region->end, region->atomic);
        if (profile) {
            Copies().gpuCopies.fetch_add(1, std::memory_order_relaxed);
            Copies().gpuCopyBytes.fetch_add(bytes, std::memory_order_relaxed);
            if (region->deviceLocal) {
                stagedAny = true;
                Copies().staged.fetch_add(1, std::memory_order_relaxed);
                Copies().stagedInBytes.fetch_add(bytes, std::memory_order_relaxed);
                if (region->atomic) Copies().stagedAtomic.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (stagedAny) reportStaging();

    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    Recorder::CountBarriers(Recorder::CommandClass::StagingIn);
    recorder->MarkCovered(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    recorder->EndGpuTiming(timing, copiedBytes);
}

void GuestBufferMemory::RecordCopyBacks(Recorder& recorder) {
    if (!uploaded || committed) return;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;

    std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
    bool recording = false;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    auto timing = Recorder::NoTiming;
    std::uint64_t copiedBytes = 0;
    for (auto& region : regions) {
        if (!region.gpuCopy || region.copiedBack) continue;
        if (merged.empty() && !writes.empty()) {
            auto sorted = writes;
            std::sort(sorted.begin(), sorted.end());
            for (const auto& range : sorted) {
                if (!merged.empty() && range.first < merged.back().second) merged.back().second = std::max(merged.back().second, range.second);
                else merged.push_back(range);
            }
        }
        for (const auto& [begin, end] : merged) {
            const auto from = std::max(begin, region.begin);
            const auto to = std::min(end, region.end);
            if (from >= to) continue;
            if (!recording) {
                commands = recorder.Commands();
                timing = recorder.BeginGpuTiming(StagingCopyBackKey);

                RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                Recorder::CountBarriers(Recorder::CommandClass::StagingOut);
                recording = true;
            }

            CopyBuffer(context, commands, region.buffer->Handle(), from - region.begin, region.copySource, from - region.copySourceBase, to - from);
            copiedBytes += to - from;
            recorder.Keep(region.buffer);
            if (profile) {
                Copies().gpuCopyBacks.fetch_add(1, std::memory_order_relaxed);
                Copies().gpuCopyBackBytes.fetch_add(to - from, std::memory_order_relaxed);
                if (region.deviceLocal) Copies().stagedOutBytes.fetch_add(to - from, std::memory_order_relaxed);
            }
        }

        region.copiedBack = true;
    }
    if (!recording) return;
    if (Recorder::BarrierValidate()) recorder.NoteAccess(Recorder::CommandClass::StagingOut, Recorder::Access{{}, merged, {}, VK_PIPELINE_STAGE_TRANSFER_BIT});

    constexpr VkAccessFlags copiedAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, copiedAccess);
    Recorder::CountBarriers(Recorder::CommandClass::StagingOut);
    recorder.MarkCovered(copiedAccess);
    recorder.EndGpuTiming(timing, copiedBytes);
}

VkDescriptorBufferInfo GuestBufferMemory::Descriptor(std::uint64_t address, std::size_t bytes, std::uint32_t& adjustment) const {
    Require(uploaded && !committed, "guest GPU memory is not available");
    Require(bytes != 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address, "invalid guest buffer view");
    const auto* found = owner(address);
    Require(found != nullptr, "guest buffer has no GPU owner");
    const auto& region = *found;
    Require(address >= region.begin && address + bytes <= region.end && (region.buffer != nullptr || region.direct != nullptr || region.mirror != nullptr), "guest buffer view exceeds its GPU owner");
    const auto base = region.direct != nullptr ? region.direct->base : region.mirror != nullptr ? region.mirror->base : region.begin;
    const auto offset = address - base;
    Require(context.limits.minStorageBufferOffsetAlignment != 0, "no storage buffer offset alignment");
    adjustment = static_cast<std::uint32_t>(offset % context.limits.minStorageBufferOffsetAlignment);
    Require(adjustment % 4 == 0, "guest buffer view off the storage buffer offset alignment is not DWORD aligned");
    Require(bytes + adjustment <= context.limits.maxStorageBufferRange, "guest buffer view exceeds descriptor range limit");
    const auto handle = region.direct != nullptr ? region.direct->buffer : region.mirror != nullptr ? region.mirror->buffer->Handle() : region.buffer->Handle();
    return {handle, offset - adjustment, bytes + adjustment};
}

ShaderRecompiler::BdaAbi::Range GuestBufferMemory::addressRange(const Region& region) {
    Require(region.buffer != nullptr || region.direct != nullptr || region.mirror != nullptr, "incomplete guest GPU upload");
    const auto address = region.direct != nullptr ? region.direct->address + (region.begin - region.direct->base) : region.mirror != nullptr ? region.mirror->buffer->DeviceAddress() + (region.begin - region.mirror->base) : region.buffer->DeviceAddress();
    Require(region.end - region.begin <= std::numeric_limits<std::uint64_t>::max() - address, "GPU address range overflow");
    const auto permissions = ShaderRecompiler::BdaAbi::Read | (region.direct != nullptr && region.writable ? ShaderRecompiler::BdaAbi::Write : 0u);
    return {region.begin, region.end, address, permissions, 0};
}

std::vector<ShaderRecompiler::BdaAbi::Range> GuestBufferMemory::AddressRanges() const {
    Require(uploaded && !committed, "guest GPU address ranges are not available");
    std::vector<ShaderRecompiler::BdaAbi::Range> result;
    result.reserve(regions.size() + (space != nullptr ? space->ranges.size() : 0));
    for (const auto& region : regions) result.push_back(addressRange(region));
    if (space == nullptr) return result;

    std::vector<ShaderRecompiler::BdaAbi::Range> merged;
    merged.reserve(result.size() + space->ranges.size());
    std::merge(space->ranges.begin(), space->ranges.end(), result.begin(), result.end(), std::back_inserter(merged), [](const auto& left, const auto& right) { return left.begin < right.begin; });
    return merged;
}

std::optional<GuestBufferMemory::CachedTable> GuestBufferMemory::CachedAddressTable() const {
    if (space == nullptr || !regions.empty()) return std::nullopt;
    Require(uploaded && !committed, "guest GPU address ranges are not available");
    return CachedTable{space->serial, &space->ranges};
}

bool GuestBufferMemory::HasCopiedWrites() const {
    for (const auto& [begin, end] : writes) {
        const auto* found = owner(begin);
        if (found == nullptr) continue;
        const auto& region = *found;
        if (region.direct != nullptr) continue;

        if (region.gpuCopy && (region.copiedBack || region.deviceLocal)) continue;

        if (region.mirror != nullptr && !region.mirror->writable) continue;
        return true;
    }
    return false;
}

void GuestBufferMemory::MarkDirectWrites() const {
    for (const auto& [begin, end] : writes) {
        const auto* found = owner(begin);
        if (found == nullptr) continue;
        const auto& region = *found;
        if (region.direct != nullptr || (region.gpuCopy && region.copiedBack)) GuestMemory::MarkWritten(begin, static_cast<std::size_t>(end - begin));
    }
}

namespace {

void storeChanged(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original) {
    const auto writable = GuestMemory::DescribeCommitted(address, current.size(), true);
    if (writable.whole) {
        GuestMemory::WriteChanged(address, current, original);
        return;
    }
    const auto unchanged = [&](std::uint64_t from, std::uint64_t to) {
        const auto at = static_cast<std::size_t>(from - address);
        if (std::memcmp(current.data() + at, original.data() + at, static_cast<std::size_t>(to - from)) == 0) return;
        char message[192];
        std::snprintf(message, sizeof(message), "the GPU changed guest memory 0x%llx+0x%llx that is not writable (a direct memory page shared by several views; aliased writes are not implemented)", static_cast<unsigned long long>(from), static_cast<unsigned long long>(to - from));
        throw std::runtime_error(message);
    };
    auto cursor = address;
    for (const auto& [first, last] : writable.ranges) {
        if (cursor < first) unchanged(cursor, first);
        const auto at = static_cast<std::size_t>(first - address);
        const auto length = static_cast<std::size_t>(last - first);
        GuestMemory::WriteChanged(first, current.subspan(at, length), original.subspan(at, length));
        cursor = last;
    }
    if (cursor < address + current.size()) unchanged(cursor, address + current.size());
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> mergeWrites(std::vector<std::pair<std::uint64_t, std::uint64_t>> writes) {
    std::sort(writes.begin(), writes.end());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
    for (const auto& range : writes) {
        if (!merged.empty() && range.first < merged.back().second) merged.back().second = std::max(merged.back().second, range.second);
        else merged.push_back(range);
    }
    return merged;
}

}

void GuestBufferMemory::takeHeapReferences() {
    heapReferences.clear();
    for (const auto& [begin, end] : mergeWrites(writes)) {
        const auto* found = owner(begin);
        if (found == nullptr || found->mirror == nullptr || !found->mirror->heap || !found->mirror->writable) continue;
        Require(end <= found->end, "written range exceeds its heap mirror");
        const auto& mirror = *found->mirror;
        const auto bytes = mirror.buffer->Bytes().subspan(static_cast<std::size_t>(begin - mirror.base), static_cast<std::size_t>(end - begin));
        heapReferences.emplace_back(begin, std::vector<std::byte>(bytes.begin(), bytes.end()));
    }
}

void GuestBufferMemory::WriteBack() {
    Require(uploaded && !committed, "guest memory cannot be committed twice or before upload");
    const auto merged = mergeWrites(writes);

    for (const auto& [begin, end] : merged) {
        const auto* found = owner(begin);
        Require(found != nullptr, "write-back range has no GPU owner");
        const auto& region = *found;

        if (region.direct == nullptr && !(region.gpuCopy && (region.copiedBack || region.deviceLocal)) && !(region.mirror != nullptr && !region.mirror->writable)) Recorder::NoteWrittenBack(begin, static_cast<std::size_t>(end - begin));
        if (region.direct != nullptr) {

            GuestMemory::MarkWritten(begin, static_cast<std::size_t>(end - begin));
            continue;
        }
        if (region.mirror != nullptr) {
            if (!region.mirror->writable) continue;
            Require(end <= region.end, "write-back range exceeds its GPU owner");

            auto& mirror = *region.mirror;
            const auto offset = static_cast<std::size_t>(begin - mirror.base);
            const auto length = static_cast<std::size_t>(end - begin);
            const auto current = mirror.buffer->Bytes().subspan(offset, length);
            if (mirror.heap) {
                const auto reference = std::find_if(heapReferences.begin(), heapReferences.end(), [&](const auto& entry) { return entry.first == begin; });
                Require(reference != heapReferences.end() && reference->second.size() == length, "heap mirror write-back has no reference");
                storeChanged(begin, current, reference->second);
                continue;
            }
            GuestMemory::WriteChanged(begin, current, std::span<const std::byte>(mirror.shadow).subspan(offset, length));
            std::memcpy(mirror.shadow.data() + offset, current.data(), length);
            continue;
        }
        if (region.gpuCopy) {
            Require(region.buffer != nullptr && end <= region.end, "write-back range exceeds its GPU owner");
            if (region.copiedBack) {

                GuestMemory::MarkWritten(begin, static_cast<std::size_t>(end - begin));
                continue;
            }
            static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
            if (region.deviceLocal) {

                continue;
            }

            GuestMemory::Write(begin, region.buffer->Bytes().subspan(static_cast<std::size_t>(begin - region.begin), static_cast<std::size_t>(end - begin)));
            if (profile) Copies().stagingStores.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        Require(region.buffer != nullptr && region.writable && end <= region.end, "write-back range exceeds its GPU owner");
        static const bool traceBig = std::getenv("APS5_TRACE_BIGBUF") != nullptr;
        if (traceBig && end - begin >= (1u << 20u)) aps5::LogErr( "[bigbuf] writeback 0x%llx+0x%llx sparse=%d\n", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin), region.sparse ? 1 : 0);
        if (!region.sparse) {
            const auto first = static_cast<std::size_t>(begin - region.begin);
            const auto length = static_cast<std::size_t>(end - begin);
            storeChanged(begin, region.buffer->Bytes().subspan(first, length), std::span<const std::byte>(region.uploaded).subspan(first, length));
            continue;
        }
        std::size_t packed = 0;
        for (const auto& [first, last] : region.backed) {
            const auto from = std::max(first, begin);
            const auto to = std::min(last, end);
            if (from < to) {
                const auto length = static_cast<std::size_t>(to - from);
                storeChanged(from, region.buffer->Bytes().subspan(static_cast<std::size_t>(from - region.begin), length), std::span<const std::byte>(region.uploaded).subspan(packed + static_cast<std::size_t>(from - first), length));
            }
            packed += static_cast<std::size_t>(last - first);
        }
    }
    committed = true;
    lease.clear();
    space.reset();
    heapReferences.clear();
}

std::optional<std::vector<std::pair<std::uint64_t, std::uint64_t>>> GuestBufferMemory::DirectRegions() const {
    if (!uploaded || committed) return std::nullopt;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> result;
    if (space != nullptr) {
        for (const auto& region : space->base) {
            if (region.direct == nullptr && (region.mirror == nullptr || region.mirror->writable || region.mirror->heap)) return std::nullopt;
            result.emplace_back(region.begin, region.end);
        }
    }
    for (const auto& region : regions) {

        const bool fixedMirror = region.mirror != nullptr && !region.mirror->writable && !region.mirror->heap;
        const bool staged = region.gpuCopy && region.deviceLocal;
        if (region.direct == nullptr && !fixedMirror && !staged) return std::nullopt;
        if (staged) {

            auto& state = Imports();
            std::lock_guard lock(state.mutex);
            const auto* entry = state.device == context.device ? findImport(state, region.begin, region.end) : nullptr;
            if (entry == nullptr || entry->buffer != region.copySource) return std::nullopt;
        }
        result.emplace_back(region.begin, region.end);
    }
    return result;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> GuestBufferMemory::InPlaceReads() const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> result;
    if (!uploaded || committed) return result;
    if (space != nullptr) {
        for (const auto& region : space->base) {
            if (region.direct != nullptr) result.emplace_back(region.begin, region.end);
        }
    }
    for (const auto& region : regions) {
        if (region.direct != nullptr) result.emplace_back(region.begin, region.end);
    }
    return result;
}

void GuestBufferMemory::RecordStagingCopies(Recorder& recorder) {
    if (!uploaded || committed) return;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    std::vector<Region*> copies;
    for (auto& region : regions) {
        if (!region.gpuCopy || !region.deviceLocal) continue;

        if (!region.copiedBack && profile) Copies().stagedLost.fetch_add(1, std::memory_order_relaxed);
        region.copiedBack = false;
        copies.push_back(&region);
    }
    if (copies.empty()) return;
    Require(Recorder::Active() == &recorder, "staging copies recorded into a recorder that is not the device's");
    if (profile) Copies().stagedReused.fetch_add(copies.size(), std::memory_order_relaxed);
    recordGpuCopies(copies, false);
}

std::uint64_t ImageMirrorSerial(const Context& context, std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address || !mirrorsEnabled()) return 0;

    const auto mirror = findMirror(context, address, address + bytes);
    return mirror != nullptr && !mirror->writable ? mirror->serial : 0;
}

std::uint64_t HostImportSerial(const Context& context, std::uint64_t address, std::size_t bytes, bool reconcile) {
    if (context.hostImportAlignment == 0 || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return 0;
    auto& state = Imports();
    std::lock_guard lock(state.mutex);

    if (reconcile && GuestAllocations::GuestAllocationsGeneration_nid_postfix() != state.refreshedGeneration) static_cast<void>(refreshImports(context, state, GuestAllocations::GuestAllocationsAcquire_nid_postfix()));
    auto found = state.imports.upper_bound(address);
    if (found == state.imports.begin()) return 0;
    --found;
    auto& entry = found->second;
    if (address < entry.base || address + bytes > entry.base + entry.bytes) return 0;

    static std::uint64_t serials = 0;
    if (entry.serial == 0) entry.serial = ++serials;
    return entry.serial;
}

bool HostImportsUnchanged(const Context& context, const HostImportsProof& proof) {
    if (proof.device == VK_NULL_HANDLE || proof.device != context.device) return false;
    auto& state = Imports();
    std::lock_guard lock(state.mutex);
    return state.device == proof.device && state.epoch == proof.epoch && state.refreshedGeneration == proof.refreshedGeneration && !importsStale(context, state);
}

HostImportsProof HostImportsIdentity(const Context& context) {
    auto& state = Imports();
    std::lock_guard lock(state.mutex);
    if (importsStale(context, state)) return {};
    return {state.device, state.epoch, state.refreshedGeneration};
}

}
