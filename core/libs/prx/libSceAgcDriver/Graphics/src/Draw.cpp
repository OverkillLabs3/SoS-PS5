#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/common/StderrLog.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetTransfer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GpuColorTransfer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Optimization/include/Optimization/ShaderStageInputInfo.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

namespace AgcDriver::Graphics {

namespace {

std::uint32_t GuestFormatFor(VkFormat format, std::uint32_t elementBytes) {
    if (const auto guest = FindGuestColorTargetFormat(format, elementBytes)) return *guest;
    throw std::runtime_error("AGC graphics: no guest texture format matches the color buffer format " + std::to_string(static_cast<int>(format)));
}

GuestTextureResource SurfaceForTarget(const ColorTarget& color) {
    Require(color.tileMode == ColorTileMode::RenderTarget || color.tileMode == ColorTileMode::Standard4KB, "only 4 KiB standard and 64 KiB tiled color targets are resident");
    const bool chain = color.mipCount > 1;
    GuestTextureResource surface{};
    surface.baseAddress = chain ? color.surfaceAddress : color.address;
    surface.width = chain ? color.surfaceExtent.width : color.extent.width;
    surface.height = chain ? color.surfaceExtent.height : color.extent.height;
    surface.depthOrLastArray = 0;
    surface.baseArray = 0;
    surface.mipCount = color.mipCount;
    surface.baseLevel = 0;
    surface.lastLevel = color.mipCount - 1;
    surface.tileMode = ColorTextureTileMode(color.tileMode);
    surface.dimension = TextureDimension::k2D;
    surface.format = GuestFormatFor(color.format, color.elementBytes);
    surface.dstSelX = 4;
    surface.dstSelY = 5;
    surface.dstSelZ = 6;
    surface.dstSelW = 7;
    surface.dccAddress = color.dccAddress;
    surface.dccAlphaOnMsb = color.dccAlphaOnMsb;
    return surface;
}

}

namespace {

std::array<std::byte, 16> clearTexel(const ColorTarget& color, DccKeys keys) {
    std::array<std::byte, 16> texel{};
    const auto elementBytes = static_cast<std::size_t>(color.elementBytes);
    Require(elementBytes != 0 && elementBytes <= texel.size(), "unexpected color element size");
    if (keys == DccKeys::ClearRegister) {
        Require(elementBytes <= sizeof(color.clearWords), "the DCC register clear of a texel over 64 bits is not modeled");
        std::memcpy(texel.data(), color.clearWords.data(), elementBytes);
    } else {
        Require(FillDccClear(color.format, keys, color.dccAlphaOnMsb, std::span(texel.data(), elementBytes)), "the target's format has no encoding of its DCC clear code");
    }
    return texel;
}

bool clearToTexel(StorageTexture& image, const std::array<std::byte, 16>& texel, std::uint32_t elementBytes, const char*& refusal) {
    std::array<std::byte, 16> repeated{};
    for (std::size_t offset = 0; offset + elementBytes <= repeated.size(); offset += elementBytes) std::memcpy(repeated.data() + offset, texel.data(), elementBytes);
    std::array<std::uint32_t, 4> pattern{};
    std::memcpy(pattern.data(), repeated.data(), repeated.size());
    return image.FillClear(std::span<const std::uint32_t, 4>(pattern), StorageTexture::WholeImage, refusal);
}

void storeClearTexels(const Context& context, const ColorTarget& color, const std::array<std::byte, 16>& texel) {
    StorageTexture::FlushPending(color.address, color.bytes, nullptr, "fast-clear materialization");
    const auto keys = ReadDccKeys(color.dccAddress, color.bytes);
    if (!IsDccClear(keys)) return;
    const auto current = keys == DccKeys::ClearRegister ? texel : clearTexel(color, keys);
    const auto elementBytes = static_cast<std::size_t>(color.elementBytes);
    std::vector<std::byte> texels(color.bytes);
    for (std::size_t offset = 0; offset + elementBytes <= texels.size(); offset += elementBytes) std::memcpy(texels.data() + offset, current.data(), elementBytes);
    GuestMemory::Write(color.address, texels);
    MarkDccUncompressed(context, color.dccAddress, color.bytes);
}

void materializeRegisterClear(const Context& context, const ColorTarget& color, StorageTexture& resident) {
    if (color.dccAddress == 0 || resident.Descriptor().dccAddress != color.dccAddress) return;
    if (CurrentDccKeys(color.dccAddress, color.bytes) != DccKeys::ClearRegister) return;
    const auto texel = clearTexel(color, DccKeys::ClearRegister);
    const char* refusal = nullptr;
    if (clearToTexel(resident, texel, color.elementBytes, refusal)) {
        MarkDccUncompressed(context, color.dccAddress, color.bytes);
        return;
    }
    storeClearTexels(context, color, texel);
    resident.Refresh();
}

void imageBarrier(const Context& context, VkCommandBuffer commands, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void memoryBarrier(const Context& context, VkCommandBuffer commands, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) {
    const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, sourceAccess, destinationAccess};
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, sourceStage, destinationStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

TileMipLayout ColorTargetMip(const ColorTarget& color, const ColorTargetLayout& layout) {
    TileMipLayout mip{};
    mip.width = color.extent.width;
    mip.height = color.extent.height;
    mip.blocksPerRow = layout.BlocksPerRow();
    mip.pitchBytes = color.extent.width * color.elementBytes;
    mip.tiledSize = layout.Bytes();
    mip.linearSize = layout.LinearBytes();
    return mip;
}

enum DrawPhase : std::size_t { PhaseValidate, PhaseVertex, PhaseSetup, PhaseReadTarget, PhasePrepare, PhaseLookup, PhaseResources, PhasePipeline, PhaseRecord, PhaseKeep, PhaseSync, PhaseWriteBack, PhaseDescribe, PhaseCount };
constexpr std::array<const char*, PhaseCount> DrawPhaseNames{"validate", "vertex", "setup", "readTarget", "prepare", "lookup", "resources", "pipeline", "record", "keep", "sync", "writeBack", "describe"};

enum SyncReason : std::size_t { SyncNone, SyncNotResident, SyncNoRecorder, SyncDisabled, SyncCopiedWrites, SyncLease, SyncCount };
constexpr std::array<const char*, SyncCount> SyncReasonNames{"none", "non-resident target", "no recorder", "disabled", "copied writes", "lease"};
constexpr std::size_t IndirectPathCount = static_cast<std::size_t>(IndirectDrawPath::Count);
constexpr std::array<const char*, IndirectPathCount> IndirectDrawPathNames{"gpu-side", "patched SGPR not folded", "fetch offset unknown", "non-vertex path", "GE_INDX_OFFSET", "draw index", "vertex range too large", "feature gap", "pending image results", "pending label or copied write", "not imported", "disabled"};

constexpr std::array RefreshProofKinds{LookupOutcomes::StorageDepthCheck, LookupOutcomes::StorageKeyBuild, LookupOutcomes::StorageLockWait, LookupOutcomes::RefreshUnchanged, LookupOutcomes::RefreshCompared, LookupOutcomes::UploadDirect, LookupOutcomes::UploadCpu, LookupOutcomes::UploadClear, LookupOutcomes::DccScan, LookupOutcomes::PendingFlush};
constexpr std::size_t RefreshProofCount = RefreshProofKinds.size();
constexpr std::array<const char*, RefreshProofCount> RefreshProofNames{"depth check", "key build", "lock wait", "unchanged", "compared", "upload direct", "upload cpu", "upload clear", "dcc scan", "pending flush"};

struct DrawOutcome {

    bool recorded = false;
    bool waited = false;
    bool completion = false;

    bool passBegun = false;
    bool passContinued = false;
    SyncReason reason = SyncNone;

    bool validateMemoized = false;
    bool validateHit = false;

    bool addressBased = false;

    std::uint64_t targetLookups = 0;
    std::uint64_t slowLookups = 0;
    double slowLookupUs = 0;

    std::uint64_t fullScissorLookups = 0;
    std::uint64_t partialLookups = 0;
    std::uint64_t pagesWalked = 0;
    double lookupUs = 0;

    std::array<std::uint64_t, RefreshProofCount> refreshCounts{};
    std::array<double, RefreshProofCount> refreshUs{};

    std::size_t kind = 0;

    double hookWaitUs = 0;
    double ownSyncUs = 0;
};

enum DrawKind : std::size_t { KindRecipeHit, KindTemplateHit, KindBuild, KindBda, KindCount };
constexpr std::array<const char*, KindCount> DrawKindNames{"recipe hit", "template hit", "build", "BDA"};

struct DrawProfile {
    std::mutex mutex;
    std::array<double, KindCount> kindUs{};
    std::array<std::uint64_t, KindCount> kindCounts{};
    std::uint64_t fullScissorLookups = 0;
    std::uint64_t partialLookups = 0;
    std::uint64_t pagesWalked = 0;
    double lookupUs = 0;
    std::array<std::uint64_t, RefreshProofCount> refreshCounts{};
    std::array<double, RefreshProofCount> refreshUs{};
    std::array<double, PhaseCount> totalsUs{};

    std::array<double, PhaseCount> maxUs{};
    double maxDrawUs = 0;
    double hookWaitUs = 0;
    double maxHookWaitUs = 0;
    double ownSyncUs = 0;

    double bindingsUs = 0;
    double uploadUs = 0;
    double descriptorsUs = 0;
    double otherUs = 0;
    double addressOtherUs = 0;
    std::uint64_t addressBuilds = 0;
    std::uint64_t draws = 0;
    std::uint64_t recorded = 0;
    std::uint64_t waited = 0;

    std::uint64_t completion = 0;

    std::uint64_t passesBegun = 0;
    std::uint64_t passesContinued = 0;
    std::array<std::uint64_t, SyncCount> reasons{};

    double recordedUs = 0;
    double waitedUs = 0;
    double synchronousUs = 0;
    std::uint64_t targetLookups = 0;
    std::uint64_t slowLookups = 0;
    double slowLookupUs = 0;

    std::uint64_t cacheHits = 0;
    std::uint64_t cacheMisses = 0;
    std::uint64_t cacheInvalidated = 0;
    std::uint64_t uncacheable = 0;

    std::uint64_t validateHits = 0;
    std::uint64_t validateMisses = 0;

    std::uint64_t recipeHits = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrawRecipeMiss::Count)> recipeMisses{};

    std::array<std::uint64_t, IndirectPathCount> indirect{};
    std::uint64_t indirectRewritten = 0;
    double indirectReadUs = 0;

    std::array<std::uint64_t, static_cast<std::size_t>(DrawSkip::Count)> skips{};
    std::array<double, static_cast<std::size_t>(DrawSkip::Count)> skipUs{};

    std::map<std::string, std::uint64_t> skipReasons;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};
constexpr std::array<const char*, static_cast<std::size_t>(DrawSkip::Count)> DrawSkipNames{"nothing to draw", "prechecked", "thrown"};

DrawProfile& Profile() {
    static DrawProfile profile;
    return profile;
}

void reportDraw(const std::array<double, PhaseCount>& us, const ShaderResources::BuildTiming* built, const DrawOutcome& outcome) {
    auto& profile = Profile();
    std::lock_guard lock(profile.mutex);
    double drawUs = 0;
    for (std::size_t i = 0; i < PhaseCount; ++i) {
        profile.totalsUs[i] += us[i];
        profile.maxUs[i] = std::max(profile.maxUs[i], us[i]);
        drawUs += us[i];
    }
    profile.maxDrawUs = std::max(profile.maxDrawUs, drawUs);
    profile.hookWaitUs += outcome.hookWaitUs;
    profile.maxHookWaitUs = std::max(profile.maxHookWaitUs, outcome.hookWaitUs);
    profile.ownSyncUs += outcome.ownSyncUs;
    if (built != nullptr) {
        profile.bindingsUs += built->bindingsMs * 1000.0;
        profile.uploadUs += built->uploadMs * 1000.0;
        profile.descriptorsUs += built->descriptorsMs * 1000.0;
        const auto other = std::max(0.0, us[PhaseResources] - (built->bindingsMs + built->uploadMs + built->descriptorsMs) * 1000.0);
        profile.otherUs += other;
        if (outcome.addressBased) {
            profile.addressOtherUs += other;
            ++profile.addressBuilds;
        }
    }
    ++profile.draws;
    if (outcome.recorded) ++(outcome.waited ? profile.waited : profile.recorded);
    if (outcome.completion) ++profile.completion;
    if (outcome.passBegun) ++profile.passesBegun;
    if (outcome.passContinued) ++profile.passesContinued;
    ++profile.reasons[outcome.reason];
    (outcome.recorded ? (outcome.waited ? profile.waitedUs : profile.recordedUs) : profile.synchronousUs) += drawUs;
    profile.targetLookups += outcome.targetLookups;
    profile.slowLookups += outcome.slowLookups;
    profile.slowLookupUs += outcome.slowLookupUs;
    profile.fullScissorLookups += outcome.fullScissorLookups;
    profile.partialLookups += outcome.partialLookups;
    profile.pagesWalked += outcome.pagesWalked;
    profile.lookupUs += outcome.lookupUs;
    for (std::size_t i = 0; i < RefreshProofCount; ++i) {
        profile.refreshCounts[i] += outcome.refreshCounts[i];
        profile.refreshUs[i] += outcome.refreshUs[i];
    }
    if (outcome.kind < KindCount) {
        ++profile.kindCounts[outcome.kind];
        profile.kindUs[outcome.kind] += drawUs;
    }
    if (outcome.validateMemoized) ++(outcome.validateHit ? profile.validateHits : profile.validateMisses);
    const auto now = std::chrono::steady_clock::now();
    if (now - profile.lastReport < std::chrono::seconds(10)) return;
    profile.lastReport = now;
    const auto synchronous = profile.draws - profile.recorded - profile.waited;
    const auto average = [](double total, std::uint64_t count) { return count != 0 ? total / static_cast<double>(count) : 0.0; };
    char line[4096];
    int n = std::snprintf(line, sizeof(line), "[draws] %llu draws over 10 s (%llu recorded avg %.0f us, of them %llu with completion; %llu recorded then waited avg %.0f us; %llu synchronous avg %.0f us; render passes %llu begun, %llu draws continued one; waited or synchronous because:", static_cast<unsigned long long>(profile.draws), static_cast<unsigned long long>(profile.recorded), average(profile.recordedUs, profile.recorded), static_cast<unsigned long long>(profile.completion), static_cast<unsigned long long>(profile.waited), average(profile.waitedUs, profile.waited), static_cast<unsigned long long>(synchronous), average(profile.synchronousUs, synchronous), static_cast<unsigned long long>(profile.passesBegun), static_cast<unsigned long long>(profile.passesContinued));
    const auto room = [&] { return n > 0 && static_cast<std::size_t>(n) < sizeof(line); };
    for (std::size_t i = SyncNone + 1; i < SyncCount && room(); ++i) {
        if (profile.reasons[i] != 0) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %llu", SyncReasonNames[i], static_cast<unsigned long long>(profile.reasons[i]));
    }
    if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "):");
    for (std::size_t i = 0; i < PhaseCount && room(); ++i) {
        if (profile.totalsUs[i] <= 0) continue;
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s=%.1fms", DrawPhaseNames[i], profile.totalsUs[i] / 1000.0);
        if (i == PhaseResources && room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " (bindings %.1f, upload %.1f, descriptors %.1f, other %.1f of which %.1f in %llu address-based builds)", profile.bindingsUs / 1000.0, profile.uploadUs / 1000.0, profile.descriptorsUs / 1000.0, profile.otherUs / 1000.0, profile.addressOtherUs / 1000.0, static_cast<unsigned long long>(profile.addressBuilds));
        if (i == PhaseReadTarget && room()) {
            n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " (%llu resident lookups, %llu of them >= 1 ms = %.1f; target lookups: full-scissor %llu / partial %llu / pages walked %llu / %.0f us)", static_cast<unsigned long long>(profile.targetLookups), static_cast<unsigned long long>(profile.slowLookups), profile.slowLookupUs / 1000.0, static_cast<unsigned long long>(profile.fullScissorLookups), static_cast<unsigned long long>(profile.partialLookups), static_cast<unsigned long long>(profile.pagesWalked), profile.lookupUs);

            if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; lookup proof:");
            for (std::size_t kind = 0; kind < RefreshProofCount && room(); ++kind) {
                if (profile.refreshCounts[kind] == 0) continue;
                n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %llu/%.1fms", RefreshProofNames[kind], static_cast<unsigned long long>(profile.refreshCounts[kind]), profile.refreshUs[kind] / 1000.0);
            }
        }
    }
    if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; per kind (avg us, count):");
    for (std::size_t kind = 0; kind < KindCount && room(); ++kind) {
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %.0f (%llu)", DrawKindNames[kind], average(profile.kindUs[kind], profile.kindCounts[kind]), static_cast<unsigned long long>(profile.kindCounts[kind]));
    }

    if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; longest draw %.1f ms, longest phase of any draw:", profile.maxDrawUs / 1000.0);
    for (std::size_t i = 0; i < PhaseCount && room(); ++i) {
        if (profile.maxUs[i] < 500.0) continue;
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %.1f", DrawPhaseNames[i], profile.maxUs[i] / 1000.0);
    }
    if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; hook waits inside draws %.1f ms (max %.1f; the draws' own syncs, %.1f ms, not counted)", profile.hookWaitUs / 1000.0, profile.maxHookWaitUs / 1000.0, profile.ownSyncUs / 1000.0);
    std::uint64_t indirectCpu = 0;
    for (std::size_t i = 1; i < IndirectPathCount; ++i) indirectCpu += profile.indirect[i];
    if ((profile.indirect[0] != 0 || indirectCpu != 0) && room()) {
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; indirect: gpu-side %llu (%llu rewritten), cpu-side %llu (", static_cast<unsigned long long>(profile.indirect[0]), static_cast<unsigned long long>(profile.indirectRewritten), static_cast<unsigned long long>(indirectCpu));
        for (std::size_t i = 1; i < IndirectPathCount && room(); ++i) {
            if (profile.indirect[i] != 0) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %llu", IndirectDrawPathNames[i], static_cast<unsigned long long>(profile.indirect[i]));
        }
        if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "), argument reads %.1f ms", profile.indirectReadUs / 1000.0);
    }
    if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; skipped packets:");
    for (std::size_t i = 0; i < profile.skips.size() && room(); ++i) {
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %llu in %.1f ms", DrawSkipNames[i], static_cast<unsigned long long>(profile.skips[i]), profile.skipUs[i] / 1000.0);
    }
    if (room() && !profile.skipReasons.empty()) {
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; skipped draws by reason:");
        std::multimap<std::uint64_t, std::string, std::greater<std::uint64_t>> ranked;
        for (const auto& [reason, count] : profile.skipReasons) ranked.insert({count, reason});
        for (const auto& [count, reason] : ranked) {
            if (!room()) break;
            n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %.60s; (%llu)", reason.c_str(), static_cast<unsigned long long>(count));
        }
    }
    if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; recipe hits %llu, misses by reason:", static_cast<unsigned long long>(profile.recipeHits));
    for (std::size_t i = 1; i < profile.recipeMisses.size() && room(); ++i) {
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %llu", DrawRecipeMissName(static_cast<DrawRecipeMiss>(i)), static_cast<unsigned long long>(profile.recipeMisses[i]));
    }
    if (room()) {
        std::uint64_t live = 0, created = 0;
        DepthSurfaceStats(live, created);
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; depth surfaces %llu live, %llu created", static_cast<unsigned long long>(live), static_cast<unsigned long long>(created));
    }
    aps5::LogErr( "%s\n", line);
    aps5::LogErr( "[rescache] draws: %llu hits, %llu misses, %llu invalidated, %llu uncacheable; validation memo %llu hits / %llu misses (which key words the misses differ in: the miss churn line)\n", static_cast<unsigned long long>(profile.cacheHits), static_cast<unsigned long long>(profile.cacheMisses), static_cast<unsigned long long>(profile.cacheInvalidated), static_cast<unsigned long long>(profile.uncacheable), static_cast<unsigned long long>(profile.validateHits), static_cast<unsigned long long>(profile.validateMisses));
    profile.totalsUs.fill(0);
    profile.maxUs.fill(0);
    profile.maxDrawUs = profile.hookWaitUs = profile.maxHookWaitUs = profile.ownSyncUs = 0;
    profile.bindingsUs = profile.uploadUs = profile.descriptorsUs = profile.otherUs = profile.addressOtherUs = 0;
    profile.addressBuilds = 0;
    profile.draws = profile.recorded = profile.waited = profile.completion = 0;
    profile.passesBegun = profile.passesContinued = 0;
    profile.reasons.fill(0);
    profile.skipReasons.clear();
    profile.recordedUs = profile.waitedUs = profile.synchronousUs = 0;
    profile.targetLookups = profile.slowLookups = 0;
    profile.slowLookupUs = 0;
    profile.fullScissorLookups = profile.partialLookups = profile.pagesWalked = 0;
    profile.lookupUs = 0;
    profile.refreshCounts.fill(0);
    profile.refreshUs.fill(0);
    profile.kindUs.fill(0);
    profile.kindCounts.fill(0);
    profile.cacheHits = profile.cacheMisses = profile.cacheInvalidated = profile.uncacheable = 0;
    profile.validateHits = profile.validateMisses = 0;
    profile.recipeHits = 0;
    profile.recipeMisses.fill(0);
    profile.indirect.fill(0);
    profile.indirectRewritten = 0;
    profile.indirectReadUs = 0;
    profile.skips.fill(0);
    profile.skipUs.fill(0);
}

void countCache(std::uint64_t DrawProfile::*counter) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    auto& stats = Profile();
    std::lock_guard lock(stats.mutex);
    ++(stats.*counter);
}

bool ValidationKey(const Context& context, std::span<const CompiledShader> shaders, const State& state, std::vector<std::uint64_t>& key) {
    using Stage = ShaderRecompiler::ShaderStage;
    static const bool disabled = std::getenv("APS5_NO_VALIDATE_CACHE") != nullptr;
    const auto add = [&](auto value) { key.push_back(static_cast<std::uint64_t>(value)); };
    bool keyed = !disabled;
    if (keyed) {
        key.reserve(40 + shaders.size() * 12);
        add(shaders.size());
        for (const auto& shader : shaders) {
            Require(shader.program != nullptr, "missing compiled shader");
            const auto& program = *shader.program;
            const bool generated = state.rectList && (shader.stage == Stage::TessellationControl || shader.stage == Stage::TessellationEvaluation);
            if (!generated && program.variantId == 0) {
                keyed = false;
                break;
            }
            add(shader.stage);
            add(generated ? std::uint64_t{0} : program.variantId);
            add(shader.pushConstantOffset);
            add(program.pushConstants.size());
            add(program.bdaAbiVersion);
            add(program.vertexAttributes.size());
            for (const auto& attribute : program.vertexAttributes) {
                add(attribute.location);
                add(attribute.components);
                add(attribute.fetchIndex);

                add((attribute.resource.fields[3] >> 12u) & 0x7fu);
            }
        }
    }
    if (keyed) {
        add(state.stages.path);
        add(state.rectList);
        add(state.topology);
        add(state.cullMode);
        add(state.blends.size());
        add(context.subgroup.subgroupSize);
        add(context.subgroup.supportedStages);
        add(context.subgroup.supportedOperations);
        add(context.fragmentShaderBarycentric);
        add(state.stages.mesh.has_value());
        if (state.stages.mesh) {
            const auto& mesh = *state.stages.mesh;
            add(mesh.inputPrimitive);
            add(mesh.primitivesPerGroup);
            add(mesh.verticesPerGroup);
            add(mesh.maxVertices);
            add(mesh.maxPrimitives);
            add(mesh.threadsPerGroup);
            add(mesh.ldsSizeDwords);
            add(mesh.provokingVertex);
        }
        add(state.stages.tessellation.has_value());
        if (state.stages.tessellation) {
            const auto& tessellation = *state.stages.tessellation;
            add(tessellation.inputControlPoints);
            add(tessellation.outputControlPoints);
            add(tessellation.domain);
            add(tessellation.partitioning);
            add(tessellation.outputTopology);
        }
    }
    return keyed;
}

std::mutex& validationMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::vector<std::uint64_t>, std::string>& validationFailures() {
    static std::map<std::vector<std::uint64_t>, std::string> failures;
    return failures;
}

std::set<std::uint32_t> CachedFragmentOutputs(const Context& context, std::span<const CompiledShader> shaders, const State& state, bool& memoized, bool& hit) {
    memoized = false;
    hit = false;
    std::vector<std::uint64_t> key;
    const bool keyed = ValidationKey(context, shaders, state, key);
    static std::map<std::vector<std::uint64_t>, std::set<std::uint32_t>> memo;
    if (keyed) {
        memoized = true;
        std::lock_guard lock(validationMutex());
        if (const auto found = memo.find(key); found != memo.end()) {
            hit = true;
            return found->second;
        }
    }
    std::set<std::uint32_t> outputs;
    try {
        outputs = ValidateShaders(shaders, state, context.subgroup, context.fragmentShaderBarycentric, context.descriptorIndexing);
    } catch (const std::exception& error) {
        if (keyed) {
            std::lock_guard lock(validationMutex());
            auto& failures = validationFailures();
            if (failures.size() >= 1024) failures.clear();
            failures.emplace(std::move(key), error.what());
        }
        throw;
    }
    if (keyed) {
        std::lock_guard lock(validationMutex());

        if (memo.size() >= 1024) memo.clear();
        memo.emplace(std::move(key), outputs);
    }
    return outputs;
}

ResourceCache::Key DrawResourceKey(const Context& context, std::span<const CompiledShader> shaders, const ColorTarget& target, std::uint64_t indexAddress, std::uint64_t indexBytes, bool ranges) {
    ResourceCache::Key key{0xffffffffu};
    const auto append64 = [&](std::uint64_t value) {
        key.push_back(static_cast<std::uint32_t>(value));
        key.push_back(static_cast<std::uint32_t>(value >> 32u));
    };
    append64(reinterpret_cast<std::uint64_t>(context.device));
    key.push_back(static_cast<std::uint32_t>(shaders.size()));
    for (const auto& shader : shaders) {
        const auto part = ShaderResources::ContentKey(shader);
        key.push_back(static_cast<std::uint32_t>(part.size()));
        key.insert(key.end(), part.begin(), part.end());
    }
    if (ranges) {
        append64(target.address);
        append64(target.bytes);
        append64(indexAddress);
        append64(indexBytes);
    }
    return key;
}

void CheckBufferAliases(std::span<const CompiledShader> shaders, const ColorTarget& target, std::uint64_t indexAddress, std::uint64_t indexBytes) {
    const auto overlap = [](std::uint64_t first, std::uint64_t firstSize, std::uint64_t second, std::uint64_t secondSize) { return first < second + secondSize && second < first + firstSize; };
    for (const auto& shader : shaders) {
        for (const auto& binding : shader.program->bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
            const auto& words = binding.guestDescriptor;
            for (std::size_t offset = 0; offset + 4 <= words.size(); offset += 4) {
                const ShaderRecompiler::ShaderBufferResource descriptor{{words[offset], words[offset + 1], words[offset + 2], words[offset + 3]}};
                const auto address = descriptor.Base48();
                const auto size = descriptor.GetSize();
                if (size == 0 || address == 0) continue;
                const auto element = offset / 4;
                const bool written = element >= binding.bufferWritten.size() || binding.bufferWritten[element];
                Require(!overlap(address, size, target.address, target.bytes), "shader buffer aliases the render target");
                Require(!written || !overlap(address, size, indexAddress, indexBytes), "writable shader buffer aliases the index buffer");
            }
        }
    }
}

}

std::array<std::uint32_t, 4> MeshIndexBufferDescriptor(const Pm4::DrawParameters& draw, std::uint64_t unreadAddress) {
    const auto address = draw.indexed ? draw.indexAddress : unreadAddress;
    const auto bytes = draw.indexed ? (static_cast<std::uint64_t>(draw.indexCount) * draw.indexSize + 3u) & ~std::uint64_t{3} : 4u;
    Require(address != 0 && bytes != 0 && bytes <= 0xffffffffu && (address >> 48u) == 0, "invalid mesh index buffer range");
    constexpr std::uint32_t RawWord3 = 0x31016facu;
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) & 0xffffu, static_cast<std::uint32_t>(bytes), RawWord3};
}

std::shared_ptr<std::vector<std::shared_ptr<ShaderResources>>> DrawCopiedWriters() {

    static auto* const writers = new std::shared_ptr<std::vector<std::shared_ptr<ShaderResources>>>(std::make_shared<std::vector<std::shared_ptr<ShaderResources>>>());
    return *writers;
}

const char* IndirectDrawPathName(IndirectDrawPath path) {
    return IndirectDrawPathNames[static_cast<std::size_t>(path)];
}

void CountIndirectDraw(IndirectDrawPath path, double readMs, bool rewritten) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    auto& stats = Profile();
    std::lock_guard lock(stats.mutex);
    ++stats.indirect[static_cast<std::size_t>(path)];
    if (rewritten) ++stats.indirectRewritten;
    stats.indirectReadUs += readMs * 1000.0;
}

void CountDrawSkip(DrawSkip kind, double us) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    auto& stats = Profile();
    std::lock_guard lock(stats.mutex);
    ++stats.skips[static_cast<std::size_t>(kind)];
    stats.skipUs[static_cast<std::size_t>(kind)] += us;
}

void CountDrawSkipReason(const char* reason) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile || reason == nullptr) return;
    char key[65];
    std::snprintf(key, sizeof(key), "%.64s", reason);
    auto& stats = Profile();
    std::lock_guard lock(stats.mutex);
    const auto seen = stats.skipReasons.find(key);
    if (seen != stats.skipReasons.end()) {
        ++seen->second;
    } else if (stats.skipReasons.size() < 24) {
        stats.skipReasons.emplace(key, 1u);
    }
}

bool DrawRecipes() {
    static const bool noDrawRecipe = std::getenv("APS5_NO_DRAW_RECIPE") != nullptr;
    return !noDrawRecipe;
}

const char* DrawRecipeMissName(DrawRecipeMiss miss) {
    constexpr std::array<const char*, static_cast<std::size_t>(DrawRecipeMiss::Count)> names{"none", "not recordable", "target gone", "template gone", "objects gone", "proof"};
    return names[static_cast<std::size_t>(miss)];
}

namespace {

struct DrawTimer {
    bool profile;
    std::chrono::steady_clock::time_point phaseStart;
    std::array<double, PhaseCount> us{};
    explicit DrawTimer(bool profile) : profile(profile), phaseStart(std::chrono::steady_clock::now()) {}
    void phase(DrawPhase which) {
        if (!profile) return;
        const auto now = std::chrono::steady_clock::now();
        us[which] += std::chrono::duration<double, std::micro>(now - phaseStart).count();
        phaseStart = now;
    }
};

void reportDrawEnd(const State& state, const DrawTimer& timer, const ShaderResources::BuildTiming* built, DrawOutcome& outcome, double waitedBefore, double ownWaitedMs, const char* suffix) {
    if (!timer.profile) return;
    static const bool traceDraws = std::getenv("APS5_TRACE_DRAWS") != nullptr;
    outcome.hookWaitUs = std::max(0.0, Recorder::ThreadWaitedMs() - waitedBefore - ownWaitedMs) * 1000.0;
    outcome.ownSyncUs = ownWaitedMs * 1000.0;
    if (traceDraws) {
        char line[512];
        int n = std::snprintf(line, sizeof(line), "[draw] %ux%u %zu targets%s:", state.renderExtent.width, state.renderExtent.height, state.colors.size(), suffix);
        for (std::size_t i = 0; i < PhaseCount && n > 0 && static_cast<std::size_t>(n) < sizeof(line); ++i) {
            if (timer.us[i] <= 0) continue;
            n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s=%.0fus", DrawPhaseNames[i], timer.us[i]);
        }
        aps5::LogErr( "%s\n", line);
    }
    reportDraw(timer.us, built, outcome);
}

}

DrawInputCopy CopyDrawInput(const Context& context, Recorder* recorder, std::uint64_t address, std::size_t bytes, std::size_t alignment, Recorder::SnapshotUse use) {
    Require(use != Recorder::SnapshotUse::Storage, "a draw input is a vertex or index buffer");
    DrawInputCopy copy;

    static const std::size_t reuseMinimum = [] {
        const char* text = std::getenv("APS5_DRAW_INPUT_REUSE_MIN");
        return text != nullptr ? static_cast<std::size_t>(std::strtoull(text, nullptr, 10)) : std::size_t{32768};
    }();
    if (recorder != nullptr && bytes >= reuseMinimum && bytes != 0) {
        GuestMemory::FlushGpuWrites(address, bytes);
        copy.registryGeneration = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        copy.generation = GuestMemory::CollectWrites(address, bytes);
        if (copy.generation != 0) copy.buffer = recorder->ReusableDrawSnapshot(address, bytes, use, &copy.derived);
        if (copy.buffer != nullptr) {
            copy.reused = true;
            return copy;
        }
    }
    copy.buffer = std::make_shared<Buffer>(context, bytes, use == Recorder::SnapshotUse::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    GuestMemory::Read(address, copy.buffer->Bytes(), alignment);
    return copy;
}

void KeepDrawInput(Recorder* recorder, std::uint64_t address, const DrawInputCopy& copy, Recorder::SnapshotUse use, std::uint32_t derived) {
    if (recorder == nullptr || copy.reused || copy.generation == 0 || copy.buffer == nullptr) return;
    recorder->KeepDrawSnapshot(address, copy.buffer->Bytes().size(), copy.generation, copy.registryGeneration, copy.buffer, use, derived);
}

namespace {

struct DrawInputs {

    bool nothing = false;
    std::uint64_t indexBytes = 0;
    std::shared_ptr<Buffer> indices;
    std::uint32_t maxIndex = 0;
    VertexInputLayout vertexInput;
    std::vector<std::shared_ptr<Buffer>> vertexBuffers;
    std::vector<VkBuffer> vertexHandles;
    std::vector<VkDeviceSize> vertexOffsets;
    std::set<std::uint32_t> fragmentOutputs;
    VkPipelineStageFlags shaderStages = 0;
    std::uint32_t meshGroups = 0;
};

bool HudTraceWanted(std::uint64_t frame) {
    static const unsigned modulus = std::getenv("APS5_TRACE_HUD") != nullptr ? static_cast<unsigned>(std::max(std::atoi(std::getenv("APS5_TRACE_HUD")), 4)) : 0u;
    static const std::pair<std::uint64_t, std::uint64_t> range = [] {
        const char* text = std::getenv("APS5_TRACE_RANGE");
        if (text == nullptr) return std::pair<std::uint64_t, std::uint64_t>{0, 0};
        char* end = nullptr;
        const auto from = std::strtoull(text, &end, 10);
        return std::pair<std::uint64_t, std::uint64_t>{from, *end == ':' ? std::strtoull(end + 1, nullptr, 10) : from};
    }();
    if (modulus == 0) return false;
    if (range.second != 0 && (frame < range.first || frame > range.second)) return false;
    return frame % modulus < 2;
}

DrawInputs prepareDrawInputs(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, DrawOutcome& outcome, DrawTimer& timer, const DrawRecipe* recipe) {
    DrawInputs inputs;
    APS5_LOG_OUT_DEBUG("Draw indices=%u instances=%u indexSize=%u flags=%u indexAddress=0x%llx", draw.indexCount, draw.instanceCount, draw.indexSize, draw.flags, static_cast<unsigned long long>(draw.indexAddress));
    APS5_LOG_OUT_DEBUG("State colorTarget=%u render=%ux%u colorAddress=0x%llx colorBytes=%llu colorExtent=%ux%u", state.hasColorTarget ? 1u : 0u, state.renderExtent.width, state.renderExtent.height, static_cast<unsigned long long>(state.color.address), static_cast<unsigned long long>(state.color.bytes), state.color.extent.width, state.color.extent.height);
    APS5_LOG_OUT_DEBUG("Viewport x=%f y=%f w=%f h=%f minDepth=%f maxDepth=%f", state.viewport.x, state.viewport.y, state.viewport.width, state.viewport.height, state.viewport.minDepth, state.viewport.maxDepth);
    APS5_LOG_OUT_DEBUG("Scissor x=%d y=%d w=%u h=%u topology=%u cullMode=0x%x frontFace=%u", state.scissor.offset.x, state.scissor.offset.y, state.scissor.extent.width, state.scissor.extent.height, static_cast<unsigned>(state.topology), static_cast<unsigned>(state.cullMode), static_cast<unsigned>(state.frontFace));

    const auto* args = draw.indirect ? &*draw.indirect : nullptr;
    Require(draw.indexed ? draw.flags == 0 : (draw.flags & ~0x20u) == 0, "draw modifiers are unsupported");
    if (draw.indexed) {
        Require(draw.indexSize == 2 || draw.indexSize == 4, "only uint16 and uint32 index buffers are supported");
    } else {
        Require(draw.indexAddress == 0 && draw.indexSize == 0, "auto draw must not reference an index buffer");
        if (args == nullptr) {
            if (draw.indexCount == 0 || draw.instanceCount == 0) {
                inputs.nothing = true;
                return inputs;
            }
            Require(draw.firstVertex <= std::numeric_limits<std::uint32_t>::max() - (draw.indexCount - 1u), "auto draw vertex range overflow");
            Require(draw.firstInstance <= std::numeric_limits<std::uint32_t>::max() - (draw.instanceCount - 1u), "auto draw instance range overflow");
        }
    }
    if (args == nullptr) Require(draw.indexCount != 0 && draw.instanceCount != 0, "zero-count indexed draws are unsupported");
    else Require(!state.stages.mesh && !state.stages.tessellation && !state.rectList, "indirect draw on a non-vertex path must be resolved by the driver");
    inputs.indexBytes = static_cast<std::uint64_t>(draw.indexCount) * draw.indexSize;
    const auto indexBytes = inputs.indexBytes;
    APS5_LOG_OUT_DEBUG("Index buffer bytes=%llu", static_cast<unsigned long long>(indexBytes));
    Require(indexBytes <= std::numeric_limits<std::size_t>::max(), "index buffer size overflow");
    if (draw.indexed) GuestMemory::CheckRange(reinterpret_cast<const void*>(draw.indexAddress), static_cast<std::size_t>(indexBytes), draw.indexSize);
    APS5_LOG_CHARS_OUT_DEBUG("Index buffer range OK");
    Require(!draw.indexed || !state.hasColorTarget || draw.indexAddress + indexBytes <= state.color.address || state.color.address + state.color.bytes <= draw.indexAddress, "index buffer aliases the render target");
    if (state.rectList) Require(draw.indexCount % 3 == 0, "incomplete rect-list primitive");
    APS5_LOG_CHARS_OUT_DEBUG("ValidateShaders");
    if (recipe != nullptr) {
        inputs.fragmentOutputs = recipe->fragmentOutputs;
        inputs.shaderStages = recipe->shaderStages;
    } else {
        inputs.fragmentOutputs = CachedFragmentOutputs(context, shaders, state, outcome.validateMemoized, outcome.validateHit);
        inputs.shaderStages = PipelineStages(shaders);
    }
    APS5_LOG_CHARS_OUT_DEBUG("ValidateShaders OK");
    APS5_LOG_OUT_DEBUG("PipelineStages=0x%x", static_cast<unsigned>(inputs.shaderStages));
    if (state.stages.mesh) {
        APS5_LOG_CHARS_OUT_DEBUG("Mesh path");
        Require(context.meshShader, "device does not support mesh shaders");
        const auto& mesh = *state.stages.mesh;
        const auto inputSize = mesh.inputPrimitive == 1 ? 1u : mesh.inputPrimitive == 2 ? 2u : 3u;
        Require(draw.indexCount >= inputSize && mesh.primitivesPerGroup != 0, "mesh draw contains no complete primitive");
        const auto step = mesh.inputPrimitive == 6 ? 1u : inputSize;
        const auto primitives = (draw.indexCount - inputSize) / step + 1u;
        inputs.meshGroups = (primitives - 1u) / mesh.primitivesPerGroup + 1u;
        APS5_LOG_OUT_DEBUG("Mesh primitives=%u groups=%u", primitives, inputs.meshGroups);
        Require(inputs.meshGroups <= context.meshLimits.maxMeshWorkGroupCount[0] && draw.instanceCount <= context.meshLimits.maxMeshWorkGroupCount[1] && static_cast<std::uint64_t>(inputs.meshGroups) * draw.instanceCount <= context.meshLimits.maxMeshWorkGroupTotalCount, "mesh draw exceeds workgroup count limits");
    }
    if (state.stages.tessellation) Require(draw.indexCount % state.stages.tessellation->inputControlPoints == 0, "incomplete tessellation patch");

    ValidateViewport(context, state.viewport);
    timer.phase(PhaseValidate);
    inputs.maxIndex = draw.indexed ? 0u : draw.firstVertex + draw.indexCount - 1u;
    if (draw.indexed) {
        const auto use = draw.indexSize == 2 ? Recorder::SnapshotUse::Index16 : Recorder::SnapshotUse::Index32;
        auto copy = CopyDrawInput(context, context.recorder, draw.indexAddress, static_cast<std::size_t>(indexBytes), draw.indexSize, use);
        std::uint32_t highest = copy.derived;
        if (!copy.reused) {
            highest = 0;
            const auto bytes = copy.buffer->Bytes();
            for (std::size_t offset = 0; offset < indexBytes; offset += draw.indexSize) {
                std::uint32_t index = 0;
                if (draw.indexSize == 2) {
                    std::uint16_t value = 0;
                    std::memcpy(&value, bytes.data() + offset, sizeof(value));
                    index = value;
                } else {
                    std::memcpy(&index, bytes.data() + offset, sizeof(index));
                }
                highest = std::max(highest, index);
            }
            KeepDrawInput(context.recorder, draw.indexAddress, copy, use, highest);
        }
        Require(highest <= context.limits.maxDrawIndexedIndexValue, "index exceeds the device's indexed draw limit");
        inputs.maxIndex = highest;
        inputs.indices = std::move(copy.buffer);
    }
    APS5_LOG_CHARS_OUT_DEBUG("Index validation OK");
    const auto& attributes = shaders.front().program->vertexAttributes;

    if (recipe != nullptr) inputs.vertexInput = recipe->vertexInput;
    else inputs.vertexInput = BuildVertexInputLayout(context, attributes);
    inputs.vertexOffsets.assign(attributes.size(), 0);

    if (draw.indexed) {
        Require(draw.firstVertex <= std::numeric_limits<std::uint32_t>::max() - inputs.maxIndex, "indexed draw vertex range overflow");
        inputs.maxIndex += draw.firstVertex;
    }
    std::string hudVertices;
    const bool hudSampled = HudTraceWanted(GuestMemory::TraceFrame());
    for (const auto& attribute : attributes) {

        auto bytes = args != nullptr ? VertexBufferExtent(attribute) : VertexBufferReadSize(attribute, inputs.maxIndex, draw.instanceCount, draw.firstInstance);
        const auto& fields = attribute.resource.fields;
        const auto address = fields[0] | (static_cast<std::uint64_t>(fields[1] & 0xffffu) << 32u);
        if (args == nullptr) {
            // Cover the whole last record (see VertexBufferReadSize), but only when that cannot make a draw that worked fail:
            // the extra bytes must be readable and must not overlap the render target.
            const auto whole = VertexBufferReadSize(attribute, inputs.maxIndex, draw.instanceCount, draw.firstInstance, true);
            if (whole > bytes && GuestMemory::Accessible(reinterpret_cast<const void*>(address), whole) && (!state.hasColorTarget || address + whole <= state.color.address || state.color.address + state.color.bytes <= address)) bytes = whole;
        }
        Require(!state.hasColorTarget || address + bytes <= state.color.address || state.color.address + state.color.bytes <= address, "vertex buffer aliases the render target");

        const bool unreadable = !GuestMemory::Accessible(reinterpret_cast<const void*>(address), bytes);
        if (unreadable) {
            auto empty = std::make_unique<Buffer>(context, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            std::memset(empty->Bytes().data(), 0, empty->Bytes().size());
            inputs.vertexHandles.push_back(empty->Handle());
            inputs.vertexBuffers.push_back(std::move(empty));
            continue;
        }
        GuestMemory::CheckRange(reinterpret_cast<const void*>(address), bytes, 1);
        auto copy = CopyDrawInput(context, context.recorder, address, bytes, 1, Recorder::SnapshotUse::Vertex);
        KeepDrawInput(context.recorder, address, copy, Recorder::SnapshotUse::Vertex, 0);
        if (hudSampled) {

            const auto data = copy.buffer->Bytes();
            std::uint64_t hash = 1469598103934665603ull;
            for (std::size_t i = 0; i + 4 <= data.size(); i += 4) {
                std::uint32_t word;
                std::memcpy(&word, data.data() + i, sizeof(word));
                hash = (hash ^ word) * 1099511628211ull;
            }
            char text[2400];
            int length = std::snprintf(text, sizeof(text), " vb%u@%llx/%zu h=%08x", attribute.location, static_cast<unsigned long long>(address), data.size(), static_cast<unsigned>(hash));
            static const std::size_t floatLimit = std::getenv("APS5_TRACE_HUD_FLOATS") != nullptr ? static_cast<std::size_t>(std::atoi(std::getenv("APS5_TRACE_HUD_FLOATS"))) : 12u;
            for (std::size_t i = 0; i < floatLimit && (i + 1) * 4 <= data.size(); ++i) {
                float value;
                std::memcpy(&value, data.data() + i * 4, sizeof(value));
                length += std::snprintf(text + length, sizeof(text) - static_cast<std::size_t>(length), " %g", static_cast<double>(value));
            }
            hudVertices += text;
        }
        inputs.vertexHandles.push_back(copy.buffer->Handle());
        inputs.vertexBuffers.push_back(std::move(copy.buffer));
    }
    {

        static const std::array<std::uint64_t, 5> mux = [] {
            std::array<std::uint64_t, 5> values{0, 0, 1, 1, ~0ull};
            const char* text = std::getenv("APS5_SKIP_MUX");
            if (text == nullptr) { values[3] = 0; return values; }
            char* end = nullptr;
            for (std::size_t i = 0; i < values.size() && *text != 0; ++i) {
                values[i] = std::strtoull(text, &end, 10);
                text = *end == ':' ? end + 1 : end;
            }
            return values;
        }();
        if (mux[3] != 0) {
            thread_local std::uint64_t muxFrame = ~0ull;
            thread_local std::uint64_t muxSequence = 0;
            const auto frame = GuestMemory::TraceFrame();
            if (frame != muxFrame) {
                muxFrame = frame;
                muxSequence = 0;
            }
            const auto index = muxSequence++;
            const auto group = (index / std::max<std::uint64_t>(mux[2], 1)) % mux[3];
            if (frame >= mux[0] && frame <= mux[1] && group == (mux[4] != ~0ull ? mux[4] : frame % mux[3])) {
                static std::atomic<int> logged{0};
                if (logged.fetch_add(1) < 5) aps5::LogErr("[mux] skipping frame %llu draw %llu\n", static_cast<unsigned long long>(frame), static_cast<unsigned long long>(index));
                inputs.nothing = true;
                return inputs;
            }
        }
    }
    {

        struct SkipSets {
            std::uint32_t trigger = 0;
            std::vector<std::set<std::uint32_t>> sets;
            std::vector<bool> noTarget;
        };
        static const SkipSets sets = [] {
            SkipSets parsed;
            const char* text = std::getenv("APS5_SKIP_SETS");
            if (text == nullptr) return parsed;
            std::string all = text;
            std::size_t position = 0;
            bool first = true;
            while (position <= all.size()) {
                auto next = all.find('/', position);
                if (next == std::string::npos) next = all.size();
                const auto item = all.substr(position, next - position);
                if (first) {
                    parsed.trigger = static_cast<std::uint32_t>(std::strtoul(item.c_str(), nullptr, 16));
                    first = false;
                } else {
                    std::set<std::uint32_t> ids;
                    bool none = false;
                    std::size_t at = 0;
                    while (at < item.size()) {
                        auto comma = item.find(',', at);
                        if (comma == std::string::npos) comma = item.size();
                        const auto token = item.substr(at, comma - at);
                        if (token == "N") none = true;
                        else if (!token.empty() && token != "-") ids.insert(static_cast<std::uint32_t>(std::strtoul(token.c_str(), nullptr, 16)));
                        at = comma + 1;
                    }
                    parsed.sets.push_back(std::move(ids));
                    parsed.noTarget.push_back(none);
                }
                position = next + 1;
            }
            return parsed;
        }();
        if (!sets.sets.empty()) {
            thread_local std::uint64_t lastTrigger = 0;
            thread_local std::uint64_t event = 0;
            thread_local bool started = false;
            const auto frame = GuestMemory::TraceFrame();
            std::uint32_t pixel = ~0u;
            for (const auto& shader : shaders) {
                if (static_cast<int>(shader.stage) == 5) pixel = static_cast<std::uint32_t>(shader.program->variantId);
            }
            if (pixel == sets.trigger) {
                if (!started || frame > lastTrigger + 30) {
                    event = started ? event + 1 : 0;
                    started = true;
                    aps5::LogErr("[skipsets] event %llu starts at frame %llu (set %llu)\n", static_cast<unsigned long long>(event), static_cast<unsigned long long>(frame), static_cast<unsigned long long>(event % sets.sets.size()));
                }
                lastTrigger = frame;
            }
            if (started && frame <= lastTrigger + 30) {
                const auto index = event % sets.sets.size();
                if (sets.sets[index].count(pixel) != 0 || (sets.noTarget[index] && !state.hasColorTarget)) {
                    inputs.nothing = true;
                    return inputs;
                }
            }
        }
    }
    {

        static const bool traceHud = std::getenv("APS5_TRACE_HUD") != nullptr;
        const auto traceFrame = GuestMemory::TraceFrame();
        if (traceHud && HudTraceWanted(traceFrame)) {
            thread_local std::uint64_t lastFrame = ~0ull;
            thread_local std::uint32_t sequence = 0;
            if (traceFrame != lastFrame) {
                lastFrame = traceFrame;
                sequence = 0;
            }
            char head[256];
            std::snprintf(head, sizeof(head), "[hud] f=%llu d=%u tgt=%llx %ux%u idx=%u inst=%u", static_cast<unsigned long long>(traceFrame), sequence++, static_cast<unsigned long long>(state.hasColorTarget ? state.color.address : 0ull), state.hasColorTarget ? state.color.extent.width : 0u, state.hasColorTarget ? state.color.extent.height : 0u, draw.indexCount, draw.instanceCount);
            std::string line = head;
            {
                char stateText[220];
                std::snprintf(stateText, sizeof(stateText), " sc=%d,%d,%ux%u wm=%x sten=%d cmp%d ref%02x cm%02x wm%02x ops%d/%d/%d zt=%d zw=%d zc=%d cull=%x",
                              state.scissor.offset.x, state.scissor.offset.y, state.scissor.extent.width, state.scissor.extent.height,
                              state.blends.empty() ? 0u : static_cast<unsigned>(state.blends[0].colorWriteMask), state.stencilTest ? 1 : 0,
                              static_cast<int>(state.stencilFront.compareOp), state.stencilFront.reference, state.stencilFront.compareMask, state.stencilFront.writeMask,
                              static_cast<int>(state.stencilFront.failOp), static_cast<int>(state.stencilFront.passOp), static_cast<int>(state.stencilFront.depthFailOp),
                              state.depthTest ? 1 : 0, state.depthWrite ? 1 : 0, static_cast<int>(state.depthCompare), static_cast<unsigned>(state.cullMode));
                char rawText[160];
                std::snprintf(rawText, sizeof(rawText), " dc=%08x zi=%08x si=%08x sc=%08x sr=%08x", state.rawDepthControl, state.rawZInfo, state.rawStencilInfo, state.rawStencilControl, state.rawStencilRef);
                line += rawText;
                if (state.depth) {
                    char surfaceText[96];
                    std::snprintf(surfaceText, sizeof(surfaceText), " ds=%ux%u fmt%d imp%d z%llx s%llx", state.depth->extent.width, state.depth->extent.height, static_cast<int>(state.depth->format), state.depth->implicitStencil ? 1 : 0, static_cast<unsigned long long>(state.depth->address), static_cast<unsigned long long>(state.depth->stencilAddress));
                    line += surfaceText;
                }
                line += stateText;
            }
            for (const auto& shader : shaders) {
                char text[64];
                std::snprintf(text, sizeof(text), " st%d=%llx", static_cast<int>(shader.stage), static_cast<unsigned long long>(shader.program->variantId));
                line += text;
                for (const auto& binding : shader.program->bindings) {
                    if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages || binding.count == 0) continue;
                    const auto elementWords = binding.guestDescriptor.size() / binding.count;
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        char buffer[200];
                        int length = std::snprintf(buffer, sizeof(buffer), " img%u", element);
                        for (std::size_t word = 0; word < elementWords && word < 8; ++word) length += std::snprintf(buffer + length, sizeof(buffer) - static_cast<std::size_t>(length), "%c%08x", word == 0 ? ':' : '.', binding.guestDescriptor[static_cast<std::size_t>(element) * elementWords + word]);
                        if (elementWords >= 2) {
                            const std::uint64_t base = (static_cast<std::uint64_t>(binding.guestDescriptor[static_cast<std::size_t>(element) * elementWords]) | (static_cast<std::uint64_t>(binding.guestDescriptor[static_cast<std::size_t>(element) * elementWords + 1] & 0xffu) << 32u)) << 8u;
                            std::uint32_t nonZero = 0;
                            std::uint64_t hash = 1469598103934665603ull;
                            const void* pointer = reinterpret_cast<const void*>(base);
                            if (base != 0 && GuestMemory::Accessible(pointer, 65536)) {
                                const auto* words = static_cast<const std::uint32_t*>(pointer);
                                for (std::size_t i = 0; i < 16384; ++i) {
                                    nonZero += words[i] != 0;
                                    hash = (hash ^ words[i]) * 1099511628211ull;
                                }
                            }
                            char extra[96];
                            std::snprintf(extra, sizeof(extra), "@%llx nz=%u h=%08x", static_cast<unsigned long long>(base), nonZero, static_cast<unsigned>(hash));
                            line += buffer;
                            line += extra;
                            continue;
                        }
                        line += buffer;
                    }
                }
                std::size_t number = 0;
                for (const auto& binding : shader.program->bindings) {
                    if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
                    for (std::size_t offset = 0; offset + 4 <= binding.guestDescriptor.size(); offset += 4, ++number) {
                        const ShaderRecompiler::ShaderBufferResource d{{binding.guestDescriptor[offset], binding.guestDescriptor[offset + 1], binding.guestDescriptor[offset + 2], binding.guestDescriptor[offset + 3]}};
                        const auto addr = d.Base48();
                        const auto size = d.GetSize();
                        if (addr == 0 || size < 16 || size > 65536) continue;
                        const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(size, 256));
                        if (!GuestMemory::Accessible(reinterpret_cast<const void*>(addr), bytes)) continue;
                        std::uint32_t words[64] = {};
                        std::memcpy(words, reinterpret_cast<const void*>(addr), bytes);
                        std::uint64_t hash = 1469598103934665603ull;
                        for (std::size_t i = 0; i < bytes / 4; ++i) hash = (hash ^ words[i]) * 1099511628211ull;
                        char buffer[400];
                        int length = std::snprintf(buffer, sizeof(buffer), " cb%zu@%llx/%llu h=%08x", number, static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size), static_cast<unsigned>(hash));
                        for (std::size_t i = 0; i < std::min<std::size_t>(bytes / 4, 16); ++i) {
                            float value;
                            std::memcpy(&value, &words[i], sizeof(value));
                            length += std::snprintf(buffer + length, sizeof(buffer) - static_cast<std::size_t>(length), " %g", static_cast<double>(value));
                        }
                        line += buffer;
                    }
                }
            }
            line += hudVertices;
            aps5::LogErr("%s\n", line.c_str());
        }
    }
    {
        static const bool traceDcb = std::getenv("APS5_TRACE_DCB") != nullptr;
        static std::atomic<int> tracedDcb{0};
        if (traceDcb && tracedDcb.load() < 6000) {
            for (const auto& shader : shaders) {
                for (const auto& binding : shader.program->bindings) {
                    if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
                    for (std::size_t offset = 0; offset + 4 <= binding.guestDescriptor.size(); offset += 4) {
                        const ShaderRecompiler::ShaderBufferResource d{{binding.guestDescriptor[offset], binding.guestDescriptor[offset + 1], binding.guestDescriptor[offset + 2], binding.guestDescriptor[offset + 3]}};
                        const auto addr = d.Base48();
                        const auto sz = d.GetSize();
                        if (addr == 0 || sz < 16 || sz > 4096 || !GuestMemory::Accessible(reinterpret_cast<const void*>(addr), 32)) continue;
                        tracedDcb.fetch_add(1);
                        float f[8];
                        std::memcpy(f, reinterpret_cast<const void*>(addr), 32);
                        aps5::LogErr( "[dcb] stage %d el %zu size %llu: %g %g %g %g | %g %g %g %g", static_cast<int>(shader.stage), offset / 4, static_cast<unsigned long long>(sz), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
                        aps5::LogChar(aps5::LogStdErr, 10);
                    }
                }
            }
        }
    }
    timer.phase(PhaseVertex);
    return inputs;
}

struct PassTargetProof {
    struct Slot {
        std::uint64_t address = 0;
        std::uint64_t dcc = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        std::uint32_t mip = 0;
        VkExtent2D extent{};
        std::shared_ptr<StorageTexture> resident;
    };
    const Recorder* recorder = nullptr;
    std::uint64_t epoch = 0;
    std::uint64_t passKey = 0;
    std::vector<Slot> slots;
    bool collecting = false;
};

PassTargetProof& PassProof() {
    thread_local PassTargetProof proof;
    return proof;
}

bool PassProofEnabled() {
    static const bool disabled = std::getenv("APS5_NO_PASS_PROOF") != nullptr;
    return !disabled;
}

bool PassProofSlotMatches(const PassTargetProof::Slot& slot, const ColorTarget& color) {
    return slot.address == color.address && slot.dcc == color.dccAddress && slot.format == color.format && slot.mip == color.mip && slot.extent.width == color.extent.width && slot.extent.height == color.extent.height;
}

bool PassProofHolds(const Context& context, const State& state) {
    if (!PassProofEnabled() || context.recorder == nullptr) return false;
    auto& proof = PassProof();
    if (proof.recorder != context.recorder || proof.passKey == 0 || proof.slots.size() != state.colors.size() || proof.slots.empty()) return false;
    const auto epoch = GuestMemory::ThreadCollectEpoch();
    if (epoch == 0 || epoch != proof.epoch || context.recorder->OpenPassKey() != proof.passKey) return false;
    for (std::size_t i = 0; i < proof.slots.size(); ++i) {
        const auto& slot = proof.slots[i];
        if (slot.resident == nullptr || !slot.resident->Cached() || !PassProofSlotMatches(slot, state.colors[i])) return false;
    }
    return true;
}

void PassProofBegin(const Context& context, std::size_t slots) {
    auto& proof = PassProof();
    proof.recorder = context.recorder;
    proof.epoch = GuestMemory::ThreadCollectEpoch();
    proof.passKey = 0;
    proof.slots.assign(slots, {});
    proof.collecting = proof.epoch != 0 && PassProofEnabled();
}

void PassProofAdd(std::size_t index, const ColorTarget& color, const std::shared_ptr<StorageTexture>& resident) {
    auto& proof = PassProof();
    if (!proof.collecting || index >= proof.slots.size() || resident == nullptr) {
        proof.collecting = false;
        return;
    }
    proof.slots[index] = {color.address, color.dccAddress, color.format, color.mip, color.extent, resident};
}

void PassProofLeft(const Recorder* recorder, std::uint64_t key) {
    auto& proof = PassProof();
    if (!proof.collecting || proof.recorder != recorder) return;
    proof.passKey = key;
}

std::shared_ptr<StorageTexture> refreshResidentTarget(const Context& context, const State& state, const ColorTarget& color, DrawOutcome& outcome, bool profile, const std::function<std::shared_ptr<StorageTexture>()>& lookup) {
    const auto lookupStart = std::chrono::steady_clock::now();
    const auto walkedBefore = profile ? GuestMemory::ThreadCollectedBytes() : 0;
    const LookupOutcomes outcomesBefore = profile ? ThreadLookupOutcomes() : LookupOutcomes{};
    std::shared_ptr<StorageTexture> resident;
    try {
        resident = lookup();
        if (resident != nullptr) materializeRegisterClear(context, color, *resident);
    } catch (const std::exception& error) {
        static std::mutex reportMutex;
        static std::set<std::uint64_t> reported;
        std::lock_guard lock(reportMutex);
        if (reported.insert(color.address).second) aps5::LogErr( "[gpu] color target 0x%llx stays non-resident: %s\n", static_cast<unsigned long long>(color.address), error.what());
    }
    if (profile) {
        const auto lookupUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - lookupStart).count();
        ++outcome.targetLookups;
        if (lookupUs >= 1000.0) {
            ++outcome.slowLookups;
            outcome.slowLookupUs += lookupUs;
        }
        outcome.lookupUs += lookupUs;
        outcome.pagesWalked += (GuestMemory::ThreadCollectedBytes() - walkedBefore) / 4096;
        const auto& outcomesAfter = ThreadLookupOutcomes();
        for (std::size_t kind = 0; kind < RefreshProofCount; ++kind) {
            const auto index = RefreshProofKinds[kind];
            outcome.refreshCounts[kind] += outcomesAfter.counts[index] - outcomesBefore.counts[index];
            outcome.refreshUs[kind] += (outcomesAfter.ms[index] - outcomesBefore.ms[index]) * 1000.0;
        }
        const bool fullScissor = state.scissor.offset.x <= 0 && state.scissor.offset.y <= 0 && state.scissor.extent.width >= color.extent.width && state.scissor.extent.height >= color.extent.height;
        ++(fullScissor ? outcome.fullScissorLookups : outcome.partialLookups);
    }
    return resident;
}

struct ResolvedResources {
    std::shared_ptr<ShaderResources> resources;
    ResourceCache::Key contentKey;
    bool cacheable = false;
    const ShaderResources::BuildTiming* built = nullptr;
};

ResolvedResources resolveDrawResources(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, std::uint64_t indexBytes, bool recordable, DrawOutcome& outcome, DrawTimer& timer) {
    ResolvedResources resolved;
    APS5_LOG_CHARS_OUT_DEBUG("Creating ShaderResources");

    static const bool noDrawResourceCache = std::getenv("APS5_NO_DRAW_RESOURCE_CACHE") != nullptr;
    static const bool noTextureCache = std::getenv("APS5_NO_TEXTURE_CACHE") != nullptr;

    static const bool trimKey = std::getenv("APS5_NO_DRAW_KEY_TRIM") == nullptr;
    resolved.cacheable = recordable && !noDrawResourceCache && !noTextureCache && std::all_of(shaders.begin(), shaders.end(), [](const CompiledShader& shader) { return shader.program != nullptr && shader.program->variantId != 0; });
    if (resolved.cacheable) {
        resolved.contentKey = DrawResourceKey(context, shaders, state.color, draw.indexAddress, indexBytes, !trimKey);
        if (auto cached = SharedResourceCache().Find(resolved.contentKey)) {
            if (cached->Revalidate(shaders)) {
                if (trimKey) CheckBufferAliases(shaders, state.color, draw.indexAddress, indexBytes);
                resolved.resources = std::move(cached);
                outcome.kind = KindTemplateHit;
                countCache(&DrawProfile::cacheHits);
            } else {
                SharedResourceCache().Remove(resolved.contentKey);
                countCache(&DrawProfile::cacheInvalidated);
            }
        }
    } else {
        countCache(&DrawProfile::uncacheable);
    }
    timer.phase(PhaseLookup);
    if (resolved.resources == nullptr) {
        resolved.resources = std::make_shared<ShaderResources>(context, shaders, state.color, draw.indexAddress, static_cast<std::size_t>(indexBytes), snapshots);
        resolved.built = &resolved.resources->Timing();
        outcome.addressBased = resolved.resources->HoldsLease();
        outcome.kind = outcome.addressBased ? KindBda : KindBuild;
        if (resolved.cacheable) countCache(&DrawProfile::cacheMisses);
    }
    timer.phase(PhaseResources);
    APS5_LOG_CHARS_OUT_DEBUG("ShaderResources created");
    return resolved;
}

struct IndirectRecord {
    const Pm4::DrawParameters::IndirectDraw* args = nullptr;
    IndirectDrawPath path = IndirectDrawPath::Gpu;
    const HostImport* argumentImport = nullptr;
    const HostImport* countImport = nullptr;
    std::span<const Pm4::DrawArguments> records;
    double readMs = 0;
};

void recordDrawCommands(const Context& context, VkCommandBuffer commands, const State& state, const Pm4::DrawParameters& draw, const DrawInputs& inputs, const IndirectRecord* indirect, VkBuffer argumentBuffer, VkDeviceSize argumentOffset) {
    const auto* args = indirect != nullptr ? indirect->args : nullptr;
    if (state.stages.mesh) {
        APS5_LOG_OUT_DEBUG("vkCmdDrawMeshTasksEXT groups=%u instances=%u", inputs.meshGroups, draw.instanceCount);
        context.Function<PFN_vkCmdDrawMeshTasksEXT>("vkCmdDrawMeshTasksEXT")(commands, inputs.meshGroups, draw.instanceCount, 1);
        return;
    }
    if (!inputs.vertexHandles.empty()) context.Resolved(&DeviceFunctions::cmdBindVertexBuffers, "vkCmdBindVertexBuffers")(commands, 0, static_cast<std::uint32_t>(inputs.vertexHandles.size()), inputs.vertexHandles.data(), inputs.vertexOffsets.data());
    if (draw.indexed) context.Resolved(&DeviceFunctions::cmdBindIndexBuffer, "vkCmdBindIndexBuffer")(commands, inputs.indices->Handle(), 0, draw.indexSize == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    if (args == nullptr) {
        if (draw.indexed) context.Resolved(&DeviceFunctions::cmdDrawIndexed, "vkCmdDrawIndexed")(commands, draw.indexCount, draw.instanceCount, 0, static_cast<std::int32_t>(draw.firstVertex), draw.firstInstance);
        else context.Resolved(&DeviceFunctions::cmdDraw, "vkCmdDraw")(commands, draw.indexCount, draw.instanceCount, draw.firstVertex, draw.firstInstance);
    } else if (indirect->path == IndirectDrawPath::Gpu) {
        if (args->countIndirect) {
            if (draw.indexed) context.Function<PFN_vkCmdDrawIndexedIndirectCountKHR>("vkCmdDrawIndexedIndirectCountKHR")(commands, argumentBuffer, argumentOffset, indirect->countImport->buffer, args->countAddress - indirect->countImport->base, args->count, args->stride);
            else context.Function<PFN_vkCmdDrawIndirectCountKHR>("vkCmdDrawIndirectCountKHR")(commands, argumentBuffer, argumentOffset, indirect->countImport->buffer, args->countAddress - indirect->countImport->base, args->count, args->stride);
        } else if (args->count <= 1 || context.multiDrawIndirect) {
            if (draw.indexed) context.Resolved(&DeviceFunctions::cmdDrawIndexedIndirect, "vkCmdDrawIndexedIndirect")(commands, argumentBuffer, argumentOffset, args->count, args->stride);
            else context.Resolved(&DeviceFunctions::cmdDrawIndirect, "vkCmdDrawIndirect")(commands, argumentBuffer, argumentOffset, args->count, args->stride);
        } else {
            for (std::uint32_t record = 0; record < args->count; ++record) {
                const auto offset = argumentOffset + static_cast<VkDeviceSize>(record) * args->stride;
                if (draw.indexed) context.Resolved(&DeviceFunctions::cmdDrawIndexedIndirect, "vkCmdDrawIndexedIndirect")(commands, argumentBuffer, offset, 1, args->stride);
                else context.Resolved(&DeviceFunctions::cmdDrawIndirect, "vkCmdDrawIndirect")(commands, argumentBuffer, offset, 1, args->stride);
            }
        }
    } else {

        using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
        for (const auto& record : indirect->records) {
            if (record.count == 0 || record.instances == 0) continue;
            const auto firstInstance = args->instanceRule == Rule::InPlace ? record.firstInstance : args->instanceConstant;
            if (draw.indexed) {

                if (record.firstVertexOrIndex >= draw.indexCount) continue;
                const auto vertexOffset = args->vertexRule == Rule::InPlace ? record.vertexOffset : args->vertexConstant;
                context.Resolved(&DeviceFunctions::cmdDrawIndexed, "vkCmdDrawIndexed")(commands, std::min(record.count, draw.indexCount - record.firstVertexOrIndex), record.instances, record.firstVertexOrIndex, static_cast<std::int32_t>(vertexOffset), firstInstance);
            } else {
                const auto firstVertex = args->vertexRule == Rule::InPlace ? record.firstVertexOrIndex : args->vertexConstant;
                context.Resolved(&DeviceFunctions::cmdDraw, "vkCmdDraw")(commands, record.count, record.instances, firstVertex, firstInstance);
            }
        }
    }
}

bool recordIndirectArguments(const Context& context, VkCommandBuffer commands, Recorder* recorder, bool recorded, const IndirectRecord& indirect, std::unique_ptr<DeviceBuffer>& scratch, VkBuffer& argumentBuffer, VkDeviceSize& argumentOffset, const std::function<void(std::uint32_t)>& countBarrier) {
    using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
    const auto* args = indirect.args;
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    countBarrier(1);
    if (recorded) {

        recorder->NotePendingRead(args->arguments, static_cast<std::size_t>(args->RangeBytes()), Recorder::ReadKind::Indirect);
        if (args->countIndirect) recorder->NotePendingRead(args->countAddress, 4, Recorder::ReadKind::Indirect);
    }
    argumentBuffer = indirect.argumentImport->buffer;
    argumentOffset = args->arguments - indirect.argumentImport->base;
    const bool rewritten = args->vertexRule == Rule::Constant || args->instanceRule == Rule::Constant;
    if (!rewritten) return false;
    const auto bytes = static_cast<std::size_t>(args->RangeBytes());
    scratch = std::make_unique<DeviceBuffer>(context, bytes, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    CopyBuffer(context, commands, argumentBuffer, argumentOffset, scratch->Handle(), 0, bytes);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    countBarrier(1);
    const auto update = context.Resolved(&DeviceFunctions::cmdUpdateBuffer, "vkCmdUpdateBuffer");
    for (std::uint32_t record = 0; record < args->count; ++record) {
        const VkDeviceSize base = static_cast<VkDeviceSize>(record) * args->stride;
        if (args->vertexRule == Rule::Constant) update(commands, scratch->Handle(), base + args->VertexDwordOffset(), 4, &args->vertexConstant);
        if (args->instanceRule == Rule::Constant) update(commands, scratch->Handle(), base + args->InstanceDwordOffset(), 4, &args->instanceConstant);
    }
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    countBarrier(1);
    argumentBuffer = scratch->Handle();
    argumentOffset = 0;
    return true;
}

std::function<void()> indirectRecordCheck(const IndirectRecord* indirect) {
    static const bool checkArguments = std::getenv("APS5_CHECK_INDIRECT_ARGS") != nullptr;
    if (indirect == nullptr || indirect->args == nullptr || indirect->path != IndirectDrawPath::Gpu || !checkArguments) return {};
    return [args = *indirect->args] {
        using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
        static std::atomic<std::uint64_t> printed{0};
        static std::atomic<std::uint64_t> ignoredValues{0};
        static std::atomic<std::uint64_t> checked{0};
        try {
            for (std::uint32_t record = 0; record < args.count; ++record) {
                const auto arguments = Pm4::ReadDrawArguments(args, record);
                const bool ignoredVertex = args.vertexRule == Rule::Constant && (args.recordBytes == 20 ? arguments.vertexOffset : arguments.firstVertexOrIndex) != 0;
                const bool ignoredInstance = args.instanceRule == Rule::Constant && arguments.firstInstance != 0;
                ++checked;
                if (ignoredVertex || ignoredInstance) ++ignoredValues;
                if (printed.fetch_add(1) < 16) aps5::LogErr( "[draw] indirect record 0x%llx: count %u instances %u first %u vertexOffset %u startInstance %u (vertex %s, instance %s)\n", static_cast<unsigned long long>(args.arguments + static_cast<std::uint64_t>(record) * args.stride), arguments.count, arguments.instances, arguments.firstVertexOrIndex, arguments.vertexOffset, arguments.firstInstance, args.vertexRule == Rule::Constant ? "const: record value ignored" : "in-place", args.instanceRule == Rule::Constant ? "const: record value ignored" : "in-place");
            }
        } catch (const std::exception& error) {
            aps5::LogErr( "[draw] indirect record read-back failed: %s\n", error.what());
        }
        if (checked % 64 == 0) aps5::LogErr( "[draw] indirect records checked %llu, with a value in a Constant dimension %llu\n", static_cast<unsigned long long>(checked.load()), static_cast<unsigned long long>(ignoredValues.load()));
    };
}

struct Kept {
    std::shared_ptr<ShaderResources> resources;
    std::shared_ptr<Pipeline> pipeline;
    std::shared_ptr<Framebuffer> framebuffer;
    std::shared_ptr<Buffer> indices;
    std::vector<std::shared_ptr<Buffer>> vertexBuffers;
    std::vector<std::shared_ptr<StorageTexture>> targets;
    std::unique_ptr<DeviceBuffer> scratch;
};

void keepRecordedDraw(Recorder& recorder, const std::shared_ptr<ShaderResources>& resources, std::shared_ptr<Pipeline> pipeline, std::shared_ptr<Framebuffer> framebuffer, DrawInputs& inputs, std::vector<std::shared_ptr<StorageTexture>> targets, std::unique_ptr<DeviceBuffer> scratch, std::function<void()> checkRecords, bool listed, bool completion, const DrawOutcome& outcome) {
    auto kept = std::make_shared<Kept>();
    kept->resources = resources;
    kept->pipeline = std::move(pipeline);
    kept->framebuffer = std::move(framebuffer);
    kept->indices = std::move(inputs.indices);
    kept->vertexBuffers = std::move(inputs.vertexBuffers);
    kept->scratch = std::move(scratch);
    kept->targets = std::move(targets);
    const auto& residents = kept->targets;
    recorder.Keep(kept);
    if (checkRecords) recorder.OnComplete(std::move(checkRecords));

    if (resources->HoldsLease()) CountLeaseOutcome(outcome.waited, outcome.waited ? 0 : recorder.Submissions() + 1);

    resources->MarkGpuWrites(recorder);

    if (listed) {

        auto writers = DrawCopiedWriters();
        recorder.OnComplete([resources, writers] {
            writers->erase(std::remove(writers->begin(), writers->end(), resources), writers->end());
            resources->WriteBackBuffers();
        });
        writers->push_back(resources);
    } else if (completion) {
        recorder.OnComplete([resources] { resources->WriteBackBuffers(); });
    }
    for (const auto& resident : residents) {
        if (resident != nullptr) resident->MarkDirty();
    }
}

struct RecordedDraw {
    Recorder* recorder = nullptr;
    std::shared_ptr<ShaderResources> resources;
    std::shared_ptr<Pipeline> pipeline;
    std::shared_ptr<Framebuffer> framebuffer;
    std::span<const VkImageView> targetViews;
    std::vector<std::shared_ptr<StorageTexture>> targets;
    const IndirectRecord* indirect = nullptr;
    bool listed = false;
    bool completion = false;
    bool waited = false;
    const std::array<std::byte, PipelinePushConstantBytes>* pushBytes = nullptr;
    VkShaderStageFlags pushStages = 0;
};

void pushDrawConstants(const Pipeline& pipeline, VkCommandBuffer commands, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, const ShaderResources& resources, const std::array<std::byte, PipelinePushConstantBytes>* bytes, VkShaderStageFlags stages) {
    auto block = bytes != nullptr ? *bytes : AssemblePushConstants(shaders);
    if (bytes == nullptr) {
        resources.PatchPushConstants(block);
        stages = PushConstantStages(shaders);
    }
    if (!state.stages.mesh) {
        pipeline.PushConstants(commands, stages, block);
        return;
    }
    const std::array<std::uint32_t, ShaderRecompiler::MeshDrawPushBytes / 4> words{draw.indexCount, draw.firstVertex, draw.firstInstance, draw.indexed ? draw.indexSize : 0u, 0u, 0u};
    static_assert(ShaderRecompiler::MeshDrawPushOffsetBytes + ShaderRecompiler::MeshDrawPushBytes == PipelinePushConstantBytes);
    std::memcpy(block.data() + ShaderRecompiler::MeshDrawPushOffsetBytes, words.data(), sizeof(words));
    pipeline.PushConstants(commands, stages | VK_SHADER_STAGE_MESH_BIT_EXT, block);
}

bool CaptureInputsEnabled() {
    static const bool enabled = std::getenv("APS5_CAPTURE_INPUTS") != nullptr;
    Require(!enabled || CaptureTrace::Enabled(), "APS5_CAPTURE_INPUTS requires APS5_CAPTURE_TRACE");
    return enabled;
}

void captureInputs(const Context& context, Recorder& recorder, VkCommandBuffer commands, const ShaderResources& resources, const std::shared_ptr<ShaderResources::DrawBindings>& bindings, std::uint64_t target) {
    struct Sample {
        std::uint64_t address;
        std::size_t offset;
        std::vector<std::byte> expected;
        VkBuffer source;
        VkDeviceSize sourceOffset;
    };
    static unsigned long long nextDraw = 0;
    const auto draw = ++nextDraw;
    const auto batch = static_cast<unsigned long long>(recorder.Submissions() + 1);
    std::vector<Sample> samples;
    std::size_t total = 0;
    const auto addSample = [&](std::uint64_t address, std::size_t bytes, VkBuffer source, VkDeviceSize offset, const std::byte* expected) {
        if (bytes == 0 || bytes > 512) return;
        Require(offset % 4 == 0 && bytes % 4 == 0, "capture input is not aligned for a Vulkan buffer copy");
        Require(total + bytes <= 65536, "capture inputs exceed 64 KiB per draw");
        Sample sample{address, total, std::vector<std::byte>(bytes), source, offset};
        std::memcpy(sample.expected.data(), expected, bytes);
        total += bytes;
        samples.push_back(std::move(sample));
    };
    if (bindings != nullptr) {
        for (const auto& snapshot : bindings->snapshots) {
            const auto bytes = snapshot.buffer->Bytes();
            addSample(snapshot.address, bytes.size(), snapshot.buffer->Handle(), 0, bytes.data());
        }
    }
    for (const auto& [begin, end] : resources.InPlaceReads()) {
        Require(end >= begin, "invalid capture input range");
        const auto bytes = static_cast<std::size_t>(end - begin);
        if (bytes == 0 || bytes > 512) continue;
        if (bindings != nullptr && std::any_of(bindings->snapshots.begin(), bindings->snapshots.end(), [&](const auto& snapshot) { return snapshot.address < end && begin < snapshot.address + snapshot.buffer->Bytes().size(); })) continue;
        if (resources.WritesOverlap(begin, bytes) || recorder.PendingWriteOverlaps(begin, bytes)) {
            CaptureTrace::Log("input-skip draw=%llu batch=%llu address=%llx bytes=%zu reason=gpu-writer", draw, batch, static_cast<unsigned long long>(begin), bytes);
            continue;
        }
        const auto* imported = HostImportFor(context, begin, bytes);
        Require(imported != nullptr, "capture input has no host import");
        addSample(begin, bytes, imported->buffer, begin - imported->base, reinterpret_cast<const std::byte*>(begin));
    }
    CaptureTrace::Log("input-capture draw=%llu batch=%llu target=%llx ranges=%zu bytes=%zu", draw, batch, static_cast<unsigned long long>(target), samples.size(), total);
    if (samples.empty()) return;
    auto readback = std::make_shared<Buffer>(context, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const auto copy = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    for (const auto& sample : samples) {
        const VkBufferCopy region{sample.sourceOffset, sample.offset, sample.expected.size()};
        copy(commands, sample.source, readback->Handle(), 1, &region);
    }
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    recorder.OnComplete([draw, batch, target, readback, samples = std::move(samples)] {
        const auto actual = readback->Bytes();
        std::size_t differences = 0;
        for (const auto& sample : samples) {
            std::size_t changed = 0;
            for (std::size_t index = 0; index < sample.expected.size(); ++index) {
                const auto gpu = actual[sample.offset + index];
                if (sample.expected[index] == gpu) continue;
                if (changed < 16) CaptureTrace::Log("input-byte draw=%llu batch=%llu address=%llx offset=%zu cpu=%02x gpu=%02x", draw, batch, static_cast<unsigned long long>(sample.address), index, std::to_integer<unsigned>(sample.expected[index]), std::to_integer<unsigned>(gpu));
                ++changed;
            }
            if (changed != 0) {
                ++differences;
                CaptureTrace::Log("input-mismatch draw=%llu batch=%llu target=%llx address=%llx bytes=%zu changed=%zu", draw, batch, static_cast<unsigned long long>(target), static_cast<unsigned long long>(sample.address), sample.expected.size(), changed);
            }
        }
        CaptureTrace::Log("input-result draw=%llu batch=%llu ranges=%zu mismatched=%zu", draw, batch, samples.size(), differences);
    });
}

void recordDraw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, DrawInputs& inputs, RecordedDraw& record, DrawOutcome& outcome, DrawTimer& timer, double& ownWaitedMs) {
    auto* recorder = record.recorder;
    auto& resources = *record.resources;
    const auto* args = record.indirect != nullptr ? record.indirect->args : nullptr;
    const bool gpuIndirect = args != nullptr && record.indirect->path == IndirectDrawPath::Gpu;
    using CommandClass = Recorder::CommandClass;
    const auto countBarrier = [&](std::uint32_t count) { Recorder::CountBarriers(CommandClass::Draw, count); };

    std::uint64_t passKey = 14695981039346656037ull;
    const auto mix = [&](std::uint64_t value) {
        passKey ^= value;
        passKey *= 1099511628211ull;
    };
    for (const auto view : record.targetViews) mix(reinterpret_cast<std::uint64_t>(view));
    if (state.blends.size() != state.colors.size()) {
        mix(state.blends.size());
        for (const auto& color : state.colors) mix(color.slot);
    }
    mix(state.renderExtent.width);
    mix(state.renderExtent.height);
    const bool readsTarget = std::any_of(record.targets.begin(), record.targets.end(), [&](const std::shared_ptr<StorageTexture>& target) { return resources.ReadsImage(target.get()); });

    const auto touches = [&](std::uint64_t begin, std::uint64_t end) {
        const auto bytes = static_cast<std::size_t>(end - begin);
        if (resources.WritesOverlap(begin, bytes) || resources.ReadsOverlap(begin, bytes)) return true;
        if (!gpuIndirect) return false;
        return (begin < args->arguments + args->RangeBytes() && args->arguments < end) || (args->countIndirect && begin < args->countAddress + 4 && args->countAddress < end);
    };
    if (recorder->HasQueuedKeyStores() && (resources.HoldsLease() || recorder->AnyQueuedKeyStore(touches))) recorder->FlushKeyStores();

    if (recorder->HasQueuedStores() && (resources.HoldsLease() || recorder->AnyQueuedStore(touches))) recorder->FlushStores();
    const auto drawBindings = resources.PrepareDrawBindings(*recorder);
    const bool capture = CaptureInputsEnabled();
    const bool continued = !capture && !readsTarget && !gpuIndirect && recorder->ContinuesRenderPass(passKey);
    outcome.passContinued = continued;
    outcome.passBegun = !continued;
    const auto commands = continued ? recorder->CommandsInRenderPass() : recorder->Commands();
    if (capture) captureInputs(context, *recorder, commands, resources, drawBindings, record.targets.empty() || record.targets.front() == nullptr ? 0 : record.targets.front()->Descriptor().baseAddress);

    const auto drawTiming = !continued ? recorder->BeginGpuTiming(CommandClass::Draw) : Recorder::NoTiming;
    if (!continued && Recorder::BarrierValidate()) {
        auto reads = resources.InPlaceReads();
        if (gpuIndirect) {
            reads.emplace_back(args->arguments, args->arguments + args->RangeBytes());
            if (args->countIndirect) reads.emplace_back(args->countAddress, args->countAddress + 4);
        }
        auto images = resources.StorageImages();
        for (const auto& target : record.targets) images.emplace_back(target->Image(), true);
        recorder->NoteAccess(CommandClass::Draw, Recorder::Access{reads, resources.GpuWrites(), images, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | inputs.shaderStages, resources.HoldsLease()});
    }
    std::unique_ptr<DeviceBuffer> scratch;
    VkBuffer argumentBuffer = VK_NULL_HANDLE;
    VkDeviceSize argumentOffset = 0;
    bool rewritten = false;
    if (continued) {
        record.pipeline->Continue(commands, state);
    } else {
        const VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | inputs.shaderStages, 0, 1, &before, 0, nullptr, 0, nullptr);
        countBarrier(1);
        APS5_LOG_CHARS_OUT_DEBUG("Upload barrier recorded");
        if (gpuIndirect) rewritten = recordIndirectArguments(context, commands, recorder, true, *record.indirect, scratch, argumentBuffer, argumentOffset, countBarrier);

        APS5_LOG_OUT_DEBUG("Beginning pipeline renderExtent=%ux%u", state.renderExtent.width, state.renderExtent.height);
        record.pipeline->Begin(commands, *record.framebuffer, state.renderExtent, state);
    }
    APS5_LOG_CHARS_OUT_DEBUG("Pipeline Begin OK");
    if (drawBindings != nullptr) {
        const auto set = drawBindings->allocation.set;
        context.Resolved(&DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, record.pipeline->Layout(), 0, 1, &set, 0, nullptr);
    } else {
        resources.Bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, record.pipeline->Layout());
    }
    APS5_LOG_CHARS_OUT_DEBUG("Resources bound");
    pushDrawConstants(*record.pipeline, commands, state, draw, shaders, resources, record.pushBytes, record.pushStages);
    APS5_LOG_CHARS_OUT_DEBUG("Push constants recorded");
    recordDrawCommands(context, commands, state, draw, inputs, record.indirect, argumentBuffer, argumentOffset);
    if (args != nullptr) CountIndirectDraw(record.indirect->path, record.indirect->readMs, rewritten);
    auto checkRecords = indirectRecordCheck(record.indirect);
    APS5_LOG_CHARS_OUT_DEBUG("Draw recorded");

    recorder->LeaveRenderPassOpen(passKey, drawTiming, !resources.WritesMemory());
    if (!resources.WritesMemory()) PassProofLeft(recorder, passKey);
    timer.phase(PhaseRecord);
    keepRecordedDraw(*recorder, record.resources, std::move(record.pipeline), std::move(record.framebuffer), inputs, std::move(record.targets), std::move(scratch), std::move(checkRecords), record.listed, record.completion, outcome);
    timer.phase(PhaseKeep);
    if (record.waited) {

        Recorder::CountSync(3);
        const auto ownBefore = timer.profile ? Recorder::ThreadWaitedMs() : 0.0;
        recorder->Sync();
        if (timer.profile) ownWaitedMs += Recorder::ThreadWaitedMs() - ownBefore;
        timer.phase(PhaseSync);
    }
}

std::optional<State> maskedState(const State& state, const std::set<std::uint32_t>& fragmentOutputs) {
    std::optional<State> masked;
    for (std::size_t index = 0; index < state.blends.size(); ++index) {
        if (fragmentOutputs.contains(static_cast<std::uint32_t>(index)) || state.blends[index].colorWriteMask == 0) continue;
        if (!masked.has_value()) masked = state;
        masked->blends[index].colorWriteMask = 0;
    }
    return masked;
}

int DumpTargetLimit() {
    static const int dumpLimit = [] { const char* text = std::getenv("APS5_DUMP_TARGETS"); return text ? std::atoi(text) : 0; }();
    if (dumpLimit == 0) return 0;
    static const int afterSeconds = [] { const char* text = std::getenv("APS5_DUMP_TARGETS_AFTER"); return text ? std::atoi(text) : 0; }();
    if (afterSeconds == 0) return dumpLimit;
    static const int windowSeconds = [] { const char* text = std::getenv("APS5_DUMP_TARGETS_WINDOW"); return text ? std::max(std::atoi(text), 1) : 8; }();
    static const auto start = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
    return elapsed < afterSeconds || elapsed >= afterSeconds + windowSeconds ? 0 : dumpLimit;
}

int DumpTargetSkip() {
    static const int skip = [] { const char* text = std::getenv("APS5_DUMP_TARGETS_SKIP"); return text ? std::max(std::atoi(text), 0) : 0; }();
    return skip;
}

bool RecordDraws() {
    static const bool recordDraws = std::getenv("APS5_SYNC_DRAWS") == nullptr;
    return recordDraws;
}

void traceDrawState(const State& state, const Pm4::DrawParameters& draw, const CompiledShader* shaders, std::size_t shaderCount, bool force);

void targetMaskCensus(const State& state, const Pm4::DrawParameters& draw, const CompiledShader* shaders, std::size_t shaderCount) {
    static const bool on = std::getenv("APS5_TARGET_MASKS") != nullptr;
    if (!on || state.colors.empty()) return;
    struct Row {
        std::uint64_t count = 0;
        int format = 0;
        unsigned mapping = 0;
        unsigned extent = 0;
        unsigned elementBytes = 0;
    };
    using Table = std::map<std::uint64_t, std::map<unsigned, Row>>;
    static std::mutex mutex;
    static Table rows;
    static std::uint64_t sequence = 0;
    static std::atomic<int> detailPrinted;
    static auto lastReport = std::chrono::steady_clock::now();
    std::vector<std::pair<std::uint64_t, std::map<unsigned, Row>>> ranked;
    const bool report = std::chrono::steady_clock::now() - lastReport > std::chrono::seconds(10);
    const char* detail = nullptr;
    {
        std::lock_guard lock(mutex);
        const auto address = state.color.address;
        const auto mask = static_cast<unsigned>(state.blend.colorWriteMask);
        auto& row = rows[address][mask];
        const bool first = row.count++ == 0;
        if (first) {
            row.format = static_cast<int>(state.color.format);
            row.mapping = state.color.componentMapping;
            row.extent = (state.color.extent.width << 16) | state.color.extent.height;
            row.elementBytes = state.color.elementBytes;

            if (state.colors.size() <= 2 && detailPrinted.fetch_add(1) < 60) detail = "first";
        }
        ++sequence;
        if (report) {
            lastReport = std::chrono::steady_clock::now();
            ranked.assign(rows.begin(), rows.end());
            rows.clear();
        }
    }
    if (detail != nullptr) traceDrawState(state, draw, shaders, shaderCount, true);
    if (ranked.empty()) return;
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        auto total = [](const auto& table) { std::uint64_t sum = 0; for (const auto& [mask, row] : table) sum += row.count; return sum; };
        return total(a.second) > total(b.second);
    });
    for (std::size_t index = 0; index < ranked.size() && index < 14; ++index) {
        const auto& [address, table] = ranked[index];
        std::string text;
        std::uint64_t total = 0;
        char part[40];
        for (const auto& [mask, row] : table) {
            std::snprintf(part, sizeof(part), " 0x%04x:%llu", mask, static_cast<unsigned long long>(row.count));
            text += part;
            total += row.count;
        }
        const auto& first = table.begin()->second;
        aps5::LogErr( "[tmask] 0x%llx %ux%u vk=%d elem=%u map=0x%x draws=%llu|%s\n", static_cast<unsigned long long>(address),
                     first.extent >> 16u, first.extent & 0xffffu, first.format, first.elementBytes, first.mapping,
                     static_cast<unsigned long long>(total), text.c_str());
    }
    aps5::LogFlush(aps5::LogStdErr);
}

void traceDrawState(const State& state, const Pm4::DrawParameters& draw, const CompiledShader* shaders, std::size_t shaderCount, bool force) {

    static const bool drawGeometry = std::getenv("APS5_DRAW_GEOMETRY") != nullptr;
    if (drawGeometry) {
        static std::atomic<std::uint64_t> total, fewVerts, midVerts, manyVerts, indexedTotal, rectTotal, noColor, largest, lastReport;

        const auto vertices = static_cast<std::uint64_t>(draw.indexCount);
        const auto previous = total.fetch_add(1) + 1;
        (vertices <= 6 ? fewVerts : vertices <= 64 ? midVerts : manyVerts).fetch_add(1);
        if (draw.indexed) ++indexedTotal;
        if (state.rectList) ++rectTotal;
        if (state.colors.empty()) ++noColor;
        auto peak = largest.load();
        while (vertices > peak && !largest.compare_exchange_weak(peak, vertices)) {}

        if (previous / 5000 != lastReport.load(std::memory_order_relaxed) / 5000) {
            lastReport.store(previous);
            aps5::LogErr( "[geometry] %llu draws: <=6 verts %llu, 7-64 %llu, >64 %llu; indexed %llu, rect-list %llu, colourless %llu; largest %llu\n",
                static_cast<unsigned long long>(previous),
                static_cast<unsigned long long>(fewVerts.load()), static_cast<unsigned long long>(midVerts.load()),
                static_cast<unsigned long long>(manyVerts.load()), static_cast<unsigned long long>(indexedTotal.load()),
                static_cast<unsigned long long>(rectTotal.load()), static_cast<unsigned long long>(noColor.load()),
                static_cast<unsigned long long>(largest.load()));
            aps5::LogFlush(aps5::LogStdErr);
        }
        return;
    }
    static const std::uint64_t stride = [] {
        const char* text = std::getenv("APS5_TRACE_DRAW_STATE");
        return text ? std::max<std::uint64_t>(std::strtoull(text, nullptr, 10), 1) : 0ull;
    }();
    if (stride == 0 && !force) return;
    static std::atomic<std::uint64_t> seen;
    static std::atomic<int> printed;
    if (!force && (seen.fetch_add(1) % stride != 0 || printed.fetch_add(1) >= 400)) return;
    if (force) ++seen;
    const auto& color = state.color;
    const auto* args = draw.indirect ? &*draw.indirect : nullptr;
    aps5::LogErr(
        "[drawstate] #%llu %s addr=0x%llx %ux%u vk=%d elem=%u mip=%u/%u mask=0x%x | idx=0x%llx count=%u size=%u first=%u inst=%u | prim=%d restart=%d rect=%d | scissor (%d,%d) %ux%u | vp %g..%g x %g..%g | render %ux%u | cull=%d front=%d | depth test=%d cmp=%d write=%d | blend mask=0x%x en=%d op=%d src=%d dst=%d | colors=%zu\n",
        static_cast<unsigned long long>(seen.load()), draw.indexed ? "indexed" : "auto",
        static_cast<unsigned long long>(color.address), color.extent.width, color.extent.height, static_cast<int>(color.format),
        color.elementBytes, color.mip, color.mipCount, static_cast<unsigned>(color.componentMapping),
        static_cast<unsigned long long>(draw.indexAddress), draw.indexCount, draw.indexSize, draw.firstVertex, draw.instanceCount,
        static_cast<int>(state.topology), static_cast<int>(state.primitiveRestart), static_cast<int>(state.rectList),
        state.scissor.offset.x, state.scissor.offset.y, state.scissor.extent.width, state.scissor.extent.height,
        state.viewport.x, state.viewport.x + state.viewport.width, state.viewport.y, state.viewport.y + state.viewport.height,
        state.renderExtent.width, state.renderExtent.height, static_cast<int>(state.cullMode), static_cast<int>(state.frontFace),
        static_cast<int>(state.depthTest), static_cast<int>(state.depthCompare), static_cast<int>(state.depthWrite),
        static_cast<unsigned>(state.blend.colorWriteMask), state.blend.blendEnable, state.blend.colorBlendOp, state.blend.srcColorBlendFactor, state.blend.dstColorBlendFactor,
        state.colors.size());
    for (std::size_t i = 0; i < shaderCount && shaders != nullptr; ++i) aps5::LogErr( "[drawstate]   stage %d program=%p\n", static_cast<int>(shaders[i].stage), reinterpret_cast<const void*>(shaders[i].program));
    if (args != nullptr) aps5::LogErr( "[drawstate]   indirect args=0x%llx opcode=0x%x count=%u stride=%u recordBytes=%u\n", static_cast<unsigned long long>(args->arguments), args->opcode, args->count, args->stride, args->recordBytes);
    aps5::LogFlush(aps5::LogStdErr);
}

}

std::optional<std::string> KnownValidationFailure(const Context& context, std::span<const CompiledShader> shaders, const State& state) {
    std::vector<std::uint64_t> key;
    if (!ValidationKey(context, shaders, state, key)) return std::nullopt;
    std::lock_guard lock(validationMutex());
    const auto& failures = validationFailures();
    if (const auto found = failures.find(key); found != failures.end()) return found->second;
    return std::nullopt;
}

void Draw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, std::shared_ptr<const DrawRecipe>* recipeOut) {
    PerformanceTimer timing("Graphics.Draw");

    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    DrawTimer timer(profile);

    static const bool traceDraws = std::getenv("APS5_TRACE_DRAWS") != nullptr;
    DrawOutcome outcome;
    const ShaderResources::BuildTiming* built = nullptr;

    const auto waitedBefore = profile ? Recorder::ThreadWaitedMs() : 0.0;
    double ownWaitedMs = 0;
    const auto report = [&](const char* suffix) { reportDrawEnd(state, timer, built, outcome, waitedBefore, ownWaitedMs, suffix); };
    auto inputs = prepareDrawInputs(context, state, draw, shaders, outcome, timer, nullptr);
    if (inputs.nothing) return;
    const auto* args = draw.indirect ? &*draw.indirect : nullptr;
    const auto indexBytes = inputs.indexBytes;
    traceDrawState(state, draw, shaders.data(), shaders.size(), false);
    targetMaskCensus(state, draw, shaders.data(), shaders.size());
    timing.Mark("validate");
    timing.Mark("index_upload");

    struct TargetBinding {
        ColorTarget color;
        bool gpuTiling = false;

        std::shared_ptr<StorageTexture> resident;

        std::unique_ptr<Buffer> transfer;

        std::unique_ptr<Buffer> tiled;
        std::unique_ptr<DeviceBuffer> tiledDevice;
        std::unique_ptr<DeviceBuffer> linearDevice;
        std::vector<std::byte> original;
        TileMipLayout mip{};
        std::unique_ptr<RenderTarget> target;

        std::unique_ptr<Buffer> dump;
    };
    const int dumpLimit = DumpTargetLimit();
    static std::mutex dumpMutex;
    static std::map<std::uint64_t, int> dumped;
    static std::map<std::uint64_t, int> targetDraws;
    std::vector<TargetBinding> targets(state.colors.size());
    std::vector<VkImageView> targetViews;
    const bool passProofHolds = PassProofHolds(context, state);
    if (!passProofHolds) PassProofBegin(context, state.colors.size());
    for (std::size_t index = 0; index < state.colors.size(); ++index) {
        auto& binding = targets[index];
        binding.color = state.colors[index];
        const auto& color = binding.color;
        binding.gpuTiling = color.tileMode == ColorTileMode::RenderTarget && context.detiler != nullptr;
        APS5_LOG_OUT_DEBUG("Creating color target %zu address=0x%llx bytes=%llu extent=%ux%u", index, static_cast<unsigned long long>(color.address), static_cast<unsigned long long>(color.bytes), color.extent.width, color.extent.height);
        const ColorTargetLayout colorLayout(color.extent.width, color.extent.height, color.tileMode, color.elementBytes);
        constexpr VkBufferUsageFlags copies = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        timer.phase(PhaseSetup);

        static const bool residentTargets = std::getenv("APS5_NO_RESIDENT_TARGETS") == nullptr;

        bool dumpOwed = false;
        if (dumpLimit != 0) {
            std::lock_guard lock(dumpMutex);
            const auto found = dumped.find(color.address);
            const auto draws = ++targetDraws[color.address];
            dumpOwed = draws > DumpTargetSkip() && (found == dumped.end() || found->second < dumpLimit);
        }
        if (passProofHolds && (binding.gpuTiling || (color.tileMode == ColorTileMode::Standard4KB && context.detiler != nullptr)) && residentTargets && !dumpOwed) {
            binding.resident = PassProof().slots[index].resident;
        } else if ((binding.gpuTiling || (color.tileMode == ColorTileMode::Standard4KB && context.detiler != nullptr)) && residentTargets && !dumpOwed) {

            binding.resident = refreshResidentTarget(context, state, color, outcome, profile, [&] {
                auto resident = CachedStorageSurface(context, SurfaceForTarget(color));
                Require(resident->Attachable(), "storage format cannot be a color attachment");
                Require(color.mipCount > 1 || resident->GuestBytes() == colorLayout.Bytes(), "resident image layout differs from the color layout");
                return resident;
            });
        }
        if (binding.resident != nullptr) {
            timer.phase(PhaseReadTarget);
            if (!passProofHolds) PassProofAdd(index, color, binding.resident);
            targetViews.push_back(binding.resident->AttachmentView(color.format, color.mip));
            continue;
        }
        if (!passProofHolds) PassProofAdd(index, color, nullptr);
        Require(!color.mipTail, "rendering into a packed mip tail needs the resident image of its surface");
        if (binding.gpuTiling) {
            binding.mip = ColorTargetMip(color, colorLayout);
            binding.tiled = std::make_unique<Buffer>(context, colorLayout.Bytes(), copies);
            binding.tiledDevice = std::make_unique<DeviceBuffer>(context, colorLayout.Bytes(), copies | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            binding.linearDevice = std::make_unique<DeviceBuffer>(context, colorLayout.LinearBytes(), copies | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            binding.original.resize(colorLayout.Bytes());
            GuestMemory::Read(color.address, binding.original, colorLayout.Alignment());
            std::memcpy(binding.tiled->Bytes().data(), binding.original.data(), binding.original.size());
        } else {
            binding.transfer = std::make_unique<Buffer>(context, colorLayout.LinearBytes(), copies);
            ReadColorTarget(color, binding.transfer->Bytes());
        }

        if (color.dccAddress != 0) {
            const auto keys = CurrentDccKeys(color.dccAddress, colorLayout.Bytes());
            if (IsDccClear(keys)) {
                const auto pixels = binding.gpuTiling ? binding.tiled->Bytes() : binding.transfer->Bytes();
                if (keys == DccKeys::ClearRegister) {
                    const auto texel = clearTexel(color, keys);
                    for (std::size_t offset = 0; offset + color.elementBytes <= pixels.size(); offset += color.elementBytes) std::memcpy(pixels.data() + offset, texel.data(), color.elementBytes);
                } else if (!FillDccClear(color.format, keys, color.dccAlphaOnMsb, pixels)) {
                    static std::set<std::pair<std::uint64_t, int>> reported;
                    if (reported.size() < 32 && reported.insert({color.address, static_cast<int>(keys)}).second) aps5::LogErr( "[gpu] color target 0x%llx (VkFormat %d) has %s DCC keys; its stored texels are used\n", static_cast<unsigned long long>(color.address), static_cast<int>(color.format), DccKeysName(keys));
                    if (binding.gpuTiling) std::memcpy(pixels.data(), binding.original.data(), binding.original.size());
                    else ReadColorTarget(color, pixels);
                }
            }
        }
        timer.phase(PhaseReadTarget);
        binding.target = std::make_unique<RenderTarget>(context, color, state.blends.at(color.slot).blendEnable != 0);
        targetViews.push_back(binding.target->View());
    }
    if (state.depth) targetViews.push_back(DepthSurfaceView(context, *state.depth));
    timer.phase(PhasePrepare);
    const bool recordDraws = RecordDraws();
    auto* recorder = Recorder::Active();
    const bool recordable = recordDraws && recorder != nullptr && dumpLimit == 0 && std::all_of(targets.begin(), targets.end(), [](const TargetBinding& binding) { return binding.resident != nullptr; });
    auto resolved = resolveDrawResources(context, state, draw, shaders, snapshots, indexBytes, recordable, outcome, timer);
    auto& resources = resolved.resources;
    const auto& contentKey = resolved.contentKey;
    const bool cacheable = resolved.cacheable;
    built = resolved.built;

    IndirectRecord indirect;
    indirect.args = args;
    std::vector<Pm4::DrawArguments> records;
    if (args != nullptr) {
        static const bool gpuIndirectDraws = std::getenv("APS5_NO_GPU_INDIRECT_DRAW") == nullptr;
        const auto decide = [&](std::uint64_t address, std::size_t bytes, const HostImport*& import) {
            if (StorageTexture::FlushPending(address, bytes, nullptr, "indirect draw arguments")) {
                if (recorder != nullptr) {
                    Recorder::CountSync(2);
                    recorder->Sync();
                }
                return IndirectDrawPath::PendingImage;
            }
            if (recorder != nullptr && recorder->PendingLabelIn(address, bytes)) return IndirectDrawPath::PendingLabelOrCopy;
            const auto overlaps = [&](const auto& writer) { return writer->WritesOverlap(address, bytes); };
            if (context.copiedWriters != nullptr && std::any_of(context.copiedWriters->begin(), context.copiedWriters->end(), overlaps)) return IndirectDrawPath::PendingLabelOrCopy;
            if (std::any_of(DrawCopiedWriters()->begin(), DrawCopiedWriters()->end(), overlaps)) return IndirectDrawPath::PendingLabelOrCopy;
            if ((import = HostImportFor(context, address, bytes)) == nullptr) return IndirectDrawPath::NotImported;
            return IndirectDrawPath::Gpu;
        };
        const auto rangeBytes = args->RangeBytes();
        if (rangeBytes == 0) {
            report(" indirect draw without records");
            return;
        }
        Require(rangeBytes <= std::numeric_limits<std::size_t>::max(), "indirect draw record range overflow");
        if (!gpuIndirectDraws) indirect.path = IndirectDrawPath::Disabled;
        else indirect.path = decide(args->arguments, static_cast<std::size_t>(rangeBytes), indirect.argumentImport);
        if (indirect.path == IndirectDrawPath::Gpu && args->countIndirect) {
            Require(context.drawIndirectCount, "indirect draw count without VK_KHR_draw_indirect_count must be resolved by the driver");
            indirect.path = decide(args->countAddress, 4, indirect.countImport);
        }
        if (indirect.path != IndirectDrawPath::Gpu) {

            const auto readStart = std::chrono::steady_clock::now();
            const auto count = std::min(args->countIndirect ? Pm4::ReadDrawCount(*args) : args->count, args->count);
            records.reserve(count);
            for (std::uint32_t record = 0; record < count; ++record) records.push_back(Pm4::ReadDrawArguments(*args, record));
            indirect.readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count();
            indirect.records = records;
            if (std::none_of(records.begin(), records.end(), [](const Pm4::DrawArguments& record) { return record.count != 0 && record.instances != 0; })) {
                CountIndirectDraw(indirect.path, indirect.readMs);
                report(" indirect draw with empty records");
                return;
            }
        }
    }

    static const bool syncCompletionDraws = std::getenv("APS5_SYNC_COMPLETION_DRAWS") != nullptr;
    static const bool recorderSyncDraws = std::getenv("APS5_NO_RECORDER_SYNC_DRAWS") == nullptr;
    static const bool recordCopiedDraws = std::getenv("APS5_NO_RECORD_COPIED_DRAWS") == nullptr;
    const bool completion = resources->NeedsCompletion();
    const bool copiedWrites = completion && resources->HasCopiedWrites();

    const bool lease = resources->HoldsLease() && SyncLeaseWork();
    outcome.completion = completion;
    outcome.recorded = recordable;

    bool listed = false;
    if (!recordable) {
        outcome.reason = !recordDraws || dumpLimit != 0 ? SyncDisabled : recorder == nullptr ? SyncNoRecorder : SyncNotResident;
    } else if (completion && (syncCompletionDraws || copiedWrites || lease)) {
        outcome.reason = syncCompletionDraws ? SyncDisabled : lease ? SyncLease : SyncCopiedWrites;
        if (syncCompletionDraws) outcome.recorded = false;
        else if (copiedWrites && !lease && recordCopiedDraws) listed = true;
        else if (recorderSyncDraws) outcome.waited = true;
        else outcome.recorded = false;
    }
    const bool recorded = outcome.recorded;

    if (cacheable && built != nullptr && recorded && resources->Reusable()) SharedResourceCache().Insert(contentKey, resources);

    static const bool drawTransitions = std::getenv("APS5_DRAW_TRANSITIONS") != nullptr;
    const bool lean = recorded && !drawTransitions;
    APS5_LOG_CHARS_OUT_DEBUG("Creating Pipeline");

    auto masked = maskedState(state, inputs.fragmentOutputs);
    const State& pipelineState = masked.has_value() ? *masked : state;
    auto pipeline = CachedPipeline(context, pipelineState, inputs.vertexInput, *resources, shaders, lean ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    std::vector<std::shared_ptr<StorageTexture>> owners;
    owners.reserve(targets.size());
    for (const auto& binding : targets) owners.push_back(binding.resident);
    auto framebuffer = pipeline->AcquireFramebuffer(targetViews, owners, state.renderExtent);
    timer.phase(PhasePipeline);
    APS5_LOG_CHARS_OUT_DEBUG("Pipeline created");
    if (lean) {
        RecordedDraw record;
        record.recorder = recorder;
        record.resources = resources;
        record.pipeline = pipeline;
        record.framebuffer = framebuffer;
        record.targetViews = targetViews;
        record.targets = owners;
        record.indirect = args != nullptr ? &indirect : nullptr;
        record.listed = listed;
        record.completion = completion;
        record.waited = outcome.waited;
        recordDraw(context, state, draw, shaders, inputs, record, outcome, timer, ownWaitedMs);

        if (recipeOut != nullptr && DrawRecipes() && cacheable && !outcome.waited && args == nullptr && resources->Reusable() && !state.depth) {
            auto recipe = std::make_shared<DrawRecipe>();
            recipe->device = context.device;
            recipe->templateRef = resources;
            recipe->key = contentKey;
            recipe->pipeline = pipeline;
            recipe->framebuffer = framebuffer;
            for (const auto& owner : owners) recipe->targets.emplace_back(owner);
            recipe->targetViews = targetViews;
            std::uint64_t passKey = 14695981039346656037ull;
            for (const auto view : targetViews) passKey = (passKey ^ reinterpret_cast<std::uint64_t>(view)) * 1099511628211ull;
            passKey = (passKey ^ state.renderExtent.width) * 1099511628211ull;
            passKey = (passKey ^ state.renderExtent.height) * 1099511628211ull;
            recipe->passKey = passKey;
            recipe->vertexInput = inputs.vertexInput;
            recipe->pushStages = PushConstantStages(shaders);
            if (recipe->pushStages != 0) {
                recipe->pushBytes = AssemblePushConstants(shaders);
                resources->PatchPushConstants(recipe->pushBytes);
            }
            recipe->masked = masked;
            recipe->fragmentOutputs = inputs.fragmentOutputs;
            recipe->shaderStages = inputs.shaderStages;
            *recipeOut = std::move(recipe);
        }
        timing.Mark("draw_and_resource_release");
        report(outcome.waited ? (outcome.reason == SyncLease ? " recorded then waited (lease)" : " recorded then waited (copied writes)") : completion ? " recorded with completion" : " recorded");
        return;
    }
    APS5_LOG_CHARS_OUT_DEBUG("Creating CommandBatch");
    std::optional<CommandBatch> batch;
    if (!recorded) {

        if (context.detiler != nullptr) context.detiler->BeginBatch();
        batch.emplace(context);
    }
    using CommandClass = Recorder::CommandClass;
    const auto countBarrier = [&](std::uint32_t count = 1) { if (recorded) Recorder::CountBarriers(CommandClass::Draw, count); };
    const bool gpuIndirect = args != nullptr && indirect.path == IndirectDrawPath::Gpu;

    const auto touches = [&](std::uint64_t begin, std::uint64_t end) {
        const auto bytes = static_cast<std::size_t>(end - begin);
        if (resources->WritesOverlap(begin, bytes) || resources->ReadsOverlap(begin, bytes)) return true;
        if (!gpuIndirect) return false;
        return (begin < args->arguments + args->RangeBytes() && args->arguments < end) || (args->countIndirect && begin < args->countAddress + 4 && args->countAddress < end);
    };
    if (recorded && recorder->HasQueuedKeyStores() && (resources->HoldsLease() || recorder->AnyQueuedKeyStore(touches))) recorder->FlushKeyStores();

    if (recorded && recorder->HasQueuedStores() && (resources->HoldsLease() || recorder->AnyQueuedStore(touches))) recorder->FlushStores();
    const auto commands = recorded ? recorder->Commands() : batch->Handle();
    APS5_LOG_CHARS_OUT_DEBUG("CommandBatch created");

    const auto drawTiming = recorded ? recorder->BeginGpuTiming(CommandClass::Draw) : Recorder::NoTiming;
    if (recorded && Recorder::BarrierValidate()) {
        auto reads = resources->InPlaceReads();
        if (gpuIndirect) {
            reads.emplace_back(args->arguments, args->arguments + args->RangeBytes());
            if (args->countIndirect) reads.emplace_back(args->countAddress, args->countAddress + 4);
        }
        auto images = resources->StorageImages();
        for (const auto& binding : targets) {
            if (binding.resident != nullptr) images.emplace_back(binding.resident->Image(), true);
        }
        recorder->NoteAccess(CommandClass::Draw, Recorder::Access{reads, resources->GpuWrites(), images, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | inputs.shaderStages, resources->HoldsLease()});
    }
    std::unique_ptr<DeviceBuffer> scratch;
    VkBuffer argumentBuffer = VK_NULL_HANDLE;
    VkDeviceSize argumentOffset = 0;
    bool rewritten = false;
    VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    upload.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    upload.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | inputs.shaderStages, 0, 1, &upload, 0, nullptr, 0, nullptr);
    countBarrier();
    if (state.depth) {
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        countBarrier();
    }
    APS5_LOG_CHARS_OUT_DEBUG("Upload barrier recorded");
    if (gpuIndirect) rewritten = recordIndirectArguments(context, commands, recorder, recorded, indirect, scratch, argumentBuffer, argumentOffset, countBarrier);
    for (auto& binding : targets) {
        if (binding.resident != nullptr) {
            imageBarrier(context, commands, binding.resident->Image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            countBarrier();
            continue;
        }
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {binding.color.extent.width, binding.color.extent.height, 1};
        const VkBuffer linear = binding.gpuTiling ? binding.linearDevice->Handle() : binding.transfer->Handle();
        countBarrier(binding.gpuTiling ? 4 : 2);
        if (binding.gpuTiling) {
            CopyBuffer(context, commands, binding.tiled->Handle(), 0, binding.tiledDevice->Handle(), 0, binding.original.size());
            memoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            context.detiler->Dispatch(commands, TextureTileMode::kR64KBX, binding.color.elementBytes, binding.tiledDevice->Handle(), 0, binding.linearDevice->Handle(), 0, binding.mip);
            memoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        }
        imageBarrier(context, commands, binding.target->Image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        context.Resolved(&DeviceFunctions::cmdCopyBufferToImage, "vkCmdCopyBufferToImage")(commands, linear, binding.target->Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        imageBarrier(context, commands, binding.target->Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    APS5_LOG_OUT_DEBUG("Beginning pipeline renderExtent=%ux%u", state.renderExtent.width, state.renderExtent.height);
    pipeline->Begin(commands, *framebuffer, state.renderExtent, state);
    APS5_LOG_CHARS_OUT_DEBUG("Pipeline Begin OK");
    resources->Bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Layout());
    APS5_LOG_CHARS_OUT_DEBUG("Resources bound");
    pushDrawConstants(*pipeline, commands, state, draw, shaders, *resources, nullptr, 0);
    APS5_LOG_CHARS_OUT_DEBUG("Push constants recorded");
    recordDrawCommands(context, commands, state, draw, inputs, args != nullptr ? &indirect : nullptr, argumentBuffer, argumentOffset);
    if (args != nullptr) CountIndirectDraw(indirect.path, indirect.readMs, rewritten);
    auto checkRecords = indirectRecordCheck(args != nullptr ? &indirect : nullptr);
    APS5_LOG_CHARS_OUT_DEBUG("Draw recorded");
    context.Resolved(&DeviceFunctions::cmdEndRenderPass, "vkCmdEndRenderPass")(commands);
    APS5_LOG_CHARS_OUT_DEBUG("Render pass ended");
    for (auto& binding : targets) {
        if (binding.resident != nullptr) {
            imageBarrier(context, commands, binding.resident->Image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
            countBarrier();
            continue;
        }
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {binding.color.extent.width, binding.color.extent.height, 1};
        countBarrier(binding.gpuTiling ? 4 : 2);
        imageBarrier(context, commands, binding.target->Image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBuffer linear = binding.gpuTiling ? binding.linearDevice->Handle() : binding.transfer->Handle();
        VkBufferMemoryBarrier reuse{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        reuse.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        reuse.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        reuse.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        reuse.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        reuse.buffer = linear;
        reuse.size = VK_WHOLE_SIZE;
        context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &reuse, 0, nullptr);
        context.Resolved(&DeviceFunctions::cmdCopyImageToBuffer, "vkCmdCopyImageToBuffer")(commands, binding.target->Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, linear, 1, &copy);
        if (binding.gpuTiling) {
            memoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
            if (dumpLimit > 0) {
                std::lock_guard lock(dumpMutex);
                if (dumped[binding.color.address] < dumpLimit) {
                    binding.dump = std::make_unique<Buffer>(context, binding.linearDevice->Size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                    CopyBuffer(context, commands, binding.linearDevice->Handle(), 0, binding.dump->Handle(), 0, binding.linearDevice->Size());
                }
            }
            context.detiler->Dispatch(commands, TextureTileMode::kR64KBX, binding.color.elementBytes, binding.linearDevice->Handle(), 0, binding.tiledDevice->Handle(), 0, binding.mip, true);
            memoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            CopyBuffer(context, commands, binding.tiledDevice->Handle(), 0, binding.tiled->Handle(), 0, binding.original.size());
        }
    }
    VkMemoryBarrier download{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    download.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    download.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT | inputs.shaderStages, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &download, 0, nullptr, 0, nullptr);
    countBarrier();
    if (recorded) recorder->EndGpuTiming(drawTiming);
    APS5_LOG_CHARS_OUT_DEBUG("Download barrier recorded");
    APS5_LOG_CHARS_OUT_DEBUG("SubmitAndWait");
    timer.phase(PhaseRecord);
    if (recorded) {
        keepRecordedDraw(*recorder, resources, pipeline, framebuffer, inputs, owners, std::move(scratch), std::move(checkRecords), listed, completion, outcome);
        timer.phase(PhaseKeep);
        if (outcome.waited) {

            Recorder::CountSync(3);
            const auto ownBefore = profile ? Recorder::ThreadWaitedMs() : 0.0;
            recorder->Sync();
            if (profile) ownWaitedMs += Recorder::ThreadWaitedMs() - ownBefore;
            timer.phase(PhaseSync);
        }
        report(outcome.waited ? (outcome.reason == SyncLease ? " recorded then waited (lease)" : " recorded then waited (copied writes)") : completion ? " recorded with completion" : " recorded");
        return;
    }
    {

        const auto ownBefore = profile ? Recorder::ThreadWaitedMs() : 0.0;
        batch->SubmitAndWait();
        if (profile) ownWaitedMs += Recorder::ThreadWaitedMs() - ownBefore;
    }
    if (checkRecords) checkRecords();
    timer.phase(PhaseSync);
    APS5_LOG_CHARS_OUT_DEBUG("SubmitAndWait OK");
    for (const auto& binding : targets) GuestMemory::CheckRange(reinterpret_cast<const void*>(binding.color.address), binding.color.bytes, 256, true);
    for (auto& binding : targets) {
        if (!binding.dump) continue;
        std::lock_guard lock(dumpMutex);
        char name[64];
        std::snprintf(name, sizeof(name), "target_%llx_%d.raw", static_cast<unsigned long long>(binding.color.address), dumped[binding.color.address]++);
        if (std::FILE* file = std::fopen(name, "wb")) {
            const std::uint32_t header[3] = {binding.color.extent.width, binding.color.extent.height, static_cast<std::uint32_t>(binding.color.format)};
            std::fwrite(header, sizeof(header), 1, file);
            std::fwrite(binding.dump->Bytes().data(), 1, binding.dump->Bytes().size(), file);
            std::fclose(file);
        }
    }
    APS5_LOG_CHARS_OUT_DEBUG("Shader resources WriteBack");
    resources->WriteBack();
    APS5_LOG_CHARS_OUT_DEBUG("Shader resources WriteBack OK");
    static const bool skipTargetWrite = std::getenv("APS5_NO_TARGET_WRITEBACK") != nullptr;
    for (const auto& binding : targets) {
        if (skipTargetWrite) break;
        if (binding.resident != nullptr) {

            binding.resident->MarkDirty();
            continue;
        }
        if (binding.gpuTiling) GuestMemory::WriteChanged(binding.color.address, binding.tiled->Bytes(), binding.original);
        else WriteColorTarget(binding.color, binding.transfer->Bytes());

        MarkDccUncompressed(binding.color.dccAddress, ColorTargetLayout(binding.color.extent.width, binding.color.extent.height, binding.color.tileMode, binding.color.elementBytes).Bytes());
    }
    timer.phase(PhaseWriteBack);
    if (profile && traceDraws) {

        std::size_t nonzero = 0;
        std::size_t sampled = 0;
        if (!targets.empty() && targets.front().resident == nullptr) {
            const auto bytes = targets.front().gpuTiling ? targets.front().tiled->Bytes() : targets.front().transfer->Bytes();
            sampled = bytes.size() / 64;
            for (std::size_t i = 0; i < bytes.size(); i += 64) nonzero += bytes[i] != std::byte{0};
        }
        aps5::LogErr( "[draw]   inputs:%s\n", resources->Describe().c_str());
        if (targets.size() > 1) {
            std::string list;
            for (const auto& binding : targets) {
                char entry[64];
                std::snprintf(entry, sizeof(entry), " 0x%llx(%d,%ux%u)", static_cast<unsigned long long>(binding.color.address), static_cast<int>(binding.color.format), binding.color.extent.width, binding.color.extent.height);
                list += entry;
            }
            aps5::LogErr( "[draw]   targets:%s\n", list.c_str());
        }
        timer.phase(PhaseDescribe);
        char suffix[160];
        std::snprintf(suffix, sizeof(suffix), " synchronous (%s), first 0x%llx format %d (%zu of %zu sampled bytes nonzero)", SyncReasonNames[outcome.reason], static_cast<unsigned long long>(state.color.address), static_cast<int>(state.color.format), nonzero, sampled);
        report(suffix);
    } else {
        report(" synchronous");
    }
    APS5_LOG_CHARS_OUT_DEBUG("Draw finished");
}

DrawRecipeOutcome DrawWithRecipe(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, const DrawRecipe& recipe) {
    static_cast<void>(snapshots);
    PerformanceTimer timing("Graphics.DrawWithRecipe");
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    DrawTimer timer(profile);
    DrawRecipeOutcome result;
    DrawOutcome outcome;
    outcome.kind = KindRecipeHit;
    const auto waitedBefore = profile ? Recorder::ThreadWaitedMs() : 0.0;
    double ownWaitedMs = 0;
    const auto miss = [&](DrawRecipeMiss reason) {
        result.miss = reason;
        if (profile) {
            auto& stats = Profile();
            std::lock_guard lock(stats.mutex);
            ++stats.recipeMisses[static_cast<std::size_t>(reason)];
        }
        return result;
    };
    Require(!draw.indirect, "a draw recipe covers direct draws only");
    auto* recorder = Recorder::Active();
    if (!RecordDraws() || recorder == nullptr || DumpTargetLimit() != 0) return miss(DrawRecipeMiss::NotRecordable);
    Require(recipe.targets.size() == state.colors.size() && recipe.targetViews.size() == state.colors.size(), "draw recipe targets do not match the state");
    auto inputs = prepareDrawInputs(context, state, draw, shaders, outcome, timer, &recipe);
    if (inputs.nothing) {
        result.recorded = true;
        return result;
    }

    std::vector<std::shared_ptr<StorageTexture>> targets;
    targets.reserve(recipe.targets.size());
    const bool passProofHolds = PassProofHolds(context, state);
    if (!passProofHolds) PassProofBegin(context, state.colors.size());
    for (std::size_t index = 0; index < recipe.targets.size(); ++index) {
        timer.phase(PhaseSetup);
        auto stored = recipe.targets[index].lock();
        if (stored == nullptr || !StorageImageCached(context, stored.get())) return miss(DrawRecipeMiss::TargetGone);
        std::shared_ptr<StorageTexture> resident;
        if (passProofHolds && PassProof().slots[index].resident == stored) {
            resident = stored;
        } else {
            resident = refreshResidentTarget(context, state, state.colors[index], outcome, profile, [&] {
                stored->Refresh();
                return stored;
            });
            if (passProofHolds) PassProofBegin(context, state.colors.size());
        }
        if (!passProofHolds || PassProof().collecting) PassProofAdd(index, state.colors[index], resident);
        timer.phase(PhaseReadTarget);
        if (resident == nullptr) return miss(DrawRecipeMiss::TargetGone);
        targets.push_back(std::move(resident));
    }
    timer.phase(PhasePrepare);

    auto resources = recipe.templateRef.lock();
    if (resources == nullptr) return miss(DrawRecipeMiss::TemplateGone);
    Require(resources->Reusable(), "draw recipe over a non-reusable template");
    const auto proofStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const bool proved = resources->ProveCurrent(shaders, &result.proof);
    if (profile) result.proofUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - proofStart).count();
    if (!proved) {
        SharedResourceCache().Remove(recipe.key, resources.get());
        countCache(&DrawProfile::cacheInvalidated);
        recorder->Keep(std::move(resources));
        timer.phase(PhaseLookup);
        return miss(DrawRecipeMiss::Proof);
    }
    CheckBufferAliases(shaders, state.color, draw.indexAddress, inputs.indexBytes);
    SharedResourceCache().Touch(recipe.key);
    countCache(&DrawProfile::cacheHits);
    timer.phase(PhaseLookup);
    outcome.completion = false;
    outcome.recorded = true;

    auto pipeline = recipe.pipeline.lock();
    if (pipeline == nullptr) return miss(DrawRecipeMiss::ObjectsGone);
    auto framebuffer = recipe.framebuffer.lock();
    if (framebuffer == nullptr) framebuffer = pipeline->AcquireFramebuffer(recipe.targetViews, targets, state.renderExtent);
    timer.phase(PhasePipeline);
    const auto recordStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    RecordedDraw record;
    record.recorder = recorder;
    record.resources = std::move(resources);
    record.pipeline = std::move(pipeline);
    record.framebuffer = std::move(framebuffer);
    record.targetViews = recipe.targetViews;
    record.targets = std::move(targets);
    record.pushBytes = &recipe.pushBytes;
    record.pushStages = recipe.pushStages;
    recordDraw(context, state, draw, shaders, inputs, record, outcome, timer, ownWaitedMs);
    if (profile) {
        result.recordUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - recordStart).count();
        auto& stats = Profile();
        std::lock_guard lock(stats.mutex);
        ++stats.recipeHits;
    }
    timing.Mark("draw_and_resource_release");
    reportDrawEnd(state, timer, nullptr, outcome, waitedBefore, ownWaitedMs, " recorded from recipe");
    result.recorded = true;
    return result;
}

void RunColorMetadataPass(const Context& context, const ColorMetadataPass& pass) {
    for (const auto& color : pass.targets) {
        if (color.dccAddress == 0) continue;
        auto keys = CurrentDccKeys(color.dccAddress, color.bytes);
        if (keys == DccKeys::Uncompressed) continue;
        Require(IsDccClear(keys), std::string("CB metadata pass over DCC keys that are ") + DccKeysName(keys) + " (per-block metadata is not modeled)");
        const auto texel = clearTexel(color, keys);
        std::shared_ptr<StorageTexture> resident;
        if ((color.tileMode == ColorTileMode::RenderTarget || color.tileMode == ColorTileMode::Standard4KB) && context.detiler != nullptr) {
            try {
                resident = CachedStorageSurface(context, SurfaceForTarget(color));
            } catch (const std::exception&) {
                resident = nullptr;
            }
            if (resident != nullptr && resident->GuestBytes() != color.bytes) resident = nullptr;
        }
        if (resident != nullptr) {
            const char* refusal = nullptr;
            const bool current = keys == DccKeys::ClearRegister ? clearToTexel(*resident, texel, color.elementBytes, refusal) : StorageTexture::FindPending(color.address, color.bytes) == resident || resident->UploadedKeys() == keys;
            if (current) {
                resident->MarkDirty();
                MarkDccUncompressed(context, color.dccAddress, color.bytes);
                continue;
            }
        }
        storeClearTexels(context, color, texel);
    }
}

}
