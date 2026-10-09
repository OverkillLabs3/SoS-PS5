#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/common/StderrLog.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"

#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <stdexcept>
#include <vector>
#include <atomic>
#include <mutex>
#include <map>
#include <limits>
#include <exception>
#include <optional>
#include <unordered_map>
#include <utility>

namespace AgcDriver::Graphics {

namespace {

struct TextureProfile {
    double allocate = 0, read = 0, gpu = 0, view = 0;
    std::uint64_t count = 0;
    std::uint64_t fromStorage = 0;
    double storageCreate = 0, storageWriteBack = 0, storageAlloc = 0, storageHostCopy = 0, storageGpu = 0, storageStore = 0;
    std::uint64_t storageCount = 0;
    std::uint64_t storageBytes = 0;
    std::uint64_t storageReused = 0;
    std::uint64_t storageDirectUploads = 0;
    std::uint64_t storageDirectWriteBacks = 0;

    std::uint64_t recordedUploads = 0;

    std::uint64_t storageRecordedClears = 0;
    std::uint64_t storageWaitedClears = 0;
};

TextureProfile& Profile() {
    static TextureProfile profile;
    return profile;
}

thread_local const char* flushReason = nullptr;

thread_local const char* uploadReason = "first";

std::atomic<std::uint64_t> partialUploads{0}, partialUploadBytes{0}, partialWriteBacks{0}, partialWriteBackBytes{0}, emptyWriteBacks{0}, coalescedWriteBacks{0}, unitsDropped{0}, unitsSuperseded{0};

std::atomic<std::uint64_t> pretestSkipped{0}, unregisteredDropped{0}, keyFlipKept{0};

std::atomic<std::uint64_t> flushedAfterSkip{0}, flushedAfterSkipSameSite{0}, staleEvicted{0};

struct StorageTraffic {
    std::mutex mutex;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> writeBacks;
    std::array<std::pair<std::uint64_t, std::uint64_t>, 4> uploads{};
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> uploadReasons;
    std::uint64_t directWriteBacks = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

StorageTraffic& Traffic() {
    static StorageTraffic traffic;
    return traffic;
}

void reportStorageTraffic(StorageTraffic& traffic) {
    const auto now = std::chrono::steady_clock::now();
    if (now - traffic.lastReport < std::chrono::seconds(10)) return;
    traffic.lastReport = now;
    static constexpr const char* uploadNames[4] = {"clear", "direct", "cpu", "alias"};
    std::string line;
    char text[96];
    for (std::size_t i = 0; i < 4; ++i) {
        std::snprintf(text, sizeof(text), " %s %llu/%.1f", uploadNames[i], static_cast<unsigned long long>(traffic.uploads[i].first), traffic.uploads[i].second / 1048576.0);
        line += text;
        traffic.uploads[i] = {};
    }
    line += "; by reason:";
    for (const auto& [reason, totals] : traffic.uploadReasons) {
        std::snprintf(text, sizeof(text), " %llu/%.1f", static_cast<unsigned long long>(totals.first), totals.second / 1048576.0);
        line += " " + reason + text;
    }
    line += "; write-backs by reason (count/MiB):";
    for (const auto& [reason, totals] : traffic.writeBacks) {
        std::snprintf(text, sizeof(text), " %llu/%.1f", static_cast<unsigned long long>(totals.first), totals.second / 1048576.0);
        line += " " + reason + text;
    }
    const auto take = [](std::atomic<std::uint64_t>& counter) { return static_cast<unsigned long long>(counter.exchange(0, std::memory_order_relaxed)); };
    const auto partialUploadCount = take(partialUploads), partialWriteBackCount = take(partialWriteBacks);
    const auto uploadMiB = take(partialUploadBytes) / 1048576.0, writeBackMiB = take(partialWriteBackBytes) / 1048576.0;
    aps5::LogErr( "[storage] uploads by path (count/MiB, 10 s):%s; %llu write-backs GPU-direct, %llu stored nothing; block units: partial uploads %llu/%.1f, partial write-backs %llu/%.1f, %llu widened to every pending unit, units dropped %llu, superseded %llu, dropped unregistered %llu, kept over a key flip %llu%s, pretest skipped %llu, evicted stale %llu\n", line.c_str(), static_cast<unsigned long long>(traffic.directWriteBacks), take(emptyWriteBacks), partialUploadCount, uploadMiB, partialWriteBackCount, writeBackMiB, take(coalescedWriteBacks), take(unitsDropped), take(unitsSuperseded), take(unregisteredDropped), take(keyFlipKept), ShadowReport().c_str(), take(pretestSkipped), take(staleEvicted));
    traffic.writeBacks.clear();
    traffic.uploadReasons.clear();
    traffic.directWriteBacks = 0;
}

void countStorageUpload(std::size_t path, std::uint64_t bytes) {
    if (!LookupOutcomes::Profiled()) return;
    auto& traffic = Traffic();
    std::lock_guard lock(traffic.mutex);
    ++traffic.uploads[path].first;
    traffic.uploads[path].second += bytes;
    auto& reason = traffic.uploadReasons[uploadReason != nullptr ? uploadReason : "other"];
    ++reason.first;
    reason.second += bytes;
    reportStorageTraffic(traffic);
}

void countStorageWriteBack(std::uint64_t bytes, bool direct) {
    if (!LookupOutcomes::Profiled()) return;
    auto& traffic = Traffic();
    std::lock_guard lock(traffic.mutex);
    auto& totals = traffic.writeBacks[flushReason != nullptr ? flushReason : "other"];
    ++totals.first;
    totals.second += bytes;
    if (direct) ++traffic.directWriteBacks;
    reportStorageTraffic(traffic);
}

struct LiveImages {
    std::mutex mutex;
    std::vector<StorageTexture*> textures;
};

LiveImages& Live() {
    static LiveImages live;
    return live;
}

bool LayerTrackingEnabled() {
    static const bool disabled = std::getenv("APS5_NO_LAYER_CLEAR") != nullptr;
    return !disabled;
}

bool BlockTrackingEnabled() {
    static const bool disabled = std::getenv("APS5_NO_BLOCK_TRACKING") != nullptr;
    return !disabled;
}

struct PhaseTimer {
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    double lap() {
        const auto now = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        return ms;
    }
};

VkImageType ImageTypeFor(TextureDimension dimension) {
    return dimension == TextureDimension::k1D ? VK_IMAGE_TYPE_1D : dimension == TextureDimension::k3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
}

VkImageViewType ViewTypeFor(TextureDimension dimension, [[maybe_unused]] std::uint32_t viewLayerCount) {
    switch (dimension) {
        case TextureDimension::k1D: return VK_IMAGE_VIEW_TYPE_1D;
        case TextureDimension::k2D: return VK_IMAGE_VIEW_TYPE_2D;
        case TextureDimension::k2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;

        case TextureDimension::kCube: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case TextureDimension::k3D: return VK_IMAGE_VIEW_TYPE_3D;
    }
    throw std::runtime_error("AGC graphics: Texture encountered an unknown guest texture dimension");
}

}

namespace {

void ChainMinLod(const Context& context, const GuestTextureResource& descriptor, VkImageViewCreateInfo& viewInfo, VkImageViewMinLodCreateInfoEXT& minLod) {
    const auto clamp = EffectiveMinLod(descriptor);
    if (clamp == 0.0f) return;
    Require(context.imageViewMinLod, "guest texture descriptor uses a minimum LOD clamp, which needs VK_EXT_image_view_min_lod");
    minLod.minLod = clamp;
    minLod.pNext = viewInfo.pNext;
    viewInfo.pNext = &minLod;
}

}

Texture::Texture(const Context& context, TextureDetiler& detiler, const GuestTextureResource& descriptor, VkComponentMapping components, std::span<const std::byte> snapshot, bool depthCompare) : context(context) {

    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    PhaseTimer timer;
    try {
        const auto colorFormat = ResolveTextureFormat(descriptor.format);
        Require(!depthCompare || colorFormat == VK_FORMAT_R32_SFLOAT || colorFormat == VK_FORMAT_R16_UNORM, "comparison sampling requires an R32 float or R16 unorm depth texture");
        Require(!depthCompare || descriptor.dimension != TextureDimension::k3D, "comparison sampling does not support 3D depth textures");
        const auto vkFormat = depthCompare ? (colorFormat == VK_FORMAT_R32_SFLOAT ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM) : colorFormat;
        const VkImageAspectFlags aspect = depthCompare ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        if (IsBlockCompressed(descriptor.format)) {
            Require(context.textureCompressionBC, "device does not support BC compressed textures");
        }

        const auto geometry = DescribeSurface(descriptor);
        const auto& mips = geometry.mips;
        const auto arrayLayers = geometry.layers;
        const auto elementBytes = BytesPerElement(descriptor.format);
        APS5_LOG_OUT("Texture address=0x%llx %ux%u mips=%u layers=%u dim=%d tile=%d format=%u vk=%d element=%u", static_cast<unsigned long long>(descriptor.baseAddress), descriptor.width, descriptor.height, descriptor.mipCount, arrayLayers,
                     static_cast<int>(descriptor.dimension), static_cast<int>(descriptor.tileMode), descriptor.format, static_cast<int>(vkFormat), elementBytes);
        for (const auto& mip : mips) APS5_LOG_OUT("  mip %ux%u tiled=0x%llx+0x%llx linear=0x%llx+0x%llx blocksPerRow=%u pitch=%u tail=%d", mip.width, mip.height, static_cast<unsigned long long>(mip.tiledOffset), static_cast<unsigned long long>(mip.tiledSize), static_cast<unsigned long long>(mip.linearOffset), static_cast<unsigned long long>(mip.linearSize), mip.blocksPerRow, mip.pitchBytes, mip.tail ? 1 : 0);

        const auto guestBytes = geometry.guestBytes;
        Require(snapshot.size() == guestBytes, "texture snapshot size mismatch");

        const auto sliceLinearBytes = geometry.sliceLinearBytes;
        Require(arrayLayers == 0 || sliceLinearBytes <= UINT64_MAX / arrayLayers, "detiled texture buffer size overflows");
        const auto linearBytes = sliceLinearBytes * arrayLayers;

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.flags = descriptor.dimension == TextureDimension::kCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
        imageInfo.imageType = ImageTypeFor(descriptor.dimension);
        imageInfo.format = vkFormat;
        imageInfo.extent = {descriptor.width, descriptor.height, geometry.imageDepth};
        imageInfo.mipLevels = descriptor.mipCount;
        imageInfo.arrayLayers = geometry.imageLayers;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &imageInfo, nullptr, &image), "vkCreateImage");
        owned = std::make_shared<OwnedImage>(context, image, VK_NULL_HANDLE);

        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocationBytes = requirements.size;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &owned->memory), "vkAllocateMemory texture");
        Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, owned->memory, 0), "vkBindImageMemory");

        {

            static const std::string dumpList = [] { const char* text = std::getenv("APS5_DUMP_TEXTURE"); return text ? std::string(text) : std::string(); }();
            bool dumpWanted = false;
            if (!dumpList.empty()) {
                char address[32];
                std::snprintf(address, sizeof(address), "%llx", static_cast<unsigned long long>(descriptor.baseAddress));
                char extent[32];
                std::snprintf(extent, sizeof(extent), "%ux%u", descriptor.width, descriptor.height);

                dumpWanted = dumpList.find(address) != std::string::npos || dumpList.find(extent) != std::string::npos;
            }
            auto staging = std::make_shared<Buffer>(context, static_cast<std::size_t>(guestBytes), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            std::memcpy(staging->Bytes().data(), snapshot.data(), snapshot.size());
            auto tiled = std::make_shared<DeviceBuffer>(context, static_cast<std::size_t>(guestBytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            auto linear = std::make_shared<DeviceBuffer>(context, static_cast<std::size_t>(linearBytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            if (profile) Profile().allocate += timer.lap();
            if (profile) Profile().read += timer.lap();

            detiler.BeginBatch();

            static const bool syncUploads = std::getenv("APS5_SYNC_TEXTURE_UPLOAD") != nullptr;
            auto* recorder = dumpWanted || syncUploads ? nullptr : Recorder::Active();
            std::unique_ptr<CommandBatch> batch;
            VkCommandBuffer commands = VK_NULL_HANDLE;
            if (recorder != nullptr) {
                commands = recorder->Commands();
                recorder->Keep(staging);
                recorder->Keep(tiled);
                recorder->Keep(linear);
                recorder->Keep(owned);
            } else {
                batch = std::make_unique<CommandBatch>(context);
                commands = batch->Handle();
            }

            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            CopyBuffer(context, commands, staging->Handle(), 0, tiled->Handle(), 0, guestBytes);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

            for (std::uint32_t layer = 0; layer < arrayLayers; ++layer) {
                const auto guestLayerOffset = geometry.GuestLayerOffset(layer);
                const auto linearLayerOffset = geometry.LinearLayerOffset(layer);
                for (std::uint32_t level = 0; level < mips.size(); ++level) {
                    if (!geometry.HasLayer(level, layer)) continue;
                    const auto& mip = mips[level];
                    detiler.Dispatch(commands, descriptor.tileMode, elementBytes, tiled->Handle(), guestLayerOffset + mip.tiledOffset, linear->Handle(), linearLayerOffset + mip.linearOffset, mip, false, layer, geometry.thick);
                }
            }

            VkBufferMemoryBarrier linearReadBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            linearReadBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            linearReadBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            linearReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            linearReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            linearReadBarrier.buffer = linear->Handle();
            linearReadBarrier.offset = 0;
            linearReadBarrier.size = VK_WHOLE_SIZE;

            VkImageMemoryBarrier toTransferDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toTransferDst.srcAccessMask = 0;
            toTransferDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransferDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toTransferDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransferDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransferDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransferDst.image = image;
            toTransferDst.subresourceRange = {aspect, 0, descriptor.mipCount, 0, geometry.imageLayers};
            context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &linearReadBarrier, 1, &toTransferDst);

            std::vector<VkBufferImageCopy> regions;
            regions.reserve(static_cast<std::size_t>(arrayLayers) * mips.size());
            for (std::uint32_t layer = 0; layer < arrayLayers; ++layer) {
                const auto linearLayerOffset = static_cast<std::uint64_t>(layer) * sliceLinearBytes;
                for (std::uint32_t level = 0; level < descriptor.mipCount; ++level) {
                    if (!geometry.HasLayer(level, layer)) continue;
                    const auto& mip = mips[level];
                    VkBufferImageCopy region{};
                    region.bufferOffset = linearLayerOffset + mip.linearOffset;
                    region.bufferRowLength = mip.pitchBytes / BytesPerElement(descriptor.format) * BlockWidth(descriptor.format);
                    region.bufferImageHeight = 0;
                    region.imageSubresource = {aspect, level, geometry.CopyLayer(layer), 1};
                    region.imageOffset = {0, 0, geometry.CopyDepth(layer)};
                    region.imageExtent = {std::max(descriptor.width >> level, 1u), std::max(descriptor.height >> level, 1u), 1u};
                    regions.push_back(region);
                }
            }
            context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, linear->Handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(regions.size()), regions.data());
            std::unique_ptr<Buffer> dump;
            if (dumpWanted) {
                dump = std::make_unique<Buffer>(context, static_cast<std::size_t>(mips[0].linearSize), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                CopyBuffer(context, commands, linear->Handle(), mips[0].linearOffset, dump->Handle(), 0, mips[0].linearSize);
                RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            }

            VkImageMemoryBarrier toShaderRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toShaderRead.image = image;
            toShaderRead.subresourceRange = toTransferDst.subresourceRange;
            context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShaderRead);

            if (batch) batch->SubmitAndWait();
            else ++Profile().recordedUploads;
            if (profile) Profile().gpu += timer.lap();
            if (dump) {
                static std::mutex dumpMutex;
                static std::map<std::uint64_t, int> dumped;
                std::lock_guard lock(dumpMutex);
                auto& count = dumped[descriptor.baseAddress];
                if (count < 8) {
                    char name[64];
                    std::snprintf(name, sizeof(name), "texture_%llx_%d.raw", static_cast<unsigned long long>(descriptor.baseAddress), count++);
                    if (std::FILE* file = std::fopen(name, "wb")) {
                        const std::uint32_t header[3] = {mips[0].pitchBytes / static_cast<std::uint32_t>(elementBytes), mips[0].height, static_cast<std::uint32_t>(vkFormat)};
                        std::fwrite(header, sizeof(header), 1, file);
                        std::fwrite(dump->Bytes().data(), 1, dump->Bytes().size(), file);
                        std::fclose(file);
                        aps5::LogErr( "[texture] dumped %s (%ux%u VkFormat %d, tile %d)\n", name, header[0], header[1], static_cast<int>(vkFormat), static_cast<int>(descriptor.tileMode));
                    }
                }
            }
        }

        const auto viewLevelCount = std::min(descriptor.lastLevel, descriptor.mipCount - 1u) - descriptor.baseLevel + 1u;
        const auto viewLayerCount = geometry.imageLayers - descriptor.baseArray;
        if (descriptor.dimension == TextureDimension::kCube) {
            Require(viewLayerCount % 6u == 0, "guest cube texture view does not contain a multiple of 6 array slices");
        }

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = ViewTypeFor(descriptor.dimension, viewLayerCount);
        viewInfo.format = vkFormat;
        viewInfo.components = depthCompare ? VkComponentMapping{} : components;
        viewInfo.subresourceRange = {aspect, descriptor.baseLevel, viewLevelCount, descriptor.baseArray, viewLayerCount};
        VkImageViewMinLodCreateInfoEXT minLod{VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT};
        ChainMinLod(context, descriptor, viewInfo, minLod);

        Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView");
        createFirstLayerView(descriptor, viewInfo);
        if (profile) {
            auto& totals = Profile();
            totals.view += timer.lap();
            if (++totals.count % 200 == 0) aps5::LogErr( "[texture] %llu textures (%llu copied from storage images, %llu uploads recorded): allocate+image %.0f ms, guest read %.0f ms, detile+copy %.0f ms, view+buffer release %.0f ms\n", static_cast<unsigned long long>(totals.count), static_cast<unsigned long long>(totals.fromStorage), static_cast<unsigned long long>(totals.recordedUploads), totals.allocate, totals.read, totals.gpu, totals.view);
        }
    } catch (...) {
        release();
        throw;
    }
}

bool Texture::CanCopyFrom(const StorageTexture& source, const GuestTextureResource& descriptor) {
    const auto& from = source.Descriptor();
    if (IsBlockCompressed(descriptor.format) || IsBlockCompressed(from.format)) return false;

    return descriptor.baseAddress == from.baseAddress && descriptor.width == from.width && descriptor.height == from.height && descriptor.dimension == from.dimension && descriptor.tileMode == from.tileMode && descriptor.mipCount <= from.mipCount && descriptor.depthOrLastArray == from.depthOrLastArray && BytesPerElement(descriptor.format) == BytesPerElement(from.format) && BlockWidth(descriptor.format) == BlockWidth(from.format);
}

Texture::Texture(const Context& context, const std::shared_ptr<StorageTexture>& source, const GuestTextureResource& descriptor, VkComponentMapping components) : context(context), storageSource(source) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    PhaseTimer timer;
    try {
        Require(source != nullptr && CanCopyFrom(*source, descriptor), "storage image does not match the sampled texture");
        const auto vkFormat = ResolveTextureFormat(descriptor.format);
        const auto geometry = DescribeSurface(descriptor);
        APS5_LOG_OUT("Texture address=0x%llx %ux%u mips=%u viewed from storage image (vk=%d)", static_cast<unsigned long long>(descriptor.baseAddress), descriptor.width, descriptor.height, descriptor.mipCount, static_cast<int>(vkFormat));

        layout = VK_IMAGE_LAYOUT_GENERAL;
        const auto viewLevelCount = std::min(descriptor.lastLevel, descriptor.mipCount - 1u) - descriptor.baseLevel + 1u;
        const auto viewLayerCount = std::min(geometry.imageLayers, source->ImageLayers()) - descriptor.baseArray;
        if (descriptor.dimension == TextureDimension::kCube) {
            Require(viewLayerCount % 6u == 0, "guest cube texture view does not contain a multiple of 6 array slices");
        }
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = source->Image();
        viewInfo.viewType = ViewTypeFor(descriptor.dimension, viewLayerCount);
        viewInfo.format = vkFormat;
        viewInfo.components = components;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, descriptor.baseLevel, viewLevelCount, descriptor.baseArray, viewLayerCount};
        VkImageViewMinLodCreateInfoEXT minLod{VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT};
        ChainMinLod(context, descriptor, viewInfo, minLod);
        Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView storage view");
        createFirstLayerView(descriptor, viewInfo);
        if (profile) {
            auto& totals = Profile();
            totals.view += timer.lap();
            ++totals.fromStorage;
            if (++totals.count % 200 == 0) aps5::LogErr( "[texture] %llu textures (%llu viewed from storage images): allocate+image %.0f ms, guest read %.0f ms, detile+copy %.0f ms, view+buffer release %.0f ms\n", static_cast<unsigned long long>(totals.count), static_cast<unsigned long long>(totals.fromStorage), totals.allocate, totals.read, totals.gpu, totals.view);
        }
    } catch (...) {
        release();
        throw;
    }
}

Texture::Texture(const Context& context, VkImage depthImage, VkFormat depthFormat, VkImageAspectFlags aspect, VkComponentMapping components) : context(context) {
    layout = VK_IMAGE_LAYOUT_GENERAL;
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthFormat;
    viewInfo.components = components;
    viewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
    Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView depth plane");
}

Texture::~Texture() {
    release();
}

void Texture::createFirstLayerView(const GuestTextureResource& descriptor, VkImageViewCreateInfo viewInfo) {
    if (descriptor.dimension != TextureDimension::k2DArray) return;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.subresourceRange.layerCount = 1;
    Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &firstLayerView), "vkCreateImageView first layer");
}

void Texture::release() noexcept {
    upload.reset();
    if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
    view = VK_NULL_HANDLE;
    if (firstLayerView) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, firstLayerView, nullptr);
    firstLayerView = VK_NULL_HANDLE;

    owned.reset();
    image = VK_NULL_HANDLE;
}

VkImageView Texture::View() const {
    return view;
}

namespace {

VkBufferMemoryBarrier WholeBufferBarrier(VkBuffer buffer, VkAccessFlags from, VkAccessFlags to) {
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = from;
    barrier.dstAccessMask = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    return barrier;
}

VkFormat StorageFormatOrUndefined(const Context& context, VkFormat format) {
    struct Table {
        std::mutex mutex;
        std::unordered_map<std::uint64_t, VkFormat> formats;
    };
    static Table table;

    const auto key = (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(context.physical)) << 20u) ^ static_cast<std::uint64_t>(format);
    {
        std::lock_guard lock(table.mutex);
        if (const auto found = table.formats.find(key); found != table.formats.end()) return found->second;
    }
    const auto supports = [&](VkFormat candidate) {
        VkFormatProperties properties{};
        context.formatProperties(context.physical, candidate, &properties);
        return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    };
    VkFormat storage = VK_FORMAT_UNDEFINED;
    if (supports(format)) {
        storage = format;
    } else {
        VkFormat linear = VK_FORMAT_UNDEFINED;
        switch (format) {
            case VK_FORMAT_R8G8B8A8_SRGB: linear = VK_FORMAT_R8G8B8A8_UNORM; break;
            case VK_FORMAT_B8G8R8A8_SRGB: linear = VK_FORMAT_B8G8R8A8_UNORM; break;
            case VK_FORMAT_A8B8G8R8_SRGB_PACK32: linear = VK_FORMAT_A8B8G8R8_UNORM_PACK32; break;
            case VK_FORMAT_R8_SRGB: linear = VK_FORMAT_R8_UNORM; break;
            case VK_FORMAT_R8G8_SRGB: linear = VK_FORMAT_R8G8_UNORM; break;
            default: break;
        }
        if (linear != VK_FORMAT_UNDEFINED && supports(linear)) storage = linear;
    }
    std::lock_guard lock(table.mutex);
    table.formats.emplace(key, storage);
    return storage;
}

VkFormat StorageFormatFor(const Context& context, VkFormat format) {
    const auto storage = StorageFormatOrUndefined(context, format);
    if (storage == VK_FORMAT_UNDEFINED) Require(false, "guest storage texture format " + std::to_string(format) + " cannot be used as a storage image");
    return storage;
}

}

bool StorageFormatAvailable(const Context& context, std::uint32_t guestFormat) {
    try {
        return StorageFormatOrUndefined(context, ResolveTextureFormat(guestFormat)) != VK_FORMAT_UNDEFINED;
    } catch (const std::exception&) {

        return false;
    }
}

bool IntegerFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_UINT: case VK_FORMAT_B8G8R8A8_SINT:
        case VK_FORMAT_A8B8G8R8_UINT_PACK32: case VK_FORMAT_A8B8G8R8_SINT_PACK32: case VK_FORMAT_A2R10G10B10_UINT_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R64_UINT: case VK_FORMAT_R64_SINT:
            return true;
        default: return false;
    }
}

bool ClearColorFor(VkFormat format, DccKeys keys, VkClearColorValue& clear) {
    if (IntegerFormat(format)) return false;
    switch (keys) {
        case DccKeys::Clear0000: clear.float32[0] = clear.float32[1] = clear.float32[2] = clear.float32[3] = 0.0f; return true;
        case DccKeys::Clear0001: clear.float32[0] = clear.float32[1] = clear.float32[2] = 0.0f; clear.float32[3] = 1.0f; return true;
        case DccKeys::Clear1110: clear.float32[0] = clear.float32[1] = clear.float32[2] = 1.0f; clear.float32[3] = 0.0f; return true;
        case DccKeys::Clear1111: clear.float32[0] = clear.float32[1] = clear.float32[2] = clear.float32[3] = 1.0f; return true;
        default: return false;
    }
}

VkFormat StorageFormatForGuest(const Context& context, std::uint32_t guestFormat) {
    return StorageFormatFor(context, ResolveTextureFormat(guestFormat));
}

bool StorageClearAvailable(const Context& context, std::uint32_t guestFormat, DccKeys keys) {
    VkClearColorValue clear{};
    return IsDccClear(keys) && StorageFormatAvailable(context, guestFormat) && ClearColorFor(StorageFormatForGuest(context, guestFormat), keys, clear);
}

StorageTexture::StorageTexture(const Context& context, TextureDetiler& detiler, const GuestTextureResource& descriptor, std::uint32_t mipLevel) : context(context), detiler(detiler), descriptor(descriptor) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    PhaseTimer timer;
    try {
        Require(!IsBlockCompressed(descriptor.format), "block-compressed textures cannot be storage images");
        Require(mipLevel < descriptor.mipCount, "storage texture mip level is outside the texture");
        const auto vkFormat = StorageFormatFor(context, ResolveTextureFormat(descriptor.format));
        storageFormat = vkFormat;
        APS5_LOG_OUT("StorageTexture address=0x%llx %ux%u mips=%u mip=%u layers=%u base=%u dim=%d tile=%d format=%u vk=%d", static_cast<unsigned long long>(descriptor.baseAddress), descriptor.width, descriptor.height, descriptor.mipCount, mipLevel,
                     descriptor.depthOrLastArray, descriptor.baseArray, static_cast<int>(descriptor.dimension), static_cast<int>(descriptor.tileMode), descriptor.format, static_cast<int>(vkFormat));
        geometry = DescribeSurface(descriptor);
        mips = geometry.mips;
        arrayLayers = geometry.layers;
        const auto elementBytes = BytesPerElement(descriptor.format);
        guestBytes = geometry.guestBytes;

        Require(!GuestMemory::CommittedRanges(descriptor.baseAddress, static_cast<std::size_t>(guestBytes), true).empty(), "storage texture has no committed guest pages");

        const bool layered = geometry.layers >= 2 && !geometry.thick && geometry.imageDepth == 1 && geometry.layers == geometry.imageLayers && geometry.layerBytes * geometry.layers == guestBytes && geometry.layerBytes % 65536 == 0 && descriptor.baseAddress % 65536 == 0;

        blockUnits = LayerTrackingEnabled() && BlockTrackingEnabled() && !geometry.thick && descriptor.tileMode != TextureTileMode::kLinear && descriptor.baseAddress % 65536 == 0;
        trackedLayers = blockUnits ? static_cast<std::uint32_t>((guestBytes + 65535) / 65536) : layered && LayerTrackingEnabled() ? geometry.layers : 1u;
        trackedLayerBytes = blockUnits ? 65536 : guestBytes / trackedLayers;
        layerGeneration.assign(trackedLayers, 0);
        layerPending.assign(trackedLayers, false);
        sliceLinearBytes = geometry.sliceLinearBytes;
        const auto linearBytes = sliceLinearBytes * arrayLayers;

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};

        imageInfo.flags = (descriptor.dimension == TextureDimension::kCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u) | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        imageInfo.imageType = ImageTypeFor(descriptor.dimension);
        imageInfo.format = vkFormat;
        imageInfo.extent = {descriptor.width, descriptor.height, geometry.imageDepth};
        imageInfo.mipLevels = descriptor.mipCount;
        imageInfo.arrayLayers = geometry.imageLayers;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        {
            VkFormatProperties properties{};
            context.formatProperties(context.physical, vkFormat, &properties);
            attachable = descriptor.dimension == TextureDimension::k2D && (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
            if (attachable) imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &imageInfo, nullptr, &image), "vkCreateImage storage");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory), "vkAllocateMemory storage texture");
        Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory storage");
        uploadReason = "first";
        upload();
        defaultMip = mipLevel;
        view = createView(mipLevel);
        {
            auto& live = Live();
            std::lock_guard lock(live.mutex);
            live.textures.push_back(this);
        }
        if (profile) {
            auto& totals = Profile();
            totals.storageCreate += timer.lap();
            totals.storageBytes += guestBytes;
            if (++totals.storageCount % 50 == 0) aps5::LogErr( "[texture] %llu storage images (%.0f MiB): create %.0f ms, write-back %.0f ms (alloc %.0f, host copy %.0f, gpu %.0f, store %.0f), %llu reused, %llu direct uploads, %llu direct write-backs, clears %llu recorded / %llu waited\n", static_cast<unsigned long long>(totals.storageCount), totals.storageBytes / 1048576.0, totals.storageCreate, totals.storageWriteBack, totals.storageAlloc, totals.storageHostCopy, totals.storageGpu, totals.storageStore, static_cast<unsigned long long>(totals.storageReused), static_cast<unsigned long long>(totals.storageDirectUploads), static_cast<unsigned long long>(totals.storageDirectWriteBacks), static_cast<unsigned long long>(totals.storageRecordedClears), static_cast<unsigned long long>(totals.storageWaitedClears));
        }
    } catch (...) {
        release();
        throw;
    }
}

namespace {

class PendingList {
public:
    using iterator = std::vector<StorageTexture*>::iterator;
    iterator begin() { return textures.begin(); }
    iterator end() { return textures.end(); }
    void push_back(StorageTexture* texture) {
        textures.push_back(texture);
        ++version;
    }
    iterator erase(iterator it) {
        ++version;
        return textures.erase(it);
    }
    void remove(const StorageTexture* texture) {
        if (std::erase(textures, texture) != 0) ++version;
    }
    bool MayOverlap(std::uint64_t address, std::size_t bytes) {
        if (textures.empty() || bytes == 0) return false;
        if (indexed != version) rebuild();
        const auto end = bytes > std::numeric_limits<std::uint64_t>::max() - address ? std::numeric_limits<std::uint64_t>::max() : address + bytes;
        const auto it = std::upper_bound(ranges.begin(), ranges.end(), address, [](std::uint64_t value, const std::pair<std::uint64_t, std::uint64_t>& range) { return value < range.second; });
        return it != ranges.end() && it->first < end;
    }

private:
    void rebuild() {
        ranges.clear();
        for (const auto* texture : textures) ranges.emplace_back(texture->Descriptor().baseAddress, texture->Descriptor().baseAddress + texture->GuestBytes());
        std::sort(ranges.begin(), ranges.end());
        std::size_t merged = 0;
        for (const auto& range : ranges) {
            if (merged != 0 && range.first <= ranges[merged - 1].second) ranges[merged - 1].second = std::max(ranges[merged - 1].second, range.second);
            else ranges[merged++] = range;
        }
        ranges.resize(merged);
        indexed = version;
    }
    std::vector<StorageTexture*> textures;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::uint64_t version = 0;
    std::uint64_t indexed = ~std::uint64_t{0};
};

struct PendingWrites {
    std::mutex mutex;
    PendingList textures;

    std::vector<StorageTexture*> flushing;
};

PendingWrites& Pending() {
    static PendingWrites pending;
    return pending;
}

std::atomic<std::uint64_t> pendingSerial{0};

enum class ClearKind { Unorm, Snorm, Uint, Sint, Float, UFloat };

struct ClearChannel {
    std::uint32_t offset;
    std::uint32_t bits;
    std::uint32_t component;
};

struct ClearLayout {
    ClearKind kind;
    std::uint32_t channels;
    ClearChannel channel[4];
};

bool ClearLayoutFor(VkFormat format, ClearLayout& layout) {
    const auto uniform = [&](ClearKind kind, std::uint32_t count, std::uint32_t width, bool reversed = false) {
        layout.kind = kind;
        layout.channels = count;
        for (std::uint32_t i = 0; i < count; ++i) layout.channel[i] = {i * width, width, reversed && i < 3 ? 2u - i : i};
    };
    switch (format) {
        case VK_FORMAT_R8_UNORM: uniform(ClearKind::Unorm, 1, 8); return true;
        case VK_FORMAT_R8_SNORM: uniform(ClearKind::Snorm, 1, 8); return true;
        case VK_FORMAT_R8_UINT: uniform(ClearKind::Uint, 1, 8); return true;
        case VK_FORMAT_R8_SINT: uniform(ClearKind::Sint, 1, 8); return true;
        case VK_FORMAT_R8G8_UNORM: uniform(ClearKind::Unorm, 2, 8); return true;
        case VK_FORMAT_R8G8_SNORM: uniform(ClearKind::Snorm, 2, 8); return true;
        case VK_FORMAT_R8G8_UINT: uniform(ClearKind::Uint, 2, 8); return true;
        case VK_FORMAT_R8G8_SINT: uniform(ClearKind::Sint, 2, 8); return true;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: uniform(ClearKind::Unorm, 4, 8); return true;
        case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_A8B8G8R8_SNORM_PACK32: uniform(ClearKind::Snorm, 4, 8); return true;
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_A8B8G8R8_UINT_PACK32: uniform(ClearKind::Uint, 4, 8); return true;
        case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_A8B8G8R8_SINT_PACK32: uniform(ClearKind::Sint, 4, 8); return true;
        case VK_FORMAT_B8G8R8A8_UNORM: uniform(ClearKind::Unorm, 4, 8, true); return true;
        case VK_FORMAT_B8G8R8A8_SNORM: uniform(ClearKind::Snorm, 4, 8, true); return true;
        case VK_FORMAT_B8G8R8A8_UINT: uniform(ClearKind::Uint, 4, 8, true); return true;
        case VK_FORMAT_B8G8R8A8_SINT: uniform(ClearKind::Sint, 4, 8, true); return true;
        case VK_FORMAT_R16_UNORM: uniform(ClearKind::Unorm, 1, 16); return true;
        case VK_FORMAT_R16_SNORM: uniform(ClearKind::Snorm, 1, 16); return true;
        case VK_FORMAT_R16_UINT: uniform(ClearKind::Uint, 1, 16); return true;
        case VK_FORMAT_R16_SINT: uniform(ClearKind::Sint, 1, 16); return true;
        case VK_FORMAT_R16_SFLOAT: uniform(ClearKind::Float, 1, 16); return true;
        case VK_FORMAT_R16G16_UNORM: uniform(ClearKind::Unorm, 2, 16); return true;
        case VK_FORMAT_R16G16_SNORM: uniform(ClearKind::Snorm, 2, 16); return true;
        case VK_FORMAT_R16G16_UINT: uniform(ClearKind::Uint, 2, 16); return true;
        case VK_FORMAT_R16G16_SINT: uniform(ClearKind::Sint, 2, 16); return true;
        case VK_FORMAT_R16G16_SFLOAT: uniform(ClearKind::Float, 2, 16); return true;
        case VK_FORMAT_R16G16B16A16_UNORM: uniform(ClearKind::Unorm, 4, 16); return true;
        case VK_FORMAT_R16G16B16A16_SNORM: uniform(ClearKind::Snorm, 4, 16); return true;
        case VK_FORMAT_R16G16B16A16_UINT: uniform(ClearKind::Uint, 4, 16); return true;
        case VK_FORMAT_R16G16B16A16_SINT: uniform(ClearKind::Sint, 4, 16); return true;
        case VK_FORMAT_R16G16B16A16_SFLOAT: uniform(ClearKind::Float, 4, 16); return true;
        case VK_FORMAT_R32_UINT: uniform(ClearKind::Uint, 1, 32); return true;
        case VK_FORMAT_R32_SINT: uniform(ClearKind::Sint, 1, 32); return true;
        case VK_FORMAT_R32_SFLOAT: uniform(ClearKind::Float, 1, 32); return true;
        case VK_FORMAT_R32G32_UINT: uniform(ClearKind::Uint, 2, 32); return true;
        case VK_FORMAT_R32G32_SINT: uniform(ClearKind::Sint, 2, 32); return true;
        case VK_FORMAT_R32G32_SFLOAT: uniform(ClearKind::Float, 2, 32); return true;
        case VK_FORMAT_R32G32B32A32_UINT: uniform(ClearKind::Uint, 4, 32); return true;
        case VK_FORMAT_R32G32B32A32_SINT: uniform(ClearKind::Sint, 4, 32); return true;
        case VK_FORMAT_R32G32B32A32_SFLOAT: uniform(ClearKind::Float, 4, 32); return true;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: layout = {ClearKind::Unorm, 4, {{0, 10, 0}, {10, 10, 1}, {20, 10, 2}, {30, 2, 3}}}; return true;
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: layout = {ClearKind::Uint, 4, {{0, 10, 0}, {10, 10, 1}, {20, 10, 2}, {30, 2, 3}}}; return true;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: layout = {ClearKind::Unorm, 4, {{0, 10, 2}, {10, 10, 1}, {20, 10, 0}, {30, 2, 3}}}; return true;
        case VK_FORMAT_A2R10G10B10_UINT_PACK32: layout = {ClearKind::Uint, 4, {{0, 10, 2}, {10, 10, 1}, {20, 10, 0}, {30, 2, 3}}}; return true;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: layout = {ClearKind::UFloat, 3, {{0, 11, 0}, {11, 11, 1}, {22, 10, 2}}}; return true;
        default: return false;
    }
}

std::optional<float> SmallFloat(std::uint32_t code, std::uint32_t bits) {
    const std::uint32_t mantissaBits = bits == 16 ? 10u : bits - 5u;
    const bool negative = bits == 16 && (code >> 15u) != 0;
    const std::uint32_t exponent = (code >> mantissaBits) & 0x1fu;
    const std::uint32_t mantissa = code & ((1u << mantissaBits) - 1u);
    if (exponent == 31u) {
        if (mantissa != 0 || bits != 16) return std::nullopt;
        return negative ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
    }
    if (exponent == 0) {
        if (mantissa != 0) return std::nullopt;
        return negative ? -0.0f : 0.0f;
    }
    const float value = std::ldexp(1.0f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissaBits), static_cast<int>(exponent) - 15);
    return negative ? -value : value;
}

bool FillClearColor(VkFormat format, std::uint32_t elementBytes, std::span<const std::uint32_t, 4> pattern, VkClearColorValue& clear) {
    ClearLayout layout{};
    if (elementBytes == 0 || elementBytes > 16 || 16 % elementBytes != 0 || !ClearLayoutFor(format, layout)) return false;
    std::array<std::byte, 16> bytes{};
    std::memcpy(bytes.data(), pattern.data(), bytes.size());
    for (std::size_t at = elementBytes; at < bytes.size(); at += elementBytes) {
        if (std::memcmp(bytes.data(), bytes.data() + at, elementBytes) != 0) return false;
    }
    for (std::uint32_t i = 0; i < layout.channels; ++i) {
        const auto& channel = layout.channel[i];
        const auto word = pattern[channel.offset / 32u];
        const auto code = channel.bits == 32 ? word : (word >> (channel.offset % 32u)) & ((1u << channel.bits) - 1u);
        const auto signedCode = static_cast<std::int32_t>(code << (32u - channel.bits)) >> (32u - channel.bits);
        switch (layout.kind) {
            case ClearKind::Unorm:
                clear.float32[channel.component] = static_cast<float>(code) / static_cast<float>((1u << channel.bits) - 1u);
                break;
            case ClearKind::Snorm: {
                const auto maximum = static_cast<std::int32_t>((1u << (channel.bits - 1u)) - 1u);
                if (signedCode < -maximum) return false;
                clear.float32[channel.component] = static_cast<float>(signedCode) / static_cast<float>(maximum);
                break;
            }
            case ClearKind::Uint:
                clear.uint32[channel.component] = code;
                break;
            case ClearKind::Sint:
                clear.int32[channel.component] = signedCode;
                break;
            case ClearKind::Float:
                if (channel.bits == 32) {
                    const auto exponent = (code >> 23u) & 0xffu;
                    const auto mantissa = code & 0x7fffffu;
                    if ((exponent == 0xffu && mantissa != 0) || (exponent == 0 && mantissa != 0)) return false;
                    clear.float32[channel.component] = std::bit_cast<float>(code);
                } else {
                    const auto value = SmallFloat(code, channel.bits);
                    if (!value) return false;
                    clear.float32[channel.component] = *value;
                }
                break;
            case ClearKind::UFloat: {
                const auto value = SmallFloat(code, channel.bits);
                if (!value) return false;
                clear.float32[channel.component] = *value;
                break;
            }
        }
    }
    return true;
}

thread_local const StorageTexture* refreshing = nullptr;

void FlushHook(std::uint64_t address, std::size_t bytes) {
    StorageTexture::FlushPending(address, bytes);
}

bool AdjacentGenerationEnabled() {
    static const bool disabled = std::getenv("APS5_NO_ADJACENT_GENERATION") != nullptr;
    return !disabled;
}

}

VkImageView StorageTexture::createView(std::uint32_t mip, bool firstLayer) const {
    Require(mip < descriptor.mipCount, "storage texture mip level is outside the texture");
    Require(!firstLayer || descriptor.dimension == TextureDimension::k2DArray, "a first-layer storage view needs a 2D array surface");
    const auto viewLayerCount = firstLayer ? 1u : geometry.imageLayers - descriptor.baseArray;
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;

    viewInfo.viewType = firstLayer ? VK_IMAGE_VIEW_TYPE_2D : descriptor.dimension == TextureDimension::k1D ? VK_IMAGE_VIEW_TYPE_1D : descriptor.dimension == TextureDimension::k2D ? VK_IMAGE_VIEW_TYPE_2D : descriptor.dimension == TextureDimension::k3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = storageFormat;
    viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1u, descriptor.baseArray, viewLayerCount};
    VkImageView created = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &created), "vkCreateImageView storage");
    return created;
}

VkImageView StorageTexture::AttachmentView(VkFormat format, std::uint32_t mip) {
    Require(attachable, "storage image cannot be a color attachment");
    Require(mip < descriptor.mipCount, "attachment mip exceeds the storage image");
    if (mip == 0) attachmentFormat = format;
    const auto found = attachmentViews.find({format, mip});
    if (found != attachmentViews.end()) return found->second;
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.pNext = &usage;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1u, descriptor.baseArray, 1u};
    VkImageView created = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &created), "vkCreateImageView attachment");
    attachmentViews.emplace(std::pair{format, mip}, created);
    return created;
}

VkImageView StorageTexture::View(std::uint32_t mip) {
    if (mip == defaultMip) return view;
    const auto found = extraViews.find(mip);
    if (found != extraViews.end()) return found->second;
    const auto created = createView(mip);
    extraViews.emplace(mip, created);
    return created;
}

VkImageView StorageTexture::FirstLayerView(std::uint32_t mip) {
    const auto found = firstLayerViews.find(mip);
    if (found != firstLayerViews.end()) return found->second;
    const auto created = createView(mip, true);
    firstLayerViews.emplace(mip, created);
    return created;
}

const char* LookupOutcomes::Name(Kind kind) {
    static constexpr const char* names[Count] = {"sampled fast hit", "sampled fast miss", "sampled hit view", "sampled hit cleared view", "sampled hit snapshot", "sampled made view", "sampled made snapshot", "storage hit", "storage made", "storage depth check", "storage key build", "storage lock wait", "refresh unchanged", "refresh compared", "upload direct", "upload cpu", "upload clear", "dcc scan", "pending flush"};
    return kind < Count ? names[kind] : "?";
}

bool LookupOutcomes::Profiled() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

std::chrono::steady_clock::time_point LookupOutcomes::Add(Kind kind, std::chrono::steady_clock::time_point start) {
    const auto now = std::chrono::steady_clock::now();
    auto& outcomes = ThreadLookupOutcomes();
    ++outcomes.counts[kind];
    outcomes.ms[kind] += std::chrono::duration<double, std::milli>(now - start).count();
    return now;
}

LookupOutcomes& ThreadLookupOutcomes() {
    thread_local LookupOutcomes outcomes;
    return outcomes;
}

bool StorageTexture::Refresh() {
    const bool profile = LookupOutcomes::Profiled();
    auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    struct Exempt {
        const StorageTexture* previous;
        ~Exempt() { refreshing = previous; }
    } exempt{refreshing};

    refreshing = this;
    NoteProved();

    auto alias = pendingAlias();
    if (alias != nullptr && !(ProvedClearKeys(descriptor, guestBytes, keyProof) == DccKeys::Uncompressed && HostImportFor(context, descriptor.baseAddress, static_cast<std::size_t>(guestBytes)) != nullptr)) alias = nullptr;
    if (alias != nullptr) {
        std::vector<std::uint8_t> aliasStamped(trackedLayers);
        if (!GuestMemory::ChangedBlocks(descriptor.baseAddress, static_cast<std::size_t>(guestBytes), alias->layerGeneration, aliasStamped)) alias = nullptr;
        for (std::uint32_t unit = 0; alias != nullptr && unit < trackedLayers; ++unit) {
            if (!alias->layerPending[unit]) continue;
            if (alias->layerGeneration[unit] == 0 || aliasStamped[unit] == GuestMemory::BlockMaybeWritten || (aliasStamped[unit] == GuestMemory::BlockUnchanged && layerPending[unit])) {
                alias = nullptr;
                break;
            }
        }
    }

    const bool flushed = FlushPending(descriptor.baseAddress, static_cast<std::size_t>(guestBytes), alias.get(), "storage refresh", PublishScope::None);
    if (profile && flushed) start = LookupOutcomes::Add(LookupOutcomes::PendingFlush, start);

    const auto current = GuestMemory::CollectWrites(descriptor.baseAddress, static_cast<std::size_t>(guestBytes));

    std::vector<bool> changed(trackedLayers, false);

    std::vector<std::uint64_t> generations;
    std::vector<std::uint8_t> stampedBlocks, cpuBlocks;

    std::vector<bool> borrowed;
    bool anyBorrowed = false;
    bool unchanged = true;
    bool stamped = true;
    bool tracked = true;
    bool keysChanged = false;
    bool cpuWrote = false;
    bool direct = false;
    DccKeys keys = uploadedKeys;
    {
        const auto equalsOriginal = [&] {

            const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::TextureCompare);
            return GuestMemory::EqualsCommitted(descriptor.baseAddress, original);
        };
        const auto scanBlocks = [&] {
            blockGenerations(generations);
            stampedBlocks.assign(generations.size(), 0);
            cpuBlocks.assign(generations.size(), 0);
            tracked = GuestMemory::ChangedBlocks(descriptor.baseAddress, static_cast<std::size_t>(guestBytes), generations, stampedBlocks, cpuBlocks);
        };
        const auto keysStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        keys = ProvedClearKeys(descriptor, guestBytes, keyProof);
        if (profile && descriptor.dccAddress != 0) LookupOutcomes::Add(LookupOutcomes::DccScan, keysStart);
        if (keys != uploadedKeys) {
            changed.assign(trackedLayers, true);
            unchanged = false;
            keysChanged = true;

            if (blockUnits) scanBlocks();
        } else if (blockUnits) {
            if (!GuestMemory::UnchangedSince(descriptor.baseAddress, static_cast<std::size_t>(guestBytes), generation)) {

                scanBlocks();
                for (std::uint32_t unit = 0; unit < trackedLayers; ++unit) {
                    if (stampedBlocks[unit] == 0) {
                        layerGeneration[unit] = current;
                        continue;
                    }
                    changed[unit] = true;
                    unchanged = false;
                }
                refreshGeneration();
            }
        } else {
            for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
                if (GuestMemory::UnchangedSince(layerBegin(layer), static_cast<std::size_t>(layerBytes(layer)), layerGeneration[layer])) continue;
                changed[layer] = true;
                unchanged = false;
            }
            if (!unchanged) scanBlocks();
        }

        for (std::size_t k = 0; k < cpuBlocks.size(); ++k) {
            if (tracked && cpuBlocks[k] != 0 && generations[k] != 0) cpuWrote = true;
        }

        bool pendingChanged = false;
        for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
            if (changed[layer] && layerPending[layer]) pendingChanged = true;
        }
        if (!unchanged && !keysChanged && !pendingChanged && originalValid && equalsOriginal()) {
            unchanged = true;
            stamped = false;
        }

        direct = keys == DccKeys::Uncompressed && HostImportFor(context, descriptor.baseAddress, static_cast<std::size_t>(guestBytes)) != nullptr;
        if (alias != nullptr && direct) {

            const bool sameBorrow = borrowedFrom.lock() == alias && borrowedVersion == alias->version && borrowedUnits.size() == trackedLayers;
            std::vector<std::uint8_t> aliasStamped(trackedLayers);
            const bool aliasTracked = GuestMemory::ChangedBlocks(descriptor.baseAddress, static_cast<std::size_t>(guestBytes), alias->layerGeneration, aliasStamped);
            borrowed.assign(trackedLayers, false);
            for (std::uint32_t unit = 0; unit < trackedLayers; ++unit) {
                if (!aliasTracked || !alias->layerPending[unit] || aliasStamped[unit] != 0 || alias->layerGeneration[unit] == 0 || layerPending[unit]) continue;
                if (sameBorrow && borrowedUnits[unit]) continue;
                borrowed[unit] = true;
                anyBorrowed = true;
                changed[unit] = false;
                unchanged = false;
            }
            if (!sameBorrow) {
                borrowedFrom.reset();
                borrowedUnits.clear();
            }
        }
    }
    if (unchanged) {
        ++Profile().storageReused;
        layerGeneration.assign(trackedLayers, current);
        refreshGeneration();
        if (profile) LookupOutcomes::Add(stamped ? LookupOutcomes::RefreshUnchanged : LookupOutcomes::RefreshCompared, start);
        return true;
    }
    CaptureTrace::Log("refresh image=%llx bytes=%llu generation=%llu dirty=%d keysChanged=%d cpuWrote=%d", static_cast<unsigned long long>(descriptor.baseAddress), static_cast<unsigned long long>(guestBytes), static_cast<unsigned long long>(generation), dirty, keysChanged, cpuWrote);

    static const bool traceUpload = std::getenv("APS5_TRACE_UPLOAD") != nullptr;
    if (traceUpload) {
        const auto keys = TextureClearKeys(descriptor, guestBytes);
        aps5::LogErr( "[upload] 0x%llx+0x%llx %ux%u mips %u: %s (keys %s -> %s, originalValid %d, dirty %d)\n", static_cast<unsigned long long>(descriptor.baseAddress), static_cast<unsigned long long>(guestBytes), descriptor.width, descriptor.height, descriptor.mipCount, keys != uploadedKeys ? "DCC keys changed" : "guest memory changed", DccKeysName(uploadedKeys), DccKeysName(keys), originalValid ? 1 : 0, dirty ? 1 : 0);
    }

    std::vector<bool> stored(trackedLayers, false);
    bool pendingResults = false;
    bool anyStored = false;
    bool dropped = false;

    const auto droppable = [&](std::uint32_t unit) {
        if (keysChanged && IsDccClear(keys)) return true;
        return tracked && layerGeneration[unit] != 0 && unit < stampedBlocks.size() && stampedBlocks[unit] == GuestMemory::BlockWritten;
    };

    static const bool keyFlipDrop = std::getenv("APS5_KEYFLIP_DROP") != nullptr;
    static const bool traceKeys = std::getenv("APS5_TRACE_DCC_KEYS") != nullptr;
    const bool keyFlip = keysChanged && blockUnits && !keyFlipDrop && IsDccClear(uploadedKeys) && keys == DccKeys::Uncompressed && stampedBlocks.size() >= trackedLayers;
    if (traceKeys && keysChanged) {
        std::size_t pendingUnits = 0, stampedUnits = 0;
        for (std::uint32_t unit = 0; unit < trackedLayers; ++unit) {
            if (layerPending[unit]) ++pendingUnits;
            if (unit < cpuBlocks.size() && cpuBlocks[unit] != 0 && generations[unit] != 0) ++stampedUnits;
        }
        aps5::LogErr( "[dcc-keys] refresh 0x%llx+0x%llx keys %s -> %s (dirty %d, %zu pending units, %zu cpu-stamped units%s)\n", static_cast<unsigned long long>(descriptor.baseAddress), static_cast<unsigned long long>(guestBytes), DccKeysName(uploadedKeys), DccKeysName(keys), dirty ? 1 : 0, pendingUnits, stampedUnits, keyFlip ? ", flip keeps unstamped results" : "");
    }
    for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
        if (!layerPending[layer]) continue;
        if (keyFlip && layerGeneration[layer] != 0 && stampedBlocks[layer] != GuestMemory::BlockWritten) {
            changed[layer] = false;
            keyFlipKept.fetch_add(1, std::memory_order_relaxed);
            if (direct) continue;
            stored[layer] = true;
            anyStored = true;
            continue;
        }
        if (changed[layer]) {
            pendingResults = true;
            if (blockUnits && droppable(layer)) {
                CaptureTrace::Log("drop-unit image=%llx unit=%u generation=%llu stamped=%d keys=%d", static_cast<unsigned long long>(descriptor.baseAddress), layer, static_cast<unsigned long long>(layerGeneration[layer]), layer < stampedBlocks.size() && stampedBlocks[layer] != 0, static_cast<int>(keys));
                layerPending[layer] = false;
                dropped = true;
                continue;
            }
        } else if (direct) {
            continue;
        }
        stored[layer] = true;
        anyStored = true;
    }
    if (anyStored && !keysChanged && descriptor.dccAddress != 0 && IsDccClear(uploadedKeys)) stored.assign(trackedLayers, true);
    if (pendingResults) {
        static std::atomic<int> reports{0};
        if (reports.fetch_add(1) < 8) aps5::LogErr( "[gpu] storage image 0x%llx: guest memory changed while GPU results were pending; keeping the CPU's blocks\n", static_cast<unsigned long long>(descriptor.baseAddress));
    }
    if (anyStored) {
        const auto previous = std::exchange(flushReason, "refresh");
        writeBackLayers(stored);
        flushReason = previous;
    } else if (dropped) {
        reconcilePending();
    }
    if (anyBorrowed) {
        uploadReason = "alias";
        borrowUnits(*alias, borrowed);
        for (std::uint32_t unit = 0; unit < trackedLayers; ++unit) {
            if (borrowed[unit]) layerGeneration[unit] = current;
        }
        refreshGeneration();
    }
    if (std::any_of(changed.begin(), changed.end(), [](bool selected) { return selected; })) {
        uploadReason = keysChanged ? "keys" : cpuWrote ? "cpu" : generation == 0 ? "untracked" : flushed ? "flushed" : "store";
        upload(&changed);
    } else {
        uploadedKeys = keys;
    }
    return false;
}

namespace {

void traceKeyStore(const char* path, const GuestTextureResource& descriptor, std::uint64_t guestBytes) {
    static const bool trace = std::getenv("APS5_TRACE_DCC_KEYS") != nullptr;
    if (!trace || descriptor.dccAddress == 0) return;
    const auto packet = GuestMemory::CurrentPacket();
    aps5::LogErr( "[dcc-keys] uncompressed store by %s (%s) for surface 0x%llx+0x%llx: keys 0x%llx+0x%llx (packet 0x%x queue 0x%x)\n", path, flushReason != nullptr ? flushReason : "?", static_cast<unsigned long long>(descriptor.baseAddress), static_cast<unsigned long long>(guestBytes), static_cast<unsigned long long>(descriptor.dccAddress), static_cast<unsigned long long>(guestBytes / 256), packet.opcode, packet.queue);
}

}

void StorageTexture::upload(const std::vector<bool>* layers) {
    CaptureTrace::Log("upload image=%llx bytes=%llu generation=%llu reason=%s partial=%d", static_cast<unsigned long long>(descriptor.baseAddress), static_cast<unsigned long long>(guestBytes), static_cast<unsigned long long>(generation), uploadReason, layers != nullptr);
    const bool profile = LookupOutcomes::Profiled();
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto elementBytes = BytesPerElement(descriptor.format);
    const auto linearBytes = sliceLinearBytes * arrayLayers;
    original.resize(static_cast<std::size_t>(guestBytes));

    if (layers != nullptr && ((!blockUnits && trackedLayers != arrayLayers) || std::all_of(layers->begin(), layers->end(), [](bool selected) { return selected; }))) layers = nullptr;
    const auto now = GuestMemory::CollectWrites(descriptor.baseAddress, static_cast<std::size_t>(guestBytes));
    const auto stampLayers = [&](bool selectedOnly) {
        for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
            if (!selectedOnly || layers == nullptr || (*layers)[layer]) layerGeneration[layer] = now;
        }
        refreshGeneration();
    };
    uploadedKeys = ProvedClearKeys(descriptor, guestBytes, keyProof);
    filledKeys = DccKeys::Uncompressed;
    VkClearColorValue clearValue{};
    if (IsDccClear(uploadedKeys) && ClearColorFor(storageFormat, uploadedKeys, clearValue)) {

        originalValid = false;
        forgetBorrowed(0, trackedLayers);
        stampLayers(false);
        static const bool recordClear = std::getenv("APS5_NO_RECORDED_CLEAR") == nullptr;
        auto* recorder = recordClear ? Recorder::Active() : nullptr;
        std::unique_ptr<CommandBatch> batch;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        auto timing = Recorder::NoTiming;
        if (recorder != nullptr) {
            commands = recorder->Commands();
            timing = recorder->BeginGpuTiming(Recorder::CommandClass::DccClear);
            if (auto self = weak_from_this().lock()) recorder->Keep(std::move(self));
            ++Profile().storageRecordedClears;
            Recorder::CountBarriers(Recorder::CommandClass::DccClear, 2);
            if (Recorder::BarrierValidate()) {
                const std::pair<VkImage, bool> cleared{image, true};
                recorder->NoteAccess(Recorder::CommandClass::DccClear, Recorder::Access{{}, {}, std::span(&cleared, 1), VK_PIPELINE_STAGE_TRANSFER_BIT});
            }
        } else {
            batch = std::make_unique<CommandBatch>(context);
            commands = batch->Handle();
            ++Profile().storageWaitedClears;
        }
        VkImageMemoryBarrier toClear{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toClear.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toClear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toClear.image = image;
        toClear.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, descriptor.mipCount, 0, geometry.imageLayers};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toClear);
        context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1, &toClear.subresourceRange);
        VkImageMemoryBarrier toGeneral = toClear;
        toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
        if (batch) batch->SubmitAndWait();
        else recorder->EndGpuTiming(timing, guestBytes);
        countStorageUpload(0, guestBytes);
        ++version;
        if (profile) LookupOutcomes::Add(LookupOutcomes::UploadClear, start);
        return;
    }
    if (const auto* import = uploadedKeys == DccKeys::Uncompressed ? HostImportFor(context, descriptor.baseAddress, static_cast<std::size_t>(guestBytes)) : nullptr) {

        originalValid = false;
        stampLayers(true);

        if (blockUnits && (layers != nullptr || AnyShadowedOverlaps(descriptor.baseAddress, static_cast<std::size_t>(guestBytes)))) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
            if (layers != nullptr) {
                for (std::uint32_t unit = 0; unit < trackedLayers; ++unit) {
                    if ((*layers)[unit]) forgetBorrowed(unit, 1);
                }
                runs = unitRuns(*layers);
            } else {
                forgetBorrowed(0, trackedLayers);
                runs.emplace_back(0, guestBytes);
            }
            const auto uploadedBytes = uploadWindows(*import, runs, layers == nullptr && version == 0);
            countStorageUpload(1, uploadedBytes);
            if (layers != nullptr) {
                partialUploads.fetch_add(1, std::memory_order_relaxed);
                partialUploadBytes.fetch_add(uploadedBytes, std::memory_order_relaxed);
            }
            ++Profile().storageDirectUploads;
            ++version;
            if (profile) LookupOutcomes::Add(LookupOutcomes::UploadDirect, start);
            return;
        }

        PublishShadow(descriptor.baseAddress, static_cast<std::size_t>(guestBytes), PublishScope::Whole, PublishReason::Upload);
        forgetBorrowed(0, trackedLayers);
        std::uint64_t uploadedBytes = layers == nullptr ? guestBytes : 0;
        if (layers != nullptr) {
            for (std::uint32_t layer = 0; layer < arrayLayers; ++layer) {
                if ((*layers)[layer]) uploadedBytes += trackedLayerBytes;
            }
        }
        auto linear = std::make_shared<DeviceBuffer>(context, static_cast<std::size_t>(linearBytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        detiler.BeginBatch();
        auto* recorder = Recorder::Active();
        std::unique_ptr<CommandBatch> batch;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        auto timing = Recorder::NoTiming;
        if (recorder != nullptr) {

            recorder->FlushStoresOverlapping(descriptor.baseAddress, static_cast<std::size_t>(guestBytes));
            commands = recorder->Commands();
            timing = recorder->BeginGpuTiming(Recorder::CommandClass::StorageUpload);
            recorder->Keep(linear);

            if (auto self = weak_from_this().lock()) recorder->Keep(std::move(self));

            recorder->NotePendingRead(descriptor.baseAddress, static_cast<std::size_t>(guestBytes), Recorder::ReadKind::StorageUpload);
            Recorder::CountBarriers(Recorder::CommandClass::StorageUpload, 3);
            if (Recorder::BarrierValidate()) {
                const std::pair<std::uint64_t, std::uint64_t> read{descriptor.baseAddress, descriptor.baseAddress + guestBytes};
                const std::pair<VkImage, bool> written{image, true};
                recorder->NoteAccess(Recorder::CommandClass::StorageUpload, Recorder::Access{std::span(&read, 1), {}, std::span(&written, 1), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT});
            }
        } else {
            batch = std::make_unique<CommandBatch>(context);
            commands = batch->Handle();
        }
        const auto importOffset = descriptor.baseAddress - import->base;
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        for (std::uint32_t layer = 0; layer < arrayLayers; ++layer) {
            if (layers != nullptr && !(*layers)[layer]) continue;
            for (std::uint32_t level = 0; level < mips.size(); ++level) {
                if (!geometry.HasLayer(level, layer)) continue;
                const auto& mip = mips[level];
                detiler.Dispatch(commands, descriptor.tileMode, elementBytes, import->buffer, importOffset + geometry.GuestLayerOffset(layer) + mip.tiledOffset, linear->Handle(), geometry.LinearLayerOffset(layer) + mip.linearOffset, mip, false, layer, geometry.thick);
            }
        }
        const auto linearRead = WholeBufferBarrier(linear->Handle(), VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.srcAccessMask = layers != nullptr ? VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0u;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        toTransfer.oldLayout = layers != nullptr ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = image;
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, descriptor.mipCount, 0, geometry.imageLayers};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &linearRead, 1, &toTransfer);
        const auto regions = CopyRegions(layers);
        context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, linear->Handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(regions.size()), regions.data());
        VkImageMemoryBarrier toGeneral = toTransfer;
        toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
        if (batch) batch->SubmitAndWait();
        else recorder->EndGpuTiming(timing, uploadedBytes);
        countStorageUpload(1, uploadedBytes);
        ++Profile().storageDirectUploads;
        ++version;
        if (profile) LookupOutcomes::Add(LookupOutcomes::UploadDirect, start);
        return;
    }
    originalValid = true;
    forgetBorrowed(0, trackedLayers);
    stampLayers(false);
    GuestMemory::ReadCommitted(descriptor.baseAddress, original);
    {
            Buffer staging(context, original.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            if (uploadedKeys == DccKeys::Uncompressed) std::memcpy(staging.Bytes().data(), original.data(), original.size());
            else ReadTextureSurface(descriptor, uploadedKeys, staging.Bytes().first(original.size()));
            DeviceBuffer tiled(context, original.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            DeviceBuffer linear(context, static_cast<std::size_t>(linearBytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            detiler.BeginBatch();
            CommandBatch batch(context);
            const auto commands = batch.Handle();
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            CopyBuffer(context, commands, staging.Handle(), 0, tiled.Handle(), 0, original.size());
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            for (std::uint32_t layer = 0; layer < arrayLayers; ++layer) {
                for (std::uint32_t level = 0; level < mips.size(); ++level) {
                    if (!geometry.HasLayer(level, layer)) continue;
                    const auto& mip = mips[level];
                    detiler.Dispatch(commands, descriptor.tileMode, elementBytes, tiled.Handle(), geometry.GuestLayerOffset(layer) + mip.tiledOffset, linear.Handle(), geometry.LinearLayerOffset(layer) + mip.linearOffset, mip, false, layer, geometry.thick);
                }
            }
            const auto linearRead = WholeBufferBarrier(linear.Handle(), VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = image;
            toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, descriptor.mipCount, 0, geometry.imageLayers};
            context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &linearRead, 1, &toTransfer);
            const auto regions = CopyRegions();
            context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, linear.Handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(regions.size()), regions.data());
            VkImageMemoryBarrier toGeneral = toTransfer;
            toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
            APS5_LOG_CHARS_OUT("StorageTexture upload submit");
            batch.SubmitAndWait();
            APS5_LOG_CHARS_OUT("StorageTexture upload done");
    }
    countStorageUpload(2, guestBytes);
    ++version;
    if (profile) LookupOutcomes::Add(LookupOutcomes::UploadCpu, start);
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> StorageTexture::unitRuns(const std::vector<bool>& units) const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
    for (std::uint32_t unit = 0; unit < trackedLayers; ++unit) {
        if (!units[unit]) continue;
        const auto begin = static_cast<std::uint64_t>(unit) * trackedLayerBytes;
        const auto end = begin + layerBytes(unit);
        if (!runs.empty() && runs.back().second == begin) runs.back().second = end;
        else runs.emplace_back(begin, end);
    }
    return runs;
}

std::vector<StorageTexture::SliceWindow> StorageTexture::sliceWindows(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs) const {
    std::vector<SliceWindow> windows;
    const auto elementBytes = BytesPerElement(descriptor.format);
    const auto block = ThinBlockLayout(descriptor.tileMode, elementBytes);
    for (const auto& [runBegin, runEnd] : runs) {
        for (std::uint32_t layer = 0; layer < arrayLayers; ++layer) {
            for (std::uint32_t level = 0; level < descriptor.mipCount; ++level) {
                if (!geometry.HasLayer(level, layer)) continue;
                const auto& mip = mips[level];
                const auto sliceBegin = geometry.GuestLayerOffset(layer) + mip.tiledOffset;
                const auto sliceEnd = sliceBegin + mip.tiledSize;
                if (runEnd <= sliceBegin || sliceEnd <= runBegin) continue;
                SliceWindow window{layer, level, 0, 0, {}, 0, {}};
                const auto pitch = static_cast<std::uint32_t>(mip.pitchBytes / elementBytes * BlockWidth(descriptor.format));
                if (mip.tail) {

                    window.tiledBegin = sliceBegin;
                    window.tiledEnd = sliceEnd;
                    window.window = {0, static_cast<std::uint32_t>(mip.tiledSize), 0, 0, mip.linearSize};
                    window.linearBytes = mip.linearSize;
                    VkBufferImageCopy region{};
                    region.bufferRowLength = pitch;
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, geometry.CopyLayer(layer), 1};
                    region.imageOffset = {0, 0, geometry.CopyDepth(layer)};
                    region.imageExtent = {mip.width, mip.height, 1u};
                    window.regions.push_back(region);
                    windows.push_back(std::move(window));
                    continue;
                }
                const auto low = std::max(runBegin, sliceBegin) - sliceBegin;
                const auto high = std::min(runEnd, sliceEnd) - sliceBegin;
                Require(low % block[0] == 0 && high % block[0] == 0, "storage image window is not tile-block aligned");
                const auto firstBlock = low / block[0];
                const auto lastBlock = (high - 1) / block[0];
                const auto firstRow = static_cast<std::uint32_t>(firstBlock / mip.blocksPerRow);
                const auto lastRow = static_cast<std::uint32_t>(lastBlock / mip.blocksPerRow);
                const auto rowBegin = firstRow * block[2];
                const auto rowEnd = std::min(mip.height, (lastRow + 1u) * block[2]);
                window.tiledBegin = sliceBegin + low;
                window.tiledEnd = sliceBegin + high;
                window.linearBytes = static_cast<std::uint64_t>(rowEnd - rowBegin) * mip.pitchBytes;
                window.window = {static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(rowBegin) * mip.pitchBytes, window.linearBytes};

                window.window.rowBegin = rowBegin;
                window.window.rowEnd = rowEnd;
                if (firstRow == lastRow) {
                    window.window.columnBegin = static_cast<std::uint32_t>(firstBlock - static_cast<std::uint64_t>(firstRow) * mip.blocksPerRow) * block[1];
                    window.window.columnEnd = std::min(mip.width, static_cast<std::uint32_t>(lastBlock - static_cast<std::uint64_t>(firstRow) * mip.blocksPerRow + 1u) * block[1]);
                }

                for (auto row = firstRow; row <= lastRow; ++row) {
                    const auto rowFirst = std::max<std::uint64_t>(firstBlock, static_cast<std::uint64_t>(row) * mip.blocksPerRow);
                    const auto rowLast = std::min<std::uint64_t>(lastBlock, static_cast<std::uint64_t>(row + 1u) * mip.blocksPerRow - 1u);
                    const auto x0 = static_cast<std::uint32_t>(rowFirst - static_cast<std::uint64_t>(row) * mip.blocksPerRow) * block[1];
                    const auto x1 = std::min(mip.width, static_cast<std::uint32_t>(rowLast - static_cast<std::uint64_t>(row) * mip.blocksPerRow + 1u) * block[1]);
                    const auto y0 = row * block[2];
                    const auto y1 = std::min(mip.height, (row + 1u) * block[2]);
                    if (x0 >= x1 || y0 >= y1) continue;
                    if (!window.regions.empty()) {
                        auto& previous = window.regions.back();
                        const bool wholeRows = previous.imageOffset.x == 0 && previous.imageExtent.width == mip.width && x0 == 0 && x1 == mip.width;
                        if (wholeRows && static_cast<std::uint32_t>(previous.imageOffset.y) + previous.imageExtent.height == y0) {
                            previous.imageExtent.height += y1 - y0;
                            continue;
                        }
                    }
                    VkBufferImageCopy region{};
                    region.bufferOffset = static_cast<std::uint64_t>(y0 - rowBegin) * mip.pitchBytes + static_cast<std::uint64_t>(x0) * elementBytes;
                    region.bufferRowLength = pitch;
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, geometry.CopyLayer(layer), 1};
                    region.imageOffset = {static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0), geometry.CopyDepth(layer)};
                    region.imageExtent = {x1 - x0, y1 - y0, 1u};
                    window.regions.push_back(region);
                }
                windows.push_back(std::move(window));
            }
        }
    }
    return windows;
}

std::uint64_t StorageTexture::uploadWindows(const HostImport& import, std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, bool discard) {
    const auto elementBytes = BytesPerElement(descriptor.format);

    std::vector<std::pair<std::uint64_t, std::uint64_t>> tailBlocks;
    for (std::uint32_t layer = 0; layer < arrayLayers; ++layer) {
        for (std::uint32_t level = 0; level < descriptor.mipCount; ++level) {
            if (!geometry.HasLayer(level, layer)) continue;
            const auto& mip = mips[level];
            if (!mip.tail) continue;
            const auto sliceBegin = geometry.GuestLayerOffset(layer) + mip.tiledOffset;
            if (tailBlocks.empty() || tailBlocks.back().first != sliceBegin) tailBlocks.emplace_back(sliceBegin, sliceBegin + mip.tiledSize);
        }
    }
    const auto sources = ShadowSources(context, import, descriptor.baseAddress, runs, tailBlocks);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pieces;
    bool allShadow = true;
    for (const auto& source : sources) {
        pieces.emplace_back(source.begin, source.end);
        if (!source.shadow) allShadow = false;
    }
    if (allShadow && !sources.empty() && (std::strcmp(uploadReason, "flushed") == 0 || std::strcmp(uploadReason, "store") == 0)) uploadReason = "shadow";
    auto windows = sliceWindows(pieces);
    Require(!windows.empty(), "storage image upload selects no unit");
    const auto sourceOf = [&](const SliceWindow& window) -> const ShadowRun& {
        for (const auto& source : sources) {
            if (source.begin <= window.tiledBegin && window.tiledBegin < source.end) return source;
        }
        throw std::runtime_error("AGC graphics: storage image window has no source");
    };

    std::vector<std::uint64_t> positions;
    std::uint64_t linearTotal = 0, uploadedBytes = 0;
    for (const auto& window : windows) {
        positions.push_back(linearTotal);
        linearTotal += (window.linearBytes + 255) & ~std::uint64_t{255};
    }
    for (const auto& [begin, end] : runs) uploadedBytes += end - begin;
    auto linear = std::make_shared<DeviceBuffer>(context, static_cast<std::size_t>(linearTotal), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    detiler.BeginBatch();
    auto* recorder = Recorder::Active();
    std::unique_ptr<CommandBatch> batch;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    auto timing = Recorder::NoTiming;
    if (recorder != nullptr) {

        std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
        for (const auto& source : sources) {
            if (!source.shadow) reads.emplace_back(descriptor.baseAddress + source.begin, descriptor.baseAddress + source.end);
        }

        for (const auto& [begin, end] : reads) recorder->FlushStoresOverlapping(begin, static_cast<std::size_t>(end - begin));
        commands = recorder->Commands();
        timing = recorder->BeginGpuTiming(Recorder::CommandClass::StorageUpload);
        recorder->Keep(linear);
        for (const auto& source : sources) {
            if (source.slab != nullptr) recorder->Keep(source.slab);
        }
        if (auto self = weak_from_this().lock()) recorder->Keep(std::move(self));
        if (!reads.empty()) recorder->NotePendingReads(reads, Recorder::ReadKind::StorageUpload);
        Recorder::CountBarriers(Recorder::CommandClass::StorageUpload, 3);
        if (Recorder::BarrierValidate()) {
            const std::pair<VkImage, bool> written{image, true};
            recorder->NoteAccess(Recorder::CommandClass::StorageUpload, Recorder::Access{reads, {}, std::span(&written, 1), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT});
        }
    } else {
        batch = std::make_unique<CommandBatch>(context);
        commands = batch->Handle();
    }
    const auto importOffset = descriptor.baseAddress - import.base;
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    std::vector<VkBufferImageCopy> regions;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const auto& window = windows[i];
        const auto& mip = mips[window.level];
        const auto& source = sourceOf(window);
        auto detile = window.window;
        if (source.shadow) {

            detiler.Dispatch(commands, descriptor.tileMode, elementBytes, source.buffer, source.offset + (window.tiledBegin - source.begin), linear->Handle(), positions[i], mip, false, window.layer, false, detile);
        } else {

            detile.tiledBase = 0;
            detiler.Dispatch(commands, descriptor.tileMode, elementBytes, import.buffer, importOffset + geometry.GuestLayerOffset(window.layer) + mip.tiledOffset, linear->Handle(), positions[i], mip, false, window.layer, false, detile);
        }
        for (auto region : window.regions) {
            region.bufferOffset += positions[i];
            regions.push_back(region);
        }
    }
    const auto linearRead = WholeBufferBarrier(linear->Handle(), VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = discard ? 0u : VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    toTransfer.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, descriptor.mipCount, 0, geometry.imageLayers};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &linearRead, 1, &toTransfer);
    context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, linear->Handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(regions.size()), regions.data());
    VkImageMemoryBarrier toGeneral = toTransfer;
    toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
    if (batch) batch->SubmitAndWait();
    else recorder->EndGpuTiming(timing, uploadedBytes);
    return uploadedBytes;
}

std::uint64_t StorageTexture::writeBackWindows(const HostImport& import, std::span<const std::pair<std::uint64_t, std::uint64_t>> keep, std::uint64_t firstStored, std::uint64_t lastStored, std::vector<ShadowedRange>& shadowed, std::vector<std::pair<std::uint64_t, std::uint64_t>>& imported) {
    const auto elementBytes = BytesPerElement(descriptor.format);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
    std::uint64_t storedBytes = 0;
    for (const auto& [from, to] : keep) {
        runs.emplace_back(from - descriptor.baseAddress, to - descriptor.baseAddress);
        storedBytes += to - from;
    }
    if (runs.empty()) return 0;
    auto windows = sliceWindows(runs);

    std::vector<std::uint64_t> linearPositions, scratchPositions;
    std::uint64_t linearTotal = 0, scratchTotal = 0;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const auto& window = windows[i];
        linearPositions.push_back(linearTotal);
        linearTotal += (window.linearBytes + 255) & ~std::uint64_t{255};
        std::size_t shared = 0;
        while (shared < i && (windows[shared].tiledBegin != window.tiledBegin || windows[shared].tiledEnd != window.tiledEnd)) ++shared;
        if (shared < i) {
            scratchPositions.push_back(scratchPositions[shared]);
            continue;
        }
        scratchPositions.push_back(scratchTotal);
        scratchTotal += (window.tiledEnd - window.tiledBegin + 255) & ~std::uint64_t{255};
    }

    struct SlabPieces {
        std::shared_ptr<ShadowSlab> slab;
        std::shared_ptr<ShadowSlabPin> pin;
        std::vector<VkBufferCopy> copies;
    };
    std::vector<SlabPieces> slabPieces;
    std::vector<VkBufferCopy> importCopies;
    std::vector<ShadowedRange> seeds;
    const auto addPiece = [&](std::uint64_t scratchOffset, std::uint64_t guestBegin, std::uint64_t guestEnd) {
        while (guestBegin < guestEnd) {
            const auto pieceEnd = UnitShadowEnabled() ? std::min(guestEnd, SlabBoundary(import, guestBegin)) : guestEnd;
            const auto destination = ShadowDestinationFor(context, import, guestBegin, pieceEnd);
            if (destination.has_value()) {
                if (slabPieces.empty() || slabPieces.back().slab != destination->slab) slabPieces.push_back({destination->slab, destination->pin, {}});
                slabPieces.back().copies.push_back({scratchOffset, destination->offset, pieceEnd - guestBegin});
                for (const auto& [seedBegin, seedEnd] : destination->seedUnits) {
                    if (std::none_of(seeds.begin(), seeds.end(), [&](const ShadowedRange& seed) { return seed.begin == seedBegin; })) seeds.push_back({seedBegin, seedEnd, destination->slab});
                }
                if (!shadowed.empty() && shadowed.back().end == guestBegin && shadowed.back().slab == destination->slab) shadowed.back().end = pieceEnd;
                else shadowed.push_back({guestBegin, pieceEnd, destination->slab});
            } else {
                importCopies.push_back({scratchOffset, guestBegin - import.base, pieceEnd - guestBegin});
                if (!imported.empty() && imported.back().second == guestBegin) imported.back().second = pieceEnd;
                else imported.emplace_back(guestBegin, pieceEnd);
            }
            scratchOffset += pieceEnd - guestBegin;
            guestBegin = pieceEnd;
        }
    };
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const auto& window = windows[i];

        if (i != 0 && scratchPositions[i] == scratchPositions[i - 1]) continue;
        for (const auto& [from, to] : runs) {
            const auto begin = std::max(from, window.tiledBegin);
            const auto end = std::min(to, window.tiledEnd);
            if (begin >= end) continue;
            addPiece(scratchPositions[i] + (begin - window.tiledBegin), descriptor.baseAddress + begin, descriptor.baseAddress + end);
        }
    }
    std::sort(seeds.begin(), seeds.end(), [](const ShadowedRange& a, const ShadowedRange& b) { return a.begin < b.begin; });

    auto flushBegin = firstStored;
    auto flushEnd = lastStored;
    for (const auto& seed : seeds) {
        flushBegin = std::min(flushBegin, seed.begin);
        flushEnd = std::max(flushEnd, seed.end);
    }
    auto linear = std::make_shared<DeviceBuffer>(context, static_cast<std::size_t>(linearTotal), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto tiledScratch = std::make_shared<DeviceBuffer>(context, static_cast<std::size_t>(scratchTotal), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    detiler.BeginBatch();
    auto* recorder = Recorder::Active();
    std::unique_ptr<CommandBatch> batch;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkAccessFlags covered = 0;
    auto timing = Recorder::NoTiming;
    if (recorder != nullptr) {
        recorder->FlushKeyStoresOverlapping(flushBegin, static_cast<std::size_t>(flushEnd - flushBegin));
        recorder->FlushStoresOverlapping(flushBegin, static_cast<std::size_t>(flushEnd - flushBegin));
        commands = recorder->Commands(&covered);
        timing = recorder->BeginGpuTiming(Recorder::CommandClass::StorageWriteBack);
        recorder->Keep(linear);
        recorder->Keep(tiledScratch);
        for (const auto& pieces : slabPieces) recorder->Keep(pieces.slab);
        if (auto self = weak_from_this().lock()) recorder->Keep(std::move(self));
        Recorder::CountBarriers(Recorder::CommandClass::StorageWriteBack, 4);
        if (Recorder::BarrierValidate()) {
            const std::pair<VkImage, bool> read{image, false};
            std::vector<std::pair<std::uint64_t, std::uint64_t>> seedReads;
            for (const auto& seed : seeds) seedReads.emplace_back(seed.begin, seed.end);
            recorder->NoteAccess(Recorder::CommandClass::StorageWriteBack, Recorder::Access{seedReads, imported, std::span(&read, 1), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT});
        }
    } else {
        batch = std::make_unique<CommandBatch>(context);
        commands = batch->Handle();
    }
    VkImageMemoryBarrier toSource{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSource.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSource.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSource.image = image;
    toSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, descriptor.mipCount, 0, geometry.imageLayers};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSource);
    if (!seeds.empty()) {

        constexpr VkAccessFlags transferAccess = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        if (recorder != nullptr && (covered & transferAccess) == transferAccess && Recorder::MergeBarriers()) {
            Recorder::CountMerged(Recorder::CommandClass::StorageWriteBack);
        } else {
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, transferAccess);
            Recorder::CountBarriers(Recorder::CommandClass::StorageWriteBack);
        }
        for (const auto& [begin, end, slab] : seeds) {

            const VkBufferCopy seed{begin - import.base, SlabOffset(import, *slab, begin), end - begin};
            context.Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(commands, import.buffer, slab->buffer, 1, &seed);
            NoteShadowSeed(begin, end);
        }
    }
    std::vector<VkBufferImageCopy> regions;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        for (auto region : windows[i].regions) {
            region.bufferOffset += linearPositions[i];
            regions.push_back(region);
        }
    }
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, linear->Handle(), static_cast<std::uint32_t>(regions.size()), regions.data());
    const auto linearRead = WholeBufferBarrier(linear->Handle(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    const VkMemoryBarrier importReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &importReady, 1, &linearRead, 0, nullptr);
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const auto& window = windows[i];
        detiler.Dispatch(commands, descriptor.tileMode, elementBytes, linear->Handle(), linearPositions[i], tiledScratch->Handle(), scratchPositions[i], mips[window.level], true, window.layer, false, window.window);
    }
    {

        const VkMemoryBarrier scratchDone{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &scratchDone, 0, nullptr, 0, nullptr);
        const auto copyBuffer = context.Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer");
        for (const auto& pieces : slabPieces) copyBuffer(commands, tiledScratch->Handle(), pieces.slab->buffer, static_cast<std::uint32_t>(pieces.copies.size()), pieces.copies.data());
        if (!importCopies.empty()) copyBuffer(commands, tiledScratch->Handle(), import.buffer, static_cast<std::uint32_t>(importCopies.size()), importCopies.data());
    }
    VkImageMemoryBarrier backToGeneral = toSource;
    backToGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    backToGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    backToGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    backToGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;

    constexpr VkAccessFlags storedAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    const VkMemoryBarrier stored{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, storedAccess};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &stored, 0, nullptr, 1, &backToGeneral);
    if (batch) {
        batch->SubmitAndWait();
    } else {
        recorder->EndGpuTiming(timing, storedBytes);
        recorder->MarkCovered(storedAccess);

        if (!imported.empty()) recorder->NotePendingWrites(imported);
    }
    partialWriteBacks.fetch_add(1, std::memory_order_relaxed);
    partialWriteBackBytes.fetch_add(storedBytes, std::memory_order_relaxed);
    return storedBytes;
}

std::vector<VkBufferImageCopy> StorageTexture::CopyRegions(const std::vector<bool>* layers) const {
    std::vector<VkBufferImageCopy> regions;
    for (std::uint32_t layer = 0; layer < arrayLayers; ++layer) {
        if (layers != nullptr && !(*layers)[layer]) continue;
        for (std::uint32_t level = 0; level < descriptor.mipCount; ++level) {
            if (!geometry.HasLayer(level, layer)) continue;
            const auto& mip = mips[level];
            VkBufferImageCopy region{};
            region.bufferOffset = layer * sliceLinearBytes + mip.linearOffset;
            region.bufferRowLength = mip.pitchBytes / BytesPerElement(descriptor.format) * BlockWidth(descriptor.format);
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, geometry.CopyLayer(layer), 1};
            region.imageOffset = {0, 0, geometry.CopyDepth(layer)};
            region.imageExtent = {std::max(descriptor.width >> level, 1u), std::max(descriptor.height >> level, 1u), 1u};
            regions.push_back(region);
        }
    }
    return regions;
}

bool StorageTexture::overlaps(std::uint64_t address, std::size_t bytes) const {
    return address < descriptor.baseAddress + guestBytes && descriptor.baseAddress < address + bytes;
}

void StorageTexture::MarkDirty() {
    markLayersPending(0, trackedLayers);
}

bool StorageTexture::anyLayerPending() const {
    return std::any_of(layerPending.begin(), layerPending.end(), [](bool pending) { return pending; });
}

void StorageTexture::refreshGeneration() {
    generation = *std::min_element(layerGeneration.begin(), layerGeneration.end());
}

void StorageTexture::markLayersPending(std::uint32_t first, std::uint32_t count) {
    CaptureTrace::Log("image-write image=%llx first=%u count=%u generation=%llu", static_cast<unsigned long long>(descriptor.baseAddress), first, count, static_cast<unsigned long long>(generation));
    static const bool eager = std::getenv("APS5_EAGER_WRITEBACK") != nullptr || std::getenv("APS5_NO_TEXTURE_CACHE") != nullptr;
    for (std::uint32_t layer = first; layer < first + count; ++layer) layerPending[layer] = true;

    if (auto source = borrowedFrom.lock(); source != nullptr && borrowedUnits.size() == trackedLayers) {
        bool superseded = false;
        for (std::uint32_t unit = first; unit < first + count; ++unit) {
            if (!borrowedUnits[unit]) continue;
            borrowedUnits[unit] = false;
            if (source->version != borrowedVersion || !source->layerPending[unit]) continue;
            source->layerPending[unit] = false;
            unitsSuperseded.fetch_add(1, std::memory_order_relaxed);
            superseded = true;
        }
        if (superseded) source->reconcilePending();
    }
    if (eager) {
        WriteBack();
        return;
    }
    auto& pending = Pending();
    std::lock_guard lock(pending.mutex);

    if (Recorder::Active() == nullptr) {
        static const bool hooked = [] {
            GuestMemory::SetFlushHook(&FlushHook);
            return true;
        }();
        static_cast<void>(hooked);
    }
    ++version;
    const bool wasLent = std::exchange(lent, false);
    if (dirty) {
        if (wasLent) BumpPendingSerial();
        return;
    }
    dirty = true;
    pending.textures.push_back(this);
    BumpPendingSerial();
}

void StorageTexture::reconcilePending() {
    auto& pending = Pending();
    std::lock_guard lock(pending.mutex);
    const bool any = anyLayerPending();
    if (any == dirty) return;
    dirty = any;
    if (any) pending.textures.push_back(this);
    else pending.textures.remove(this);
    BumpPendingSerial();
}

void StorageTexture::Flush() {
    {
        auto& live = Live();
        std::lock_guard lock(live.mutex);
        released = true;
    }
    {
        auto& pending = Pending();
        std::lock_guard lock(pending.mutex);
        if (!dirty) return;
        dirty = false;
        pending.textures.remove(this);
        BumpPendingSerial();
    }
    std::lock_guard gpu(GuestMemory::GpuMutex());
    const auto previous = std::exchange(flushReason, "cache eviction");
    writeBack(descriptor.baseAddress, static_cast<std::size_t>(guestBytes));
    flushReason = previous;
}

bool StorageTexture::pendingUnitInside(std::uint64_t address, std::size_t bytes) const {
    static const bool pretest = std::getenv("APS5_NO_FLUSH_PRETEST") == nullptr;
    if (!pretest) return true;
    const bool whole = descriptor.dccAddress != 0 && uploadedKeys != DccKeys::Uncompressed;
    for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
        if (!layerPending[layer]) continue;
        if (whole) return true;
        const auto begin = layerBegin(layer);
        if (address < begin + layerBytes(layer) && begin < address + bytes) return true;
    }
    return false;
}

bool StorageTexture::FlushPending(std::uint64_t address, std::size_t bytes, const StorageTexture* except, const char* reason, PublishScope scope, bool* published) {
    if (published != nullptr) *published = false;

    std::vector<std::shared_ptr<StorageTexture>> flush;
    {
        auto& pending = Pending();
        std::lock_guard lock(pending.mutex);
        const auto* exempt = refreshing;
        for (auto it = pending.textures.MayOverlap(address, bytes) ? pending.textures.begin() : pending.textures.end(); it != pending.textures.end();) {
            auto* texture = *it;
            if (texture != except && texture != exempt && texture->overlaps(address, bytes)) {
                if (!texture->pendingUnitInside(address, bytes)) {
                    pretestSkipped.fetch_add(1, std::memory_order_relaxed);
                    ++it;
                    continue;
                }
                texture->dirty = false;
                if (const auto skips = texture->hookSkips.exchange(0, std::memory_order_relaxed); skips != 0 && texture->skippedResultsInside(address, bytes)) {
                    flushedAfterSkip.fetch_add(1, std::memory_order_relaxed);
                    if (PublishReasonFor(reason) == PublishReason::Hook && texture->hookSkipSite.load(std::memory_order_relaxed) == static_cast<std::uint8_t>(GuestMemory::CurrentReadSite())) flushedAfterSkipSameSite.fetch_add(1, std::memory_order_relaxed);
                }
                if (auto alive = texture->weak_from_this().lock()) flush.push_back(std::move(alive));
                it = pending.textures.erase(it);
            } else {
                ++it;
            }
        }

        for (const auto& texture : flush) pending.flushing.push_back(texture.get());
        if (!flush.empty()) BumpPendingSerial();
    }
    if (flush.empty()) {

        const bool units = PublishShadowsOnly(address, bytes, scope, reason);
        if (published != nullptr) *published = units;
        return false;
    }
    struct Unregister {
        const std::vector<std::shared_ptr<StorageTexture>>& flush;
        ~Unregister() {
            auto& pending = Pending();
            std::lock_guard lock(pending.mutex);
            for (const auto& texture : flush) std::erase(pending.flushing, texture.get());
            BumpPendingSerial();
        }
    } unregister{flush};

    static const bool trace = std::getenv("APS5_TRACE_FLUSH") != nullptr;
    if (trace) {
        for (const auto& texture : flush) aps5::LogErr( "[flush] image 0x%llx+0x%llx for %s 0x%llx+0x%zx\n", static_cast<unsigned long long>(texture->descriptor.baseAddress), static_cast<unsigned long long>(texture->guestBytes), reason, static_cast<unsigned long long>(address), bytes);
    }
    std::lock_guard gpu(GuestMemory::GpuMutex());
    std::exception_ptr failure;
    const auto previousReason = std::exchange(flushReason, reason);
    for (const auto& texture : flush) {
        try {

            texture->writeBack(address, bytes);
        } catch (...) {
            if (!failure) failure = std::current_exception();
        }
    }
    flushReason = previousReason;
    if (failure) std::rethrow_exception(failure);

    if (scope != PublishScope::None) {
        const auto units = PublishShadow(address, bytes, scope, PublishReasonFor(reason));
        if (published != nullptr) *published = units != 0;
    }
    return true;
}

bool StorageTexture::PublishShadowsOnly(std::uint64_t address, std::size_t bytes, PublishScope scope, const char* reason) {
    if (scope == PublishScope::None || !AnyShadowedOverlaps(address, bytes)) return false;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    return PublishShadow(address, bytes, scope, PublishReasonFor(reason)) != 0;
}

void StorageTexture::FlushAllPending(const char* reason) {
    static_cast<void>(FlushPending(0, std::numeric_limits<std::size_t>::max(), nullptr, reason));
}

std::shared_ptr<StorageTexture> StorageTexture::FindPending(std::uint64_t address, std::uint64_t bytes) {
    auto& pending = Pending();
    std::lock_guard lock(pending.mutex);
    for (auto* texture : pending.textures) {

        if (texture->descriptor.baseAddress == address && texture->guestBytes >= bytes) return texture->weak_from_this().lock();
    }
    return nullptr;
}

bool PendingStorageOverlaps(std::uint64_t address, std::size_t bytes, const StorageTexture* except) {
    auto& pending = Pending();
    std::lock_guard lock(pending.mutex);
    if (!pending.textures.MayOverlap(address, bytes)) return false;
    for (const auto* texture : pending.textures) {

        const auto begin = texture->Descriptor().baseAddress;
        if (texture != except && address < begin + texture->GuestBytes() && begin < address + bytes) return true;
    }
    return false;
}

bool StorageTexture::AnyPendingOverlaps(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
    if (ranges.empty()) return false;
    auto& pending = Pending();
    std::lock_guard lock(pending.mutex);
    const auto overlapsAny = [&](const StorageTexture* texture) {
        for (const auto& [begin, end] : ranges) {
            if (end > begin && texture->overlaps(begin, static_cast<std::size_t>(end - begin))) return true;
        }
        return false;
    };
    for (const auto* texture : pending.textures) {
        if (overlapsAny(texture)) return true;
    }
    for (const auto* texture : pending.flushing) {
        if (overlapsAny(texture)) return true;
    }
    return false;
}

std::uint64_t StorageTexture::PendingSerial() {
    return pendingSerial.load(std::memory_order_acquire);
}

void StorageTexture::NoteProved() const {
    provedPresent.store(Recorder::Presents(), std::memory_order_relaxed);
}

std::vector<std::shared_ptr<StorageTexture>> StorageTexture::overlappingPending(std::uint64_t address, std::size_t bytes) {

    std::vector<std::shared_ptr<StorageTexture>> overlapping;
    auto& pending = Pending();
    std::lock_guard lock(pending.mutex);
    if (!pending.textures.MayOverlap(address, bytes)) return overlapping;
    const auto* exempt = refreshing;
    for (auto* texture : pending.textures) {
        if (texture == exempt || !texture->overlaps(address, bytes) || !texture->pendingUnitInside(address, bytes)) continue;
        if (auto alive = texture->weak_from_this().lock()) overlapping.push_back(std::move(alive));
    }
    return overlapping;
}

bool StorageTexture::blocksKept(std::span<const std::shared_ptr<StorageTexture>> images, std::uint64_t address, std::size_t bytes) {

    constexpr std::uint64_t block = 65536;
    const auto accessEnd = address + bytes;
    for (const auto& texture : images) {
        for (std::uint32_t layer = 0; layer < texture->trackedLayers; ++layer) {
            if (!texture->layerPending[layer]) continue;
            const auto begin = std::max(address, texture->layerBegin(layer));
            const auto end = std::min(accessEnd, texture->layerBegin(layer) + texture->layerBytes(layer));
            if (end <= begin) continue;
            if (texture->layerGeneration[layer] == 0) return false;
            for (auto at = begin & ~(block - 1); at < end; at += block) {
                const auto from = std::max(at, begin);
                const auto to = std::min(at + block, end);
                if (!GuestMemory::WrittenSince(from, static_cast<std::size_t>(to - from), texture->layerGeneration[layer])) return false;
            }
        }
    }
    return true;
}

void StorageTexture::ClassifyAccess(std::uint64_t address, std::size_t bytes, AccessClassification& out) {
    out = {};
    const auto overlapping = overlappingPending(address, bytes);
    if (overlapping.empty()) return;
    out.images = overlapping.size();
    const auto present = Recorder::Presents();
    for (const auto& texture : overlapping) {
        if (texture->provedPresent.load(std::memory_order_relaxed) + DeadImagePresents <= present) ++out.dead;
        else ++out.live;
    }
    if (bytes > ClassifyLimit) return;
    out.checked = true;
    GuestMemory::CollectWritesUncached(address, bytes);
    out.allCpuWritten = blocksKept(overlapping, address, bytes);
}

bool StorageTexture::AccessKeptByCpu(std::uint64_t address, std::size_t bytes, std::size_t* images, std::size_t* evicted) {
    if (evicted != nullptr) *evicted = 0;
    const auto overlapping = overlappingPending(address, bytes);
    if (images != nullptr) *images = overlapping.size();
    if (overlapping.empty() || bytes > ClassifyLimit) return false;

    GuestMemory::CollectWritesUncached(address, bytes);

    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Hook);
    std::lock_guard gpu(GuestMemory::GpuMutex());
    if (!blocksKept(overlapping, address, bytes)) return false;
    const auto site = static_cast<std::uint8_t>(GuestMemory::CurrentReadSite());
    std::size_t dropped = 0;
    for (const auto& texture : overlapping) {
        if (texture->evictStale(address, bytes, dropped)) continue;
        texture->hookSkips.fetch_add(1, std::memory_order_relaxed);
        texture->hookSkipSite.store(site, std::memory_order_relaxed);
        for (std::uint32_t layer = 0; layer < texture->trackedLayers; ++layer) {
            if (!texture->layerPending[layer] || address >= texture->layerBegin(layer) + texture->layerBytes(layer) || texture->layerBegin(layer) >= address + bytes) continue;
            texture->hookSkipLayer = layer;
            texture->hookSkipVersion = texture->version;
            texture->hookSkipGeneration = texture->layerGeneration[layer];
            break;
        }
    }
    if (evicted != nullptr) *evicted = dropped;
    return true;
}

bool StorageTexture::evictStale(std::uint64_t address, std::size_t bytes, std::size_t& dropped) {
    static const bool staleEvict = std::getenv("APS5_STALE_EVICT") != nullptr;
    constexpr std::uint64_t block = 65536;

    const bool whole = descriptor.dccAddress != 0 && uploadedKeys != DccKeys::Uncompressed;
    const bool all = whole || (staleEvict && provedPresent.load(std::memory_order_relaxed) + DeadImagePresents <= Recorder::Presents());
    const auto accessEnd = address + bytes;
    const auto selected = [&](std::uint32_t layer) {
        return layerPending[layer] && (all || (address < layerBegin(layer) + layerBytes(layer) && layerBegin(layer) < accessEnd));
    };
    std::uint64_t first = descriptor.baseAddress + guestBytes;
    std::uint64_t last = descriptor.baseAddress;
    bool beyond = false;
    for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
        if (!selected(layer)) continue;
        if (layerGeneration[layer] == 0) return false;
        const auto begin = layerBegin(layer);
        const auto end = begin + layerBytes(layer);
        first = std::min(first, begin);
        last = std::max(last, end);
        if (begin < (address & ~(block - 1)) || end > ((accessEnd + block - 1) & ~(block - 1))) beyond = true;
    }
    if (last <= first) return false;
    if (beyond) {

        const auto present = Recorder::Presents();
        if (staleCheckPresent == present) return false;
        staleCheckPresent = present;
        GuestMemory::CollectWritesUncached(first, static_cast<std::size_t>(last - first));
    }
    for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
        if (!selected(layer)) continue;
        const auto begin = layerBegin(layer);
        const auto end = begin + layerBytes(layer);
        for (auto at = begin & ~(block - 1); at < end; at += block) {
            const auto from = std::max(at, begin);
            if (!GuestMemory::WrittenSince(from, static_cast<std::size_t>(std::min(at + block, end) - from), layerGeneration[layer])) return false;
        }
    }

    for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
        if (selected(layer)) layerPending[layer] = false;
    }
    originalValid = false;
    if (!anyLayerPending()) {
        ++dropped;
        staleEvicted.fetch_add(1, std::memory_order_relaxed);
    }
    reconcilePending();
    return true;
}

bool StorageTexture::skippedResultsInside(std::uint64_t address, std::size_t bytes) const {
    const auto layer = hookSkipLayer;
    if (layer >= trackedLayers || !layerPending[layer] || version != hookSkipVersion || layerGeneration[layer] != hookSkipGeneration) return false;
    const bool whole = descriptor.dccAddress != 0 && uploadedKeys != DccKeys::Uncompressed;
    return whole || (address < layerBegin(layer) + layerBytes(layer) && layerBegin(layer) < address + bytes);
}

StorageTexture::HookSkipCounts StorageTexture::TakeHookSkipCounts() {
    return {flushedAfterSkip.exchange(0, std::memory_order_relaxed), flushedAfterSkipSameSite.exchange(0, std::memory_order_relaxed)};
}

void StorageTexture::BumpPendingSerial() {
    pendingSerial.fetch_add(1, std::memory_order_release);
}

bool StorageTexture::ScanPending(std::span<PendingQuery> queries) {
    auto& pending = Pending();
    std::lock_guard lock(pending.mutex);
    bool identities = true;
    for (auto& query : queries) {
        query.overlaps = false;
        query.found = nullptr;
        if (query.end <= query.begin) continue;
        const auto bytes = static_cast<std::size_t>(query.end - query.begin);
        for (const auto* texture : pending.textures) {
            if (query.found == nullptr && texture->descriptor.baseAddress == query.begin && texture->guestBytes >= bytes) query.found = texture;
            if (texture != query.except && texture->overlaps(query.begin, bytes)) query.overlaps = true;
        }
        for (const auto* texture : pending.flushing) {
            if (texture != query.except && texture->overlaps(query.begin, bytes)) query.overlaps = true;
        }
        if (query.pending != nullptr && query.found != query.pending) identities = false;
    }
    return identities;
}

std::size_t StorageTexture::DiscardPendingInside(std::uint64_t address, std::size_t bytes) {
    auto& pending = Pending();
    std::lock_guard lock(pending.mutex);
    std::size_t discarded = 0;
    const auto end = address + bytes;
    const auto* exempt = refreshing;
    for (auto it = pending.textures.begin(); it != pending.textures.end();) {
        auto* texture = *it;
        const auto begin = texture->descriptor.baseAddress;
        if (texture != exempt && begin >= address && begin + texture->guestBytes <= end) {

            texture->dirty = false;
            texture->layerPending.assign(texture->trackedLayers, false);
            texture->originalValid = false;
            it = pending.textures.erase(it);
            ++discarded;
        } else if (texture != exempt && texture->blockUnits && texture->overlaps(address, bytes)) {

            bool dropped = false;
            for (std::uint32_t unit = 0; unit < texture->trackedLayers; ++unit) {
                const auto unitBegin = texture->layerBegin(unit);
                if (!texture->layerPending[unit] || unitBegin < address || unitBegin + texture->layerBytes(unit) > end) continue;
                texture->layerPending[unit] = false;
                unitsDropped.fetch_add(1, std::memory_order_relaxed);
                dropped = true;
            }
            if (dropped) {
                texture->originalValid = false;
                ++discarded;
            }
            if (dropped && !texture->anyLayerPending()) {
                texture->dirty = false;
                it = pending.textures.erase(it);
                continue;
            }
            ++it;
        } else {
            ++it;
        }
    }
    if (discarded != 0) BumpPendingSerial();
    return discarded;
}

StorageTexture::FillCoverage StorageTexture::ClassifyFill(std::uint64_t address, std::size_t bytes) {
    FillCoverage coverage;
    auto& live = Live();
    std::lock_guard lock(live.mutex);
    std::vector<StorageTexture*> overlapping;
    for (auto* texture : live.textures) {
        if (!texture->released && texture->guestBytes != 0 && texture->overlaps(address, bytes)) overlapping.push_back(texture);
    }
    const auto end = address + bytes;
    StorageTexture* covered = nullptr;

    for (auto* texture : overlapping) {
        if (texture->descriptor.baseAddress == address && texture->guestBytes == bytes) {
            covered = texture;
            coverage.cover = FillCover::Exact;
            coverage.layer = WholeImage;
        }
    }

    static const bool layerMemo = std::getenv("APS5_NO_LAYER_REFUSAL_MEMO") == nullptr;
    for (auto* texture : overlapping) {
        if (covered != nullptr) break;

        const auto& geometry = texture->geometry;
        const auto base = texture->descriptor.baseAddress;
        if (geometry.layers < 2 || geometry.thick || geometry.imageDepth != 1 || geometry.layers != geometry.imageLayers || geometry.layerBytes != bytes || geometry.layerBytes * geometry.layers != texture->guestBytes || address < base) continue;
        if (layerMemo && texture->layerRefusedGeneration == texture->generation) continue;
        const auto offset = address - base;
        if (offset % bytes != 0 || offset / bytes >= geometry.layers) continue;
        covered = texture;
        coverage.cover = FillCover::Layer;
        coverage.layer = static_cast<std::uint32_t>(offset / bytes);
    }
    if (covered != nullptr) {
        coverage.image = covered->weak_from_this().lock();
        if (coverage.image == nullptr) {
            coverage.cover = FillCover::None;
        } else {
            for (const auto* texture : overlapping) {
                if (texture == covered) continue;
                ++coverage.others;
                if (texture->descriptor.baseAddress >= address && texture->descriptor.baseAddress + texture->guestBytes <= end) ++coverage.inside;
            }
        }
    } else if (overlapping.empty()) {
        constexpr std::uint64_t keyBytes = 256;
        for (const auto* texture : live.textures) {
            if (!texture->released && texture->descriptor.dccAddress == address && texture->guestBytes / keyBytes != 0 && bytes >= texture->guestBytes / keyBytes) coverage.cover = FillCover::Keys;
        }
    } else if (overlapping.size() > 1) {
        coverage.cover = FillCover::Several;
    } else {
        auto* single = overlapping.front();
        const auto begin = single->descriptor.baseAddress;
        const auto stop = begin + single->guestBytes;
        if (address >= begin && end <= stop) coverage.cover = FillCover::Inside;
        else if (address <= begin && end >= stop) coverage.cover = FillCover::Around;
        else coverage.cover = FillCover::Straddle;
    }

    static const bool trace = std::getenv("APS5_TRACE_FILL_COVER") != nullptr;
    static int traced = 0;
    const auto cover = coverage.cover;
    if (trace && (cover == FillCover::Inside || cover == FillCover::Around || cover == FillCover::Straddle || cover == FillCover::Several) && traced < 96) {
        ++traced;
        aps5::LogErr( "[fill-cover] fill 0x%llx+0x%zx meets %zu image(s):", static_cast<unsigned long long>(address), bytes, overlapping.size());
        for (const auto* texture : overlapping) {
            const auto& d = texture->descriptor;
            aps5::LogErr( " [0x%llx+0x%llx %ux%u mips %u layers %u dim %d tile %d format %u dcc 0x%llx%s]", static_cast<unsigned long long>(d.baseAddress), static_cast<unsigned long long>(texture->guestBytes), d.width, d.height, d.mipCount, texture->geometry.imageLayers, static_cast<int>(d.dimension), static_cast<int>(d.tileMode), d.format, static_cast<unsigned long long>(d.dccAddress), texture->dirty ? " dirty" : "");
        }
        aps5::LogErr( "\n");
    }
    return coverage;
}

std::size_t StorageTexture::NoteKeysFill(std::uint64_t address, std::size_t bytes, std::uint8_t key) {
    static const bool refillDisabled = std::getenv("APS5_NO_KEYS_REFILL") != nullptr;
    DccKeys keys = DccKeys::Mixed;
    switch (key) {
        case 0x00: keys = DccKeys::Clear0000; break;
        case 0x40: keys = DccKeys::Clear0001; break;
        case 0x80: keys = DccKeys::Clear1110; break;
        case 0xc0: keys = DccKeys::Clear1111; break;
        case 0x20: keys = DccKeys::ClearRegister; break;
        case 0xff: keys = DccKeys::Uncompressed; break;
        default: return 0;
    }
    constexpr std::uint64_t keyBytes = 256;
    static const bool traceKeys = std::getenv("APS5_TRACE_DCC_KEYS") != nullptr;
    auto& live = Live();
    std::lock_guard lock(live.mutex);
    std::size_t covered = 0;
    for (auto* texture : live.textures) {
        if (texture->released || texture->descriptor.dccAddress != address || texture->guestBytes / keyBytes == 0 || bytes < texture->guestBytes / keyBytes) continue;
        texture->filledKeys = keys;
        ++covered;
        if (traceKeys && IsDccClear(keys)) aps5::LogErr( "[dcc-keys] %s key fill over 0x%llx+0x%llx (uploaded %s, dirty %d)\n", DccKeysName(keys), static_cast<unsigned long long>(texture->descriptor.baseAddress), static_cast<unsigned long long>(texture->guestBytes), DccKeysName(texture->uploadedKeys), texture->dirty ? 1 : 0);
        if (refillDisabled || !IsDccClear(keys) || texture->uploadedKeys != keys) continue;
        texture->uploadedKeys = DccKeys::Uncompressed;
    }
    return covered;
}

std::size_t StorageTexture::ClearByKeysFill(std::uint64_t address, std::size_t bytes, std::uint8_t key) {
    static const bool enabled = [] { const char* text = std::getenv("APS5_KEYS_FILL_CLEAR"); return text != nullptr && std::strcmp(text, "1") == 0; }();
    if (!enabled) return 0;
    DccKeys keys = DccKeys::Mixed;
    switch (key) {
        case 0x00: keys = DccKeys::Clear0000; break;
        case 0x40: keys = DccKeys::Clear0001; break;
        case 0x80: keys = DccKeys::Clear1110; break;
        case 0xc0: keys = DccKeys::Clear1111; break;
        case 0x20: keys = DccKeys::ClearRegister; break;
        default: return 0;
    }
    constexpr std::uint64_t keyBytes = 256;
    std::vector<std::shared_ptr<StorageTexture>> covered;
    {
        auto& live = Live();
        std::lock_guard lock(live.mutex);
        for (auto* texture : live.textures) {
            if (texture->released || texture->descriptor.dccAddress != address || texture->guestBytes / keyBytes == 0 || bytes < texture->guestBytes / keyBytes) continue;
            if (auto shared = texture->weak_from_this().lock()) covered.push_back(std::move(shared));
        }
    }
    std::size_t cleared = 0;
    for (const auto& texture : covered) {
        if (texture->clearByKeysFill(keys, key)) ++cleared;
    }
    return cleared;
}

bool StorageTexture::clearByKeysFill(DccKeys keys, std::uint8_t key) {
    static const bool traceKeys = std::getenv("APS5_TRACE_DCC_KEYS") != nullptr;
    VkClearColorValue clearValue{};
    auto* recorder = Recorder::Active();
    const char* refusal = !ClearColorFor(storageFormat, keys, clearValue) ? (keys == DccKeys::ClearRegister ? "clear-register code" : "no clear value in the storage format") : recorder == nullptr ? "no recorder" : nullptr;
    if (refusal != nullptr) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) aps5::LogErr( "[dcc-keys] key fill 0x%02x (%s) over surface 0x%llx+0x%llx (guest format %u, vk format %d, keys 0x%llx) not applied at once: %s\n", key, DccKeysName(keys), static_cast<unsigned long long>(descriptor.baseAddress), static_cast<unsigned long long>(guestBytes), descriptor.format, static_cast<int>(storageFormat), static_cast<unsigned long long>(descriptor.dccAddress), refusal);
        return false;
    }
    std::size_t droppedUnits = 0;
    const bool wasDirty = dirty;
    {
        auto& pending = Pending();
        std::lock_guard lock(pending.mutex);
        droppedUnits = static_cast<std::size_t>(std::count(layerPending.begin(), layerPending.end(), true));
        layerPending.assign(trackedLayers, false);
        if (dirty) {
            dirty = false;
            pending.textures.remove(this);
            BumpPendingSerial();
        }
    }
    const auto commands = recorder->Commands();
    const auto timing = recorder->BeginGpuTiming(Recorder::CommandClass::DccClear);
    if (auto self = weak_from_this().lock()) recorder->Keep(std::move(self));
    Recorder::CountBarriers(Recorder::CommandClass::DccClear, 2);
    if (Recorder::BarrierValidate()) {
        const std::pair<VkImage, bool> clearedImage{image, true};
        recorder->NoteAccess(Recorder::CommandClass::DccClear, Recorder::Access{{}, {}, std::span(&clearedImage, 1), VK_PIPELINE_STAGE_TRANSFER_BIT});
    }
    VkImageMemoryBarrier toClear{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toClear.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toClear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toClear.image = image;
    toClear.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, descriptor.mipCount, 0, geometry.imageLayers};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toClear);
    context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1, &toClear.subresourceRange);
    VkImageMemoryBarrier toGeneral = toClear;
    toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
    recorder->EndGpuTiming(timing, guestBytes);
    originalValid = false;
    uploadedKeys = keys;
    filledKeys = DccKeys::Uncompressed;
    keyProof = {};
    forgetBorrowed(0, trackedLayers);
    layerGeneration.assign(trackedLayers, GuestMemory::CollectWrites(descriptor.baseAddress, static_cast<std::size_t>(guestBytes)));
    refreshGeneration();
    ++version;
    if (traceKeys) aps5::LogErr( "[dcc-keys] key fill %s clears 0x%llx+0x%llx at once (keys 0x%llx, %zu pending units dropped, dirty %d)\n", DccKeysName(keys), static_cast<unsigned long long>(descriptor.baseAddress), static_cast<unsigned long long>(guestBytes), static_cast<unsigned long long>(descriptor.dccAddress), droppedUnits, wasDirty ? 1 : 0);
    return true;
}

bool StorageTexture::FillClear(std::span<const std::uint32_t, 4> pattern, std::uint32_t layer, const char*& refusal) {

    static const bool trace = std::getenv("APS5_TRACE_FILL_COVER") != nullptr;
    struct TraceLayerRefusal {
        const StorageTexture& texture;
        std::span<const std::uint32_t, 4> pattern;
        std::uint32_t layer;
        const char*& refusal;
        bool cleared = false;
        ~TraceLayerRefusal() {
            static std::atomic<int> traced{0};
            if (cleared || !trace || layer == WholeImage || refusal == nullptr || traced.fetch_add(1) >= 64) return;
            const auto& d = texture.descriptor;
            aps5::LogErr( "[fill-cover] layer %u of 0x%llx+0x%llx (%ux%u mips %u layers %u, %llu bytes per layer, format %u tile %d dcc 0x%llx%s) refused: %s; pattern %08x %08x %08x %08x; generation %llu\n", layer, static_cast<unsigned long long>(d.baseAddress), static_cast<unsigned long long>(texture.guestBytes), d.width, d.height, d.mipCount, texture.geometry.imageLayers, static_cast<unsigned long long>(texture.geometry.layerBytes), d.format, static_cast<int>(d.tileMode), static_cast<unsigned long long>(d.dccAddress), texture.dirty ? ", dirty" : "", refusal, pattern[0], pattern[1], pattern[2], pattern[3], static_cast<unsigned long long>(texture.generation));
        }
    } traceRefusal{*this, pattern, layer, refusal};
    VkClearColorValue clearValue{};
    if (!FillClearColor(storageFormat, BytesPerElement(descriptor.format), pattern, clearValue)) {
        refusal = "pattern";
        return false;
    }
    if (layer != WholeImage && (layer >= geometry.imageLayers || geometry.thick || geometry.imageDepth != 1)) {
        refusal = "layer";
        return false;
    }
    if (descriptor.dccAddress != 0) {

        if (Recorder::SnapshotWriteOverlaps(descriptor.dccAddress, static_cast<std::size_t>(guestBytes / 256))) {
            refusal = "keys pending";
            return false;
        }
        if (TextureClearKeys(descriptor, guestBytes) != DccKeys::Uncompressed) {
            refusal = "keys";
            return false;
        }
    }
    if (HostImportFor(context, descriptor.baseAddress, static_cast<std::size_t>(guestBytes)) == nullptr) {
        refusal = "not imported";
        return false;
    }
    auto* recorder = Recorder::Active();
    if (recorder == nullptr) {
        refusal = "no recorder";
        return false;
    }
    const auto begin = layer == WholeImage ? descriptor.baseAddress : descriptor.baseAddress + geometry.GuestLayerOffset(layer);
    const auto bytes = static_cast<std::size_t>(layer == WholeImage ? guestBytes : geometry.layerBytes);

    if (layer != WholeImage && trackedLayers == 1) {

        GuestMemory::CollectWritesUncached(descriptor.baseAddress, static_cast<std::size_t>(guestBytes));
        const auto before = static_cast<std::size_t>(begin - descriptor.baseAddress);
        const auto after = static_cast<std::size_t>(descriptor.baseAddress + guestBytes - (begin + bytes));
        if ((before != 0 && !GuestMemory::UnchangedSince(descriptor.baseAddress, before, generation)) || (after != 0 && !GuestMemory::UnchangedSince(begin + bytes, after, generation))) {
            refusal = "surface changed";
            layerRefusedGeneration = generation;
            return false;
        }
    }
    if (layer != WholeImage && blockUnits && ((begin - descriptor.baseAddress) % trackedLayerBytes != 0 || bytes % trackedLayerBytes != 0)) {
        refusal = "layer alignment";
        return false;
    }

    bool aliasedKeys = false;
    {
        auto& live = Live();
        std::lock_guard lock(live.mutex);
        for (auto* texture : live.textures) {
            if (texture == this || texture->released || !texture->overlaps(begin, bytes)) continue;
            texture->originalValid = false;
            if (texture->descriptor.dccAddress != 0) aliasedKeys = true;
        }
    }

    static std::atomic<int> traced{0};
    if (trace && (descriptor.dccAddress != 0 || aliasedKeys) && traced.fetch_add(1) < 16) aps5::LogErr( "[fill-cover] clear of surface 0x%llx+0x%llx (format %u, keys 0x%llx%s): pattern %08x %08x %08x %08x\n", static_cast<unsigned long long>(descriptor.baseAddress), static_cast<unsigned long long>(guestBytes), descriptor.format, static_cast<unsigned long long>(descriptor.dccAddress), aliasedKeys ? ", another image over the memory has keys" : "", pattern[0], pattern[1], pattern[2], pattern[3]);

    PublishShadow(begin, bytes, PublishScope::PartialUnits, PublishReason::FillClear);
    GuestMemory::MarkWritten(begin, bytes);
    const auto commands = recorder->Commands();
    const auto timing = recorder->BeginGpuTiming(Recorder::CommandClass::FillClear);
    Recorder::CountBarriers(Recorder::CommandClass::FillClear, 2);
    if (auto self = weak_from_this().lock()) recorder->Keep(std::move(self));
    if (Recorder::BarrierValidate()) {
        const std::pair<VkImage, bool> cleared{image, true};
        recorder->NoteAccess(Recorder::CommandClass::FillClear, Recorder::Access{{}, {}, std::span(&cleared, 1), VK_PIPELINE_STAGE_TRANSFER_BIT});
    }
    VkImageMemoryBarrier toClear{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toClear.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    toClear.oldLayout = layer == WholeImage ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toClear.image = image;
    toClear.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, descriptor.mipCount, layer == WholeImage ? 0u : layer, layer == WholeImage ? geometry.imageLayers : 1u};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toClear);
    context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1, &toClear.subresourceRange);
    VkImageMemoryBarrier toGeneral = toClear;
    toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
    recorder->EndGpuTiming(timing, bytes);
    traceRefusal.cleared = true;

    originalValid = false;
    uploadedKeys = DccKeys::Uncompressed;
    if (layer != WholeImage && trackedLayers > 1) {

        const auto first = blockUnits ? static_cast<std::uint32_t>((begin - descriptor.baseAddress) / trackedLayerBytes) : layer;
        const auto count = blockUnits ? static_cast<std::uint32_t>(bytes / trackedLayerBytes) : 1u;
        const auto now = GuestMemory::CollectWritesUncached(begin, bytes);
        for (auto unit = first; unit < first + count; ++unit) layerGeneration[unit] = now;
        refreshGeneration();
        forgetBorrowed(first, count);
        markLayersPending(first, count);
        return true;
    }
    layerGeneration.assign(trackedLayers, GuestMemory::CollectWritesUncached(descriptor.baseAddress, static_cast<std::size_t>(guestBytes)));
    refreshGeneration();
    forgetBorrowed(0, trackedLayers);
    MarkDirty();
    return true;
}

void StorageTexture::WriteBack() {
    const auto previous = std::exchange(flushReason, "explicit");
    writeBack(descriptor.baseAddress, static_cast<std::size_t>(guestBytes));
    flushReason = previous;
}

std::shared_ptr<StorageTexture> StorageTexture::FindLive(std::uint64_t address, std::uint64_t bytes) {
    auto& live = Live();
    std::lock_guard lock(live.mutex);
    for (auto it = live.textures.rbegin(); it != live.textures.rend(); ++it) {
        auto* texture = *it;
        if (texture->released || !texture->Cached() || texture->descriptor.baseAddress != address || texture->guestBytes != bytes) continue;
        return texture->weak_from_this().lock();
    }
    return nullptr;
}

bool StorageTexture::SameSurfaceShape(const StorageTexture& other) const {
    const auto& mine = descriptor;
    const auto& theirs = other.descriptor;
    return guestBytes == other.guestBytes && mine.width == theirs.width && mine.height == theirs.height && mine.depthOrLastArray == theirs.depthOrLastArray && mine.baseArray == theirs.baseArray && mine.mipCount == theirs.mipCount && mine.tileMode == theirs.tileMode && mine.dimension == theirs.dimension && mine.format == theirs.format && storageFormat == other.storageFormat && geometry.imageLayers == other.geometry.imageLayers && geometry.imageDepth == other.geometry.imageDepth;
}

bool StorageTexture::CopyFrom(StorageTexture& source, const char*& refusal) {
    if (&source == this || !SameSurfaceShape(source)) {
        refusal = "shape";
        return false;
    }
    if (descriptor.dccAddress != 0 || source.descriptor.dccAddress != 0) {
        refusal = "keys";
        return false;
    }
    if (released || source.released || !Cached() || !source.Cached()) {
        refusal = "released";
        return false;
    }
    if (HostImportFor(context, descriptor.baseAddress, static_cast<std::size_t>(guestBytes)) == nullptr) {
        refusal = "not imported";
        return false;
    }
    auto* recorder = Recorder::Active();
    if (recorder == nullptr) {
        refusal = "no recorder";
        return false;
    }

    source.Refresh();

    static_cast<void>(DiscardPendingInside(descriptor.baseAddress, static_cast<std::size_t>(guestBytes)));
    FlushPending(descriptor.baseAddress, static_cast<std::size_t>(guestBytes), this, "buffer copy destination", PublishScope::PartialUnits);
    {
        auto& live = Live();
        std::lock_guard lock(live.mutex);
        for (auto* texture : live.textures) {
            if (texture != this && !texture->released && texture->overlaps(descriptor.baseAddress, static_cast<std::size_t>(guestBytes))) texture->originalValid = false;
        }
    }
    GuestMemory::MarkWritten(descriptor.baseAddress, static_cast<std::size_t>(guestBytes));
    const auto commands = recorder->Commands();
    const auto timing = recorder->BeginGpuTiming(Recorder::CommandClass::Copy);
    Recorder::CountBarriers(Recorder::CommandClass::Copy, 2);
    if (auto self = weak_from_this().lock()) recorder->Keep(std::move(self));
    if (auto other = source.weak_from_this().lock()) recorder->Keep(std::move(other));
    if (Recorder::BarrierValidate()) {
        const std::array<std::pair<VkImage, bool>, 2> images{{{source.image, false}, {image, true}}};
        recorder->NoteAccess(Recorder::CommandClass::Copy, Recorder::Access{{}, {}, images, VK_PIPELINE_STAGE_TRANSFER_BIT});
    }
    VkImageMemoryBarrier barriers[2]{};
    for (auto& barrier : barriers) {
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, descriptor.mipCount, 0, geometry.imageLayers};
    }
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].image = source.image;
    barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].image = image;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
    std::vector<VkImageCopy> regions;
    for (std::uint32_t layer = 0; layer < arrayLayers; ++layer) {
        for (std::uint32_t level = 0; level < descriptor.mipCount; ++level) {
            if (!geometry.HasLayer(level, layer)) continue;
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, geometry.CopyLayer(layer), 1};
            region.srcOffset = {0, 0, geometry.CopyDepth(layer)};
            region.dstSubresource = region.srcSubresource;
            region.dstOffset = region.srcOffset;
            region.extent = {std::max(descriptor.width >> level, 1u), std::max(descriptor.height >> level, 1u), 1u};
            regions.push_back(region);
        }
    }
    context.Function<PFN_vkCmdCopyImage>("vkCmdCopyImage")(commands, source.image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_GENERAL, static_cast<std::uint32_t>(regions.size()), regions.data());
    VkImageMemoryBarrier copied = barriers[1];
    copied.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copied.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &copied);
    recorder->EndGpuTiming(timing, guestBytes);

    originalValid = false;
    uploadedKeys = DccKeys::Uncompressed;
    layerGeneration.assign(trackedLayers, GuestMemory::CollectWritesUncached(descriptor.baseAddress, static_cast<std::size_t>(guestBytes)));
    refreshGeneration();
    forgetBorrowed(0, trackedLayers);
    MarkDirty();
    return true;
}

std::vector<StorageTexture::Adjacent> StorageTexture::adjacentPendingUnchanged(std::uint64_t firstBlock, std::uint64_t lastBlock) const {
    std::vector<Adjacent> adjacent;
    if (!AdjacentGenerationEnabled() || guestBytes == 0) return adjacent;
    constexpr std::uint64_t block = 65536;
    {
        auto& pending = Pending();
        std::lock_guard lock(pending.mutex);
        const auto consider = [&](StorageTexture* texture) {
            if (texture == this || texture->guestBytes == 0 || texture->overlaps(descriptor.baseAddress, static_cast<std::size_t>(guestBytes))) return;
            const auto begin = texture->descriptor.baseAddress;
            const auto neighbourFirst = begin / block;
            const auto neighbourLast = (begin + texture->guestBytes - 1) / block;
            if (neighbourFirst > lastBlock || neighbourLast < firstBlock) return;

            const auto shared = std::max(neighbourFirst, firstBlock);
            const auto layer = static_cast<std::uint32_t>((std::max(shared * block, begin) - begin) / texture->trackedLayerBytes);
            if (auto alive = texture->weak_from_this().lock()) adjacent.push_back({std::move(alive), layer, texture->layerGeneration[layer]});
        };
        for (auto* texture : pending.textures) consider(texture);
        for (auto* texture : pending.flushing) consider(texture);
    }

    std::erase_if(adjacent, [](const Adjacent& entry) {
        const auto& texture = entry.texture;
        const auto begin = texture->layerBegin(entry.layer);
        const auto bytes = static_cast<std::size_t>(texture->layerBytes(entry.layer));
        GuestMemory::CollectWritesUncached(begin, bytes);
        return !GuestMemory::UnchangedSince(begin, bytes, entry.generation);
    });
    return adjacent;
}

void StorageTexture::advanceAdjacent(const std::vector<Adjacent>& adjacent, std::uint64_t now, std::uint64_t firstBlock, std::uint64_t lastBlock) {
    static const bool trace = std::getenv("APS5_TRACE_FLUSH") != nullptr;
    constexpr std::uint64_t block = 65536;
    for (const auto& [texture, layer, seen] : adjacent) {

        if (texture->layerGeneration[layer] != seen || now <= seen) continue;

        const auto begin = texture->layerBegin(layer);
        const auto end = begin + texture->layerBytes(layer);
        const auto beforeEnd = std::min(end, firstBlock * block);
        const auto afterBegin = std::max(begin, (lastBlock + 1) * block);
        if (begin < beforeEnd && !GuestMemory::UnchangedSince(begin, static_cast<std::size_t>(beforeEnd - begin), seen)) continue;
        if (afterBegin < end && !GuestMemory::UnchangedSince(afterBegin, static_cast<std::size_t>(end - afterBegin), seen)) continue;
        if (trace) aps5::LogErr( "[flush] adjacent pending image 0x%llx+0x%llx layer %u advanced past a write-back's stamps (generation %llu -> %llu)\n", static_cast<unsigned long long>(texture->descriptor.baseAddress), static_cast<unsigned long long>(texture->guestBytes), layer, static_cast<unsigned long long>(seen), static_cast<unsigned long long>(now));
        texture->layerGeneration[layer] = now;
        texture->refreshGeneration();
    }
}

void StorageTexture::blockGenerations(std::vector<std::uint64_t>& generations) const {
    constexpr std::uint64_t block = 65536;
    const auto spanBegin = descriptor.baseAddress & ~(block - 1);
    generations.assign(static_cast<std::size_t>((descriptor.baseAddress + guestBytes - spanBegin + block - 1) / block), 0);
    for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
        const auto end = layerBegin(layer) + layerBytes(layer);
        for (auto at = layerBegin(layer) & ~(block - 1); at < end; at += block) generations[static_cast<std::size_t>((at - spanBegin) / block)] = layerGeneration[layer];
    }
}

std::shared_ptr<StorageTexture> StorageTexture::pendingAlias() const {
    static const bool disabled = std::getenv("APS5_NO_ALIAS_BORROW") != nullptr;
    if (disabled || !blockUnits) return nullptr;
    std::vector<std::shared_ptr<StorageTexture>> candidates;
    {
        auto& pending = Pending();
        std::lock_guard lock(pending.mutex);
        for (auto* texture : pending.textures) {
            if (texture == this || texture->descriptor.baseAddress != descriptor.baseAddress || texture->guestBytes != guestBytes || !texture->blockUnits) continue;
            if (auto alive = texture->weak_from_this().lock()) candidates.push_back(std::move(alive));
        }
    }
    for (const auto& candidate : candidates) {
        const auto& mine = descriptor;
        const auto& theirs = candidate->descriptor;

        const bool sameShape = mine.width == theirs.width && mine.height == theirs.height && mine.depthOrLastArray == theirs.depthOrLastArray && mine.baseArray == theirs.baseArray && mine.mipCount == theirs.mipCount && mine.tileMode == theirs.tileMode && mine.dimension == theirs.dimension && BytesPerElement(mine.format) == BytesPerElement(theirs.format) && geometry.imageLayers == candidate->geometry.imageLayers && geometry.imageDepth == candidate->geometry.imageDepth && trackedLayers == candidate->trackedLayers;
        if (sameShape && candidate->Cached() && candidate->uploadedKeys == DccKeys::Uncompressed) return candidate;
    }
    return nullptr;
}

void StorageTexture::forgetBorrowed(std::uint32_t first, std::uint32_t count) {
    if (borrowedUnits.empty()) return;
    for (auto unit = first; unit < first + count && unit < borrowedUnits.size(); ++unit) borrowedUnits[unit] = false;
    if (std::none_of(borrowedUnits.begin(), borrowedUnits.end(), [](bool held) { return held; })) {
        borrowedUnits.clear();
        borrowedFrom.reset();
    }
}

std::uint64_t StorageTexture::borrowUnits(StorageTexture& source, const std::vector<bool>& units) {
    const auto runs = unitRuns(units);
    const auto windows = sliceWindows(runs);
    Require(!windows.empty(), "storage image borrows no unit");
    std::vector<VkImageCopy> regions;
    for (const auto& window : windows) {
        for (const auto& region : window.regions) regions.push_back({region.imageSubresource, region.imageOffset, region.imageSubresource, region.imageOffset, region.imageExtent});
    }
    std::uint64_t bytes = 0;
    for (const auto& [begin, end] : runs) bytes += end - begin;
    auto* recorder = Recorder::Active();
    std::unique_ptr<CommandBatch> batch;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    auto timing = Recorder::NoTiming;
    if (recorder != nullptr) {
        commands = recorder->Commands();
        timing = recorder->BeginGpuTiming(Recorder::CommandClass::StorageUpload);
        if (auto self = weak_from_this().lock()) recorder->Keep(std::move(self));
        if (auto other = source.weak_from_this().lock()) recorder->Keep(std::move(other));
        Recorder::CountBarriers(Recorder::CommandClass::StorageUpload, 2);
        if (Recorder::BarrierValidate()) {
            const std::array<std::pair<VkImage, bool>, 2> images{{{source.image, false}, {image, true}}};
            recorder->NoteAccess(Recorder::CommandClass::StorageUpload, Recorder::Access{{}, {}, images, VK_PIPELINE_STAGE_TRANSFER_BIT});
        }
    } else {
        batch = std::make_unique<CommandBatch>(context);
        commands = batch->Handle();
    }
    VkImageMemoryBarrier barriers[2]{};
    for (auto& barrier : barriers) {
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, descriptor.mipCount, 0, geometry.imageLayers};
    }
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].image = source.image;
    barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].image = image;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
    context.Function<PFN_vkCmdCopyImage>("vkCmdCopyImage")(commands, source.image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_GENERAL, static_cast<std::uint32_t>(regions.size()), regions.data());
    VkImageMemoryBarrier copied = barriers[1];
    copied.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copied.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &copied);
    if (batch) batch->SubmitAndWait();
    else recorder->EndGpuTiming(timing, bytes);
    countStorageUpload(3, bytes);
    if (borrowedFrom.lock() != source.weak_from_this().lock() || borrowedUnits.size() != trackedLayers) {
        borrowedUnits.assign(trackedLayers, false);
        borrowedFrom = source.weak_from_this();
    }
    borrowedVersion = source.version;
    source.lent = true;
    for (std::uint32_t unit = 0; unit < trackedLayers; ++unit) {
        if (units[unit]) borrowedUnits[unit] = true;
    }
    ++version;
    return bytes;
}

void StorageTexture::writeBack(std::uint64_t address, std::size_t bytes) {

    static const bool each = std::getenv("APS5_BLOCK_WRITEBACK_EACH") != nullptr || std::getenv("APS5_BLOCK_WRITEBACK_WIDEN") == nullptr;
    static const std::uint32_t pieces = [] { const char* text = std::getenv("APS5_BLOCK_WRITEBACK_PIECES"); return text != nullptr ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10)) : 4u; }();
    static const std::uint64_t window = [] { const char* text = std::getenv("APS5_BLOCK_WRITEBACK_FRAMES"); return text != nullptr ? std::strtoull(text, nullptr, 10) : 8ull; }();
    std::vector<bool> layers(trackedLayers, false);
    std::size_t selected = 0, pendingCount = 0;

    const bool whole = descriptor.dccAddress != 0 && uploadedKeys != DccKeys::Uncompressed;
    for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
        if (!layerPending[layer]) continue;
        ++pendingCount;
        layers[layer] = whole || (address < layerBegin(layer) + layerBytes(layer) && layerBegin(layer) < address + bytes);
        if (layers[layer]) ++selected;
    }
    if (selected == 0) {
        reconcilePending();
        return;
    }
    if (blockUnits && !each && selected < pendingCount) {
        const auto present = Recorder::Presents();
        if (present - partialWindowStart >= window) {
            partialWindowStart = present;
            partialStores = 0;
        }
        if (present < storeWholeUntil || ++partialStores > pieces) {
            storeWholeUntil = present + window;
            layers = layerPending;
            coalescedWriteBacks.fetch_add(1, std::memory_order_relaxed);
        }
    }
    writeBackLayers(layers);
}

void StorageTexture::writeBackLayers(const std::vector<bool>& layers) {
    CaptureTrace::Log("writeback image=%llx generation=%llu reason=%s units=%zu", static_cast<unsigned long long>(descriptor.baseAddress), static_cast<unsigned long long>(generation), flushReason, static_cast<std::size_t>(std::count(layers.begin(), layers.end(), true)));
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    PhaseTimer timer;
    struct Account {
        bool enabled;
        PhaseTimer& timer;
        ~Account() { if (enabled) Profile().storageWriteBack += timer.lap(); }
    } account{profile, timer};

    struct Reconcile {
        StorageTexture& texture;
        ~Reconcile() { texture.reconcilePending(); }
    } reconcile{*this};
    const auto elementBytes = BytesPerElement(descriptor.format);

    std::vector<std::pair<std::uint64_t, std::uint64_t>> keep;
    std::vector<bool> skippedLayer(trackedLayers, false);
    bool skippedAny = false;
    std::size_t skipped = 0;
    std::uint64_t firstStored = descriptor.baseAddress + guestBytes;
    std::uint64_t lastStored = descriptor.baseAddress;
    for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
        if (!layers[layer]) continue;
        firstStored = std::min(firstStored, layerBegin(layer));
        lastStored = std::max(lastStored, layerBegin(layer) + layerBytes(layer));
    }
    if (lastStored <= firstStored) return;
    constexpr std::uint64_t block = 65536;
    const auto spanBegin = firstStored & ~(block - 1);
    const auto spanBlocks = static_cast<std::size_t>((lastStored - spanBegin + block - 1) / block);

    GuestMemory::CollectWritesUncached(firstStored, static_cast<std::size_t>(lastStored - firstStored));
    std::vector<std::uint64_t> generations(spanBlocks, 0);
    for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
        if (!layers[layer]) continue;
        const auto end = layerBegin(layer) + layerBytes(layer);
        for (auto at = layerBegin(layer) & ~(block - 1); at < end; at += block) generations[static_cast<std::size_t>((at - spanBegin) / block)] = layerGeneration[layer];
    }
    std::vector<std::uint8_t> changedBlocks(spanBlocks);
    const bool tracked = GuestMemory::ChangedBlocks(firstStored, static_cast<std::size_t>(lastStored - firstStored), generations, changedBlocks);
    for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
        if (!layers[layer]) continue;
        const auto begin = layerBegin(layer);
        const auto end = begin + layerBytes(layer);
        if (!tracked || layerGeneration[layer] == 0) {
            keep.emplace_back(begin, end);
            continue;
        }
        for (auto at = begin & ~(block - 1); at < end; at += block) {
            const auto from = std::max(at, begin);
            const auto to = std::min(at + block, end);
            if (from >= to) continue;
            if (changedBlocks[static_cast<std::size_t>((at - spanBegin) / block)] == GuestMemory::BlockWritten) {
                skippedAny = true;
                skippedLayer[layer] = true;
                ++skipped;
                continue;
            }
            if (!keep.empty() && keep.back().second == from) keep.back().second = to;
            else keep.emplace_back(from, to);
        }
    }

    static const bool traceKept = std::getenv("APS5_TRACE_FLUSH") != nullptr;
    if (traceKept && skippedAny) {
        const auto first = keep.empty() ? descriptor.baseAddress + guestBytes : keep.front().first;
        aps5::LogErr( "[flush] image 0x%llx+0x%llx keeps %zu CPU-written 64 KiB blocks (first stored byte at +0x%llx, %zu ranges stored, generation %llu)\n", static_cast<unsigned long long>(descriptor.baseAddress), static_cast<unsigned long long>(guestBytes), skipped, static_cast<unsigned long long>(first - descriptor.baseAddress), keep.size(), static_cast<unsigned long long>(generation));
    }

    const auto firstBlock = firstStored / block;
    const auto lastBlock = (lastStored - 1) / block;
    const auto adjacent = adjacentPendingUnchanged(firstBlock, lastBlock);

    std::vector<ShadowedRange> shadowed;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> imported;
    const HostImport* shadowImport = nullptr;
    const auto settle = [&](bool memoizedCollect) {
        for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
            if (layers[layer]) layerPending[layer] = false;
        }
        const bool allKept = std::none_of(skippedLayer.begin(), skippedLayer.end(), [](bool kept) { return kept; });
        if (allKept || !adjacent.empty()) {
            const auto now = memoizedCollect ? GuestMemory::CollectWrites(firstStored, static_cast<std::size_t>(lastStored - firstStored)) : GuestMemory::CollectWritesUncached(firstStored, static_cast<std::size_t>(lastStored - firstStored));
            for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
                if (layers[layer] && !skippedLayer[layer]) layerGeneration[layer] = now;
            }
            refreshGeneration();
            advanceAdjacent(adjacent, now, firstBlock, lastBlock);
            if (!shadowed.empty()) MarkShadowed(*shadowImport, shadowed, now);
        } else if (!shadowed.empty()) {
            MarkShadowed(*shadowImport, shadowed, GuestMemory::TrackerGeneration());
        }
    };
    if (keep.empty()) {

        emptyWriteBacks.fetch_add(1, std::memory_order_relaxed);
        originalValid = false;
        settle(true);
        return;
    }
    if (const auto* import = HostImportFor(context, descriptor.baseAddress, static_cast<std::size_t>(guestBytes))) {
        if (blockUnits) {

            shadowImport = import;
            const auto storedBytes = writeBackWindows(*import, keep, firstStored, lastStored, shadowed, imported);
            countStorageWriteBack(storedBytes, true);
            if (profile) Profile().storageGpu += timer.lap();
            originalValid = false;
            traceKeyStore("block write-back", descriptor, guestBytes);
            if (!IsDccClear(filledKeys)) MarkDccUncompressed(context, descriptor.dccAddress, guestBytes);
            uploadedKeys = DccKeys::Uncompressed;
            for (const auto& [from, to] : keep) GuestMemory::MarkWritten(from, static_cast<std::size_t>(to - from));
            settle(true);
            ++Profile().storageDirectWriteBacks;
            return;
        }

        PublishShadow(firstStored, static_cast<std::size_t>(lastStored - firstStored), PublishScope::PartialUnits, PublishReason::Upload);

        auto linear = std::make_shared<DeviceBuffer>(context, static_cast<std::size_t>(sliceLinearBytes * arrayLayers), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto tiledScratch = std::make_shared<DeviceBuffer>(context, static_cast<std::size_t>(guestBytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        detiler.BeginBatch();
        auto* recorder = Recorder::Active();
        std::unique_ptr<CommandBatch> batch;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        auto timing = Recorder::NoTiming;
        std::uint64_t storedBytes = 0;
        for (const auto& [from, to] : keep) storedBytes += to - from;
        if (recorder != nullptr) {
            recorder->FlushKeyStoresOverlapping(firstStored, static_cast<std::size_t>(lastStored - firstStored));
            recorder->FlushStoresOverlapping(firstStored, static_cast<std::size_t>(lastStored - firstStored));
            commands = recorder->Commands();
            timing = recorder->BeginGpuTiming(Recorder::CommandClass::StorageWriteBack);
            recorder->Keep(linear);
            recorder->Keep(tiledScratch);

            if (auto self = weak_from_this().lock()) recorder->Keep(std::move(self));
            Recorder::CountBarriers(Recorder::CommandClass::StorageWriteBack, 4);
            if (Recorder::BarrierValidate()) {
                const std::pair<VkImage, bool> read{image, false};
                recorder->NoteAccess(Recorder::CommandClass::StorageWriteBack, Recorder::Access{{}, keep, std::span(&read, 1), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT});
            }
        } else {
            batch = std::make_unique<CommandBatch>(context);
            commands = batch->Handle();
        }
        const auto importOffset = descriptor.baseAddress - import->base;

        const auto* storedLayers = trackedLayers == arrayLayers && trackedLayers > 1 ? &layers : nullptr;
        VkImageMemoryBarrier toSource{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toSource.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSource.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSource.image = image;
        toSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, descriptor.mipCount, 0, geometry.imageLayers};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSource);
        const auto regions = CopyRegions(storedLayers);
        context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, linear->Handle(), static_cast<std::uint32_t>(regions.size()), regions.data());
        const auto linearRead = WholeBufferBarrier(linear->Handle(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        const VkMemoryBarrier importReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &importReady, 1, &linearRead, 0, nullptr);
        for (std::uint32_t layer = 0; layer < arrayLayers; ++layer) {
            if (storedLayers != nullptr && !(*storedLayers)[layer]) continue;
            for (std::uint32_t level = 0; level < mips.size(); ++level) {
                if (!geometry.HasLayer(level, layer)) continue;
                const auto& mip = mips[level];
                detiler.Dispatch(commands, descriptor.tileMode, elementBytes, linear->Handle(), geometry.LinearLayerOffset(layer) + mip.linearOffset, tiledScratch->Handle(), geometry.GuestLayerOffset(layer) + mip.tiledOffset, mip, true, layer, geometry.thick);
            }
        }
        {
            const auto scratchDone = WholeBufferBarrier(tiledScratch->Handle(), VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &scratchDone, 0, nullptr);
            std::vector<VkBufferCopy> copies;
            for (const auto& [from, to] : keep) copies.push_back({from - descriptor.baseAddress, importOffset + (from - descriptor.baseAddress), to - from});
            if (!copies.empty()) context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, tiledScratch->Handle(), import->buffer, static_cast<std::uint32_t>(copies.size()), copies.data());
        }
        VkImageMemoryBarrier backToGeneral = toSource;
        backToGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        backToGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        backToGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        backToGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        const VkMemoryBarrier stored{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &stored, 0, nullptr, 1, &backToGeneral);
        if (batch) {
            batch->SubmitAndWait();
        } else {
            recorder->EndGpuTiming(timing, storedBytes);
            recorder->MarkShaderReadsCovered();
            recorder->NotePendingWrite(firstStored, static_cast<std::size_t>(lastStored - firstStored));
        }
        countStorageWriteBack(storedBytes, true);
        if (profile) Profile().storageGpu += timer.lap();

        originalValid = false;

        traceKeyStore("layer write-back", descriptor, guestBytes);
        if (!IsDccClear(filledKeys)) MarkDccUncompressed(context, descriptor.dccAddress, guestBytes);
        uploadedKeys = DccKeys::Uncompressed;
        for (const auto& [from, to] : keep) GuestMemory::MarkWritten(from, static_cast<std::size_t>(to - from));

        settle(true);
        ++Profile().storageDirectWriteBacks;
        return;
    }

    static const bool unregisteredDrop = std::getenv("APS5_NO_UNREGISTERED_DROP") == nullptr;
    if (unregisteredDrop && !RegisteredReadableCovers(descriptor.baseAddress, static_cast<std::size_t>(guestBytes))) {
        static std::atomic<int> reports{0};
        if (reports.fetch_add(1) < 4) aps5::LogErr( "[gpu] storage image 0x%llx+0x%llx: memory no longer registered; %zu pending ranges dropped\n", static_cast<unsigned long long>(descriptor.baseAddress), static_cast<unsigned long long>(guestBytes), keep.size());
        unregisteredDropped.fetch_add(1, std::memory_order_relaxed);
        originalValid = false;
        for (std::uint32_t layer = 0; layer < trackedLayers; ++layer) {
            if (layers[layer]) layerPending[layer] = false;
        }
        return;
    }

    const bool wasValid = originalValid;
    if (!wasValid) GuestMemory::ReadCommitted(descriptor.baseAddress, original);
    DeviceBuffer linear(context, static_cast<std::size_t>(sliceLinearBytes * arrayLayers), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    DeviceBuffer tiled(context, original.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buffer host(context, original.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (profile) Profile().storageAlloc += timer.lap();

    std::memcpy(host.Bytes().data(), original.data(), original.size());
    if (profile) Profile().storageHostCopy += timer.lap();
    detiler.BeginBatch();
    CommandBatch batch(context);
    const auto commands = batch.Handle();
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    CopyBuffer(context, commands, host.Handle(), 0, tiled.Handle(), 0, original.size());
    VkImageMemoryBarrier toSource{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSource.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSource.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSource.image = image;
    toSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, descriptor.mipCount, 0, geometry.imageLayers};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSource);
    const auto regions = CopyRegions();
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, linear.Handle(), static_cast<std::uint32_t>(regions.size()), regions.data());
    const VkBufferMemoryBarrier toShader[] = {WholeBufferBarrier(linear.Handle(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT), WholeBufferBarrier(tiled.Handle(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2, toShader, 0, nullptr);
    for (std::uint32_t layer = 0; layer < arrayLayers; ++layer) {
        for (std::uint32_t level = 0; level < mips.size(); ++level) {
            if (!geometry.HasLayer(level, layer)) continue;
            const auto& mip = mips[level];
            detiler.Dispatch(commands, descriptor.tileMode, elementBytes, linear.Handle(), geometry.LinearLayerOffset(layer) + mip.linearOffset, tiled.Handle(), geometry.GuestLayerOffset(layer) + mip.tiledOffset, mip, true, layer, geometry.thick);
        }
    }

    static const std::uint64_t dumpAddress = [] { const char* text = std::getenv("APS5_DUMP_STORAGE"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();
    static int dumps = 0;
    std::unique_ptr<Buffer> dump;
    if (dumpAddress != 0 && descriptor.baseAddress == dumpAddress && dumps < 8) {
        dump = std::make_unique<Buffer>(context, static_cast<std::size_t>(mips[0].linearSize), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        CopyBuffer(context, commands, linear.Handle(), mips[0].linearOffset, dump->Handle(), 0, mips[0].linearSize);
    }
    const auto toCopy = WholeBufferBarrier(tiled.Handle(), VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkImageMemoryBarrier backToGeneral = toSource;
    backToGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    backToGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    backToGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    backToGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 1, &toCopy, 1, &backToGeneral);
    CopyBuffer(context, commands, tiled.Handle(), 0, host.Handle(), 0, original.size());
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    APS5_LOG_CHARS_OUT("StorageTexture writeback submit");
    batch.SubmitAndWait();
    APS5_LOG_CHARS_OUT("StorageTexture writeback done");
    countStorageWriteBack(guestBytes, false);
    if (profile) Profile().storageGpu += timer.lap();
    if (dump) {
        char name[64];
        std::snprintf(name, sizeof(name), "storage_%llx_%d.raw", static_cast<unsigned long long>(descriptor.baseAddress), dumps++);
        if (std::FILE* file = std::fopen(name, "wb")) {
            const std::uint32_t header[3] = {mips[0].pitchBytes / static_cast<std::uint32_t>(elementBytes), mips[0].height, static_cast<std::uint32_t>(storageFormat)};
            std::fwrite(header, sizeof(header), 1, file);
            std::fwrite(dump->Bytes().data(), 1, dump->Bytes().size(), file);
            std::fclose(file);
        }
    }

    const auto current = host.Bytes();
    for (const auto& [from, to] : keep) {
        const auto offset = static_cast<std::size_t>(from - descriptor.baseAddress);
        const auto length = static_cast<std::size_t>(to - from);
        GuestMemory::WriteChangedCommitted(from, current.subspan(offset, length), std::span<const std::byte>(original).subspan(offset, length));
        std::memcpy(original.data() + offset, current.data() + offset, length);
    }

    traceKeyStore("cpu write-back", descriptor, guestBytes);
    if (!IsDccClear(filledKeys)) MarkDccUncompressed(context, descriptor.dccAddress, guestBytes);
    uploadedKeys = DccKeys::Uncompressed;

    originalValid = !skippedAny && (wasValid || std::all_of(layers.begin(), layers.end(), [](bool selected) { return selected; }));

    settle(false);
    if (profile) Profile().storageStore += timer.lap();
}

StorageTexture::~StorageTexture() {
    {
        auto& live = Live();
        std::lock_guard lock(live.mutex);
        std::erase(live.textures, this);
    }

    {
        auto& pending = Pending();
        std::lock_guard lock(pending.mutex);
        if (dirty) {
            dirty = false;
            pending.textures.remove(this);
            BumpPendingSerial();
            static std::atomic<int> reports{0};
            if (reports.fetch_add(1) < 4) aps5::LogErr( "[gpu] storage image 0x%llx destroyed with GPU results pending\n", static_cast<unsigned long long>(descriptor.baseAddress));
        }
    }
    release();
}

void StorageTexture::release() noexcept {
    for (const auto& [mip, extra] : extraViews) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, extra, nullptr);
    extraViews.clear();
    for (const auto& [mip, extra] : firstLayerViews) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, extra, nullptr);
    firstLayerViews.clear();
    for (const auto& [format, attachment] : attachmentViews) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, attachment, nullptr);
    attachmentViews.clear();
    if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
    if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
    if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
}

std::uint64_t StorageTexture::GuestBytes() const {
    return guestBytes;
}

VkImageView StorageTexture::View() const {
    return view;
}

}
