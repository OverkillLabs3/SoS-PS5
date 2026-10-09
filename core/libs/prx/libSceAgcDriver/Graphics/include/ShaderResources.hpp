#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADERRESOURCES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADERRESOURCES_HPP

#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BdaResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Sampler.hpp"
#include "Recompiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include <array>
#include <chrono>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Graphics {

class Recorder;

void FlushCachedTextures(VkDevice device);
void ClearCachedTextures(VkDevice device);

std::shared_ptr<StorageTexture> CachedStorageSurface(const Context& context, const GuestTextureResource& resource);

bool StorageImageCached(const Context& context, const StorageTexture* image);

bool PendingStorageOverlaps(std::uint64_t address, std::size_t bytes, const StorageTexture* except);

class DescriptorCache {
public:
    explicit DescriptorCache(const Context& context);
    ~DescriptorCache();
    DescriptorCache(const DescriptorCache&) = delete;
    DescriptorCache& operator=(const DescriptorCache&) = delete;

    VkDescriptorSetLayout Layout(std::span<const std::uint32_t> key, std::span<const VkDescriptorSetLayoutBinding> bindings);
    struct SetAllocation {
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
    };

    SetAllocation Allocate(VkDescriptorSetLayout layout, std::span<const VkDescriptorPoolSize> sizes);
    void Free(const SetAllocation& allocation) noexcept;

    struct Stats {
        std::uint64_t layoutHits = 0;
        std::uint64_t layoutMisses = 0;
        std::uint64_t sets = 0;
        std::uint64_t pools = 0;
    };
    Stats Counters() const;

private:
    Context context;
    PFN_vkDestroyDescriptorSetLayout destroyLayout;
    PFN_vkDestroyDescriptorPool destroyPool;
    PFN_vkFreeDescriptorSets freeSets;
    mutable std::mutex mutex;
    std::map<std::vector<std::uint32_t>, VkDescriptorSetLayout> layouts;
    std::vector<VkDescriptorPool> pools;
    Stats stats;
};

class ShaderResources {
public:
    ShaderResources(const Context& context, const ShaderRecompiler::RecompileResult& vertex, const ShaderRecompiler::RecompileResult& fragment, const ColorTarget& target, std::uint64_t indexAddress, std::size_t indexBytes);
    ShaderResources(const Context& context, std::span<const CompiledShader> shaders, const ColorTarget& target, std::uint64_t indexAddress, std::size_t indexBytes, std::span<const GuestMemorySnapshot> snapshots = {});
    ShaderResources(const Context& context, const CompiledShader& compute, std::span<const GuestMemorySnapshot> snapshots = {});

    ShaderResources(const Context& context, const CompiledShader& compute, std::span<const GuestMemorySnapshot> snapshots, bool deferred);
    void Complete();
    bool Completed() const { return completed; }
    ~ShaderResources();
    ShaderResources(const ShaderResources&) = delete;
    ShaderResources& operator=(const ShaderResources&) = delete;
    VkDescriptorSetLayout Layout() const;
    void Bind(VkCommandBuffer commands, VkPipelineBindPoint bindPoint, VkPipelineLayout layout) const;
    struct DrawBindings {
        DrawBindings() = default;
        DrawBindings(const DrawBindings&) = delete;
        DrawBindings& operator=(const DrawBindings&) = delete;
        struct Snapshot {
            std::uint64_t address;
            std::shared_ptr<Buffer> buffer;
        };
        DescriptorCache* cache = nullptr;
        DescriptorCache::SetAllocation allocation;
        std::vector<Snapshot> snapshots;
        ~DrawBindings();
    };
    std::shared_ptr<DrawBindings> PrepareDrawBindings(Recorder& recorder) const;
    void WriteBack();

    void MarkGpuWrites(Recorder& recorder);
    void WriteBackBuffers();

    bool NeedsCompletion() const { return bda != nullptr || guestMemory.HasCopiedWrites(); }

    bool HasCopiedWrites() const { return guestMemory.HasCopiedWrites(); }
    bool HoldsLease() const { return guestMemory.HoldsLease(); }
    bool WritesOverlap(std::uint64_t address, std::size_t bytes) const { return guestMemory.WritesOverlap(address, bytes); }

    bool ReadsOverlap(std::uint64_t address, std::size_t bytes) const;

    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& GpuWrites() const { return guestMemory.Writes(); }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> InPlaceReads() const { return guestMemory.InPlaceReads(); }
    std::vector<std::pair<VkImage, bool>> StorageImages() const;

    bool WritesMemory() const;
    bool ReadsImage(const StorageTexture* image) const;
    const std::vector<std::uint32_t>& LayoutKey() const { return layoutKey; }

    std::string Describe() const;

    static std::vector<std::uint32_t> ContentKey(const CompiledShader& shader, bool dataWords = true);

    bool RefreshData(VkCommandBuffer commands, const CompiledShader& shader, Recorder* recorder = nullptr);

    void PrecollectSurfaces() const;
    bool Reusable() const { return reusable; }

    enum class ProofPath : std::size_t { Fast, OwnRefreshed, Full, Count };
    enum class ProofFailure : std::size_t { None, Imports, Evicted, Pending, Changed, Keys, Other, Count };
    struct ProofReport {
        ProofPath path = ProofPath::Fast;
        ProofFailure failure = ProofFailure::None;
    };
    bool Revalidate(std::span<const CompiledShader> shaders, ProofReport* report = nullptr);
    bool Revalidate(const CompiledShader& shader, ProofReport* report = nullptr) { return Revalidate(std::span<const CompiledShader>(&shader, 1), report); }

    bool ProveCurrent(std::span<const CompiledShader> shaders, ProofReport* report = nullptr) { return Revalidate(shaders, report); }
    bool ProveCurrent(const CompiledShader& shader, ProofReport* report = nullptr) { return Revalidate(shader, report); }

    std::uint64_t DataWordsHash() const { return dataWordsHash; }
    static std::uint64_t DataWordsHash(const CompiledShader& shader);
    void PatchPushConstants(std::span<std::byte, PipelinePushConstantBytes> bytes) const {
        for (const auto& [position, adjustment] : pushPatches) bytes[position] = static_cast<std::byte>(adjustment);
    }

    bool DataWordsDiffer(const CompiledShader& shader) const;

    enum class FastFail : std::size_t { NoRecord, Collect, Pending, Evicted, Changed, Keys, ClearedView, StorageKeys, Count };

    enum class OwnRefreshFallback : std::size_t { Disabled, Snapshot, Keys, ForeignView, SurfaceKey, NotImported, Uncached, Rerun, Count };

    enum class BuildPhase : std::size_t { Bindings, Precollect, Upload, Descriptors, StageA, Images, Bda, StageB, Count };
    struct BuildTiming {
        double bindingsMs = 0;
        double uploadMs = 0;
        double descriptorsMs = 0;
        double prepareMs = 0;
        double completeMs = 0;
    };
    const BuildTiming& Timing() const { return timing; }

    std::vector<std::pair<std::uint64_t, std::uint64_t>> PresyncSurfaces() const;

private:
    struct DescribedRange {
        const char* kind;
        std::uint64_t address;
        std::uint64_t bytes;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t format = 0;
        int tileMode = 0;
        std::uint64_t dccAddress = 0;
    };
    std::vector<DescribedRange> describedRanges;
    struct Allocation {
        std::uint64_t address;
        std::size_t size;
        bool guest;
        std::unique_ptr<Buffer> buffer;
        ShaderRecompiler::DescriptorRole role = ShaderRecompiler::DescriptorRole::ShaderData;

        bool written = true;

        std::vector<std::uint32_t> dataWords;
        std::uint32_t adjustment = 0;
        std::int32_t pushByte = -1;
        std::int64_t dataAllocation = -1;
        std::uint32_t dataByte = 0;
    };
    struct DataPatch {
        std::size_t allocation;
        std::uint32_t byte;
        std::uint32_t adjustment;
    };
    void writeDataWords(VkCommandBuffer commands, std::size_t allocation, std::span<const std::uint32_t> words) const;

    struct Binding {
        VkDescriptorSetLayoutBinding layout;
        std::vector<std::size_t> allocations;
        std::vector<std::size_t> imageAllocations;
    };

    struct DirectRegion {
        std::uint64_t begin;
        std::uint64_t end;
        std::uint64_t serial;
    };

    void build(std::span<const CompiledShader> shaders, const ColorTarget* target, std::uint64_t indexAddress, std::size_t indexBytes);
    void buildPrepare(std::span<const CompiledShader> shaders, const ColorTarget* target, std::uint64_t indexAddress, std::size_t indexBytes);
    void buildComplete();

    double phase(BuildPhase which);

    std::size_t addGuestBuffer(std::span<const std::uint32_t> words, const ColorTarget* target, std::uint64_t indexAddress, std::size_t indexBytes, bool written, bool atomic);
    std::size_t addDataBuffer(std::span<const std::uint32_t> words);

    void addImageBinding(const ShaderRecompiler::DescriptorBinding& binding, VkShaderStageFlags flags);

    bool precollectImages();
    struct ImageRecord {
        bool sampled = false;

        bool decoded = false;
        std::array<std::uint32_t, 8> words{};
        GuestTextureResource resource{};
        std::uint64_t guestBytes = 0;
        VkComponentMapping components{};

        DccKeys keys = DccKeys::Uncompressed;
        std::uint64_t generation = 0;

        std::shared_ptr<Texture> texture;
        std::shared_ptr<StorageTexture> source;
        DccKeys entryKeys = DccKeys::Uncompressed;
        std::uint64_t entryGeneration = 0;
    };
    std::vector<ImageRecord> imageRecords;

    std::size_t nextImageRecord = 0;

    std::shared_ptr<Texture> fastTexture(const ImageRecord& record);
    void resolveImageBinding(const ShaderRecompiler::DescriptorBinding& binding, Binding& item);
    void forgetDeferredInputs();
    void release() noexcept;
    void prepareAddressBindings(std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots);
    VkDescriptorBufferInfo descriptor(Allocation& allocation);
    void noteReusable();
    void reportDescriptorCaches() const;

    struct ValidatedSurface {
        GuestTextureResource resource{};
        std::uint64_t bytes = 0;

        DccKeys keys = DccKeys::Uncompressed;

        std::uint64_t generation = 0;

        std::uint64_t collected = 0;

        const StorageTexture* source = nullptr;
        bool valid = false;
    };
    void captureValidation();

    struct PendingOverlap {
        std::size_t element;
        bool storage;
        bool viewUncompressed;
        bool sourceEligible = false;
    };

    bool fastRevalidate(std::uint64_t serialBefore, std::span<const PendingOverlap> refreshed, FastFail& reason, std::vector<PendingOverlap>& overlapping, bool& accepted);

    OwnRefreshFallback refreshOwnObjects(std::span<const CompiledShader> shaders, std::span<PendingOverlap> overlapping);

    bool fastRevalidateEach();
    Context context;
    std::vector<std::uint32_t> layoutKey;
    GuestBufferMemory guestMemory;
    std::unique_ptr<BdaResources> bda;
    bool usesBda = false;
    bool usesFaultBuffer = false;
    VkDescriptorSetLayout _layout = VK_NULL_HANDLE;

    bool ownsLayout = false;
    VkDescriptorSet _set = VK_NULL_HANDLE;

    VkDescriptorPool pool = VK_NULL_HANDLE;

    VkDescriptorPool cachePool = VK_NULL_HANDLE;
    std::vector<Allocation> allocations;

    std::size_t readOnlyBuffers = 0;
    bool drawBuild = false;
    std::vector<std::shared_ptr<Texture>> textures;
    std::vector<bool> textureFirstLayer;
    std::vector<std::shared_ptr<StorageTexture>> storageTextures;
    std::vector<std::uint32_t> storageMips;
    std::vector<bool> storageFirstLayer;
    std::vector<bool> storageWritten;
    std::vector<std::shared_ptr<Sampler>> samplers;
    bool reusable = false;
    std::vector<DirectRegion> directRegions;
    std::vector<ValidatedSurface> validatedTextures;

    std::uint64_t pendingSerialSeen = 0;

    HostImportsProof importsProof;

    std::uint64_t dataWordsHash = 14695981039346656037ull;
    void rehashDataWords();
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pushPatches;
    std::vector<DataPatch> dataPatches;
    BuildTiming timing;

    std::vector<Binding> bindings;
    struct DeferredImages {
        const ShaderRecompiler::DescriptorBinding* binding;
        std::size_t index;
    };
    std::vector<DeferredImages> deferredImages;
    std::uint64_t storageBuffers = 0;
    std::uint32_t plannedSampledImages = 0;
    std::uint32_t plannedStorageImages = 0;

    CompiledShader deferredCompute{};
    std::span<const GuestMemorySnapshot> deferredSnapshots;

    bool lockedBuild = false;

    bool unlockedPrepare = false;
    bool completed = false;
    std::chrono::steady_clock::time_point phaseStart;
};

class ResourceCache {
public:
    using Key = std::vector<std::uint32_t>;
    std::shared_ptr<ShaderResources> Find(const Key& key);

    void Insert(const Key& key, std::shared_ptr<ShaderResources> resources, std::vector<std::shared_ptr<ShaderResources>>* evicted = nullptr);

    void Remove(const Key& key, const ShaderResources* object = nullptr);

    bool Touch(const Key& key);

    static std::uint64_t Finds();
    static std::uint64_t Touches();
    void Clear();
    std::size_t Size() const;

private:
    void erase(const Key& key);
    void noteMiss(const Key& key);
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            std::uint64_t hash = 14695981039346656037ull;
            for (const auto word : key) hash = (hash ^ word) * 1099511628211ull;
            return static_cast<std::size_t>(hash);
        }
    };
    mutable std::mutex mutex;
    std::list<std::pair<Key, std::shared_ptr<ShaderResources>>> entries;
    std::unordered_map<Key, decltype(entries)::iterator, KeyHash> index;
};

ResourceCache& SharedResourceCache();

}

#endif
