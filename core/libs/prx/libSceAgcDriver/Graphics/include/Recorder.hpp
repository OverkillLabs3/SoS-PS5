#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDER_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <array>
#include <deque>
#include <mutex>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

class Buffer;

class Recorder {
public:

    explicit Recorder(const Context& context, bool timelineSemaphores = false);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    VkCommandBuffer Commands(VkAccessFlags* coveredAccess = nullptr);

    void MarkCovered(VkAccessFlags access);
    void MarkShaderReadsCovered() { MarkCovered(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT); }

    bool ContinuesRenderPass(std::uint64_t key) const;

    std::uint64_t OpenPassKey() const;
    VkCommandBuffer CommandsInRenderPass();
    void LeaveRenderPassOpen(std::uint64_t key, std::uint32_t timing, bool continuable);

    void QueueKeyStore(VkBuffer buffer, VkDeviceSize first, VkDeviceSize last, std::shared_ptr<void> seed, std::uint64_t begin, std::uint64_t end);
    bool HasQueuedKeyStores() const { return open != nullptr && !open->keyStores.empty(); }
    bool QueuedKeyStoreOverlaps(std::uint64_t address, std::size_t bytes) const;

    bool AnyQueuedKeyStore(const std::function<bool(std::uint64_t, std::uint64_t)>& overlaps) const;
    void FlushKeyStores();
    void FlushKeyStoresOverlapping(std::uint64_t address, std::size_t bytes) { if (HasQueuedKeyStores() && QueuedKeyStoreOverlaps(address, bytes)) FlushKeyStores(); }
    bool Recording() const { return open != nullptr; }
    bool Idle() const { return open == nullptr && inFlight.empty(); }

    bool HasCompletions() const;

    void Keep(std::shared_ptr<void> object);
    enum class SnapshotUse : std::uint8_t { Storage, Vertex, Index16, Index32 };
    static constexpr std::size_t DrawSnapshotBudget = std::size_t{256} << 20u;
    static constexpr std::size_t DrawSnapshotEntries = 1024;
    static constexpr std::size_t DrawInputBudget = std::size_t{1024} << 20u;
    static constexpr std::size_t DrawInputEntries = 16384;
    std::shared_ptr<Buffer> ReusableDrawSnapshot(std::uint64_t address, std::size_t bytes, SnapshotUse use = SnapshotUse::Storage, std::uint32_t* derived = nullptr);
    void KeepDrawSnapshot(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::uint64_t registryGeneration, std::shared_ptr<Buffer> buffer, SnapshotUse use = SnapshotUse::Storage, std::uint32_t derived = 0);
    void OnComplete(std::function<void()> action);
    void NotePendingWrite(std::uint64_t address, std::size_t bytes);

    void NotePendingWrites(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);
    bool PendingWriteOverlaps(std::uint64_t address, std::size_t bytes) const;

    bool PendingWriteSettled(std::uint64_t address, std::size_t bytes) const;
    std::uint64_t LastWriteNote(std::uint64_t address, std::size_t bytes) const;
    std::uint64_t NewestWriteNote(std::uint64_t address, std::size_t bytes) const;

    enum class ReadKind : std::uint8_t { DispatchElement = 0, GpuCopy, AddressBased, Indirect, StorageUpload, CopySource, Count };
    static bool ReadTracking();
    void NotePendingRead(std::uint64_t address, std::size_t bytes, ReadKind kind);
    void NotePendingReads(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, ReadKind kind);

    bool PendingReadOverlaps(std::uint64_t address, std::size_t bytes, bool ignoreSignaled = true) const;

    struct PendingReadInfo {
        std::uint64_t serial;
        std::uint32_t queue;
        ReadKind kind;
        bool open;
        bool signaled;
    };
    std::optional<PendingReadInfo> DescribePendingRead(std::uint64_t address, std::size_t bytes) const;

    struct ReadStatistics {
        std::uint64_t noted, queries, staleIgnored;
        std::array<std::uint64_t, static_cast<std::size_t>(ReadKind::Count)> hits;
    };
    static ReadStatistics ReadCounts();

    bool OpenWriteOverlaps(std::uint64_t address, std::size_t bytes) const;

    struct PendingWriteInfo {
        std::uint64_t serial;
        bool open;
        bool signaled;
        std::uint64_t rangeBegin;
        std::uint64_t rangeEnd;
        std::size_t batchesToFinish;
    };
    std::optional<PendingWriteInfo> DescribePendingWrite(std::uint64_t address, std::size_t bytes) const;

    void Submit();

    std::uint64_t SubmitAndEpoch();
    bool HasTimeline() const { return timeline != VK_NULL_HANDLE; }

    void WaitSerial(std::uint64_t serial);

    void FinishUpTo(std::uint64_t serial);

    void Sync();
    void CountSamples();
    static std::uint64_t SamplesPassed();

    void SyncThrough(std::uint64_t address, std::size_t bytes, bool waitUnlocked = false);

    bool Reap();
    std::uint64_t Submissions() const { return submissions; }

    std::size_t InFlightBatches() const { return inFlight.size(); }

    static std::uint64_t ReapsWithWork();

    void RecordStore(VkBuffer buffer, VkDeviceSize offset, std::span<const std::byte> bytes, std::uint64_t address);
    bool HasQueuedStores() const { return open != nullptr && !open->run.queued.empty(); }
    bool QueuedStoreOverlaps(std::uint64_t address, std::size_t bytes) const;

    bool AnyQueuedStore(const std::function<bool(std::uint64_t, std::uint64_t)>& overlaps) const;
    void FlushStores();
    void FlushStoresOverlapping(std::uint64_t address, std::size_t bytes) { if (HasQueuedStores() && QueuedStoreOverlaps(address, bytes)) FlushStores(); }

    static constexpr std::size_t LabelTableBytes = 64;
    void NoteLabel(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue);

    static void CloseLabelGroup(std::uint64_t trackerGeneration);
    static bool LateTrust();
    struct LabelHit {
        std::uint64_t value;

        std::uint32_t queue;
        std::uint64_t stamp;

        bool late;
        std::uint64_t generation;
    };

    enum class LabelRefusal : std::uint8_t { None = 0, TrustOff, Queued, Overwritten, Unclosed, BehindCompletion };

    struct LateStatistics {
        std::uint64_t candidates, queued, overwritten, unclosed;
    };
    static LateStatistics LateCounts();

    std::optional<LabelHit> PendingLabel(std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp, LabelRefusal* refusal = nullptr) const;
    std::size_t PendingLabels() const;

    bool PendingLabelIn(std::uint64_t address, std::size_t bytes) const;

    static std::optional<LabelHit> LookupLabel(std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp, LabelRefusal* refusal = nullptr);

    static std::optional<std::uint64_t> LookupLabelValue(std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp);
    static bool WideLabelIn(std::uint64_t address, std::size_t bytes);

    static void NoteQueuedLabel(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue);

    static void ForgetQueuedLabels();

    static void SetQueuedLabelRecorder(void (*recorder)());

    static bool SnapshotWriteOverlaps(std::uint64_t address, std::size_t bytes);

    using WriteRanges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
    static std::shared_ptr<const WriteRanges> PendingWriteSnapshot();
    static bool SnapshotOverlaps(const WriteRanges* snapshot, std::uint64_t address, std::size_t bytes);

    void AfterCompletions(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue, bool storedOnGpu);

    static void NoteWrittenBack(std::uint64_t address, std::size_t bytes);

    static std::optional<std::chrono::steady_clock::time_point> PendingLabelSince();

    static std::uint64_t WriteGeneration();

    static std::uint64_t PublishGeneration();

    static std::uint64_t PendingCompletionLabels();

    static std::uint64_t PendingWriteBackCompletions();

    static std::uint64_t RecordedWorkSinceSubmit();
    static void CountRecordedWork();

    static Recorder* Active();

    static bool InCompletion();
    void Activate();

    static void CountSync(int source, const void* site = nullptr);

    static void AnnounceSyncSite(const void* site);

    static double ThreadWaitedMs();

    static std::uint64_t ThreadHookWaits();

    struct StoreStatistics {
        std::uint64_t stores, runs, joined, replaced, wawBarriers, joinsRefused, queuedNoted, queuedOverRecorded, queuedHits, queuedHookRecords, queuedHookInCompletion;

        std::uint64_t keyStores, keyStoreRuns, keyStoreRunsForWriter, keyStoresJoined;

        std::uint64_t runsAtSubmit, runsForced;
    };
    static StoreStatistics StoreCounts();

    static constexpr std::uint32_t NoTiming = 0xffffffffu;
    static bool GpuTimingEnabled();
    static bool BatchStampsEnabled();
    std::uint32_t BeginGpuTiming(std::uint64_t key);

    void EndGpuTiming(std::uint32_t index, std::uint64_t bytes = 0);

    static constexpr std::uint64_t BatchTimingKey = 0x3;

    enum class CommandClass : std::uint8_t { DispatchLeading = 0, DispatchTrailing, IndirectArguments, LabelRun, Fill, FillClear, Copy, StagingIn, StagingOut, Draw, StorageUpload, StorageWriteBack, DccClear, DccKeyStore, PresentBlit, ShadowPublish, TemplateDataRefresh, Count };
    static constexpr std::uint64_t ClassKey(CommandClass which) { return 0x10 + static_cast<std::uint64_t>(which); }
    std::uint32_t BeginGpuTiming(CommandClass which) { return BeginGpuTiming(ClassKey(which)); }
    static void CountBarriers(CommandClass which, std::uint32_t count = 1);

    static bool MergeBarriers();
    static void CountMerged(CommandClass which);

    struct Access {
        std::span<const std::pair<std::uint64_t, std::uint64_t>> reads;
        std::span<const std::pair<std::uint64_t, std::uint64_t>> writes;

        std::span<const std::pair<VkImage, bool>> images;
        VkPipelineStageFlags stages;
        bool conservative = false;
    };
    static bool BarrierValidate();
    void NoteAccess(CommandClass which, const Access& access);

    static void AddGpuTiming(CommandClass which, double nanoseconds, std::uint64_t bytes);

    static void CountPresent();

    static std::uint64_t Presents();

    struct Completed {
        std::uint64_t serial = 0;
        double gpuStartNs = 0;
        double gpuEndNs = 0;
        std::chrono::steady_clock::time_point submittedAt{};
        std::chrono::steady_clock::time_point fenceSeenAt{};
        std::uint64_t readGeneration = 0;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
    };

    static constexpr std::size_t CompletedRingSize = 512;
    std::vector<Completed> CompletedBatches(std::uint64_t afterSerial, std::uint64_t throughSerial, std::size_t& missing) const;

    std::uint64_t NewestSubmitted(std::chrono::steady_clock::time_point* submittedAt = nullptr) const;

    std::size_t UnsignaledBatches() const;
    static bool FlipReadCheck();

private:
    struct Batch {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        std::vector<std::shared_ptr<void>> kept;
        std::vector<std::function<void()>> completions;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> writes;
        std::vector<std::uint64_t> writeNotes;

        struct Read {
            std::uint64_t begin;
            std::uint64_t end;
            ReadKind kind;
        };
        std::vector<Read> reads;

        std::uint64_t serial = 0;
        bool submitted = false;

        std::uint32_t queue = 0xffffffffu;
        std::chrono::steady_clock::time_point submittedAt{};
        VkQueryPool queries = VK_NULL_HANDLE;
        VkQueryPool samples = VK_NULL_HANDLE;
        std::vector<std::uint64_t> timedKeys;
        std::vector<std::uint64_t> timedBytes;

        std::uint32_t batchTiming = NoTiming;
        double gpuStartNs = 0;
        double gpuEndNs = 0;
        std::uint64_t readGeneration = 0;

        std::vector<std::uint64_t> labelDwords;
        std::vector<std::multimap<std::uint64_t, std::uint64_t>::iterator> wideLabels;

        std::uint32_t completionLabelCount = 0;

        std::uint32_t writeBackCompletionCount = 0;

        struct CompletionLabel {
            std::uint64_t begin;
            std::uint64_t end;
            bool counted;
        };
        std::vector<CompletionLabel> completionLabelRanges;

        struct StoreRun {
            bool open = false;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            std::vector<std::byte> bytes;
            std::vector<std::tuple<VkBuffer, VkDeviceSize, VkDeviceSize>> recorded;

            std::uint32_t timing = NoTiming;
            std::uint64_t storedBytes = 0;

            struct Queued {
                VkBuffer buffer;
                VkDeviceSize offset;
                std::vector<std::byte> bytes;
                std::uint64_t address;
            };
            std::vector<Queued> queued;
        } run;

        VkAccessFlags coveredAccess = 0;

        struct Tracker {
            struct Range {
                std::uint64_t begin;
                std::uint64_t end;
                VkPipelineStageFlags stages;
            };
            std::vector<Range> reads;
            std::vector<Range> writes;
            struct Image {
                VkImage image;
                bool written;
                VkPipelineStageFlags stages;
            };
            std::vector<Image> images;
        } tracker;

        struct RenderPass {
            bool open = false;
            bool continuable = false;
            std::uint64_t key = 0;
            std::uint32_t timing = NoTiming;
        } renderPass;

        bool hostReadOwed = false;

        struct KeyStore {
            VkBuffer buffer;
            VkDeviceSize first;
            VkDeviceSize last;
            std::shared_ptr<void> seed;
            std::uint64_t begin;
            std::uint64_t end;
        };
        std::vector<KeyStore> keyStores;
    };
    struct LabelEntry {
        std::uint32_t value;
        std::uint32_t queue;
        std::uint64_t stamp;

        const Batch* batch;

        std::uint64_t generation = 0;
        bool overwritten = false;

        bool behindCompletion = false;
    };
    void readGpuTiming(Batch& batch);
    void beginSamples(Batch& batch);
    void readSamples(Batch& batch);
    bool countingSamples = false;

    std::uint32_t beginTiming(std::uint64_t key);

    PFN_vkGetFenceStatus getFenceStatus = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkCmdUpdateBuffer cmdUpdateBuffer = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    template<typename TFunction>
    TFunction function(TFunction resolved, const char* name) const {
        return resolved != nullptr ? resolved : context.Function<TFunction>(name);
    }
    VkResult fenceStatus(VkFence fence) const { return function(getFenceStatus, "vkGetFenceStatus")(context.device, fence); }
    void recordBarrier(VkCommandBuffer commands, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) const;

    void ensureOpen();

    void flushPendingStore();

    bool closeStoreRun(bool atSubmit = false);

    void endOpenRenderPass();

    void recordKeyStores(bool forWriter);

    bool syncThroughUnlocked(std::uint64_t address, std::uint64_t end, int source, const void* site);

    void finish(std::unique_ptr<Batch> batch, bool wait, int source);
    void release(Batch& batch) noexcept;
    static bool overlaps(const Batch& batch, std::uint64_t address, std::uint64_t end);

    static const Batch::Read* readOverlap(const Batch& batch, std::uint64_t address, std::uint64_t end);
    bool signaled(const Batch& batch) const;

    void publishPendingWrites() const;

    bool noteWrite(std::uint64_t address, std::size_t bytes, bool ownLabel = false);

    void noteWriteOn(Batch& batch, std::uint64_t address, std::size_t bytes, bool ownLabel = false);
    void noteLabelOn(Batch& batch, std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue, bool behindCompletion = false);

    void markBehindCompletion(const Batch& batch, std::uint64_t begin, std::uint64_t end);

    void markOverwritten(std::uint64_t address, std::uint64_t end);

    bool writtenBackSince(std::uint64_t sequence, std::uint64_t begin, std::uint64_t end);
    std::mutex writtenBackMutex;
    std::deque<std::array<std::uint64_t, 3>> writtenBack;
    std::uint64_t writtenBackSequence = 0;

    std::optional<LabelHit> lookupLabel(std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp, LabelRefusal* refusal) const;

    bool unsignaled(std::uint64_t serial) const;

    Context context;
    VkSemaphore timeline = VK_NULL_HANDLE;

    std::uint64_t id = 0;

    std::map<std::uint64_t, LabelEntry> labels;

    std::map<std::uint64_t, LabelEntry> queuedLabels;
    std::multimap<std::uint64_t, std::uint64_t> wideLabels;
    std::uint64_t wideLabelBytes = 0;
    bool wideLabelInLocked(std::uint64_t address, std::size_t bytes) const;

    std::atomic<std::size_t> recordedLabels{0};
    std::unique_ptr<Batch> open;
    std::deque<std::unique_ptr<Batch>> inFlight;

    std::vector<const Batch*> finishing;
    std::uint64_t submissions = 0;
    std::uint64_t writeNoteCount = 0;

    std::vector<std::pair<VkCommandBuffer, VkFence>> spare;
    std::vector<VkQueryPool> sparePools;
    mutable std::mutex completedMutex;
    std::array<Completed, CompletedRingSize> completed;
    std::uint64_t newestSubmitted = 0;
    std::chrono::steady_clock::time_point newestSubmittedAt{};
    using DrawSnapshotKey = std::tuple<std::uint64_t, SnapshotUse, std::size_t>;
    struct DrawSnapshot {
        std::uint64_t generation;
        std::uint64_t registryGeneration;
        std::list<DrawSnapshotKey>::iterator recent;
        std::shared_ptr<Buffer> buffer;
        std::uint32_t derived;
    };
    struct DrawSnapshotPool {
        std::list<DrawSnapshotKey> recency;
        std::size_t bytes = 0;
    };
    std::map<DrawSnapshotKey, DrawSnapshot> drawSnapshots;
    std::array<DrawSnapshotPool, 2> drawSnapshotPools;
    void eraseDrawSnapshot(std::map<DrawSnapshotKey, DrawSnapshot>::iterator entry);
};

}

#endif
