#ifdef _WIN32
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <windows.h>
#include "prx/libc/include/general/VabiMacros.hpp"

namespace {

// Reveal full map (SOS_REVEAL_FULL_MAP=1), patched into Il2cppUserAssemblies when this library loads. Offsets are module RVAs.
//
// Three rewrites and one hook, each presentation-only because of a game invariant.
//
// The map screen paints the room background, edges and doors of every cell, and a cell's MapCell.IsVisible is read only to decide whether
// the hiddenTilemap covers it, so passing a null cover tile uncovers the map while discovery, RevealedMapCoordinates and the saved map
// data keep their real values.
//
// A Region Report row takes its name and its counts from ObjectiveProgressData, and the region percentage from MapZoneItem, so the row's
// discovered state decides nothing but which of its three presentation states is shown.
//
// Every authored map marker the game hides for exploration reasons is an entry of the one MapMarkerTrackingDatabase gated by
// BooleanEvaluator_HasDiscoveredWorldPosition, which the shipped data uses nowhere else. Granting it places the marker and nothing else.
//
// Campsites and temples are different: their markers do not exist until MapFastTravelMarkerManager.AddFastTravelPoint creates them from an
// unlocked FastTravelData.FlagData. The hook places the game's own marker prefab for the locked ones and never gives it a FastTravelData,
// which is what Select, GetFastTravelMapMarker and the travel-method toggle all require before they will act on a marker.

std::uint8_t* game = nullptr;
bool revealMarkers = false;

constexpr std::uint32_t kHiddenTileLoad = 0xa9ec81;   // in MapSystem.MapTilemap.SetHiddenTileInHiddenTilemap
constexpr std::uint32_t kMapAltarCall = 0xab2090;     // in ObjectiveProgressItem.<UpdateProgressData>g__IsObjectiveDiscovered
constexpr std::uint32_t kMarkerDiscovery = 0xc1a5b4;  // in BooleanEvaluators.BooleanEvaluator_HasDiscoveredWorldPosition.EvaluateFilter
constexpr std::uint32_t kMapOpened = 0x984160;        // MenuSystem.MapMenu.OnAfterOpen

// mov rdx, qword ptr [r14 + 0x58], the hiddenTile field read, replaced by a null tile. The xor's flags are dead: the next instruction
// loads the icall pointer and the branch after it tests that pointer.
constexpr std::uint8_t kHiddenTileBytes[] = {0x49, 0x8b, 0x56, 0x58};
constexpr std::uint8_t kNullTileBytes[] = {0x31, 0xd2, 0x66, 0x90};  // xor edx, edx; nop

// call g__HasUnlockedMapAltar, whose only caller this is, replaced by its true result. mov and nop leave the flags for the test al, al
// that follows, and rbx, which the method reads next, is untouched.
constexpr std::uint8_t kMapAltarBytes[] = {0xe8, 0x5b, 0x00, 0x00, 0x00};
constexpr std::uint8_t kUnlockedBytes[] = {0xb0, 0x01, 0x0f, 0x1f, 0x00};  // mov al, 1; nop dword ptr [rax]

// The tail jump to MapUtilities.IsMapCoordinateDiscovered that is the evaluator's whole result, replaced by a true return. The epilogue
// ahead of it has already restored the stack, so returning here is what the jump would have done.
constexpr std::uint8_t kMarkerDiscoveryBytes[] = {0xe9, 0x77, 0x7c, 0xe8, 0xff};
constexpr std::uint8_t kDiscoveredBytes[] = {0xb0, 0x01, 0xc3, 0x66, 0x90};  // mov al, 1; ret; nop

struct Vector2Int { std::int32_t x, y; };
struct Vector3 { float x, y, z; };

// Game methods the hook calls. Every one takes a trailing MethodInfo* that these call sites leave null, as the game's own do.
constexpr std::uint32_t kDestroy = 0x4379de0;             // UnityEngine.Object.Destroy
constexpr std::uint32_t kPlaceTempleMarker = 0x694940;    // MapMarkerManager.PlaceFastTravelTempleMarkerAt
constexpr std::uint32_t kPlaceSavePointMarker = 0x694a30;  // MapMarkerManager.PlaceFastTravelSavePointMarkerAt
constexpr std::uint32_t kRemoveFromTile = 0x696ad0;       // MapMarkerTileAddressHandler.RemoveMarkerFromTileAddress
constexpr std::uint32_t kRemoveFromCategory = 0x696c60;   // MapMarkerManager.RemoveMarkerFromCategoryCollection
constexpr std::uint32_t kPlayerObject = 0x6c8190;         // MenuUtilities.GetPlayer1GameObjectForUI
constexpr std::uint32_t kFlagsState = 0x8c06d0;           // RuntimeData.PlayerRuntimeDataUtils.TryGetFlagsState
constexpr std::uint32_t kHasFlagTrue = 0x8c5c10;          // PersistentData.PlayerFlagsState.HasFlagTrue
constexpr std::uint32_t kHasMap = 0x981aa0;               // MenuSystem.MapMenu.get_HasMap
constexpr std::uint32_t kTravelPosition = 0xa8d700;       // MapSystem.MapFastTravelMarkerManager.GetFastTravelPosition
constexpr std::uint32_t kFlagId = 0xb690c0;               // DataConfigs.FlagData.get_flagID, which is the asset name

constexpr std::size_t kMenuMarkerManager = 0x140, kMenuTravelManager = 0x150;
constexpr std::size_t kMarkerManagerTileHandler = 0x20, kMarkerManagerTravelHandler = 0x28;
constexpr std::size_t kTravelManagerCollection = 0x38;
constexpr std::size_t kTravelCollectionPoints = 0x18;
constexpr std::size_t kTravelHandlerMarkers = 0x10;
constexpr std::size_t kTravelDataFlag = 0x30, kTravelDataDestination = 0x48;
constexpr std::size_t kMarkerCoordinate = 0x24, kMarkerTravelData = 0xa8;
constexpr std::size_t kListItems = 0x10, kListSize = 0x18, kArrayLength = 0x18, kArrayData = 0x20;
constexpr std::int32_t kTempleDestination = 0;  // MapSystem.FastTravel.FastTravelDestinationType.Temple

using GetObject = void*(APS5_VABI*)(void* instance, const void* method);
using GetStatic = void*(APS5_VABI*)(const void* method);
using GetBool = bool(APS5_VABI*)(void* instance, const void* method);
using HasFlag = bool(APS5_VABI*)(void* flagsState, void* flagId, const void* method);
using TravelPosition = Vector3(APS5_VABI*)(void* travelManager, void* travelData, const void* method);
using PlaceMarker = void*(APS5_VABI*)(void* markerManager, Vector3 mapPosition, const void* method);
using RemoveFromTile = void(APS5_VABI*)(void* tileHandler, Vector2Int tileAddress, void* marker, const void* method);
using RemoveFromCategory = void(APS5_VABI*)(void* markerManager, void* marker, const void* method);
using Destroy = void(APS5_VABI*)(void* object, const void* method);

template <typename T> T& Field(void* object, std::size_t offset) { return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset); }
template <typename T> T Read(void* object, std::size_t offset) { return object != nullptr ? Field<T>(object, offset) : T{}; }
template <typename Function> Function Game(std::uint32_t offset) { return reinterpret_cast<Function>(game + offset); }

// One visual marker per locked fast travel point, kept only for the map the current MapMarkerManager belongs to.
struct Visual {
    void* travelData;
    void* object;
    void* marker;
};
constexpr std::size_t kMaxVisuals = 96;
Visual visuals[kMaxVisuals] = {};
std::size_t visualCount = 0;
void* visualOwner = nullptr;

void* ListItem(void* list, std::int32_t index) {
    void* const items = Read<void*>(list, kListItems);
    if (items == nullptr || index < 0 || static_cast<std::uint64_t>(index) >= Field<std::uint64_t>(items, kArrayLength)) return nullptr;
    return Field<void*>(items, kArrayData + static_cast<std::size_t>(index) * sizeof(void*));
}

Visual* FindVisual(void* travelData) {
    for (std::size_t i = 0; i < visualCount; ++i) {
        if (visuals[i].travelData == travelData) return &visuals[i];
    }
    return nullptr;
}

// The marker PlaceFastTravel*MarkerAt just handed to FastTravelMapMarkerHandler.TrackFastTravelMarker is the last of its collection, and
// a visual one is recognisable by its missing FastTravelData.
void* TrackedVisualMarker(void* markerManager) {
    void* const list = Read<void*>(Read<void*>(markerManager, kMarkerManagerTravelHandler), kTravelHandlerMarkers);
    void* const marker = ListItem(list, Read<std::int32_t>(list, kListSize) - 1);
    return marker != nullptr && Field<void*>(marker, kMarkerTravelData) == nullptr ? marker : nullptr;
}

void ShowLockedPoint(void* markerManager, void* travelManager, void* travelData) {
    if (visualCount >= kMaxVisuals) return;
    const Vector3 position = Game<TravelPosition>(kTravelPosition)(travelManager, travelData, nullptr);
    const std::uint32_t place = Field<std::int32_t>(travelData, kTravelDataDestination) == kTempleDestination ? kPlaceTempleMarker : kPlaceSavePointMarker;
    void* const object = Game<PlaceMarker>(place)(markerManager, position, nullptr);
    if (object == nullptr) return;
    visuals[visualCount++] = Visual{travelData, object, TrackedVisualMarker(markerManager)};
}

// The game's own removal, as it deletes a custom marker: unregister from the tile address and category collections, then destroy.
void HideVisualPoint(void* markerManager, Visual& entry) {
    if (entry.marker == nullptr) return;  // nothing to unregister with, so the marker is left where it is rather than dangling
    void* const tileHandler = Read<void*>(markerManager, kMarkerManagerTileHandler);
    if (tileHandler != nullptr) {
        Game<RemoveFromTile>(kRemoveFromTile)(tileHandler, Field<Vector2Int>(entry.marker, kMarkerCoordinate), entry.marker, nullptr);
    }
    Game<RemoveFromCategory>(kRemoveFromCategory)(markerManager, entry.marker, nullptr);
    if (entry.object != nullptr) Game<Destroy>(kDestroy)(entry.object, nullptr);
    entry.travelData = nullptr;
    entry.object = nullptr;
    entry.marker = nullptr;
}

void APS5_VABI OnMapOpened(void* mapMenu) {
    if (!revealMarkers || mapMenu == nullptr) return;
    void* const markerManager = Field<void*>(mapMenu, kMenuMarkerManager);
    void* const travelManager = Field<void*>(mapMenu, kMenuTravelManager);
    if (markerManager == nullptr || travelManager == nullptr) return;
    if (!Game<GetBool>(kHasMap)(mapMenu, nullptr)) return;
    void* const player = Game<GetStatic>(kPlayerObject)(nullptr);
    void* const flags = player != nullptr ? Game<GetObject>(kFlagsState)(player, nullptr) : nullptr;
    if (flags == nullptr) return;
    void* const points = Read<void*>(Read<void*>(travelManager, kTravelManagerCollection), kTravelCollectionPoints);
    const std::int32_t count = Read<std::int32_t>(points, kListSize);
    if (count <= 0) return;
    if (markerManager != visualOwner) {  // a new map brought new markers, so the old entries are gone with it
        visualOwner = markerManager;
        visualCount = 0;
        std::memset(visuals, 0, sizeof(visuals));
    }
    for (std::int32_t i = 0; i < count; ++i) {
        void* const travelData = ListItem(points, i);
        void* const flag = travelData != nullptr ? Field<void*>(travelData, kTravelDataFlag) : nullptr;
        void* const flagId = flag != nullptr ? Game<GetObject>(kFlagId)(flag, nullptr) : nullptr;
        if (flagId == nullptr) continue;
        Visual* const entry = FindVisual(travelData);
        if (Game<HasFlag>(kHasFlagTrue)(flags, flagId, nullptr)) {
            if (entry != nullptr) HideVisualPoint(markerManager, *entry);
        } else if (entry == nullptr) {
            ShowLockedPoint(markerManager, travelManager, travelData);
        }
    }
}

struct Patch {
    std::uint32_t site;
    const std::uint8_t* original;
    const std::uint8_t* replacement;
    std::size_t size;
};

template <std::size_t size>
constexpr Patch Rewrite(std::uint32_t site, const std::uint8_t (&original)[size], const std::uint8_t (&replacement)[size]) {
    return Patch{site, original, replacement, size};
}

constexpr Patch kPatches[] = {
    Rewrite(kHiddenTileLoad, kHiddenTileBytes, kNullTileBytes),
    Rewrite(kMapAltarCall, kMapAltarBytes, kUnlockedBytes),
    Rewrite(kMarkerDiscovery, kMarkerDiscoveryBytes, kDiscoveredBytes),
};

struct Range {
    std::uint32_t begin, end;
    std::uint64_t hash;
};

// FNV-1a of each patched instruction with the control flow around it, and of the two methods the hook calls and the one whose refusal of a
// marker without FastTravelData keeps those markers out of fast travel. Only what the feature depends on is hashed, so unrelated code
// cannot make the whole feature unavailable.
constexpr Range kCode[] = {
    {kPlaceTempleMarker, 0x694b20, 0x2cec2fed698ca43aull},  // PlaceFastTravelTempleMarkerAt and PlaceFastTravelSavePointMarkerAt
    {kMapOpened, 0x984180, 0xcdc102da1fa6f413ull},          // the prologue the hook replaces and copies
    {0xa9eb30, 0xa9ed70, 0x639f2883510e799bull},            // MapTilemap.SetHiddenTileInHiddenTilemap
    {0xab1ff0, 0xab21d0, 0x15b779355f79ed82ull},            // g__IsObjectiveDiscovered and g__HasUnlockedMapAltar
    {0xad61b0, 0xad63a0, 0x6435a37c0726d6cdull},            // FastTravelMapMarker.Select
    {0xc1a4c0, 0xc1a5c0, 0x3f5a52e9c926600aull},            // HasDiscoveredWorldPosition.EvaluateFilter
};
constexpr std::uint64_t kCodeHash = 0xcf0b1cc239ce2322ull;

std::uint64_t Hash(std::uint32_t begin, std::uint32_t end, std::uint64_t seed) {
    for (std::uint32_t offset = begin; offset < end; ++offset) seed = (seed ^ game[offset]) * 0x100000001b3;
    return seed;
}

bool Recognised(std::uint32_t imageSize) {
    std::uint64_t hash = 0xcbf29ce484222325;
    for (const Range& range : kCode) {
        if (range.end > imageSize) return false;
        hash = Hash(range.begin, range.end, hash);
    }
    return hash == kCodeHash;
}

bool Apply(const Patch& patch, bool undo) {
    std::uint8_t* at = game + patch.site;
    const std::uint8_t* from = undo ? patch.replacement : patch.original;
    const std::uint8_t* to = undo ? patch.original : patch.replacement;
    if (std::memcmp(at, from, patch.size) != 0) return false;
    DWORD protection = 0;
    if (!VirtualProtect(at, patch.size, PAGE_EXECUTE_READWRITE, &protection)) return false;
    std::memcpy(at, to, patch.size);
    VirtualProtect(at, patch.size, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), at, patch.size);
    return true;
}

// Entry hook: the method starts with a jump to a stub that calls the handler with the argument registers kept, then runs the method's
// copied first instructions and continues it. The stub is allocated near the module so a five-byte relative jump reaches it.
constexpr std::uint8_t kStub[] = {
    0x57, 0x56, 0x52, 0x51,              // push rdi; push rsi; push rdx; push rcx
    0x41, 0x50, 0x41, 0x51,              // push r8; push r9
    0x48, 0x83, 0xec, 0x08,              // sub rsp, 8, which leaves rsp aligned for the call
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax, handler (offset 14)
    0xff, 0xd0,                          // call rax
    0x48, 0x83, 0xc4, 0x08,              // add rsp, 8
    0x41, 0x59, 0x41, 0x58,              // pop r9; pop r8
    0x59, 0x5a, 0x5e, 0x5f,              // pop rcx; pop rdx; pop rsi; pop rdi
};
constexpr std::size_t kStubHandler = 14;
constexpr std::uint8_t kJumpBack[] = {0xff, 0x25, 0, 0, 0, 0};  // jmp qword ptr [rip], followed by the address
// push rbp; mov rbp, rsp; push r15: the first whole instructions of the hooked method, none rip-relative and none jumped to.
constexpr std::uint8_t kPrologueBytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
constexpr std::size_t kPrologue = sizeof(kPrologueBytes);
constexpr std::size_t kEntryJump = 5;  // jmp rel32
static_assert(kPrologue >= kEntryJump);

std::uint8_t* stub = nullptr;

// An executable block within 2 GB below the module, so the relative jump reaches it.
std::uint8_t* ReserveStub(std::size_t size) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::uintptr_t step = system.dwAllocationGranularity;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(game) & ~(step - 1);
    for (std::uintptr_t distance = step; distance < 0x40000000 && distance < base; distance += step) {
        auto* block = static_cast<std::uint8_t*>(
            VirtualAlloc(reinterpret_cast<void*>(base - distance), (size + step - 1) & ~(step - 1), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (block != nullptr) return block;
    }
    return nullptr;
}

bool Hook(std::uint32_t method, const void* handler) {
    std::uint8_t* const at = game + method;
    if (std::memcmp(at, kPrologueBytes, kPrologue) != 0) return false;
    const std::size_t size = sizeof(kStub) + kPrologue + sizeof(kJumpBack) + sizeof(void*);
    stub = ReserveStub(size);
    if (stub == nullptr) return false;
    const void* const resume = at + kPrologue;
    const std::int64_t relative = stub - (at + kEntryJump);
    if (relative < -0x7fffffff || relative > 0x7fffffff) return false;
    std::memcpy(stub, kStub, sizeof(kStub));
    std::memcpy(stub + kStubHandler, &handler, sizeof(handler));
    std::memcpy(stub + sizeof(kStub), kPrologueBytes, kPrologue);
    std::memcpy(stub + sizeof(kStub) + kPrologue, kJumpBack, sizeof(kJumpBack));
    std::memcpy(stub + sizeof(kStub) + kPrologue + sizeof(kJumpBack), &resume, sizeof(resume));
    std::uint8_t jump[kPrologue];
    std::memset(jump, 0x90, sizeof(jump));
    jump[0] = 0xe9;
    const auto displacement = static_cast<std::int32_t>(relative);
    std::memcpy(jump + 1, &displacement, sizeof(displacement));
    DWORD protection = 0;
    if (!VirtualProtect(at, sizeof(jump), PAGE_EXECUTE_READWRITE, &protection)) return false;
    std::memcpy(at, jump, sizeof(jump));
    VirtualProtect(at, sizeof(jump), protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), stub, size);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(jump));
    return true;
}

// DEBUG_SAULO: temporary startup diagnostics, to be removed once the real game reports the feature installing.
void ReportRecognition(std::uint32_t imageSize) {
    std::fprintf(stderr, "[DEBUG_SAULO][RevealFullMap] base=%p imageSize=0x%x\n", static_cast<const void*>(game), imageSize);
    std::uint64_t hash = 0xcbf29ce484222325;
    for (std::size_t i = 0; i < std::size(kCode); ++i) {
        const Range& range = kCode[i];
        if (range.end > imageSize) {
            std::fprintf(stderr, "[DEBUG_SAULO][RevealFullMap][FP] range=%zu rva=0x%x OUTSIDE IMAGE\n", i, range.begin);
            continue;
        }
        const std::uint64_t actual = Hash(range.begin, range.end, 0xcbf29ce484222325);
        hash = Hash(range.begin, range.end, hash);
        std::fprintf(stderr, "[DEBUG_SAULO][RevealFullMap][FP] range=%zu rva=0x%x size=0x%x expected=0x%llx actual=0x%llx %s\n", i,
                     range.begin, range.end - range.begin, static_cast<unsigned long long>(range.hash),
                     static_cast<unsigned long long>(actual), actual == range.hash ? "MATCH" : "MISMATCH");
        if (actual == range.hash) continue;
        const std::uint8_t* head = game + range.begin;
        std::fprintf(stderr, "[DEBUG_SAULO][RevealFullMap][FP]   head: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                     head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7], head[8], head[9], head[10], head[11], head[12],
                     head[13], head[14], head[15]);
        // 32-bit hash of each 64-byte chunk, so the divergence can be located without printing the whole range.
        for (std::uint32_t chunk = 0; chunk < range.end - range.begin; chunk += 512) {
            char line[160] = {};
            int used = 0;
            for (std::uint32_t at = chunk; at < chunk + 512 && at < range.end - range.begin; at += 64) {
                const std::uint32_t size = range.end - range.begin - at < 64 ? range.end - range.begin - at : 64;
                const auto value = static_cast<std::uint32_t>(Hash(range.begin + at, range.begin + at + size, 0xcbf29ce484222325));
                used += std::snprintf(line + used, sizeof(line) - static_cast<std::size_t>(used), "%08x ", value);
            }
            std::fprintf(stderr, "[DEBUG_SAULO][RevealFullMap][FP]   +0x%03x chunks: %s\n", chunk, line);
        }
    }
    std::fprintf(stderr, "[DEBUG_SAULO][RevealFullMap] fingerprint expected=0x%llx actual=0x%llx %s\n",
                 static_cast<unsigned long long>(kCodeHash), static_cast<unsigned long long>(hash),
                 hash == kCodeHash ? "MATCH" : "MISMATCH");
    const char* const names[] = {"A", "B", "C"};
    for (std::size_t i = 0; i < std::size(kPatches); ++i) {
        const Patch& patch = kPatches[i];
        char expected[24] = {}, actual[24] = {};
        for (std::size_t k = 0; k < patch.size; ++k) {
            std::snprintf(expected + 3 * k, 4, "%02x ", patch.original[k]);
            std::snprintf(actual + 3 * k, 4, "%02x ", game[patch.site + k]);
        }
        std::fprintf(stderr, "[DEBUG_SAULO][RevealFullMap][SITE] %s rva=0x%x expected=%s actual=%s %s\n", names[i], patch.site, expected, actual,
                     std::memcmp(game + patch.site, patch.original, patch.size) == 0 ? "MATCH" : "MISMATCH");
    }
    char expected[24] = {}, actual[24] = {};
    for (std::size_t k = 0; k < kPrologue; ++k) {
        std::snprintf(expected + 3 * k, 4, "%02x ", kPrologueBytes[k]);
        std::snprintf(actual + 3 * k, 4, "%02x ", game[kMapOpened + k]);
    }
    std::fprintf(stderr, "[DEBUG_SAULO][RevealFullMap][SITE] D rva=0x%x expected=%s actual=%s %s\n", kMapOpened, expected, actual,
                 std::memcmp(game + kMapOpened, kPrologueBytes, kPrologue) == 0 ? "MATCH" : "MISMATCH");
}

// Every rewrite and the hook, or none, so the option never leaves the map half revealed.
bool Install() {
    for (std::size_t i = 0; i < std::size(kPatches); ++i) {
        if (Apply(kPatches[i], false)) continue;
        while (i-- > 0) Apply(kPatches[i], true);
        return false;
    }
    if (Hook(kMapOpened, reinterpret_cast<const void*>(&OnMapOpened))) {
        revealMarkers = true;
        return true;
    }
    for (std::size_t i = std::size(kPatches); i-- > 0;) Apply(kPatches[i], true);
    if (stub != nullptr) VirtualFree(stub, 0, MEM_RELEASE);
    return false;
}

// Nothing is patched while the option is off, so the game keeps its own map, region reports and markers exactly.
bool StartRevealFullMap() {
    const char* option = std::getenv("SOS_REVEAL_FULL_MAP");
    if (option == nullptr || std::strcmp(option, "1") != 0) return false;
    game = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(L"Il2cppUserAssemblies.prx.guest.prx"));
    const auto* headers =
        game != nullptr ? reinterpret_cast<const IMAGE_NT_HEADERS*>(game + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew) : nullptr;
    if (headers == nullptr) {
        std::fprintf(stderr, "Reveal Full Map: unavailable (Il2cppUserAssemblies is not loaded)\n");
        return false;
    }
    ReportRecognition(headers->OptionalHeader.SizeOfImage);  // DEBUG_SAULO
    if (!Recognised(headers->OptionalHeader.SizeOfImage)) {
        std::fprintf(stderr, "Reveal Full Map: unavailable (map code not recognised)\n");
        return false;
    }
    if (!Install()) {
        std::fprintf(stderr, "Reveal Full Map: unavailable (map code could not be patched)\n");
        return false;
    }
    std::fprintf(stderr, "Reveal Full Map: patch applied\n");
    return true;
}

[[maybe_unused]] const bool revealFullMapStarted = StartRevealFullMap();

}
#endif
