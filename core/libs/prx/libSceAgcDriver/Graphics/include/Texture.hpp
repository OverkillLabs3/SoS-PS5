#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXTURE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

class ResidentColor;
class CommandBatch;
class StorageTexture;
struct HostImport;

VkFormat StorageFormatForGuest(const Context& context, std::uint32_t guestFormat);

bool StorageFormatAvailable(const Context& context, std::uint32_t guestFormat);

bool StorageClearAvailable(const Context& context, std::uint32_t guestFormat, DccKeys keys);

struct OwnedImage {
    OwnedImage(const Context& context, VkImage image, VkDeviceMemory memory) : context(context), image(image), memory(memory) {}
    OwnedImage(const OwnedImage&) = delete;
    OwnedImage& operator=(const OwnedImage&) = delete;
    ~OwnedImage() {
        if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
        if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
    }
    Context context;
    VkImage image;
    VkDeviceMemory memory;
};

class Texture {
public:
    Texture(const Context& context, TextureDetiler& detiler, const GuestTextureResource& descriptor, VkComponentMapping components, std::span<const std::byte> snapshot, bool depthCompare = false);
    Texture(const Context& context, const std::shared_ptr<ResidentColor>& source, const GuestTextureResource& descriptor, VkComponentMapping components);

    Texture(const Context& context, const std::shared_ptr<StorageTexture>& source, const GuestTextureResource& descriptor, VkComponentMapping components);
    Texture(const Context& context, VkImage depthImage, VkFormat depthFormat, VkImageAspectFlags aspect, VkComponentMapping components);
    static bool CanCopyFrom(const StorageTexture& source, const GuestTextureResource& descriptor);
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    VkImageView View() const;
    VkImageView FirstLayerView() const { return firstLayerView; }

    VkImageLayout Layout() const { return layout; }
    VkDeviceSize AllocationBytes() const { return allocationBytes; }

    bool ViewsStorageImage() const { return storageSource != nullptr; }
    const StorageTexture* StorageSource() const { return storageSource.get(); }
    const std::shared_ptr<StorageTexture>& SharedStorageSource() const { return storageSource; }

    DccKeyProof& KeyProof() const { return keyProof; }

private:
    void release() noexcept;
    void createFirstLayerView(const GuestTextureResource& descriptor, VkImageViewCreateInfo viewInfo);

    Context context;
    mutable DccKeyProof keyProof;

    VkImage image = VK_NULL_HANDLE;
    std::shared_ptr<OwnedImage> owned;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView firstLayerView = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDeviceSize allocationBytes = 0;
    std::shared_ptr<ResidentColor> source;
    std::shared_ptr<StorageTexture> storageSource;
    std::unique_ptr<CommandBatch> upload;
};

class StorageTexture : public std::enable_shared_from_this<StorageTexture> {
public:
    StorageTexture(const Context& context, TextureDetiler& detiler, const GuestTextureResource& descriptor, std::uint32_t mipLevel);
    ~StorageTexture();
    StorageTexture(const StorageTexture&) = delete;
    StorageTexture& operator=(const StorageTexture&) = delete;

    VkImageView View() const;

    VkImageView View(std::uint32_t mip);
    VkImageView FirstLayerView(std::uint32_t mip);

    bool Attachable() const { return attachable; }
    VkImageView AttachmentView(VkFormat format, std::uint32_t mip = 0);

    VkFormat AttachmentFormat() const { return attachmentFormat; }
    void WriteBack();

    void MarkDirty();
    void Flush();

    static bool FlushPending(std::uint64_t address, std::size_t bytes, const StorageTexture* except = nullptr, const char* reason = "memory access", PublishScope scope = PublishScope::Whole, bool* published = nullptr);
    static void FlushAllPending(const char* reason);

    static void BumpPendingSerial();

    static bool AnyPendingOverlaps(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);

    static std::uint64_t PendingSerial();

    struct PendingQuery {
        std::uint64_t begin;
        std::uint64_t end;
        const StorageTexture* except;
        const StorageTexture* pending;
        bool overlaps;
        const StorageTexture* found = nullptr;
    };
    static bool ScanPending(std::span<PendingQuery> queries);

    struct AccessClassification {
        std::size_t images = 0;
        std::size_t dead = 0;
        std::size_t live = 0;
        bool allCpuWritten = false;

        bool checked = false;
    };
    static constexpr std::uint64_t DeadImagePresents = 2;
    static void ClassifyAccess(std::uint64_t address, std::size_t bytes, AccessClassification& out);

    static constexpr std::size_t ClassifyLimit = 1u << 20u;
    static bool AccessKeptByCpu(std::uint64_t address, std::size_t bytes, std::size_t* images = nullptr, std::size_t* evicted = nullptr);

    static bool PublishShadowsOnly(std::uint64_t address, std::size_t bytes, PublishScope scope, const char* reason = "memory access");

    struct HookSkipCounts {
        std::uint64_t flushedAfterSkip;
        std::uint64_t flushedAfterSkipSameSite;
    };
    static HookSkipCounts TakeHookSkipCounts();

    void NoteProved() const;

    bool Cached() const { return cached.load(std::memory_order_acquire); }
    void SetCached(bool value) { cached.store(value, std::memory_order_release); }

    static std::shared_ptr<StorageTexture> FindPending(std::uint64_t address, std::uint64_t bytes);
    static std::shared_ptr<StorageTexture> FindPending(const GuestTextureResource& resource, std::uint64_t bytes);

    enum class FillCover { None, Exact, Inside, Around, Straddle, Several, Keys, Layer };
    struct FillCoverage {
        FillCover cover = FillCover::None;
        std::shared_ptr<StorageTexture> image;
        std::uint32_t layer = 0;
        std::size_t others = 0;
        std::size_t inside = 0;
    };
    static FillCoverage ClassifyFill(std::uint64_t address, std::size_t bytes);
    static std::size_t NoteKeysFill(std::uint64_t address, std::size_t bytes, std::uint8_t key);
    static std::size_t ClearByKeysFill(std::uint64_t address, std::size_t bytes, std::uint8_t key);

    static std::size_t DiscardPendingInside(std::uint64_t address, std::size_t bytes);

    static constexpr std::uint32_t WholeImage = ~0u;
    bool FillClear(std::span<const std::uint32_t, 4> pattern, std::uint32_t layer, const char*& refusal);

    static std::shared_ptr<StorageTexture> FindLive(std::uint64_t address, std::uint64_t bytes);

    bool SameSurfaceShape(const StorageTexture& other) const;

    bool CopyFrom(StorageTexture& source, const char*& refusal);
    VkImage Image() const { return image; }
    const GuestTextureResource& Descriptor() const { return descriptor; }
    std::uint32_t ImageLayers() const { return geometry.imageLayers; }
    std::uint32_t ImageDepth() const { return geometry.imageDepth; }

    std::uint64_t Version() const { return version; }
    std::uint64_t Generation() const { return generation; }

    bool CpuAddressable() const { return descriptor.tileMode == TextureTileMode::kLinear; }

    DccKeys UploadedKeys() const { return uploadedKeys; }
    DccKeys FilledKeys() const { return filledKeys; }
    DccKeyProof& KeyProof() const { return keyProof; }

    bool Refresh();
    std::uint64_t GuestBytes() const;

private:

    std::vector<VkBufferImageCopy> CopyRegions(const std::vector<bool>* layers = nullptr) const;

    void upload(const std::vector<bool>* layers = nullptr);

    void writeBack(std::uint64_t address, std::size_t bytes);
    void writeBackLayers(const std::vector<bool>& layers);

    std::uint64_t layerBytes(std::uint32_t layer) const { return std::min<std::uint64_t>(trackedLayerBytes, guestBytes - static_cast<std::uint64_t>(layer) * trackedLayerBytes); }

    std::vector<std::pair<std::uint64_t, std::uint64_t>> unitRuns(const std::vector<bool>& units) const;
    struct SliceWindow {
        std::uint32_t layer;
        std::uint32_t level;

        std::uint64_t tiledBegin;
        std::uint64_t tiledEnd;
        DetileWindow window;
        std::uint64_t linearBytes;

        std::vector<VkBufferImageCopy> regions;
    };

    std::vector<SliceWindow> sliceWindows(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs) const;

    std::uint64_t uploadWindows(const HostImport& import, std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, bool discard = false);
    std::uint64_t writeBackWindows(const HostImport& import, std::span<const std::pair<std::uint64_t, std::uint64_t>> keep, std::uint64_t firstStored, std::uint64_t lastStored, std::vector<ShadowedRange>& shadowed, std::vector<std::pair<std::uint64_t, std::uint64_t>>& imported);

    std::uint64_t layerBegin(std::uint32_t layer) const { return descriptor.baseAddress + static_cast<std::uint64_t>(layer) * trackedLayerBytes; }
    bool anyLayerPending() const;
    void refreshGeneration();

    void markLayersPending(std::uint32_t first, std::uint32_t count);
    void acknowledgeWrites(std::uint32_t first, std::uint32_t end);
    void snapshotSharedBlocks(std::uint32_t first, std::uint32_t end);

    void reconcilePending();

    struct Adjacent {
        std::shared_ptr<StorageTexture> texture;
        std::uint32_t layer;
        std::uint64_t generation;
    };
    std::vector<Adjacent> adjacentPendingUnchanged(std::uint64_t firstBlock, std::uint64_t lastBlock) const;

    static void advanceAdjacent(const std::vector<Adjacent>& adjacent, std::uint64_t now, std::uint64_t firstBlock, std::uint64_t lastBlock);

    void blockGenerations(std::vector<std::uint64_t>& generations) const;

    std::shared_ptr<StorageTexture> pendingAlias() const;

    static std::vector<std::shared_ptr<StorageTexture>> overlappingPending(std::uint64_t address, std::size_t bytes);

    static bool blocksKept(std::span<const std::shared_ptr<StorageTexture>> images, std::uint64_t address, std::size_t bytes);

    bool evictStale(std::uint64_t address, std::size_t bytes, std::size_t& dropped);

    bool skippedResultsInside(std::uint64_t address, std::size_t bytes) const;
    std::uint64_t borrowUnits(StorageTexture& source, const std::vector<bool>& units);
    void forgetBorrowed(std::uint32_t first, std::uint32_t count);
    bool clearByKeysFill(DccKeys keys, std::uint8_t key);
    bool overlaps(std::uint64_t address, std::size_t bytes) const;
    bool pendingUnitInside(std::uint64_t address, std::size_t bytes) const;
    VkImageView createView(std::uint32_t mip, bool firstLayer = false) const;
    void release() noexcept;

    Context context;
    TextureDetiler& detiler;
    GuestTextureResource descriptor;
    std::vector<TileMipLayout> mips;
    std::uint32_t arrayLayers = 1;
    std::uint64_t guestBytes = 0;
    std::uint64_t sliceLinearBytes = 0;
    SurfaceGeometry geometry;
    std::vector<std::byte> original;

    DccKeys uploadedKeys = DccKeys::Uncompressed;
    DccKeys filledKeys = DccKeys::Uncompressed;
    mutable DccKeyProof keyProof;

    std::uint64_t generation = 0;
    std::uint32_t trackedLayers = 1;
    std::uint64_t trackedLayerBytes = 0;
    std::vector<std::uint64_t> layerGeneration;
    std::vector<bool> layerPending;
    bool blockUnits = false;

    struct BoundaryBytes {
        std::uint64_t from;
        std::uint64_t to;
        std::vector<std::byte> bytes;
    };
    std::vector<BoundaryBytes> boundaryBytes;

    std::uint32_t partialStores = 0;
    std::uint64_t partialWindowStart = 0;
    std::uint64_t storeWholeUntil = 0;

    std::weak_ptr<StorageTexture> borrowedFrom;
    std::uint64_t borrowedVersion = 0;
    std::vector<bool> borrowedUnits;

    bool lent = false;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::uint32_t defaultMip = 0;
    std::map<std::uint32_t, VkImageView> extraViews;
    std::map<std::uint32_t, VkImageView> firstLayerViews;
    bool attachable = false;
    std::map<std::pair<VkFormat, std::uint32_t>, VkImageView> attachmentViews;
    VkFormat storageFormat = VK_FORMAT_UNDEFINED;
    VkFormat attachmentFormat = VK_FORMAT_UNDEFINED;

    bool dirty = false;
    std::uint64_t version = 0;

    bool originalValid = true;

    bool released = false;
    std::atomic<bool> cached{false};

    mutable std::atomic<std::uint64_t> provedPresent{0};

    std::atomic<std::uint32_t> hookSkips{0};
    std::atomic<std::uint8_t> hookSkipSite{0};
    std::uint32_t hookSkipLayer = 0;
    std::uint64_t hookSkipVersion = 0;
    std::uint64_t hookSkipGeneration = 0;

    std::uint64_t staleCheckPresent = ~0ull;

    std::uint64_t layerRefusedGeneration = ~0ull;
};

struct LookupOutcomes {
    enum Kind : std::size_t { SampledFast, SampledFastMiss, SampledHitView, SampledHitClearedView, SampledHitSnapshot, SampledMadeView, SampledMadeSnapshot, StorageHit, StorageMade, StorageDepthCheck, StorageKeyBuild, StorageLockWait, RefreshUnchanged, RefreshCompared, UploadDirect, UploadCpu, UploadClear, DccScan, PendingFlush, Count };
    static const char* Name(Kind kind);
    static bool Profiled();

    static std::chrono::steady_clock::time_point Add(Kind kind, std::chrono::steady_clock::time_point start);
    std::array<std::uint64_t, Count> counts{};
    std::array<double, Count> ms{};
};
LookupOutcomes& ThreadLookupOutcomes();

}

#endif
