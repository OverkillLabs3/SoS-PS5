#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/common/StderrLog.hpp"
#include "BdaAbi.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/PresentationOverlay.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/BdaFeatures.hpp"
#include "prx/libSceAgcDriver/Execution/include/PresentationScaler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GpuColorTransfer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/General.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include <atomic>
#include <chrono>
#include <charconv>
#include <condition_variable>
#include <fstream>
#include <cstdlib>
#include <mutex>
#include <SDL_loadso.h>
#include <SDL_error.h>
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <optional>
#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>
#include <list>
#include <stdexcept>
#include <string_view>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <utility>

namespace AgcDriver {
namespace {

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + ": Vulkan result " + std::to_string(result));
    }
}

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("Vulkan presentation: ") + reason);
}

using ResourceCache = Graphics::ResourceCache;

std::uint32_t DumpScale();
void WriteFrameBmp(int index, std::uint32_t fullWidth, std::uint32_t fullHeight, std::span<const std::byte> full, std::uint32_t scale);

class FrameDumpWriter {
public:
    ~FrameDumpWriter() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_one();
        if (worker.joinable()) worker.join();
    }

    void Enqueue(int index, std::uint32_t width, std::uint32_t height, std::uint32_t scale, std::vector<std::byte> pixels) {
        std::lock_guard lock(mutex);
        require(!stopping, "frame dump writer has stopped");
        require(pixels.size() <= maxBytes - queuedBytes, "frame dump queue exceeded 64 MiB");
        if (!worker.joinable()) worker = std::thread([this] { run(); });
        const auto bytes = pixels.size();
        pending.push_back({index, width, height, scale, std::move(pixels)});
        queuedBytes += bytes;
        changed.notify_one();
    }

private:
    struct Frame {
        int index;
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t scale;
        std::vector<std::byte> pixels;
    };

    void run() {
        try {
            for (;;) {
                std::deque<Frame> batch;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [this] { return stopping || !pending.empty(); });
                    if (pending.empty() && stopping) return;
                    batch.swap(pending);
                }
                for (const auto& frame : batch) {
                    WriteFrameBmp(frame.index, frame.width, frame.height, frame.pixels, frame.scale);
                    std::lock_guard lock(mutex);
                    queuedBytes -= frame.pixels.size();
                }
            }
        } catch (const std::exception& error) {
            aps5::LogErr( "[gpu] frame dump writer failed: %s\n", error.what());
            std::terminate();
        }
    }

    static constexpr std::size_t maxBytes = 64 * 1024 * 1024;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Frame> pending;
    std::thread worker;
    std::size_t queuedBytes = 0;
    bool stopping = false;
};

VulkanDevice::PresentStatistics presentCounters{};

}

struct ComputePipelineObjects {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkDestroyShaderModule destroyModule = nullptr;
    PFN_vkDestroyPipelineLayout destroyLayout = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    ~ComputePipelineObjects() {
        if (pipeline != VK_NULL_HANDLE) destroyPipeline(device, pipeline, nullptr);
        if (layout != VK_NULL_HANDLE) destroyLayout(device, layout, nullptr);
        if (module != VK_NULL_HANDLE) destroyModule(device, module, nullptr);
    }
};

namespace {

std::mutex deferredLoadersMutex;
std::vector<void*> deferredLoaders;

// Unloading takes the Windows loader lock, so a loader released under GpuMutex waits for ReleaseDeferredVulkanLoaders.
void ReleaseVulkanLoader(void* library) {
    if (!GuestMemory::GpuMutex().HeldByThisThread()) {
        SDL_UnloadObject(library);
        return;
    }
    std::lock_guard lock(deferredLoadersMutex);
    deferredLoaders.push_back(library);
}

struct VulkanLoaderLibrary {
    void* handle = nullptr;

    VulkanLoaderLibrary() = default;
    VulkanLoaderLibrary(const VulkanLoaderLibrary&) = delete;
    VulkanLoaderLibrary& operator=(const VulkanLoaderLibrary&) = delete;
    ~VulkanLoaderLibrary() {
        if (handle != nullptr) ReleaseVulkanLoader(handle);
    }
};

}

void ReleaseDeferredVulkanLoaders() {
    std::vector<void*> pending;
    {
        std::lock_guard lock(deferredLoadersMutex);
        pending.swap(deferredLoaders);
    }
    for (void* library : pending) ReleaseVulkanLoader(library);
}

struct VulkanDevice::State {
    // First member, so the loader outlives every other member that may still call through it.
    VulkanLoaderLibrary library;
    PFN_vkGetInstanceProcAddr instanceProc = nullptr;
    PFN_vkGetDeviceProcAddr deviceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    void* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    VkFence acquireFence = VK_NULL_HANDLE;
    struct RetiredSwapchain {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        std::vector<VkSemaphore> rendered;
    };
    std::vector<VkSemaphore> rendered;
    std::vector<RetiredSwapchain> retiredSwapchains;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkBuffer uploadBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uploadMemory = VK_NULL_HANDLE;
    void* uploadMapping = nullptr;
    VkDeviceSize uploadSize = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    std::vector<std::uint32_t> capabilities{1};
    std::vector<std::string_view> spirvExtensions;
    bool tessellationShader = false;
    bool meshShader = false;
    bool fragmentShaderBarycentric = false;

    bool descriptorIndexing = false;
    bool primitiveListRestart = false;
    bool depthClipControl = false;
    bool imageViewMinLod = false;
    bool maintenance8 = false;
    bool depthClamp = false;
    bool depthBounds = false;
    bool depthBiasClamp = false;
    bool occlusionQueryPrecise = false;
    VkDeviceSize hostImportAlignment = 0;
    bool depthRangeUnrestricted = false;
    bool samplerAnisotropy = false;
    bool textureCompressionBC = false;

    bool timelineSemaphores = false;
    bool computeWave32 = false;

    bool drawIndirectFirstInstance = false;
    bool multiDrawIndirect = false;
    bool drawIndirectCount = false;

    Graphics::DeviceFunctions deviceFunctions;
    bool functionsReady = false;
    Graphics::Context context;
    bool contextReady = false;
    std::unique_ptr<Graphics::TextureDetiler> detiler;
    std::unique_ptr<Graphics::Recorder> recorder;

    std::vector<std::pair<std::array<std::uint32_t, 4>, std::shared_ptr<Graphics::DeviceBuffer>>> patternBuffers;

    std::mutex computePipelinesMutex;
    std::map<std::uint64_t, std::shared_ptr<ComputePipelineObjects>> computePipelines;
    std::unique_ptr<Graphics::GpuColorTransfer> colorTransfer;
    std::shared_ptr<Graphics::BufferPool> bufferPool;
    std::unique_ptr<Graphics::Buffer> emptyBuffer;
    std::unique_ptr<Graphics::TextureCache> textureCache;
    std::unique_ptr<Graphics::PipelineCache> pipelineCache;
    std::unique_ptr<Graphics::DescriptorCache> descriptorCache;
    std::unique_ptr<Graphics::SamplerCache> samplerCache;
    Graphics::ResourceCache& resourceCache = Graphics::SharedResourceCache();

    std::shared_ptr<std::vector<std::shared_ptr<Graphics::ShaderResources>>> copiedWriters = std::make_shared<std::vector<std::shared_ptr<Graphics::ShaderResources>>>();
    VkPhysicalDeviceMeshShaderPropertiesEXT meshLimits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    std::unique_ptr<PresentationScaler> scaler;
    PresentationOverlay* overlay = nullptr;
    bool overlayUsable = false;
    std::uint32_t queueFamily = 0;
    std::uint64_t swapchainSerial = 0;

    bool imageAcquired = false;
    std::uint32_t acquiredIndex = 0;

    struct PresentSlot {
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        std::shared_ptr<void> kept;
        std::unique_ptr<Graphics::Buffer> dumpBuffer;
        std::unique_ptr<PresentationScaler> dumpScaler;
        std::uint32_t dumpWidth = 0;
        std::uint32_t dumpHeight = 0;
        std::uint32_t dumpScale = 1;
        bool dumpRecorded = false;
        int dumpIndex = 0;
        std::uint32_t imageIndex = 0;
        bool inFlight = false;

        bool shared = false;

        std::uint64_t recorderSerial = 0;
        std::uint64_t previousSerial = 0;
        std::chrono::steady_clock::time_point submittedAt{};

        VkQueryPool queries = VK_NULL_HANDLE;
        std::uint64_t blitBytes = 0;
    };
    std::vector<PresentSlot> presentSlots;
    std::size_t presentCursor = 0;
    std::uint64_t lastPresentSerial = 0;

    struct RetiredRange {
        std::uint64_t afterSerial;
        std::uint64_t throughSerial;
    };
    std::deque<RetiredRange> retiredRanges;
    std::mutex presentMutex;

    bool queuePending = false;
    std::uint32_t queueIndex = 0;
    int nextDumpIndex = 0;
    FrameDumpWriter dumpWriter;

    template<typename TFunction>
    TFunction InstanceFunction(const char* name) const {
        auto function = reinterpret_cast<TFunction>(instanceProc(instance, name));
        if (function == nullptr) {
            throw std::runtime_error(std::string("Vulkan instance function missing: ") + name);
        }
        return function;
    }

    template<typename TFunction>
    TFunction DeviceFunction(const char* name) const {
        ++Graphics::DeviceProcLookups();
        auto function = reinterpret_cast<TFunction>(deviceProc(device, name));
        if (function == nullptr) {
            throw std::runtime_error(std::string("Vulkan device function missing: ") + name);
        }
        return function;
    }

    void Upload(std::span<const std::byte> pixels) {
        APS5_LOG_OUT("Upload pixels=%zu uploadSize=%llu buffer=%p memory=%p mapping=%p", pixels.size(), static_cast<unsigned long long>(uploadSize), reinterpret_cast<void*>(uploadBuffer), reinterpret_cast<void*>(uploadMemory), uploadMapping);
        if (uploadSize < pixels.size()) {
            APS5_LOG_OUT("Upload reallocating oldSize=%llu newSize=%zu", static_cast<unsigned long long>(uploadSize), pixels.size());
            if (uploadMapping) DeviceFunction<PFN_vkUnmapMemory>("vkUnmapMemory")(device, uploadMemory);
            uploadMapping = nullptr;
            if (uploadBuffer) DeviceFunction<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, uploadBuffer, nullptr);
            uploadBuffer = VK_NULL_HANDLE;
            if (uploadMemory) DeviceFunction<PFN_vkFreeMemory>("vkFreeMemory")(device, uploadMemory, nullptr);
            uploadMemory = VK_NULL_HANDLE;
            uploadSize = 0;
            VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer.size = pixels.size();
            buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(DeviceFunction<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &buffer, nullptr, &uploadBuffer), "vkCreateBuffer display upload");
            VkMemoryRequirements requirements{};
            DeviceFunction<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, uploadBuffer, &requirements);
            std::uint32_t memoryType = memoryProperties.memoryTypeCount;
            const auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
                if ((requirements.memoryTypeBits & (1u << i)) != 0 && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags) {
                    memoryType = i;
                    break;
                }
            }
            require(memoryType < memoryProperties.memoryTypeCount, "coherent host upload memory is unavailable");
            APS5_LOG_OUT("Upload requirements size=%llu alignment=%llu typeBits=0x%x memoryType=%u", static_cast<unsigned long long>(requirements.size), static_cast<unsigned long long>(requirements.alignment), requirements.memoryTypeBits, memoryType);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memoryType;
            check(DeviceFunction<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &allocation, nullptr, &uploadMemory), "vkAllocateMemory display upload");
            check(DeviceFunction<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, uploadBuffer, uploadMemory, 0), "vkBindBufferMemory display upload");
            check(DeviceFunction<PFN_vkMapMemory>("vkMapMemory")(device, uploadMemory, 0, pixels.size(), 0, &uploadMapping), "vkMapMemory display upload");
            uploadSize = pixels.size();
            APS5_LOG_OUT("Upload allocation complete buffer=%p memory=%p mapping=%p size=%llu", reinterpret_cast<void*>(uploadBuffer), reinterpret_cast<void*>(uploadMemory), uploadMapping, static_cast<unsigned long long>(uploadSize));
        }
        std::memcpy(uploadMapping, pixels.data(), pixels.size());
        APS5_LOG_OUT("Upload memcpy complete bytes=%zu", pixels.size());
    }

    bool SharedSlotInFlight() {
        std::lock_guard lock(presentMutex);
        return std::any_of(presentSlots.begin(), presentSlots.end(), [](const auto& slot) { return slot.inFlight && slot.shared; });
    }

    bool CopiedWriterOverlaps(std::uint64_t address, std::size_t bytes) const {
        const auto overlaps = [&](const auto& writer) { return writer->WritesOverlap(address, bytes); };
        return std::any_of(copiedWriters->begin(), copiedWriters->end(), overlaps) || std::any_of(Graphics::DrawCopiedWriters()->begin(), Graphics::DrawCopiedWriters()->end(), overlaps);
    }

    void DestroyRetiredSwapchains() {
        if (retiredSwapchains.empty()) return;
        const auto destroySemaphore = DeviceFunction<PFN_vkDestroySemaphore>("vkDestroySemaphore");
        const auto destroySwapchain = DeviceFunction<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR");
        for (const auto& retired : retiredSwapchains) {
            for (auto semaphore : retired.rendered) {
                if (semaphore) destroySemaphore(device, semaphore, nullptr);
            }
            destroySwapchain(device, retired.swapchain, nullptr);
        }
        retiredSwapchains.clear();
    }

    void RetireSlot(PresentSlot& slot) {
        slot.inFlight = false;
        slot.kept.reset();
        if (slot.dumpRecorded) {
            slot.dumpRecorded = false;
            WriteFrameDump(slot);
        }
        if (slot.queries != VK_NULL_HANDLE) {
            std::uint64_t stamps[2] = {};
            if (DeviceFunction<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(device, slot.queries, 0, 2, sizeof(stamps), stamps, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && stamps[1] > stamps[0]) {
                Graphics::Recorder::AddGpuTiming(Graphics::Recorder::CommandClass::PresentBlit, static_cast<double>(stamps[1] - stamps[0]) * properties.limits.timestampPeriod, slot.blitBytes);
            }
        }
        if (!recorder) return;
        const auto afterFlip = recorder->NewestSubmitted() - slot.recorderSerial;
        presentCounters.batchesAfterFlip += afterFlip;
        if (auto* frame = PerformanceContext::Current()) frame->NoteRetired(afterFlip, 0, 0);
        retiredRanges.push_back({slot.previousSerial, slot.recorderSerial});
        SettleRetired(false);
    }

    void SettleRetired(bool force) {
        const auto newest = recorder->NewestSubmitted();
        while (!retiredRanges.empty()) {
            const auto range = retiredRanges.front();
            std::size_t missing = 0;
            const auto batches = recorder->CompletedBatches(range.afterSerial, range.throughSerial, missing);
            if (missing != 0 && !force && newest < range.afterSerial + Graphics::Recorder::CompletedRingSize / 2) break;
            retiredRanges.pop_front();
            double busyMs = 0, firstNs = 0, lastNs = 0;
            bool stamped = false;
            std::uint64_t checked = 0, overwritten = 0;
            for (const auto& batch : batches) {
                if (batch.gpuEndNs > batch.gpuStartNs) {
                    busyMs += (batch.gpuEndNs - batch.gpuStartNs) / 1e6;
                    firstNs = stamped ? std::min(firstNs, batch.gpuStartNs) : batch.gpuStartNs;
                    lastNs = stamped ? std::max(lastNs, batch.gpuEndNs) : batch.gpuEndNs;
                    stamped = true;
                }
                if (batch.readGeneration == 0) continue;
                for (const auto& [begin, end] : batch.reads) {
                    ++checked;
                    GuestMemory::CollectWritesUncached(begin, end - begin);
                    if (!GuestMemory::UnchangedSince(begin, end - begin, batch.readGeneration)) ++overwritten;
                }
            }
            const double gapMs = stamped ? (lastNs - firstNs) / 1e6 - busyMs : 0;
            if (auto* frame = PerformanceContext::Current()) frame->NoteRetired(0, busyMs, gapMs);
            presentCounters.gpuBusyMs += busyMs;
            presentCounters.gpuGapMs += gapMs;
            presentCounters.gpuUnread += missing;
            presentCounters.readsChecked += checked;
            presentCounters.readsOverwritten += overwritten;
        }
    }

    void WriteFrameDump(const PresentSlot& slot) {
        require(slot.dumpBuffer != nullptr, "frame dump was not recorded");
        std::vector<std::byte> full(slot.dumpBuffer->Bytes().begin(), slot.dumpBuffer->Bytes().end());
        const auto index = slot.dumpIndex;
        const auto width = slot.dumpWidth;
        const auto height = slot.dumpHeight;
        static const auto start = std::chrono::steady_clock::now();
        aps5::LogErr( "[gpu] frame_%03d.bmp: %ux%u display read back on the GPU at %.1f s\n", index, width, height, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        dumpWriter.Enqueue(index, width, height, slot.dumpScale, std::move(full));
    }

    void WaitPresentFence(VkFence fence) {
        static const bool poll = std::getenv("APS5_PRESENT_POLL_FENCE") != nullptr;
        if (!poll) {
            check(DeviceFunction<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()), "vkWaitForFences present");
            return;
        }
        const auto status = DeviceFunction<PFN_vkGetFenceStatus>("vkGetFenceStatus");
        for (;;) {
            const auto result = status(device, fence);
            if (result == VK_SUCCESS) return;
            if (result != VK_NOT_READY) check(result, "vkGetFenceStatus present");
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
            while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
        }
    }

    ~State() {
        contextReady = false;
        if (device != VK_NULL_HANDLE) {
            const auto idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(deviceProc(device, "vkDeviceWaitIdle"))(device);
            if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST) std::terminate();

            {
                std::lock_guard lock(presentMutex);
                for (auto& slot : presentSlots) {
                    if (slot.inFlight) RetireSlot(slot);
                }
                if (recorder) SettleRetired(true);
            }
            for (auto& slot : presentSlots) {
                slot.dumpBuffer.reset();
                slot.dumpScaler.reset();
            }

            if (recorder) {
                try {
                    Graphics::PublishAllShadows(context, Graphics::PublishReason::Teardown);
                    recorder->Sync();
                } catch (const std::exception& error) {
                    aps5::LogErr( "[gpu] unit shadow teardown: %s\n", error.what());
                }
            }
            Graphics::DestroyShadows(device);
            recorder.reset();

            Graphics::ClearCachedPipelines(device);
            Graphics::ClearDepthSurfaces(device);
            {
                std::lock_guard pipelines(computePipelinesMutex);
                computePipelines.clear();
            }

            copiedWriters->clear();
            Graphics::DrawCopiedWriters()->clear();
            resourceCache.Clear();
            Graphics::ClearCachedTextures(device);
            Graphics::ClearImageMirrors(device);
            Graphics::ClearHostImports(device);
            patternBuffers.clear();
            descriptorCache.reset();
            emptyBuffer.reset();
            samplerCache.reset();
            textureCache.reset();
            detiler.reset();
            colorTransfer.reset();
            scaler.reset();
            pipelineCache.reset();
            context.bufferPool.reset();
            bufferPool.reset();
            const auto destroyFence = reinterpret_cast<PFN_vkDestroyFence>(deviceProc(device, "vkDestroyFence"));
            if (acquireFence) destroyFence(device, acquireFence, nullptr);
            const auto destroyQueryPool = reinterpret_cast<PFN_vkDestroyQueryPool>(deviceProc(device, "vkDestroyQueryPool"));
            for (const auto& slot : presentSlots) {
                if (slot.fence) destroyFence(device, slot.fence, nullptr);
                if (slot.queries) destroyQueryPool(device, slot.queries, nullptr);
            }
            const auto destroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(deviceProc(device, "vkDestroySemaphore"));
            for (auto semaphore : rendered) {
                if (semaphore) destroySemaphore(device, semaphore, nullptr);
            }
            DestroyRetiredSwapchains();
            if (uploadMapping) reinterpret_cast<PFN_vkUnmapMemory>(deviceProc(device, "vkUnmapMemory"))(device, uploadMemory);
            if (uploadBuffer) reinterpret_cast<PFN_vkDestroyBuffer>(deviceProc(device, "vkDestroyBuffer"))(device, uploadBuffer, nullptr);
            if (uploadMemory) reinterpret_cast<PFN_vkFreeMemory>(deviceProc(device, "vkFreeMemory"))(device, uploadMemory, nullptr);
            if (swapchain) reinterpret_cast<PFN_vkDestroySwapchainKHR>(deviceProc(device, "vkDestroySwapchainKHR"))(device, swapchain, nullptr);
            const auto destroyPool = reinterpret_cast<PFN_vkDestroyCommandPool>(deviceProc(device, "vkDestroyCommandPool"));
            const auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(deviceProc(device, "vkDestroyDevice"));
            if (pool != VK_NULL_HANDLE) {
                destroyPool(device, pool, nullptr);
            }
            destroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            if (surface) reinterpret_cast<PFN_vkDestroySurfaceKHR>(instanceProc(instance, "vkDestroySurfaceKHR"))(instance, surface, nullptr);
            reinterpret_cast<PFN_vkDestroyInstance>(instanceProc(instance, "vkDestroyInstance"))(instance, nullptr);
        }
    }
};

VulkanDevice::VulkanDevice(const PresentationWindow* window) : state(std::make_unique<State>()), serial([] { static std::atomic<std::uint64_t> serials{0}; return serials.fetch_add(1, std::memory_order_relaxed) + 1; }()) {
    APS5_LOG_OUT("VulkanDevice constructor window=%p", static_cast<const void*>(window));
#ifdef _WIN32
    state->library.handle = SDL_LoadObject("vulkan-1.dll");
#else
    state->library.handle = SDL_LoadObject("libvulkan.so.1");
#endif
    if (state->library.handle == nullptr) {
        throw std::runtime_error(std::string("Vulkan loader: ") + SDL_GetError());
    }
    APS5_LOG_OUT("Vulkan loader loaded library=%p", state->library.handle);
    state->instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(state->library.handle, "vkGetInstanceProcAddr"));
    if (state->instanceProc == nullptr) {
        throw std::runtime_error("Vulkan loader: vkGetInstanceProcAddr missing");
    }
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "AnyPS5 libSceAgcDriver";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &application;
    std::vector<const char*> instanceExtensions;
    if (window != nullptr) {
        APS5_LOG_OUT("Presentation window context=%p extent=%ux%u extensions=%zu", window->context, window->width, window->height, window->extensions.size());
        require(window->context && window->createSurface && window->getDrawableSize && window->width && window->height, "invalid window descriptor");
        instanceExtensions.assign(window->extensions.begin(), window->extensions.end());
        std::uint32_t availableCount = 0;
        const auto enumerateExtensions = state->InstanceFunction<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties");
        check(enumerateExtensions(nullptr, &availableCount, nullptr), "vkEnumerateInstanceExtensionProperties");
        std::vector<VkExtensionProperties> available(availableCount);
        check(enumerateExtensions(nullptr, &availableCount, available.data()), "vkEnumerateInstanceExtensionProperties");
        for (const auto* name : instanceExtensions) {
            require(name != nullptr, "null instance extension");
            if (std::none_of(available.begin(), available.end(), [&](const auto& item) { return std::strcmp(item.extensionName, name) == 0; })) {
                throw std::runtime_error(std::string("Vulkan presentation: required instance extension missing: ") + name);
            }
        }
        create.enabledExtensionCount = static_cast<std::uint32_t>(instanceExtensions.size());
        create.ppEnabledExtensionNames = instanceExtensions.data();
    }
    check(state->InstanceFunction<PFN_vkCreateInstance>("vkCreateInstance")(&create, nullptr, &state->instance), "vkCreateInstance");
    APS5_LOG_OUT("Vulkan instance created instance=%p", reinterpret_cast<void*>(state->instance));
    if (window != nullptr) {
        state->surface = window->createSurface(window->context, state->instance);
        require(state->surface != VK_NULL_HANDLE, "window returned a null surface");
        state->window = window->context;
        APS5_LOG_OUT("Vulkan surface created surface=%p window=%p", reinterpret_cast<void*>(state->surface), state->window);
    }
    state->deviceProc = state->InstanceFunction<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    const auto enumerate = state->InstanceFunction<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    std::uint32_t count = 0;
    check(enumerate(state->instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    APS5_LOG_OUT("Physical device count=%u", count);
    std::vector<VkPhysicalDevice> devices(count);
    check(enumerate(state->instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
    devices.resize(count);
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    int selectedRank = -1;
    const auto rankDeviceType = [](VkPhysicalDeviceType type) {
        switch (type) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 1;
            default: return 0;
        }
    };
    const std::array<const char*, 1> presentationExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    for (auto physical : devices) {
        VkPhysicalDeviceProperties properties{};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical, &properties);
        APS5_LOG_OUT("Physical device candidate=%p name=%s api=0x%x type=%d", reinterpret_cast<void*>(physical), properties.deviceName, properties.apiVersion, static_cast<int>(properties.deviceType));
        if (properties.apiVersion < VK_API_VERSION_1_1) {
            continue;
        }
        const int rank = rankDeviceType(properties.deviceType);
        if (rank <= selectedRank) {
            continue;
        }
        if (window != nullptr) {
            std::uint32_t extensionCount = 0;
            auto enumerateExtensions = state->InstanceFunction<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
            check(enumerateExtensions(physical, nullptr, &extensionCount, nullptr), "vkEnumerateDeviceExtensionProperties");
            std::vector<VkExtensionProperties> extensions(extensionCount);
            check(enumerateExtensions(physical, nullptr, &extensionCount, extensions.data()), "vkEnumerateDeviceExtensionProperties");
            const bool supported = std::all_of(presentationExtensions.begin(), presentationExtensions.end(), [&](const char* name) {
                return std::any_of(extensions.begin(), extensions.end(), [&](const auto& item) { return std::strcmp(item.extensionName, name) == 0; });
            });
            if (!supported) continue;
        }
        std::uint32_t families = 0;
        auto getFamilies = state->InstanceFunction<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        getFamilies(physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        getFamilies(physical, &families, queues.data());
        for (std::uint32_t i = 0; i < families; ++i) {
            if (queues[i].queueCount != 0 && (queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                if (window != nullptr) {
                    VkBool32 supported = VK_FALSE;
                    check(state->InstanceFunction<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>("vkGetPhysicalDeviceSurfaceSupportKHR")(physical, i, state->surface, &supported), "vkGetPhysicalDeviceSurfaceSupportKHR");
                    if (!supported) continue;
                }
                selected = physical;
                family = i;
                selectedRank = rank;
                break;
            }
        }
    }
    if (selected == VK_NULL_HANDLE) {
        throw std::runtime_error(window ? "Vulkan: no Vulkan 1.1 device with graphics, compute and swapchain presentation" : "Vulkan: no Vulkan 1.1 graphics and compute queue");
    }
    APS5_LOG_OUT("Physical device selected physical=%p queueFamily=%u", reinterpret_cast<void*>(selected), family);
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &state->subgroup;
    state->InstanceFunction<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(selected, &properties);
    state->properties = properties.properties;
    state->physical = selected;
    if ((state->subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0) {
        state->capabilities.push_back(spv::CapabilityGroupNonUniform);
        if ((state->subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0) state->capabilities.push_back(spv::CapabilityGroupNonUniformBallot);
        if ((state->subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0) state->capabilities.push_back(spv::CapabilityGroupNonUniformShuffle);
    }
    APS5_LOG_OUT("Selected GPU name=%s vendor=0x%x device=0x%x subgroup=%u", state->properties.deviceName, state->properties.vendorID, state->properties.deviceID, state->subgroup.subgroupSize);
    state->InstanceFunction<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(selected, &state->memoryProperties);
    for (std::uint32_t i = 0; i < state->memoryProperties.memoryHeapCount; ++i) APS5_LOG_OUT("Memory heap %u size=%.1f MiB flags=0x%x", i, state->memoryProperties.memoryHeaps[i].size / 1048576.0, state->memoryProperties.memoryHeaps[i].flags);
    for (std::uint32_t i = 0; i < state->memoryProperties.memoryTypeCount; ++i) APS5_LOG_OUT("Memory type %u heap=%u flags=0x%x", i, state->memoryProperties.memoryTypes[i].heapIndex, state->memoryProperties.memoryTypes[i].propertyFlags);
    std::uint32_t extensionCount = 0;
    const auto enumerateDeviceExtensions = state->InstanceFunction<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    check(enumerateDeviceExtensions(selected, nullptr, &extensionCount, nullptr), "vkEnumerateDeviceExtensionProperties");
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    check(enumerateDeviceExtensions(selected, nullptr, &extensionCount, availableExtensions.data()), "vkEnumerateDeviceExtensionProperties");
    const auto hasExtension = [&](const char* name) { return std::any_of(availableExtensions.begin(), availableExtensions.end(), [&](const auto& item) { return std::strcmp(item.extensionName, name) == 0; }); };
    auto byteFeatures = QueryBdaByteFeatures(selected, state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2"), availableExtensions);
    auto bdaFeatures = QueryBdaFeatures(selected, state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2"), availableExtensions);
    require(hasExtension(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME), "VK_KHR_shader_float_controls is unavailable");
    VkPhysicalDeviceFloatControlsProperties floatControls{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceProperties2 floatProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &floatControls};
    state->InstanceFunction<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(selected, &floatProperties);
    require(floatControls.shaderSignedZeroInfNanPreserveFloat32 == VK_TRUE, "shaderSignedZeroInfNanPreserveFloat32 is unavailable");
    const std::array<const char*, 2> meshExtensions{VK_EXT_MESH_SHADER_EXTENSION_NAME, VK_KHR_SPIRV_1_4_EXTENSION_NAME};
    const bool meshAvailable = std::all_of(meshExtensions.begin(), meshExtensions.end(), hasExtension);
    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    if (meshAvailable) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &meshFeatures};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        state->meshShader = meshFeatures.meshShader == VK_TRUE;
        VkPhysicalDeviceProperties2 meshProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &state->meshLimits};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(selected, &meshProperties);
    }
    meshFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    meshFeatures.meshShader = state->meshShader;
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentricFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    if (hasExtension(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &barycentricFeatures};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        state->fragmentShaderBarycentric = barycentricFeatures.fragmentShaderBarycentric == VK_TRUE;
    }
    std::vector<const char*> deviceExtensions;
    if (window != nullptr) deviceExtensions.assign(presentationExtensions.begin(), presentationExtensions.end());
    if (state->fragmentShaderBarycentric) {
        deviceExtensions.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
        state->capabilities.push_back(spv::CapabilityFragmentBarycentricKHR);
        state->spirvExtensions.push_back("SPV_KHR_fragment_shader_barycentric");
    }
    deviceExtensions.push_back(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
    state->capabilities.push_back(spv::CapabilitySignedZeroInfNanPreserve);
    state->spirvExtensions.push_back("SPV_KHR_float_controls");
    deviceExtensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    deviceExtensions.push_back(VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
    state->capabilities.push_back(4448);
    state->spirvExtensions.push_back("SPV_KHR_8bit_storage");
    state->capabilities.push_back(11);
    state->capabilities.push_back(5347);
    state->spirvExtensions.push_back("SPV_KHR_physical_storage_buffer");
    state->depthRangeUnrestricted = hasExtension(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
    if (state->depthRangeUnrestricted) deviceExtensions.push_back(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);

    const bool imageRobustness = hasExtension(VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME);
    if (imageRobustness) deviceExtensions.push_back(VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME);

    state->drawIndirectCount = hasExtension(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
    if (state->drawIndirectCount) deviceExtensions.push_back(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);

    if (hasExtension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) && std::getenv("APS5_HOST_IMPORT") != nullptr && std::getenv("APS5_NO_HOST_IMPORT") == nullptr) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hostProperties};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(selected, &properties);
        state->hostImportAlignment = hostProperties.minImportedHostPointerAlignment;
#ifdef _WIN32

        if (const char* value = std::getenv("APS5_WORKING_SET_MIB")) {
            const SIZE_T bytes = static_cast<SIZE_T>(std::strtoull(value, nullptr, 10)) << 20u;
            if (!SetProcessWorkingSetSizeEx(GetCurrentProcess(), bytes, bytes * 2, QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE)) aps5::LogErr( "[gpu] SetProcessWorkingSetSizeEx failed: %lu\n", GetLastError());
        }
#endif
        deviceExtensions.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    }
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT listRestartFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
    if (hasExtension(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &listRestartFeatures};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        state->primitiveListRestart = listRestartFeatures.primitiveTopologyListRestart == VK_TRUE;
        if (state->primitiveListRestart) deviceExtensions.push_back(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
    }
    listRestartFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
    listRestartFeatures.primitiveTopologyListRestart = state->primitiveListRestart ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceImageViewMinLodFeaturesEXT minLodFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT};
    if (hasExtension(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &minLodFeatures};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        state->imageViewMinLod = minLodFeatures.minLod == VK_TRUE;
        if (state->imageViewMinLod) deviceExtensions.push_back(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
    }
    minLodFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT};
    minLodFeatures.minLod = VK_TRUE;
    VkPhysicalDeviceMaintenance8FeaturesKHR maintenance8Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR};
    if (hasExtension(VK_KHR_MAINTENANCE_8_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &maintenance8Features};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        state->maintenance8 = maintenance8Features.maintenance8 == VK_TRUE;
        if (state->maintenance8) deviceExtensions.push_back(VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
    }
    maintenance8Features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR};
    maintenance8Features.maintenance8 = VK_TRUE;
    VkPhysicalDeviceDepthClipControlFeaturesEXT depthClipFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    if (hasExtension(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &depthClipFeatures};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        state->depthClipControl = depthClipFeatures.depthClipControl == VK_TRUE;
        if (state->depthClipControl) deviceExtensions.push_back(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    }
    if (state->meshShader) {
        deviceExtensions.insert(deviceExtensions.end(), meshExtensions.begin(), meshExtensions.end());
        state->capabilities.push_back(5283);
        state->spirvExtensions.push_back("SPV_EXT_mesh_shader");
    }
    APS5_LOG_OUT("Vulkan features tessellationAvailable=%u mesh=%u depthClip=%u depthRangeUnrestricted=%u", static_cast<unsigned>(state->tessellationShader), static_cast<unsigned>(state->meshShader), static_cast<unsigned>(state->depthClipControl), static_cast<unsigned>(state->depthRangeUnrestricted));
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    state->queueFamily = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    VkPhysicalDeviceFeatures available{};
    state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(selected, &available);
    require(available.vertexPipelineStoresAndAtomics && available.fragmentStoresAndAtomics, "graphics shader buffer writes and atomics are unavailable");
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderInt64 = VK_TRUE;
    enabled.vertexPipelineStoresAndAtomics = VK_TRUE;
    enabled.fragmentStoresAndAtomics = VK_TRUE;
    enabled.tessellationShader = available.tessellationShader;
    state->tessellationShader = enabled.tessellationShader == VK_TRUE;
    if (state->tessellationShader) state->capabilities.push_back(3);
    require(available.samplerAnisotropy && available.textureCompressionBC, "device lacks sampler anisotropy or BC texture compression support required for texture sampling");
    enabled.samplerAnisotropy = VK_TRUE;
    enabled.textureCompressionBC = VK_TRUE;

    enabled.robustBufferAccess = available.robustBufferAccess;

    enabled.drawIndirectFirstInstance = available.drawIndirectFirstInstance;
    enabled.multiDrawIndirect = available.multiDrawIndirect;
    state->drawIndirectFirstInstance = enabled.drawIndirectFirstInstance == VK_TRUE;
    state->multiDrawIndirect = enabled.multiDrawIndirect == VK_TRUE;

    enabled.depthClamp = available.depthClamp;
    state->depthClamp = enabled.depthClamp == VK_TRUE;
    enabled.depthBounds = available.depthBounds;
    state->depthBounds = enabled.depthBounds == VK_TRUE;
    enabled.depthBiasClamp = available.depthBiasClamp;
    state->depthBiasClamp = enabled.depthBiasClamp == VK_TRUE;
    enabled.occlusionQueryPrecise = available.occlusionQueryPrecise;
    state->occlusionQueryPrecise = enabled.occlusionQueryPrecise == VK_TRUE;

    enabled.shaderStorageImageWriteWithoutFormat = available.shaderStorageImageWriteWithoutFormat;
    enabled.shaderStorageImageReadWithoutFormat = available.shaderStorageImageReadWithoutFormat;

    enabled.shaderImageGatherExtended = available.shaderImageGatherExtended;
    if (enabled.shaderImageGatherExtended) state->capabilities.push_back(spv::CapabilityImageGatherExtended);
    enabled.shaderResourceMinLod = available.shaderResourceMinLod;
    if (enabled.shaderResourceMinLod) state->capabilities.push_back(spv::CapabilityMinLod);
    enabled.sampleRateShading = available.sampleRateShading;
    enabled.geometryShader = available.geometryShader;
    if (enabled.geometryShader) state->capabilities.push_back(spv::CapabilityGeometry);
    enabled.shaderClipDistance = available.shaderClipDistance;
    if (enabled.shaderStorageImageWriteWithoutFormat) state->capabilities.push_back(spv::CapabilityStorageImageWriteWithoutFormat);
    if (enabled.shaderStorageImageReadWithoutFormat) state->capabilities.push_back(spv::CapabilityStorageImageReadWithoutFormat);

    enabled.shaderSampledImageArrayDynamicIndexing = available.shaderSampledImageArrayDynamicIndexing;
    enabled.shaderStorageImageArrayDynamicIndexing = available.shaderStorageImageArrayDynamicIndexing;
    if (enabled.shaderSampledImageArrayDynamicIndexing) state->capabilities.push_back(spv::CapabilitySampledImageArrayDynamicIndexing);
    if (enabled.shaderStorageImageArrayDynamicIndexing) state->capabilities.push_back(spv::CapabilityStorageImageArrayDynamicIndexing);
    VkPhysicalDeviceDescriptorIndexingFeaturesEXT descriptorIndexingFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT};
    if (hasExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &descriptorIndexingFeatures};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        state->descriptorIndexing = descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing == VK_TRUE && descriptorIndexingFeatures.shaderStorageImageArrayNonUniformIndexing == VK_TRUE;
    }
    descriptorIndexingFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT};
    if (state->descriptorIndexing) {
        descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        descriptorIndexingFeatures.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
        deviceExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        state->capabilities.push_back(spv::CapabilityShaderNonUniform);
        state->capabilities.push_back(spv::CapabilitySampledImageArrayNonUniformIndexing);
        state->capabilities.push_back(spv::CapabilityStorageImageArrayNonUniformIndexing);
        state->spirvExtensions.push_back("SPV_EXT_descriptor_indexing");
    }
    state->samplerAnisotropy = true;
    state->textureCompressionBC = true;
    deviceInfo.pEnabledFeatures = &enabled;
    deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    if (state->meshShader) {
        meshFeatures.pNext = const_cast<void*>(deviceInfo.pNext);
        deviceInfo.pNext = &meshFeatures;
    }
    if (state->depthClipControl) {
        depthClipFeatures.pNext = const_cast<void*>(deviceInfo.pNext);
        deviceInfo.pNext = &depthClipFeatures;
    }
    if (state->primitiveListRestart) {
        listRestartFeatures.pNext = const_cast<void*>(deviceInfo.pNext);
        deviceInfo.pNext = &listRestartFeatures;
    }
    if (state->imageViewMinLod) {
        minLodFeatures.pNext = const_cast<void*>(deviceInfo.pNext);
        deviceInfo.pNext = &minLodFeatures;
    }
    if (state->maintenance8) {
        maintenance8Features.pNext = const_cast<void*>(deviceInfo.pNext);
        deviceInfo.pNext = &maintenance8Features;
    }
    byteFeatures.pNext = const_cast<void*>(deviceInfo.pNext);
    if (state->fragmentShaderBarycentric) {
        barycentricFeatures.pNext = byteFeatures.pNext;
        byteFeatures.pNext = &barycentricFeatures;
    }
    VkPhysicalDeviceImageRobustnessFeaturesEXT imageRobustnessFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT, nullptr, VK_TRUE};
    if (imageRobustness) {
        imageRobustnessFeatures.pNext = byteFeatures.pNext;
        byteFeatures.pNext = &imageRobustnessFeatures;
    }
    if (state->descriptorIndexing) {
        descriptorIndexingFeatures.pNext = byteFeatures.pNext;
        byteFeatures.pNext = &descriptorIndexingFeatures;
    }

    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timelineFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR};
    if (hasExtension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) && std::getenv("APS5_NO_TIMELINE") == nullptr) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timelineFeatures};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        state->timelineSemaphores = timelineFeatures.timelineSemaphore == VK_TRUE;
    }
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT subgroupSizeFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT};
    if (hasExtension(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME) && state->subgroup.subgroupSize > 32u) {
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &subgroupSizeFeatures};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(selected, &features);
        VkPhysicalDeviceSubgroupSizeControlPropertiesEXT subgroupSize{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 sizeProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroupSize};
        state->InstanceFunction<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(selected, &sizeProperties);
        state->computeWave32 = subgroupSizeFeatures.subgroupSizeControl == VK_TRUE && subgroupSize.minSubgroupSize <= 32u && subgroupSize.maxSubgroupSize >= 32u &&
            (subgroupSize.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
    }
    if (state->computeWave32) {
        subgroupSizeFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT};
        subgroupSizeFeatures.subgroupSizeControl = VK_TRUE;
        deviceExtensions.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
        deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
        subgroupSizeFeatures.pNext = byteFeatures.pNext;
        byteFeatures.pNext = &subgroupSizeFeatures;
        APS5_LOG_OUT("Compute wave32 programs run on subgroups of %u", 32u);
    }
    timelineFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR};
    timelineFeatures.timelineSemaphore = VK_TRUE;
    if (state->timelineSemaphores) {
        deviceExtensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
        timelineFeatures.pNext = byteFeatures.pNext;
        byteFeatures.pNext = &timelineFeatures;
    } else {
        aps5::LogErr( "[gpu] timeline semaphores unavailable or disabled; drains wait under the GPU mutex\n");
    }
    bdaFeatures.pNext = &byteFeatures;
    deviceInfo.pNext = &bdaFeatures;
    check(state->InstanceFunction<PFN_vkCreateDevice>("vkCreateDevice")(selected, &deviceInfo, nullptr, &state->device), "vkCreateDevice");
    state->DeviceFunction<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(state->device, family, 0, &state->queue);
    APS5_LOG_OUT("Vulkan device ready device=%p queue=%p family=%u", reinterpret_cast<void*>(state->device), reinterpret_cast<void*>(state->queue), family);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = family;
    check(state->DeviceFunction<PFN_vkCreateCommandPool>("vkCreateCommandPool")(state->device, &poolInfo, nullptr, &state->pool), "vkCreateCommandPool");
    Graphics::PrepareImportWatch(graphicsContext());
    state->bufferPool = std::make_shared<Graphics::BufferPool>(graphicsContext());
    state->emptyBuffer = std::make_unique<Graphics::Buffer>(graphicsContext(), Graphics::EmptyBufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    state->pipelineCache = std::make_unique<Graphics::PipelineCache>(graphicsContext(), state->properties);
    state->detiler = std::make_unique<Graphics::TextureDetiler>(graphicsContext());
    state->textureCache = std::make_unique<Graphics::TextureCache>(graphicsContext());
    state->colorTransfer = std::make_unique<Graphics::GpuColorTransfer>(graphicsContext());
    state->descriptorCache = std::make_unique<Graphics::DescriptorCache>(graphicsContext());
    state->samplerCache = std::make_unique<Graphics::SamplerCache>();
    state->recorder = std::make_unique<Graphics::Recorder>(graphicsContext(), state->timelineSemaphores);
    state->recorder->Activate();

    static const bool procTable = std::getenv("APS5_NO_PROC_TABLE") == nullptr;
    if (procTable) {
        Graphics::FillDeviceFunctions(graphicsContext(), state->deviceFunctions);
        state->functionsReady = true;
    }
    state->context = buildContext();
    state->contextReady = true;
    if (window != nullptr) {
        require(window->getDrawableSize != nullptr, "missing window drawable size query");
        std::uint32_t drawableWidth = 0;
        std::uint32_t drawableHeight = 0;
        window->getDrawableSize(window->context, &drawableWidth, &drawableHeight);
        require(drawableWidth != 0 && drawableHeight != 0, "window has a zero drawable size at creation");
        VkSurfaceCapabilitiesKHR surface{};
        check(state->InstanceFunction<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(selected, state->surface, &surface), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        state->extent = {drawableWidth, drawableHeight};
        APS5_LOG_OUT("Surface capabilities drawable=%ux%u min=%ux%u max=%ux%u minImages=%u maxImages=%u usage=0x%x", drawableWidth, drawableHeight, surface.minImageExtent.width, surface.minImageExtent.height, surface.maxImageExtent.width, surface.maxImageExtent.height, surface.minImageCount, surface.maxImageCount, surface.supportedUsageFlags);

        if (surface.currentExtent.width != std::numeric_limits<std::uint32_t>::max() && surface.currentExtent.width != 0 && surface.currentExtent.height != 0) {
            drawableWidth = surface.currentExtent.width;
            drawableHeight = surface.currentExtent.height;
            state->extent = {drawableWidth, drawableHeight};
        }
        require(drawableWidth >= surface.minImageExtent.width && drawableWidth <= surface.maxImageExtent.width && drawableHeight >= surface.minImageExtent.height && drawableHeight <= surface.maxImageExtent.height, "unsupported output extent");
        require((surface.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0, "surface does not support transfer destination images");
        require((surface.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0, "opaque composition is unavailable");
        require((surface.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0, "identity surface transform is unavailable");
        std::uint32_t formatCount = 0;
        auto getFormats = state->InstanceFunction<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR");
        check(getFormats(selected, state->surface, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR");
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        check(getFormats(selected, state->surface, &formatCount, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");
        require(std::any_of(formats.begin(), formats.end(), [](const auto& format) { return format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; }), "BGRA8 sRGB-nonlinear surface format is unavailable");
        VkSwapchainCreateInfoKHR swapchain{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        swapchain.surface = state->surface;
        swapchain.minImageCount = surface.minImageCount;
        swapchain.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        swapchain.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        swapchain.imageExtent = state->extent;
        swapchain.imageArrayLayers = 1;
        state->overlayUsable = (surface.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        swapchain.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (state->overlayUsable ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u);
        swapchain.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchain.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        swapchain.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swapchain.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapchain.clipped = VK_FALSE;
        check(state->DeviceFunction<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR")(state->device, &swapchain, nullptr, &state->swapchain), "vkCreateSwapchainKHR");
        ++state->swapchainSerial;
        std::uint32_t imageCount = 0;
        auto getImages = state->DeviceFunction<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
        check(getImages(state->device, state->swapchain, &imageCount, nullptr), "vkGetSwapchainImagesKHR");
        state->images.resize(imageCount);
        check(getImages(state->device, state->swapchain, &imageCount, state->images.data()), "vkGetSwapchainImagesKHR");
        APS5_LOG_OUT("Swapchain created swapchain=%p extent=%ux%u images=%u", reinterpret_cast<void*>(state->swapchain), state->extent.width, state->extent.height, imageCount);
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        const auto createFence = state->DeviceFunction<PFN_vkCreateFence>("vkCreateFence");
        check(createFence(state->device, &fence, nullptr, &state->acquireFence), "vkCreateFence");
        state->images.resize(imageCount);
        state->rendered.resize(imageCount, VK_NULL_HANDLE);
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = state->pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        state->presentSlots.resize(FlipInFlight() + 1);
        for (auto& slot : state->presentSlots) {
            check(createFence(state->device, &fence, nullptr, &slot.fence), "vkCreateFence present");
            check(state->DeviceFunction<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(state->device, &allocation, &slot.commands), "vkAllocateCommandBuffers");
        }
        state->scaler = std::make_unique<PresentationScaler>(graphicsContext(), VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM);
    }
}

VulkanDevice::~VulkanDevice() = default;

std::uint32_t VulkanDevice::PhysicalVendorId() const {
    return state ? state->properties.vendorID : 0;
}

void VulkanDevice::PrepareForReplacement() {
    GuestMemory::AssertGpuLockHeld("VulkanDevice::PrepareForReplacement");
    require(state->recorder != nullptr && Graphics::Recorder::Active() == state->recorder.get(), "device replacement requires its active recorder");
    Graphics::FlushCachedTextures(state->device);
    Graphics::PublishAllShadows(state->context, Graphics::PublishReason::Teardown);
    WaitIdle();
    Graphics::DestroyShadows(state->device);
}

void VulkanDevice::WaitIdle() {
    APS5_LOG_CHARS_OUT_DEBUG("VulkanDevice::WaitIdle begin");
    RetirePresents(0);
    if (state->recorder) {

        if (!state->recorder->Idle()) Graphics::Recorder::CountSync(0, __builtin_return_address(0));
        state->recorder->Sync();
    }
    check(state->DeviceFunction<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(state->device), "vkDeviceWaitIdle");
    APS5_LOG_CHARS_OUT_DEBUG("VulkanDevice::WaitIdle complete");
}

namespace {

bool OpportunisticReap() {
    static const bool enabled = std::getenv("APS5_NO_OPPORTUNISTIC_REAP") == nullptr;
    return enabled;
}

}

void VulkanDevice::SubmitRecorded(bool reapFirst) {
    if (!state->recorder) return;

    if (reapFirst && OpportunisticReap()) state->recorder->Reap();
    state->recorder->Submit();
}

bool VulkanDevice::CanWaitUnlocked() const {
    return state->recorder && state->recorder->HasTimeline();
}

std::uint64_t VulkanDevice::SubmitAndEpoch() {
    if (!state->recorder) return 0;
    if (OpportunisticReap()) state->recorder->Reap();
    return state->recorder->SubmitAndEpoch();
}

void VulkanDevice::WaitRecorded(std::uint64_t serial) {
    if (state->recorder) state->recorder->WaitSerial(serial);
}

void VulkanDevice::ReapRecorded(std::uint64_t serial) {
    if (!state->recorder) return;

    Graphics::Recorder::AnnounceSyncSite(__builtin_return_address(0));
    state->recorder->FinishUpTo(serial);
}

void VulkanDevice::ReapRecorded() {
    if (state->recorder) state->recorder->Reap();
}

std::optional<Graphics::Recorder::LabelHit> VulkanDevice::PendingLabel(std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp, Graphics::Recorder::LabelRefusal* refusal) const {
    if (!state->recorder) return std::nullopt;
    return state->recorder->PendingLabel(address, bytes, afterStamp, refusal);
}

bool VulkanDevice::OpenWriteOverlaps(std::uint64_t address, std::size_t bytes) const {
    return state->recorder && state->recorder->OpenWriteOverlaps(address, bytes);
}

int VulkanDevice::WriteLabelOnGpu(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue, bool reapFirst) {
    if (!state->recorder || bytes.empty() || bytes.size() > 65536 || address % 4 != 0 || bytes.size() % 4 != 0) return 4;
    auto& recorder = *state->recorder;

    static const bool reapAllQueues = std::getenv("APS5_LABEL_REAP_ALL_QUEUES") != nullptr;
    if (reapFirst && (queue == 0 || reapAllQueues) && OpportunisticReap()) recorder.Reap();
    if (recorder.Idle()) return 1;
    static const bool drain = std::getenv("APS5_DRAIN_COMPLETION_LABELS") != nullptr;
    const auto context = graphicsContext();
    const auto* import = Graphics::HostImportFor(context, address, bytes.size());
    if (import == nullptr) {

        if (drain) return 3;
        recorder.AfterCompletions(address, bytes, stamp, queue, false);
        return 6;
    }

    if (Graphics::AnyShadowedOverlaps(address, bytes.size())) Graphics::PublishShadow(address, bytes.size(), Graphics::PublishScope::PartialUnits, Graphics::PublishReason::Label);

    const auto recordStore = [&] { recorder.RecordStore(import->buffer, address - import->base, bytes, address); };

    static const bool gateAll = std::getenv("APS5_LABEL_GATE_ALL") != nullptr;
    if (gateAll ? recorder.HasCompletions() : state->CopiedWriterOverlaps(address, bytes.size())) {

        if (drain) return 2;
        recordStore();
        GuestMemory::MarkWritten(address, bytes.size());

        recorder.AfterCompletions(address, bytes, stamp, queue, true);
        return 5;
    }
    recordStore();

    recorder.NoteLabel(address, bytes, stamp, queue);
    GuestMemory::MarkWritten(address, bytes.size());

    static const bool submitNow = std::getenv("APS5_LABEL_SUBMIT_NOW") != nullptr;
    if (submitNow) recorder.Submit();
    return 0;
}

bool VulkanDevice::FillBuffer(std::uint64_t address, std::size_t bytes, std::span<const std::uint32_t, 4> pattern) {
    if (!state->recorder || bytes == 0 || bytes % 16 != 0 || address % 16 != 0) return false;
    auto& recorder = *state->recorder;

    if (OpportunisticReap()) recorder.Reap();
    const auto context = graphicsContext();
    const Graphics::HostImport* import = Graphics::HostImportFor(context, address, bytes);
    if (import == nullptr) return false;

    static const bool alwaysSync = std::getenv("APS5_FILL_SYNC") != nullptr;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;

    static std::uint64_t fills = 0, synced = 0, labelSynced = 0, ordered = 0;
    static double syncedMs = 0;

    static double decideMs = 0, recordMs = 0, notesMs = 0;
    static std::uint64_t uniformFills = 0, patternFills = 0, doublingSteps = 0, patternBuffersReused = 0, patternBuffersMade = 0;
    static auto lastReport = std::chrono::steady_clock::now();

    static const bool chainInPlace = std::getenv("APS5_FILL_CHAIN") != nullptr;
    constexpr std::size_t PatternBufferBytes = 16u << 20u;
    constexpr std::size_t PatternBuffersKept = 8;
    const auto fillStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ++fills;
    if (recorder.PendingWriteOverlaps(address, bytes)) {

        bool sync = alwaysSync || std::any_of(state->copiedWriters->begin(), state->copiedWriters->end(), [&](const auto& writer) { return writer->WritesOverlap(address, bytes); })
            || std::any_of(Graphics::DrawCopiedWriters()->begin(), Graphics::DrawCopiedWriters()->end(), [&](const auto& writer) { return writer->WritesOverlap(address, bytes); });
        if (!sync && Graphics::Recorder::PendingCompletionLabels() != 0) {

            sync = recorder.PendingLabelIn(address, bytes);
            if (sync) ++labelSynced;
        }
        if (sync) {
            ++synced;
            const auto syncStart = std::chrono::steady_clock::now();
            Graphics::Recorder::CountSync(4);
            recorder.SyncThrough(address, bytes);
            syncedMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - syncStart).count();

            import = Graphics::HostImportFor(context, address, bytes);
            if (import == nullptr) return false;
        } else {
            ++ordered;
        }
    }
    if (profile) {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::seconds(10)) {
            lastReport = now;
            aps5::LogErr( "[fill-sync] fills: %llu synced (%llu with a completion label pending) waited %.0f ms, %llu ordered by barrier, %llu with no pending write; phases ms: decide %.0f record %.0f notes %.0f; %llu uniform, %llu pattern (%llu doubling steps in place; pattern buffers reused %llu, made %llu)\n", static_cast<unsigned long long>(synced), static_cast<unsigned long long>(labelSynced), syncedMs, static_cast<unsigned long long>(ordered), static_cast<unsigned long long>(fills - synced - ordered), decideMs, recordMs, notesMs, static_cast<unsigned long long>(uniformFills), static_cast<unsigned long long>(patternFills), static_cast<unsigned long long>(doublingSteps), static_cast<unsigned long long>(patternBuffersReused), static_cast<unsigned long long>(patternBuffersMade));
        }
    }
    auto phaseStart = fillStart;
    const auto phase = [&](double& total) {
        if (!profile) return;
        const auto now = std::chrono::steady_clock::now();
        total += std::chrono::duration<double, std::milli>(now - phaseStart).count();
        phaseStart = now;
    };
    phase(decideMs);
    using CommandClass = Graphics::Recorder::CommandClass;

    recorder.FlushKeyStoresOverlapping(address, bytes);
    const bool uniform = pattern[0] == pattern[1] && pattern[1] == pattern[2] && pattern[2] == pattern[3];
    const std::array<std::uint32_t, 4> patternWords{pattern[0], pattern[1], pattern[2], pattern[3]};
    std::shared_ptr<Graphics::DeviceBuffer> patternBuffer;
    if (!uniform && !chainInPlace) {
        auto& buffers = state->patternBuffers;
        const auto found = std::find_if(buffers.begin(), buffers.end(), [&](const auto& entry) { return entry.first == patternWords; });
        if (found != buffers.end()) {
            patternBuffer = found->second;
            std::rotate(found, std::next(found), buffers.end());
            ++patternBuffersReused;
        } else {
            patternBuffer = std::make_shared<Graphics::DeviceBuffer>(context, PatternBufferBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            auto seed = std::make_shared<Graphics::Buffer>(context, 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            std::memcpy(seed->Bytes().data(), pattern.data(), 16);
            recorder.Keep(seed);
            const auto chain = recorder.Commands();
            const auto copyBuffer = context.Resolved(&Graphics::DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer");
            const VkBufferCopy first{0, 0, 16};
            copyBuffer(chain, seed->Handle(), patternBuffer->Handle(), 1, &first);
            for (std::size_t done = 16; done < PatternBufferBytes; done *= 2) {
                Graphics::RecordMemoryBarrier(context, chain, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                const VkBufferCopy copy{0, done, done};
                copyBuffer(chain, patternBuffer->Handle(), patternBuffer->Handle(), 1, &copy);
            }
            Graphics::RecordMemoryBarrier(context, chain, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            if (buffers.size() >= PatternBuffersKept) buffers.erase(buffers.begin());
            buffers.emplace_back(patternWords, patternBuffer);
            ++patternBuffersMade;
        }

        recorder.Keep(patternBuffer);
    }

    recorder.FlushStoresOverlapping(address, bytes);
    VkAccessFlags covered = 0;
    const auto commands = recorder.Commands(&covered);
    const auto timing = recorder.BeginGpuTiming(CommandClass::Fill);
    if (Graphics::Recorder::BarrierValidate()) {
        const std::pair<std::uint64_t, std::uint64_t> range{address, address + bytes};
        recorder.NoteAccess(CommandClass::Fill, Graphics::Recorder::Access{{}, std::span(&range, 1), {}, VK_PIPELINE_STAGE_TRANSFER_BIT});
    }

    if ((covered & VK_ACCESS_TRANSFER_WRITE_BIT) != 0 && Graphics::Recorder::MergeBarriers()) {
        Graphics::Recorder::CountMerged(CommandClass::Fill);
    } else {
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        Graphics::Recorder::CountBarriers(CommandClass::Fill);
    }
    const auto offset = address - import->base;
    if (uniform) {
        ++uniformFills;
        context.Resolved(&Graphics::DeviceFunctions::cmdFillBuffer, "vkCmdFillBuffer")(commands, import->buffer, offset, bytes, pattern[0]);
    } else if (patternBuffer != nullptr) {
        ++patternFills;
        std::vector<VkBufferCopy> copies;
        for (std::size_t done = 0; done < bytes; done += PatternBufferBytes) copies.push_back({0, offset + done, std::min(PatternBufferBytes, bytes - done)});
        context.Resolved(&Graphics::DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(commands, patternBuffer->Handle(), import->buffer, static_cast<std::uint32_t>(copies.size()), copies.data());
    } else {
        ++patternFills;

        auto seed = std::make_shared<Graphics::Buffer>(context, 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(seed->Bytes().data(), pattern.data(), 16);
        recorder.Keep(seed);
        const VkBufferCopy first{0, offset, 16};
        const auto copyBuffer = context.Resolved(&Graphics::DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer");
        copyBuffer(commands, seed->Handle(), import->buffer, 1, &first);
        for (std::size_t done = 16; done < bytes;) {
            const auto chunk = std::min(done, bytes - done);
            Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            Graphics::Recorder::CountBarriers(CommandClass::Fill);
            const VkBufferCopy copy{offset, offset + done, chunk};
            copyBuffer(commands, import->buffer, import->buffer, 1, &copy);
            done += chunk;
            ++doublingSteps;
        }
    }

    constexpr VkAccessFlags filledAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, filledAccess);
    Graphics::Recorder::CountBarriers(CommandClass::Fill);
    recorder.EndGpuTiming(timing, bytes);
    recorder.MarkCovered(filledAccess);
    phase(recordMs);
    recorder.NotePendingWrite(address, bytes);
    GuestMemory::MarkWritten(address, bytes);
    phase(notesMs);
    return true;
}

namespace {

std::atomic<std::uint64_t> copiesVerified{0}, copySourceChanged{0}, copyDestinationChanged{0}, copyReaderMissed{0};
std::atomic<std::uint64_t> copiesAliased{0}, copyAliasCreated{0}, copyAliasNoSource{0}, copyAliasShape{0}, copyAliasRefused{0};

bool AliasCopy(const Graphics::Context& context, std::uint64_t destination, std::uint64_t source, std::size_t bytes) {
    static const bool disabled = std::getenv("APS5_NO_COPY_ALIAS") != nullptr;
    if (disabled) return false;
    auto from = Graphics::StorageTexture::FindLive(source, bytes);
    if (from == nullptr) {
        copyAliasNoSource.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto to = Graphics::StorageTexture::FindLive(destination, bytes);
    if (to != nullptr && !to->SameSurfaceShape(*from)) {
        copyAliasShape.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (to == nullptr) {
        auto resource = from->Descriptor();
        resource.baseAddress = destination;
        resource.dccAddress = 0;
        try {
            to = Graphics::CachedStorageSurface(context, resource);
        } catch (const std::exception& error) {
            static std::atomic<int> reported{0};
            if (reported.fetch_add(1) < 8) aps5::LogErr( "[copy] no storage image for the copy destination 0x%llx: %s\n", static_cast<unsigned long long>(destination), error.what());
            copyAliasRefused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        copyAliasCreated.fetch_add(1, std::memory_order_relaxed);
    }
    const char* refusal = nullptr;
    if (!to->CopyFrom(*from, refusal)) {
        static std::atomic<int> reported{0};
        if (reported.fetch_add(1) < 8) aps5::LogErr( "[copy] image copy 0x%llx -> 0x%llx (0x%zx bytes) refused: %s\n", static_cast<unsigned long long>(source), static_cast<unsigned long long>(destination), bytes, refusal);
        copyAliasRefused.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    copiesAliased.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}

VulkanDevice::CopyVerification VulkanDevice::CopyVerifyCounts() {
    return {copiesVerified.load(std::memory_order_relaxed), copySourceChanged.load(std::memory_order_relaxed), copyDestinationChanged.load(std::memory_order_relaxed), copyReaderMissed.load(std::memory_order_relaxed)};
}

VulkanDevice::CopyAliasing VulkanDevice::CopyAliasCounts() {
    return {copiesAliased.load(std::memory_order_relaxed), copyAliasCreated.load(std::memory_order_relaxed), copyAliasNoSource.load(std::memory_order_relaxed), copyAliasShape.load(std::memory_order_relaxed), copyAliasRefused.load(std::memory_order_relaxed)};
}

VulkanDevice::CopyOutcome VulkanDevice::CopyBuffer(std::uint64_t destination, std::uint64_t source, std::size_t bytes, std::size_t cpuMax, std::size_t gpuMax, std::size_t knownMax, std::uint64_t programAddress, std::uint32_t queue, const CopyWriterNote& noteWriter) {
    CopyOutcome outcome{2, false, 0, CopyOutcome::None, false, false, 0, 0, 0, 0, false};
    if (!state->recorder || bytes == 0) return outcome;
    auto& recorder = *state->recorder;

    static const bool alwaysSync = std::getenv("APS5_COPY_SYNC") != nullptr;
    static const bool waitForSource = std::getenv("APS5_COPY_WAIT_SOURCE") != nullptr;
    static const bool verify = std::getenv("APS5_COPY_VERIFY") != nullptr;

    const auto completionStoreOverlaps = [&](std::uint64_t address) {
        if (alwaysSync || std::any_of(state->copiedWriters->begin(), state->copiedWriters->end(), [&](const auto& writer) { return writer->WritesOverlap(address, bytes); })
            || std::any_of(Graphics::DrawCopiedWriters()->begin(), Graphics::DrawCopiedWriters()->end(), [&](const auto& writer) { return writer->WritesOverlap(address, bytes); })) return true;
        return Graphics::Recorder::PendingCompletionLabels() != 0 && recorder.PendingLabelIn(address, bytes);
    };

    bool settledBySignal = false;
    const auto refusal = [&]() -> int {
        settledBySignal = false;
        if (bytes > cpuMax) return CopyOutcome::Size;

        const std::array<std::pair<std::uint64_t, std::uint64_t>, 2> ranges{{{source, source + bytes}, {destination, destination + bytes}}};
        if (Graphics::StorageTexture::AnyPendingOverlaps(ranges)) return CopyOutcome::Image;

        if (Graphics::AnyShadowedOverlaps(ranges)) return CopyOutcome::Shadow;
        if (recorder.PendingWriteOverlaps(source, bytes)) {
            if (!recorder.PendingWriteSettled(source, bytes)) return CopyOutcome::SourcePending;
            if (completionStoreOverlaps(source) || recorder.PendingLabelIn(source, bytes)) return CopyOutcome::SourceUnsettled;
            settledBySignal = true;
        }
        if (recorder.PendingWriteOverlaps(destination, bytes)) {

            static const bool strict = std::getenv("APS5_COPY_DESTINATION_STRICT") != nullptr;
            if (strict || !recorder.PendingWriteSettled(destination, bytes)) return CopyOutcome::DestinationPending;
            if (completionStoreOverlaps(destination)) return CopyOutcome::DestinationUnsettled;
        }
        if (recorder.PendingLabelIn(destination, bytes)) return CopyOutcome::Label;
        if (Graphics::Recorder::ReadTracking() ? recorder.PendingReadOverlaps(destination, bytes) : !recorder.Idle()) return CopyOutcome::Reader;
        return CopyOutcome::None;
    };
    outcome.reason = refusal();
    if (outcome.reason == CopyOutcome::SourcePending && waitForSource && queue != 0) {

        if (const auto info = recorder.DescribePendingWrite(source, bytes); info.has_value() && !info->open && info->batchesToFinish == 1) {
            const auto waitStart = std::chrono::steady_clock::now();
            Graphics::Recorder::CountSync(4);
            recorder.SyncThrough(source, bytes, true);
            outcome.waitedForSource = true;
            outcome.waitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
            outcome.reason = refusal();
        }
    }
    if (outcome.reason == CopyOutcome::None) {

        std::vector<std::byte> expected;
        if (verify) expected.assign(reinterpret_cast<const std::byte*>(source), reinterpret_cast<const std::byte*>(source) + bytes);
        std::memcpy(reinterpret_cast<void*>(destination), reinterpret_cast<const void*>(source), bytes);
        GuestMemory::MarkWritten(destination, bytes);
        outcome.path = 0;
        outcome.sourceSettledBySignal = settledBySignal;
        if (verify) {

            if (const auto reader = recorder.DescribePendingRead(destination, bytes); reader.has_value() && !reader->signaled) copyReaderMissed.fetch_add(1, std::memory_order_relaxed);
            Graphics::Recorder::CountSync(4);
            recorder.Sync();
            copiesVerified.fetch_add(1, std::memory_order_relaxed);
            if (GuestMemory::Accessible(reinterpret_cast<const void*>(source), bytes) && std::memcmp(expected.data(), reinterpret_cast<const void*>(source), bytes) != 0) copySourceChanged.fetch_add(1, std::memory_order_relaxed);
            if (GuestMemory::Accessible(reinterpret_cast<const void*>(destination), bytes) && std::memcmp(expected.data(), reinterpret_cast<const void*>(destination), bytes) != 0) copyDestinationChanged.fetch_add(1, std::memory_order_relaxed);
        }
        return outcome;
    }
    if (outcome.reason == CopyOutcome::Reader) {
        if (const auto reader = recorder.DescribePendingRead(destination, bytes)) {
            outcome.readerSerial = reader->serial;
            outcome.readerQueue = reader->queue;
            outcome.readerKind = static_cast<int>(reader->kind);
            outcome.readerOpen = reader->open;
        }
    }
    const auto context = graphicsContext();

    const bool waitSource = recorder.PendingWriteOverlaps(source, bytes) && completionStoreOverlaps(source);
    const bool waitDestination = recorder.PendingWriteOverlaps(destination, bytes) && completionStoreOverlaps(destination);
    if (waitSource || waitDestination) {
        outcome.synced = true;
        const auto syncStart = std::chrono::steady_clock::now();
        if (waitSource) {
            Graphics::Recorder::CountSync(4);
            recorder.SyncThrough(source, bytes);
        }
        if (waitDestination) {
            Graphics::Recorder::CountSync(4);
            recorder.SyncThrough(destination, bytes);
        }
        outcome.syncMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - syncStart).count();
    }

    if (AliasCopy(context, destination, source, bytes)) {
        noteWriter({}, 0);
        recorder.NotePendingWrite(destination, bytes);
        outcome.path = 3;
        return outcome;
    }
    if (bytes > gpuMax) {
        outcome.path = 8;
        return outcome;
    }

    Graphics::StorageTexture::FlushPending(source, bytes, nullptr, "buffer copy source", Graphics::PublishScope::Whole);
    Graphics::StorageTexture::FlushPending(destination, bytes, nullptr, "buffer copy destination", Graphics::PublishScope::PartialUnits);

    const Graphics::HostImport* destinationImport = Graphics::HostImportFor(context, destination, bytes);
    const Graphics::HostImport* sourceImport = destinationImport != nullptr ? Graphics::HostImportFor(context, source, bytes) : nullptr;
    if (destinationImport == nullptr || sourceImport == nullptr) return outcome;
    using CommandClass = Graphics::Recorder::CommandClass;

    recorder.FlushKeyStoresOverlapping(source, bytes);
    recorder.FlushKeyStoresOverlapping(destination, bytes);

    recorder.FlushStoresOverlapping(source, bytes);
    recorder.FlushStoresOverlapping(destination, bytes);
    VkAccessFlags covered = 0;
    const auto commands = recorder.Commands(&covered);

    const auto classTiming = recorder.BeginGpuTiming(CommandClass::Copy);
    if (Graphics::Recorder::BarrierValidate()) {
        const std::pair<std::uint64_t, std::uint64_t> read{source, source + bytes}, written{destination, destination + bytes};
        recorder.NoteAccess(CommandClass::Copy, Graphics::Recorder::Access{std::span(&read, 1), std::span(&written, 1), {}, VK_PIPELINE_STAGE_TRANSFER_BIT});
    }

    constexpr VkAccessFlags transferAccess = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    if ((covered & transferAccess) == transferAccess && Graphics::Recorder::MergeBarriers()) {
        Graphics::Recorder::CountMerged(CommandClass::Copy);
    } else {
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, transferAccess);
        Graphics::Recorder::CountBarriers(CommandClass::Copy);
    }
    const auto gpuTiming = recorder.BeginGpuTiming(programAddress);
    const VkBufferCopy region{source - sourceImport->base, destination - destinationImport->base, bytes};
    context.Resolved(&Graphics::DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(commands, sourceImport->buffer, destinationImport->buffer, 1, &region);
    recorder.EndGpuTiming(gpuTiming, bytes);

    constexpr VkAccessFlags copiedAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, copiedAccess);
    Graphics::Recorder::CountBarriers(CommandClass::Copy);
    recorder.EndGpuTiming(classTiming, bytes);
    recorder.MarkCovered(copiedAccess);
    recorder.NotePendingRead(source, bytes, Graphics::Recorder::ReadKind::CopySource);

    const auto generation = GuestMemory::MarkWritten(destination, bytes);
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> sourceRange{{{source, source + bytes}}};
    const bool sourceCurrent = bytes <= knownMax && generation != 0 && !recorder.PendingWriteOverlaps(source, bytes) && !recorder.PendingLabelIn(source, bytes) && !completionStoreOverlaps(source) && !Graphics::StorageTexture::AnyPendingOverlaps(sourceRange) && !Graphics::AnyShadowedOverlaps(sourceRange);
    noteWriter(sourceCurrent ? std::span(reinterpret_cast<const std::byte*>(source), bytes) : std::span<const std::byte>{}, sourceCurrent ? generation : 0);
    recorder.NotePendingWrite(destination, bytes);
    outcome.path = 1;
    return outcome;
}

void* VulkanDevice::Window() const {
    return state->window;
}

void VulkanDevice::Resize(std::uint32_t width, std::uint32_t height) {
    APS5_LOG_OUT_DEBUG("Resize requested=%ux%u current=%ux%u", width, height, state->extent.width, state->extent.height);
    require(state->swapchain != VK_NULL_HANDLE, "cannot resize an unavailable swapchain");
    if (width == 0 || height == 0) {
        state->extent = {0, 0};
        return;
    }
    if (state->extent.width == width && state->extent.height == height) return;
    WaitIdle();
    VkSurfaceCapabilitiesKHR surface{};
    check(state->InstanceFunction<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(state->physical, state->surface, &surface), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR resize");

    if (surface.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        width = surface.currentExtent.width;
        height = surface.currentExtent.height;
        if (width == 0 || height == 0) {
            state->extent = {0, 0};
            return;
        }
        if (state->extent.width == width && state->extent.height == height) return;
    }
    require(width >= surface.minImageExtent.width && width <= surface.maxImageExtent.width && height >= surface.minImageExtent.height && height <= surface.maxImageExtent.height, "unsupported resized output extent");
    require((surface.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 && (surface.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0 && (surface.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0, "resized surface capabilities are unsupported");
    VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    create.surface = state->surface;
    create.minImageCount = surface.minImageCount;
    create.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    create.imageExtent = {width, height};
    create.imageArrayLayers = 1;
    create.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (state->overlayUsable ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u);
    create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    create.oldSwapchain = state->swapchain;
    state->retiredSwapchains.reserve(state->retiredSwapchains.size() + 1);
    VkSwapchainKHR replacement = VK_NULL_HANDLE;
    check(state->DeviceFunction<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR")(state->device, &create, nullptr, &replacement), "vkCreateSwapchainKHR resize");
    state->retiredSwapchains.push_back({state->swapchain, std::move(state->rendered)});
    state->swapchain = replacement;
    ++state->swapchainSerial;
    state->extent = create.imageExtent;
    auto getImages = state->DeviceFunction<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
    std::uint32_t count = 0;
    check(getImages(state->device, replacement, &count, nullptr), "vkGetSwapchainImagesKHR resize");
    state->images.resize(count);
    check(getImages(state->device, replacement, &count, state->images.data()), "vkGetSwapchainImagesKHR resize");
    state->images.resize(count);
    state->rendered.assign(count, VK_NULL_HANDLE);
    APS5_LOG_OUT_DEBUG("Resize complete swapchain=%p extent=%ux%u images=%u", reinterpret_cast<void*>(state->swapchain), state->extent.width, state->extent.height, count);
}

bool VulkanDevice::Presentable() const {
    return state->extent.width != 0 && state->extent.height != 0;
}

void VulkanDevice::SetPresentationOverlay(PresentationOverlay* overlay) {
    state->overlay = overlay;
}

bool VulkanDevice::PresentClear(std::uint32_t width, std::uint32_t height, bool opaque) {
    APS5_LOG_OUT_DEBUG("PresentClear width=%u height=%u opaque=%u", width, height, static_cast<unsigned>(opaque));
    return present(width, height, opaque, {});
}

void VulkanDevice::PresentPixels(std::uint32_t width, std::uint32_t height, std::span<const std::byte> pixels) {
    APS5_LOG_OUT("PresentPixels width=%u height=%u pixels=%zu expected=%llu", width, height, pixels.size(), static_cast<unsigned long long>(width) * height * 4u);
    require(width != 0 && height != 0 && width <= 16384 && height <= 16384, "invalid display image extent");
    require(pixels.size() == static_cast<std::uint64_t>(width) * height * 4, "invalid display pixel buffer size");

    static const int dumpLimit = [] {
        const char* value = std::getenv("APS5_DUMP_FRAMES");
        return value ? std::atoi(value) : 0;
    }();
    static int dumped = 0;
    if (dumped < dumpLimit) {
        char name[32];
        std::snprintf(name, sizeof(name), "frame_%03d.bmp", dumped++);
        if (std::FILE* file = std::fopen(name, "wb")) {
            const std::uint32_t imageBytes = width * height * 4;
            const std::uint32_t header[13] = {0, 0, 54, 40, width, static_cast<std::uint32_t>(-static_cast<std::int32_t>(height)), 1u | (32u << 16u), 0, imageBytes, 2835, 2835, 0, 0};
            const std::uint16_t magic = 0x4d42;
            const std::uint32_t fileBytes = 54 + imageBytes;
            std::fwrite(&magic, 2, 1, file);
            std::fwrite(&fileBytes, 4, 1, file);
            std::fwrite(header + 1, 4, 12, file);
            std::fwrite(pixels.data(), 1, pixels.size(), file);
            std::fclose(file);
        }
    }

    if (!present(width, height, true, pixels)) return;
    FinishPresent();
    QueuePresent();
}

namespace {

std::uint32_t DumpScale() {
    static const std::uint32_t scale = [] {
        const char* value = std::getenv("APS5_DUMP_SCALE");
        if (value == nullptr) return std::uint32_t{4};
        std::uint32_t parsed = 0;
        const auto end = value + std::strlen(value);
        const auto result = std::from_chars(value, end, parsed);
        require(result.ec == std::errc{} && result.ptr == end && parsed > 0 && parsed <= 16384, "APS5_DUMP_SCALE must be between 1 and 16384");
        return parsed;
    }();
    return scale;
}

void WriteFrameBmp(int index, std::uint32_t fullWidth, std::uint32_t fullHeight, std::span<const std::byte> full, std::uint32_t scale) {
    require(scale != 0 && full.size() == static_cast<std::size_t>(fullWidth) * fullHeight * 4, "invalid frame dump pixels");
    const std::uint32_t width = (fullWidth + scale - 1) / scale;
    const std::uint32_t height = (fullHeight + scale - 1) / scale;
    std::vector<std::byte> pixels(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) std::memcpy(pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4, full.data() + (static_cast<std::size_t>(y) * scale * fullWidth + x * scale) * 4, 4);
    }
    char name[32];
    std::snprintf(name, sizeof(name), "frame_%03d.bmp", index);
    {
        std::ofstream file;
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file.open(name, std::ios::binary);
        const std::uint32_t imageBytes = width * height * 4;
        const std::uint32_t header[13] = {0, 0, 54, 40, width, static_cast<std::uint32_t>(-static_cast<std::int32_t>(height)), 1u | (32u << 16u), 0, imageBytes, 2835, 2835, 0, 0};
        const std::uint16_t magic = 0x4d42;
        const std::uint32_t fileBytes = 54 + imageBytes;
        file.write(reinterpret_cast<const char*>(&magic), 2);
        file.write(reinterpret_cast<const char*>(&fileBytes), 4);
        file.write(reinterpret_cast<const char*>(header + 1), 48);
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        file.close();
    }
}

bool EquivalentComponentSizes(VkFormat from, VkFormat to) {
    const bool eightBit = (from == VK_FORMAT_R8G8B8A8_UNORM || from == VK_FORMAT_B8G8R8A8_UNORM) && (to == VK_FORMAT_R8G8B8A8_UNORM || to == VK_FORMAT_R8G8B8A8_UNORM);
    const bool tenBit = (from == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || from == VK_FORMAT_A2R10G10B10_UNORM_PACK32) && (to == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || to == VK_FORMAT_A2R10G10B10_UNORM_PACK32);
    return eightBit || tenBit;
}

VkFormat DisplayFormatFor(const DisplayBuffer& buffer) {
    constexpr std::uint64_t tenBitFormat = 0x0100000000000000ull;
    const bool tenBit = (buffer.pixelFormat & tenBitFormat) != 0;
    const bool rgba = (buffer.pixelFormat & ~tenBitFormat) == 0x8000000022000000ull;
    return tenBit ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : rgba ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
}

struct ResidentFormats {
    VkFormat storage = VK_FORMAT_UNDEFINED, rendered = VK_FORMAT_UNDEFINED, display = VK_FORMAT_UNDEFINED;
    bool equivalent = false;
};

bool ResidentPresentable(const Graphics::Context& context, const Graphics::StorageTexture& image, const DisplayBuffer& buffer, VkFilter& filter, bool& copy, ResidentFormats& formats, const char*& reason) {
    const auto& descriptor = image.Descriptor();
    if (descriptor.width != buffer.width || descriptor.height != buffer.height) { reason = "extent"; return false; }
    if (descriptor.mipCount != 1 || image.ImageLayers() != 1 || image.ImageDepth() != 1) { reason = "mips/layers"; return false; }
    if (image.GuestBytes() != DisplayBufferSize(buffer)) { reason = "bytes"; return false; }
    formats.storage = Graphics::StorageFormatForGuest(context, descriptor.format);
    formats.rendered = image.AttachmentFormat();
    formats.display = DisplayFormatFor(buffer);
    formats.equivalent = EquivalentComponentSizes(formats.storage, formats.display);
    copy = false;
    if (formats.storage != formats.display) {

        if (formats.rendered == VK_FORMAT_UNDEFINED) { reason = "unattached"; return false; }

        if (formats.rendered == formats.display) copy = true;
        else if (formats.rendered != formats.storage) { reason = "byte-order"; return false; }
        else if (!formats.equivalent) { reason = "format"; return false; }
    }
    VkFormatProperties properties{};
    context.formatProperties(context.physical, formats.storage, &properties);
    if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0) { reason = "blit-src"; return false; }
    filter = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    return true;
}

std::shared_ptr<Graphics::StorageTexture> PresentableResident(const Graphics::Context& context, const DisplayBuffer& buffer, VkFilter& filter, bool& copy, ResidentFormats& formats, bool& pending, const char*& reason) {
    auto resident = Graphics::StorageTexture::FindPending(buffer.address, DisplayBufferSize(buffer));
    pending = resident != nullptr;
    if (resident != nullptr && !ResidentPresentable(context, *resident, buffer, filter, copy, formats, reason)) resident.reset();
    return resident;
}

bool NoResidentPresent() {
    static const bool no = std::getenv("APS5_NO_RESIDENT_PRESENT") != nullptr;
    return no;
}

struct FrameDumps {
    int limit;
    bool cpu;
    int every = 1;
    int dumped = 0;
    std::uint64_t presents = 0;
};

FrameDumps& Dumps() {
    static FrameDumps dumps{[] {
        const char* value = std::getenv("APS5_DUMP_FRAMES");
        return value ? std::atoi(value) : 0;
    }(), std::getenv("APS5_NO_GPU_DUMP") != nullptr, [] {
        const char* value = std::getenv("APS5_DUMP_FRAMES_EVERY");
        return value ? std::max(std::atoi(value), 1) : 1;
    }()};
    return dumps;
}

}

bool VulkanDevice::PresentDisplayBuffer(const DisplayBuffer& buffer) {
    if (buffer.tilingMode == 1) {
        const auto pixels = ReadDisplayBuffer(buffer);
        return present(buffer.width, buffer.height, true, pixels);
    }
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;

    static std::uint64_t residentPresents = 0, residentCopies = 0, refreshedPresents = 0, notPending = 0, unsuitable = 0, gpuDumps = 0;
    static std::uint64_t unsuitableExtent = 0, unsuitableMips = 0, unsuitableBytes = 0, unsuitableFormat = 0, unsuitableByteOrder = 0, unsuitableUnattached = 0, unsuitableBlit = 0;
    static auto lastReport = std::chrono::steady_clock::now();
    const auto bytes = DisplayBufferSize(buffer);
    std::shared_ptr<Graphics::StorageTexture> resident;
    VkFilter filter = VK_FILTER_LINEAR;
    bool residentCopy = false;
    ResidentFormats formats;
    if (!NoResidentPresent()) {
        bool pending = false;
        const char* reason = nullptr;
        resident = PresentableResident(graphicsContext(), buffer, filter, residentCopy, formats, pending, reason);
        if (!pending) {
            ++notPending;
        } else if (resident == nullptr) {
            ++unsuitable;
            if (reason == std::string_view("extent")) ++unsuitableExtent;
            else if (reason == std::string_view("mips/layers")) ++unsuitableMips;
            else if (reason == std::string_view("bytes")) ++unsuitableBytes;
            else if (reason == std::string_view("format")) ++unsuitableFormat;
            else if (reason == std::string_view("byte-order")) ++unsuitableByteOrder;
            else if (reason == std::string_view("unattached")) ++unsuitableUnattached;
            else ++unsuitableBlit;

            static std::uint64_t detailed = 0;
            if (detailed < 12) {
                ++detailed;
                const auto found = Graphics::StorageTexture::FindPending(buffer.address, bytes);
                aps5::LogErr( "[flip] unsuitable (%s): display buffer 0x%llx %ux%u format 0x%016llx tiling %u pitch %u size %zu; image %ux%u mips %u layers %u depth %u guest format %u storage vk %d rendered vk %d display vk %d eq %d constants %d/%d guestBytes %zu\n",
                             reason ? reason : "?", static_cast<unsigned long long>(buffer.address), buffer.width, buffer.height, static_cast<unsigned long long>(buffer.pixelFormat), buffer.tilingMode, buffer.pitchInPixel, bytes,
                             found ? found->Descriptor().width : 0, found ? found->Descriptor().height : 0, found ? found->Descriptor().mipCount : 0,
                             found ? found->ImageLayers() : 0, found ? found->ImageDepth() : 0, found ? found->Descriptor().format : 0,
                             static_cast<int>(formats.storage), static_cast<int>(formats.rendered), static_cast<int>(formats.display), static_cast<int>(formats.equivalent),
                             static_cast<int>(VK_FORMAT_R8G8B8A8_UNORM), static_cast<int>(VK_FORMAT_B8G8R8A8_UNORM), found ? found->GuestBytes() : 0);
            }
        } else {

            static std::uint64_t named = 0;
            if (named < 6) {
                ++named;
                aps5::LogErr( "[flip] resident %s present from 0x%llx: display buffer format 0x%016llx, image guest format %u storage vk %d rendered vk %d display vk %d\n",
                             residentCopy ? "copy" : "blit",
                             static_cast<unsigned long long>(buffer.address), static_cast<unsigned long long>(buffer.pixelFormat),
                             resident->Descriptor().format, static_cast<int>(formats.storage), static_cast<int>(formats.rendered), static_cast<int>(formats.display));
            }

            GuestMemory::CollectWritesUncached(buffer.address, bytes);
            if (!GuestMemory::UnchangedSince(buffer.address, bytes, resident->Generation())) {
                resident->Refresh();
                ++refreshedPresents;
            }
            ++residentPresents;
            if (residentCopy) ++residentCopies;
        }
    }
    auto& dumps = Dumps();
    bool dumpFrame = false;

    static const std::uint64_t dumpFrom = [] { const char* text = std::getenv("APS5_DUMP_FROM"); return text ? std::strtoull(text, nullptr, 10) : 0ull; }();
    static const std::uint64_t dumpTo = [] { const char* text = std::getenv("APS5_DUMP_TO"); return text ? std::strtoull(text, nullptr, 10) : ~0ull; }();
    const bool inDumpRange = dumps.presents + 1 >= dumpFrom && dumps.presents + 1 <= dumpTo;
    if (dumps.dumped < dumps.limit && ++dumps.presents % static_cast<std::uint64_t>(dumps.every) == 0 && inDumpRange) {
        if (std::getenv("APS5_DUMP_FROM") != nullptr) aps5::LogErr("[dump] file=%d present=%llu collect=%llu\n", dumps.dumped, static_cast<unsigned long long>(dumps.presents), static_cast<unsigned long long>(GuestMemory::CurrentCollectFrame()));
        if (dumps.cpu) {
            const auto full = ReadDisplayBuffer(buffer);
            WriteFrameBmp(dumps.dumped++, buffer.width, buffer.height, full, DumpScale());
        } else {
            state->nextDumpIndex = dumps.dumped++;
            dumpFrame = true;
            ++gpuDumps;
        }
    }
    if (profile && std::chrono::steady_clock::now() - lastReport > std::chrono::seconds(10)) {
        lastReport = std::chrono::steady_clock::now();
        aps5::LogErr( "[flip] %llu presents from the resident image (%llu as texel copies, %llu refreshed first), through guest memory: %llu not pending, %llu unsuitable (extent %llu, mips/layers %llu, bytes %llu, format %llu, byte-order %llu, unattached %llu, blit-src %llu); %llu GPU frame dumps\n", static_cast<unsigned long long>(residentPresents), static_cast<unsigned long long>(residentCopies), static_cast<unsigned long long>(refreshedPresents), static_cast<unsigned long long>(notPending), static_cast<unsigned long long>(unsuitable), static_cast<unsigned long long>(unsuitableExtent), static_cast<unsigned long long>(unsuitableMips), static_cast<unsigned long long>(unsuitableBytes), static_cast<unsigned long long>(unsuitableFormat), static_cast<unsigned long long>(unsuitableByteOrder), static_cast<unsigned long long>(unsuitableUnattached), static_cast<unsigned long long>(unsuitableBlit), static_cast<unsigned long long>(gpuDumps));
    }
    CaptureTrace::Log("present dump=%d address=%llx width=%u height=%u resident=%d copy=%d generation=%llu", dumpFrame ? state->nextDumpIndex : -1, static_cast<unsigned long long>(buffer.address), buffer.width, buffer.height, resident != nullptr, residentCopy, static_cast<unsigned long long>(resident ? resident->Generation() : 0));
    if (!present(buffer.width, buffer.height, true, {}, &buffer, resident, filter, dumpFrame, residentCopy)) {

        if (dumpFrame) --dumps.dumped;
        return false;
    }
    return true;
}

bool VulkanDevice::AcquireImage() {
    PerformanceTimer timing("Vulkan.Acquire");
    require(state->swapchain != VK_NULL_HANDLE, "device has no swapchain");
    require(state->extent.width != 0 && state->extent.height != 0, "output window is minimized");
    require(!state->imageAcquired, "the previously acquired image was not presented");
    auto wait = state->DeviceFunction<PFN_vkWaitForFences>("vkWaitForFences");
    auto reset = state->DeviceFunction<PFN_vkResetFences>("vkResetFences");
    check(reset(state->device, 1, &state->acquireFence), "vkResetFences");
    APS5_LOG_CHARS_OUT_DEBUG("acquire fence reset");
    std::uint32_t index = 0;
    timing.Mark("fence_reset");

    const auto acquired = state->DeviceFunction<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR")(state->device, state->swapchain, 5'000'000'000ULL, VK_NULL_HANDLE, state->acquireFence, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        state->extent = {0, 0};
        return false;
    }
    if (acquired != VK_SUBOPTIMAL_KHR) check(acquired, "vkAcquireNextImageKHR");
    timing.Mark("acquire_image");
    APS5_LOG_OUT_DEBUG("vkAcquireNextImageKHR index=%u imageCount=%zu", index, state->images.size());
    check(wait(state->device, 1, &state->acquireFence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()), "vkWaitForFences acquire");
    timing.Mark("acquire_fence_wait");
    APS5_LOG_OUT_DEBUG("Acquire fence complete index=%u", index);
    require(index < state->images.size() && index < state->rendered.size(), "acquired image index is out of range");
    auto& rendered = state->rendered[index];
    if (rendered != VK_NULL_HANDLE) {
        state->DestroyRetiredSwapchains();
    } else {
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(state->DeviceFunction<PFN_vkCreateSemaphore>("vkCreateSemaphore")(state->device, &semaphore, nullptr, &rendered), "vkCreateSemaphore presentation");
    }
    timing.Mark("retired_swapchains");
    state->acquiredIndex = index;
    state->imageAcquired = true;
    return true;
}

bool VulkanDevice::present(std::uint32_t width, std::uint32_t height, bool opaque, std::span<const std::byte> pixels, const DisplayBuffer* display, const std::shared_ptr<Graphics::StorageTexture>& resident, VkFilter residentFilter, bool dumpFrame, bool residentCopy) {
    PerformanceTimer timing("Vulkan.Present");
    APS5_LOG_OUT_DEBUG("present begin width=%u height=%u opaque=%u pixels=%zu", width, height, static_cast<unsigned>(opaque), pixels.size());
    require(state->swapchain != VK_NULL_HANDLE, "device has no swapchain");
    require(state->extent.width != 0 && state->extent.height != 0, "output window is minimized");
    require(!state->queuePending, "the previous presentation was not queued");
    require(!state->presentSlots.empty(), "device has no presentation slots");

    if (!state->imageAcquired && !AcquireImage()) return false;
    state->imageAcquired = false;
    const auto index = state->acquiredIndex;
    require(index < state->images.size() && index < state->rendered.size(), "acquired image index is out of range");
    auto& rendered = state->rendered[index];
    const bool clearOnly = pixels.empty() && display == nullptr;

    const bool direct = resident != nullptr && !residentCopy;

    const bool shared = !clearOnly && !direct;
    if ((shared || (!clearOnly && (state->scaler == nullptr || state->scaler->SourceWidth() != width || state->scaler->SourceHeight() != height))) && state->SharedSlotInFlight()) RetirePresents(0);

    if (state->presentSlots[state->presentCursor].inFlight) RetirePresents(state->presentSlots.size() - 1);
    auto& slot = state->presentSlots[state->presentCursor];
    require(!slot.inFlight, "presentation slot is still in flight");
    if (display != nullptr && resident == nullptr) {
        static_cast<void>(DisplayBufferSize(*display));
        state->colorTransfer->Upload(display->address, width, height, Graphics::ColorTileMode::RenderTarget);
    }
    if (!pixels.empty()) state->Upload(pixels);

    SubmitRecorded(false);
    std::chrono::steady_clock::time_point lastSubmittedAt{};
    const std::uint64_t batchesAtBlit = state->recorder ? state->recorder->NewestSubmitted(&lastSubmittedAt) : 0;
    CaptureTrace::Log("blit dump=%d batch=%llu slot=%zu image=%u direct=%d", dumpFrame ? state->nextDumpIndex : -1, static_cast<unsigned long long>(batchesAtBlit), state->presentCursor, index, direct);
    timing.Mark("pixel_upload");
    APS5_LOG_OUT_DEBUG("present source=%s bytes=%zu", pixels.empty() ? "clear" : "pixels", pixels.size());
    auto commands = slot.commands;
    check(state->DeviceFunction<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(commands, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(state->DeviceFunction<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin), "vkBeginCommandBuffer");
    APS5_LOG_OUT_DEBUG("Presentation command buffer begin commands=%p image=%p", reinterpret_cast<void*>(commands), reinterpret_cast<void*>(state->images[index]));
    using CommandClass = Graphics::Recorder::CommandClass;
    Graphics::Recorder::CountPresent();
    if (Graphics::Recorder::GpuTimingEnabled()) {
        if (slot.queries == VK_NULL_HANDLE) {
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = 2;
            if (state->DeviceFunction<PFN_vkCreateQueryPool>("vkCreateQueryPool")(state->device, &info, nullptr, &slot.queries) != VK_SUCCESS) slot.queries = VK_NULL_HANDLE;
        }
        if (slot.queries != VK_NULL_HANDLE) {
            state->DeviceFunction<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(commands, slot.queries, 0, 2);
            state->DeviceFunction<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.queries, 0);
        }
        slot.blitBytes = static_cast<std::uint64_t>(state->extent.width) * state->extent.height * 4;
    }
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = state->images[index];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    auto pipelineBarrier = state->DeviceFunction<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    pipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    Graphics::Recorder::CountBarriers(CommandClass::PresentBlit);
    if (pixels.empty() && display == nullptr) {
        APS5_LOG_OUT_DEBUG("Recording swapchain clear opaque=%u image=%p", static_cast<unsigned>(opaque), reinterpret_cast<void*>(barrier.image));
        VkClearColorValue clear{};
        clear.float32[3] = opaque ? 1.0f : 0.0f;
        state->DeviceFunction<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &barrier.subresourceRange);
    } else {
        APS5_LOG_OUT_DEBUG("Recording swapchain scaled blit width=%u height=%u bytes=%zu buffer=%p image=%p", width, height, pixels.size(), reinterpret_cast<void*>(state->uploadBuffer), reinterpret_cast<void*>(barrier.image));
        require(state->scaler != nullptr, "presentation scaler is unavailable");
        state->scaler->EnsureSourceImage(width, height);
        VkImageMemoryBarrier residentBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        if (resident != nullptr) {

            residentBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            residentBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            residentBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            residentBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            residentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            residentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            residentBarrier.image = resident->Image();
            residentBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            pipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &residentBarrier);
            Graphics::Recorder::CountBarriers(CommandClass::PresentBlit);

            if (!direct) state->scaler->RecordCopyInto(commands, resident->Image(), VK_IMAGE_LAYOUT_GENERAL);
        } else {
            if (display != nullptr) {

                constexpr std::uint64_t tenBitFormat = 0x0100000000000000ull;
                const bool tenBit = (display->pixelFormat & tenBitFormat) != 0;
                state->colorTransfer->Detile(commands, (display->pixelFormat & ~tenBitFormat) == 0x8000000022000000ull, tenBit);
            }
            state->scaler->RecordUpload(commands, display != nullptr ? state->colorTransfer->LinearBuffer() : state->uploadBuffer);
        }
        VkClearColorValue letterbox{};
        letterbox.float32[3] = 1.0f;
        state->DeviceFunction<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &letterbox, 1, &barrier.subresourceRange);
        VkImageMemoryBarrier letterboxBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        letterboxBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        letterboxBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        letterboxBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        letterboxBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        letterboxBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        letterboxBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        letterboxBarrier.image = barrier.image;
        letterboxBarrier.subresourceRange = barrier.subresourceRange;
        pipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &letterboxBarrier);
        Graphics::Recorder::CountBarriers(CommandClass::PresentBlit);
        if (direct) PresentationScaler::RecordBlitFrom(graphicsContext(), commands, resident->Image(), VK_IMAGE_LAYOUT_GENERAL, width, height, residentFilter, barrier.image, state->extent.width, state->extent.height);
        else state->scaler->RecordBlit(commands, barrier.image, state->extent.width, state->extent.height);
        if (dumpFrame && direct) {
            const auto scale = DumpScale();
            slot.dumpWidth = (width + scale - 1) / scale;
            slot.dumpHeight = (height + scale - 1) / scale;
            slot.dumpScale = 1;
            if (!slot.dumpScaler) slot.dumpScaler = std::make_unique<PresentationScaler>(graphicsContext(), VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM);
            slot.dumpScaler->EnsureSourceImage(slot.dumpWidth, slot.dumpHeight);
            slot.dumpScaler->RecordBlitInto(commands, resident->Image(), VK_IMAGE_LAYOUT_GENERAL, VK_FILTER_NEAREST, width, height);
        }
        if (resident != nullptr) {

            residentBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            residentBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            pipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &residentBarrier);
            Graphics::Recorder::CountBarriers(CommandClass::PresentBlit);
        }
        if (dumpFrame) {
            if (!direct) {
                slot.dumpWidth = state->scaler->SourceWidth();
                slot.dumpHeight = state->scaler->SourceHeight();
                slot.dumpScale = DumpScale();
            }
            const auto dumpBytes = static_cast<std::size_t>(slot.dumpWidth) * slot.dumpHeight * 4;
            if (!slot.dumpBuffer || slot.dumpBuffer->Bytes().size() != dumpBytes) slot.dumpBuffer = std::make_unique<Graphics::Buffer>(graphicsContext(), dumpBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            (direct ? slot.dumpScaler : state->scaler)->RecordReadback(commands, slot.dumpBuffer->Handle());
        }
    }
    const PresentationOverlayFrame overlayFrame{commands, barrier.image, state->extent, VK_FORMAT_B8G8R8A8_UNORM, state->swapchainSerial, state->instance, state->physical, state->device, state->queue, state->queueFamily, state->instanceProc, state->deviceProc};
    const bool overlayDrawn = state->overlay != nullptr && state->overlayUsable && state->overlay->Wanted(overlayFrame);
    if (overlayDrawn) {
        VkImageMemoryBarrier toAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toAttachment.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toAttachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toAttachment.image = barrier.image;
        toAttachment.subresourceRange = barrier.subresourceRange;
        pipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toAttachment);
        Graphics::Recorder::CountBarriers(CommandClass::PresentBlit);
        state->overlay->Record(overlayFrame);
    }
    barrier.srcAccessMask = overlayDrawn ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = overlayDrawn ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pipelineBarrier(commands, overlayDrawn ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    Graphics::Recorder::CountBarriers(CommandClass::PresentBlit);
    if (slot.queries != VK_NULL_HANDLE) state->DeviceFunction<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.queries, 1);
    check(state->DeviceFunction<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands), "vkEndCommandBuffer");
    APS5_LOG_CHARS_OUT_DEBUG("Presentation command buffer recorded");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &rendered;
    timing.Mark("command_record_scale");

    check(state->DeviceFunction<PFN_vkResetFences>("vkResetFences")(state->device, 1, &slot.fence), "vkResetFences present");
    check(state->DeviceFunction<PFN_vkQueueSubmit>("vkQueueSubmit")(state->queue, 1, &submit, slot.fence), "vkQueueSubmit clear");
    const auto submittedAt = std::chrono::steady_clock::now();
    timing.Mark("queue_submit");
    APS5_LOG_CHARS_OUT_DEBUG("Presentation vkQueueSubmit OK");
    {
        std::lock_guard lock(state->presentMutex);

        slot.kept = resident;
        slot.imageIndex = index;
        slot.inFlight = true;
        slot.shared = shared;

        slot.dumpRecorded = dumpFrame;
        slot.dumpIndex = state->nextDumpIndex;
        slot.recorderSerial = batchesAtBlit;
        slot.previousSerial = state->lastPresentSerial;
        slot.submittedAt = submittedAt;
        state->lastPresentSerial = batchesAtBlit;
        state->presentCursor = (state->presentCursor + 1) % state->presentSlots.size();
        state->queuePending = true;
        state->queueIndex = index;
        ++presentCounters.presents;
        if (auto* frame = PerformanceContext::Current()) {
            frame->NoteBlit(batchesAtBlit, lastSubmittedAt, submittedAt);

            const auto atFlip = frame->BatchesAtFlip();
            if (atFlip != 0 && batchesAtBlit >= atFlip) presentCounters.batchesAheadOfBlit += batchesAtBlit - atFlip;
            if (frame->FlipReached() != std::chrono::steady_clock::time_point{}) {
                presentCounters.blitSubmitAfterFlipMs += std::chrono::duration<double, std::milli>(submittedAt - frame->FlipReached()).count();
                presentCounters.lastSubmitAfterFlipMs += std::chrono::duration<double, std::milli>(std::max(lastSubmittedAt, submittedAt) - frame->FlipReached()).count();
            }
        }
    }
    return true;
}

std::size_t VulkanDevice::FlipInFlight() {
    static const std::size_t count = [] {
        if (std::getenv("APS5_SYNC_FLIP") != nullptr) return std::size_t{0};
        const char* value = std::getenv("APS5_FLIP_INFLIGHT");
        if (value == nullptr) return std::size_t{1};
        const long parsed = std::strtol(value, nullptr, 10);
        if (parsed >= 0 && parsed <= 2) return static_cast<std::size_t>(parsed);
        aps5::LogErr( "[present] APS5_FLIP_INFLIGHT=%s refused (0, 1 or 2 presentations may trail on the GPU); using 1\n", value);
        return std::size_t{1};
    }();
    return count;
}

VulkanDevice::PresentStatistics VulkanDevice::PresentCounts() {
    return presentCounters;
}

bool VulkanDevice::PresentWaitsForSlots(const DisplayBuffer* buffer) const {
    if (buffer == nullptr || !state->SharedSlotInFlight()) return false;
    if (buffer->tilingMode == 1) return true;
    if (NoResidentPresent()) return true;
    VkFilter filter = VK_FILTER_LINEAR;
    bool copy = false, pending = false;
    ResidentFormats formats;
    const char* reason = nullptr;
    if (PresentableResident(graphicsContext(), *buffer, filter, copy, formats, pending, reason) == nullptr) return true;

    if (copy) return true;
    return state->scaler == nullptr || state->scaler->SourceWidth() != buffer->width || state->scaler->SourceHeight() != buffer->height;
}

void VulkanDevice::FlipBatches(std::uint64_t& submissions, std::uint64_t& unsignaled) const {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    submissions = state->recorder ? state->recorder->Submissions() : 0;
    unsignaled = profile && state->recorder ? state->recorder->UnsignaledBatches() : 0;
}

double VulkanDevice::RetirePresents(std::size_t keepInFlight) {
    auto& slots = state->presentSlots;
    if (slots.empty()) return 0;
    PerformanceTimer timing("Vulkan.Retire");
    std::lock_guard lock(state->presentMutex);
    const auto status = state->DeviceFunction<PFN_vkGetFenceStatus>("vkGetFenceStatus");
    auto inFlight = static_cast<std::size_t>(std::count_if(slots.begin(), slots.end(), [](const auto& slot) { return slot.inFlight; }));
    double waitedMs = 0;

    for (std::size_t i = 0; i < slots.size() && inFlight != 0; ++i) {
        auto& slot = slots[(state->presentCursor + i) % slots.size()];
        if (!slot.inFlight) continue;
        if (inFlight <= keepInFlight) {
            const auto result = status(state->device, slot.fence);
            if (result == VK_NOT_READY) break;
            check(result, "vkGetFenceStatus present");
            timing.Mark("poll");
        } else {
            const auto start = std::chrono::steady_clock::now();
            state->WaitPresentFence(slot.fence);
            waitedMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            timing.Mark("wait");
        }
        const bool dump = slot.dumpRecorded;
        state->RetireSlot(slot);
        if (dump) timing.Mark("dump");
        --inFlight;
    }
    return waitedMs;
}

double VulkanDevice::FinishPresent() {
    return RetirePresents(0);
}

void VulkanDevice::QueuePresent() {
    if (!state->queuePending) return;
    PerformanceTimer timing("Vulkan.QueuePresent");
    state->queuePending = false;
    const auto index = state->queueIndex;
    require(index < state->rendered.size(), "presented image index is out of range");
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &state->rendered[index];
    present.swapchainCount = 1;
    present.pSwapchains = &state->swapchain;
    present.pImageIndices = &index;
    const auto presented = state->DeviceFunction<PFN_vkQueuePresentKHR>("vkQueuePresentKHR")(state->queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) state->extent = {0, 0};
    else check(presented, "vkQueuePresentKHR");
    timing.Mark("queue_present");
    APS5_LOG_OUT_DEBUG("vkQueuePresentKHR queued imageIndex=%u", index);
}

ShaderRecompiler::SpirvTarget VulkanDevice::ComputeTarget(std::uint32_t waveSize) const {
    auto target = Target();
    if (waveSize == 32u && state->computeWave32) target.subgroupSize = 32u;
    return target;
}

ShaderRecompiler::SpirvTarget VulkanDevice::Target() const {
    const auto& limits = state->properties.limits;
    ShaderRecompiler::SpirvTarget target{VK_API_VERSION_1_1, state->meshShader ? 0x00010400u : 0x00010300u, state->subgroup.subgroupSize, ShaderRecompiler::BdaAbi::Version, state->capabilities, state->spirvExtensions, false, {limits.maxComputeWorkGroupSize[0], limits.maxComputeWorkGroupSize[1], limits.maxComputeWorkGroupSize[2]}, limits.maxComputeWorkGroupInvocations, limits.maxComputeSharedMemorySize, {}, {}};
    target.fragmentShaderBarycentricEnabled = state->fragmentShaderBarycentric;
    target.nonConstantImageOffsets = state->maintenance8;
    if (state->meshShader) {
        const auto& mesh = state->meshLimits;
        target.mesh = ShaderRecompiler::MeshTargetLimits{{mesh.maxMeshWorkGroupSize[0], mesh.maxMeshWorkGroupSize[1], mesh.maxMeshWorkGroupSize[2]}, mesh.maxMeshWorkGroupInvocations, std::min(mesh.maxMeshSharedMemorySize, mesh.maxMeshPayloadAndSharedMemorySize), mesh.maxMeshOutputVertices, mesh.maxMeshOutputPrimitives, mesh.maxMeshOutputComponents, std::min(mesh.maxMeshOutputMemorySize, mesh.maxMeshPayloadAndOutputMemorySize), mesh.meshOutputPerVertexGranularity, mesh.meshOutputPerPrimitiveGranularity};
    }
    if (state->tessellationShader) target.tessellation = ShaderRecompiler::TessellationTargetLimits{limits.maxTessellationPatchSize, limits.maxTessellationControlPerVertexInputComponents, limits.maxTessellationControlPerVertexOutputComponents, limits.maxTessellationControlPerPatchOutputComponents, limits.maxTessellationControlTotalOutputComponents, limits.maxTessellationEvaluationInputComponents, limits.maxTessellationEvaluationOutputComponents};
    return target;
}

bool VulkanDevice::PrimitiveListRestart() const {
    return state->primitiveListRestart;
}

Graphics::Context VulkanDevice::graphicsContext() const {
    static const bool noCache = std::getenv("APS5_NO_CONTEXT_CACHE") != nullptr;
    if (state->contextReady && !noCache) return state->context;
    return buildContext();
}

Graphics::Context VulkanDevice::buildContext() const {
    auto context = Graphics::Context{
        state->device,
        state->physical,
        state->queue,
        state->pool,
        state->deviceProc,
        state->InstanceFunction<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties"),
        state->InstanceFunction<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties"),
        state->memoryProperties,
        state->properties.limits,
        state->tessellationShader,
        state->meshShader,
        state->meshLimits,
        state->depthClipControl,
        state->depthRangeUnrestricted,
        true,
        state->subgroup,
        state->fragmentShaderBarycentric,
        state->samplerAnisotropy,
        state->textureCompressionBC,
        state->detiler.get(),
        state->colorTransfer.get(),
        state->bufferPool,
        state->textureCache.get(),
        state->pipelineCache ? state->pipelineCache->Handle() : VK_NULL_HANDLE,
        state->depthClamp,
        state->hostImportAlignment
    };
    context.recorder = state->recorder.get();
    context.descriptorCache = state->descriptorCache.get();
    context.samplerCache = state->samplerCache.get();
    context.drawIndirectFirstInstance = state->drawIndirectFirstInstance;
    context.multiDrawIndirect = state->multiDrawIndirect;
    context.depthBounds = state->depthBounds;
    context.depthBiasClamp = state->depthBiasClamp;
    context.drawIndirectCount = state->drawIndirectCount;
    context.occlusionQueryPrecise = state->occlusionQueryPrecise;
    context.emptyBuffer = state->emptyBuffer ? state->emptyBuffer->Handle() : VK_NULL_HANDLE;
    context.copiedWriters = state->copiedWriters.get();
    context.functions = state->functionsReady ? &state->deviceFunctions : nullptr;
    context.descriptorIndexing = state->descriptorIndexing;
    context.primitiveListRestart = state->primitiveListRestart;
    context.imageViewMinLod = state->imageViewMinLod;
    return context;
}

VulkanDevice::IndirectDrawSupport VulkanDevice::DrawIndirectSupport() const {
    return {state->drawIndirectFirstInstance, state->multiDrawIndirect, state->drawIndirectCount};
}

std::optional<std::string> VulkanDevice::KnownDrawRejection(const Graphics::State& graphics, std::span<const Graphics::CompiledShader> shaders) const {
    return Graphics::KnownValidationFailure(graphicsContext(), shaders, graphics);
}

void VulkanDevice::ColorMetadataPass(const Graphics::ColorMetadataPass& pass) {
    Graphics::RunColorMetadataPass(graphicsContext(), pass);
}

void VulkanDevice::Draw(const Graphics::State& graphics, const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> shaders, std::span<const Graphics::GuestMemorySnapshot> snapshots, std::shared_ptr<const DrawRecipe>* recipe) {

    static const bool trace = std::getenv("APS5_TRACE_DRAWS") != nullptr;
    if (trace) APS5_LOG_OUT("VulkanDevice::Draw indices=%u instances=%u indexSize=%u address=0x%llx shaders=%zu colorTarget=%u", draw.indexCount, draw.instanceCount, draw.indexSize, static_cast<unsigned long long>(draw.indexAddress), shaders.size(), static_cast<unsigned>(graphics.hasColorTarget));
    const auto context = graphicsContext();
    Graphics::Draw(context, graphics, draw, shaders, snapshots, recipe);
    if (trace) APS5_LOG_CHARS_OUT("VulkanDevice::Draw complete");
}

namespace {

bool ResourceCacheEnabled() {
    static const bool noResourceCache = std::getenv("APS5_NO_RESOURCE_CACHE") != nullptr;
    static const bool noTextureCache = std::getenv("APS5_NO_TEXTURE_CACHE") != nullptr;
    return !noResourceCache && !noTextureCache;
}

bool TemplateDataRefresh() {
    static const bool enabled = std::getenv("APS5_NO_TEMPLATE_DATA_REFRESH") == nullptr;
    return enabled;
}

ResourceCache::Key DispatchContentKey(const Graphics::CompiledShader& shader, VkDevice device) {
    auto key = Graphics::ShaderResources::ContentKey(shader, !TemplateDataRefresh());
    const auto deviceHandle = reinterpret_cast<std::uint64_t>(device);
    key.push_back(static_cast<std::uint32_t>(deviceHandle));
    key.push_back(static_cast<std::uint32_t>(deviceHandle >> 32u));
    return key;
}

enum DispatchPhase : std::size_t { PhaseReap, PhaseResources, PhaseResourcesInsert, PhaseResourcesComplete, PhaseResourcesImages, PhaseResourcesUpload, PhaseResourcesDescriptors, PhaseResourcesOther, PhaseResourcesALocked, PhaseResourcesAddress, PhaseResourcesAUnlocked, PhaseResourcesRevalidate, PhaseResourcesFullBuild, PhaseResourcesHookWaits, PhaseProof, PhasePipeline, PhaseDecide, PhaseArgumentRead, PhaseRecordCommands, PhaseRecordKeeps, PhaseRecordDataRefresh, PhaseRecordBind, PhaseRecordMarks, PhaseRecordCompletion, PhaseRecord, PhaseSync, DispatchPhaseCount };
constexpr std::array<const char*, DispatchPhaseCount> DispatchPhaseNames{"reap", "resources", "resources: cache insert", "resources: complete (wall)", "resources: B images", "resources: B upload", "resources: B descriptors", "resources: B bda+other", "resources: A locked (bda)", "resources: address bindings (bda)", "resources A (unlocked)", "resources: revalidate", "resources: full build (locked)", "resources: hook waits", "proof", "pipeline", "decide", "argument read", "record: commands", "record: keeps", "record: data refresh", "record: bind+dispatch", "record: marks", "record: completion", "record", "sync"};
constexpr std::size_t DispatchRows = static_cast<std::size_t>(DispatchPhaseCount) + static_cast<std::size_t>(Graphics::LookupOutcomes::Count);

const char* DispatchRowName(std::size_t row) {
    if (row < DispatchPhaseCount) return DispatchPhaseNames[row];
    static const std::array<std::string, Graphics::LookupOutcomes::Count> imageRows = [] {
        std::array<std::string, Graphics::LookupOutcomes::Count> rows;
        for (std::size_t kind = 0; kind < Graphics::LookupOutcomes::Count; ++kind) rows[kind] = std::string("images: ") + Graphics::LookupOutcomes::Name(static_cast<Graphics::LookupOutcomes::Kind>(kind));
        return rows;
    }();
    return imageRows[row - DispatchPhaseCount].c_str();
}

}

struct PreparedDispatch {

    std::shared_ptr<Graphics::ShaderResources> resources;
    std::shared_ptr<Graphics::ShaderResources> cached;
    ResourceCache::Key key;

    double prepareMs = 0;
    enum PreparePhase : std::size_t { PrepareKey, PrepareFind, PreparePrecollect, PreparePresync, PrepareStageA, PreparePhaseCount };
    std::array<double, PreparePhaseCount> phaseMs{};

    std::uint64_t presyncSerial = 0;
};

struct RecipeHit {
    std::shared_ptr<const Recipe> recipe;

    std::shared_ptr<Graphics::ShaderResources> resources;
    std::shared_ptr<ComputePipelineObjects> objects;

    std::uint64_t presyncSerial = 0;
    bool indirect = false;

    double precheckMs = 0;
};

namespace {

struct PresyncCounters {
    std::atomic<std::uint64_t> checked{0};
    std::atomic<std::uint64_t> presyncs{0};
    std::atomic<std::uint64_t> waitedUs{0};
    std::atomic<std::int64_t> lastReport{0};
};

PresyncCounters& Presyncs() {
    static PresyncCounters counters;
    return counters;
}

bool CachedPrecollect() {
    static const bool cachedPrecollect = std::getenv("APS5_NO_CACHED_PRECOLLECT") == nullptr;
    return cachedPrecollect;
}

bool NoPresync() {
    static const bool noPresync = std::getenv("APS5_NO_PRESYNC") != nullptr;
    return noPresync;
}

bool SyncEachDispatch() {
    static const bool syncEachDispatch = std::getenv("APS5_SYNC_DISPATCH") != nullptr;
    return syncEachDispatch;
}

bool TraceDispatchIo() {
    static const bool traceIo = std::getenv("APS5_TRACE_DISPATCH_IO") != nullptr;
    return traceIo;
}

void WatchMemory(std::uint64_t programAddress) {
    struct Range {
        std::uint64_t address = 0;
        std::uint64_t bytes = 0;
    };
    static const std::vector<Range> ranges = [] {
        std::vector<Range> parsed;
        const char* text = std::getenv("APS5_WATCH_MEMORY");
        if (text == nullptr) return parsed;
        while (true) {
            char* end = nullptr;
            const auto address = std::strtoull(text, &end, 16);
            const auto bytes = *end == ':' ? std::strtoull(end + 1, &end, 16) : 0ull;
            if (address == 0 || bytes == 0 || (*end != ',' && *end != '\0')) throw std::runtime_error("APS5_WATCH_MEMORY takes <hex address>:<hex bytes>[,...]");
            parsed.push_back({address, bytes});
            if (*end == '\0') break;
            text = end + 1;
        }
        if (!SyncEachDispatch()) throw std::runtime_error("APS5_WATCH_MEMORY needs APS5_SYNC_DISPATCH=1");
        return parsed;
    }();
    if (ranges.empty()) return;
    static std::mutex mutex;
    static std::deque<std::pair<std::uint64_t, std::vector<std::uint8_t>>> history;
    static bool written = false;
    std::lock_guard lock(mutex);
    if (written) return;
    static std::vector<std::uint64_t> addresses;
    if (addresses.empty()) {
        std::vector<std::uint64_t> found;
        for (const auto& range : ranges) {
            std::uint64_t address = 0;
            for (std::uint64_t high = 0; high < 16 && address == 0; ++high) {
                const auto candidate = (range.address & 0xfffffffffull) | (high << 36u);
                if (GuestMemory::Accessible(reinterpret_cast<const void*>(candidate), static_cast<std::size_t>(range.bytes))) address = candidate;
            }
            if (address == 0) return;
            found.push_back(address);
        }
        addresses = std::move(found);
        for (std::size_t i = 0; i < ranges.size(); ++i) aps5::LogErr( "[watch] watching 0x%llx+0x%llx\n", static_cast<unsigned long long>(addresses[i]), static_cast<unsigned long long>(ranges[i].bytes));
    }
    std::vector<std::uint8_t> contents;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(addresses[i]);
        if (!GuestMemory::Accessible(bytes, static_cast<std::size_t>(ranges[i].bytes))) return;
        contents.insert(contents.end(), bytes, bytes + ranges[i].bytes);
    }
    if (history.empty() || history.back().second != contents) {
        history.emplace_back(programAddress, std::move(contents));
        if (history.size() > 64) history.pop_front();
    }
    if (!Graphics::LoopGuardTripped()) return;
    written = true;
    for (std::size_t i = 0; i < history.size(); ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "watch_%02zu_%llx.bin", i, static_cast<unsigned long long>(history[i].first));
        std::ofstream(name, std::ios::binary).write(reinterpret_cast<const char*>(history[i].second.data()), static_cast<std::streamsize>(history[i].second.size()));
    }
    aps5::LogErr( "[watch] loop guard tripped: %zu contents written (watch_*.bin, oldest first)\n", history.size());
}

struct IndirectHold {
    std::array<double, DispatchRows> phaseMs{};

    std::array<std::uint64_t, DispatchRows> phaseCounts{};
    std::uint64_t count = 0;
    double totalMs = 0;
    double maxMs = 0;

    std::string maxPhases;
    std::uint64_t writersScanned = 0;
    std::uint64_t drawWritersScanned = 0;
    std::size_t maxWriters = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

struct DispatchCounters {
    std::array<double, DispatchRows> phaseTotals{};
    std::uint64_t profiledDispatches = 0;
    IndirectHold indirectHold;
    std::uint64_t cacheHits = 0, cacheMisses = 0, cacheInvalidated = 0, templateRefreshed = 0, templateSameWords = 0;
    double templateRevalidateMs = 0;
    std::uint64_t procLookups = 0, preBarriersRecorded = 0, preBarriersSkipped = 0;
    std::chrono::steady_clock::time_point cacheReport = std::chrono::steady_clock::now();
};

DispatchCounters& Dispatches() {
    static DispatchCounters counters;
    return counters;
}

enum RecipeMiss : std::size_t { MissNoRecipe, MissDevice, MissTemplateGone, MissObjectsGone, MissNotRecordable, RecipeMissCount };
constexpr std::array<const char*, RecipeMissCount> RecipeMissNames{"no recipe", "device", "template gone", "objects gone", "not recordable"};

constexpr std::array<const char*, RecipeMissCount> DrawRecipeMissNames{"no recipe", "device", "template gone", "targets gone", "not recordable"};
constexpr std::array<const char*, static_cast<std::size_t>(Graphics::ShaderResources::ProofPath::Count)> ProofPathNames{"fast", "T1 refreshed", "full"};
constexpr std::array<const char*, static_cast<std::size_t>(Graphics::ShaderResources::ProofFailure::Count)> ProofFailureNames{"none", "imports", "evicted", "pending", "changed", "keys", "other"};

struct RecipeCounters {
    struct Kind {
        std::atomic<std::uint64_t> hits{0};
        std::array<std::atomic<std::uint64_t>, RecipeMissCount> precheckMisses{};
        std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Graphics::ShaderResources::ProofPath::Count)> proofPaths{};
        std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Graphics::ShaderResources::ProofFailure::Count)> rebuilds{};
        std::atomic<std::uint64_t> rebuildDevice{0};
        std::atomic<std::uint64_t> presyncsFromRecipe{0}, presyncsRecomputed{0}, presyncWaits{0};
        std::atomic<std::uint64_t> dataRefreshed{0}, dataSkipped{0}, refreshByWords{0};
        std::atomic<std::uint64_t> restarts{0}, attaches{0}, verified{0}, verifyReplaced{0};
        std::atomic<std::uint64_t> precheckNs{0}, proofNs{0}, recordNs{0};
    };
    std::array<Kind, 3> kinds;
    std::atomic<std::int64_t> lastReport{0};
};

RecipeCounters& Recipes() {
    static RecipeCounters counters;
    return counters;
}

void reportRecipes() {
    auto& recipes = Recipes();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = recipes.lastReport.load();
    if (nowMs - last < 10000 || !recipes.lastReport.compare_exchange_strong(last, nowMs)) return;
    const auto take = [](std::atomic<std::uint64_t>& counter) { return static_cast<unsigned long long>(counter.exchange(0, std::memory_order_relaxed)); };
    for (std::size_t kind = 0; kind < 3; ++kind) {
        auto& counters = recipes.kinds[kind];
        const auto hits = take(counters.hits);
        std::string misses, proofs, rebuilds;
        char text[64];
        for (std::size_t i = 0; i < RecipeMissCount; ++i) {
            std::snprintf(text, sizeof(text), "%s%s %llu", i == 0 ? "" : ", ", (kind == 2 ? DrawRecipeMissNames : RecipeMissNames)[i], take(counters.precheckMisses[i]));
            misses += text;
        }
        for (std::size_t i = 0; i < ProofPathNames.size(); ++i) {
            std::snprintf(text, sizeof(text), "%s%s %llu", i == 0 ? "" : ", ", ProofPathNames[i], take(counters.proofPaths[i]));
            proofs += text;
        }
        for (std::size_t i = 1; i < ProofFailureNames.size(); ++i) {
            std::snprintf(text, sizeof(text), "%s%s %llu", i == 1 ? "" : ", ", ProofFailureNames[i], take(counters.rebuilds[i]));
            rebuilds += text;
        }
        std::snprintf(text, sizeof(text), ", device %llu", take(counters.rebuildDevice));
        rebuilds += text;
        const auto perHit = [&](std::atomic<std::uint64_t>& ns) { return hits != 0 ? static_cast<double>(take(ns)) / 1000.0 / static_cast<double>(hits) : 0.0; };
        const auto precheckUs = perHit(counters.precheckNs);
        const auto proofUs = perHit(counters.proofNs);
        const auto recordUs = perHit(counters.recordNs);
        aps5::LogErr( "[recipe] %s (10 s): hits %llu; pre-check misses: %s; proofs: %s; rebuilds: %s; presyncs from the recipe %llu (recomputed %llu, waited %llu); data refreshes recorded %llu / skipped by hash %llu (%llu decided by words: data hits); restarts %llu, attaches %llu; us per hit: pre-check %.1f, proof %.1f, record %.1f; verified %llu (object replaced %llu)\n", kind == 0 ? "dispatch" : kind == 1 ? "indirect" : "draw", hits, misses.c_str(), proofs.c_str(), rebuilds.c_str(), take(counters.presyncsFromRecipe), take(counters.presyncsRecomputed), take(counters.presyncWaits), take(counters.dataRefreshed), take(counters.dataSkipped), take(counters.refreshByWords), take(counters.restarts), take(counters.attaches), precheckUs, proofUs, recordUs, take(counters.verified), take(counters.verifyReplaced));
    }
}

struct DispatchTimer {
    bool profile;
    bool indirect;
    std::size_t spirvWords;
    std::uint64_t programAddress;
    std::array<double, DispatchRows> callMs{};
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point phaseStart;
    DispatchTimer(bool profile, bool indirect, std::size_t spirvWords, std::uint64_t programAddress) : profile(profile), indirect(indirect), spirvWords(spirvWords), programAddress(programAddress), start(std::chrono::steady_clock::now()), phaseStart(start) {}
    void add(DispatchPhase which, double ms) {
        if (!profile) return;
        auto& d = Dispatches();
        callMs[which] += ms;
        d.phaseTotals[which] += ms;
        if (indirect) d.indirectHold.phaseMs[which] += ms;
    }
    void phase(DispatchPhase which) {
        if (!profile) return;
        const auto now = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration<double, std::milli>(now - phaseStart).count();
        add(which, ms);
        phaseStart = now;

        if (ms > 1000) aps5::LogErr( "[dispatch] %s took %.0f ms (%zu words, program 0x%llx)\n", DispatchRowName(which), ms, spirvWords, static_cast<unsigned long long>(programAddress));
    }
    void restart() { phaseStart = std::chrono::steady_clock::now(); }

    std::string formatCall() const {
        std::string text;
        char row[96];
        for (std::size_t i = 0; i < DispatchRows; ++i) {
            if (callMs[i] == 0) continue;
            std::snprintf(row, sizeof(row), " %s=%.2fms", DispatchRowName(i), callMs[i]);
            text += row;
        }
        return text;
    }

    static void countDispatch(bool profile) {
        if (!profile) return;
        auto& d = Dispatches();
        if (++d.profiledDispatches % 1000 != 0) return;
        std::string report;
        for (std::size_t row = 0; row < DispatchPhaseCount; ++row) {
            if (d.phaseTotals[row] != 0) report += " " + std::string(DispatchRowName(row)) + "=" + std::to_string(static_cast<long long>(d.phaseTotals[row] / 1000)) + "s";
        }
        aps5::LogErr( "[dispatch] %llu dispatches, phase totals:%s\n", static_cast<unsigned long long>(d.profiledDispatches), report.c_str());
    }

    void finish(std::uint64_t lookupsBefore, const char* groupsText, bool indirectHold) const {
        if (!profile) return;
        auto& d = Dispatches();
        d.procLookups += Graphics::DeviceProcLookups() - lookupsBefore;
        const auto now = std::chrono::steady_clock::now();
        const auto totalMs = std::chrono::duration<double, std::milli>(now - start).count();
        if (totalMs > 100) aps5::LogErr( "[dispatch] %s %zu words:%s\n", groupsText, spirvWords, formatCall().c_str());
        if (!indirectHold) return;
        auto& hold = d.indirectHold;
        ++hold.count;
        hold.totalMs += totalMs;
        if (totalMs > hold.maxMs) {
            hold.maxMs = totalMs;
            hold.maxPhases = formatCall();
        }
        if (now - hold.lastReport <= std::chrono::seconds(10)) return;
        hold.lastReport = now;
        std::string report;
        for (std::size_t i = 0; i < DispatchRows; ++i) {
            if (hold.phaseMs[i] == 0 && hold.phaseCounts[i] == 0) continue;
            char text[112];
            if (hold.phaseCounts[i] != 0) std::snprintf(text, sizeof(text), " %s %llux %.0f ms", DispatchRowName(i), static_cast<unsigned long long>(hold.phaseCounts[i]), hold.phaseMs[i]);
            else std::snprintf(text, sizeof(text), " %s %.0f ms", DispatchRowName(i), hold.phaseMs[i]);
            report += text;
        }
        aps5::LogErr( "[indirect] %llu indirect dispatches spent %.0f ms inside the device call (10 s; max %.1f ms:%s), by phase (the 'resources: ...' rows split 'resources'; 'hook waits' overlaps them; 'images: ...' rows are the lookups by outcome, count x ms, inside 'B images'):%s; copied-writer lists scanned per decision: dispatch avg %.1f (max %zu), draw avg %.1f\n", static_cast<unsigned long long>(hold.count), hold.totalMs, hold.maxMs, hold.maxPhases.c_str(), report.c_str(), hold.count != 0 ? static_cast<double>(hold.writersScanned) / hold.count : 0.0, hold.maxWriters, hold.count != 0 ? static_cast<double>(hold.drawWritersScanned) / hold.count : 0.0);
        hold.phaseMs = {};
        hold.phaseCounts = {};
        hold.count = 0;
        hold.totalMs = 0;
        hold.maxMs = 0;
        hold.maxPhases.clear();
        hold.writersScanned = 0;
        hold.drawWritersScanned = 0;
        hold.maxWriters = 0;
    }
};

}

struct RecordedDispatch {
    const Graphics::Context* context;
    const Graphics::CompiledShader* shader;
    std::shared_ptr<Graphics::ShaderResources> resources;
    std::shared_ptr<ComputePipelineObjects> objects;
    VkShaderStageFlags pushStages;
    const std::array<std::byte, Graphics::PipelinePushConstantBytes>* pushBytes;
    std::uint32_t x, y, z;

    std::uint64_t arguments;
    const Graphics::HostImport* argumentImport;
    std::uint64_t programAddress;

    enum class DataRefresh { None, Words, Hash };
    DataRefresh dataRefresh;
    std::uint64_t dataWordsHash;

    double revalidateMs;
    DispatchTimer* timer;

    bool refreshed = false;
};

VkDevice VulkanDevice::Device() const {
    return state->device;
}

bool VulkanDevice::DispatchRecipes() {
    static const bool noDispatchRecipe = std::getenv("APS5_NO_DISPATCH_RECIPE") != nullptr;
    return !noDispatchRecipe;
}

bool VulkanDevice::VerifyRecipes() {
    static const bool verifyRecipe = std::getenv("APS5_VERIFY_RECIPE") != nullptr;
    return verifyRecipe;
}

bool VulkanDevice::TemplateDataRefresh() {
    return AgcDriver::TemplateDataRefresh();
}

void VulkanDevice::NoteRecipe(RecipeEvent event, bool indirect) {
    NoteRecipe(event, indirect ? RecipeKind::Indirect : RecipeKind::Dispatch);
}

void VulkanDevice::NoteRecipe(RecipeEvent event, RecipeKind kind) {
    auto& counters = Recipes().kinds[static_cast<std::size_t>(kind)];
    (event == RecipeEvent::Restart ? counters.restarts : counters.attaches).fetch_add(1, std::memory_order_relaxed);
}

void VulkanDevice::NoteDrawRecipeMiss(DrawRecipePrecheck miss) {
    auto& counters = Recipes().kinds[static_cast<std::size_t>(RecipeKind::Draw)];
    counters.precheckMisses[miss == DrawRecipePrecheck::NoRecipe ? MissNoRecipe : MissDevice].fetch_add(1, std::memory_order_relaxed);
}

RecipeOutcome VulkanDevice::DrawFromRecipe(const Graphics::State& graphics, const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> shaders, std::span<const Graphics::GuestMemorySnapshot> snapshots, const std::shared_ptr<const DrawRecipe>& recipe) {
    PerformanceTimer timing("Vulkan.DrawFromRecipe");
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& counters = Recipes().kinds[static_cast<std::size_t>(RecipeKind::Draw)];

    if (recipe->device != state->device) {
        counters.rebuildDevice.fetch_add(1, std::memory_order_relaxed);
        return RecipeOutcome::Rebuild;
    }
    const auto context = graphicsContext();
    const auto outcome = Graphics::DrawWithRecipe(context, graphics, draw, shaders, snapshots, *recipe);
    if (!outcome.recorded) {
        switch (outcome.miss) {
            case Graphics::DrawRecipeMiss::NotRecordable: counters.precheckMisses[MissNotRecordable].fetch_add(1, std::memory_order_relaxed); break;
            case Graphics::DrawRecipeMiss::TargetGone:
            case Graphics::DrawRecipeMiss::ObjectsGone: counters.precheckMisses[MissObjectsGone].fetch_add(1, std::memory_order_relaxed); break;
            case Graphics::DrawRecipeMiss::TemplateGone: counters.precheckMisses[MissTemplateGone].fetch_add(1, std::memory_order_relaxed); break;
            case Graphics::DrawRecipeMiss::Proof: counters.rebuilds[static_cast<std::size_t>(outcome.proof.failure)].fetch_add(1, std::memory_order_relaxed); break;
            default: break;
        }
        return RecipeOutcome::Rebuild;
    }
    counters.proofPaths[static_cast<std::size_t>(outcome.proof.path)].fetch_add(1, std::memory_order_relaxed);
    counters.hits.fetch_add(1, std::memory_order_relaxed);
    if (profile) {
        counters.proofNs.fetch_add(static_cast<std::uint64_t>(outcome.proofUs * 1000.0), std::memory_order_relaxed);
        counters.recordNs.fetch_add(static_cast<std::uint64_t>(outcome.recordUs * 1000.0), std::memory_order_relaxed);
        reportRecipes();
    }
    return RecipeOutcome::Recorded;
}

std::uint64_t VulkanDevice::presync(std::span<const std::pair<std::uint64_t, std::uint64_t>> surfaces) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& counters = Presyncs();
    bool overlaps = false;
    for (const auto& [address, bytes] : surfaces) overlaps = overlaps || Graphics::Recorder::SnapshotWriteOverlaps(address, static_cast<std::size_t>(bytes));
    std::uint64_t serial = 0;
    if (overlaps) {

        std::lock_guard lock(GuestMemory::GpuMutex());
        if (state->recorder) {
            bool open = false;
            for (const auto& [address, bytes] : surfaces) {
                const auto info = state->recorder->DescribePendingWrite(address, static_cast<std::size_t>(bytes));
                if (!info.has_value()) continue;
                open = open || info->open;
                serial = std::max(serial, info->serial);
            }
            if (open) serial = state->recorder->SubmitAndEpoch();
        }
    }
    if (serial != 0) {
        const auto waitStart = std::chrono::steady_clock::now();
        WaitRecorded(serial);
        counters.presyncs.fetch_add(1, std::memory_order_relaxed);
        counters.waitedUs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - waitStart).count()), std::memory_order_relaxed);
    }
    if (profile) {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        auto last = counters.lastReport.load();
        if (nowMs - last >= 10000 && counters.lastReport.compare_exchange_strong(last, nowMs)) aps5::LogErr( "[presync] %llu dispatches checked, %llu pre-syncs waited %.0f ms (cumulative)\n", static_cast<unsigned long long>(counters.checked.load()), static_cast<unsigned long long>(counters.presyncs.load()), counters.waitedUs.load() / 1000.0);
    }
    return serial;
}

std::shared_ptr<PreparedDispatch> VulkanDevice::PrepareDispatch(const ShaderRecompiler::RecompileResult& shader, std::span<const Graphics::GuestMemorySnapshot> snapshots) {

    static const bool lockedBuild = std::getenv("APS5_LOCKED_BUILD") != nullptr;
    if (lockedBuild || shader.spirv.size() < 5 || shader.spirv[0] != 0x07230203u) return nullptr;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto start = std::chrono::steady_clock::now();
    const Graphics::CompiledShader compute{ShaderRecompiler::ShaderStage::Compute, &shader, 0};
    const auto context = graphicsContext();
    auto prepared = std::make_shared<PreparedDispatch>();

    std::shared_ptr<Graphics::ShaderResources> cached;
    auto phaseStart = start;
    const auto phase = [&](PreparedDispatch::PreparePhase which) {
        if (!profile) return;
        const auto now = std::chrono::steady_clock::now();
        prepared->phaseMs[which] += std::chrono::duration<double, std::milli>(now - phaseStart).count();
        phaseStart = now;
    };
    if (ResourceCacheEnabled() && shader.variantId != 0) {
        prepared->key = DispatchContentKey(compute, context.device);
        phase(PreparedDispatch::PrepareKey);
        cached = state->resourceCache.Find(prepared->key);
        prepared->cached = cached;
        phase(PreparedDispatch::PrepareFind);
    }
    if (cached == nullptr) {
        prepared->resources = std::make_shared<Graphics::ShaderResources>(context, compute, snapshots, true);
        phase(PreparedDispatch::PrepareStageA);
    }
    if (cached != nullptr && CachedPrecollect()) {
        cached->PrecollectSurfaces();
        phase(PreparedDispatch::PreparePrecollect);
    }

    if (!NoPresync() && CanWaitUnlocked()) {
        Presyncs().checked.fetch_add(1, std::memory_order_relaxed);
        const auto surfaces = cached != nullptr ? cached->PresyncSurfaces() : prepared->resources->PresyncSurfaces();
        prepared->presyncSerial = presync(surfaces);
        phase(PreparedDispatch::PreparePresync);
    }
    if (profile) prepared->prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return prepared;
}

std::span<const double, 5> VulkanDevice::PreparePhaseMs(const PreparedDispatch& prepared) {
    static_assert(PreparedDispatch::PreparePhaseCount == 5);
    return std::span<const double, 5>(prepared.phaseMs);
}

std::shared_ptr<RecipeHit> VulkanDevice::PrepareRecipe(const std::shared_ptr<const Recipe>& recipe, bool indirect) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& counters = Recipes().kinds[indirect ? 1 : 0];
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto miss = [&](RecipeMiss reason) {
        counters.precheckMisses[reason].fetch_add(1, std::memory_order_relaxed);
        return std::shared_ptr<RecipeHit>{};
    };
    if (!DispatchRecipes() || recipe == nullptr) return miss(MissNoRecipe);
    if (recipe->device != state->device) return miss(MissDevice);
    auto hit = std::make_shared<RecipeHit>();
    hit->recipe = recipe;
    hit->indirect = indirect;
    hit->resources = recipe->templateRef.lock();
    if (hit->resources == nullptr) return miss(MissTemplateGone);
    hit->objects = recipe->objects.lock();
    if (hit->objects == nullptr) return miss(MissObjectsGone);
    if (CachedPrecollect()) hit->resources->PrecollectSurfaces();

    if (!NoPresync() && CanWaitUnlocked()) {
        Presyncs().checked.fetch_add(1, std::memory_order_relaxed);
        if (Graphics::HostImportsUnchanged(graphicsContext(), recipe->presyncProof)) {
            counters.presyncsFromRecipe.fetch_add(1, std::memory_order_relaxed);
            hit->presyncSerial = presync(recipe->presyncSurfaces);
        } else {
            counters.presyncsRecomputed.fetch_add(1, std::memory_order_relaxed);
            hit->presyncSerial = presync(hit->resources->PresyncSurfaces());
        }
        if (hit->presyncSerial != 0) counters.presyncWaits.fetch_add(1, std::memory_order_relaxed);
    }
    if (profile) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        hit->precheckMs = static_cast<double>(ns) / 1e6;
        counters.precheckNs.fetch_add(static_cast<std::uint64_t>(ns), std::memory_order_relaxed);
    }
    return hit;
}

void VulkanDevice::Dispatch(const ShaderRecompiler::RecompileResult& shader, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::span<const Graphics::GuestMemorySnapshot> snapshots, std::uint64_t programAddress, std::shared_ptr<PreparedDispatch> prepared, std::shared_ptr<const Recipe>* recipe) {
    static_cast<void>(dispatch(shader, x, y, z, 0, snapshots, programAddress, std::move(prepared), recipe));
}

VulkanDevice::IndirectOutcome VulkanDevice::DispatchIndirect(const ShaderRecompiler::RecompileResult& shader, std::uint64_t arguments, std::span<const Graphics::GuestMemorySnapshot> snapshots, std::uint64_t programAddress, std::shared_ptr<PreparedDispatch> prepared, std::shared_ptr<const Recipe>* recipe) {
    return dispatch(shader, 0, 0, 0, arguments, snapshots, programAddress, std::move(prepared), recipe);
}

void VulkanDevice::decideIndirect(RecordedDispatch& record, IndirectOutcome& outcome, char* groupsText) {
    auto& recorder = *state->recorder;
    auto& timer = *record.timer;
    const auto& context = *record.context;
    const auto arguments = record.arguments;
    const auto* limit = state->properties.limits.maxComputeWorkGroupCount;

    const auto labelPending = [&] {
        for (std::uint64_t dword = arguments; dword < arguments + 12; dword += 4) {
            if (recorder.PendingLabel(dword, 4, 0).has_value()) return true;
        }
        return false;
    };
    if (Graphics::StorageTexture::FlushPending(arguments, 12, nullptr, "indirect dispatch arguments")) {

        outcome.cpuReason = 1;
        Graphics::Recorder::CountSync(2);
        recorder.Sync();
    } else if (labelPending() || std::any_of(state->copiedWriters->begin(), state->copiedWriters->end(), [&](const auto& writer) { return writer->WritesOverlap(arguments, 12); })
               || std::any_of(Graphics::DrawCopiedWriters()->begin(), Graphics::DrawCopiedWriters()->end(), [&](const auto& writer) { return writer->WritesOverlap(arguments, 12); })) {
        outcome.cpuReason = 2;
    } else if ((record.argumentImport = Graphics::HostImportFor(context, arguments, 12)) == nullptr) {

        outcome.cpuReason = 3;
    }
    if (timer.profile) {
        auto& hold = Dispatches().indirectHold;
        hold.writersScanned += state->copiedWriters->size();
        hold.drawWritersScanned += Graphics::DrawCopiedWriters()->size();
        hold.maxWriters = std::max(hold.maxWriters, state->copiedWriters->size());
    }
    timer.phase(PhaseDecide);
    if (outcome.cpuReason == 0) return;

    const auto readStart = std::chrono::steady_clock::now();
    std::array<std::uint32_t, 3> groups{};
    GuestMemory::Read(arguments, std::as_writable_bytes(std::span(groups)), 4);
    outcome.argumentReadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count();
    record.x = groups[0];
    record.y = groups[1];
    record.z = groups[2];

    timer.phase(PhaseArgumentRead);

    record.arguments = 0;
    timer.indirect = false;
    std::snprintf(groupsText, 40, "%ux%ux%u", record.x, record.y, record.z);
    if (record.x > limit[0] || record.y > limit[1] || record.z > limit[2]) throw std::runtime_error("Vulkan dispatch: workgroup count exceeds device limits");
}

void VulkanDevice::recordDispatch(RecordedDispatch& record) {
    auto& d = Dispatches();
    auto& recorder = *state->recorder;
    auto& timer = *record.timer;
    const auto& context = *record.context;
    auto& resources = *record.resources;
    const auto arguments = record.arguments;
    const auto* argumentImport = record.argumentImport;

    auto recordFrom = timer.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto recordStep = [&](DispatchPhase which) {
        if (!timer.profile) return;
        const auto now = std::chrono::steady_clock::now();
        timer.add(which, std::chrono::duration<double, std::milli>(now - recordFrom).count());
        recordFrom = now;
    };

    const bool barrierElision = Graphics::Recorder::MergeBarriers();

    const auto touches = [&](std::uint64_t begin, std::uint64_t end) {
        const auto bytes = static_cast<std::size_t>(end - begin);
        return resources.WritesOverlap(begin, bytes) || resources.ReadsOverlap(begin, bytes) || (argumentImport != nullptr && begin < arguments + 12 && arguments < end);
    };
    if (recorder.HasQueuedKeyStores() && (resources.HoldsLease() || recorder.AnyQueuedKeyStore(touches))) recorder.FlushKeyStores();
    if (recorder.HasQueuedStores() && (resources.HoldsLease() || recorder.AnyQueuedStore(touches))) recorder.FlushStores();
    VkAccessFlags covered = 0;
    const auto commands = recorder.Commands(&covered);
    recordStep(PhaseRecordCommands);
    recorder.Keep(record.objects);
    recorder.Keep(record.resources);
    recordStep(PhaseRecordKeeps);
    if (record.dataRefresh != RecordedDispatch::DataRefresh::None) {

        const bool differs = record.dataRefresh == RecordedDispatch::DataRefresh::Words || resources.DataWordsHash() != record.dataWordsHash;
        if (differs && resources.RefreshData(commands, *record.shader, &recorder)) {
            covered = 0;
            ++d.templateRefreshed;
            d.templateRevalidateMs += record.revalidateMs;
            record.refreshed = true;
        } else {
            ++d.templateSameWords;
        }
        recordStep(PhaseRecordDataRefresh);
    }
    using CommandClass = Graphics::Recorder::CommandClass;
    if (Graphics::Recorder::BarrierValidate()) {
        if (argumentImport != nullptr) {
            const std::pair<std::uint64_t, std::uint64_t> argumentRange{arguments, arguments + 12};
            recorder.NoteAccess(CommandClass::IndirectArguments, Graphics::Recorder::Access{std::span(&argumentRange, 1), {}, {}, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT});
        }
        const auto reads = resources.InPlaceReads();
        const auto images = resources.StorageImages();
        recorder.NoteAccess(CommandClass::DispatchLeading, Graphics::Recorder::Access{reads, resources.GpuWrites(), images, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, resources.HoldsLease()});
    }
    if (argumentImport != nullptr) {

        const auto timing = recorder.BeginGpuTiming(CommandClass::IndirectArguments);
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
        Graphics::Recorder::CountBarriers(CommandClass::IndirectArguments);
        recorder.EndGpuTiming(timing, 12);
        recorder.NotePendingRead(arguments, 12, Graphics::Recorder::ReadKind::Indirect);
    }

    constexpr VkAccessFlags shaderAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    if ((covered & shaderAccess) == shaderAccess && barrierElision) {
        ++d.preBarriersSkipped;
        Graphics::Recorder::CountMerged(CommandClass::DispatchLeading);
    } else {
        const auto timing = recorder.BeginGpuTiming(CommandClass::DispatchLeading);
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        Graphics::Recorder::CountBarriers(CommandClass::DispatchLeading);
        recorder.EndGpuTiming(timing);
        ++d.preBarriersRecorded;
    }
    context.Resolved(&Graphics::DeviceFunctions::cmdBindPipeline, "vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, record.objects->pipeline);
    resources.Bind(commands, VK_PIPELINE_BIND_POINT_COMPUTE, record.objects->layout);
    if (record.pushStages != 0) {
        context.Resolved(&Graphics::DeviceFunctions::cmdPushConstants, "vkCmdPushConstants")(commands, record.objects->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, Graphics::PipelinePushConstantBytes, record.pushBytes->data());
    }
    const auto gpuTiming = recorder.BeginGpuTiming(record.programAddress != 0 ? record.programAddress : record.shader->program->variantId);
    if (argumentImport != nullptr) context.Resolved(&Graphics::DeviceFunctions::cmdDispatchIndirect, "vkCmdDispatchIndirect")(commands, argumentImport->buffer, arguments - argumentImport->base);
    else context.Resolved(&Graphics::DeviceFunctions::cmdDispatch, "vkCmdDispatch")(commands, record.x, record.y, record.z);
    recorder.EndGpuTiming(gpuTiming);
    const auto trailingTiming = recorder.BeginGpuTiming(CommandClass::DispatchTrailing);
    constexpr VkAccessFlags dispatchedAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT, dispatchedAccess);
    Graphics::Recorder::CountBarriers(CommandClass::DispatchTrailing);
    recorder.EndGpuTiming(trailingTiming);
    recorder.MarkCovered(dispatchedAccess);
    recordStep(PhaseRecordBind);
    resources.MarkGpuWrites(recorder);
    recordStep(PhaseRecordMarks);

    if (resources.NeedsCompletion()) {

        auto writers = state->copiedWriters;
        auto kept = record.resources;
        recorder.OnComplete([kept, writers] {

            writers->erase(std::remove(writers->begin(), writers->end(), kept), writers->end());
            kept->WriteBackBuffers();
        });

        writers->push_back(kept);
    }
    recordStep(PhaseRecordCompletion);
}

VulkanDevice::IndirectOutcome VulkanDevice::dispatch(const ShaderRecompiler::RecompileResult& shader, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint64_t arguments, std::span<const Graphics::GuestMemorySnapshot> snapshots, std::uint64_t programAddress, std::shared_ptr<PreparedDispatch> prepared, std::shared_ptr<const Recipe>* recipeOut) {
    PerformanceTimer timing("Vulkan.Dispatch");
    if (recipeOut != nullptr) *recipeOut = nullptr;

    char groupsText[40];
    if (arguments != 0) std::snprintf(groupsText, sizeof(groupsText), "indirect");
    else std::snprintf(groupsText, sizeof(groupsText), "%ux%ux%u", x, y, z);
    APS5_LOG_OUT_DEBUG("Dispatch groups=%s spirvWords=%zu bindings=%zu pushConstants=%zu", groupsText, shader.spirv.size(), shader.bindings.size(), shader.pushConstants.size());
    if (shader.spirv.size() < 5 || shader.spirv[0] != 0x07230203u) {
        throw std::runtime_error("Vulkan dispatch: invalid SPIR-V");
    }
    const std::array<Graphics::CompiledShader, 1> shaders{{{ShaderRecompiler::ShaderStage::Compute, &shader, 0}}};
    const auto pushStages = Graphics::PushConstantStages(shaders);
    if (pushStages != 0 && state->properties.limits.maxPushConstantsSize < Graphics::PipelinePushConstantBytes) {
        throw std::runtime_error("Vulkan dispatch: compute push constant range exceeds device limit");
    }
    auto pushBytes = Graphics::AssemblePushConstants(shaders);
    const auto context = graphicsContext();
    const auto* limit = state->properties.limits.maxComputeWorkGroupCount;
    if (arguments == 0 && (x > limit[0] || y > limit[1] || z > limit[2])) {
        throw std::runtime_error("Vulkan dispatch: workgroup count exceeds device limits");
    }

    const std::uint64_t pipelineKey = shader.variantId != 0 ? (shader.variantId << 1u) | (pushStages != 0 ? 1u : 0u) : 0u;
    std::shared_ptr<ComputePipelineObjects> objects;
    if (pipelineKey != 0) {
        std::lock_guard pipelines(state->computePipelinesMutex);
        if (const auto found = state->computePipelines.find(pipelineKey); found != state->computePipelines.end()) objects = found->second;
    }
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& d = Dispatches();

    DispatchTimer timer(profile, arguments != 0, shader.spirv.size(), programAddress);
    DispatchTimer::countDispatch(profile);
    const auto lookupsBefore = Graphics::DeviceProcLookups();
    std::shared_ptr<Graphics::ShaderResources> resources;
    ResourceCache::Key contentKey;
    const bool cacheable = ResourceCacheEnabled() && shader.variantId != 0;

    bool fromCache = false;
    double revalidateMs = 0;

    if (prepared != nullptr && prepared->presyncSerial != 0) {

        timer.restart();
        ReapRecorded(prepared->presyncSerial);
        timer.phase(PhaseReap);
    }

    const auto subPhaseStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto waitedBefore = profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;

    if (profile && arguments != 0) Graphics::ThreadLookupOutcomes() = {};

    const auto insert = [&] {
        const auto insertStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

        static const bool deferEviction = std::getenv("APS5_NO_DEFERRED_EVICTION") == nullptr;
        std::vector<std::shared_ptr<Graphics::ShaderResources>> evicted;
        state->resourceCache.Insert(contentKey, resources, deferEviction ? &evicted : nullptr);
        for (auto& object : evicted) state->recorder->Keep(std::move(object));
        if (profile) {
            timer.add(PhaseResourcesInsert, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - insertStart).count());
            if (arguments != 0) d.indirectHold.phaseCounts[PhaseResourcesInsert] += evicted.size();
        }
    };
    if (prepared != nullptr && prepared->resources != nullptr) {

        resources = std::move(prepared->resources);
        contentKey = std::move(prepared->key);

        const auto before = profile ? resources->Timing() : Graphics::ShaderResources::BuildTiming{};
        const auto completeStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        resources->Complete();
        if (profile) {
            const auto completeWall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - completeStart).count();
            timer.add(PhaseResourcesComplete, completeWall);
            const auto& after = resources->Timing();
            const auto images = after.bindingsMs - before.bindingsMs;
            const auto upload = after.uploadMs - before.uploadMs;
            const auto descriptors = after.descriptorsMs - before.descriptorsMs;
            const auto stageA = after.prepareMs - before.prepareMs;
            const auto stageB = after.completeMs - before.completeMs;
            timer.add(PhaseResourcesImages, images);
            timer.add(PhaseResourcesUpload, upload);
            timer.add(PhaseResourcesDescriptors, descriptors);
            timer.add(PhaseResourcesOther, std::max(0.0, stageB - images - upload - descriptors));
            timer.add(PhaseResourcesALocked, stageA);

            timer.add(PhaseResourcesAddress, std::max(0.0, completeWall - stageA - stageB));
            timer.callMs[PhaseResourcesAUnlocked] += prepared->prepareMs;
            d.phaseTotals[PhaseResourcesAUnlocked] += prepared->prepareMs;
        }
        if (cacheable) {
            ++d.cacheMisses;
            if (resources->Reusable()) insert();
        }
    } else if (cacheable) {

        static const bool preparedFind = std::getenv("APS5_NO_PREPARED_FIND") == nullptr;
        contentKey = prepared != nullptr && !prepared->key.empty() ? std::move(prepared->key) : DispatchContentKey(shaders[0], context.device);
        auto cached = preparedFind && prepared != nullptr && prepared->cached != nullptr ? std::move(prepared->cached) : state->resourceCache.Find(contentKey);
        if (cached != nullptr) {
            if (cached->Revalidate(shaders[0])) {
                resources = std::move(cached);
                fromCache = true;
                ++d.cacheHits;
            } else {
                state->resourceCache.Remove(contentKey, cached.get());
                ++d.cacheInvalidated;

                state->recorder->Keep(std::move(cached));
            }
            if (profile) {
                revalidateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - subPhaseStart).count();
                timer.add(PhaseResourcesRevalidate, revalidateMs);
            }
        }
    }
    if (resources == nullptr) {
        const auto buildStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        resources = std::make_shared<Graphics::ShaderResources>(context, shaders[0], snapshots);
        if (profile) timer.add(PhaseResourcesFullBuild, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count());
        if (cacheable) {
            ++d.cacheMisses;
            if (resources->Reusable()) insert();
        }
    }
    resources->PatchPushConstants(pushBytes);
    if (profile) timer.add(PhaseResourcesHookWaits, Graphics::Recorder::ThreadWaitedMs() - waitedBefore);

    if (profile && arguments != 0) {
        const auto& outcomes = Graphics::ThreadLookupOutcomes();
        for (std::size_t kind = 0; kind < Graphics::LookupOutcomes::Count; ++kind) {
            if (outcomes.counts[kind] == 0) continue;
            const auto row = DispatchPhaseCount + kind;
            timer.callMs[row] += outcomes.ms[kind];
            d.indirectHold.phaseMs[row] += outcomes.ms[kind];
            d.indirectHold.phaseCounts[row] += outcomes.counts[kind];
        }
    }
    if (profile && std::chrono::steady_clock::now() - d.cacheReport > std::chrono::seconds(10)) {
        d.cacheReport = std::chrono::steady_clock::now();
        aps5::LogErr( "[rescache] %llu hits, %llu misses, %llu invalidated, %zu entries (dispatch + draw); template hits: %llu refreshed the data buffers (revalidate %.1f ms), %llu had the same words; Find calls %llu, Touch calls %llu\n", static_cast<unsigned long long>(d.cacheHits), static_cast<unsigned long long>(d.cacheMisses), static_cast<unsigned long long>(d.cacheInvalidated), state->resourceCache.Size(), static_cast<unsigned long long>(d.templateRefreshed), d.templateRevalidateMs, static_cast<unsigned long long>(d.templateSameWords), static_cast<unsigned long long>(ResourceCache::Finds()), static_cast<unsigned long long>(ResourceCache::Touches()));
        aps5::LogErr( "[vk] deviceProc lookups inside the device call: %llu (%.2f per dispatch); pre-dispatch barriers recorded %llu, skipped %llu\n", static_cast<unsigned long long>(d.procLookups), d.profiledDispatches != 0 ? static_cast<double>(d.procLookups) / static_cast<double>(d.profiledDispatches) : 0.0, static_cast<unsigned long long>(d.preBarriersRecorded), static_cast<unsigned long long>(d.preBarriersSkipped));
        reportRecipes();
    }
    timing.Mark("shader_resources");
    timer.phase(PhaseResources);
    if (objects == nullptr) {
        objects = std::make_shared<ComputePipelineObjects>();
        objects->device = state->device;
        objects->destroyModule = state->DeviceFunction<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
        objects->destroyLayout = state->DeviceFunction<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
        objects->destroyPipeline = state->DeviceFunction<PFN_vkDestroyPipeline>("vkDestroyPipeline");
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = shader.spirv.size() * sizeof(std::uint32_t);
        moduleInfo.pCode = shader.spirv.data();
        check(state->DeviceFunction<PFN_vkCreateShaderModule>("vkCreateShaderModule")(state->device, &moduleInfo, nullptr, &objects->module), "vkCreateShaderModule");
        timing.Mark("validate_shader_module");
        const auto setLayout = resources->Layout();
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, Graphics::PipelinePushConstantBytes};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;
        layoutInfo.pushConstantRangeCount = pushStages != 0 ? 1 : 0;
        layoutInfo.pPushConstantRanges = pushStages != 0 ? &push : nullptr;
        check(state->DeviceFunction<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(state->device, &layoutInfo, nullptr, &objects->layout), "vkCreatePipelineLayout");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = objects->module;
        pipelineInfo.stage.pName = "main";
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT requiredSubgroup{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT};
        requiredSubgroup.requiredSubgroupSize = shader.hostSubgroupSize;
        if (state->computeWave32 && shader.hostSubgroupSize == 32u) pipelineInfo.stage.pNext = &requiredSubgroup;
        pipelineInfo.layout = objects->layout;
        if (profile && shader.spirv.size() > 100000) aps5::LogErr( "[dispatch] creating a pipeline for %zu SPIR-V words (program 0x%llx)\n", shader.spirv.size(), static_cast<unsigned long long>(programAddress));
        check(state->DeviceFunction<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(state->device, context.pipelineCache, 1, &pipelineInfo, nullptr, &objects->pipeline), "vkCreateComputePipelines");
        timing.Mark("pipeline_create");
        if (pipelineKey != 0) {

            std::lock_guard pipelines(state->computePipelinesMutex);
            objects = state->computePipelines.try_emplace(pipelineKey, objects).first->second;
        }
        timer.phase(PhasePipeline);
    }

    auto& recorder = *state->recorder;
    IndirectOutcome outcome{0, 0};
    RecordedDispatch record{&context, &shaders[0], resources, objects, pushStages, &pushBytes, x, y, z, arguments, nullptr, programAddress, fromCache && TemplateDataRefresh() ? RecordedDispatch::DataRefresh::Words : RecordedDispatch::DataRefresh::None, 0, revalidateMs, &timer};
    if (arguments != 0) decideIndirect(record, outcome, groupsText);

    const bool indirect = record.arguments != 0 || outcome.cpuReason != 0;
    recordDispatch(record);
    timing.Mark("command_record");
    timer.phase(PhaseRecord);
    APS5_LOG_OUT_DEBUG("Dispatch recorded groups=%s", groupsText);

    if (SyncEachDispatch() || (resources->HoldsLease() && Graphics::SyncLeaseWork())) {
        Graphics::Recorder::CountSync(3);
        const auto syncStart = std::chrono::steady_clock::now();
        recorder.Sync();
        timer.phase(PhaseSync);
        if (resources->HoldsLease()) Graphics::CountLeaseOutcome(true, 0);
        if (profile) {

            struct Waits { std::uint64_t count = 0; double ms = 0; };
            static std::map<std::uint64_t, Waits> byProgram;
            static std::uint64_t syncs = 0;
            static double waitedMs = 0;
            static auto lastReport = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration<double, std::milli>(now - syncStart).count();
            auto& waits = byProgram[programAddress != 0 ? programAddress : shader.variantId];
            ++waits.count;
            waits.ms += ms;
            ++syncs;
            waitedMs += ms;
            if (now - lastReport > std::chrono::seconds(10)) {
                lastReport = now;
                std::vector<std::pair<std::uint64_t, Waits>> hot(byProgram.begin(), byProgram.end());
                std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
                aps5::LogErr( "[address-sync] %llu address-based dispatch syncs waited %.1f s in total; by program (10 s):", static_cast<unsigned long long>(syncs), waitedMs / 1000);
                for (std::size_t i = 0; i < hot.size() && i < 8; ++i) aps5::LogErr( " 0x%llx x%llu %.0fms", static_cast<unsigned long long>(hot[i].first), static_cast<unsigned long long>(hot[i].second.count), hot[i].second.ms);
                aps5::LogErr( "\n");
                byProgram.clear();
            }
        }
    } else if (resources->HoldsLease()) {

        Graphics::CountLeaseOutcome(false, recorder.Submissions() + 1);
    }
    if (TraceDispatchIo()) aps5::LogErr( "[dispatch-io] %s:%s\n", groupsText, resources->Describe().c_str());
    WatchMemory(programAddress);

    if (recipeOut != nullptr && DispatchRecipes() && cacheable && resources->Reusable() && objects != nullptr && !contentKey.empty()) {
        auto recipe = std::make_shared<Recipe>();
        recipe->device = state->device;
        recipe->templateRef = resources;
        recipe->key = contentKey;
        recipe->objects = objects;
        recipe->pushBytes = pushBytes;
        recipe->pushes = pushStages != 0;
        recipe->dataWordsHash = Graphics::ShaderResources::DataWordsHash(shaders[0]);
        if (!NoPresync() && CanWaitUnlocked()) {
            recipe->presyncSurfaces = resources->PresyncSurfaces();
            recipe->presyncProof = Graphics::HostImportsIdentity(context);
        }
        recipe->needsCompletion = resources->NeedsCompletion();
        recipe->holdsLease = resources->HoldsLease();
        *recipeOut = std::move(recipe);
    }
    timer.finish(lookupsBefore, groupsText, indirect);
    return outcome;
}

RecipeOutcome VulkanDevice::DispatchRecipe(const ShaderRecompiler::RecompileResult& shader, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint64_t arguments, std::uint64_t programAddress, const std::shared_ptr<RecipeHit>& hit, IndirectOutcome& outcome, const std::shared_ptr<PreparedDispatch>& verify, bool refreshByWords) {
    PerformanceTimer timing("Vulkan.DispatchRecipe");
    outcome = {0, 0};
    char groupsText[40];
    if (arguments != 0) std::snprintf(groupsText, sizeof(groupsText), "indirect");
    else std::snprintf(groupsText, sizeof(groupsText), "%ux%ux%u", x, y, z);
    const auto& recipe = *hit->recipe;
    auto& counters = Recipes().kinds[hit->indirect ? 1 : 0];
    auto& d = Dispatches();
    const std::array<Graphics::CompiledShader, 1> shaders{{{ShaderRecompiler::ShaderStage::Compute, &shader, 0}}};
    const auto context = graphicsContext();
    const auto* limit = state->properties.limits.maxComputeWorkGroupCount;
    if (arguments == 0 && (x > limit[0] || y > limit[1] || z > limit[2])) {
        throw std::runtime_error("Vulkan dispatch: workgroup count exceeds device limits");
    }
    if (recipe.needsCompletion || recipe.holdsLease || !hit->resources->Reusable()) throw std::runtime_error("Vulkan dispatch: recipe over a non-reusable template");

    if (refreshByWords && !TemplateDataRefresh()) return RecipeOutcome::Rebuild;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    DispatchTimer timer(profile, arguments != 0, shader.spirv.size(), programAddress);
    DispatchTimer::countDispatch(profile);
    const auto lookupsBefore = Graphics::DeviceProcLookups();

    if (recipe.device != state->device) {
        counters.rebuildDevice.fetch_add(1, std::memory_order_relaxed);
        return RecipeOutcome::Rebuild;
    }
    if (hit->presyncSerial != 0) {
        timer.restart();
        ReapRecorded(hit->presyncSerial);
        timer.phase(PhaseReap);
    }
    if (verify != nullptr) {

        if (verify->cached.get() != hit->resources.get()) {
            counters.verifyReplaced.fetch_add(1, std::memory_order_relaxed);
            state->recorder->Keep(std::move(hit->resources));
            return RecipeOutcome::Rebuild;
        }
        std::shared_ptr<ComputePipelineObjects> mapped;
        const auto pipelineKey = (shader.variantId << 1u) | (recipe.pushes ? 1u : 0u);
        {
            std::lock_guard pipelines(state->computePipelinesMutex);
            if (const auto found = state->computePipelines.find(pipelineKey); found != state->computePipelines.end()) mapped = found->second;
        }

        const bool byHash = TemplateDataRefresh() && hit->resources->DataWordsHash() != (refreshByWords ? Graphics::ShaderResources::DataWordsHash(shaders[0]) : recipe.dataWordsHash);
        const bool byWords = TemplateDataRefresh() && hit->resources->DataWordsDiffer(shaders[0]);
        const char* disagreement = mapped != hit->objects ? "the pipeline objects" : byHash != byWords ? "the RefreshData decision" : nullptr;
        if (disagreement != nullptr) {
            aps5::LogErr( "[recipe] APS5_VERIFY_RECIPE: %s of the recipe disagrees with the ordinary path (program 0x%llx)\n", disagreement, static_cast<unsigned long long>(programAddress));
            aps5::LogFlush(aps5::LogStdErr);
            std::abort();
        }
        counters.verified.fetch_add(1, std::memory_order_relaxed);
    }

    const auto waitedBefore = profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
    const auto proofStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    Graphics::ShaderResources::ProofReport report;
    const bool proved = hit->resources->ProveCurrent(shaders[0], &report);
    if (profile) {
        counters.proofNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - proofStart).count()), std::memory_order_relaxed);
        timer.add(PhaseResourcesHookWaits, Graphics::Recorder::ThreadWaitedMs() - waitedBefore);
    }
    timer.phase(PhaseProof);
    if (!proved) {
        state->resourceCache.Remove(recipe.key, hit->resources.get());
        ++d.cacheInvalidated;

        state->recorder->Keep(std::move(hit->resources));
        counters.rebuilds[static_cast<std::size_t>(report.failure)].fetch_add(1, std::memory_order_relaxed);
        return RecipeOutcome::Rebuild;
    }
    counters.proofPaths[static_cast<std::size_t>(report.path)].fetch_add(1, std::memory_order_relaxed);
    ++d.cacheHits;
    state->resourceCache.Touch(recipe.key);

    const auto dataRefresh = !TemplateDataRefresh() ? RecordedDispatch::DataRefresh::None : refreshByWords ? RecordedDispatch::DataRefresh::Words : RecordedDispatch::DataRefresh::Hash;
    if (dataRefresh == RecordedDispatch::DataRefresh::Words) counters.refreshByWords.fetch_add(1, std::memory_order_relaxed);
    RecordedDispatch record{&context, &shaders[0], hit->resources, hit->objects, recipe.pushes ? VkShaderStageFlags{VK_SHADER_STAGE_COMPUTE_BIT} : VkShaderStageFlags{0}, &recipe.pushBytes, x, y, z, arguments, nullptr, programAddress, dataRefresh, recipe.dataWordsHash, 0, &timer};
    if (arguments != 0) decideIndirect(record, outcome, groupsText);
    const bool indirect = record.arguments != 0 || outcome.cpuReason != 0;
    const auto recordStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    recordDispatch(record);
    if (profile) counters.recordNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - recordStart).count()), std::memory_order_relaxed);
    timing.Mark("command_record");
    timer.phase(PhaseRecord);
    counters.hits.fetch_add(1, std::memory_order_relaxed);
    if (record.dataRefresh != RecordedDispatch::DataRefresh::None) (record.refreshed ? counters.dataRefreshed : counters.dataSkipped).fetch_add(1, std::memory_order_relaxed);
    APS5_LOG_OUT_DEBUG("Dispatch recorded from recipe groups=%s", groupsText);
    if (SyncEachDispatch()) {
        Graphics::Recorder::CountSync(3);
        state->recorder->Sync();
        timer.phase(PhaseSync);
    }
    if (TraceDispatchIo()) aps5::LogErr( "[dispatch-io] %s:%s\n", groupsText, hit->resources->Describe().c_str());
    WatchMemory(programAddress);
    timer.finish(lookupsBefore, groupsText, indirect);
    if (profile) reportRecipes();
    return RecipeOutcome::Recorded;
}

}
