#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/common/StderrLog.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4Opcodes.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>

namespace AgcDriver::Graphics {

namespace {

Recorder* activeRecorder = nullptr;

bool DrawProfiled() {
    static const bool profiled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profiled;
}

std::uint64_t syncCounts[5] = {};

double syncWaitedMs[5] = {};
thread_local int announcedSource = 4;

thread_local const void* announcedSite = nullptr;
struct SyncSiteWaits {
    int source;
    const void* site;
    std::uint64_t syncs;
    std::uint64_t batches;
    double waitedMs;
};
std::vector<SyncSiteWaits> syncSites;
constexpr std::size_t NoSyncSite = static_cast<std::size_t>(-1);
constexpr std::size_t SyncSiteLimit = 48;
thread_local std::size_t activeSyncSite = NoSyncSite;

bool SyncSitesProfiled() {

    static const bool profiled = std::getenv("APS5_PROFILE_DRAW") != nullptr && std::getenv("APS5_NO_SYNC_SITES") == nullptr;
    return profiled;
}

std::size_t BeginSyncSite(int source, const void* site) {
    const auto previous = activeSyncSite;
    if (!SyncSitesProfiled()) return previous;
    auto it = std::find_if(syncSites.begin(), syncSites.end(), [&](const SyncSiteWaits& entry) { return entry.source == source && entry.site == site; });
    if (it == syncSites.end()) {
        if (syncSites.size() >= SyncSiteLimit) {
            it = std::find_if(syncSites.begin(), syncSites.end(), [&](const SyncSiteWaits& entry) { return entry.source == source && entry.site == nullptr; });
            if (it == syncSites.end()) it = syncSites.insert(syncSites.end(), SyncSiteWaits{source, nullptr, 0, 0, 0});
        } else {
            it = syncSites.insert(syncSites.end(), SyncSiteWaits{source, site, 0, 0, 0});
        }
    }
    ++it->syncs;
    activeSyncSite = static_cast<std::size_t>(it - syncSites.begin());
    return previous;
}

void CountSiteWait(double ms) {
    if (activeSyncSite == NoSyncSite || activeSyncSite >= syncSites.size()) return;
    auto& entry = syncSites[activeSyncSite];
    ++entry.batches;
    entry.waitedMs += ms;
}

std::string SyncSiteReport() {
    static const char* const names[5] = {"idle", "pending-write", "recorded-store", "address-based", "other"};
    std::vector<const SyncSiteWaits*> order;
    for (const auto& entry : syncSites) order.push_back(&entry);
    std::sort(order.begin(), order.end(), [](const SyncSiteWaits* a, const SyncSiteWaits* b) { return a->waitedMs > b->waitedMs; });
    std::string report;
    for (std::size_t i = 0; i < order.size() && i < 8; ++i) {
        char text[96];
        std::snprintf(text, sizeof(text), " %s@+0x%llx %llu/%llu/%.1fs", names[order[i]->source], order[i]->site != nullptr ? GuestMemory::CodeOffset(order[i]->site) : 0ull, static_cast<unsigned long long>(order[i]->syncs), static_cast<unsigned long long>(order[i]->batches), order[i]->waitedMs / 1000);
        report += text;
    }
    return report;
}

struct ThreadSyncs {
    std::uint32_t tag;
    std::array<std::uint64_t, 5> counts;
    std::array<double, 5> waitedMs;
};
std::vector<ThreadSyncs> threadSyncs;
std::mutex threadSyncsMutex;

thread_local double threadWaitedMs = 0;

void CountThreadSync(int source, double ms) {
    threadWaitedMs += ms;
    const auto tag = GuestMemory::GpuLockThreadTag();
    std::lock_guard lock(threadSyncsMutex);
    auto it = std::find_if(threadSyncs.begin(), threadSyncs.end(), [&](const ThreadSyncs& thread) { return thread.tag == tag; });
    if (it == threadSyncs.end()) it = threadSyncs.insert(threadSyncs.end(), ThreadSyncs{tag, {}, {}});
    ++it->counts[source];
    it->waitedMs[source] += ms;
}

std::string ThreadSyncReport() {
    static const char* const names[5] = {"idle", "pending-write", "recorded-store", "address-based", "other"};
    std::string report;
    std::lock_guard lock(threadSyncsMutex);
    for (const auto& thread : threadSyncs) {
        char text[64];
        if (thread.tag == 0xffffffffu) std::snprintf(text, sizeof(text), " untagged:");
        else std::snprintf(text, sizeof(text), " queue 0x%x:", thread.tag);
        report += text;
        for (int source = 0; source < 5; ++source) {
            if (thread.counts[source] == 0) continue;
            std::snprintf(text, sizeof(text), " %s %llu/%.1fs", names[source], static_cast<unsigned long long>(thread.counts[source]), thread.waitedMs[source] / 1000);
            report += text;
        }
    }
    return report;
}

std::atomic<std::uint64_t> unlockedWaits{0}, unlockedWaitedUs{0};

std::uint64_t submitCount = 0;
double submitUs = 0, submitMaxUs = 0;

constexpr std::size_t WaiterSlots = 16;
std::atomic<int> unlockedWaiters[WaiterSlots]{};
std::atomic<int>& WaitersOf(std::uint64_t id) { return unlockedWaiters[id % WaiterSlots]; }

std::mutex liveRecordersMutex;
std::vector<std::uint64_t> liveRecorders;
std::atomic<std::uint64_t> nextRecorderId{1};
std::atomic<std::uint64_t> samplesPassed{0};

bool RecorderAlive(std::uint64_t id) {
    std::lock_guard lock(liveRecordersMutex);
    return std::find(liveRecorders.begin(), liveRecorders.end(), id) != liveRecorders.end();
}

thread_local int completionDepth = 0;

thread_local std::uint64_t hookRealWaits = 0;

}

bool Recorder::InCompletion() {
    return completionDepth != 0;
}

namespace {

bool HookCompletionGuard() {
    static const bool guard = std::getenv("APS5_NO_HOOK_COMPLETION_GUARD") == nullptr;
    return guard;
}

bool CompletionStoreSyncs() {
    static const bool enabled = std::getenv("APS5_COMPLETION_STORE_SYNC") != nullptr;
    return enabled;
}

bool HookLockedWait() {
    static const bool locked = std::getenv("APS5_HOOK_LOCKED_WAIT") != nullptr;
    return locked;
}

bool HookFullSync() {
    static const bool hookFullSync = std::getenv("APS5_HOOK_FULL_SYNC") != nullptr;
    return hookFullSync;
}

bool HookFlushCpuBlocks() {
    static const bool hookFlushCpuBlocks = [] {
        const char* text = std::getenv("APS5_HOOK_FLUSH_CPU_BLOCKS");
        return text != nullptr && text[0] == '0';
    }();
    return hookFlushCpuBlocks;
}

struct HoldCounters {
    std::uint64_t completions = 0;
    double completionMs = 0;
    double keptReleaseMs = 0;
    std::uint64_t completionSyncsSkipped = 0;
    std::uint64_t completionSyncsWaited = 0;
    double completionSyncWaitMs = 0;
    std::uint64_t reaps = 0;
    std::uint64_t reapsWithWork = 0;
    std::uint64_t reapBatches = 0;
    double reapMs = 0;
    std::uint64_t hookUnlockedWaits = 0;

    double hookUnlockedWaitMs = 0;
    double hookRelockMs = 0;

    std::array<std::uint64_t, 5> hookUnlockedWaitsBySource{};
    std::array<double, 5> hookUnlockedWaitMsBySource{};
    std::uint64_t hookUnlockedTornDown = 0;

    std::uint64_t hookLockedWaits = 0;
};
HoldCounters holdCounters;

struct DeferredBatch {
    std::vector<std::shared_ptr<void>> kept;
    std::vector<std::function<void()>> completions;
};
struct DeferredBatchesTag {};
auto& DeferredBatches() { return HostThreadLocal<std::vector<DeferredBatch>, DeferredBatchesTag>(); }
std::atomic<std::uint64_t> deferredPending{0};

std::atomic<std::uint64_t> threadReleases{0}, threadObjects{0}, threadReleaseUs{0};
std::atomic<std::uint64_t> inlineReleases{0}, inlineObjects{0}, inlineReleaseUs{0}, inlineOverBound{0};
std::atomic<std::uint64_t> releaseQueueMax{0};

bool ReleaseUnderLock() {
    static const bool locked = std::getenv("APS5_RELEASE_UNDER_LOCK") != nullptr;
    return locked;
}

bool ReleaseOnUnlock() {
    static const bool onUnlock = std::getenv("APS5_RELEASE_ON_UNLOCK") != nullptr;
    return onUnlock;
}

std::size_t ReleaseQueueBound() {
    static const std::size_t bound = [] {
        const char* value = std::getenv("APS5_RELEASE_QUEUE_MAX");
        const auto parsed = value != nullptr ? std::strtoull(value, nullptr, 10) : 0ull;
        return parsed != 0 ? static_cast<std::size_t>(parsed) : std::size_t{256};
    }();
    return bound;
}

void DestroyDeferred(std::vector<DeferredBatch> releasing, bool onThread) {
    if (releasing.empty()) return;
    const bool profile = DrawProfiled();
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto count = releasing.size();
    std::uint64_t objects = 0;
    for (auto& batch : releasing) {
        objects += batch.kept.size();

        batch.completions.clear();
        batch.kept.clear();
        deferredPending.fetch_sub(1, std::memory_order_acq_rel);
    }
    releasing.clear();
    if (profile) {
        const auto us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        (onThread ? threadReleases : inlineReleases).fetch_add(count, std::memory_order_relaxed);
        (onThread ? threadObjects : inlineObjects).fetch_add(objects, std::memory_order_relaxed);
        (onThread ? threadReleaseUs : inlineReleaseUs).fetch_add(us, std::memory_order_relaxed);
    }
}

struct ReleaseQueue {
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<DeferredBatch> items;
    std::thread thread;
    bool started = false;

    bool stop = false;
    std::mutex joinMutex;
};

ReleaseQueue& ReleaseThreadQueue() {
    static ReleaseQueue* const queue = new ReleaseQueue;
    return *queue;
}

void ReleaseThreadMain() {
    CpuTopology::PinHelperThread("release thread");
    auto& queue = ReleaseThreadQueue();
    std::unique_lock lock(queue.mutex);
    for (;;) {
        queue.wake.wait(lock, [&] { return !queue.items.empty() || queue.stop; });
        if (queue.items.empty()) return;
        auto items = std::move(queue.items);
        queue.items.clear();
        lock.unlock();
        DestroyDeferred(std::move(items), true);
        lock.lock();
    }
}

void JoinReleaseThread() {
    auto& queue = ReleaseThreadQueue();
    std::lock_guard joining(queue.joinMutex);
    std::thread worker;
    {
        std::lock_guard lock(queue.mutex);
        if (!queue.started) return;
        queue.stop = true;
        worker = std::move(queue.thread);
    }
    queue.wake.notify_all();
    worker.join();
    std::lock_guard lock(queue.mutex);
    queue.started = false;
    queue.stop = false;
}

void ReleaseDeferredKeeps() {
    if (DeferredBatches().empty()) return;

    auto releasing = std::move(DeferredBatches());
    DeferredBatches().clear();
    if (ReleaseOnUnlock()) {
        DestroyDeferred(std::move(releasing), false);
        return;
    }
    auto& queue = ReleaseThreadQueue();
    const bool profile = DrawProfiled();
    bool handedOff = false;
    bool overBound = false;
    try {
        std::lock_guard lock(queue.mutex);
        if (queue.items.size() + releasing.size() > ReleaseQueueBound()) {
            overBound = true;
        } else if (!queue.stop) {

            if (!queue.started) {
                queue.thread = std::thread(&ReleaseThreadMain);
                queue.started = true;
            }

            queue.items.reserve(queue.items.size() + releasing.size());
            for (auto& batch : releasing) queue.items.push_back(std::move(batch));
            handedOff = true;
            if (profile) {
                const auto queued = static_cast<std::uint64_t>(queue.items.size());
                auto seen = releaseQueueMax.load(std::memory_order_relaxed);
                while (queued > seen && !releaseQueueMax.compare_exchange_weak(seen, queued, std::memory_order_relaxed)) {
                }
            }
        }
    } catch (...) {
    }
    if (!handedOff) {
        if (profile && overBound) inlineOverBound.fetch_add(releasing.size(), std::memory_order_relaxed);
        DestroyDeferred(std::move(releasing), false);
        return;
    }
    queue.wake.notify_one();
}

constexpr std::int64_t NoPendingLabel = std::numeric_limits<std::int64_t>::min();
std::atomic<std::int64_t> pendingLabelSince{NoPendingLabel};
std::atomic<std::uint64_t> writeGeneration{0};
std::atomic<std::uint64_t> publishGeneration{0};
std::atomic<std::uint64_t> completionLabels{0};
std::atomic<std::uint64_t> writeBackCompletions{0};
std::atomic<std::uint64_t> completionStoresSkipped{0};
std::atomic<std::uint64_t> completionStoresRun{0};

std::atomic<std::uint64_t> writeBacksOverLabels{0};

std::atomic<std::uint64_t> completionLabelsCountedLate{0};
std::atomic<std::uint64_t> workSinceSubmit{0};

bool CountAllCompletionLabels() {
    static const bool all = std::getenv("APS5_COUNT_ALL_COMPLETION_LABELS") != nullptr;
    return all;
}

bool ProcTable() {
    static const bool table = std::getenv("APS5_NO_PROC_TABLE") == nullptr;
    return table;
}

std::mutex labelTableMutex;
Recorder* labelTableOwner = nullptr;

struct QueuedLabelRangesTag {};
auto& QueuedLabelRanges() { return HostThreadLocal<std::vector<std::pair<std::uint64_t, std::uint64_t>>, QueuedLabelRangesTag>(); }
std::atomic<void (*)()> queuedLabelRecorder{nullptr};

struct LabelGroupDwordsTag {};
auto& LabelGroupDwords() { return HostThreadLocal<std::vector<std::pair<std::uint64_t, std::uint64_t>>, LabelGroupDwordsTag>(); }

thread_local Recorder::LateStatistics lateCounts{};

bool LateTrustEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_LABEL_TRUST_LATE");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

std::atomic<std::uint64_t> storeCount{0}, storeRuns{0}, storesJoined{0}, storesReplaced{0}, storeWawBarriers{0}, storeJoinsRefused{0};
std::atomic<std::uint64_t> keyStoreCount{0}, keyStoreRuns{0}, keyStoreRunsForWriter{0}, keyStoresJoined{0};

bool KeyStoresEach() {
    static const bool each = std::getenv("APS5_DCC_KEYS_EACH") != nullptr;
    return each;
}
std::atomic<std::uint64_t> queuedLabelsNoted{0}, queuedLabelsOverRecorded{0}, queuedLabelHits{0}, queuedLabelHookRecords{0}, queuedLabelHookInCompletion{0};

constexpr std::size_t ReadKinds = static_cast<std::size_t>(Recorder::ReadKind::Count);
std::atomic<std::uint64_t> readsNoted{0}, readQueries{0}, readStaleIgnored{0};
std::atomic<std::uint64_t> readHits[ReadKinds]{};

bool ReadTrackingEnabled() {

    static const bool enabled = [] {
        const char* value = std::getenv("APS5_COPY_READ_TRACKING");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool LabelRunsEnabled() {

    static const bool enabled = std::getenv("APS5_NO_LABEL_RUNS") == nullptr && std::getenv("APS5_NO_LABEL_BATCHING") == nullptr;
    return enabled;
}

bool LabelRunsPerBatch() {
    static const bool perBatch = LabelRunsEnabled() && std::getenv("APS5_LABEL_RUNS_INLINE") == nullptr;
    return perBatch;
}
std::atomic<std::uint64_t> storeRunsAtSubmit{0}, storeRunsForced{0};

bool JoinWawCheck() {
    static const bool check = std::getenv("APS5_NO_JOIN_WAW_CHECK") == nullptr;
    return check;
}

bool SeparateQueuedLabels() {
    static const bool separate = std::getenv("APS5_NO_SEPARATE_QUEUED_LABELS") == nullptr;
    return separate;
}

bool QueuedLabelOverlaps(std::uint64_t address, std::size_t bytes) {
    const auto end = address + bytes;
    for (const auto& [begin, finish] : QueuedLabelRanges()) {
        if (address < finish && begin < end) return true;
    }
    return false;
}

std::atomic<std::uint64_t> hookCalls{0}, hookLocks{0}, targetedSyncs{0}, batchesLeftInFlight{0};

std::uint64_t snapshotCovered = 0, snapshotRebuilds = 0;
double snapshotRebuildMs = 0;

using WriteRanges = Recorder::WriteRanges;

std::atomic<std::shared_ptr<const WriteRanges>> pendingWrites;

bool HookSnapshotEnabled() {

    static const bool enabled = std::getenv("APS5_NO_HOOK_SNAPSHOT") == nullptr;
    return enabled;
}

bool SnapshotOverlaps(const WriteRanges* snapshot, std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || snapshot == nullptr || snapshot->empty()) return false;

    const auto it = std::partition_point(snapshot->begin(), snapshot->end(), [&](const auto& range) { return range.second <= address; });
    return it != snapshot->end() && it->first < address + bytes;
}

bool SnapshotOverlaps(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0) return false;
    const auto snapshot = pendingWrites.load(std::memory_order_acquire);
    return SnapshotOverlaps(snapshot.get(), address, bytes);
}

bool SnapshotCovers(std::uint64_t address, std::uint64_t end) {
    const auto snapshot = pendingWrites.load(std::memory_order_acquire);
    if (snapshot == nullptr || snapshot->empty()) return false;
    const auto it = std::partition_point(snapshot->begin(), snapshot->end(), [&](const auto& range) { return range.second <= address; });
    return it != snapshot->end() && it->first <= address && end <= it->second;
}

bool SyncThroughEnabled() {
    static const bool enabled = std::getenv("APS5_NO_SYNC_THROUGH") == nullptr;
    return enabled;
}

constexpr std::size_t HookSyncFrames = 6;

struct HookSyncKey {
    std::uint32_t queue;
    std::uint32_t opcode;
    GuestMemory::ReadSite site;
    std::array<unsigned long long, HookSyncFrames> frames;
    bool operator<(const HookSyncKey& other) const {
        return std::tie(queue, opcode, site, frames) < std::tie(other.queue, other.opcode, other.site, other.frames);
    }
};

struct HookSyncOutcomes {
    std::uint64_t unchangedOpen = 0;
    std::uint64_t unchangedPending = 0;
    std::uint64_t unchangedSignaled = 0;
    std::uint64_t changed = 0;
    std::uint64_t stores = 0;
    std::uint64_t unchecked = 0;
};

struct HookSyncTotals {
    std::uint64_t count = 0;
    HookSyncOutcomes outcomes;
    std::uint64_t rangeBytes = 0;
    std::uint64_t accessBytes = 0;
    double waitedMs = 0;
};

constexpr std::size_t SizeBuckets = 6;
constexpr const char* SizeBucketNames[SizeBuckets] = {"<=4K", "<=64K", "<=1M", "<=16M", "<=256M", ">256M"};

std::size_t SizeBucket(std::uint64_t bytes) {
    if (bytes <= 4096) return 0;
    if (bytes <= 65536) return 1;
    if (bytes <= (1u << 20u)) return 2;
    if (bytes <= (16u << 20u)) return 3;
    if (bytes <= (256u << 20u)) return 4;
    return 5;
}

struct HookSyncStats {
    std::map<HookSyncKey, HookSyncTotals> byKey;
    std::uint64_t count = 0, openTargets = 0, signaledTargets = 0, batchesFinished = 0;
    HookSyncOutcomes outcomes;
    double waitedMs = 0;
    std::array<std::uint64_t, SizeBuckets> rangeBuckets{};
    std::array<std::uint64_t, SizeBuckets> accessBuckets{};
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

HookSyncStats& HookSyncs() {
    static HookSyncStats stats;
    return stats;
}

bool HookSyncProfiled() {
    static const bool profiled = std::getenv("APS5_PROFILE_DRAW") != nullptr && std::getenv("APS5_NO_HOOKSYNC_PROFILE") == nullptr;
    return profiled;
}

std::string PacketName(std::uint32_t opcode) {
    if (opcode == GuestMemory::NoPacket) return "no-packet";
    if (opcode == 0xffffu) return "flip";

    if (opcode == 0xfffdu) return "deferred-labels";
    for (const auto& entry : Pm4::Opcodes) {
        if (entry.value == opcode) return std::string(entry.name);
    }
    char text[24];
    std::snprintf(text, sizeof(text), "op 0x%x", opcode);
    return text;
}

void ReportHookSyncs(HookSyncStats& stats) {
    std::vector<std::pair<const HookSyncKey*, const HookSyncTotals*>> hot;
    hot.reserve(stats.byKey.size());
    for (const auto& [key, totals] : stats.byKey) hot.emplace_back(&key, &totals);
    std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second->waitedMs > b.second->waitedMs; });
    std::string report;
    char text[512];
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto& o = stats.outcomes;
    std::snprintf(text, sizeof(text), "[hooksync] %llu pending-write syncs waited %.0f ms (10 s); read bytes after the wait: unchanged %llu (target open %llu, in flight unsignaled %llu, signaled %llu), changed %llu; stores %llu, unchecked %llu; targets: %llu open, %llu already signaled, %llu batches finished; noted range:", count(stats.count), stats.waitedMs, count(o.unchangedOpen + o.unchangedPending + o.unchangedSignaled), count(o.unchangedOpen), count(o.unchangedPending), count(o.unchangedSignaled), count(o.changed), count(o.stores), count(o.unchecked), count(stats.openTargets), count(stats.signaledTargets), count(stats.batchesFinished));
    report += text;
    for (std::size_t i = 0; i < SizeBuckets; ++i) {
        if (stats.rangeBuckets[i] == 0) continue;
        std::snprintf(text, sizeof(text), " %s %llu", SizeBucketNames[i], static_cast<unsigned long long>(stats.rangeBuckets[i]));
        report += text;
    }
    report += "; access:";
    for (std::size_t i = 0; i < SizeBuckets; ++i) {
        if (stats.accessBuckets[i] == 0) continue;
        std::snprintf(text, sizeof(text), " %s %llu", SizeBucketNames[i], static_cast<unsigned long long>(stats.accessBuckets[i]));
        report += text;
    }
    report += "; top by wait (queue packet site frames: count/ms, unchanged open/unsignaled/signaled, changed, stores, avg range/access):";
    for (std::size_t i = 0; i < hot.size() && i < 10; ++i) {
        const auto& key = *hot[i].first;
        const auto& totals = *hot[i].second;
        const auto& k = totals.outcomes;
        std::snprintf(text, sizeof(text), " [0x%x %s %s +0x%llx/+0x%llx/+0x%llx/+0x%llx/+0x%llx/+0x%llx: %llu/%.0fms u%llu/%llu/%llu c%llu s%llu %.0fK/%.0fK]", key.queue, PacketName(key.opcode).c_str(), GuestMemory::ReadSiteName(key.site), key.frames[0], key.frames[1], key.frames[2], key.frames[3], key.frames[4], key.frames[5], count(totals.count), totals.waitedMs, count(k.unchangedOpen), count(k.unchangedPending), count(k.unchangedSignaled), count(k.changed), count(k.stores), totals.rangeBytes / 1024.0 / totals.count, totals.accessBytes / 1024.0 / totals.count);
        report += text;
    }
    aps5::LogErr( "%s\n", report.c_str());
    stats = HookSyncStats{};
}

class HookSyncScope {
public:
    HookSyncScope(const Recorder& recorder, std::uint64_t address, std::size_t bytes) : enabled(HookSyncProfiled()), address(address), bytes(bytes) {
        if (!enabled) return;
        info = recorder.DescribePendingWrite(address, bytes);
        const auto packet = GuestMemory::CurrentPacket();
        key = HookSyncKey{packet.queue, packet.opcode, GuestMemory::CurrentReadSite(), {}};

        GuestMemory::CaptureCallerOffsets(key.frames, 1);

        if (key.site != GuestMemory::ReadSite::Store && bytes <= CompareLimit && GuestMemory::Accessible(reinterpret_cast<const void*>(address), bytes)) {
            before.resize(bytes);
            std::memcpy(before.data(), reinterpret_cast<const void*>(address), bytes);
        }
        start = std::chrono::steady_clock::now();
    }
    ~HookSyncScope() {
        if (!enabled) return;
        const auto now = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration<double, std::milli>(now - start).count();
        auto& stats = HookSyncs();
        auto& totals = stats.byKey[key];
        ++totals.count;
        totals.waitedMs += ms;
        totals.accessBytes += bytes;
        ++stats.count;
        stats.waitedMs += ms;
        stats.accessBuckets[SizeBucket(bytes)] += 1;
        if (info.has_value()) {
            const auto rangeBytes = info->rangeEnd - info->rangeBegin;
            totals.rangeBytes += rangeBytes;
            stats.rangeBuckets[SizeBucket(rangeBytes)] += 1;
            if (info->open) ++stats.openTargets;
            if (info->signaled) ++stats.signaledTargets;
            stats.batchesFinished += info->batchesToFinish;
        }

        const auto outcome = [&]() -> std::uint64_t HookSyncOutcomes::* {
            if (key.site == GuestMemory::ReadSite::Store) return &HookSyncOutcomes::stores;
            if (before.empty() || !GuestMemory::Accessible(reinterpret_cast<const void*>(address), bytes)) return &HookSyncOutcomes::unchecked;
            if (std::memcmp(before.data(), reinterpret_cast<const void*>(address), bytes) != 0) return &HookSyncOutcomes::changed;

            if (!info.has_value() || info->signaled) return &HookSyncOutcomes::unchangedSignaled;
            return info->open ? &HookSyncOutcomes::unchangedOpen : &HookSyncOutcomes::unchangedPending;
        }();
        ++(totals.outcomes.*outcome);
        ++(stats.outcomes.*outcome);
        if (now - stats.lastReport > std::chrono::seconds(10)) ReportHookSyncs(stats);
    }
    HookSyncScope(const HookSyncScope&) = delete;
    HookSyncScope& operator=(const HookSyncScope&) = delete;

private:
    static constexpr std::size_t CompareLimit = 65536;
    bool enabled;
    std::uint64_t address;
    std::size_t bytes;
    std::optional<Recorder::PendingWriteInfo> info;
    HookSyncKey key{};
    std::vector<std::byte> before;
    std::chrono::steady_clock::time_point start;
};

struct RecordedStoreTotals {
    std::uint64_t count = 0;
    double waitedMs = 0;
    std::uint64_t batchesFinished = 0;
    std::uint64_t cpuWrittenYes = 0, cpuWrittenNo = 0, unclassified = 0, publishOnly = 0;
    std::uint64_t changed = 0, unchanged = 0, unchecked = 0;
    std::uint64_t imagesDead = 0, imagesLive = 0;
    std::uint64_t skipped = 0, flushed = 0, evicted = 0;
};

struct RecordedStoreStats {
    std::mutex mutex;
    std::map<HookSyncKey, RecordedStoreTotals> byKey;
    RecordedStoreTotals totals;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

RecordedStoreStats& RecordedStoreSyncs() {
    static RecordedStoreStats stats;
    return stats;
}

void ReportRecordedStoreSyncs(RecordedStoreStats& stats) {
    std::vector<std::pair<const HookSyncKey*, const RecordedStoreTotals*>> hot;
    for (const auto& [key, totals] : stats.byKey) hot.emplace_back(&key, &totals);
    std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return std::tie(a.second->waitedMs, a.second->skipped) > std::tie(b.second->waitedMs, b.second->skipped); });
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto& t = stats.totals;
    const auto skips = StorageTexture::TakeHookSkipCounts();
    char text[640];
    std::snprintf(text, sizeof(text), "[hooksync] recorded-store syncs (10 s): %llu waited %.0f ms, %llu batches in flight at them; skipped (cpu-written blocks) %llu / flushed %llu / evicted stale %llu; flushed later after a skip %llu (by the same site %llu); C4 every block CPU-written: yes %llu / no %llu / unclassified %llu (publish only %llu); bytes after the wait: changed %llu, unchanged %llu, unchecked %llu; images overlapped: dead %llu / live %llu (no consumer proof for %llu presents); by queue packet site (count/ms, skipped, cpu-written yes/no, changed/unchanged, dead/live):", count(t.count), t.waitedMs, count(t.batchesFinished), count(t.skipped), count(t.flushed), count(t.evicted), count(skips.flushedAfterSkip), count(skips.flushedAfterSkipSameSite), count(t.cpuWrittenYes), count(t.cpuWrittenNo), count(t.unclassified), count(t.publishOnly), count(t.changed), count(t.unchanged), count(t.unchecked), count(t.imagesDead), count(t.imagesLive), count(StorageTexture::DeadImagePresents));
    std::string report = text;
    for (std::size_t i = 0; i < hot.size() && i < 10; ++i) {
        const auto& key = *hot[i].first;
        const auto& k = *hot[i].second;
        std::snprintf(text, sizeof(text), " [0x%x %s %s +0x%llx/+0x%llx/+0x%llx: %llu/%.0fms k%llu y%llu/n%llu c%llu/u%llu d%llu/l%llu]", key.queue, PacketName(key.opcode).c_str(), GuestMemory::ReadSiteName(key.site), key.frames[0], key.frames[1], key.frames[2], count(k.count), k.waitedMs, count(k.skipped), count(k.cpuWrittenYes), count(k.cpuWrittenNo), count(k.changed), count(k.unchanged), count(k.imagesDead), count(k.imagesLive));
        report += text;
    }
    aps5::LogErr( "%s\n", report.c_str());
    stats.byKey.clear();
    stats.totals = {};
}

[[gnu::always_inline]] inline HookSyncKey RecordedStoreKey() {
    const auto packet = GuestMemory::CurrentPacket();
    HookSyncKey key{packet.queue, packet.opcode, GuestMemory::CurrentReadSite(), {}};
    GuestMemory::CaptureCallerOffsets(key.frames, 1);
    return key;
}

void CountRecordedStoreSkip(const HookSyncKey& key, std::size_t evicted) {
    auto& stats = RecordedStoreSyncs();
    std::lock_guard lock(stats.mutex);
    for (auto* totals : {&stats.byKey[key], &stats.totals}) {
        ++totals->skipped;
        totals->evicted += evicted;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - stats.lastReport > std::chrono::seconds(10)) {
        stats.lastReport = now;
        ReportRecordedStoreSyncs(stats);
    }
}

class RecordedStoreSyncScope {
public:
    RecordedStoreSyncScope(const Recorder& recorder, std::uint64_t address, std::size_t bytes, const StorageTexture::AccessClassification& classification, bool stored, const HookSyncKey& key) : enabled(HookSyncProfiled()), address(address), bytes(bytes), classification(classification), stored(stored), key(key) {
        if (!enabled) return;
        batchesInFlight = recorder.InFlightBatches();
        if (key.site != GuestMemory::ReadSite::Store && bytes <= CompareLimit && GuestMemory::Accessible(reinterpret_cast<const void*>(address), bytes)) {
            before.resize(bytes);
            std::memcpy(before.data(), reinterpret_cast<const void*>(address), bytes);
        }
        start = std::chrono::steady_clock::now();
    }
    ~RecordedStoreSyncScope() {
        if (!enabled) return;
        const auto now = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration<double, std::milli>(now - start).count();
        auto& stats = RecordedStoreSyncs();
        std::lock_guard lock(stats.mutex);
        const auto add = [&](RecordedStoreTotals& totals) {
            ++totals.count;
            totals.waitedMs += ms;
            totals.batchesFinished += batchesInFlight;
            if (stored) ++totals.flushed;
            if (classification.images == 0) ++totals.publishOnly;
            else if (!classification.checked) ++totals.unclassified;
            else ++(classification.allCpuWritten ? totals.cpuWrittenYes : totals.cpuWrittenNo);
            totals.imagesDead += classification.dead;
            totals.imagesLive += classification.live;
            if (before.empty() || !GuestMemory::Accessible(reinterpret_cast<const void*>(address), bytes)) ++totals.unchecked;
            else if (std::memcmp(before.data(), reinterpret_cast<const void*>(address), bytes) != 0) ++totals.changed;
            else ++totals.unchanged;
        };
        add(stats.byKey[key]);
        add(stats.totals);
        if (now - stats.lastReport > std::chrono::seconds(10)) {
            stats.lastReport = now;
            ReportRecordedStoreSyncs(stats);
        }
    }
    RecordedStoreSyncScope(const RecordedStoreSyncScope&) = delete;
    RecordedStoreSyncScope& operator=(const RecordedStoreSyncScope&) = delete;

private:
    static constexpr std::size_t CompareLimit = 65536;
    bool enabled;
    std::uint64_t address;
    std::size_t bytes;
    StorageTexture::AccessClassification classification;
    bool stored;
    HookSyncKey key;
    std::size_t batchesInFlight = 0;
    std::vector<std::byte> before;
    std::chrono::steady_clock::time_point start;
};

void FlushForAccess(std::uint64_t address, std::size_t bytes) {
    if (DrawProfiled()) hookCalls.fetch_add(1, std::memory_order_relaxed);

    if (!QueuedLabelRanges().empty() && QueuedLabelOverlaps(address, bytes)) {
        if (completionDepth != 0 && HookCompletionGuard()) {
            queuedLabelHookInCompletion.fetch_add(1, std::memory_order_relaxed);
        } else if (auto* record = queuedLabelRecorder.load(std::memory_order_acquire); record != nullptr) {
            queuedLabelHookRecords.fetch_add(1, std::memory_order_relaxed);
            record();
        }
    }
    if (!HookSnapshotEnabled() || SnapshotOverlaps(address, bytes)) {
        if (DrawProfiled()) hookLocks.fetch_add(1, std::memory_order_relaxed);
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Hook);
        std::lock_guard gpu(GuestMemory::GpuMutex());
        if (auto* recorder = Recorder::Active(); recorder != nullptr && recorder->PendingWriteOverlaps(address, bytes)) {
            const HookSyncScope attribution(*recorder, address, bytes);
            Recorder::CountSync(1);

            recorder->SyncThrough(address, bytes, true);
        }
    }

    const auto site = GuestMemory::CurrentReadSite();
    const auto scope = site != GuestMemory::ReadSite::Store ? PublishScope::Whole : completionDepth != 0 ? PublishScope::None : PublishScope::PartialUnits;
    bool published = false;

    StorageTexture::AccessClassification classification{};
    if (HookSyncProfiled()) StorageTexture::ClassifyAccess(address, bytes, classification);

    bool stored = false;
    std::size_t images = 0, evicted = 0;
    const bool kept = !HookFlushCpuBlocks() && StorageTexture::AccessKeptByCpu(address, bytes, &images, &evicted);
    if (kept || (images == 0 && !HookFlushCpuBlocks())) {
        published = StorageTexture::PublishShadowsOnly(address, bytes, scope);
        if (kept && HookSyncProfiled()) CountRecordedStoreSkip(RecordedStoreKey(), evicted);
    } else {
        stored = StorageTexture::FlushPending(address, bytes, nullptr, "memory access", scope, &published);
    }
    if (stored || published) {

        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Hook);
        std::lock_guard gpu(GuestMemory::GpuMutex());
        if (auto* recorder = Recorder::Active(); recorder != nullptr) {
            const RecordedStoreSyncScope attribution(*recorder, address, bytes, classification, stored, HookSyncProfiled() ? RecordedStoreKey() : HookSyncKey{});
            Recorder::CountSync(2);
            if (HookFullSync() || completionDepth != 0) recorder->Sync();
            else recorder->SyncThrough(address, bytes, true);
        }
    }
}

}

Recorder::Recorder(const Context& context, bool timelineSemaphores) : context(context), id(nextRecorderId.fetch_add(1)) {
    {
        std::lock_guard lock(liveRecordersMutex);
        liveRecorders.push_back(id);
    }

    GuestMemory::SetGpuUnlockHook(&ReleaseDeferredKeeps);
    if (ProcTable() && context.deviceProc != nullptr) {
        getFenceStatus = context.Function<PFN_vkGetFenceStatus>("vkGetFenceStatus");
        resetFences = context.Function<PFN_vkResetFences>("vkResetFences");
        waitForFences = context.Function<PFN_vkWaitForFences>("vkWaitForFences");
        cmdUpdateBuffer = context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer");
        beginCommandBuffer = context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
        endCommandBuffer = context.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
        queueSubmit = context.Function<PFN_vkQueueSubmit>("vkQueueSubmit");
        cmdPipelineBarrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    }
    if (!timelineSemaphores) return;

    VkSemaphoreTypeCreateInfoKHR type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
    type.initialValue = 0;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    const auto result = context.Function<PFN_vkCreateSemaphore>("vkCreateSemaphore")(context.device, &info, nullptr, &timeline);
    if (result != VK_SUCCESS) {
        aps5::LogErr( "[gpu] timeline semaphore creation failed (Vulkan result %d); drains wait under the GPU mutex\n", static_cast<int>(result));
        timeline = VK_NULL_HANDLE;
    }
}

Recorder::~Recorder() {
    try {
        Sync();
    } catch (const std::exception& error) {
        aps5::LogErr( "[gpu] recorder teardown: %s\n", error.what());
    }
    if (activeRecorder == this) {
        activeRecorder = nullptr;
        pendingWrites.store(nullptr, std::memory_order_release);
        publishGeneration.fetch_add(1, std::memory_order_release);
        pendingLabelSince.store(NoPendingLabel, std::memory_order_release);
        completionLabels.store(0, std::memory_order_release);
        writeBackCompletions.store(0, std::memory_order_release);
        workSinceSubmit.store(0, std::memory_order_release);
    }
    {

        std::lock_guard lock(liveRecordersMutex);
        liveRecorders.erase(std::remove(liveRecorders.begin(), liveRecorders.end(), id), liveRecorders.end());
    }

    while (WaitersOf(id).load(std::memory_order_acquire) != 0) std::this_thread::yield();

    if (!DeferredBatches().empty()) {
        auto own = std::move(DeferredBatches());
        DeferredBatches().clear();
        DestroyDeferred(std::move(own), false);
    }
    JoinReleaseThread();
    while (deferredPending.load(std::memory_order_acquire) != 0) std::this_thread::yield();
    {

        std::lock_guard tableLock(labelTableMutex);
        if (labelTableOwner == this) labelTableOwner = nullptr;
    }
    for (auto& [commands, fence] : spare) {
        context.Function<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(context.device, context.pool, 1, &commands);
        context.Function<PFN_vkDestroyFence>("vkDestroyFence")(context.device, fence, nullptr);
    }
    spare.clear();
    for (const auto pool : sparePools) context.Function<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(context.device, pool, nullptr);
    sparePools.clear();

    if (timeline != VK_NULL_HANDLE) context.Function<PFN_vkDestroySemaphore>("vkDestroySemaphore")(context.device, timeline, nullptr);
    timeline = VK_NULL_HANDLE;
}

std::optional<std::chrono::steady_clock::time_point> Recorder::PendingLabelSince() {
    const auto since = pendingLabelSince.load(std::memory_order_acquire);
    if (since == NoPendingLabel) return std::nullopt;
    return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(since));
}

std::uint64_t Recorder::WriteGeneration() {
    return writeGeneration.load(std::memory_order_acquire);
}

std::uint64_t Recorder::PublishGeneration() {
    return publishGeneration.load(std::memory_order_acquire);
}

std::uint64_t Recorder::PendingCompletionLabels() {
    return completionLabels.load(std::memory_order_acquire);
}

std::uint64_t Recorder::PendingWriteBackCompletions() {
    return writeBackCompletions.load(std::memory_order_acquire);
}

std::uint64_t Recorder::RecordedWorkSinceSubmit() {
    return workSinceSubmit.load(std::memory_order_relaxed);
}

void Recorder::CountRecordedWork() {
    workSinceSubmit.fetch_add(1, std::memory_order_relaxed);
}

Recorder* Recorder::Active() {
    return activeRecorder;
}

void Recorder::Activate() {
    activeRecorder = this;
    {
        std::lock_guard tableLock(labelTableMutex);
        labelTableOwner = this;
    }
    GuestMemory::SetFlushHook(&FlushForAccess);
}

std::optional<Recorder::LabelHit> Recorder::LookupLabel(std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp, LabelRefusal* refusal) {
    std::lock_guard tableLock(labelTableMutex);
    if (labelTableOwner == nullptr) return std::nullopt;
    return labelTableOwner->lookupLabel(address, bytes, afterStamp, refusal);
}

bool Recorder::WideLabelIn(std::uint64_t address, std::size_t bytes) {
    std::lock_guard tableLock(labelTableMutex);
    return labelTableOwner != nullptr && labelTableOwner->wideLabelInLocked(address, bytes);
}

bool Recorder::wideLabelInLocked(std::uint64_t address, std::size_t bytes) const {
    if (wideLabels.empty() || bytes == 0) return false;
    const auto end = address + bytes;
    for (auto it = wideLabels.lower_bound(address >= wideLabelBytes ? address - wideLabelBytes + 1 : 0); it != wideLabels.end() && it->first < end; ++it) {
        if (it->second > address) return true;
    }
    return false;
}

std::optional<std::uint64_t> Recorder::LookupLabelValue(std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp) {
    const auto hit = LookupLabel(address, bytes, afterStamp);
    if (!hit.has_value()) return std::nullopt;
    return hit->value;
}

bool Recorder::LateTrust() {
    return LateTrustEnabled();
}

Recorder::LateStatistics Recorder::LateCounts() {
    return lateCounts;
}

void Recorder::CloseLabelGroup(std::uint64_t trackerGeneration) {
    if (LabelGroupDwords().empty()) return;
    auto group = std::move(LabelGroupDwords());
    LabelGroupDwords().clear();
    if (trackerGeneration == 0) return;
    std::lock_guard tableLock(labelTableMutex);
    if (labelTableOwner == nullptr) return;
    auto& recorded = labelTableOwner->labels;
    for (const auto& [dword, stamp] : group) {

        const auto found = recorded.find(dword);
        if (found != recorded.end() && found->second.stamp == stamp && found->second.batch != nullptr && !found->second.batch->submitted) found->second.generation = trackerGeneration;
    }

    writeGeneration.fetch_add(1, std::memory_order_release);
}

void Recorder::NoteQueuedLabel(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue) {
    if (bytes.size() < 4 || address % 4 != 0) return;

    QueuedLabelRanges().emplace_back(address, address + bytes.size());
    queuedLabelsNoted.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard tableLock(labelTableMutex);
    if (labelTableOwner == nullptr) return;
    auto& recorded = labelTableOwner->labels;
    auto& table = SeparateQueuedLabels() ? labelTableOwner->queuedLabels : recorded;
    for (std::size_t offset = 0; offset + 4 <= bytes.size(); offset += 4) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, 4);
        if (const auto found = recorded.find(address + offset); found != recorded.end() && found->second.batch != nullptr) queuedLabelsOverRecorded.fetch_add(1, std::memory_order_relaxed);
        table.insert_or_assign(address + offset, LabelEntry{value, queue, stamp, nullptr});
    }
    labelTableOwner->recordedLabels.store(recorded.size(), std::memory_order_relaxed);
}

void Recorder::ForgetQueuedLabels() {
    if (QueuedLabelRanges().empty()) return;
    auto ranges = std::move(QueuedLabelRanges());
    QueuedLabelRanges().clear();
    std::lock_guard tableLock(labelTableMutex);
    if (labelTableOwner == nullptr) return;
    auto& recorded = labelTableOwner->labels;
    auto& queued = labelTableOwner->queuedLabels;
    const auto tag = GuestMemory::GpuLockThreadTag();
    for (const auto& [begin, end] : ranges) {
        for (auto dword = begin; dword + 4 <= end; dword += 4) {
            if (const auto own = queued.find(dword); own != queued.end() && own->second.queue == tag) queued.erase(own);
            const auto found = recorded.find(dword);
            if (found != recorded.end() && found->second.batch == nullptr) recorded.erase(found);
        }
    }
    labelTableOwner->recordedLabels.store(recorded.size(), std::memory_order_relaxed);
}

void Recorder::SetQueuedLabelRecorder(void (*recorder)()) {
    queuedLabelRecorder.store(recorder, std::memory_order_release);
}

Recorder::StoreStatistics Recorder::StoreCounts() {
    return StoreStatistics{storeCount.load(std::memory_order_relaxed), storeRuns.load(std::memory_order_relaxed), storesJoined.load(std::memory_order_relaxed), storesReplaced.load(std::memory_order_relaxed), storeWawBarriers.load(std::memory_order_relaxed), storeJoinsRefused.load(std::memory_order_relaxed), queuedLabelsNoted.load(std::memory_order_relaxed), queuedLabelsOverRecorded.load(std::memory_order_relaxed), queuedLabelHits.load(std::memory_order_relaxed), queuedLabelHookRecords.load(std::memory_order_relaxed), queuedLabelHookInCompletion.load(std::memory_order_relaxed), keyStoreCount.load(std::memory_order_relaxed), keyStoreRuns.load(std::memory_order_relaxed), keyStoreRunsForWriter.load(std::memory_order_relaxed), keyStoresJoined.load(std::memory_order_relaxed), storeRunsAtSubmit.load(std::memory_order_relaxed), storeRunsForced.load(std::memory_order_relaxed)};
}

std::uint64_t Recorder::ThreadHookWaits() {
    return hookRealWaits;
}

bool Recorder::SnapshotWriteOverlaps(std::uint64_t address, std::size_t bytes) {
    return AgcDriver::Graphics::SnapshotOverlaps(address, bytes);
}

std::shared_ptr<const Recorder::WriteRanges> Recorder::PendingWriteSnapshot() {
    return pendingWrites.load(std::memory_order_acquire);
}

bool Recorder::SnapshotOverlaps(const WriteRanges* snapshot, std::uint64_t address, std::size_t bytes) {
    return AgcDriver::Graphics::SnapshotOverlaps(snapshot, address, bytes);
}

void Recorder::CountSync(int source, const void* site) {
    if (source < 0 || source >= 5) return;
    ++syncCounts[source];
    announcedSource = source;
    announcedSite = site;
}

void Recorder::AnnounceSyncSite(const void* site) {
    announcedSite = site;
}

double Recorder::ThreadWaitedMs() {
    return threadWaitedMs;
}

std::uint64_t Recorder::ReapsWithWork() {
    return holdCounters.reapsWithWork;
}

VkCommandBuffer Recorder::Commands(VkAccessFlags* coveredAccess) {
    ensureOpen();

    if (open->renderPass.open) endOpenRenderPass();
    if (open->run.open && !LabelRunsPerBatch()) closeStoreRun();
    if (coveredAccess != nullptr) *coveredAccess = open->coveredAccess;
    open->coveredAccess = 0;
    return open->commands;
}

void Recorder::MarkCovered(VkAccessFlags access) {
    if (open != nullptr) open->coveredAccess = access;
}

bool Recorder::ContinuesRenderPass(std::uint64_t key) const {
    return open != nullptr && open->renderPass.open && open->renderPass.continuable && open->renderPass.key == key;
}

std::uint64_t Recorder::OpenPassKey() const {
    return open != nullptr && open->renderPass.open && open->renderPass.continuable ? open->renderPass.key : 0;
}

VkCommandBuffer Recorder::CommandsInRenderPass() {
    Require(open != nullptr && open->renderPass.open, "no render pass is open in the recorder");
    return open->commands;
}

void Recorder::LeaveRenderPassOpen(std::uint64_t key, std::uint32_t timing, bool continuable) {
    Require(open != nullptr, "no batch is open for the render pass");
    auto& pass = open->renderPass;
    if (!pass.open) pass.timing = timing;
    pass.open = true;
    pass.key = key;
    pass.continuable = continuable;
}

void Recorder::endOpenRenderPass() {
    auto& pass = open->renderPass;
    context.Resolved(&DeviceFunctions::cmdEndRenderPass, "vkCmdEndRenderPass")(open->commands);

    recordBarrier(open->commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    CountBarriers(CommandClass::Draw);
    EndGpuTiming(pass.timing);
    open->coveredAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    open->hostReadOwed = true;
    pass = {};
}

void Recorder::QueueKeyStore(VkBuffer buffer, VkDeviceSize first, VkDeviceSize last, std::shared_ptr<void> seed, std::uint64_t begin, std::uint64_t end) {
    ensureOpen();

    FlushStoresOverlapping(begin, static_cast<std::size_t>(end - begin));
    keyStoreCount.fetch_add(1, std::memory_order_relaxed);
    if (seed != nullptr) open->kept.push_back(seed);

    for (const auto& store : open->keyStores) {
        if (store.buffer == buffer && store.first == first && store.last == last) {
            keyStoresJoined.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    open->keyStores.push_back({buffer, first, last, std::move(seed), begin, end});
    if (KeyStoresEach()) recordKeyStores(false);
}

bool Recorder::QueuedKeyStoreOverlaps(std::uint64_t address, std::size_t bytes) const {
    if (open == nullptr || bytes == 0) return false;
    const auto end = address + bytes;
    return std::any_of(open->keyStores.begin(), open->keyStores.end(), [&](const Batch::KeyStore& store) { return address < store.end && store.begin < end; });
}

bool Recorder::AnyQueuedKeyStore(const std::function<bool(std::uint64_t, std::uint64_t)>& overlaps) const {
    if (open == nullptr) return false;
    return std::any_of(open->keyStores.begin(), open->keyStores.end(), [&](const Batch::KeyStore& store) { return overlaps(store.begin, store.end); });
}

void Recorder::FlushKeyStores() {
    if (open == nullptr || open->keyStores.empty()) return;
    recordKeyStores(true);
}

void Recorder::recordKeyStores(bool forWriter) {
    auto stores = std::move(open->keyStores);
    open->keyStores.clear();
    if (stores.empty()) return;
    const auto commands = Commands();
    const auto timing = beginTiming(ClassKey(CommandClass::DccKeyStore));

    recordBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    const auto fill = context.Resolved(&DeviceFunctions::cmdFillBuffer, "vkCmdFillBuffer");
    const auto copy = context.Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer");
    std::uint64_t bytes = 0;
    for (const auto& store : stores) {
        const VkDeviceSize fillBegin = std::min((store.first + 3) & ~VkDeviceSize{3}, store.last);
        const VkDeviceSize fillEnd = std::max(store.last & ~VkDeviceSize{3}, fillBegin);
        if (fillEnd > fillBegin) fill(commands, store.buffer, fillBegin, fillEnd - fillBegin, 0xffffffffu);
        if (store.seed != nullptr) {
            VkBufferCopy copies[2];
            std::uint32_t copyCount = 0;
            if (fillBegin > store.first) copies[copyCount++] = {0, store.first, fillBegin - store.first};
            if (store.last > fillEnd) copies[copyCount++] = {0, fillEnd, store.last - fillEnd};
            if (copyCount != 0) copy(commands, static_cast<Buffer*>(store.seed.get())->Handle(), store.buffer, copyCount, copies);
        }
        bytes += store.last - store.first;
    }
    recordBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT);
    CountBarriers(CommandClass::DccKeyStore, 2);
    EndGpuTiming(timing, bytes);
    open->coveredAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    keyStoreRuns.fetch_add(1, std::memory_order_relaxed);
    if (forWriter) keyStoreRunsForWriter.fetch_add(1, std::memory_order_relaxed);
}

void Recorder::ensureOpen() {
    GuestMemory::AssertGpuLockHeld("Recorder::Commands");
    if (open == nullptr) {
        auto batch = std::make_unique<Batch>();
        try {
            if (!spare.empty()) {

                std::tie(batch->commands, batch->fence) = spare.back();
                spare.pop_back();
            } else {
                VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                allocation.commandPool = context.pool;
                allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocation.commandBufferCount = 1;
                Check(context.Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(context.device, &allocation, &batch->commands), "vkAllocateCommandBuffers recorder");
                VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                Check(context.Function<PFN_vkCreateFence>("vkCreateFence")(context.device, &info, nullptr, &batch->fence), "vkCreateFence recorder");
            }
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            Check(function(beginCommandBuffer, "vkBeginCommandBuffer")(batch->commands, &begin), "vkBeginCommandBuffer recorder");
        } catch (...) {
            release(*batch);
            throw;
        }
        batch->queue = GuestMemory::GpuLockThreadTag();
        if (countingSamples) beginSamples(*batch);
        open = std::move(batch);

        if (BatchStampsEnabled()) open->batchTiming = beginTiming(BatchTimingKey);
    }
}

void Recorder::RecordStore(VkBuffer buffer, VkDeviceSize offset, std::span<const std::byte> bytes, std::uint64_t address) {
    if (bytes.empty()) return;
    ensureOpen();
    auto& run = open->run;
    const VkDeviceSize end = offset + bytes.size();
    storeCount.fetch_add(1, std::memory_order_relaxed);
    if (LabelRunsPerBatch()) {

        if (run.queued.empty()) storeRuns.fetch_add(1, std::memory_order_relaxed);
        else if (auto& last = run.queued.back(); last.buffer == buffer) {
            const VkDeviceSize lastEnd = last.offset + last.bytes.size();
            if (offset >= last.offset && end <= lastEnd) {
                std::memcpy(last.bytes.data() + (offset - last.offset), bytes.data(), bytes.size());
                storesReplaced.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (offset == lastEnd && last.bytes.size() + bytes.size() <= 65536) {
                last.bytes.insert(last.bytes.end(), bytes.begin(), bytes.end());
                storesJoined.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        run.queued.push_back({buffer, offset, std::vector<std::byte>(bytes.begin(), bytes.end()), address});
        return;
    }

    if (!open->keyStores.empty()) recordKeyStores(true);
    if (open->renderPass.open) endOpenRenderPass();
    open->coveredAccess = 0;

    const auto overlapsRecorded = [&] { return std::any_of(run.recorded.begin(), run.recorded.end(), [&](const auto& store) { return std::get<0>(store) == buffer && offset < std::get<2>(store) && std::get<1>(store) < end; }); };
    if (!run.open) {
        run.timing = beginTiming(ClassKey(CommandClass::LabelRun));
        run.storedBytes = 0;

        recordBarrier(open->commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        CountBarriers(CommandClass::LabelRun);
        run.open = true;
        storeRuns.fetch_add(1, std::memory_order_relaxed);
    } else if (run.buffer == buffer && !run.bytes.empty()) {
        const VkDeviceSize pendingEnd = run.offset + run.bytes.size();
        if (offset >= run.offset && end <= pendingEnd) {
            std::memcpy(run.bytes.data() + (offset - run.offset), bytes.data(), bytes.size());
            storesReplaced.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (offset == pendingEnd && run.bytes.size() + bytes.size() <= 65536) {

            if (!JoinWawCheck() || !overlapsRecorded()) {
                run.bytes.insert(run.bytes.end(), bytes.begin(), bytes.end());
                storesJoined.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            storeJoinsRefused.fetch_add(1, std::memory_order_relaxed);
        }
    }
    flushPendingStore();
    const bool waw = overlapsRecorded();
    if (waw) {
        recordBarrier(open->commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        CountBarriers(CommandClass::LabelRun);
        storeWawBarriers.fetch_add(1, std::memory_order_relaxed);
    }
    run.buffer = buffer;
    run.offset = offset;
    run.bytes.assign(bytes.begin(), bytes.end());
    if (!LabelRunsEnabled()) closeStoreRun();
}

void Recorder::recordBarrier(VkCommandBuffer commands, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) const {
    if (cmdPipelineBarrier == nullptr) {
        RecordMemoryBarrier(context, commands, sourceStage, destinationStage, sourceAccess, destinationAccess);
        return;
    }
    const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, sourceAccess, destinationAccess};
    cmdPipelineBarrier(commands, sourceStage, destinationStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void Recorder::flushPendingStore() {
    auto& run = open->run;
    if (run.bytes.empty()) return;
    function(cmdUpdateBuffer, "vkCmdUpdateBuffer")(open->commands, run.buffer, run.offset, run.bytes.size(), run.bytes.data());
    run.recorded.emplace_back(run.buffer, run.offset, run.offset + run.bytes.size());
    run.storedBytes += run.bytes.size();
    run.bytes.clear();
}

bool Recorder::closeStoreRun(bool atSubmit) {
    auto& run = open->run;
    if (LabelRunsPerBatch()) {
        auto stores = std::move(run.queued);
        run.queued.clear();
        if (stores.empty()) return false;

        if (!open->keyStores.empty()) recordKeyStores(true);
        if (open->renderPass.open) endOpenRenderPass();
        const auto commands = open->commands;
        run.timing = beginTiming(ClassKey(CommandClass::LabelRun));
        if (BarrierValidate()) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
            for (const auto& store : stores) ranges.emplace_back(store.address, store.address + store.bytes.size());
            NoteAccess(CommandClass::LabelRun, Access{{}, ranges, {}, VK_PIPELINE_STAGE_TRANSFER_BIT});
        }

        if (atSubmit) recordBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT);
        else recordBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        std::uint32_t barriers = 2;
        std::uint64_t storedBytes = 0;
        const auto update = function(cmdUpdateBuffer, "vkCmdUpdateBuffer");
        for (const auto& store : stores) {
            const VkDeviceSize end = store.offset + store.bytes.size();

            if (std::any_of(run.recorded.begin(), run.recorded.end(), [&](const auto& earlier) { return std::get<0>(earlier) == store.buffer && store.offset < std::get<2>(earlier) && std::get<1>(earlier) < end; })) {
                recordBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                ++barriers;
                storeWawBarriers.fetch_add(1, std::memory_order_relaxed);
            }
            update(commands, store.buffer, store.offset, store.bytes.size(), store.bytes.data());
            run.recorded.emplace_back(store.buffer, store.offset, end);
            storedBytes += store.bytes.size();
        }
        if (atSubmit) {
            recordBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            open->coveredAccess = 0;
            storeRunsAtSubmit.fetch_add(1, std::memory_order_relaxed);
        } else {
            recordBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
            open->coveredAccess = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            storeRunsForced.fetch_add(1, std::memory_order_relaxed);
        }
        CountBarriers(CommandClass::LabelRun, barriers);
        EndGpuTiming(run.timing, storedBytes);
        run.timing = NoTiming;
        run.recorded.clear();
        return atSubmit;
    }
    flushPendingStore();

    recordBarrier(open->commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    CountBarriers(CommandClass::LabelRun);
    EndGpuTiming(run.timing, run.storedBytes);
    run.timing = NoTiming;
    open->coveredAccess = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    run.open = false;
    run.buffer = VK_NULL_HANDLE;
    run.recorded.clear();
    return false;
}

bool Recorder::QueuedStoreOverlaps(std::uint64_t address, std::size_t bytes) const {
    if (open == nullptr || bytes == 0) return false;
    const auto end = address + bytes;
    return std::any_of(open->run.queued.begin(), open->run.queued.end(), [&](const Batch::StoreRun::Queued& store) { return address < store.address + store.bytes.size() && store.address < end; });
}

bool Recorder::AnyQueuedStore(const std::function<bool(std::uint64_t, std::uint64_t)>& overlaps) const {
    if (open == nullptr) return false;
    return std::any_of(open->run.queued.begin(), open->run.queued.end(), [&](const Batch::StoreRun::Queued& store) { return overlaps(store.address, store.address + store.bytes.size()); });
}

void Recorder::FlushStores() {
    if (open == nullptr || open->run.queued.empty()) return;
    closeStoreRun(false);
}

namespace {

constexpr std::uint32_t MaxTimedRanges = 512;
constexpr std::size_t CommandClasses = static_cast<std::size_t>(Recorder::CommandClass::Count);
constexpr const char* CommandClassNames[CommandClasses] = {"dispatch-lead", "dispatch-trail", "indirect-args", "label-run", "fill", "fill-clear", "copy", "staging-in", "staging-out", "draw", "storage-upload", "storage-writeback", "dcc-clear", "dcc-keys", "present-blit", "shadow-publish", "template-refresh"};

std::atomic<std::uint64_t> classBarriers[CommandClasses]{};
std::atomic<std::uint64_t> classMerged[CommandClasses]{};

constexpr std::size_t HazardKinds = 5;
constexpr const char* HazardNames[HazardKinds] = {"raw", "waw", "war", "image", "conservative"};
std::atomic<std::uint64_t> validateEmitted[CommandClasses]{}, validateSkipped[CommandClasses]{}, validateKinds[HazardKinds]{};
std::atomic<std::uint64_t> validateBatchEnds{0};
std::atomic<std::int64_t> traceBarriersLeft{[] {
    const char* text = std::getenv("APS5_TRACE_BARRIERS");
    return text != nullptr ? static_cast<std::int64_t>(std::strtoll(text, nullptr, 10)) : std::int64_t{0};
}()};
std::atomic<std::uint64_t> timingPresents{0};
std::atomic<std::uint64_t> presentSerial{0};
std::atomic<std::uint64_t> timingDropped{0};
struct TimingTotals { std::uint64_t count = 0; double ms = 0; std::uint64_t bytes = 0; };
std::mutex timingMutex;
std::map<std::uint64_t, TimingTotals> timingByKey;
double timingProgramMs = 0, timingClassMs = 0, timingUnionMs = 0, timingBatchMs = 0;
std::uint64_t timingBatches = 0;

bool DrawOrGpuProfiled() {
    static const bool profiled = std::getenv("APS5_PROFILE_DRAW") != nullptr || Recorder::GpuTimingEnabled();
    return profiled;
}

void reportBarriers() {
    if (!DrawOrGpuProfiled()) return;
    static auto lastReport = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(10)) return;
    lastReport = now;
    std::uint64_t total = 0, merged = 0;
    std::string line, mergedLine;
    for (std::size_t i = 0; i < CommandClasses; ++i) {
        const auto count = classBarriers[i].exchange(0, std::memory_order_relaxed);
        total += count;
        char text[64];
        std::snprintf(text, sizeof(text), " %s %llu", CommandClassNames[i], static_cast<unsigned long long>(count));
        line += text;
        const auto mergedCount = classMerged[i].exchange(0, std::memory_order_relaxed);
        if (mergedCount == 0) continue;
        merged += mergedCount;
        std::snprintf(text, sizeof(text), " %s %llu", CommandClassNames[i], static_cast<unsigned long long>(mergedCount));
        mergedLine += text;
    }
    aps5::LogErr( "[barriers] %llu recorded (10 s) by class:%s; merged %llu:%s", static_cast<unsigned long long>(total), line.c_str(), static_cast<unsigned long long>(merged), mergedLine.c_str());
    if (Recorder::BarrierValidate()) {
        std::uint64_t emitted = 0, skipped = 0;
        std::string classes, kinds;
        for (std::size_t i = 0; i < CommandClasses; ++i) {
            const auto emittedCount = validateEmitted[i].exchange(0, std::memory_order_relaxed);
            const auto skippedCount = validateSkipped[i].exchange(0, std::memory_order_relaxed);
            emitted += emittedCount;
            skipped += skippedCount;
            if (emittedCount == 0 && skippedCount == 0) continue;
            char text[96];
            std::snprintf(text, sizeof(text), " %s %llu/%llu", CommandClassNames[i], static_cast<unsigned long long>(emittedCount), static_cast<unsigned long long>(skippedCount));
            classes += text;
        }
        for (std::size_t i = 0; i < HazardKinds; ++i) {
            char text[64];
            std::snprintf(text, sizeof(text), " %s %llu", HazardNames[i], static_cast<unsigned long long>(validateKinds[i].exchange(0, std::memory_order_relaxed)));
            kinds += text;
        }
        aps5::LogErr( "; validate: would emit %llu (%s) + %llu batch ends, would skip %llu; emitted/skipped by class:%s", static_cast<unsigned long long>(emitted), kinds.c_str() + 1, static_cast<unsigned long long>(validateBatchEnds.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(skipped), classes.c_str());
    }
    aps5::LogChar(aps5::LogStdErr, '\n');
}

}

bool Recorder::GpuTimingEnabled() {
    static const bool enabled = std::getenv("APS5_PROFILE_GPU") != nullptr;
    return enabled;
}

bool Recorder::BatchStampsEnabled() {
    return GpuTimingEnabled() || DrawProfiled();
}

void Recorder::CountBarriers(CommandClass which, std::uint32_t count) {
    if (!DrawOrGpuProfiled()) return;
    classBarriers[static_cast<std::size_t>(which)].fetch_add(count, std::memory_order_relaxed);
}

bool Recorder::MergeBarriers() {
    static const bool merge = std::getenv("APS5_FULL_BARRIERS") == nullptr && std::getenv("APS5_NO_BARRIER_ELISION") == nullptr;
    return merge;
}

void Recorder::CountMerged(CommandClass which) {
    if (!DrawOrGpuProfiled()) return;
    classMerged[static_cast<std::size_t>(which)].fetch_add(1, std::memory_order_relaxed);
}

bool Recorder::BarrierValidate() {
    static const bool validate = std::getenv("APS5_BARRIER_VALIDATE") != nullptr;
    return validate;
}

void Recorder::NoteAccess(CommandClass which, const Access& access) {
    if (!BarrierValidate()) return;
    ensureOpen();
    auto& tracker = open->tracker;
    const auto overlapsAny = [](const auto& ranges, const std::pair<std::uint64_t, std::uint64_t>& range) {
        return std::any_of(ranges.begin(), ranges.end(), [&](const auto& other) { return range.first < other.end && other.begin < range.second; });
    };
    std::size_t kind = HazardKinds;
    if (access.conservative) kind = 4;
    for (const auto& range : access.reads) {
        if (kind != HazardKinds) break;
        if (overlapsAny(tracker.writes, range)) kind = 0;
    }
    for (const auto& range : access.writes) {
        if (kind != HazardKinds) break;
        if (overlapsAny(tracker.writes, range)) kind = 1;
        else if (overlapsAny(tracker.reads, range)) kind = 2;
    }
    for (const auto& [image, written] : access.images) {
        if (kind != HazardKinds) break;
        if (std::any_of(tracker.images.begin(), tracker.images.end(), [&](const auto& seen) { return seen.image == image && (seen.written || written); })) kind = 3;
    }
    const auto index = static_cast<std::size_t>(which);
    if (kind != HazardKinds) {
        validateEmitted[index].fetch_add(1, std::memory_order_relaxed);
        validateKinds[kind].fetch_add(1, std::memory_order_relaxed);
        tracker.reads.clear();
        tracker.writes.clear();
        tracker.images.clear();
    } else {
        validateSkipped[index].fetch_add(1, std::memory_order_relaxed);
        if (traceBarriersLeft.load(std::memory_order_relaxed) > 0 && traceBarriersLeft.fetch_sub(1, std::memory_order_relaxed) > 0) {
            const auto first = [](const auto& ranges) { return ranges.empty() ? std::pair<std::uint64_t, std::uint64_t>{0, 0} : std::pair<std::uint64_t, std::uint64_t>{ranges.front().first, ranges.front().second}; };
            const auto reads = first(access.reads);
            const auto writes = first(access.writes);
            aps5::LogErr( "[barriers] would skip %s: reads %zu (first 0x%llx+0x%llx), writes %zu (first 0x%llx+0x%llx), images %zu; since the last barrier: %zu reads, %zu writes, %zu images\n", CommandClassNames[index], access.reads.size(), static_cast<unsigned long long>(reads.first), static_cast<unsigned long long>(reads.second - reads.first), access.writes.size(), static_cast<unsigned long long>(writes.first), static_cast<unsigned long long>(writes.second - writes.first), access.images.size(), tracker.reads.size(), tracker.writes.size(), tracker.images.size());
        }
    }
    for (const auto& [begin, end] : access.reads) tracker.reads.push_back({begin, end, access.stages});
    for (const auto& [begin, end] : access.writes) tracker.writes.push_back({begin, end, access.stages});
    for (const auto& [image, written] : access.images) tracker.images.push_back({image, written, access.stages});
}

void Recorder::CountPresent() {
    timingPresents.fetch_add(1, std::memory_order_relaxed);
    presentSerial.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t Recorder::Presents() {
    return presentSerial.load(std::memory_order_relaxed);
}

void Recorder::AddGpuTiming(CommandClass which, double nanoseconds, std::uint64_t bytes) {
    std::lock_guard lock(timingMutex);
    auto& totals = timingByKey[ClassKey(which)];
    ++totals.count;
    totals.ms += nanoseconds / 1e6;
    totals.bytes += bytes;
    timingClassMs += nanoseconds / 1e6;
}

std::uint32_t Recorder::BeginGpuTiming(std::uint64_t key) {
    if (!GpuTimingEnabled()) return NoTiming;
    Commands();
    return beginTiming(key);
}

std::uint32_t Recorder::beginTiming(std::uint64_t key) {
    const bool full = GpuTimingEnabled();
    if (!full && !(key == BatchTimingKey && DrawProfiled())) return NoTiming;
    const auto commands = open->commands;
    const std::uint32_t queryCount = full ? MaxTimedRanges * 2 : 2;
    if (open->queries == VK_NULL_HANDLE) {
        if (!full && !sparePools.empty()) {
            open->queries = sparePools.back();
            sparePools.pop_back();
        } else {
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = queryCount;
            if (context.Function<PFN_vkCreateQueryPool>("vkCreateQueryPool")(context.device, &info, nullptr, &open->queries) != VK_SUCCESS) {
                open->queries = VK_NULL_HANDLE;
                return NoTiming;
            }
        }
        context.Function<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(commands, open->queries, 0, queryCount);
    }
    if (open->timedKeys.size() >= queryCount / 2) {
        timingDropped.fetch_add(1, std::memory_order_relaxed);
        return NoTiming;
    }
    const auto index = static_cast<std::uint32_t>(open->timedKeys.size());
    open->timedKeys.push_back(key);
    open->timedBytes.push_back(0);

    context.Function<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, open->queries, index * 2);
    return index;
}

void Recorder::EndGpuTiming(std::uint32_t index, std::uint64_t bytes) {
    if (index == NoTiming || open == nullptr || open->queries == VK_NULL_HANDLE) return;
    if (index < open->timedBytes.size()) open->timedBytes[index] += bytes;
    context.Function<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(open->commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, open->queries, index * 2 + 1);
}

void Recorder::CountSamples() {
    GuestMemory::AssertGpuLockHeld("Recorder::CountSamples");
    countingSamples = true;
    if (open == nullptr || open->samples != VK_NULL_HANDLE) return;
    if (open->renderPass.open) endOpenRenderPass();
    beginSamples(*open);
}

std::uint64_t Recorder::SamplesPassed() {
    return samplesPassed.load(std::memory_order_acquire);
}

void Recorder::beginSamples(Batch& batch) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_OCCLUSION;
    info.queryCount = 1;
    Check(context.Function<PFN_vkCreateQueryPool>("vkCreateQueryPool")(context.device, &info, nullptr, &batch.samples), "vkCreateQueryPool occlusion");
    context.Function<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(batch.commands, batch.samples, 0, 1);
    context.Function<PFN_vkCmdBeginQuery>("vkCmdBeginQuery")(batch.commands, batch.samples, 0, context.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0u);
}

void Recorder::readSamples(Batch& batch) {
    if (batch.samples == VK_NULL_HANDLE || !batch.submitted) return;
    std::uint64_t samples = 0;
    Check(context.Function<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(context.device, batch.samples, 0, 1, sizeof(samples), &samples, sizeof(samples), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults occlusion");
    samplesPassed.fetch_add(samples, std::memory_order_acq_rel);
}

void Recorder::readGpuTiming(Batch& batch) {
    if (batch.queries == VK_NULL_HANDLE || batch.timedKeys.empty()) return;
    std::vector<std::uint64_t> stamps(batch.timedKeys.size() * 2);
    const auto result = context.Function<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(context.device, batch.queries, 0, static_cast<std::uint32_t>(stamps.size()), stamps.size() * sizeof(std::uint64_t), stamps.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (result != VK_SUCCESS) return;
    const auto period = context.limits.timestampPeriod;
    if (!GpuTimingEnabled()) {
        if (batch.batchTiming == 0 && stamps[1] >= stamps[0]) {
            batch.gpuStartNs = static_cast<double>(stamps[0]) * period;
            batch.gpuEndNs = static_cast<double>(stamps[1]) * period;
        }
        return;
    }
    static auto lastReport = std::chrono::steady_clock::now();

    std::vector<std::pair<std::uint64_t, std::uint64_t>> intervals;
    intervals.reserve(batch.timedKeys.size());
    std::unique_lock lock(timingMutex);
    for (std::size_t i = 0; i < batch.timedKeys.size(); ++i) {
        const auto ns = static_cast<double>(stamps[i * 2 + 1] - stamps[i * 2]) * period;
        if (i == batch.batchTiming) {

            batch.gpuStartNs = static_cast<double>(stamps[i * 2]) * period;
            batch.gpuEndNs = static_cast<double>(stamps[i * 2 + 1]) * period;
            timingBatchMs += ns / 1e6;
            continue;
        }
        const auto key = batch.timedKeys[i];
        auto& totals = timingByKey[key];
        ++totals.count;
        totals.ms += ns / 1e6;
        totals.bytes += batch.timedBytes[i];
        (key >= ClassKey(CommandClass::DispatchLeading) && key < ClassKey(CommandClass::Count) ? timingClassMs : timingProgramMs) += ns / 1e6;
        intervals.emplace_back(stamps[i * 2], stamps[i * 2 + 1]);
    }
    std::sort(intervals.begin(), intervals.end());
    std::uint64_t coveredTicks = 0, unionEnd = 0;
    for (const auto& [begin, end] : intervals) {
        const auto from = std::max(begin, unionEnd);
        if (end > from) coveredTicks += end - from;
        unionEnd = std::max(unionEnd, end);
    }
    timingUnionMs += static_cast<double>(coveredTicks) * period / 1e6;
    ++timingBatches;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(10)) return;
    lastReport = now;
    const auto presents = timingPresents.exchange(0, std::memory_order_relaxed);
    const double perPresent = presents != 0 ? 1.0 / static_cast<double>(presents) : 0.0;
    std::vector<std::pair<std::uint64_t, TimingTotals>> hot;
    std::string classes;
    for (const auto& [key, totals] : timingByKey) {
        if (key >= ClassKey(CommandClass::DispatchLeading) && key < ClassKey(CommandClass::Count)) {
            char text[160];
            std::snprintf(text, sizeof(text), " %s x%llu %.1fms %.1fMiB (per present x%.1f %.2fms %.2fMiB)", CommandClassNames[key - ClassKey(CommandClass::DispatchLeading)], static_cast<unsigned long long>(totals.count), totals.ms, totals.bytes / 1048576.0, static_cast<double>(totals.count) * perPresent, totals.ms * perPresent, totals.bytes / 1048576.0 * perPresent);
            classes += text;
        } else {
            hot.emplace_back(key, totals);
        }
    }
    std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });

    aps5::LogErr( "[gputime] %.0f ms of GPU time in %llu batches over 10 s (batch %.0f ms first to last command; classes %.0f ms, all ranges %.0f ms, untimed %.0f ms outside every range; %llu presents; %llu ranges dropped at the %u cap); by program:", timingProgramMs, static_cast<unsigned long long>(timingBatches), timingBatchMs, timingClassMs, timingUnionMs, timingBatchMs - timingUnionMs, static_cast<unsigned long long>(presents), static_cast<unsigned long long>(timingDropped.exchange(0, std::memory_order_relaxed)), MaxTimedRanges);
    for (std::size_t i = 0; i < hot.size() && i < 12; ++i) aps5::LogErr( " 0x%llx x%llu %.0fms", static_cast<unsigned long long>(hot[i].first), static_cast<unsigned long long>(hot[i].second.count), hot[i].second.ms);
    aps5::LogErr( "; by class:%s\n", classes.c_str());
    timingByKey.clear();
    timingProgramMs = timingClassMs = timingUnionMs = timingBatchMs = 0;
    timingBatches = 0;
    lock.unlock();
    reportBarriers();
}

bool Recorder::FlipReadCheck() {
    static const bool enabled = std::getenv("APS5_FLIP_READ_CHECK") != nullptr;
    return enabled;
}

std::vector<Recorder::Completed> Recorder::CompletedBatches(std::uint64_t afterSerial, std::uint64_t throughSerial, std::size_t& missing) const {
    std::vector<Completed> found;
    missing = 0;
    if (throughSerial <= afterSerial) return found;
    std::lock_guard lock(completedMutex);
    for (auto serial = afterSerial + 1; serial <= throughSerial; ++serial) {
        const auto& entry = completed[serial % completed.size()];
        if (entry.serial == serial) found.push_back(entry);
        else ++missing;
    }
    return found;
}

std::uint64_t Recorder::NewestSubmitted(std::chrono::steady_clock::time_point* submittedAt) const {
    std::lock_guard lock(completedMutex);
    if (submittedAt != nullptr) *submittedAt = newestSubmittedAt;
    return newestSubmitted;
}

std::size_t Recorder::UnsignaledBatches() const {
    return static_cast<std::size_t>(std::count_if(inFlight.begin(), inFlight.end(), [&](const auto& batch) { return !signaled(*batch); }));
}

void Recorder::Keep(std::shared_ptr<void> object) {
    ensureOpen();
    open->kept.push_back(std::move(object));
}

namespace {
std::size_t SnapshotPool(Recorder::SnapshotUse use) {
    return use == Recorder::SnapshotUse::Storage ? 0 : 1;
}
}

void Recorder::eraseDrawSnapshot(std::map<DrawSnapshotKey, DrawSnapshot>::iterator entry) {
    auto& pool = drawSnapshotPools[SnapshotPool(std::get<1>(entry->first))];
    pool.bytes -= std::get<2>(entry->first);
    pool.recency.erase(entry->second.recent);
    drawSnapshots.erase(entry);
}

std::shared_ptr<Buffer> Recorder::ReusableDrawSnapshot(std::uint64_t address, std::size_t bytes, SnapshotUse use, std::uint32_t* derived) {
    auto found = use == SnapshotUse::Vertex ? drawSnapshots.lower_bound({address, use, bytes}) : drawSnapshots.find({address, use, bytes});
    if (found == drawSnapshots.end() || std::get<0>(found->first) != address || std::get<1>(found->first) != use) return {};
    if (found->second.registryGeneration != GuestAllocations::GuestAllocationsGeneration_nid_postfix() || !GuestMemory::UnchangedSince(address, bytes, found->second.generation)) {
        eraseDrawSnapshot(found);
        return {};
    }
    auto& recency = drawSnapshotPools[SnapshotPool(use)].recency;
    recency.splice(recency.end(), recency, found->second.recent);
    if (derived != nullptr) *derived = found->second.derived;
    return found->second.buffer;
}

void Recorder::KeepDrawSnapshot(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::uint64_t registryGeneration, std::shared_ptr<Buffer> buffer, SnapshotUse use, std::uint32_t derived) {
    const bool storage = use == SnapshotUse::Storage;
    const auto budget = storage ? DrawSnapshotBudget : DrawInputBudget;
    const auto maxEntries = storage ? DrawSnapshotEntries : DrawInputEntries;
    auto& pool = drawSnapshotPools[SnapshotPool(use)];
    if (generation == 0 || bytes > budget) return;
    if (use == SnapshotUse::Vertex) {
        for (auto it = drawSnapshots.lower_bound({address, use, 0}); it != drawSnapshots.end() && std::get<0>(it->first) == address && std::get<1>(it->first) == use && std::get<2>(it->first) <= bytes;) eraseDrawSnapshot(it++);
    } else if (const auto found = drawSnapshots.find({address, use, bytes}); found != drawSnapshots.end()) {
        eraseDrawSnapshot(found);
    }
    while (!pool.recency.empty() && (pool.bytes + bytes > budget || pool.recency.size() >= maxEntries)) eraseDrawSnapshot(drawSnapshots.find(pool.recency.front()));
    const DrawSnapshotKey key{address, use, bytes};
    pool.recency.push_back(key);
    try {
        drawSnapshots.emplace(key, DrawSnapshot{generation, registryGeneration, std::prev(pool.recency.end()), std::move(buffer), derived});
    } catch (...) {
        pool.recency.pop_back();
        throw;
    }
    pool.bytes += bytes;
}

void Recorder::OnComplete(std::function<void()> action) {
    ensureOpen();
    open->completions.push_back(std::move(action));
    ++open->writeBackCompletionCount;
    writeBackCompletions.fetch_add(1, std::memory_order_acq_rel);
}

bool Recorder::noteWrite(std::uint64_t address, std::size_t bytes, bool ownLabel) {
    CaptureTrace::Log("buffer-write batch=%llu address=%llx bytes=%zu label=%d", static_cast<unsigned long long>(submissions + 1), static_cast<unsigned long long>(address), bytes, ownLabel);
    if (bytes == 0) return false;
    ensureOpen();
    const auto end = address + bytes;
    open->writes.emplace_back(address, end);
    open->writeNotes.push_back(++writeNoteCount);
    if (!ownLabel) markOverwritten(address, end);

    if (activeRecorder == this) writeGeneration.fetch_add(1, std::memory_order_release);

    if (SnapshotCovers(address, end)) {
        ++snapshotCovered;
        return false;
    }
    return true;
}

void Recorder::noteWriteOn(Batch& batch, std::uint64_t address, std::size_t bytes, bool ownLabel) {
    if (bytes == 0) return;
    if (&batch == open.get()) {
        if (!noteWrite(address, bytes, ownLabel)) return;
        publishPendingWrites();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return;
    }

    batch.writes.emplace_back(address, address + bytes);
    batch.writeNotes.push_back(++writeNoteCount);
    if (!ownLabel) markOverwritten(address, address + bytes);
    if (activeRecorder == this) writeGeneration.fetch_add(1, std::memory_order_release);
    if (!SnapshotCovers(address, address + bytes)) publishPendingWrites();
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void Recorder::markOverwritten(std::uint64_t address, std::uint64_t end) {
    if (recordedLabels.load(std::memory_order_relaxed) == 0) return;
    std::lock_guard tableLock(labelTableMutex);

    for (auto it = labels.lower_bound(address >= 3 ? address - 3 : 0); it != labels.end() && it->first < end; ++it) it->second.overwritten = true;
}

void Recorder::NotePendingWrite(std::uint64_t address, std::size_t bytes) {
    if (!noteWrite(address, bytes)) return;
    publishPendingWrites();

    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void Recorder::NotePendingWrites(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
    bool publish = false;
    for (const auto& [begin, end] : ranges) {
        if (end > begin && noteWrite(begin, static_cast<std::size_t>(end - begin))) publish = true;
    }
    if (!publish) return;
    publishPendingWrites();
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void Recorder::publishPendingWrites() const {

    if (activeRecorder != this) return;
    const auto started = DrawProfiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto merged = std::make_shared<WriteRanges>();
    if (open != nullptr) merged->insert(merged->end(), open->writes.begin(), open->writes.end());
    for (const auto& batch : inFlight) merged->insert(merged->end(), batch->writes.begin(), batch->writes.end());
    for (const auto* batch : finishing) merged->insert(merged->end(), batch->writes.begin(), batch->writes.end());
    std::sort(merged->begin(), merged->end());
    std::size_t out = 0;
    for (const auto& [begin, end] : *merged) {
        if (out != 0 && begin <= (*merged)[out - 1].second) (*merged)[out - 1].second = std::max((*merged)[out - 1].second, end);
        else (*merged)[out++] = {begin, end};
    }
    merged->resize(out);
    pendingWrites.store(std::move(merged), std::memory_order_release);
    publishGeneration.fetch_add(1, std::memory_order_release);
    ++snapshotRebuilds;
    if (DrawProfiled()) snapshotRebuildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

bool Recorder::overlaps(const Batch& batch, std::uint64_t address, std::uint64_t end) {
    for (const auto& [begin, finish] : batch.writes) {
        if (address < finish && begin < end) return true;
    }
    return false;
}

bool Recorder::PendingWriteOverlaps(std::uint64_t address, std::size_t bytes) const {
    if (bytes == 0) return false;
    const auto end = address + bytes;
    if (open != nullptr && overlaps(*open, address, end)) return true;
    for (const auto& batch : inFlight) {
        if (overlaps(*batch, address, end)) return true;
    }
    return false;
}

bool Recorder::OpenWriteOverlaps(std::uint64_t address, std::size_t bytes) const {
    return bytes != 0 && open != nullptr && overlaps(*open, address, address + bytes);
}

bool Recorder::signaled(const Batch& batch) const {
    return batch.submitted && fenceStatus(batch.fence) == VK_SUCCESS;
}

bool Recorder::PendingWriteSettled(std::uint64_t address, std::size_t bytes) const {
    if (bytes == 0) return true;
    const auto end = address + bytes;
    if (open != nullptr && overlaps(*open, address, end)) return false;
    for (const auto& batch : inFlight) {
        if (overlaps(*batch, address, end) && !signaled(*batch)) return false;
    }
    return true;
}

std::uint64_t Recorder::LastWriteNote(std::uint64_t address, std::size_t bytes) const {
    if (open == nullptr || open->writes.empty() || open->writeNotes.size() != open->writes.size()) return 0;
    if (open->writes.back() != std::pair<std::uint64_t, std::uint64_t>{address, address + bytes}) return 0;
    return open->writeNotes.back() == writeNoteCount ? writeNoteCount : 0;
}

std::uint64_t Recorder::NewestWriteNote(std::uint64_t address, std::size_t bytes) const {
    if (bytes == 0) return 0;
    const auto end = address + bytes;
    std::uint64_t newest = 0;
    const auto scan = [&](const Batch& batch) {
        for (std::size_t i = 0; i < batch.writes.size(); ++i) {
            if (address < batch.writes[i].second && batch.writes[i].first < end) newest = std::max(newest, batch.writeNotes[i]);
        }
    };
    if (open != nullptr) scan(*open);
    for (const auto& batch : inFlight) scan(*batch);
    return newest;
}

bool Recorder::ReadTracking() {
    return ReadTrackingEnabled();
}

void Recorder::NotePendingRead(std::uint64_t address, std::size_t bytes, ReadKind kind) {
    CaptureTrace::Log("buffer-read batch=%llu address=%llx bytes=%zu kind=%d", static_cast<unsigned long long>(submissions + 1), static_cast<unsigned long long>(address), bytes, static_cast<int>(kind));
    if (bytes == 0 || !ReadTrackingEnabled()) return;
    ensureOpen();
    open->reads.push_back({address, address + bytes, kind});
    readsNoted.fetch_add(1, std::memory_order_relaxed);
}

void Recorder::NotePendingReads(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, ReadKind kind) {
    if (ranges.empty() || !ReadTrackingEnabled()) return;
    ensureOpen();
    for (const auto& [begin, end] : ranges) {
        CaptureTrace::Log("buffer-read batch=%llu address=%llx bytes=%llu kind=%d", static_cast<unsigned long long>(submissions + 1), static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin), static_cast<int>(kind));
        if (end > begin) open->reads.push_back({begin, end, kind});
    }
    readsNoted.fetch_add(ranges.size(), std::memory_order_relaxed);
}

const Recorder::Batch::Read* Recorder::readOverlap(const Batch& batch, std::uint64_t address, std::uint64_t end) {
    for (const auto& read : batch.reads) {
        if (address < read.end && read.begin < end) return &read;
    }
    return nullptr;
}

bool Recorder::PendingReadOverlaps(std::uint64_t address, std::size_t bytes, bool ignoreSignaled) const {
    if (bytes == 0) return false;
    readQueries.fetch_add(1, std::memory_order_relaxed);
    const auto end = address + bytes;
    if (open != nullptr) {
        if (const auto* read = readOverlap(*open, address, end)) {
            readHits[static_cast<std::size_t>(read->kind)].fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    for (auto it = inFlight.rbegin(); it != inFlight.rend(); ++it) {
        const auto* read = readOverlap(**it, address, end);
        if (read == nullptr) continue;
        if (ignoreSignaled && signaled(**it)) {
            readStaleIgnored.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        readHits[static_cast<std::size_t>(read->kind)].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

std::optional<Recorder::PendingReadInfo> Recorder::DescribePendingRead(std::uint64_t address, std::size_t bytes) const {
    if (bytes == 0) return std::nullopt;
    const auto end = address + bytes;
    if (open != nullptr) {
        if (const auto* read = readOverlap(*open, address, end)) return PendingReadInfo{submissions + 1, open->queue, read->kind, true, false};
    }
    for (auto it = inFlight.rbegin(); it != inFlight.rend(); ++it) {
        if (const auto* read = readOverlap(**it, address, end)) return PendingReadInfo{(*it)->serial, (*it)->queue, read->kind, false, signaled(**it)};
    }
    return std::nullopt;
}

Recorder::ReadStatistics Recorder::ReadCounts() {
    ReadStatistics counts{readsNoted.load(std::memory_order_relaxed), readQueries.load(std::memory_order_relaxed), readStaleIgnored.load(std::memory_order_relaxed), {}};
    for (std::size_t kind = 0; kind < ReadKinds; ++kind) counts.hits[kind] = readHits[kind].load(std::memory_order_relaxed);
    return counts;
}

std::optional<Recorder::PendingWriteInfo> Recorder::DescribePendingWrite(std::uint64_t address, std::size_t bytes) const {
    if (bytes == 0) return std::nullopt;
    const auto end = address + bytes;

    const auto firstOverlap = [&](const Batch& batch) -> const std::pair<std::uint64_t, std::uint64_t>* {
        for (const auto& range : batch.writes) {
            if (address < range.second && range.first < end) return &range;
        }
        return nullptr;
    };

    const bool syncAll = !SyncThroughEnabled();
    const auto allBatches = inFlight.size() + (open != nullptr ? 1 : 0);
    if (open != nullptr) {
        if (const auto* range = firstOverlap(*open)) return PendingWriteInfo{submissions + 1, true, false, range->first, range->second, inFlight.size() + 1};
    }
    std::size_t finished = inFlight.size();
    for (auto it = inFlight.rbegin(); it != inFlight.rend(); ++it, --finished) {
        const auto* range = firstOverlap(**it);
        if (range == nullptr) continue;

        const bool signaled = fenceStatus((*it)->fence) == VK_SUCCESS;
        if (syncAll) return PendingWriteInfo{submissions + (open != nullptr ? 1 : 0), open != nullptr, signaled, range->first, range->second, allBatches};
        return PendingWriteInfo{(*it)->serial, false, signaled, range->first, range->second, finished};
    }
    return std::nullopt;
}

bool Recorder::HasCompletions() const {
    if (open != nullptr && !open->completions.empty()) return true;
    for (const auto& batch : inFlight) {
        if (!batch->completions.empty()) return true;
    }
    return false;
}

void Recorder::noteLabelOn(Batch& batch, std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue, bool behindCompletion) {

    std::lock_guard tableLock(labelTableMutex);
    for (std::size_t offset = 0; offset + 4 <= bytes.size(); offset += 4) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, 4);
        const auto dword = address + offset;
        labels.insert_or_assign(dword, LabelEntry{value, queue, stamp, &batch, 0, false, behindCompletion});
        batch.labelDwords.push_back(dword);
        LabelGroupDwords().emplace_back(dword, stamp);
    }
    recordedLabels.store(labels.size(), std::memory_order_relaxed);
}

void Recorder::markBehindCompletion(const Batch& batch, std::uint64_t begin, std::uint64_t end) {
    std::lock_guard tableLock(labelTableMutex);
    for (auto it = labels.lower_bound(begin); it != labels.end() && it->first < end; ++it) {
        if (it->second.batch == &batch) it->second.behindCompletion = true;
    }
}

void Recorder::NoteLabel(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue) {
    ensureOpen();

    const bool tabled = bytes.size() <= LabelTableBytes;
    if (tabled) {
        noteLabelOn(*open, address, bytes, stamp, queue);
    } else {
        std::lock_guard tableLock(labelTableMutex);
        labels.erase(labels.lower_bound(address), labels.lower_bound(address + bytes.size()));
        recordedLabels.store(labels.size(), std::memory_order_relaxed);
        open->wideLabels.push_back(wideLabels.emplace(address, address + bytes.size()));
        wideLabelBytes = std::max<std::uint64_t>(wideLabelBytes, bytes.size());
    }
    if (activeRecorder == this && pendingLabelSince.load(std::memory_order_relaxed) == NoPendingLabel) {
        pendingLabelSince.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
    }
    noteWriteOn(*open, address, bytes.size(), true);
}

std::optional<Recorder::LabelHit> Recorder::PendingLabel(std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp, LabelRefusal* refusal) const {

    std::lock_guard tableLock(labelTableMutex);
    return lookupLabel(address, bytes, afterStamp, refusal);
}

std::size_t Recorder::PendingLabels() const {
    std::lock_guard tableLock(labelTableMutex);
    return labels.size();
}

bool Recorder::PendingLabelIn(std::uint64_t address, std::size_t bytes) const {
    std::lock_guard tableLock(labelTableMutex);
    if (bytes == 0) return false;
    const auto end = address + bytes;
    if (const auto first = labels.lower_bound(address); first != labels.end() && first->first < end) return true;
    if (wideLabelInLocked(address, bytes)) return true;

    if (const auto first = queuedLabels.lower_bound(address); first != queuedLabels.end() && first->first < end) return true;
    return false;
}

std::optional<Recorder::LabelHit> Recorder::lookupLabel(std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp, LabelRefusal* refusal) const {
    if (refusal != nullptr) *refusal = LabelRefusal::None;
    if ((labels.empty() && queuedLabels.empty()) || (bytes != 4 && bytes != 8) || address % 4 != 0) return std::nullopt;
    LabelHit hit{0, 0, 0, false, std::numeric_limits<std::uint64_t>::max()};
    bool queued = false;
    bool candidate = false;
    const auto tag = GuestMemory::GpuLockThreadTag();
    const auto refuse = [&](LabelRefusal reason, std::uint64_t LateStatistics::*counter, const LabelEntry& entry) {
        if (refusal != nullptr) *refusal = entry.behindCompletion ? LabelRefusal::BehindCompletion : reason;
        if (counter != nullptr) ++(lateCounts.*counter);
        return std::nullopt;
    };
    for (std::size_t offset = 0; offset < bytes; offset += 4) {
        const auto dword = address + offset;
        const LabelEntry* entry = nullptr;

        if (const auto own = queuedLabels.find(dword); own != queuedLabels.end() && own->second.queue == tag) {
            entry = &own->second;
            queued = true;
        } else if (const auto found = labels.find(dword); found != labels.end()) {
            entry = &found->second;
            if (entry->batch == nullptr) {
                if (entry->queue != tag) return std::nullopt;
                queued = true;
            }
        }
        if (entry == nullptr) return std::nullopt;
        if (entry->stamp <= afterStamp) {

            if (!candidate) ++lateCounts.candidates;
            candidate = true;
            if (!LateTrustEnabled()) return refuse(LabelRefusal::TrustOff, nullptr, *entry);
            if (entry->batch == nullptr) return refuse(LabelRefusal::Queued, &LateStatistics::queued, *entry);
            if (entry->overwritten) return refuse(LabelRefusal::Overwritten, &LateStatistics::overwritten, *entry);
            if (entry->generation == 0) return refuse(LabelRefusal::Unclosed, &LateStatistics::unclosed, *entry);
            hit.late = true;
            hit.generation = std::min(hit.generation, entry->generation);
        }
        hit.value |= static_cast<std::uint64_t>(entry->value) << (offset * 8u);
        if (offset == 0) {
            hit.queue = entry->queue;
            hit.stamp = entry->stamp;
        }
    }
    if (!hit.late) hit.generation = 0;
    if (queued) queuedLabelHits.fetch_add(1, std::memory_order_relaxed);
    return hit;
}

bool Recorder::unsignaled(std::uint64_t serial) const {
    for (const auto& batch : inFlight) {
        if (batch->serial == serial) return fenceStatus(batch->fence) != VK_SUCCESS;
    }
    return false;
}

void Recorder::AfterCompletions(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue, bool storedOnGpu) {
    Require(open != nullptr || !inFlight.empty(), "no batch to append a completion label to");
    Batch& batch = open != nullptr ? *open : *inFlight.back();
    std::vector<std::byte> copy(bytes.begin(), bytes.end());

    static const bool always = std::getenv("APS5_LABEL_STORE_ALWAYS") != nullptr;
    const auto sequence = [&] {
        std::lock_guard ringLock(writtenBackMutex);
        return writtenBackSequence;
    }();
    batch.completions.push_back([this, address, copy = std::move(copy), sequence, storedOnGpu] {
        if (storedOnGpu && !always && !writtenBackSince(sequence, address, address + copy.size())) {
            completionStoresSkipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        completionStoresRun.fetch_add(1, std::memory_order_relaxed);
        GuestMemory::CheckRange(reinterpret_cast<const void*>(address), copy.size(), 4, true);
        std::memcpy(reinterpret_cast<void*>(address), copy.data(), copy.size());
        GuestMemory::MarkWritten(address, copy.size());
    });

    const bool behindCompletion = !(storedOnGpu && !always && !CountAllCompletionLabels());
    if (!behindCompletion) {
        batch.completionLabelRanges.push_back({address, address + bytes.size(), false});
    } else {
        ++batch.completionLabelCount;
        completionLabels.fetch_add(1, std::memory_order_acq_rel);
    }

    noteLabelOn(batch, address, bytes, stamp, queue, behindCompletion);
    noteWriteOn(batch, address, bytes.size(), true);
    if (&batch == open.get() && activeRecorder == this && pendingLabelSince.load(std::memory_order_relaxed) == NoPendingLabel) {
        pendingLabelSince.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
    }
}

void Recorder::NoteWrittenBack(std::uint64_t address, std::size_t bytes) {
    Recorder* recorder = activeRecorder;
    if (recorder == nullptr || bytes == 0) return;
    GuestMemory::AssertGpuLockHeld("Recorder::NoteWrittenBack");
    const auto end = address + bytes;
    {
        std::lock_guard ringLock(recorder->writtenBackMutex);
        recorder->writtenBack.push_back({++recorder->writtenBackSequence, address, end});
        while (recorder->writtenBack.size() > 16384) recorder->writtenBack.pop_front();
    }
    if (recorder->recordedLabels.load(std::memory_order_relaxed) != 0 && recorder->PendingLabelIn(address, bytes)) writeBacksOverLabels.fetch_add(1, std::memory_order_relaxed);

    const auto count = [&](Batch& batch) {
        for (auto& label : batch.completionLabelRanges) {
            if (label.counted || label.begin >= end || address >= label.end) continue;
            label.counted = true;
            ++batch.completionLabelCount;
            completionLabels.fetch_add(1, std::memory_order_acq_rel);
            completionLabelsCountedLate.fetch_add(1, std::memory_order_relaxed);
            recorder->markBehindCompletion(batch, label.begin, label.end);
        }
    };
    if (recorder->open != nullptr) count(*recorder->open);
    for (auto& batch : recorder->inFlight) count(*batch);
}

bool Recorder::writtenBackSince(std::uint64_t sequence, std::uint64_t begin, std::uint64_t end) {
    std::lock_guard ringLock(writtenBackMutex);
    if (writtenBack.empty() || writtenBack.back()[0] <= sequence) return false;
    if (writtenBack.front()[0] > sequence + 1) return true;
    for (auto it = writtenBack.rbegin(); it != writtenBack.rend() && (*it)[0] > sequence; ++it) {
        if ((*it)[1] < end && begin < (*it)[2]) return true;
    }
    return false;
}

void Recorder::Submit() {

    if (activeRecorder == this) workSinceSubmit.store(0, std::memory_order_relaxed);
    if (open == nullptr) return;
    GuestMemory::AssertGpuLockHeld("Recorder::Submit");
    if (!open->keyStores.empty()) recordKeyStores(false);
    if (open->renderPass.open) endOpenRenderPass();
    if (open->samples != VK_NULL_HANDLE) context.Function<PFN_vkCmdEndQuery>("vkCmdEndQuery")(open->commands, open->samples, 0);
    const bool hostReadCovered = (open->run.open || !open->run.queued.empty()) && closeStoreRun(true);
    if (BarrierValidate()) validateBatchEnds.fetch_add(1, std::memory_order_relaxed);
    if (open->hostReadOwed && !hostReadCovered) {

        recordBarrier(open->commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        CountBarriers(CommandClass::Draw);
    }
    EndGpuTiming(open->batchTiming);
    if (!GpuTimingEnabled()) reportBarriers();
    if (FlipReadCheck()) {

        for (const auto& read : open->reads) open->readGeneration = std::max(open->readGeneration, GuestMemory::CollectWrites(read.begin, read.end - read.begin));
    }
    auto batch = std::move(open);
    Check(function(endCommandBuffer, "vkEndCommandBuffer")(batch->commands), "vkEndCommandBuffer recorder");
    VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &batch->commands;

    const std::uint64_t serial = submissions + 1;
    CaptureTrace::Log("submit batch=%llu reads=%zu writes=%zu", static_cast<unsigned long long>(serial), batch->reads.size(), batch->writes.size());
    VkTimelineSemaphoreSubmitInfoKHR timelineInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR};
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &serial;
    if (timeline != VK_NULL_HANDLE) {
        submission.pNext = &timelineInfo;
        submission.signalSemaphoreCount = 1;
        submission.pSignalSemaphores = &timeline;
    }
    const auto submitStart = std::chrono::steady_clock::now();
    Check(function(queueSubmit, "vkQueueSubmit")(context.queue, 1, &submission, batch->fence), "vkQueueSubmit recorder");
    batch->submitted = true;
    batch->serial = ++submissions;
    batch->submittedAt = std::chrono::steady_clock::now();
    if (DrawProfiled()) {
        const auto us = std::chrono::duration<double, std::micro>(batch->submittedAt - submitStart).count();
        ++submitCount;
        submitUs += us;
        submitMaxUs = std::max(submitMaxUs, us);
        if (auto* frame = PerformanceContext::Current()) frame->Add(frame->Get("Recorder", "submit"), batch->submittedAt - submitStart);
    }
    {
        std::lock_guard lock(completedMutex);
        newestSubmitted = batch->serial;
        newestSubmittedAt = batch->submittedAt;
    }
    inFlight.push_back(std::move(batch));
    if (activeRecorder == this) pendingLabelSince.store(NoPendingLabel, std::memory_order_release);
}

std::uint64_t Recorder::SubmitAndEpoch() {
    Submit();
    return inFlight.empty() ? 0 : submissions;
}

namespace {

struct UnlockedWaiter {
    std::atomic<int>& count;
    explicit UnlockedWaiter(std::uint64_t recorderId) : count(WaitersOf(recorderId)) { count.fetch_add(1, std::memory_order_acq_rel); }
    ~UnlockedWaiter() { count.fetch_sub(1, std::memory_order_acq_rel); }
    UnlockedWaiter(const UnlockedWaiter&) = delete;
    UnlockedWaiter& operator=(const UnlockedWaiter&) = delete;
};

struct RestoreSyncSite {
    std::size_t site;
    ~RestoreSyncSite() { activeSyncSite = site; }
};

VkResult WaitTimeline(VkDevice device, VkSemaphore timeline, PFN_vkWaitSemaphoresKHR waitSemaphores, std::uint64_t serial) {
    VkSemaphoreWaitInfoKHR wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR};
    wait.semaphoreCount = 1;
    wait.pSemaphores = &timeline;
    wait.pValues = &serial;
    auto result = waitSemaphores(device, &wait, 5'000'000'000ull);
    for (int waited = 5; result == VK_TIMEOUT; waited += 5) {

        aps5::LogErr( "[gpu] recorded batch %llu still running on the GPU after %d s (timeline wait)\n", static_cast<unsigned long long>(serial), waited);
        result = waitSemaphores(device, &wait, 5'000'000'000ull);
    }
    return result;
}

}

void Recorder::WaitSerial(std::uint64_t serial) {
    if (timeline == VK_NULL_HANDLE || serial == 0) return;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const UnlockedWaiter waiter{id};
    const auto result = WaitTimeline(context.device, timeline, context.Function<PFN_vkWaitSemaphoresKHR>("vkWaitSemaphoresKHR"), serial);
    if (profile) {
        unlockedWaits.fetch_add(1, std::memory_order_relaxed);
        unlockedWaitedUs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
    }
    Check(result, "vkWaitSemaphoresKHR recorder");
}

void Recorder::FinishUpTo(std::uint64_t serial) {

    ++syncCounts[0];
    announcedSource = 4;
    const void* site = std::exchange(announcedSite, nullptr);
    const auto previousSite = inFlight.empty() || inFlight.front()->serial > serial ? activeSyncSite : BeginSyncSite(0, site != nullptr ? site : __builtin_return_address(0));
    while (!inFlight.empty() && inFlight.front()->serial <= serial) {
        auto batch = std::move(inFlight.front());
        inFlight.pop_front();
        finish(std::move(batch), true, 0);
    }
    activeSyncSite = previousSite;
}

void Recorder::Sync() {
    const auto source = std::exchange(announcedSource, 4);
    const void* site = std::exchange(announcedSite, nullptr);
    Submit();

    const auto previousSite = inFlight.empty() ? activeSyncSite : BeginSyncSite(source, site != nullptr ? site : __builtin_return_address(0));
    while (!inFlight.empty()) {
        auto batch = std::move(inFlight.front());
        inFlight.pop_front();
        finish(std::move(batch), true, source);
    }
    activeSyncSite = previousSite;
}

void Recorder::SyncThrough(std::uint64_t address, std::size_t bytes, bool waitUnlocked) {
    if (bytes == 0) return;
    const auto end = address + bytes;
    if (completionDepth != 0 && !CompletionStoreSyncs()) {

        announcedSource = 4;
        announcedSite = nullptr;
        ++holdCounters.completionSyncsSkipped;
        return;
    }
    if (completionDepth != 0) ++holdCounters.completionSyncsWaited;
    const auto completionWaitStart = completionDepth != 0 && DrawProfiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    struct CompletionWait {
        std::chrono::steady_clock::time_point start;
        ~CompletionWait() {
            if (start != std::chrono::steady_clock::time_point{}) holdCounters.completionSyncWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
    } completionWait{completionWaitStart};
    if (waitUnlocked) {

        const auto source = announcedSource;
        const void* site = announcedSite != nullptr ? announcedSite : __builtin_return_address(0);
        if (syncThroughUnlocked(address, end, source, site)) return;

        if (completionDepth == 0) ++holdCounters.hookLockedWaits;
    }
    if (!SyncThroughEnabled() || (open != nullptr && overlaps(*open, address, end))) {

        if (announcedSite == nullptr) announcedSite = __builtin_return_address(0);

        if (open != nullptr || (!inFlight.empty() && unsignaled(inFlight.back()->serial))) ++hookRealWaits;
        Sync();
        return;
    }
    const auto source = std::exchange(announcedSource, 4);
    const void* site = std::exchange(announcedSite, nullptr);
    std::uint64_t targetSerial = 0;
    for (auto it = inFlight.rbegin(); it != inFlight.rend(); ++it) {
        if (overlaps(**it, address, end)) {
            targetSerial = (*it)->serial;
            break;
        }
    }
    if (targetSerial == 0) return;
    if (unsignaled(targetSerial)) ++hookRealWaits;
    targetedSyncs.fetch_add(1, std::memory_order_relaxed);
    const RestoreSyncSite restoreSite{BeginSyncSite(source, site != nullptr ? site : __builtin_return_address(0))};

    while (!inFlight.empty() && inFlight.front()->serial <= targetSerial) {
        auto batch = std::move(inFlight.front());
        inFlight.pop_front();
        finish(std::move(batch), true, source);
    }
    batchesLeftInFlight.fetch_add(inFlight.size(), std::memory_order_relaxed);
}

namespace {

struct SyncWaitSite {
    std::uint64_t count = 0;
    std::uint64_t batchesToTarget = 0;
    std::uint64_t unsignaledAtStart = 0;
    std::uint64_t newest = 0;
    double waitedMs = 0;

    double targetAgeMs = 0;
};

struct SyncWaitStats {
    std::array<SyncWaitSite, static_cast<std::size_t>(GuestMemory::ReadSite::Count)> bySite{};

    std::map<std::uint32_t, std::uint64_t> aheadByQueue;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

SyncWaitStats& SyncWaits() {
    static SyncWaitStats stats;
    return stats;
}

bool TraceCapSync() {
    static const bool trace = std::getenv("APS5_TRACE_CAPSYNC") != nullptr;
    return trace;
}

void ReportSyncWaits(SyncWaitStats& stats) {
    std::string report = "[syncwait] unlocked hook waits by read site (10 s; count/GPU wait, avg batches up to the target, avg of them unsignaled at the start, target was the newest submission, avg target submit-to-signal ms):";
    for (std::size_t site = 0; site < stats.bySite.size(); ++site) {
        const auto& entry = stats.bySite[site];
        if (entry.count == 0) continue;
        char text[160];
        std::snprintf(text, sizeof(text), " %s %llu/%.0fms %.1f %.1f %.0f%% %.1f", GuestMemory::ReadSiteName(static_cast<GuestMemory::ReadSite>(site)), static_cast<unsigned long long>(entry.count), entry.waitedMs, static_cast<double>(entry.batchesToTarget) / entry.count, static_cast<double>(entry.unsignaledAtStart) / entry.count, entry.newest * 100.0 / entry.count, entry.targetAgeMs / entry.count);
        report += text;
    }
    report += "; batches ahead by queue:";
    for (const auto& [queue, count] : stats.aheadByQueue) {
        char text[48];
        if (queue == 0xffffffffu) std::snprintf(text, sizeof(text), " untagged %llu", static_cast<unsigned long long>(count));
        else std::snprintf(text, sizeof(text), " 0x%x %llu", queue, static_cast<unsigned long long>(count));
        report += text;
    }
    aps5::LogErr( "%s\n", report.c_str());
    stats = SyncWaitStats{};
}

}

bool Recorder::syncThroughUnlocked(std::uint64_t address, std::uint64_t end, int source, const void* site) {

    if (HookLockedWait() || timeline == VK_NULL_HANDLE || GuestMemory::GpuMutex().DepthOnThisThread() != 1) return false;

    std::uint64_t targetSerial = 0;
    if (!SyncThroughEnabled() || (open != nullptr && overlaps(*open, address, end))) {
        Submit();
        targetSerial = submissions;
    } else {
        for (auto it = inFlight.rbegin(); it != inFlight.rend(); ++it) {
            if (overlaps(**it, address, end)) {
                targetSerial = (*it)->serial;
                break;
            }
        }
    }
    announcedSource = 4;
    announcedSite = nullptr;
    if (targetSerial == 0) return true;
    if (unsignaled(targetSerial)) ++hookRealWaits;

    const auto myId = id;
    const auto device = context.device;
    const auto semaphore = timeline;
    const auto waitSemaphores = context.Function<PFN_vkWaitSemaphoresKHR>("vkWaitSemaphoresKHR");

    const RestoreSyncSite restoreSite{BeginSyncSite(source, site)};
    const bool profile = DrawProfiled();
    const bool trace = TraceCapSync();
    const auto readSite = GuestMemory::CurrentReadSite();
    const auto newestSerial = submissions;
    std::size_t batchesToTarget = 0, unsignaledAtStart = 0, targetRanges = 0;
    std::pair<std::uint64_t, std::uint64_t> hit{0, 0};
    std::chrono::steady_clock::time_point targetSubmittedAt{};
    std::map<std::uint32_t, std::uint64_t> aheadByQueue;
    if (profile || trace) {
        const auto status = context.Function<PFN_vkGetFenceStatus>("vkGetFenceStatus");
        for (const auto& batch : inFlight) {
            if (batch->serial > targetSerial) break;
            ++batchesToTarget;
            ++aheadByQueue[batch->queue];
            if (status(context.device, batch->fence) != VK_SUCCESS) ++unsignaledAtStart;
            if (batch->serial != targetSerial) continue;
            targetSubmittedAt = batch->submittedAt;
            targetRanges = batch->writes.size();
            for (const auto& range : batch->writes) {
                if (address < range.second && range.first < end) {
                    hit = range;
                    break;
                }
            }
        }
    }
    const auto start = profile || trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto waited = start;
    auto& mutex = GuestMemory::GpuMutex();
    VkResult result = VK_SUCCESS;
    {

        const UnlockedWaiter waiter{myId};
        mutex.unlock();
        result = WaitTimeline(device, semaphore, waitSemaphores, targetSerial);

        if (profile || trace) waited = std::chrono::steady_clock::now();
    }
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Hook);
    mutex.lock();
    const auto ms = std::chrono::duration<double, std::milli>(waited - start).count();
    if (profile) {

        syncWaitedMs[source] += ms;
        CountThreadSync(source, ms);
        CountSiteWait(ms);
        ++holdCounters.hookUnlockedWaits;
        holdCounters.hookUnlockedWaitMs += ms;
        if (source >= 0 && source < 5) {
            ++holdCounters.hookUnlockedWaitsBySource[source];
            holdCounters.hookUnlockedWaitMsBySource[source] += ms;
        }
        holdCounters.hookRelockMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waited).count();
        auto& stats = SyncWaits();
        auto& siteStats = stats.bySite[static_cast<std::size_t>(readSite)];
        ++siteStats.count;
        siteStats.waitedMs += ms;
        siteStats.batchesToTarget += batchesToTarget;
        siteStats.unsignaledAtStart += unsignaledAtStart;
        if (targetSerial == newestSerial) ++siteStats.newest;
        if (targetSubmittedAt != std::chrono::steady_clock::time_point{}) siteStats.targetAgeMs += std::chrono::duration<double, std::milli>(waited - targetSubmittedAt).count();
        for (const auto& [queue, count] : aheadByQueue) stats.aheadByQueue[queue] += count;
        if (std::chrono::steady_clock::now() - stats.lastReport > std::chrono::seconds(10)) ReportSyncWaits(stats);
    }
    if (trace) {
        static std::atomic<int> lines{0};
        if (lines.fetch_add(1) < 600) {
            const auto packet = GuestMemory::CurrentPacket();
            aps5::LogErr( "[capsync] sync q0x%x %s %s 0x%llx+0x%llx: target %llu (newest %llu), %zu batches up to it (%zu unsignaled at the start), target noted %zu ranges, hit 0x%llx+0x%llx, GPU wait %.1f ms\n", packet.queue, PacketName(packet.opcode).c_str(), GuestMemory::ReadSiteName(readSite), static_cast<unsigned long long>(address), static_cast<unsigned long long>(end - address), static_cast<unsigned long long>(targetSerial), static_cast<unsigned long long>(newestSerial), batchesToTarget, unsignaledAtStart, targetRanges, static_cast<unsigned long long>(hit.first), static_cast<unsigned long long>(hit.second - hit.first), ms);
        }
    }
    if (!RecorderAlive(myId)) {

        ++holdCounters.hookUnlockedTornDown;
        Check(result, "vkWaitSemaphoresKHR recorder");
        return true;
    }
    Check(result, "vkWaitSemaphoresKHR recorder");

    targetedSyncs.fetch_add(1, std::memory_order_relaxed);
    while (!inFlight.empty() && inFlight.front()->serial <= targetSerial) {
        auto batch = std::move(inFlight.front());
        inFlight.pop_front();
        finish(std::move(batch), true, source);
    }
    batchesLeftInFlight.fetch_add(inFlight.size(), std::memory_order_relaxed);
    return true;
}

bool Recorder::Reap() {
    const bool profile = DrawProfiled();
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::uint64_t retired = 0;
    while (!inFlight.empty()) {
        if (fenceStatus(inFlight.front()->fence) != VK_SUCCESS) break;
        auto batch = std::move(inFlight.front());
        inFlight.pop_front();
        finish(std::move(batch), false, 4);
        ++retired;
    }
    if (profile) {
        ++holdCounters.reaps;
        if (retired != 0) ++holdCounters.reapsWithWork;
        holdCounters.reapBatches += retired;
        holdCounters.reapMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    return inFlight.empty();
}

void Recorder::finish(std::unique_ptr<Batch> batch, bool wait, int source) {

    struct Finishing {
        Recorder& recorder;
        const Batch* batch;
        ~Finishing() {
            auto& list = recorder.finishing;
            list.erase(std::remove(list.begin(), list.end(), batch), list.end());

            if (batch->completionLabelCount != 0) completionLabels.fetch_sub(batch->completionLabelCount, std::memory_order_acq_rel);
            if (batch->writeBackCompletionCount != 0) writeBackCompletions.fetch_sub(batch->writeBackCompletionCount, std::memory_order_acq_rel);

            try {
                recorder.publishPendingWrites();
            } catch (...) {
            }
        }
    } finishingScope{*this, batch.get()};
    finishing.push_back(batch.get());
    if (wait) {

        static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
        static double waitedMs = 0;
        static std::uint64_t waits = 0;
        static auto lastReport = std::chrono::steady_clock::now();
        const auto waitStart = std::chrono::steady_clock::now();

        const bool signaledAtStart = !profile || context.Function<PFN_vkGetFenceStatus>("vkGetFenceStatus")(context.device, batch->fence) == VK_SUCCESS;
        struct Report {
            bool enabled;
            int source;
            std::chrono::steady_clock::time_point start;
            const Recorder& recorder;
            bool signaledAtStart;
            ~Report() {
                if (!enabled) return;
                const auto now = std::chrono::steady_clock::now();
                const auto ms = std::chrono::duration<double, std::milli>(now - start).count();
                waitedMs += ms;
                syncWaitedMs[source] += ms;
                ++waits;
                CountThreadSync(source, ms);
                CountSiteWait(ms);
                if (!signaledAtStart) GuestMemory::NoteLockedGpuWait(ms);
                if (now - lastReport > std::chrono::seconds(10)) {
                    lastReport = now;
                    const auto& h = holdCounters;
                    aps5::LogErr( "[recorder] %llu syncs waited %.1f s for the GPU in total (sources, count/wait: idle %llu/%.1fs, pending write %llu/%.1fs, recorded store %llu/%.1fs, address-based %llu/%.1fs, other %llu/%.1fs); hook %llu calls, %llu locked; %llu targeted syncs left %llu batches in flight; snapshot %llu rebuilds %.0f ms, %llu notes covered; %llu unlocked timeline waits %.1f s; %llu submissions, %zu label entries, %llu completion labels pending; fence waits by thread (count/wait):%s; top sync sites (source@caller syncs/batches/wait):%s; under holds (cumulative): %llu completions ran %.0f ms (kept objects released %.0f ms), pending-write syncs from completions: %llu skipped, %llu waited %.0f ms; %llu reaps (%llu with work) retired %llu batches in %.0f ms; hook waits unlocked %llu / %.0f ms GPU (pending write %llu / %.0f ms, recorded store %llu / %.0f ms) + %.0f ms relock (%llu found the recorder torn down), locked %llu; deferred releases: on the release thread %llu batches (%llu objects) in %.0f ms, inline %llu batches (%llu objects) in %.0f ms (%llu batches over the queue bound of %zu), queue max %llu batches, %llu pending\n",static_cast<unsigned long long>(waits), waitedMs / 1000, static_cast<unsigned long long>(syncCounts[0]), syncWaitedMs[0] / 1000, static_cast<unsigned long long>(syncCounts[1]), syncWaitedMs[1] / 1000, static_cast<unsigned long long>(syncCounts[2]), syncWaitedMs[2] / 1000, static_cast<unsigned long long>(syncCounts[3]), syncWaitedMs[3] / 1000, static_cast<unsigned long long>(syncCounts[4]), syncWaitedMs[4] / 1000, static_cast<unsigned long long>(hookCalls.load()), static_cast<unsigned long long>(hookLocks.load()), static_cast<unsigned long long>(targetedSyncs.load()), static_cast<unsigned long long>(batchesLeftInFlight.load()), static_cast<unsigned long long>(snapshotRebuilds), snapshotRebuildMs, static_cast<unsigned long long>(snapshotCovered), static_cast<unsigned long long>(unlockedWaits.load()), unlockedWaitedUs.load() / 1e6, static_cast<unsigned long long>(recorder.submissions), recorder.PendingLabels(), static_cast<unsigned long long>(completionLabels.load()), ThreadSyncReport().c_str(), SyncSiteReport().c_str(), static_cast<unsigned long long>(h.completions), h.completionMs, h.keptReleaseMs, static_cast<unsigned long long>(h.completionSyncsSkipped), static_cast<unsigned long long>(h.completionSyncsWaited), h.completionSyncWaitMs, static_cast<unsigned long long>(h.reaps), static_cast<unsigned long long>(h.reapsWithWork), static_cast<unsigned long long>(h.reapBatches), h.reapMs, static_cast<unsigned long long>(h.hookUnlockedWaits), h.hookUnlockedWaitMs, static_cast<unsigned long long>(h.hookUnlockedWaitsBySource[1]), h.hookUnlockedWaitMsBySource[1], static_cast<unsigned long long>(h.hookUnlockedWaitsBySource[2]), h.hookUnlockedWaitMsBySource[2], h.hookRelockMs, static_cast<unsigned long long>(h.hookUnlockedTornDown), static_cast<unsigned long long>(h.hookLockedWaits), static_cast<unsigned long long>(threadReleases.load()), static_cast<unsigned long long>(threadObjects.load()), threadReleaseUs.load() / 1000.0, static_cast<unsigned long long>(inlineReleases.load()), static_cast<unsigned long long>(inlineObjects.load()), inlineReleaseUs.load() / 1000.0, static_cast<unsigned long long>(inlineOverBound.load()), ReleaseQueueBound(), static_cast<unsigned long long>(releaseQueueMax.load()), static_cast<unsigned long long>(deferredPending.load()));
                    aps5::LogErr( "[recorder] completion label stores: %llu run, %llu skipped (no CPU write-back overlapped them); %llu counted pending at a write-back; %llu write-backs over a tracked label; %llu write-back completions pending\n", static_cast<unsigned long long>(completionStoresRun.load()), static_cast<unsigned long long>(completionStoresSkipped.load()), static_cast<unsigned long long>(completionLabelsCountedLate.load()), static_cast<unsigned long long>(writeBacksOverLabels.load()), static_cast<unsigned long long>(writeBackCompletions.load()));
                    aps5::LogErr( "[recorder] submits %llu, vkQueueSubmit mean %.1f us, max %.1f us\n", static_cast<unsigned long long>(submitCount), submitCount != 0 ? submitUs / static_cast<double>(submitCount) : 0.0, submitMaxUs);
                    const auto reads = Recorder::ReadCounts();
                    aps5::LogErr( "[recorder] in-place reads: %llu noted, %llu queries, hits by reader: dispatch element %llu, gpu copy %llu, address-based %llu, indirect %llu, storage upload %llu, copy source %llu; %llu hits on signaled batches ignored\n", static_cast<unsigned long long>(reads.noted), static_cast<unsigned long long>(reads.queries), static_cast<unsigned long long>(reads.hits[0]), static_cast<unsigned long long>(reads.hits[1]), static_cast<unsigned long long>(reads.hits[2]), static_cast<unsigned long long>(reads.hits[3]), static_cast<unsigned long long>(reads.hits[4]), static_cast<unsigned long long>(reads.hits[5]), static_cast<unsigned long long>(reads.staleIgnored));
                }
            }
        } report{profile, source, waitStart, *this, signaledAtStart};
        const auto waitFences = function(waitForFences, "vkWaitForFences");
        auto result = waitFences(context.device, 1, &batch->fence, VK_TRUE, 5'000'000'000ull);
        for (int waited = 5; result == VK_TIMEOUT; waited += 5) {

            aps5::LogErr( "[gpu] recorded batch still running on the GPU after %d s\n", waited);
            result = waitFences(context.device, 1, &batch->fence, VK_TRUE, 5'000'000'000ull);
        }
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
            const auto idle = context.Function<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(context.device);
            Check(idle, "vkDeviceWaitIdle after recorder fence failure");
        }
        if (result != VK_SUCCESS) {
            release(*batch);
            Check(result, "vkWaitForFences recorder");
        }
    }
    readGpuTiming(*batch);
    readSamples(*batch);
    if (batch->serial != 0) {
        std::lock_guard ringLock(completedMutex);
        auto& entry = completed[batch->serial % completed.size()];
        entry.serial = batch->serial;
        entry.gpuStartNs = batch->gpuStartNs;
        entry.gpuEndNs = batch->gpuEndNs;
        entry.submittedAt = batch->submittedAt;
        entry.fenceSeenAt = std::chrono::steady_clock::now();
        entry.readGeneration = batch->readGeneration;
        entry.reads.clear();
        if (FlipReadCheck()) {
            for (const auto& read : batch->reads) entry.reads.emplace_back(read.begin, read.end);
        }
    }

    const bool profile = DrawProfiled();
    const auto completionStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    {
        struct InCompletion {
            InCompletion() { ++completionDepth; }
            ~InCompletion() { --completionDepth; }
        } inCompletion;
        for (auto& action : batch->completions) {
            try {
                action();
            } catch (const std::exception& error) {
                aps5::LogErr( "[gpu] deferred write-back failed: %s\n", error.what());
            }
        }
    }
    if (profile) holdCounters.completions += batch->completions.size();
    const auto keptStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    if (ReleaseUnderLock() || !GuestMemory::GpuMutex().HeldByThisThread()) {
        batch->completions.clear();
        batch->kept.clear();
    } else {

        try {
            DeferredBatches().push_back({std::move(batch->kept), std::move(batch->completions)});
            deferredPending.fetch_add(1, std::memory_order_acq_rel);
        } catch (...) {
        }
        batch->completions.clear();
        batch->kept.clear();
    }
    if (profile) {
        const auto now = std::chrono::steady_clock::now();
        holdCounters.keptReleaseMs += std::chrono::duration<double, std::milli>(now - keptStart).count();
        holdCounters.completionMs += std::chrono::duration<double, std::milli>(now - completionStart).count();
    }

    if (!batch->labelDwords.empty() || !batch->wideLabels.empty()) {
        std::lock_guard tableLock(labelTableMutex);
        for (const auto dword : batch->labelDwords) {
            const auto found = labels.find(dword);
            if (found != labels.end() && found->second.batch == batch.get()) labels.erase(found);
        }
        for (const auto entry : batch->wideLabels) wideLabels.erase(entry);
        recordedLabels.store(labels.size(), std::memory_order_relaxed);
    }
    batch->labelDwords.clear();
    batch->wideLabels.clear();
    release(*batch);
}

void Recorder::release(Batch& batch) noexcept {
    if (batch.queries != VK_NULL_HANDLE) {
        if (!GpuTimingEnabled() && sparePools.size() < 64) {
            try {
                sparePools.push_back(batch.queries);
                batch.queries = VK_NULL_HANDLE;
            } catch (...) {
            }
        }
        if (batch.queries != VK_NULL_HANDLE) context.Function<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(context.device, batch.queries, nullptr);
    }
    batch.queries = VK_NULL_HANDLE;
    if (batch.samples != VK_NULL_HANDLE) context.Function<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(context.device, batch.samples, nullptr);
    batch.samples = VK_NULL_HANDLE;

    if (batch.commands != VK_NULL_HANDLE && batch.fence != VK_NULL_HANDLE && spare.size() < 64 && function(resetFences, "vkResetFences")(context.device, 1, &batch.fence) == VK_SUCCESS) {
        spare.emplace_back(batch.commands, batch.fence);
        batch.commands = VK_NULL_HANDLE;
        batch.fence = VK_NULL_HANDLE;
        return;
    }
    if (batch.commands != VK_NULL_HANDLE) context.Function<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(context.device, context.pool, 1, &batch.commands);
    if (batch.fence != VK_NULL_HANDLE) context.Function<PFN_vkDestroyFence>("vkDestroyFence")(context.device, batch.fence, nullptr);
    batch.commands = VK_NULL_HANDLE;
    batch.fence = VK_NULL_HANDLE;
}

}
