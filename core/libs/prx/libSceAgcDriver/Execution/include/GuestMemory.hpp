#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTMEMORY_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::GuestMemory {

void CheckRange(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable = false);

bool Accessible(const void* pointer, std::size_t bytes, bool writable = false);

std::vector<std::pair<std::uint64_t, std::uint64_t>> CommittedRanges(std::uint64_t address, std::size_t bytes, bool writable = false);

struct Commitment {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    bool whole = false;
};
Commitment DescribeCommitted(std::uint64_t address, std::size_t bytes, bool writable = false);
void Read(std::uint64_t address, std::span<std::byte> destination, std::size_t alignment = 1);

void ReadCommitted(std::uint64_t address, std::span<std::byte> destination);
void ReadCommittedUnsynced(std::uint64_t address, std::span<std::byte> destination);
bool EqualsCommitted(std::uint64_t address, std::span<const std::byte> bytes);

bool EqualsCommittedUnsynced(std::uint64_t address, std::span<const std::byte> bytes);

enum class Compare : std::uint8_t { Equal, Differs, Unmapped };
Compare CompareMapped(std::uint64_t address, std::span<const std::byte> bytes);

Compare CopyMapped(std::uint64_t address, std::span<std::byte> out);
void WriteChangedCommitted(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original);
void Write(std::uint64_t address, std::span<const std::byte> source, std::size_t alignment = 1);

void WriteChanged(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original);

bool WriteWatched();
void Unwatch(std::uint64_t address, std::size_t bytes);
bool ImportWatched(std::uint64_t address, std::size_t bytes, const std::function<bool()>& import);
bool Watched(std::uint64_t address, std::size_t bytes);
std::uint64_t CollectWrites(std::uint64_t address, std::size_t bytes);
bool UnchangedSince(std::uint64_t address, std::size_t bytes, std::uint64_t generation);

struct UnchangedQuery {
    std::uint64_t address;
    std::size_t bytes;
    std::uint64_t generation;
};
bool WrittenSince(std::uint64_t address, std::size_t bytes, std::uint64_t generation);
bool UnchangedSinceAll(std::span<const UnchangedQuery> queries);

std::uint64_t MarkWritten(std::uint64_t address, std::size_t bytes);

void BumpCollectEpoch();
std::uint64_t CollectEpochBumps();

void BeginCollectFrame();
std::uint64_t CollectWritesUncached(std::uint64_t address, std::size_t bytes);

std::uint64_t TrackerGeneration();

bool UnchangedSinceCollected(std::uint64_t address, std::size_t bytes, std::uint64_t generation);

constexpr std::uint8_t BlockUnchanged = 0;
constexpr std::uint8_t BlockWritten = 1;
constexpr std::uint8_t BlockMaybeWritten = 2;
bool ChangedBlocks(std::uint64_t address, std::size_t bytes, std::span<const std::uint64_t> generations, std::span<std::uint8_t> changed, std::span<std::uint8_t> cpu = {});

class GpuMutexType {
public:
    void lock();
    bool try_lock();
    void unlock();

    bool HeldByThisThread() const;

    std::uint32_t DepthOnThisThread() const;

private:
    void acquired();
    std::recursive_mutex mutex;

    std::atomic<const void*> owner{nullptr};
    std::uint32_t depth = 0;
};
GpuMutexType& GpuMutex();

void SetGpuUnlockHook(void (*hook)());

void AssertGpuLockHeld(const char* where);
void TagGpuLockThread(std::uint32_t queue);

void MarkPresenterThread();

std::uint32_t GpuLockThreadTag();

enum class GpuLockSite : std::uint8_t { Other = 0, Dispatch, Indirect, Draw, Hook, Wait, Label, Flush, Present, Fill, Copy, End, Try, Count };
void TagGpuLockSite(GpuLockSite site);

void NoteLockedGpuWait(double ms);

std::uint64_t ThreadCollectedBytes();

std::uint64_t ThreadCollectEpoch();

std::uint64_t CurrentCollectFrame();
void SetTraceFrame(std::uint64_t frame);
std::uint64_t TraceFrame();

unsigned long long CodeOffset(const void* address);

void SetFlushHook(void (*hook)(std::uint64_t address, std::size_t bytes));
void FlushGpuWrites(std::uint64_t address, std::size_t bytes);

std::uint64_t ForgetSerial();

struct PacketTag {
    std::uint32_t opcode;
    std::uint32_t queue;
};
constexpr std::uint32_t NoPacket = 0xfffffffeu;
void SetCurrentPacket(std::uint32_t opcode, std::uint32_t queue);
PacketTag CurrentPacket();

enum class ReadSite : std::uint8_t { Unknown = 0, Capture, DispatchCache, TextureCompare, TextureRead, BufferUpload, IndexBuffer, VertexBuffer, Registers, IndirectArguments, Wait, Label, Scanout, Store, MirrorRefresh, DrawCache, Count };
const char* ReadSiteName(ReadSite site);

ReadSite SetReadSite(ReadSite site);
ReadSite CurrentReadSite();
class ReadSiteScope {
public:
    explicit ReadSiteScope(ReadSite site) : previous(SetReadSite(site)) {}
    ~ReadSiteScope() { SetReadSite(previous); }
    ReadSiteScope(const ReadSiteScope&) = delete;
    ReadSiteScope& operator=(const ReadSiteScope&) = delete;

private:
    ReadSite previous;
};

std::size_t CaptureCallerOffsets(std::span<unsigned long long> frames, unsigned skip);

}

extern "C" void AgcDriverCheckGuestMemory_nid_postfix(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable = false);

#endif
