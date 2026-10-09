#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTBUFFERMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTBUFFERMEMORY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "BdaAbi.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

void ShutdownGuestBufferWorkers();
void ClearImageMirrors(VkDevice device);
void ClearHostImports(VkDevice device);

struct GuestMemorySnapshot {
    std::uint64_t address;
    std::span<const std::byte> bytes;
};

struct HostImport {
    std::uint64_t base;
    std::uint64_t bytes;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceAddress address;
    void* alias = nullptr;

    std::uint64_t serial = 0;
    bool unwatched = false;
};

enum class ImportWatch : std::uint8_t { Watch, Unwatch };

struct ImportProbe {
    const char* failure = nullptr;
    VkResult result = VK_SUCCESS;
    std::uint32_t pages = 0;
    std::uint32_t writtenAtImport = 0;
    std::uint32_t writtenAfterSubmit = 0;
};

ImportProbe ProbeImportWriteProtection(const Context& context);
ImportWatch PrepareImportWatch(const Context& context);
void SetImportWatch(const Context& context, ImportWatch watch);

const HostImport* HostImportFor(const Context& context, std::uint64_t address, std::size_t bytes);

bool HostImportCovers(const Context& context, std::uint64_t address, std::size_t bytes);

bool RegisteredReadableCovers(std::uint64_t address, std::size_t bytes);

struct ImageMirror;
class Recorder;

std::uint64_t HostImportSerial(const Context& context, std::uint64_t address, std::size_t bytes, bool reconcile);

struct HostImportsProof {
    VkDevice device = VK_NULL_HANDLE;
    std::uint64_t epoch = 0;
    std::uint64_t refreshedGeneration = 0;
};

bool HostImportsUnchanged(const Context& context, const HostImportsProof& proof);

HostImportsProof HostImportsIdentity(const Context& context);

std::uint64_t ImageMirrorSerial(const Context& context, std::uint64_t address, std::size_t bytes);

bool SyncLeaseWork();
void CountLeaseOutcome(bool synced, std::uint64_t batchSerial);
struct LeaseStats {
    std::uint64_t deferred = 0;
    std::uint64_t synced = 0;

    std::uint64_t contentionWaits = 0;
    std::uint64_t contentionSyncs = 0;
    std::uint64_t contentionDrains = 0;
    std::uint64_t cacheDrops = 0;
    double contentionMs = 0;
};
LeaseStats LeaseCounters();

struct AddressSpaceStats {
    bool enabled = true;
    std::uint64_t hits = 0;
    std::uint64_t rebuiltFirst = 0;
    std::uint64_t rebuiltGeneration = 0;
    std::uint64_t rebuiltEpoch = 0;
    std::uint64_t rebuiltDevice = 0;
    std::uint64_t rebuiltWaiterDrop = 0;
    std::uint64_t unpublished = 0;

    std::uint64_t dissolvedOverlap = 0;
    std::uint64_t dissolvedImports = 0;
    std::uint64_t waiterDrops = 0;
};
AddressSpaceStats AddressSpaceCounters();

struct MirrorStats {
    std::uint64_t heapMirrors = 0;
    std::uint64_t heapBytes = 0;
    std::uint64_t rebuilds = 0;
    std::uint64_t blocksCopied = 0;
    std::uint64_t heapRefills = 0;
};
MirrorStats MirrorCounters();

struct AddressCopy {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t committed;
    const char* reason;
};
std::string AddressCopyOverflow(std::vector<AddressCopy> copies, std::uint64_t limit);

class GuestBufferMemory {
public:
    explicit GuestBufferMemory(const Context& context);
    ~GuestBufferMemory();
    void AcquireRegistered();

    static void CountAddressBuild(double snapshotsUs);

    void AddWritable(std::uint64_t address, std::size_t bytes, bool atomic = false);
    void AddReadable(std::uint64_t address, std::size_t bytes);

    void AllowDeviceStaging() { stagingAllowed = true; }
    void AllowAdjustedRegions() { adjustedRegions = true; }

    void RecordStagingCopies(Recorder& recorder);
    void AddSnapshot(const GuestMemorySnapshot& snapshot);

    void Upload(bool addressable);
    void UploadPrepare(bool addressable);
    void UploadFinish(bool addressable);
    VkDescriptorBufferInfo Descriptor(std::uint64_t address, std::size_t bytes, std::uint32_t& adjustment) const;
    std::vector<ShaderRecompiler::BdaAbi::Range> AddressRanges() const;

    struct CachedTable {
        std::uint64_t serial;
        const std::vector<ShaderRecompiler::BdaAbi::Range>* ranges;
    };
    std::optional<CachedTable> CachedAddressTable() const;

    struct AddressSpace;
    void WriteBack();
    bool WritesOverlap(std::uint64_t address, std::size_t bytes) const;

    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& Writes() const { return writes; }

    bool HasCopiedWrites() const;

    void MarkDirectWrites() const;

    void RecordCopyBacks(Recorder& recorder);

    bool HoldsLease() const { return !lease.empty() || space != nullptr; }

    std::optional<std::vector<std::pair<std::uint64_t, std::uint64_t>>> DirectRegions() const;

    std::vector<std::pair<std::uint64_t, std::uint64_t>> InPlaceReads() const;

private:
    struct Region {
        std::uint64_t begin;
        std::uint64_t end;
        bool writable;
        std::vector<std::byte> snapshot;

        std::shared_ptr<Buffer> buffer;

        std::vector<std::byte> uploaded {};

        bool hostBacked = false;

        const HostImport* direct = nullptr;

        bool sparse = false;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> backed {};

        std::shared_ptr<ImageMirror> mirror {};

        bool subrangeMirror = false;

        bool pending = false;

        bool gpuCopy = false;
        VkBuffer copySource = VK_NULL_HANDLE;
        std::uint64_t copySourceBase = 0;
        bool copiedBack = false;

        bool atomic = false;

        bool deviceLocal = false;

        bool unstaged = false;
    };

    enum class BaseOverlap { None, Inside, Partial };
    BaseOverlap baseOverlap(std::uint64_t begin, std::uint64_t end, const Region** owner) const;

    void dissolveSpace(bool resolve);

    const Region* owner(std::uint64_t address) const;

    struct CopiedRange {
        std::uint64_t begin;
        std::uint64_t end;
        bool writable;
    };
    void addCopiedRange(const CopiedRange& range);

    static ShaderRecompiler::BdaAbi::Range addressRange(const Region& region);
    void validate(std::uint64_t address, std::size_t bytes) const;

    void addDescriptorRegion(std::uint64_t address, std::size_t bytes, bool atomic);

    void copyRegion(Region& region, bool addressable);

    bool gpuCopyEligible(const Region& region) const;

    bool stagingEligible(const Region& region, bool addressable) const;
    bool bindableInPlace(std::uint64_t offset, bool addressable) const;

    void recordGpuCopies(std::span<Region* const> copies, bool addressable);
    void takeHeapReferences();
    Context context;
    bool stagingAllowed = false;
    bool adjustedRegions = false;
    GuestAllocations::Lease lease;

    std::shared_ptr<const AddressSpace> space;

    std::uint64_t importsEpoch = 0;
    std::vector<Region> regions;

    bool regionsSorted = false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> writes;
    std::vector<std::pair<std::uint64_t, std::vector<std::byte>>> heapReferences;

    bool prepared = false;
    bool uploaded = false;
    bool committed = false;
};

}

#endif
