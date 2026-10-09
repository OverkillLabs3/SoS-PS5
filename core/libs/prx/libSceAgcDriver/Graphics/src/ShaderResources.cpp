#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/common/StderrLog.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libc/include/General.hpp"
#include "Optimization/include/Optimization/ShaderStageInputInfo.hpp"
#include "RdnaDecoder/include/RdnaDecoder/RdnaDescriptorFormat.hpp"
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <tuple>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include "prx/libc/include/GuestAllocations.hpp"

namespace AgcDriver::Graphics {
namespace {

bool overlap(std::uint64_t first, std::size_t firstSize, std::uint64_t second, std::size_t secondSize) {
    return first < second + secondSize && second < first + firstSize;
}

VkComponentSwizzle ComponentSwizzleFor(std::uint8_t dstSel) {
    switch (dstSel) {
        case 0: return VK_COMPONENT_SWIZZLE_ZERO;
        case 1: return VK_COMPONENT_SWIZZLE_ONE;
        case 4: return VK_COMPONENT_SWIZZLE_R;
        case 5: return VK_COMPONENT_SWIZZLE_G;
        case 6: return VK_COMPONENT_SWIZZLE_B;
        case 7: return VK_COMPONENT_SWIZZLE_A;
        default: throw std::runtime_error("AGC graphics: guest texture descriptor has an invalid destination channel selector " + std::to_string(dstSel));
    }
}

struct TextureKey {
    VkDevice device;
    std::array<std::uint32_t, 8> words;
    std::array<std::uint32_t, 4> components;
    bool depthCompare = false;
    bool operator==(const TextureKey&) const = default;
};

std::uint64_t hashWords(std::uint64_t hash, const std::uint32_t* words, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) hash = (hash ^ words[i]) * 1099511628211ull;
    return hash;
}

struct TextureKeyHash {
    std::size_t operator()(const TextureKey& key) const noexcept {
        auto hash = 14695981039346656037ull ^ reinterpret_cast<std::uintptr_t>(key.device);
        hash = hashWords(hash, key.words.data(), key.words.size()) ^ static_cast<std::uint64_t>(key.depthCompare);
        return static_cast<std::size_t>(hashWords(hash, key.components.data(), key.components.size()));
    }
};

TextureKey MakeTextureKey(VkDevice device, std::span<const std::uint32_t> words, VkComponentMapping components, bool depthCompare = false) {
    TextureKey key{device, {}, {static_cast<std::uint32_t>(components.r), static_cast<std::uint32_t>(components.g), static_cast<std::uint32_t>(components.b), static_cast<std::uint32_t>(components.a)}};
    key.depthCompare = depthCompare;
    std::copy(words.begin(), words.end(), key.words.begin());
    return key;
}

struct CachedTexture {
    TextureKey key;
    std::uint64_t address;
    std::vector<std::byte> bytes;
    std::shared_ptr<Texture> texture;

    DccKeys keys = DccKeys::Uncompressed;

    std::uint64_t generation = 0;

    std::shared_ptr<StorageTexture> source;
    std::uint64_t sourceVersion = 0;
    std::uint64_t accounted = 0;
};

struct TextureCache {
    std::mutex mutex;
    std::list<CachedTexture> entries;
    std::unordered_map<TextureKey, std::list<CachedTexture>::iterator, TextureKeyHash> index;
    std::uint64_t bytes = 0;
};

TextureCache& Textures() {
    static TextureCache cache;
    return cache;
}

bool TextureHashEnabled() {
    static const bool disabled = std::getenv("APS5_NO_TEXTURE_HASH") != nullptr;
    return !disabled;
}

std::list<CachedTexture>::iterator findTexture(TextureCache& cache, const TextureKey& key) {
    if (TextureHashEnabled()) {
        const auto found = cache.index.find(key);
        return found == cache.index.end() ? cache.entries.end() : found->second;
    }
    for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
        if (it->key == key) return it;
    }
    return cache.entries.end();
}

void eraseTexture(TextureCache& cache, std::list<CachedTexture>::iterator it) {
    cache.bytes -= it->accounted;
    cache.index.erase(it->key);
    cache.entries.erase(it);
}

void touchTexture(TextureCache& cache, std::list<CachedTexture>::iterator it) {
    cache.entries.splice(cache.entries.begin(), cache.entries, it);
}

struct TextureCounters {
    std::atomic<std::uint64_t> fromStorage{0};
    std::atomic<std::uint64_t> snapshots{0};
    std::atomic<std::uint64_t> pendingReads{0};
    std::atomic<std::uint64_t> storageFallbacks{0};
    std::atomic<std::uint64_t> records{0};
    std::atomic<std::uint64_t> fastHits{0};
    std::atomic<std::uint64_t> fastMisses{0};
    std::atomic<std::uint64_t> storageHits{0};
    std::atomic<std::uint64_t> storageCreated{0};

    std::atomic<std::uint64_t> ownRefreshes{0};
    std::atomic<std::int64_t> lastReport{0};
};

TextureCounters& TextureCounts() {
    static TextureCounters counters;
    return counters;
}

void reportTextureCounters() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    auto& counters = TextureCounts();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = counters.lastReport.load();
    if (nowMs - last < 10000 || !counters.lastReport.compare_exchange_strong(last, nowMs)) return;
    const auto count = [](const std::atomic<std::uint64_t>& value) { return static_cast<unsigned long long>(value.load(std::memory_order_relaxed)); };
    aps5::LogErr( "[textures] sampled created: %llu from storage images, %llu snapshots (%llu snapshot reads over recorded writes inside cachedTexture, %llu storage-path fallbacks); stage-A records %llu: %llu fast hits, %llu full lookups; storage images %llu hits, %llu created, %llu own-object refreshes\n", count(counters.fromStorage), count(counters.snapshots), count(counters.pendingReads), count(counters.storageFallbacks), count(counters.records), count(counters.fastHits), count(counters.fastMisses), count(counters.storageHits), count(counters.storageCreated), count(counters.ownRefreshes));
}

struct LookupRecord {
    const void* object;
    GuestTextureResource resource;
    std::uint64_t bytes;
    DccKeys keys;
    std::uint64_t generation;
    const StorageTexture* source;
};

thread_local std::vector<LookupRecord> lookupLog;

void logLookup(const LookupRecord& record) {

    constexpr std::size_t bound = 1024;
    if (lookupLog.size() >= bound) lookupLog.erase(lookupLog.begin(), lookupLog.begin() + bound / 2);
    lookupLog.push_back(record);
}

std::shared_ptr<StorageTexture> cachedStorageTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, std::uint32_t mip, std::uint64_t guestBytes = 0);

bool MetadataMoved(const StorageTexture& image, const GuestTextureResource& resource) {
    return resource.dccAddress != 0 && image.Descriptor().dccAddress != resource.dccAddress;
}

bool SampledFromStorageEligible(const Context& context, std::uint32_t format, std::uint64_t address, std::uint64_t guestBytes) {
    static const bool disabled = std::getenv("APS5_NO_SAMPLED_FROM_STORAGE") != nullptr;
    if (disabled || IsBlockCompressed(format) || !StorageFormatAvailable(context, format)) return false;
    return HostImportCovers(context, address, static_cast<std::size_t>(guestBytes));
}

bool SampledFromStorageEligible(const Context& context, const GuestTextureResource& resource, std::uint64_t guestBytes) {
    return SampledFromStorageEligible(context, resource.format, resource.baseAddress, guestBytes);
}

bool ClearedViewEnabled() {
    static const bool disabled = std::getenv("APS5_NO_CLEARED_VIEW") != nullptr;
    return !disabled;
}

struct StorageFailures {
    std::mutex mutex;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> generations;
};

StorageFailures& StorageFailed() {
    static StorageFailures failures;
    return failures;
}

std::shared_ptr<StorageTexture> sampledStorageSource(const Context& context, const GuestTextureResource& resource, std::uint64_t guestBytes) {
    auto& counters = TextureCounts();
    auto& failures = StorageFailed();
    const auto surface = std::make_pair(resource.baseAddress, guestBytes);
    const auto generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    {
        std::lock_guard lock(failures.mutex);
        if (const auto it = failures.generations.find(surface); it != failures.generations.end()) {
            if (it->second == generation) {
                counters.storageFallbacks.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            failures.generations.erase(it);
        }
    }
    try {
        return cachedStorageTexture(context, {}, resource, 0, guestBytes);
    } catch (const std::exception& error) {
        counters.storageFallbacks.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(failures.mutex);
        if (failures.generations.size() < 4096 && failures.generations.emplace(surface, generation).second) aps5::LogErr( "[textures] sampled texture 0x%llx (%ux%u format %u) keeps the snapshot path: %s\n", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, error.what());
        return nullptr;
    }
}

void traceVideoLookup(const char* how, const GuestTextureResource& resource, std::uint64_t bytes, std::uint64_t generation) {
    static const bool enabled = std::getenv("APS5_TRACE_VIDEO_TEX") != nullptr;
    if (!enabled || bytes < 500000 || bytes > 3500000) return;
    struct Seen { unsigned width = 0, height = 0, format = 0, made = 0, unchanged = 0, equal = 0, view = 0; std::uint64_t generation = 0; };
    static std::mutex mutex;
    static std::map<std::uint64_t, Seen> seen;
    static auto last = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex);
    auto& entry = seen[resource.baseAddress];
    entry.width = resource.width; entry.height = resource.height; entry.format = resource.format; entry.generation = generation;
    if (how[0] == 'm') ++entry.made; else if (how[4] == 'v') ++entry.view; else if (std::strstr(how, "unchanged") != nullptr) ++entry.unchanged; else ++entry.equal;
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(1)) return;
    last = now;
    for (const auto& [address, item] : seen) aps5::LogErr("[tex1s] 0x%llx %ux%u format %u made=%u hit-unchanged=%u hit-equal=%u hit-view=%u gen=%llu\n", static_cast<unsigned long long>(address), item.width, item.height, item.format, item.made, item.unchanged, item.equal, item.view, static_cast<unsigned long long>(item.generation));
    aps5::LogErr("[tex1s] --\n");
    seen.clear();
}

std::shared_ptr<Texture> cachedTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components, std::uint64_t guestBytes = 0, bool depthCompare = false) {
    CaptureTrace::Log("sampled-lookup address=%llx width=%u height=%u dcc=%llx", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, static_cast<unsigned long long>(resource.dccAddress));
    if (auto depth = DepthSurfaceTexture(context, words, resource, components)) return depth;
    const auto depthBitsWidth = words.size() >= 4 ? ShaderRecompiler::DepthBitsTextureWidth(words[1], words[3]) : 0u;
    if (depthBitsWidth == 32u) {
        char text[160];
        std::snprintf(text, sizeof(text), "AGC graphics: 32-bit integer read of the depth-layout texture 0x%llx, which is no depth surface drawn with, is not implemented", static_cast<unsigned long long>(resource.baseAddress));
        throw std::runtime_error(text);
    }
    constexpr auto unorm16 = static_cast<std::uint32_t>(ShaderRecompiler::IrBufferFormat::Format16UNorm);
    if (depthBitsWidth == 16u && resource.format != unorm16) {
        auto normalized = resource;
        normalized.format = unorm16;
        return cachedTexture(context, words, normalized, components, guestBytes, depthCompare);
    }
    static const bool disabled = std::getenv("APS5_NO_TEXTURE_CACHE") != nullptr;
    const bool profile = LookupOutcomes::Profiled();
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto scanKeys = [&] {
        const auto scanStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto keys = TextureClearKeys(resource, guestBytes);
        if (profile && resource.dccAddress != 0) LookupOutcomes::Add(LookupOutcomes::DccScan, scanStart);
        return keys;
    };
    if (guestBytes == 0) guestBytes = DescribeSurface(resource).guestBytes;
    auto& counters = TextureCounts();
    const auto address = resource.baseAddress;
    const auto bytes = static_cast<std::size_t>(guestBytes);

    auto source = StorageTexture::FindPending(address, guestBytes);
    if (source != nullptr && (depthCompare || !Texture::CanCopyFrom(*source, resource) || MetadataMoved(*source, resource))) source.reset();
    std::optional<DccKeys> keys;
    bool clearThroughKeys = false;
    if (source != nullptr && resource.dccAddress != 0 && IsDccClear(source->FilledKeys())) {
        keys = scanKeys();
        if (*keys == source->FilledKeys()) {
            std::array<std::byte, 16> probe{};
            if (!FillDccClear(ResolveTextureFormat(resource.format), *keys, resource.dccAlphaOnMsb, probe)) {
                char text[256];
                std::snprintf(text, sizeof(text), "AGC graphics: sampled texture 0x%llx (%ux%u format %u, dcc 0x%llx) reads %s DCC keys filled over its pending image, a clear value the format has no encoding for", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, static_cast<unsigned long long>(resource.dccAddress), DccKeysName(*keys));
                throw std::runtime_error(text);
            }
            static const bool traceKeys = std::getenv("APS5_TRACE_DCC_KEYS") != nullptr;
            if (traceKeys) aps5::LogErr( "[dcc-keys] sampled 0x%llx through keys 0x%llx reads the %s fill, not the pending image\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(resource.dccAddress), DccKeysName(*keys));
            source.reset();
            clearThroughKeys = true;
        }
    }

    if (!depthCompare && source == nullptr && !clearThroughKeys && SampledFromStorageEligible(context, resource, guestBytes)) {

        keys = scanKeys();
        if (*keys == DccKeys::Uncompressed) {
            source = sampledStorageSource(context, resource, guestBytes);
        } else if (ClearedViewEnabled() && StorageClearAvailable(context, resource.format, *keys)) {

            auto candidate = sampledStorageSource(context, resource, guestBytes);
            if (candidate != nullptr && candidate->Descriptor().dccAddress == resource.dccAddress) source = std::move(candidate);
        }

        static const bool rescan = std::getenv("APS5_NO_KEYS_RESCAN") == nullptr;
        if (rescan && source == nullptr) keys.reset();
    }

    auto generation = GuestMemory::CollectWrites(address, bytes);
    if (source != nullptr && !GuestMemory::UnchangedSince(address, bytes, source->Generation())) {

        source->Refresh();
    }

    const auto flushStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (source == nullptr && !clearThroughKeys && StorageTexture::FlushPending(address, bytes, nullptr, "sampled texture")) {
        if (profile) LookupOutcomes::Add(LookupOutcomes::PendingFlush, flushStart);
        generation = GuestMemory::CollectWrites(address, bytes);
        keys.reset();
    }
    if (!keys.has_value()) keys = scanKeys();
    if (disabled) {
        if (source != nullptr) return std::make_shared<Texture>(context, source, resource, components);
        std::vector<std::byte> snapshot(bytes);
        ReadTextureSurface(resource, *keys, snapshot);
        return std::make_shared<Texture>(context, *context.detiler, resource, components, snapshot, depthCompare);
    }
    auto& cache = Textures();
    const auto key = MakeTextureKey(context.device, words, components, depthCompare);
    std::lock_guard lock(cache.mutex);
    if (auto it = findTexture(cache, key); it != cache.entries.end()) {
        if (it->source != nullptr) {

            if ((source == nullptr || source == it->source) && (source != nullptr || *keys == DccKeys::Uncompressed)) {
                if (!GuestMemory::UnchangedSince(address, bytes, it->source->Generation())) it->source->Refresh();
                it->keys = *keys;
                touchTexture(cache, it);
                traceVideoLookup("hit-view", resource, guestBytes, it->source->Generation());
                logLookup({it->texture.get(), resource, guestBytes, *keys, 0, it->source.get()});
                reportTextureCounters();
                if (profile) LookupOutcomes::Add(*keys != DccKeys::Uncompressed ? LookupOutcomes::SampledHitClearedView : LookupOutcomes::SampledHitView, start);
                return it->texture;
            }
        } else if (source == nullptr && it->bytes.size() == guestBytes && it->keys == *keys) {

            const auto equalsCommitted = [&] {
                if (Recorder::SnapshotWriteOverlaps(address, bytes)) counters.pendingReads.fetch_add(1, std::memory_order_relaxed);
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::TextureCompare);
                return GuestMemory::EqualsCommitted(address, it->bytes);
            };
            const bool trackerSaysSame = *keys != DccKeys::Uncompressed || GuestMemory::UnchangedSince(address, it->bytes.size(), it->generation);

            static const bool verifyHits = std::getenv("APS5_VERIFY_TEXTURE_HITS") != nullptr;
            if (verifyHits && trackerSaysSame && *keys == DccKeys::Uncompressed && !equalsCommitted()) {
                static std::atomic<unsigned> reported{0};
                if (reported.fetch_add(1) < 40) aps5::LogErr("[stale] texture 0x%llx %ux%u format %u: the tracker says unchanged since generation %llu but the bytes differ\n", static_cast<unsigned long long>(address), resource.width, resource.height, resource.format, static_cast<unsigned long long>(it->generation));
            }
            if (trackerSaysSame || equalsCommitted()) {
                it->generation = generation;
                touchTexture(cache, it);
                traceVideoLookup(trackerSaysSame ? "hit-snapshot-unchanged" : "hit-snapshot-equal", resource, guestBytes, generation);
                logLookup({it->texture.get(), resource, guestBytes, *keys, generation, nullptr});
                reportTextureCounters();
                if (profile) LookupOutcomes::Add(LookupOutcomes::SampledHitSnapshot, start);
                return it->texture;
            }
        }
        eraseTexture(cache, it);
    }
    CachedTexture entry{key, address, std::vector<std::byte>(source != nullptr ? 0u : bytes), nullptr, *keys, generation};
    entry.accounted = guestBytes;
    if (source != nullptr) {
        entry.source = source;
        entry.sourceVersion = source->Version();
        entry.texture = std::make_shared<Texture>(context, source, resource, components);
        counters.fromStorage.fetch_add(1, std::memory_order_relaxed);
    } else {

        if (*keys == DccKeys::Uncompressed && Recorder::SnapshotWriteOverlaps(address, bytes)) counters.pendingReads.fetch_add(1, std::memory_order_relaxed);
        ReadTextureSurface(resource, *keys, entry.bytes);
        static const bool traceTextures = std::getenv("APS5_TRACE_TEXTURES") != nullptr;
        if (traceTextures) {
            std::size_t nonzero = 0;
            for (std::size_t i = 0; i < entry.bytes.size(); i += 64) nonzero += entry.bytes[i] != std::byte{0};

            static const char* selNames[] = {"ID", "0", "1", "R", "G", "B", "A"};
            auto name = [](VkComponentSwizzle s) { return selNames[std::min<std::size_t>(static_cast<std::size_t>(s), 6)]; };
            aps5::LogErr( "[texture] 0x%llx %ux%u dim=%d d=%u format %u tile %d sel=%s%s%s%s: %zu of %zu sampled bytes nonzero\n", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, static_cast<int>(resource.dimension), resource.depthOrLastArray, resource.format, static_cast<int>(resource.tileMode),
                          name(components.r), name(components.g), name(components.b), name(components.a), nonzero, entry.bytes.size() / 64);
        }
        if (traceTextures && (resource.format == 169 || resource.format == 173)) {

            const std::size_t blockBytes = resource.format == 169 ? 8 : 16, colorOffset = resource.format == 169 ? 0 : 8;
            unsigned maxR = 0, maxG = 0, maxB = 0; double sumR = 0, sumG = 0, sumB = 0; std::size_t n = 0;
            for (std::size_t o = 0; o + blockBytes <= entry.bytes.size(); o += blockBytes) {
                for (int e = 0; e < 2; ++e) {
                    const unsigned v = static_cast<unsigned>(entry.bytes[o + colorOffset + e * 2]) | (static_cast<unsigned>(entry.bytes[o + colorOffset + e * 2 + 1]) << 8);
                    const unsigned r = v >> 11, g = (v >> 5) & 63, bl = v & 31;
                    maxR = std::max(maxR, r); maxG = std::max(maxG, g); maxB = std::max(maxB, bl);
                    sumR += r / 31.0; sumG += g / 63.0; sumB += bl / 31.0; ++n;
                }
            }
            if (n != 0) aps5::LogErr( "[texture-bc] 0x%llx fmt %u endpoints %zu max R%u G%u B%u mean R%.2f G%.2f B%.2f\n", static_cast<unsigned long long>(resource.baseAddress), resource.format, n, maxR, maxG, maxB, sumR / n, sumG / n, sumB / n);
        }
        entry.texture = std::make_shared<Texture>(context, *context.detiler, resource, components, entry.bytes, depthCompare);
        counters.snapshots.fetch_add(1, std::memory_order_relaxed);
    }
    constexpr std::uint64_t budget = 2048ull << 20u;
    while (!cache.entries.empty() && cache.bytes + entry.accounted > budget) eraseTexture(cache, std::prev(cache.entries.end()));
    cache.bytes += entry.accounted;
    auto texture = entry.texture;
    traceVideoLookup(source != nullptr ? "made-view" : "made-snapshot", resource, guestBytes, generation);
    logLookup({texture.get(), resource, guestBytes, *keys, generation, source.get()});
    cache.entries.push_front(std::move(entry));
    cache.index[key] = cache.entries.begin();
    reportTextureCounters();
    if (profile) LookupOutcomes::Add(source != nullptr ? LookupOutcomes::SampledMadeView : LookupOutcomes::SampledMadeSnapshot, start);
    return texture;
}

struct StorageKey {
    VkDevice device;
    std::array<std::uint32_t, 8> words;
    bool operator==(const StorageKey&) const = default;
};

struct StorageKeyHash {
    std::size_t operator()(const StorageKey& key) const noexcept {
        return static_cast<std::size_t>(hashWords(14695981039346656037ull ^ reinterpret_cast<std::uintptr_t>(key.device), key.words.data(), key.words.size()));
    }
};

struct CachedStorageTexture {
    StorageKey key;
    std::uint32_t mip;
    std::shared_ptr<StorageTexture> texture;
};

struct StorageTextureCache {
    std::mutex mutex;
    std::list<CachedStorageTexture> entries;
    std::unordered_map<StorageKey, std::list<CachedStorageTexture>::iterator, StorageKeyHash> index;
    std::unordered_map<const StorageTexture*, std::list<CachedStorageTexture>::iterator> byImage;
    std::uint64_t bytes = 0;
};

StorageTextureCache& StorageTextures() {
    static StorageTextureCache cache;
    return cache;
}

std::list<CachedStorageTexture>::iterator findStorage(StorageTextureCache& cache, const StorageKey& key) {
    if (TextureHashEnabled()) {
        const auto found = cache.index.find(key);
        return found == cache.index.end() ? cache.entries.end() : found->second;
    }
    for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
        if (it->key == key) return it;
    }
    return cache.entries.end();
}

std::list<CachedStorageTexture>::iterator findStorageByImage(StorageTextureCache& cache, VkDevice device, const StorageTexture* image) {
    if (TextureHashEnabled()) {
        const auto found = cache.byImage.find(image);
        return found == cache.byImage.end() || found->second->key.device != device ? cache.entries.end() : found->second;
    }
    for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
        if (it->key.device == device && it->texture.get() == image) return it;
    }
    return cache.entries.end();
}

void evictStorage(StorageTextureCache& cache, std::list<CachedStorageTexture>::iterator it) {
    it->texture->SetCached(false);
    it->texture->Flush();
    cache.bytes -= it->texture->GuestBytes();
    cache.index.erase(it->key);
    cache.byImage.erase(it->texture.get());
    cache.entries.erase(it);
}

std::array<std::uint32_t, 8> SurfaceKey(const Context& context, const GuestTextureResource& resource) {

    return {static_cast<std::uint32_t>(resource.baseAddress), static_cast<std::uint32_t>(resource.baseAddress >> 32u), resource.width, resource.height, (resource.depthOrLastArray << 16u) | (resource.mipCount & 0xffffu), (static_cast<std::uint32_t>(resource.tileMode) << 12u) | (static_cast<std::uint32_t>(resource.dimension) << 20u), resource.baseArray, static_cast<std::uint32_t>(StorageFormatForGuest(context, resource.format))};
}

bool IsNullTextureDescriptor(std::span<const std::uint32_t> words) {
    return words.size() == 8 && words[0] == 0 && (words[1] & 0xffu) == 0;
}

struct NullTextures {
    std::mutex mutex;
    std::map<std::tuple<VkDevice, int, std::uint32_t>, std::shared_ptr<Texture>> textures;
};
NullTextures& NullTextureCache() {
    static NullTextures cache;
    return cache;
}

std::shared_ptr<Texture> nullTexture(const Context& context, ShaderRecompiler::DescriptorImageShape shape, std::span<const std::uint32_t> words) {
    const auto swizzle = words.size() > 3 ? words[3] & 0xfffu : 0u;
    const auto select = [&](std::uint32_t channel) { return ((swizzle >> (3u * channel)) & 7u) == 1u ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_ZERO; };
    const VkComponentMapping mapping{select(0), select(1), select(2), select(3)};
    const auto ones = static_cast<std::uint32_t>(mapping.r == VK_COMPONENT_SWIZZLE_ONE) | static_cast<std::uint32_t>(mapping.g == VK_COMPONENT_SWIZZLE_ONE) << 1u | static_cast<std::uint32_t>(mapping.b == VK_COMPONENT_SWIZZLE_ONE) << 2u | static_cast<std::uint32_t>(mapping.a == VK_COMPONENT_SWIZZLE_ONE) << 3u;
    auto& cache = NullTextureCache();
    std::lock_guard lock(cache.mutex);
    auto& texture = cache.textures[{context.device, static_cast<int>(shape), ones}];
    if (texture == nullptr) {
        GuestTextureResource resource{};
        resource.width = 1;
        resource.height = 1;
        resource.mipCount = 1;
        resource.tileMode = TextureTileMode::kLinear;
        resource.format = 56;
        switch (shape) {
            case ShaderRecompiler::DescriptorImageShape::Image1D: resource.dimension = TextureDimension::k1D; break;
            case ShaderRecompiler::DescriptorImageShape::Image2D: resource.dimension = TextureDimension::k2D; break;
            case ShaderRecompiler::DescriptorImageShape::Image2DArray: resource.dimension = TextureDimension::k2DArray; break;
            case ShaderRecompiler::DescriptorImageShape::ImageCube: resource.dimension = TextureDimension::kCube; resource.depthOrLastArray = 5; break;
            case ShaderRecompiler::DescriptorImageShape::Image3D: resource.dimension = TextureDimension::k3D; break;
        }
        resource.dstSelX = 4;
        resource.dstSelY = 5;
        resource.dstSelZ = 6;
        resource.dstSelW = 7;
        const std::vector<std::byte> zeros(static_cast<std::size_t>(DescribeSurface(resource).guestBytes));
        texture = std::make_shared<Texture>(context, *context.detiler, resource, mapping, zeros);
    }
    return texture;
}

std::shared_ptr<StorageTexture> cachedStorageTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, std::uint32_t mip, std::uint64_t guestBytes) {
    const bool profile = LookupOutcomes::Profiled();
    auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (DepthSurfaceAt(resource.baseAddress)) {
        char text[112];
        std::snprintf(text, sizeof(text), "AGC graphics: storage image access to depth/stencil surface 0x%llx is not implemented", static_cast<unsigned long long>(resource.baseAddress));
        throw std::runtime_error(text);
    }

    if (profile) start = LookupOutcomes::Add(LookupOutcomes::StorageDepthCheck, start);
    static const bool disabled = std::getenv("APS5_NO_TEXTURE_CACHE") != nullptr;
    if (disabled) return std::make_shared<StorageTexture>(context, *context.detiler, resource, mip);
    static_cast<void>(words);
    auto& counters = TextureCounts();
    const StorageKey key{context.device, SurfaceKey(context, resource)};
    auto& cache = StorageTextures();
    if (profile) start = LookupOutcomes::Add(LookupOutcomes::StorageKeyBuild, start);
    std::lock_guard lock(cache.mutex);
    if (profile) start = LookupOutcomes::Add(LookupOutcomes::StorageLockWait, start);
    auto it = findStorage(cache, key);
    if (it != cache.entries.end() && MetadataMoved(*it->texture, resource)) {
        evictStorage(cache, it);
        it = cache.entries.end();
    }
    if (it != cache.entries.end()) {
        it->texture->Refresh();
        cache.entries.splice(cache.entries.begin(), cache.entries, it);
        counters.storageHits.fetch_add(1, std::memory_order_relaxed);
        if (profile) LookupOutcomes::Add(LookupOutcomes::StorageHit, start);
        return it->texture;
    }

    if (guestBytes == 0) guestBytes = DescribeSurface(resource).guestBytes;
    if (StorageTexture::FlushPending(resource.baseAddress, static_cast<std::size_t>(guestBytes), nullptr, "storage image creation", PublishScope::None) && profile) start = LookupOutcomes::Add(LookupOutcomes::PendingFlush, start);
    CachedStorageTexture entry{key, mip, std::make_shared<StorageTexture>(context, *context.detiler, resource, mip)};

    static const bool keepNew = std::getenv("APS5_NO_KEEP_NEW_STORAGE") == nullptr;
    if (auto* recorder = Recorder::Active(); keepNew && recorder != nullptr && GuestMemory::GpuMutex().HeldByThisThread() && recorder->Recording()) recorder->Keep(entry.texture);
    constexpr std::uint64_t budget = 2048ull << 20u;
    while (!cache.entries.empty() && cache.bytes + entry.texture->GuestBytes() > budget) evictStorage(cache, std::prev(cache.entries.end()));
    cache.bytes += entry.texture->GuestBytes();
    auto texture = entry.texture;
    cache.entries.push_front(std::move(entry));
    cache.index[key] = cache.entries.begin();
    cache.byImage[texture.get()] = cache.entries.begin();
    texture->SetCached(true);
    counters.storageCreated.fetch_add(1, std::memory_order_relaxed);
    if (profile) LookupOutcomes::Add(LookupOutcomes::StorageMade, start);
    return texture;
}

}

void FlushCachedTextures(VkDevice device) {
    Require(device != VK_NULL_HANDLE, "cannot flush textures without a Vulkan device");
    GuestMemory::AssertGpuLockHeld("FlushCachedTextures");
    auto& cache = StorageTextures();
    std::lock_guard lock(cache.mutex);
    for (const auto& entry : cache.entries) {
        if (entry.key.device == device) entry.texture->Flush();
    }
}

void ClearCachedTextures(VkDevice device) {
    Require(device != VK_NULL_HANDLE, "cannot clear textures without a Vulkan device");
    auto& sampled = Textures();
    {
        std::lock_guard lock(sampled.mutex);
        for (auto it = sampled.entries.begin(); it != sampled.entries.end();) {
            if (it->key.device == device) eraseTexture(sampled, it++);
            else ++it;
        }
    }
    {
        auto& zeros = NullTextureCache();
        std::lock_guard lock(zeros.mutex);
        for (auto it = zeros.textures.begin(); it != zeros.textures.end();) {
            if (std::get<0>(it->first) == device) it = zeros.textures.erase(it);
            else ++it;
        }
    }
    auto& storage = StorageTextures();
    std::lock_guard lock(storage.mutex);
    for (auto it = storage.entries.begin(); it != storage.entries.end();) {
        if (it->key.device != device) {
            ++it;
            continue;
        }
        it->texture->SetCached(false);
        storage.bytes -= it->texture->GuestBytes();
        storage.index.erase(it->key);
        storage.byImage.erase(it->texture.get());
        it = storage.entries.erase(it);
    }
}

bool StorageImageCached(const Context& context, const StorageTexture* image) {
    auto& cache = StorageTextures();
    std::lock_guard lock(cache.mutex);
    const auto it = findStorageByImage(cache, context.device, image);
    if (it == cache.entries.end()) return false;
    cache.entries.splice(cache.entries.begin(), cache.entries, it);
    return true;
}

namespace {

bool StorageImagesCached(const Context& context, std::span<const StorageTexture* const> images) {
    if (images.empty()) return true;
    auto& cache = StorageTextures();
    std::lock_guard lock(cache.mutex);
    for (const auto* image : images) {
        const auto it = findStorageByImage(cache, context.device, image);
        if (it == cache.entries.end()) return false;
        cache.entries.splice(cache.entries.begin(), cache.entries, it);
    }
    return true;
}

}

std::shared_ptr<StorageTexture> CachedStorageSurface(const Context& context, const GuestTextureResource& resource) {
    return cachedStorageTexture(context, {}, resource, 0);
}

namespace {

const char* roleName(ShaderRecompiler::DescriptorRole role) {
    switch (role) {
        case ShaderRecompiler::DescriptorRole::GuestBuffers: return "GuestBuffers";
        case ShaderRecompiler::DescriptorRole::GuestImages: return "GuestImages";
        case ShaderRecompiler::DescriptorRole::GuestSamplers: return "GuestSamplers";
        case ShaderRecompiler::DescriptorRole::Gds: return "Gds";
        case ShaderRecompiler::DescriptorRole::BdaPagetable: return "BdaPagetable";
        case ShaderRecompiler::DescriptorRole::FaultBuffer: return "FaultBuffer";
        case ShaderRecompiler::DescriptorRole::FlattenedSrt: return "FlattenedSrt";
        case ShaderRecompiler::DescriptorRole::ShaderData: return "ShaderData";
    }
    throw std::runtime_error("AGC graphics: unknown descriptor role");
}

const char* kindName(ShaderRecompiler::DescriptorKind kind) {
    switch (kind) {
        case ShaderRecompiler::DescriptorKind::UniformBuffer: return "UniformBuffer";
        case ShaderRecompiler::DescriptorKind::StorageBuffer: return "StorageBuffer";
        case ShaderRecompiler::DescriptorKind::UniformTexelBuffer: return "UniformTexelBuffer";
        case ShaderRecompiler::DescriptorKind::StorageTexelBuffer: return "StorageTexelBuffer";
        case ShaderRecompiler::DescriptorKind::SampledImage: return "SampledImage";
        case ShaderRecompiler::DescriptorKind::StorageImage: return "StorageImage";
        case ShaderRecompiler::DescriptorKind::Sampler: return "Sampler";
    }
    throw std::runtime_error("AGC graphics: unknown descriptor kind");
}

bool StorageDedupeEnabled() {
    static const bool disabled = std::getenv("APS5_NO_STORAGE_DEDUPE") != nullptr;
    return !disabled;
}

bool SameAsPreviousStorageElement(const ShaderRecompiler::DescriptorBinding& binding, std::uint32_t element) {
    if (element == 0) return false;
    const auto words = binding.guestDescriptor.begin() + static_cast<std::size_t>(element) * 8u;
    return std::equal(words, words + 8, words - 8);
}

}

ShaderResources::ShaderResources(const Context& context, const ShaderRecompiler::RecompileResult& vertex, const ShaderRecompiler::RecompileResult& fragment, const ColorTarget& target, std::uint64_t indexAddress, std::size_t indexBytes) : ShaderResources(context, std::array<CompiledShader, 2>{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &fragment, static_cast<std::uint32_t>(vertex.pushConstants.size())}}}, target, indexAddress, indexBytes) {}

ShaderResources::ShaderResources(const Context& context, std::span<const CompiledShader> shaders, const ColorTarget& target, std::uint64_t indexAddress, std::size_t indexBytes, std::span<const GuestMemorySnapshot> snapshots) : context(context), guestMemory(context) {
    drawBuild = true;
    prepareAddressBindings(shaders, snapshots);
    build(shaders, &target, indexAddress, indexBytes);
}

ShaderResources::ShaderResources(const Context& context, const CompiledShader& compute, std::span<const GuestMemorySnapshot> snapshots) : ShaderResources(context, compute, snapshots, false) {}

namespace {

bool UsesAddressTables(const CompiledShader& compute) {
    Require(compute.program != nullptr, "missing compiled shader");
    return std::any_of(compute.program->bindings.begin(), compute.program->bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable; });
}

}

ShaderResources::ShaderResources(const Context& context, const CompiledShader& compute, std::span<const GuestMemorySnapshot> snapshots, bool deferred) : context(context), guestMemory(context), deferredCompute(compute), deferredSnapshots(snapshots) {
    Require(compute.stage == ShaderRecompiler::ShaderStage::Compute, "compute resources require a compute shader");

    guestMemory.AllowDeviceStaging();
    const std::span<const CompiledShader> shaders(&deferredCompute, 1);
    if (!deferred) {
        prepareAddressBindings(shaders, snapshots);
        build(shaders, nullptr, 0, 0);
        forgetDeferredInputs();
        return;
    }

    lockedBuild = UsesAddressTables(compute);
    if (lockedBuild) return;
    unlockedPrepare = true;
    prepareAddressBindings(shaders, snapshots);
    buildPrepare(shaders, nullptr, 0, 0);
}

void ShaderResources::Complete() {
    Require(!completed, "shader resources were already completed");
    const std::span<const CompiledShader> shaders(&deferredCompute, 1);
    if (lockedBuild) {
        prepareAddressBindings(shaders, deferredSnapshots);
        buildPrepare(shaders, nullptr, 0, 0);
    }
    buildComplete();
    forgetDeferredInputs();
}

void ShaderResources::forgetDeferredInputs() {

    deferredCompute = {};
    deferredSnapshots = {};
}

void ShaderResources::build(std::span<const CompiledShader> shaders, const ColorTarget* target, std::uint64_t indexAddress, std::size_t indexBytes) {
    buildPrepare(shaders, target, indexAddress, indexBytes);
    buildComplete();
}

namespace {

constexpr std::size_t BuildPhaseCount = static_cast<std::size_t>(ShaderResources::BuildPhase::Count);
constexpr std::array<const char*, BuildPhaseCount> BuildPhaseNames{"bindings", "precollect", "guest memory upload", "descriptors", "stage A", "images", "bda", "stage B"};

struct BuildProfile {
    std::mutex mutex;
    std::array<double, BuildPhaseCount> ms{};
    std::uint64_t builds = 0;
};

BuildProfile& Builds() {
    static BuildProfile profile;
    return profile;
}

bool BuildProfiled() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

struct ThreadBuildProfile {
    std::array<double, BuildPhaseCount> ms{};
    std::uint64_t builds = 0;
    ~ThreadBuildProfile() { merge(); }
    void merge() {
        auto& profile = Builds();
        std::lock_guard lock(profile.mutex);
        for (std::size_t i = 0; i < BuildPhaseCount; ++i) profile.ms[i] += ms[i];
        profile.builds += builds;
        ms = {};
        if (builds == 0) return;
        builds = 0;
        std::string report;
        for (std::size_t i = 0; i < BuildPhaseCount; ++i) report += " " + std::string(BuildPhaseNames[i]) + "=" + std::to_string(static_cast<long long>(profile.ms[i])) + "ms";
        aps5::LogErr( "[resources] %llu builds, phase totals:%s\n", static_cast<unsigned long long>(profile.builds), report.c_str());
    }
};

ThreadBuildProfile& ThreadBuilds() {
    thread_local ThreadBuildProfile profile;
    return profile;
}

void addBuildPhase(ShaderResources::BuildPhase which, double ms) {
    ThreadBuilds().ms[static_cast<std::size_t>(which)] += ms;
}

struct BufferWriteCounters {
    std::atomic<std::uint64_t> elements{0};
    std::atomic<std::uint64_t> readOnly{0};
    std::atomic<std::uint64_t> notesSkipped{0};
    std::atomic<std::int64_t> lastReport{0};
};

BufferWriteCounters& BufferWrites() {
    static BufferWriteCounters counters;
    return counters;
}

}

double ShaderResources::phase(BuildPhase which) {
    if (!BuildProfiled()) return 0.0;
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration<double, std::milli>(now - phaseStart).count();
    addBuildPhase(which, ms);
    if (ms > 50) aps5::LogErr( "[resources] %s took %.0f ms (%zu textures, %zu storage images, %zu buffers, bda %d)\n", BuildPhaseNames[static_cast<std::size_t>(which)], ms, textures.size(), storageTextures.size(), allocations.size(), usesBda ? 1 : 0);
    phaseStart = now;
    return ms;
}

void ShaderResources::buildPrepare(std::span<const CompiledShader> shaders, const ColorTarget* target, std::uint64_t indexAddress, std::size_t indexBytes) {
    const auto stageStart = std::chrono::steady_clock::now();
    phaseStart = stageStart;
    if (BuildProfiled()) {
        auto& profile = ThreadBuilds();
        if (++profile.builds % 1000 == 0) profile.merge();
    }
    try {
        Require(!shaders.empty() && context.limits.maxBoundDescriptorSets >= 1, "shader descriptor set exceeds device limits");
        std::set<std::uint32_t> occupied;
        for (const auto& shader : shaders) {
            Require(shader.program != nullptr, "missing compiled shader");
            const VkShaderStageFlags flags = VulkanStage(shader.stage);
            std::uint64_t stageDescriptors = 0;
            std::vector<std::size_t> offsetsInData;
            std::int64_t shaderData = -1;
            for (const auto& binding : shader.program->bindings) {
                Require(binding.descriptorSet == 0, "unexpected descriptor set: every shader resource must use descriptor set zero");
                Require(occupied.insert(binding.binding).second, "duplicate shader binding");
                const bool addressRole = binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable || binding.role == ShaderRecompiler::DescriptorRole::FaultBuffer;
                const bool bufferRole = addressRole || binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers || binding.role == ShaderRecompiler::DescriptorRole::ShaderData || binding.role == ShaderRecompiler::DescriptorRole::FlattenedSrt || binding.role == ShaderRecompiler::DescriptorRole::Gds;
                const bool imageRole = binding.role == ShaderRecompiler::DescriptorRole::GuestImages || binding.role == ShaderRecompiler::DescriptorRole::GuestSamplers;
                if (imageRole) {
                    addImageBinding(binding, flags);
                    continue;
                }
                if (!bufferRole) Require(false, std::string("unsupported descriptor role ") + roleName(binding.role));
                if (binding.kind != ShaderRecompiler::DescriptorKind::StorageBuffer) Require(false, std::string("unsupported descriptor kind ") + kindName(binding.kind) + " for role " + roleName(binding.role) + ": only StorageBuffer is supported");
                Require(!binding.readOnly, "read-only descriptors are unsupported because the recompiler emits no NonWritable decoration");
                Require(binding.count != 0, "empty descriptor binding");
                stageDescriptors += binding.count;
                storageBuffers += binding.count;
                Require(stageDescriptors <= context.limits.maxPerStageDescriptorStorageBuffers && stageDescriptors <= context.limits.maxPerStageResources, "shader descriptors exceed per-stage limits");
                Binding item{{binding.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, binding.count, flags, nullptr}, {}};
                if (binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
                    Require(binding.guestDescriptor.size() == static_cast<std::uint64_t>(binding.count) * 4, "guest buffer descriptor must contain four DWORDs per array element");
                    for (std::uint32_t element = 0; element < binding.count; ++element) {

                        const bool written = element >= binding.bufferWritten.size() || binding.bufferWritten[element];
                        const bool atomic = element < binding.bufferAtomic.size() && binding.bufferAtomic[element];
                        const auto index = addGuestBuffer(std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 4, 4), target, indexAddress, indexBytes, written, atomic);
                        const auto& push = shader.program->pushConstants;
                        if (!push.empty()) {
                            const auto position = shader.program->memoryOffsetDword * 4u + element;
                            Require(position < push.size(), "guest buffer offset lies outside the shader's push constants");
                            allocations[index].pushByte = static_cast<std::int32_t>(shader.pushConstantOffset + position);
                        } else {
                            allocations[index].dataByte = shader.program->memoryOffsetDword * 4u + element;
                            offsetsInData.push_back(index);
                        }
                        item.allocations.push_back(index);
                    }
                } else if (addressRole) {
                    item.allocations.push_back(allocations.size());
                    allocations.push_back({0, 0, false, nullptr, binding.role});
                } else if (binding.role == ShaderRecompiler::DescriptorRole::Gds) {
                    Require(binding.count == 1 && binding.guestDescriptor.empty(), "invalid GDS descriptor contract");
                    const auto gds = Pm4::GdsAddress();
                    guestMemory.AddWritable(gds, Pm4::GdsBytes, true);
                    item.allocations.push_back(allocations.size());
                    allocations.push_back({gds, Pm4::GdsBytes, true, nullptr, ShaderRecompiler::DescriptorRole::ShaderData, true});
                } else {
                    Require(binding.count == 1, "shader data and flattened SRT descriptors must not be arrays");
                    Require(!binding.guestDescriptor.empty(), "empty shader data descriptor");
                    item.allocations.push_back(addDataBuffer(binding.guestDescriptor));
                    if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData) shaderData = static_cast<std::int64_t>(item.allocations.back());
                }
                bindings.push_back(std::move(item));
            }
            for (const auto index : offsetsInData) allocations[index].dataAllocation = shaderData;
        }
        Require(storageBuffers <= context.limits.maxDescriptorSetStorageBuffers, "pipeline descriptors exceed device limits");
        timing.bindingsMs = phase(BuildPhase::Bindings);

        if (precollectImages()) phase(BuildPhase::Precollect);
        if (drawBuild && !usesBda && std::all_of(allocations.begin(), allocations.end(), [&](const Allocation& allocation) { return !allocation.guest || allocation.pushByte >= 0 || (allocation.dataAllocation >= 0 && allocation.dataByte < allocations[static_cast<std::size_t>(allocation.dataAllocation)].size); })) guestMemory.AllowAdjustedRegions();
        guestMemory.UploadPrepare(usesBda);
        timing.uploadMs = phase(BuildPhase::Upload);
        std::vector<VkDescriptorSetLayoutBinding> description;
        for (const auto& binding : bindings) {
            description.push_back(binding.layout);
            layoutKey.insert(layoutKey.end(), {binding.layout.binding, static_cast<std::uint32_t>(binding.layout.descriptorType), binding.layout.descriptorCount, binding.layout.stageFlags});
        }
        static const bool noLayoutCache = std::getenv("APS5_NO_LAYOUT_CACHE") != nullptr;
        static const bool noPoolCache = std::getenv("APS5_NO_POOL_CACHE") != nullptr;
        if (context.descriptorCache != nullptr && !noLayoutCache) {
            _layout = context.descriptorCache->Layout(layoutKey, description);
        } else {
            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            info.bindingCount = static_cast<std::uint32_t>(description.size());
            info.pBindings = description.data();
            Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &info, nullptr, &_layout), "vkCreateDescriptorSetLayout");
            ownsLayout = true;
        }
        if (!bindings.empty()) {

            std::vector<VkDescriptorPoolSize> sizes;
            if (storageBuffers != 0) sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<std::uint32_t>(storageBuffers)});
            if (plannedSampledImages != 0) sizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, plannedSampledImages});
            if (plannedStorageImages != 0) sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, plannedStorageImages});
            if (!samplers.empty()) sizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLER, static_cast<std::uint32_t>(samplers.size())});
            if (context.descriptorCache != nullptr && !noPoolCache) {
                const auto allocated = context.descriptorCache->Allocate(_layout, sizes);
                _set = allocated.set;
                cachePool = allocated.pool;
            }
            if (_set == VK_NULL_HANDLE) {
                VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                poolInfo.maxSets = 1;
                poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
                poolInfo.pPoolSizes = sizes.data();
                Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
                VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                allocation.descriptorPool = pool;
                allocation.descriptorSetCount = 1;
                allocation.pSetLayouts = &_layout;
                Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocation, &_set), "vkAllocateDescriptorSets");
            }
        }
        timing.descriptorsMs = phase(BuildPhase::Descriptors);
        if (BuildProfiled()) {
            timing.prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stageStart).count();
            addBuildPhase(BuildPhase::StageA, timing.prepareMs);
        }
    } catch (...) {
        release();
        throw;
    }
}

void ShaderResources::buildComplete() {
    const auto stageStart = std::chrono::steady_clock::now();
    phaseStart = stageStart;
    try {

        for (const auto& deferred : deferredImages) resolveImageBinding(*deferred.binding, bindings[deferred.index]);
        deferredImages.clear();

        imageRecords.clear();
        imageRecords.shrink_to_fit();
        nextImageRecord = 0;
        Require(textures.size() == plannedSampledImages && storageTextures.size() == plannedStorageImages, "image lookups disagree with the descriptor plan");
        timing.bindingsMs += phase(BuildPhase::Images);
        guestMemory.UploadFinish(usesBda);
        timing.uploadMs += phase(BuildPhase::Upload);
        if (usesBda) bda = std::make_unique<BdaResources>(context, guestMemory);
        else if (usesFaultBuffer) bda = std::make_unique<BdaResources>(context);
        phase(BuildPhase::Bda);
        if (_set != VK_NULL_HANDLE) {

            std::size_t bufferCount = 0;
            std::size_t imageCount = 0;
            for (const auto& binding : bindings) {
                bufferCount += binding.allocations.size();
                imageCount += binding.imageAllocations.size();
            }
            std::vector<VkDescriptorBufferInfo> buffers;
            std::vector<VkDescriptorImageInfo> images;
            buffers.reserve(bufferCount);
            images.reserve(imageCount);
            std::vector<VkWriteDescriptorSet> writes;
            writes.reserve(bindings.size());
            for (const auto& binding : bindings) {
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = _set;
                write.dstBinding = binding.layout.binding;
                write.descriptorCount = binding.layout.descriptorCount;
                write.descriptorType = binding.layout.descriptorType;
                switch (binding.layout.descriptorType) {
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                        write.pBufferInfo = buffers.data() + buffers.size();
                        for (const auto index : binding.allocations) buffers.push_back(descriptor(allocations[index]));
                        break;
                    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                        write.pImageInfo = images.data() + images.size();
                        for (const auto index : binding.imageAllocations) images.push_back({VK_NULL_HANDLE, textureFirstLayer[index] ? textures[index]->FirstLayerView() : textures[index]->View(), textures[index]->Layout()});
                        break;
                    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                        write.pImageInfo = images.data() + images.size();
                        for (const auto index : binding.imageAllocations) images.push_back({VK_NULL_HANDLE, storageFirstLayer[index] ? storageTextures[index]->FirstLayerView(storageMips[index]) : storageTextures[index]->View(storageMips[index]), VK_IMAGE_LAYOUT_GENERAL});
                        break;
                    case VK_DESCRIPTOR_TYPE_SAMPLER:
                        write.pImageInfo = images.data() + images.size();
                        for (const auto index : binding.imageAllocations) images.push_back({samplers[index]->Handle(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                        break;
                    default: throw std::runtime_error("AGC graphics: ShaderResources encountered an unknown descriptor type while writing the descriptor set");
                }
                writes.push_back(write);
            }
            context.Resolved(&DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        for (const auto& allocation : allocations) {
            if (allocation.adjustment == 0) continue;
            if (allocation.pushByte >= 0) {
                pushPatches.emplace_back(static_cast<std::uint32_t>(allocation.pushByte), allocation.adjustment);
                continue;
            }
            Require(allocation.dataAllocation >= 0, "a guest buffer off the storage buffer offset alignment in a shader without shader data is not implemented");
            auto& data = allocations[static_cast<std::size_t>(allocation.dataAllocation)];
            Require(data.buffer != nullptr && allocation.dataByte < data.size, "guest buffer offset lies outside the shader's data buffer");
            data.buffer->Bytes()[allocation.dataByte] = static_cast<std::byte>(allocation.adjustment);
            dataPatches.push_back({static_cast<std::size_t>(allocation.dataAllocation), allocation.dataByte, allocation.adjustment});
        }
        timing.descriptorsMs += phase(BuildPhase::Descriptors);
        noteReusable();
        completed = true;
        if (BuildProfiled()) {
            timing.completeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stageStart).count();
            addBuildPhase(BuildPhase::StageB, timing.completeMs);
            reportDescriptorCaches();
        }
    } catch (...) {
        release();
        throw;
    }
}

namespace {

bool TemplateDataRefresh() {
    static const bool enabled = std::getenv("APS5_NO_TEMPLATE_DATA_REFRESH") == nullptr;
    return enabled;
}

bool DataRole(ShaderRecompiler::DescriptorRole role) {
    return role == ShaderRecompiler::DescriptorRole::ShaderData || role == ShaderRecompiler::DescriptorRole::FlattenedSrt;
}

constexpr std::size_t MaxRefreshBytes = 65536;

constexpr std::uint64_t FnvOffset = 14695981039346656037ull;
constexpr std::uint64_t FnvPrime = 1099511628211ull;
void mixDataWords(std::uint64_t& hash, std::span<const std::uint32_t> words) {
    hash = (hash ^ static_cast<std::uint64_t>(words.size())) * FnvPrime;
    for (const auto word : words) hash = (hash ^ word) * FnvPrime;
}

std::atomic<std::uint64_t> resourceCacheFinds{0};
std::atomic<std::uint64_t> resourceCacheTouches{0};

}

void ShaderResources::noteReusable() {

    captureValidation();
    reusable = false;
    directRegions.clear();
    if (NeedsCompletion() || HoldsLease()) return;
    if (TemplateDataRefresh() && std::any_of(allocations.begin(), allocations.end(), [](const Allocation& allocation) { return allocation.buffer != nullptr && !allocation.guest && allocation.size > MaxRefreshBytes; })) return;
    const auto regions = guestMemory.DirectRegions();
    if (!regions.has_value()) return;
    for (const auto& [begin, end] : *regions) {

        auto serial = HostImportSerial(context, begin, static_cast<std::size_t>(end - begin), false);

        if (serial == 0) serial = ImageMirrorSerial(context, begin, static_cast<std::size_t>(end - begin));
        if (serial == 0) return;
        directRegions.push_back({begin, end, serial});
    }
    reusable = true;
}

void ShaderResources::reportDescriptorCaches() const {
    static std::mutex reportMutex;
    static auto lastReport = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(reportMutex);
    if (now - lastReport < std::chrono::seconds(10)) return;
    lastReport = now;
    const auto descriptors = context.descriptorCache != nullptr ? context.descriptorCache->Counters() : DescriptorCache::Stats{};
    const auto samplerHits = context.samplerCache != nullptr ? context.samplerCache->Hits() : 0;
    const auto samplerMisses = context.samplerCache != nullptr ? context.samplerCache->Misses() : 0;
    aps5::LogErr( "[descriptors] layouts %llu hits / %llu created, sets %llu from %llu pools, samplers %llu hits / %llu created\n", static_cast<unsigned long long>(descriptors.layoutHits), static_cast<unsigned long long>(descriptors.layoutMisses), static_cast<unsigned long long>(descriptors.sets), static_cast<unsigned long long>(descriptors.pools), static_cast<unsigned long long>(samplerHits), static_cast<unsigned long long>(samplerMisses));
}

std::vector<std::uint32_t> ShaderResources::ContentKey(const CompiledShader& shader, bool dataWords) {
    Require(shader.program != nullptr, "missing compiled shader");
    const auto& program = *shader.program;
    std::vector<std::uint32_t> key;
    key.reserve(8 + program.bindings.size() * 12);
    key.push_back(dataWords ? 1u : 0u);
    key.push_back(static_cast<std::uint32_t>(shader.stage));
    key.push_back(static_cast<std::uint32_t>(program.variantId));
    key.push_back(static_cast<std::uint32_t>(program.variantId >> 32u));
    key.push_back(static_cast<std::uint32_t>(program.bindings.size()));
    const auto packBits = [&](const std::vector<bool>& bits) {
        key.push_back(static_cast<std::uint32_t>(bits.size()));
        std::uint32_t word = 0;
        for (std::size_t i = 0; i < bits.size(); ++i) {
            if (bits[i]) word |= 1u << (i % 32u);
            if (i % 32u == 31u || i + 1 == bits.size()) {
                key.push_back(word);
                word = 0;
            }
        }
    };
    for (const auto& binding : program.bindings) {
        key.insert(key.end(), {static_cast<std::uint32_t>(binding.kind), static_cast<std::uint32_t>(binding.role), binding.descriptorSet, binding.binding, binding.count, binding.readOnly ? 1u : 0u, binding.imageShape.has_value() ? static_cast<std::uint32_t>(*binding.imageShape) + 1u : 0u, static_cast<std::uint32_t>(binding.guestDescriptor.size())});
        if (dataWords || !DataRole(binding.role)) key.insert(key.end(), binding.guestDescriptor.begin(), binding.guestDescriptor.end());
        packBits(binding.imageWritten);
        packBits(binding.samplerDepthCompare);
        packBits(binding.imageDepthCompare);

        packBits(binding.bufferWritten);
    }
    return key;
}

namespace {

using FastFail = ShaderResources::FastFail;
constexpr std::array<const char*, static_cast<std::size_t>(FastFail::Count)> FastFailNames{"no record", "collect", "pending image", "evicted image", "memory changed", "keys", "cleared view", "storage keys"};
using OwnRefreshFallback = ShaderResources::OwnRefreshFallback;
constexpr std::array<const char*, static_cast<std::size_t>(OwnRefreshFallback::Count)> OwnRefreshFallbackNames{"disabled", "snapshot texture", "cleared view", "foreign view", "surface key", "not imported", "uncached", "re-run failed"};

struct RevalidateProfile {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> nanoseconds{0};
    std::atomic<std::uint64_t> fast{0};
    std::atomic<std::uint64_t> full{0};
    std::atomic<std::uint64_t> failed{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(FastFail::Count)> fastFails{};

    std::atomic<std::uint64_t> serialSkips{0};
    std::atomic<std::uint64_t> importSkips{0};

    std::atomic<std::uint64_t> keysProven{0};
    std::atomic<std::uint64_t> storageKeysProven{0};

    std::atomic<std::uint64_t> pendingForeign{0};
    std::atomic<std::uint64_t> pendingSnapshot{0};
    std::atomic<std::uint64_t> pendingT1Eligible{0};

    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(FastFail::Count)> fullByReason{};
    std::atomic<std::uint64_t> ownRefreshed{0};
    std::atomic<std::uint64_t> ownStorageRefreshes{0};
    std::atomic<std::uint64_t> ownViewRefreshes{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(OwnRefreshFallback::Count)> ownFallbacks{};
    std::atomic<std::uint64_t> ownSourceViews{0};
    std::atomic<std::uint64_t> refreshedOverlaps{0};
    std::atomic<std::uint64_t> proofsVerified{0};
    std::atomic<std::int64_t> lastReport{0};
};

RevalidateProfile& Revalidations() {
    static RevalidateProfile profile;
    return profile;
}

void countFastFail(FastFail reason) {
    if (BuildProfiled()) Revalidations().fastFails[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void countPendingFail(std::span<const StorageTexture::PendingQuery> pending) {
    if (!BuildProfiled()) return;
    auto& profile = Revalidations();
    bool eligible = true;
    for (const auto& query : pending) {
        if (!query.overlaps) continue;
        if (query.except != nullptr) {
            profile.pendingForeign.fetch_add(1, std::memory_order_relaxed);
        } else {
            profile.pendingSnapshot.fetch_add(1, std::memory_order_relaxed);
            eligible = false;
        }
    }
    if (eligible) profile.pendingT1Eligible.fetch_add(1, std::memory_order_relaxed);
}

void countKeysProven(bool storage) {
    if (BuildProfiled()) (storage ? Revalidations().storageKeysProven : Revalidations().keysProven).fetch_add(1, std::memory_order_relaxed);
}

void countFullWalk(FastFail reason) {
    if (BuildProfiled() && reason != FastFail::Count) Revalidations().fullByReason[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void countOwnRefreshFallback(OwnRefreshFallback reason) {
    if (BuildProfiled()) Revalidations().ownFallbacks[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void countOwnRefresh(bool storage) {
    TextureCounts().ownRefreshes.fetch_add(1, std::memory_order_relaxed);
    if (BuildProfiled()) (storage ? Revalidations().ownStorageRefreshes : Revalidations().ownViewRefreshes).fetch_add(1, std::memory_order_relaxed);
}

bool OwnImageRefreshEnabled() {
    static const bool disabled = std::getenv("APS5_NO_OWN_IMAGE_REFRESH") != nullptr;
    return !disabled;
}

bool VerifyProofs() {
    static const bool enabled = std::getenv("APS5_VERIFY_PROOFS") != nullptr;
    return enabled;
}

bool SameKeySurface(const StorageTexture* source, const GuestTextureResource& resource, std::uint64_t guestBytes) {
    if (source == nullptr) return false;
    const auto& own = source->Descriptor();
    return own.dccAddress == resource.dccAddress && source->GuestBytes() == guestBytes && own.format == resource.format && own.dccAlphaOnMsb == resource.dccAlphaOnMsb;
}

void countRevalidate(bool fast, bool ok, std::chrono::steady_clock::time_point start) {
    auto& profile = Revalidations();
    const auto now = std::chrono::steady_clock::now();
    profile.calls.fetch_add(1, std::memory_order_relaxed);
    profile.nanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count()), std::memory_order_relaxed);
    (!ok ? profile.failed : fast ? profile.fast : profile.full).fetch_add(1, std::memory_order_relaxed);
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    auto last = profile.lastReport.load();
    if (nowMs - last < 10000 || !profile.lastReport.compare_exchange_strong(last, nowMs)) return;
    const auto byReason = [](const auto& counts, const auto& names) {
        std::string reasons;
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto count = counts[i].load(std::memory_order_relaxed);
            if (count == 0) continue;
            reasons += " " + std::string(names[i]) + " " + std::to_string(count);
        }
        return reasons;
    };
    const auto reasons = byReason(profile.fastFails, FastFailNames);
    const auto fullReasons = byReason(profile.fullByReason, FastFailNames);
    const auto fallbacks = byReason(profile.ownFallbacks, OwnRefreshFallbackNames);
    aps5::LogErr( "[rescache] revalidate %llu calls %.1f ms: fast %llu, full %llu, failed %llu; fast-fail by reason:%s; full walks: T1 refreshed %llu (%llu storage images, %llu view sources, %llu views served by their own pending source, %llu overlaps left by the refresh accepted), full by reason:%s, T1 fallback by reason:%s, proofs verified %llu; fast-fail pending image: own 0 (the query excepts the own object), foreign %llu, snapshot %llu, T1 eligible %llu; keys proven: %llu sampled, %llu storage; epoch gate: %llu registry scans skipped, %llu import loops skipped\n", static_cast<unsigned long long>(profile.calls.load()), profile.nanoseconds.load() / 1e6, static_cast<unsigned long long>(profile.fast.load()), static_cast<unsigned long long>(profile.full.load()), static_cast<unsigned long long>(profile.failed.load()), reasons.c_str(), static_cast<unsigned long long>(profile.ownRefreshed.load()), static_cast<unsigned long long>(profile.ownStorageRefreshes.load()), static_cast<unsigned long long>(profile.ownViewRefreshes.load()), static_cast<unsigned long long>(profile.ownSourceViews.load()), static_cast<unsigned long long>(profile.refreshedOverlaps.load()), fullReasons.c_str(), fallbacks.c_str(), static_cast<unsigned long long>(profile.proofsVerified.load()), static_cast<unsigned long long>(profile.pendingForeign.load()), static_cast<unsigned long long>(profile.pendingSnapshot.load()), static_cast<unsigned long long>(profile.pendingT1Eligible.load()), static_cast<unsigned long long>(profile.keysProven.load()), static_cast<unsigned long long>(profile.storageKeysProven.load()), static_cast<unsigned long long>(profile.serialSkips.load()), static_cast<unsigned long long>(profile.importSkips.load()));
}

bool EpochRevalidate() {
    static const bool enabled = std::getenv("APS5_NO_EPOCH_REVALIDATE") == nullptr;
    return enabled;
}

}

void ShaderResources::captureValidation() {
    const auto record = [](const void* object) -> const LookupRecord* {
        for (auto it = lookupLog.rbegin(); it != lookupLog.rend(); ++it) {
            if (it->object == object) return &*it;
        }
        return nullptr;
    };
    validatedTextures.assign(textures.size(), {});
    for (std::size_t i = 0; i < textures.size(); ++i) {
        if (const auto* found = record(textures[i].get())) validatedTextures[i] = {found->resource, found->bytes, found->keys, found->generation, 0, found->source, true};
    }
    lookupLog.clear();
}

bool ShaderResources::fastRevalidate(std::uint64_t serialBefore, std::span<const PendingOverlap> refreshed, FastFail& reason, std::vector<PendingOverlap>& overlapping, bool& accepted) {
    reason = FastFail::Count;
    overlapping.clear();
    accepted = false;
    if (!EpochRevalidate()) return fastRevalidateEach();
    const auto fail = [&reason](FastFail why) {
        reason = why;
        countFastFail(why);
        return false;
    };
    if (validatedTextures.size() != textures.size()) return fail(FastFail::NoRecord);
    const bool unchanged = pendingSerialSeen != 0 && pendingSerialSeen == serialBefore;
    const bool keyProofs = KeyFastPath();
    thread_local std::vector<GuestMemory::UnchangedQuery> queries;
    thread_local std::vector<StorageTexture::PendingQuery> pending;

    thread_local std::vector<PendingOverlap> owners;
    thread_local std::vector<const StorageTexture*> images;
    thread_local std::vector<DccKeys> scannedKeys;
    queries.clear();
    pending.clear();
    owners.clear();
    images.clear();
    scannedKeys.assign(textures.size(), DccKeys::Uncompressed);
    const auto query = [&](std::uint64_t begin, std::uint64_t end, const StorageTexture* except, const StorageTexture* identity, PendingOverlap owner) {
        pending.push_back({begin, end, except, identity, false});
        owners.push_back(owner);
    };
    for (std::size_t i = 0; i < textures.size(); ++i) {
        auto& surface = validatedTextures[i];
        if (!surface.valid) return fail(FastFail::NoRecord);
        const auto address = surface.resource.baseAddress;
        const auto bytes = static_cast<std::size_t>(surface.bytes);
        surface.collected = GuestMemory::CollectWrites(address, bytes);
        if (surface.collected == 0) return fail(FastFail::Collect);
        const auto* source = surface.source;
        if (!keyProofs) {

            if (surface.resource.dccAddress != 0 && TextureClearKeys(surface.resource, surface.bytes) != surface.keys) return fail(FastFail::Keys);
            scannedKeys[i] = surface.keys;
            if (!unchanged) query(address, address + bytes, source, source != nullptr && surface.keys != DccKeys::Uncompressed ? source : nullptr, {i, false, source != nullptr && surface.keys == DccKeys::Uncompressed});
        } else if (source != nullptr) {

            const auto& own = source->Descriptor();
            const auto sourceKeys = own.dccAddress != 0 ? ProvedClearKeys(own, source->GuestBytes(), source->KeyProof()) : DccKeys::Uncompressed;
            if (sourceKeys != source->UploadedKeys()) return fail(FastFail::Keys);
            const auto keys = surface.resource.dccAddress == 0 ? DccKeys::Uncompressed : SameKeySurface(source, surface.resource, surface.bytes) ? sourceKeys : ProvedClearKeys(surface.resource, surface.bytes, textures[i]->KeyProof());
            scannedKeys[i] = keys;

            if (keys != DccKeys::Uncompressed && !(own.dccAddress == surface.resource.dccAddress && ClearedViewEnabled() && StorageClearAvailable(context, surface.resource.format, keys))) query(address, address + bytes, source, source, {i, false, false});
            else if (!unchanged) query(address, address + bytes, source, nullptr, {i, false, keys == DccKeys::Uncompressed});
            if (surface.resource.dccAddress != 0 || own.dccAddress != 0) countKeysProven(false);
        } else {

            const auto keys = surface.resource.dccAddress != 0 ? ProvedClearKeys(surface.resource, surface.bytes, textures[i]->KeyProof()) : DccKeys::Uncompressed;
            if (keys != surface.keys) return fail(FastFail::Keys);
            scannedKeys[i] = keys;
            if (!unchanged) query(address, address + bytes, nullptr, nullptr, {i, false, false});
            if (surface.resource.dccAddress != 0) countKeysProven(false);
        }
        if (source != nullptr) {
            if (!source->Cached()) return fail(FastFail::Evicted);
            queries.push_back({address, bytes, source->Generation()});
            images.push_back(source);
        } else {
            queries.push_back({address, bytes, surface.generation});
        }
    }
    for (std::size_t i = 0; i < storageTextures.size(); ++i) {
        if (i != 0 && storageTextures[i] == storageTextures[i - 1]) continue;
        const auto* image = storageTextures[i].get();
        if (image == nullptr) return fail(FastFail::NoRecord);
        const auto& own = image->Descriptor();
        const auto address = own.baseAddress;
        const auto bytes = static_cast<std::size_t>(image->GuestBytes());
        if (GuestMemory::CollectWrites(address, bytes) == 0) return fail(FastFail::Collect);
        if (own.dccAddress != 0) {

            if (!keyProofs || ProvedClearKeys(own, bytes, image->KeyProof()) != image->UploadedKeys()) return fail(FastFail::StorageKeys);
            countKeysProven(true);
        }
        if (!unchanged) query(address, address + bytes, image, nullptr, {i, true, false});
        if (!image->Cached()) return fail(FastFail::Evicted);
        queries.push_back({address, bytes, image->Generation()});
        images.push_back(image);
    }
    if (!pending.empty()) {
        if (!StorageTexture::ScanPending(pending)) return fail(FastFail::ClearedView);

        const auto refreshedOwner = [&](const PendingOverlap& owner) {
            return std::find_if(refreshed.begin(), refreshed.end(), [&](const PendingOverlap& entry) { return entry.element == owner.element && entry.storage == owner.storage; });
        };
        for (std::size_t k = 0; k < pending.size(); ++k) {
            if (!pending[k].overlaps) continue;
            if (OwnImageRefreshEnabled() && owners[k].viewUncompressed && pending[k].found == pending[k].except) {
                if (BuildProfiled()) Revalidations().ownSourceViews.fetch_add(1, std::memory_order_relaxed);
                accepted = true;
                continue;
            }
            if (const auto entry = refreshedOwner(owners[k]); entry != refreshed.end() && (entry->storage || pending[k].found == pending[k].except || (pending[k].found == nullptr && entry->sourceEligible))) {
                if (BuildProfiled()) Revalidations().refreshedOverlaps.fetch_add(1, std::memory_order_relaxed);
                accepted = true;
                continue;
            }
            overlapping.push_back(owners[k]);
        }
        if (!overlapping.empty()) {
            countPendingFail(pending);
            return fail(FastFail::Pending);
        }
    } else if (unchanged && BuildProfiled()) {
        Revalidations().serialSkips.fetch_add(1, std::memory_order_relaxed);
    }
    if (!GuestMemory::UnchangedSinceAll(queries)) return fail(FastFail::Changed);
    if (!StorageImagesCached(context, images)) return fail(FastFail::Evicted);
    for (const auto* image : images) image->NoteProved();
    for (std::size_t i = 0; i < validatedTextures.size(); ++i) {
        auto& surface = validatedTextures[i];
        if (surface.source == nullptr) surface.generation = surface.collected;
        surface.keys = scannedKeys[i];
    }
    return true;
}

bool ShaderResources::fastRevalidateEach() {
    if (validatedTextures.size() != textures.size()) return false;
    for (std::size_t i = 0; i < textures.size(); ++i) {
        auto& surface = validatedTextures[i];
        if (!surface.valid) return false;
        const auto address = surface.resource.baseAddress;
        const auto bytes = static_cast<std::size_t>(surface.bytes);
        surface.collected = GuestMemory::CollectWrites(address, bytes);
        if (surface.collected == 0) return false;
        if (PendingStorageOverlaps(address, bytes, surface.source)) return false;
        if (surface.source != nullptr) {

            if (!StorageImageCached(context, surface.source) || !GuestMemory::UnchangedSince(address, bytes, surface.source->Generation())) return false;
        } else if (!GuestMemory::UnchangedSince(address, bytes, surface.generation)) {
            return false;
        }
        if (surface.resource.dccAddress != 0 && TextureClearKeys(surface.resource, surface.bytes) != surface.keys) return false;

        if (surface.source != nullptr && surface.keys != DccKeys::Uncompressed && StorageTexture::FindPending(address, surface.bytes).get() != surface.source) return false;
    }
    for (std::size_t i = 0; i < storageTextures.size(); ++i) {

        if (i != 0 && storageTextures[i] == storageTextures[i - 1]) continue;
        const auto* image = storageTextures[i].get();
        if (image == nullptr) return false;

        const auto& own = image->Descriptor();
        if (own.dccAddress != 0) return false;
        const auto address = own.baseAddress;
        const auto bytes = static_cast<std::size_t>(image->GuestBytes());
        if (GuestMemory::CollectWrites(address, bytes) == 0) return false;
        if (PendingStorageOverlaps(address, bytes, image)) return false;
        if (!StorageImageCached(context, image)) return false;
        if (!GuestMemory::UnchangedSince(address, bytes, image->Generation())) return false;
    }

    for (auto& surface : validatedTextures) {
        if (surface.source == nullptr) surface.generation = surface.collected;
    }
    return true;
}

ShaderResources::OwnRefreshFallback ShaderResources::refreshOwnObjects(std::span<const CompiledShader> shaders, std::span<PendingOverlap> overlapping) {
    const auto listed = [&](std::size_t element, bool storage) {
        return std::find_if(overlapping.begin(), overlapping.end(), [&](const PendingOverlap& overlap) { return overlap.element == element && overlap.storage == storage; });
    };

    const auto refreshView = [&](std::size_t i, PendingOverlap& overlap) {
        const auto& surface = validatedTextures[i];
        const auto& source = textures[i]->SharedStorageSource();
        if (source == nullptr || source.get() != surface.source) return OwnRefreshFallback::Snapshot;
        if (!source->Cached()) return OwnRefreshFallback::Uncached;
        const auto address = surface.resource.baseAddress;
        const auto bytes = static_cast<std::size_t>(surface.bytes);
        const bool imported = SampledFromStorageEligible(context, surface.resource, surface.bytes);
        overlap.sourceEligible = imported && SurfaceKey(context, source->Descriptor()) == SurfaceKey(context, surface.resource);
        auto found = StorageTexture::FindPending(address, surface.bytes);
        if (found != nullptr && !Texture::CanCopyFrom(*found, surface.resource)) found.reset();
        if (found != nullptr) {
            if (found != source) return OwnRefreshFallback::ForeignView;
            if (!GuestMemory::UnchangedSince(address, bytes, source->Generation())) {
                countOwnRefresh(false);
                source->Refresh();
            }
            return OwnRefreshFallback::Count;
        }
        if (!imported) return OwnRefreshFallback::NotImported;
        if (!overlap.sourceEligible) return OwnRefreshFallback::SurfaceKey;
        countOwnRefresh(false);

        source->Refresh();
        return OwnRefreshFallback::Count;
    };

    const auto refreshStorage = [&](std::size_t i) {
        auto& image = storageTextures[i];
        if (image == nullptr || !image->Cached()) return OwnRefreshFallback::Uncached;
        countOwnRefresh(true);
        image->Refresh();
        return OwnRefreshFallback::Count;
    };
    std::size_t textureIndex = 0;
    std::size_t storageIndex = 0;
    for (const auto& shader : shaders) {
        if (shader.program == nullptr) return OwnRefreshFallback::Uncached;
        for (const auto& binding : shader.program->bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages) continue;
            if (binding.kind == ShaderRecompiler::DescriptorKind::SampledImage) {
                for (std::uint32_t element = 0; element < binding.count; ++element, ++textureIndex) {
                    if (textureIndex >= textures.size()) return OwnRefreshFallback::Uncached;
                    const auto overlap = listed(textureIndex, false);
                    if (overlap == overlapping.end()) continue;
                    if (const auto fallback = refreshView(textureIndex, *overlap); fallback != OwnRefreshFallback::Count) return fallback;
                }
            } else if (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) {
                for (std::uint32_t element = 0; element < binding.count; ++element, ++storageIndex) {
                    if (storageIndex >= storageTextures.size()) return OwnRefreshFallback::Uncached;
                    if (listed(storageIndex, true) == overlapping.end()) continue;
                    if (const auto fallback = refreshStorage(storageIndex); fallback != OwnRefreshFallback::Count) return fallback;
                }
            }
        }
    }
    return OwnRefreshFallback::Count;
}

bool ShaderResources::Revalidate(std::span<const CompiledShader> shaders, ProofReport* report) {
    if (report != nullptr) *report = {ProofPath::Full, ProofFailure::Other};
    if (!reusable || shaders.empty()) return false;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;

    static const bool noFast = std::getenv("APS5_NO_FAST_REVALIDATE") != nullptr;
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto finish = [&](bool fast, bool ok, ProofFailure failure = ProofFailure::Other) {
        if (profile) countRevalidate(fast, ok, start);
        if (report != nullptr) report->failure = ok ? ProofFailure::None : failure;
        return ok;
    };

    const auto serialBefore = StorageTexture::PendingSerial();

    const auto fullWalk = [&] {
        lookupLog.clear();
        std::size_t textureIndex = 0;
        std::size_t storageIndex = 0;
        for (const auto& shader : shaders) {
            if (shader.program == nullptr) return false;
            for (const auto& binding : shader.program->bindings) {
                if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages) continue;
                if (binding.kind == ShaderRecompiler::DescriptorKind::SampledImage) {
                    const auto elementWords = binding.count != 0 ? binding.guestDescriptor.size() / binding.count : 0;
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
                        if (IsNullTextureDescriptor(words) && binding.imageShape.has_value()) {
                            if (textureIndex >= textures.size() || nullTexture(context, *binding.imageShape, words) != textures[textureIndex]) return false;
                            ++textureIndex;
                            continue;
                        }
                        const auto resource = DecodeTextureResource(words);
                        const VkComponentMapping components{ComponentSwizzleFor(resource.dstSelX), ComponentSwizzleFor(resource.dstSelY), ComponentSwizzleFor(resource.dstSelZ), ComponentSwizzleFor(resource.dstSelW)};
                        if (textureIndex >= textures.size() || cachedTexture(context, words, resource, components, 0, !binding.imageDepthCompare.empty() && binding.imageDepthCompare.at(element)) != textures[textureIndex]) return false;
                        ++textureIndex;
                    }
                } else if (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) {
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u);
                        if (storageIndex >= storageTextures.size()) return false;
                        std::shared_ptr<StorageTexture> expected;
                        if (SameAsPreviousStorageElement(binding, element) && StorageDedupeEnabled()) expected = storageTextures[storageIndex - 1];
                        else expected = cachedStorageTexture(context, words, DecodeTextureResource(words), storageMips[storageIndex]);
                        if (expected != storageTextures[storageIndex]) return false;
                        ++storageIndex;
                    }
                }
            }
        }
        if (textureIndex != textures.size() || storageIndex != storageTextures.size()) return false;

        captureValidation();
        return true;
    };
    thread_local std::vector<PendingOverlap> overlapping;
    thread_local std::vector<PendingOverlap> refreshed;
    refreshed.clear();
    FastFail reason = FastFail::Count;
    bool accepted = false;
    bool fast = !noFast && fastRevalidate(serialBefore, refreshed, reason, overlapping, accepted);

    bool ownRefreshed = false;
    if (!fast && !overlapping.empty()) {
        auto fallback = OwnRefreshFallback::Count;
        if (!OwnImageRefreshEnabled()) fallback = OwnRefreshFallback::Disabled;
        else {
            refreshed = overlapping;
            fallback = refreshOwnObjects(shaders, refreshed);
        }
        if (fallback == OwnRefreshFallback::Count) {
            fast = fastRevalidate(serialBefore, refreshed, reason, overlapping, accepted);
            if (fast) ownRefreshed = true;
            else fallback = OwnRefreshFallback::Rerun;
        }
        if (fallback != OwnRefreshFallback::Count) countOwnRefreshFallback(fallback);
    }
    if (report != nullptr) report->path = !fast ? ProofPath::Full : ownRefreshed ? ProofPath::OwnRefreshed : ProofPath::Fast;
    if (!fast) {
        countFullWalk(reason);
        if (!fullWalk()) {
            const auto failure = [&] {
                switch (reason) {
                    case FastFail::Pending: return ProofFailure::Pending;
                    case FastFail::Evicted: return ProofFailure::Evicted;
                    case FastFail::Changed: return ProofFailure::Changed;
                    case FastFail::Keys: case FastFail::ClearedView: case FastFail::StorageKeys: return ProofFailure::Keys;
                    default: return ProofFailure::Other;
                }
            }();
            return finish(false, false, failure);
        }
    } else if (ownRefreshed || accepted) {
        if (profile && ownRefreshed) Revalidations().ownRefreshed.fetch_add(1, std::memory_order_relaxed);
        if (VerifyProofs()) {

            thread_local std::vector<std::pair<const StorageTexture*, std::uint64_t>> versions;
            versions.clear();
            for (const auto& surface : validatedTextures) {
                if (surface.source != nullptr) versions.emplace_back(surface.source, surface.source->Version());
            }
            for (const auto& image : storageTextures) versions.emplace_back(image.get(), image->Version());
            const bool same = fullWalk();
            const auto moved = std::find_if(versions.begin(), versions.end(), [](const auto& entry) { return entry.first->Version() != entry.second; });
            if (!same || moved != versions.end()) {
                aps5::LogErr( "[rescache] APS5_VERIFY_PROOFS: the T1 proof (%s) disagrees with the full walk (%s)\n", ownRefreshed ? "own-object refresh" : "accepted overlap", !same ? "another object" : "an upload");
                aps5::LogFlush(aps5::LogStdErr);
                std::abort();
            }
            if (profile) Revalidations().proofsVerified.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const auto serialLoop = [&] {
        for (const auto& region : directRegions) {
            auto serial = HostImportSerial(context, region.begin, static_cast<std::size_t>(region.end - region.begin), true);
            if (serial == 0) serial = ImageMirrorSerial(context, region.begin, static_cast<std::size_t>(region.end - region.begin));
            if (serial != region.serial) return false;
        }
        return true;
    };
    if (EpochRevalidate()) {
        if (!directRegions.empty() && !(pendingSerialSeen != 0 && pendingSerialSeen == StorageTexture::PendingSerial())) {
            thread_local std::vector<StorageTexture::PendingQuery> regions;
            regions.clear();
            for (const auto& region : directRegions) regions.push_back({region.begin, region.end, nullptr, nullptr, false});
            StorageTexture::ScanPending(regions);

            for (const auto& region : regions) {
                if (region.overlaps || AnyShadowedOverlaps(region.begin, static_cast<std::size_t>(region.end - region.begin))) StorageTexture::FlushPending(region.begin, static_cast<std::size_t>(region.end - region.begin), nullptr, "imported buffer region");
            }
        }
        if (HostImportsUnchanged(context, importsProof)) {
            if (profile) Revalidations().importSkips.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (!serialLoop()) return finish(fast, false, ProofFailure::Imports);
            importsProof = HostImportsIdentity(context);
        }
    } else {
        for (const auto& region : directRegions) StorageTexture::FlushPending(region.begin, static_cast<std::size_t>(region.end - region.begin), nullptr, "imported buffer region");
        if (!serialLoop()) return finish(fast, false, ProofFailure::Imports);
    }

    if (auto* recorder = Recorder::Active(); recorder != nullptr) {
        try {
            guestMemory.RecordStagingCopies(*recorder);
        } catch (const std::exception& error) {
            aps5::LogErr( "[resources] staging copies of a reused build failed: %s\n", error.what());
            return finish(fast, false);
        }
    }
    pendingSerialSeen = EpochRevalidate() && StorageTexture::PendingSerial() == serialBefore ? serialBefore : 0;
    return finish(fast, true);
}

namespace {

struct ChurnCounts {
    std::array<std::uint64_t, 8> roles{};
    std::array<std::uint64_t, 4> bufferWords{};
    std::array<std::uint64_t, 8> imageWords{};
    std::uint64_t firstSeen = 0;
    std::uint64_t sameKey = 0;
    std::uint64_t layout = 0;
    std::uint64_t drawWords = 0;
};

struct ChurnProfile {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, ResourceCache::Key> lastByVariant;
    ChurnCounts dispatch;
    ChurnCounts draws;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

ChurnProfile& Churn() {
    static ChurnProfile profile;
    return profile;
}

bool chargeShaderKey(ChurnCounts& profile, const ResourceCache::Key& key, std::size_t& at, std::size_t diff) {
    if (at + 5 > key.size() || diff < at + 5) {
        ++profile.layout;
        return true;
    }
    const bool dataWords = key[at] != 0;
    const auto bindings = key[at + 4];
    at += 5;
    for (std::uint32_t binding = 0; binding < bindings; ++binding) {
        if (at + 8 > key.size() || diff < at + 8) {
            ++profile.layout;
            return true;
        }
        const auto kind = key[at];
        const auto role = key[at + 1];
        const auto descriptorWords = dataWords || !DataRole(static_cast<ShaderRecompiler::DescriptorRole>(role)) ? key[at + 7] : 0u;
        at += 8;
        if (diff < at + descriptorWords) {
            if (role < profile.roles.size()) ++profile.roles[role];
            const auto index = diff - at;
            if (role == static_cast<std::uint32_t>(ShaderRecompiler::DescriptorRole::GuestBuffers)) ++profile.bufferWords[index % 4];
            else if (role == static_cast<std::uint32_t>(ShaderRecompiler::DescriptorRole::GuestImages) && (kind == static_cast<std::uint32_t>(ShaderRecompiler::DescriptorKind::SampledImage) || kind == static_cast<std::uint32_t>(ShaderRecompiler::DescriptorKind::StorageImage))) ++profile.imageWords[index % 8];
            return true;
        }
        at += descriptorWords;
        for (int flags = 0; flags < 4; ++flags) {
            if (at >= key.size()) {
                ++profile.layout;
                return true;
            }
            const auto words = 1 + (static_cast<std::size_t>(key[at]) + 31) / 32;
            if (diff < at + words) {
                ++profile.layout;
                return true;
            }
            at += words;
        }
    }
    return false;
}

}

std::shared_ptr<ShaderResources> ResourceCache::Find(const Key& key) {
    resourceCacheFinds.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    const auto found = index.find(key);
    if (found == index.end()) {
        noteMiss(key);
        return nullptr;
    }
    entries.splice(entries.begin(), entries, found->second);
    return found->second->second;
}

void ResourceCache::noteMiss(const Key& key) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile || key.size() < 5) return;

    const bool draw = key[0] == 0xffffffffu;
    std::uint64_t variants = 0;
    if (draw) {
        std::size_t at = 4;
        for (std::uint32_t stage = 0; stage < key[3] && at + 4 < key.size(); ++stage) {
            variants = variants * 1000003ull ^ (key[at + 3] | (static_cast<std::uint64_t>(key[at + 4]) << 32u));
            at += 1 + key[at];
        }
    } else {
        variants = key[2] | (static_cast<std::uint64_t>(key[3]) << 32u);
    }
    auto& churn = Churn();
    std::lock_guard lock(churn.mutex);
    auto& counts = draw ? churn.draws : churn.dispatch;
    const auto found = churn.lastByVariant.find(variants);
    if (found == churn.lastByVariant.end()) {
        ++counts.firstSeen;
        churn.lastByVariant.emplace(variants, key);
    } else {
        const auto& previous = found->second;
        const auto common = std::min(key.size(), previous.size());
        std::size_t diff = 0;
        while (diff < common && key[diff] == previous[diff]) ++diff;
        if (diff == common && key.size() == previous.size()) {
            ++counts.sameKey;
        } else if (draw) {
            std::size_t at = 4;
            bool charged = false;
            for (std::uint32_t stage = 0; stage < key[3] && at < key.size() && !charged; ++stage) {
                ++at;
                charged = chargeShaderKey(counts, key, at, diff);
            }
            if (!charged) ++counts.drawWords;
        } else {
            std::size_t at = 0;
            if (!chargeShaderKey(counts, key, at, diff)) ++counts.layout;
        }
        found->second = key;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - churn.lastReport < std::chrono::seconds(10)) return;
    churn.lastReport = now;
    const auto report = [](const char* what, const ChurnCounts& churn) {
        std::string line = std::string("[rescache] miss churn (") + what + "), first differing word by role:";
        char item[256];
        for (std::size_t role = 0; role < churn.roles.size(); ++role) {
            if (churn.roles[role] == 0) continue;
            std::snprintf(item, sizeof(item), " %s %llu", roleName(static_cast<ShaderRecompiler::DescriptorRole>(role)), static_cast<unsigned long long>(churn.roles[role]));
            line += item;
            if (role == static_cast<std::size_t>(ShaderRecompiler::DescriptorRole::GuestBuffers)) {
                std::snprintf(item, sizeof(item), " (V# word %llu/%llu/%llu/%llu)", static_cast<unsigned long long>(churn.bufferWords[0]), static_cast<unsigned long long>(churn.bufferWords[1]), static_cast<unsigned long long>(churn.bufferWords[2]), static_cast<unsigned long long>(churn.bufferWords[3]));
                line += item;
            } else if (role == static_cast<std::size_t>(ShaderRecompiler::DescriptorRole::GuestImages)) {
                std::snprintf(item, sizeof(item), " (T# word %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu)", static_cast<unsigned long long>(churn.imageWords[0]), static_cast<unsigned long long>(churn.imageWords[1]), static_cast<unsigned long long>(churn.imageWords[2]), static_cast<unsigned long long>(churn.imageWords[3]), static_cast<unsigned long long>(churn.imageWords[4]), static_cast<unsigned long long>(churn.imageWords[5]), static_cast<unsigned long long>(churn.imageWords[6]), static_cast<unsigned long long>(churn.imageWords[7]));
                line += item;
            }
        }
        std::snprintf(item, sizeof(item), "; layout %llu, draw target/index %llu, same key %llu (not inserted or evicted), first seen %llu", static_cast<unsigned long long>(churn.layout), static_cast<unsigned long long>(churn.drawWords), static_cast<unsigned long long>(churn.sameKey), static_cast<unsigned long long>(churn.firstSeen));
        line += item;
        aps5::LogErr( "%s\n", line.c_str());
    };
    report("dispatch", churn.dispatch);
    report("draws", churn.draws);
}

void ResourceCache::Insert(const Key& key, std::shared_ptr<ShaderResources> resources, std::vector<std::shared_ptr<ShaderResources>>* evicted) {
    std::lock_guard lock(mutex);
    if (const auto found = index.find(key); found != index.end()) {
        if (evicted != nullptr) evicted->push_back(std::move(found->second->second));
        entries.erase(found->second);
        index.erase(found);
    }
    entries.emplace_front(key, std::move(resources));
    index.emplace(key, entries.begin());

    static const std::size_t capacity = [] {
        const char* value = std::getenv("APS5_RESOURCE_CACHE_ENTRIES");
        const auto parsed = value ? std::strtoull(value, nullptr, 10) : 1024ull;
        return static_cast<std::size_t>(parsed != 0 ? parsed : 1024ull);
    }();
    while (entries.size() > capacity) {
        if (evicted != nullptr) evicted->push_back(std::move(entries.back().second));
        index.erase(entries.back().first);
        entries.pop_back();
    }
}

void ResourceCache::Remove(const Key& key, const ShaderResources* object) {
    std::lock_guard lock(mutex);
    if (object != nullptr) {
        const auto found = index.find(key);
        if (found == index.end() || found->second->second.get() != object) return;
    }
    erase(key);
}

bool ResourceCache::Touch(const Key& key) {
    resourceCacheTouches.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    const auto found = index.find(key);
    if (found == index.end()) return false;
    entries.splice(entries.begin(), entries, found->second);
    return true;
}

std::uint64_t ResourceCache::Finds() {
    return resourceCacheFinds.load(std::memory_order_relaxed);
}

std::uint64_t ResourceCache::Touches() {
    return resourceCacheTouches.load(std::memory_order_relaxed);
}

void ResourceCache::Clear() {
    std::lock_guard lock(mutex);
    index.clear();
    entries.clear();
}

std::size_t ResourceCache::Size() const {
    std::lock_guard lock(mutex);
    return entries.size();
}

void ResourceCache::erase(const Key& key) {
    const auto found = index.find(key);
    if (found == index.end()) return;
    entries.erase(found->second);
    index.erase(found);
}

ResourceCache& SharedResourceCache() {
    static auto* cache = new ResourceCache();
    return *cache;
}

DescriptorCache::DescriptorCache(const Context& context) : context(context), destroyLayout(context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")), destroyPool(context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")), freeSets(context.Function<PFN_vkFreeDescriptorSets>("vkFreeDescriptorSets")) {}

DescriptorCache::~DescriptorCache() {
    for (const auto pool : pools) destroyPool(context.device, pool, nullptr);
    for (const auto& [key, layout] : layouts) destroyLayout(context.device, layout, nullptr);
}

VkDescriptorSetLayout DescriptorCache::Layout(std::span<const std::uint32_t> key, std::span<const VkDescriptorSetLayoutBinding> bindings) {
    std::lock_guard lock(mutex);
    std::vector<std::uint32_t> keyCopy(key.begin(), key.end());
    if (const auto found = layouts.find(keyCopy); found != layouts.end()) {
        ++stats.layoutHits;
        return found->second;
    }
    ++stats.layoutMisses;
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &info, nullptr, &layout), "vkCreateDescriptorSetLayout");
    layouts.emplace(std::move(keyCopy), layout);
    return layout;
}

namespace {

constexpr std::uint32_t ChainPoolSets = 1024;
constexpr std::array<VkDescriptorPoolSize, 4> ChainPoolSizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 512}}};
}

DescriptorCache::SetAllocation DescriptorCache::Allocate(VkDescriptorSetLayout layout, std::span<const VkDescriptorPoolSize> sizes) {
    for (const auto& size : sizes) {
        const auto capacity = std::find_if(ChainPoolSizes.begin(), ChainPoolSizes.end(), [&](const auto& item) { return item.type == size.type; });
        if (capacity == ChainPoolSizes.end() || size.descriptorCount > capacity->descriptorCount) return {};
    }
    std::lock_guard lock(mutex);
    const auto allocate = context.Resolved(&DeviceFunctions::allocateDescriptorSets, "vkAllocateDescriptorSets");
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &layout;

    for (auto it = pools.rbegin(); it != pools.rend(); ++it) {
        allocation.descriptorPool = *it;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const auto result = allocate(context.device, &allocation, &set);
        if (result == VK_SUCCESS) {
            ++stats.sets;
            return {set, *it};
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) Check(result, "vkAllocateDescriptorSets");
    }
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = ChainPoolSets;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(ChainPoolSizes.size());
    poolInfo.pPoolSizes = ChainPoolSizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    pools.push_back(pool);
    ++stats.pools;
    allocation.descriptorPool = pool;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Check(allocate(context.device, &allocation, &set), "vkAllocateDescriptorSets");
    ++stats.sets;
    return {set, pool};
}

void DescriptorCache::Free(const SetAllocation& allocation) noexcept {
    if (allocation.set == VK_NULL_HANDLE || allocation.pool == VK_NULL_HANDLE) return;
    std::lock_guard lock(mutex);
    static_cast<void>(freeSets(context.device, allocation.pool, 1, &allocation.set));
}

DescriptorCache::Stats DescriptorCache::Counters() const {
    std::lock_guard lock(mutex);
    return stats;
}

std::size_t ShaderResources::addGuestBuffer(std::span<const std::uint32_t> words, const ColorTarget* target, std::uint64_t indexAddress, std::size_t indexBytes, bool written, bool atomic) {
    Require(words.size() == 4, "buffer descriptor must contain four DWORDs");
    Require((words[1] & 0x40000000u) == 0, "buffer descriptor has reserved bits set");
    const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1], words[2], words[3]}};
    Require(descriptor.Type() == 0u, "buffer descriptor uses an unsupported type");
    const auto address = descriptor.Base48();
    const auto byteSize = descriptor.GetSize();
    if (byteSize == 0 || address == 0) {
        allocations.push_back({0, EmptyBufferBytes, false, nullptr, ShaderRecompiler::DescriptorRole::GuestBuffers, false});
        return allocations.size() - 1;
    }
    Require(byteSize <= context.limits.maxStorageBufferRange, "shader buffer exceeds descriptor range limit");
    Require(byteSize <= std::numeric_limits<std::size_t>::max(), "shader buffer size exceeds host address space");
    const auto size = static_cast<std::size_t>(byteSize);
    Require(target == nullptr || !overlap(address, size, target->address, target->bytes), "shader buffer aliases the render target");

    static const bool allWritten = std::getenv("APS5_ALL_BUFFERS_WRITTEN") != nullptr;
    Require(!written || !overlap(address, size, indexAddress, indexBytes), "writable shader buffer aliases the index buffer");
    written = written || allWritten;
    if (written) guestMemory.AddWritable(address, size, atomic);
    else {
        guestMemory.AddReadable(address, size);
        ++readOnlyBuffers;
    }
    if (BuildProfiled()) {
        auto& counters = BufferWrites();
        counters.elements.fetch_add(1, std::memory_order_relaxed);
        if (!written) counters.readOnly.fetch_add(1, std::memory_order_relaxed);
    }
    {
        static const bool traceCb = std::getenv("APS5_TRACE_CB") != nullptr;
        static std::atomic<int> traced{0};
        if (traceCb && !written && size >= 16 && size <= 4096 && traced.load() < 6000 && GuestMemory::Accessible(reinterpret_cast<const void*>(address), 32)) {
            traced.fetch_add(1);
            float f[8];
            std::memcpy(f, reinterpret_cast<const void*>(address), 32);
            if (size == 176 && GuestMemory::Accessible(reinterpret_cast<const void*>(address), 176)) {
                float all[44];
                std::memcpy(all, reinterpret_cast<const void*>(address), 176);
                aps5::LogErr( "[cb176]");
                for (int i = 0; i < 44; ++i) aps5::LogErr( " %g", all[i]);
                aps5::LogChar(aps5::LogStdErr, 10);
            }
            aps5::LogErr( "[cb] 0x%llx size %zu w3=%08x: %g %g %g %g | %g %g %g %g\n", static_cast<unsigned long long>(address), size, words[3], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
        }
    }
    allocations.push_back({address, size, true, nullptr, ShaderRecompiler::DescriptorRole::ShaderData, written});
    return allocations.size() - 1;
}

std::string ShaderResources::Describe() const {
    const auto sample = [](std::uint64_t address, std::uint64_t bytes) {

        if (!GuestMemory::Accessible(reinterpret_cast<const void*>(address), static_cast<std::size_t>(bytes))) return -1.0;
        std::size_t nonzero = 0;
        std::size_t samples = 0;
        const auto* data = reinterpret_cast<const std::uint32_t*>(address);
        const auto words = bytes / 4;
        const auto step = std::max<std::uint64_t>(1, words / 4096);
        for (std::uint64_t i = 0; i < words; i += step, ++samples) nonzero += data[i] != 0;
        return samples == 0 ? 0.0 : static_cast<double>(nonzero) / samples;
    };
    std::string text;
    char line[160];
    for (const auto& range : describedRanges) {
        std::snprintf(line, sizeof(line), " %s 0x%llx+0x%llx(%ux%u f%u t%d) nz=%.2f", range.kind, static_cast<unsigned long long>(range.address), static_cast<unsigned long long>(range.bytes), range.width, range.height, range.format, range.tileMode, sample(range.address, range.bytes));
        text += line;
        if (range.dccAddress != 0) {
            const auto keys = ReadDccKeys(range.dccAddress, range.bytes);
            std::snprintf(line, sizeof(line), " dcc=%s@0x%llx", DccKeysName(keys), static_cast<unsigned long long>(range.dccAddress));
            text += line;
            if (keys == DccKeys::Mixed) {

                const auto* bytes = reinterpret_cast<const std::uint8_t*>(range.dccAddress);
                const auto count = static_cast<std::size_t>(range.bytes / 256u);
                std::size_t run = 1;
                while (run < count && bytes[run] == bytes[0]) ++run;
                std::snprintf(line, sizeof(line), "(%02x x%zu then %02x of %zu)", bytes[0], run, run < count ? bytes[run] : 0u, count);
                text += line;
            }
        }
    }

    static const bool words = [] { const char* value = std::getenv("APS5_TRACE_DISPATCH_IO"); return value != nullptr && value[0] == '2'; }();
    const auto appendWords = [&](const std::uint32_t* data, std::size_t bytes) {
        if (!words || bytes > 0x200) return;
        text += " [";
        for (std::size_t i = 0; i < bytes / 4; ++i) {
            char word[12];
            std::snprintf(word, sizeof(word), "%s%08x", i == 0 ? "" : " ", data[i]);
            text += word;
        }
        text += "]";
    };
    for (const auto& allocation : allocations) {
        if (allocation.guest) {
            std::snprintf(line, sizeof(line), " buffer%s 0x%llx+0x%zx nz=%.2f", allocation.written ? "" : "(ro)", static_cast<unsigned long long>(allocation.address), allocation.size, sample(allocation.address, allocation.size));
            text += line;
            if (GuestMemory::Accessible(reinterpret_cast<const void*>(allocation.address), allocation.size)) appendWords(reinterpret_cast<const std::uint32_t*>(allocation.address), allocation.size);
        } else if (allocation.buffer) {
            const auto bytes = allocation.buffer->Bytes();
            std::snprintf(line, sizeof(line), " data+0x%zx nz=%.2f", allocation.size, sample(reinterpret_cast<std::uint64_t>(bytes.data()), allocation.size));
            text += line;
            appendWords(reinterpret_cast<const std::uint32_t*>(bytes.data()), allocation.size);
        }
    }
    return text;
}

std::size_t ShaderResources::addDataBuffer(std::span<const std::uint32_t> words) {
    const auto size = words.size() * sizeof(std::uint32_t);
    Require(size <= context.limits.maxStorageBufferRange, "shader data buffer exceeds descriptor range limit");
    const bool refreshable = TemplateDataRefresh() && size <= MaxRefreshBytes;
    auto buffer = std::make_unique<Buffer>(context, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | (refreshable ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0u));
    std::memcpy(buffer->Bytes().data(), words.data(), size);
    Allocation allocation{0, size, false, std::move(buffer)};
    if (refreshable) allocation.dataWords.assign(words.begin(), words.end());
    allocations.push_back(std::move(allocation));
    mixDataWords(dataWordsHash, allocations.back().dataWords);
    return allocations.size() - 1;
}

void ShaderResources::rehashDataWords() {
    dataWordsHash = FnvOffset;

    for (const auto& allocation : allocations) {
        if (!allocation.guest && allocation.buffer != nullptr) mixDataWords(dataWordsHash, allocation.dataWords);
    }
}

std::uint64_t ShaderResources::DataWordsHash(const CompiledShader& shader) {
    Require(shader.program != nullptr, "missing compiled shader");
    std::uint64_t hash = FnvOffset;
    for (const auto& binding : shader.program->bindings) {
        if (DataRole(binding.role)) mixDataWords(hash, binding.guestDescriptor);
    }
    return hash;
}

bool ShaderResources::DataWordsDiffer(const CompiledShader& shader) const {
    Require(shader.program != nullptr, "missing compiled shader");
    const auto& program = *shader.program;
    if (program.bindings.size() != bindings.size()) return true;
    for (std::size_t index = 0; index < program.bindings.size(); ++index) {
        const auto& binding = program.bindings[index];
        if (!DataRole(binding.role)) continue;
        if (bindings[index].allocations.size() != 1) return true;
        const auto& allocation = allocations[bindings[index].allocations.front()];
        if (allocation.dataWords.size() != binding.guestDescriptor.size() || !std::equal(allocation.dataWords.begin(), allocation.dataWords.end(), binding.guestDescriptor.begin())) return true;
    }
    return false;
}

bool ShaderResources::RefreshData(VkCommandBuffer commands, const CompiledShader& shader, Recorder* recorder) {
    Require(shader.program != nullptr, "missing compiled shader");
    const auto& program = *shader.program;
    Require(program.bindings.size() == bindings.size(), "template bindings disagree with the shader");
    bool recorded = false;

    auto timing = Recorder::NoTiming;
    std::uint64_t refreshedBytes = 0;
    for (std::size_t index = 0; index < program.bindings.size(); ++index) {
        const auto& binding = program.bindings[index];
        if (!DataRole(binding.role)) continue;
        Require(bindings[index].allocations.size() == 1, "data binding without its buffer");
        auto& allocation = allocations[bindings[index].allocations.front()];
        const auto size = binding.guestDescriptor.size() * sizeof(std::uint32_t);
        Require(allocation.buffer != nullptr && !allocation.guest && allocation.size == size && size <= MaxRefreshBytes, "template data buffer cannot take the dispatch's words");
        if (allocation.dataWords.size() == binding.guestDescriptor.size() && std::equal(allocation.dataWords.begin(), allocation.dataWords.end(), binding.guestDescriptor.begin())) continue;
        if (recorder != nullptr && timing == Recorder::NoTiming) timing = recorder->BeginGpuTiming(Recorder::CommandClass::TemplateDataRefresh);
        writeDataWords(commands, bindings[index].allocations.front(), binding.guestDescriptor);
        allocation.dataWords.assign(binding.guestDescriptor.begin(), binding.guestDescriptor.end());
        refreshedBytes += size;
        recorded = true;
    }
    if (timing != Recorder::NoTiming) recorder->EndGpuTiming(timing, refreshedBytes);
    if (recorded) rehashDataWords();
    return recorded;
}

void ShaderResources::writeDataWords(VkCommandBuffer commands, std::size_t allocation, std::span<const std::uint32_t> words) const {
    const auto& buffer = *allocations[allocation].buffer;
    const auto size = words.size() * sizeof(std::uint32_t);
    if (std::none_of(dataPatches.begin(), dataPatches.end(), [&](const DataPatch& patch) { return patch.allocation == allocation; })) {
        context.Resolved(&DeviceFunctions::cmdUpdateBuffer, "vkCmdUpdateBuffer")(commands, buffer.Handle(), 0, size, words.data());
        return;
    }
    std::vector<std::uint32_t> patched(words.begin(), words.end());
    auto* bytes = reinterpret_cast<std::byte*>(patched.data());
    for (const auto& patch : dataPatches) {
        if (patch.allocation == allocation && patch.byte < size) bytes[patch.byte] = static_cast<std::byte>(patch.adjustment);
    }
    context.Resolved(&DeviceFunctions::cmdUpdateBuffer, "vkCmdUpdateBuffer")(commands, buffer.Handle(), 0, size, patched.data());
}

void ShaderResources::PrecollectSurfaces() const {
    for (const auto& range : describedRanges) GuestMemory::CollectWrites(range.address, static_cast<std::size_t>(range.bytes));
}

void ShaderResources::addImageBinding(const ShaderRecompiler::DescriptorBinding& binding, VkShaderStageFlags flags) {
    Require(binding.count != 0, "empty descriptor binding");
    if (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) {

        Require(binding.role == ShaderRecompiler::DescriptorRole::GuestImages, "storage image binding has a non-image role");
        Require(binding.guestDescriptor.size() == static_cast<std::size_t>(binding.count) * 8u, "guest storage image descriptors must contain 8 dwords each");
        Require(context.detiler != nullptr, "device texture detiler is unavailable");
        Require(binding.count <= context.limits.maxPerStageDescriptorStorageImages, "shader storage-image descriptors exceed per-stage limits");
        bindings.push_back({{binding.binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, binding.count, flags, nullptr}, {}, {}});
        plannedStorageImages += binding.count;
        deferredImages.push_back({&binding, bindings.size() - 1});
        return;
    }
    const bool sampledImage = binding.kind == ShaderRecompiler::DescriptorKind::SampledImage;
    const bool samplerKind = binding.kind == ShaderRecompiler::DescriptorKind::Sampler;
    if (!(sampledImage || samplerKind)) Require(false, std::string("unsupported descriptor kind ") + kindName(binding.kind) + " for role " + roleName(binding.role));
    Require((sampledImage && binding.role == ShaderRecompiler::DescriptorRole::GuestImages) || (samplerKind && binding.role == ShaderRecompiler::DescriptorRole::GuestSamplers), "guest image descriptor role disagrees with its kind");
    Require(binding.guestDescriptor.size() % binding.count == 0, "guest image descriptor size is not a multiple of the binding count");
    const auto elementWords = binding.guestDescriptor.size() / binding.count;

    Binding item{{binding.binding, sampledImage ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, binding.count, flags, nullptr}, {}, {}};

    if (sampledImage) {
        Require(elementWords == 8, "guest texture descriptor must contain 8 dwords");
        Require(binding.imageShape.has_value(), "guest image binding is missing an image shape");
        Require(context.detiler != nullptr, "device texture detiler is unavailable");
        Require(context.textureCache != nullptr, "device texture cache is unavailable");
        Require(binding.count <= context.limits.maxPerStageDescriptorSampledImages, "shader sampled-image descriptors exceed per-stage limits");
        plannedSampledImages += binding.count;
        Require(plannedSampledImages <= context.limits.maxDescriptorSetSampledImages, "pipeline sampled-image descriptors exceed device limits");
    } else {
        Require(elementWords == 4, "guest sampler descriptor must contain 4 dwords");
        Require(binding.count <= context.limits.maxPerStageDescriptorSamplers, "shader sampler descriptors exceed per-stage limits");
        Require(binding.samplerDepthCompare.size() == binding.count, "guest sampler binding is missing depth comparison metadata");
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
            const bool compareEnable = binding.samplerDepthCompare.at(element);
            static const bool noSamplerCache = std::getenv("APS5_NO_SAMPLER_CACHE") != nullptr;
            if (context.samplerCache != nullptr && !noSamplerCache) {
                samplers.push_back(context.samplerCache->Get(context, words, compareEnable));
            } else {
                auto resource = DecodeSamplerResource(words);
                resource.compareEnable = compareEnable;
                samplers.push_back(std::make_shared<Sampler>(context, resource));
            }
            item.imageAllocations.push_back(samplers.size() - 1);
        }
        Require(samplers.size() <= context.limits.maxDescriptorSetSamplers, "pipeline sampler descriptors exceed device limits");
    }

    bindings.push_back(std::move(item));
    if (sampledImage) deferredImages.push_back({&binding, bindings.size() - 1});
}

namespace {

template <typename Visit>
void forEachImageElement(const ShaderRecompiler::RecompileResult& program, Visit&& visit) {
    for (const auto& binding : program.bindings) {
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages || binding.count == 0) continue;
        if (binding.kind != ShaderRecompiler::DescriptorKind::SampledImage && binding.kind != ShaderRecompiler::DescriptorKind::StorageImage) continue;
        const auto elementWords = binding.guestDescriptor.size() / binding.count;
        for (std::uint32_t element = 0; element < binding.count; ++element) visit(binding, element, std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords));
    }
}

}

bool ShaderResources::precollectImages() {

    static const bool disabled = std::getenv("APS5_NO_PRECOLLECT") != nullptr;
    static const bool noRecords = std::getenv("APS5_NO_STAGE_A_IMAGES") != nullptr;
    if (disabled || deferredImages.empty()) return false;
    imageRecords.clear();
    nextImageRecord = 0;
    auto& counters = TextureCounts();
    for (const auto& deferred : deferredImages) {
        const auto& binding = *deferred.binding;
        const auto elementWords = binding.guestDescriptor.size() / binding.count;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
            ImageRecord record;
            record.sampled = binding.kind == ShaderRecompiler::DescriptorKind::SampledImage;
            if (IsNullTextureDescriptor(words)) {
                record.decoded = false;
                imageRecords.push_back(std::move(record));
                continue;
            }
            try {
                record.resource = DecodeTextureResource(words);
                record.guestBytes = DescribeSurface(record.resource).guestBytes;
                record.generation = GuestMemory::CollectWrites(record.resource.baseAddress, static_cast<std::size_t>(record.guestBytes));
                record.decoded = true;
                if (record.sampled && !noRecords && words.size() == 8 && (binding.imageDepthCompare.empty() || !binding.imageDepthCompare.at(element))) {
                    std::copy(words.begin(), words.end(), record.words.begin());
                    record.components = {ComponentSwizzleFor(record.resource.dstSelX), ComponentSwizzleFor(record.resource.dstSelY), ComponentSwizzleFor(record.resource.dstSelZ), ComponentSwizzleFor(record.resource.dstSelW)};
                    record.keys = TextureClearKeys(record.resource, record.guestBytes);
                    auto& cache = Textures();
                    std::lock_guard lock(cache.mutex);
                    if (const auto it = findTexture(cache, MakeTextureKey(context.device, words, record.components)); it != cache.entries.end()) {
                        record.texture = it->texture;
                        record.source = it->source;
                        record.entryKeys = it->keys;
                        record.entryGeneration = it->generation;
                        counters.records.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (const std::exception&) {
                record.decoded = false;
            }
            imageRecords.push_back(std::move(record));
        }
    }
    return true;
}

std::shared_ptr<Texture> ShaderResources::fastTexture(const ImageRecord& record) {
    if (record.texture == nullptr) return nullptr;
    struct Outcome {
        bool profile;
        std::chrono::steady_clock::time_point start;
        bool hit = false;
        ~Outcome() {
            if (profile) LookupOutcomes::Add(hit ? LookupOutcomes::SampledFast : LookupOutcomes::SampledFastMiss, start);
        }
    } outcome{LookupOutcomes::Profiled(), LookupOutcomes::Profiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};

    const auto address = record.resource.baseAddress;
    const auto bytes = static_cast<std::size_t>(record.guestBytes);
    if (GuestMemory::CollectWrites(address, bytes) == 0) return nullptr;
    if (PendingStorageOverlaps(address, bytes, record.source.get())) return nullptr;
    auto keys = record.keys;
    if (record.resource.dccAddress != 0) {
        const auto scanStart = outcome.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

        keys = ProvedClearKeys(record.resource, record.guestBytes, SameKeySurface(record.source.get(), record.resource, record.guestBytes) ? record.source->KeyProof() : record.texture->KeyProof());
        if (outcome.profile) LookupOutcomes::Add(LookupOutcomes::DccScan, scanStart);
    }

    if (keys != record.entryKeys) return nullptr;
    if (record.source != nullptr) {
        if (!StorageImageCached(context, record.source.get()) || !GuestMemory::UnchangedSince(address, bytes, record.source->Generation())) return nullptr;
        if (record.resource.dccAddress != 0 && IsDccClear(record.source->FilledKeys())) return nullptr;

        if (keys != DccKeys::Uncompressed && StorageTexture::FindPending(address, record.guestBytes) != record.source) return nullptr;
    } else if (keys == DccKeys::Uncompressed && !GuestMemory::UnchangedSince(address, bytes, record.entryGeneration)) {
        return nullptr;
    }

    auto& cache = Textures();
    std::lock_guard lock(cache.mutex);
    const auto it = findTexture(cache, MakeTextureKey(context.device, record.words, record.components));
    if (it == cache.entries.end() || it->texture != record.texture) return nullptr;
    if (it->source == nullptr) it->generation = record.generation;
    touchTexture(cache, it);
    logLookup({record.texture.get(), record.resource, record.guestBytes, keys, record.source != nullptr ? 0 : record.generation, record.source.get()});
    outcome.hit = true;
    return record.texture;
}

void ShaderResources::resolveImageBinding(const ShaderRecompiler::DescriptorBinding& binding, Binding& item) {
    auto& counters = TextureCounts();

    const auto nextRecord = [&]() -> const ImageRecord* { return nextImageRecord < imageRecords.size() ? &imageRecords[nextImageRecord++] : nullptr; };
    if (binding.kind == ShaderRecompiler::DescriptorKind::SampledImage) {
        const auto elementWords = binding.guestDescriptor.size() / binding.count;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
            const auto* record = nextRecord();
            if (IsNullTextureDescriptor(words) && binding.imageShape.has_value()) {
                textures.push_back(nullTexture(context, *binding.imageShape, words));
                textureFirstLayer.push_back(false);
                describedRanges.push_back({"texture", 0, 0, 1, 1, 56, 0, 0});
                item.imageAllocations.push_back(textures.size() - 1);
                continue;
            }
            const auto resource = record != nullptr && record->decoded ? record->resource : DecodeTextureResource(words);
            const bool firstLayer = binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2D && resource.dimension == TextureDimension::k2DArray;
            if (!firstLayer && !MatchesGuestDimension(*binding.imageShape, resource.dimension)) throw std::runtime_error("AGC graphics: guest texture dimension disagrees with the shader's declared image shape (shape " + std::to_string(static_cast<int>(*binding.imageShape)) + ", dimension " + std::to_string(static_cast<int>(resource.dimension)) + ")");
            const VkComponentMapping components{ComponentSwizzleFor(resource.dstSelX), ComponentSwizzleFor(resource.dstSelY), ComponentSwizzleFor(resource.dstSelZ), ComponentSwizzleFor(resource.dstSelW)};
            const auto guestBytes = record != nullptr && record->decoded ? record->guestBytes : DescribeSurface(resource).guestBytes;
            std::shared_ptr<Texture> texture;
            if (record != nullptr && record->texture != nullptr) {
                texture = fastTexture(*record);
                (texture != nullptr ? counters.fastHits : counters.fastMisses).fetch_add(1, std::memory_order_relaxed);
            }
            if (texture == nullptr) texture = cachedTexture(context, words, resource, components, guestBytes, !binding.imageDepthCompare.empty() && binding.imageDepthCompare.at(element));
            textures.push_back(std::move(texture));
            textureFirstLayer.push_back(firstLayer);
            describedRanges.push_back({"texture", resource.baseAddress, guestBytes, resource.width, resource.height, resource.format, static_cast<int>(resource.tileMode), resource.dccAddress});
            item.imageAllocations.push_back(textures.size() - 1);
        }

        reportTextureCounters();
        return;
    }

    std::uint32_t mipOffset = 0;
    for (std::uint32_t element = 0; element < binding.count; ++element) {
        const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u);
        const bool sameAsPrevious = SameAsPreviousStorageElement(binding, element);
        if (sameAsPrevious) ++mipOffset;
        else mipOffset = 0;
        const auto* record = nextRecord();
        const auto resource = record != nullptr && record->decoded ? record->resource : DecodeTextureResource(words);
        const bool firstLayer = binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2D && resource.dimension == TextureDimension::k2DArray;
        if (binding.imageShape.has_value() && !firstLayer && !MatchesGuestDimension(*binding.imageShape, resource.dimension)) throw std::runtime_error("AGC graphics: guest storage texture dimension disagrees with the shader's declared image shape (shape " + std::to_string(static_cast<int>(*binding.imageShape)) + ", dimension " + std::to_string(static_cast<int>(resource.dimension)) + ")");
        const auto mip = std::min(resource.baseLevel + mipOffset, resource.mipCount - 1u);
        Require(resource.minLod <= mip * 256u, "guest storage texture descriptor clamps its minimum LOD above the level it addresses, which is not implemented");
        const auto guestBytes = record != nullptr && record->decoded ? record->guestBytes : DescribeSurface(resource).guestBytes;

        if (sameAsPrevious && StorageDedupeEnabled()) storageTextures.push_back(storageTextures.back());
        else storageTextures.push_back(cachedStorageTexture(context, words, resource, mip, guestBytes));
        storageMips.push_back(mip);
        storageFirstLayer.push_back(firstLayer);

        storageWritten.push_back(element >= binding.imageWritten.size() || binding.imageWritten[element]);
        describedRanges.push_back({"storage", resource.baseAddress, guestBytes, resource.width, resource.height, resource.format, static_cast<int>(resource.tileMode), resource.dccAddress});
        item.imageAllocations.push_back(storageTextures.size() - 1);
    }
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> ShaderResources::PresyncSurfaces() const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> surfaces;

    const auto consider = [&](std::uint32_t format, std::uint64_t address, std::uint64_t guestBytes, bool sampled) {
        const bool gpuDirect = sampled ? SampledFromStorageEligible(context, format, address, guestBytes) : HostImportCovers(context, address, static_cast<std::size_t>(guestBytes));
        if (!gpuDirect) surfaces.emplace_back(address, guestBytes);
    };
    if (completed) {

        std::size_t textureIndex = 0;
        for (const auto& range : describedRanges) {
            if (std::strcmp(range.kind, "texture") == 0 && textureIndex < textures.size()) {
                const auto& texture = textures[textureIndex++];
                if (range.bytes == 0) continue;
                if (texture->ViewsStorageImage()) consider(range.format, range.address, range.bytes, false);
                else consider(range.format, range.address, range.bytes, true);
            }
        }
        for (std::size_t index = 0; index < storageTextures.size(); ++index) {
            if (index != 0 && storageTextures[index] == storageTextures[index - 1]) continue;
            const auto& image = *storageTextures[index];
            consider(image.Descriptor().format, image.Descriptor().baseAddress, image.GuestBytes(), false);
        }
        return surfaces;
    }
    if (!imageRecords.empty()) {
        for (const auto& record : imageRecords) {
            if (record.decoded) consider(record.resource.format, record.resource.baseAddress, record.guestBytes, record.sampled);
        }
        return surfaces;
    }

    if (deferredCompute.program == nullptr) return surfaces;
    forEachImageElement(*deferredCompute.program, [&](const ShaderRecompiler::DescriptorBinding& binding, std::uint32_t, std::span<const std::uint32_t> words) {
        if (IsNullTextureDescriptor(words)) return;
        try {
            const auto resource = DecodeTextureResource(words);
            consider(resource.format, resource.baseAddress, DescribeSurface(resource).guestBytes, binding.kind == ShaderRecompiler::DescriptorKind::SampledImage);
        } catch (const std::exception&) {

        }
    });
    return surfaces;
}

ShaderResources::~ShaderResources() {
    release();
}

void ShaderResources::release() noexcept {

    if (cachePool && context.descriptorCache != nullptr) context.descriptorCache->Free({_set, cachePool});
    if (pool) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
    if (_layout && ownsLayout) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, _layout, nullptr);
    cachePool = VK_NULL_HANDLE;
    pool = VK_NULL_HANDLE;
    _set = VK_NULL_HANDLE;
    _layout = VK_NULL_HANDLE;
}

VkDescriptorSetLayout ShaderResources::Layout() const {
    return _layout;
}

ShaderResources::DrawBindings::~DrawBindings() {
    if (cache != nullptr && allocation.set != VK_NULL_HANDLE) cache->Free(allocation);
}

std::shared_ptr<ShaderResources::DrawBindings> ShaderResources::PrepareDrawBindings(Recorder& recorder) const {
    if (_set == VK_NULL_HANDLE || usesBda) return {};
    const auto reads = guestMemory.InPlaceReads();
    auto result = std::make_shared<DrawBindings>();
    std::vector<std::size_t> selected;
    for (std::size_t index = 0; index < allocations.size(); ++index) {
        const auto& item = allocations[index];
        if (!item.guest || item.written || guestMemory.WritesOverlap(item.address, item.size)) continue;
        const bool direct = std::any_of(reads.begin(), reads.end(), [&](const auto& range) { return item.address >= range.first && item.address < range.second && item.size <= range.second - item.address; });
        if (!direct || recorder.PendingWriteOverlaps(item.address, item.size)) continue;
        const auto begin = item.address - item.adjustment;
        const auto bytes = item.size + item.adjustment;
        const auto registryGeneration = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        const auto generation = GuestMemory::CollectWrites(begin, bytes);
        auto buffer = recorder.ReusableDrawSnapshot(begin, bytes);
        if (buffer == nullptr) {
            buffer = std::make_shared<Buffer>(context, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            std::memcpy(buffer->Bytes().data(), reinterpret_cast<const void*>(begin), bytes);
            recorder.KeepDrawSnapshot(begin, bytes, generation, registryGeneration, buffer);
        }
        selected.push_back(index);
        result->snapshots.push_back({begin, std::move(buffer)});
        CaptureTrace::Log("draw-snapshot batch=%llu address=%llx bytes=%zu", static_cast<unsigned long long>(recorder.Submissions() + 1), static_cast<unsigned long long>(begin), bytes);
    }
    if (selected.empty()) return {};
    Require(context.descriptorCache != nullptr, "draw snapshots require a descriptor cache");
    std::map<VkDescriptorType, std::uint32_t> counts;
    for (const auto& binding : bindings) counts[binding.layout.descriptorType] += binding.layout.descriptorCount;
    std::vector<VkDescriptorPoolSize> sizes;
    for (const auto& [type, count] : counts) sizes.push_back({type, count});
    result->cache = context.descriptorCache;
    result->allocation = result->cache->Allocate(_layout, sizes);
    Require(result->allocation.set != VK_NULL_HANDLE, "draw snapshot descriptor allocation failed");
    std::vector<VkCopyDescriptorSet> copies;
    for (const auto& binding : bindings) {
        VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
        copy.srcSet = _set;
        copy.srcBinding = binding.layout.binding;
        copy.dstSet = result->allocation.set;
        copy.dstBinding = binding.layout.binding;
        copy.descriptorCount = binding.layout.descriptorCount;
        copies.push_back(copy);
    }
    const auto update = context.Resolved(&DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets");
    update(context.device, 0, nullptr, static_cast<std::uint32_t>(copies.size()), copies.data());
    std::vector<VkDescriptorBufferInfo> infos;
    infos.reserve(selected.size());
    for (const auto& snapshot : result->snapshots) infos.push_back({snapshot.buffer->Handle(), 0, snapshot.buffer->Bytes().size()});
    std::vector<VkWriteDescriptorSet> writes;
    for (const auto& binding : bindings) {
        for (std::size_t element = 0; element < binding.allocations.size(); ++element) {
            const auto found = std::find(selected.begin(), selected.end(), binding.allocations[element]);
            if (found == selected.end()) continue;
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = result->allocation.set;
            write.dstBinding = binding.layout.binding;
            write.dstArrayElement = static_cast<std::uint32_t>(element);
            write.descriptorCount = 1;
            write.descriptorType = binding.layout.descriptorType;
            write.pBufferInfo = &infos[static_cast<std::size_t>(found - selected.begin())];
            writes.push_back(write);
        }
    }
    update(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    recorder.Keep(result);
    return result;
}

void ShaderResources::Bind(VkCommandBuffer commands, VkPipelineBindPoint bindPoint, VkPipelineLayout layout) const {
    if (_set == VK_NULL_HANDLE) return;
    context.Resolved(&DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, bindPoint, layout, 0, 1, &_set, 0, nullptr);
}

namespace {

bool SkipWriteBack() {
    static const bool skip = std::getenv("APS5_GPU_NO_WRITEBACK") != nullptr;
    return skip;
}
}

void ShaderResources::WriteBack() {
    WriteBackBuffers();
    if (SkipWriteBack()) return;
    for (std::size_t index = 0; index < storageTextures.size(); ++index) {
        if (storageWritten[index]) storageTextures[index]->MarkDirty();
    }
}

void ShaderResources::MarkGpuWrites(Recorder& recorder) {

    recorder.NotePendingReads(guestMemory.InPlaceReads(), guestMemory.HoldsLease() ? Recorder::ReadKind::AddressBased : Recorder::ReadKind::DispatchElement);
    if (SkipWriteBack()) return;
    for (std::size_t index = 0; index < storageTextures.size(); ++index) {
        if (storageWritten[index]) storageTextures[index]->MarkDirty();
    }

    guestMemory.RecordCopyBacks(recorder);

    recorder.NotePendingWrites(guestMemory.Writes());
    guestMemory.MarkDirectWrites();
    if (!BuildProfiled()) return;
    auto& counters = BufferWrites();

    counters.notesSkipped.fetch_add(readOnlyBuffers, std::memory_order_relaxed);
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = counters.lastReport.load();
    if (nowMs - last < 10000 || !counters.lastReport.compare_exchange_strong(last, nowMs)) return;
    const auto elements = counters.elements.load();
    const auto readOnly = counters.readOnly.load();
    aps5::LogErr( "[buffers] descriptor elements bound: %llu total, %llu written, %llu read-only; %llu pending-write notes skipped\n", static_cast<unsigned long long>(elements), static_cast<unsigned long long>(elements - readOnly), static_cast<unsigned long long>(readOnly), static_cast<unsigned long long>(counters.notesSkipped.load()));
}

void ShaderResources::WriteBackBuffers() {
    if (bda) bda->CheckFault();
    if (SkipWriteBack()) return;
    guestMemory.WriteBack();
}

bool ShaderResources::WritesMemory() const {
    return HoldsLease() || NeedsCompletion() || !guestMemory.Writes().empty() || std::any_of(storageWritten.begin(), storageWritten.end(), [](bool written) { return written; });
}

bool ShaderResources::ReadsOverlap(std::uint64_t address, std::size_t bytes) const {
    const auto reads = guestMemory.InPlaceReads();
    return std::any_of(reads.begin(), reads.end(), [&](const auto& range) { return address < range.second && range.first < address + bytes; });
}

std::vector<std::pair<VkImage, bool>> ShaderResources::StorageImages() const {
    std::vector<std::pair<VkImage, bool>> images;
    for (std::size_t index = 0; index < storageTextures.size(); ++index) {
        if (storageTextures[index] != nullptr) images.emplace_back(storageTextures[index]->Image(), storageWritten[index]);
    }
    for (const auto& texture : textures) {
        if (texture != nullptr && texture->StorageSource() != nullptr) images.emplace_back(texture->StorageSource()->Image(), false);
    }
    return images;
}

bool ShaderResources::ReadsImage(const StorageTexture* image) const {
    if (image == nullptr) return false;
    for (const auto& texture : textures) {
        if (texture != nullptr && texture->StorageSource() == image) return true;
    }
    return std::any_of(storageTextures.begin(), storageTextures.end(), [&](const auto& storage) { return storage.get() == image; });
}

}
