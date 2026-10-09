#ifdef _WIN32
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <windows.h>
#include "prx/libc/include/general/VabiMacros.hpp"

namespace {

// Reveal full map (SOS_REVEAL_FULL_MAP=1), patched into Il2cppUserAssemblies when this library loads. Offsets are module RVAs. Each patch
// only changes what is shown, because what it changes decides nothing else: the fog's cover tile, a Region Report row's discovered state,
// and HasDiscoveredWorldPosition, which only map markers use. The markers added for locked fast travel points have no FastTravelData,
// which every travel action requires.

std::uint8_t* game = nullptr;
bool revealMarkers = false;

constexpr std::uint32_t kHiddenTileLoad = 0xa9ec81;
constexpr std::uint32_t kMapAltarCall = 0xab2090;
constexpr std::uint32_t kMarkerDiscovery = 0xc1a5b4;
constexpr std::uint32_t kMapOpened = 0x984160;

// mov rdx, qword ptr [r14 + 0x58], the hiddenTile read, replaced by a null tile. The xor's flags are dead: the branch after the next
// instruction tests the pointer that instruction loads.
constexpr std::uint8_t kHiddenTileBytes[] = {0x49, 0x8b, 0x56, 0x58};
constexpr std::uint8_t kNullTileBytes[] = {0x31, 0xd2, 0x66, 0x90};  // xor edx, edx; nop

// call g__HasUnlockedMapAltar, whose only caller this is, replaced by its true result; the test al, al that follows sets the flags.
constexpr std::uint8_t kMapAltarBytes[] = {0xe8, 0x5b, 0x00, 0x00, 0x00};
constexpr std::uint8_t kUnlockedBytes[] = {0xb0, 0x01, 0x0f, 0x1f, 0x00};  // mov al, 1; nop dword ptr [rax]

// The tail jump to MapUtilities.IsMapCoordinateDiscovered that is the evaluator's whole result, replaced by a true return. The epilogue
// ahead of it has already restored the stack, so returning here is what the jump would have done.
constexpr std::uint8_t kMarkerDiscoveryBytes[] = {0xe9, 0x77, 0x7c, 0xe8, 0xff};
constexpr std::uint8_t kDiscoveredBytes[] = {0xb0, 0x01, 0xc3, 0x66, 0x90};  // mov al, 1; ret; nop

struct Vector2Int { std::int32_t x, y; };
struct Vector3 { float x, y, z; };

// Game methods the hook calls. Every one takes a trailing MethodInfo* that these call sites leave null, as the game's own do.
constexpr std::uint32_t kDestroy = 0x4379de0;
constexpr std::uint32_t kPlaceTempleMarker = 0x694940;
constexpr std::uint32_t kPlaceSavePointMarker = 0x694a30;
constexpr std::uint32_t kRemoveFromTile = 0x696ad0;
constexpr std::uint32_t kRemoveFromCategory = 0x696c60;
constexpr std::uint32_t kPlayerObject = 0x6c8190;
constexpr std::uint32_t kFlagsState = 0x8c06d0;
constexpr std::uint32_t kHasFlagTrue = 0x8c5c10;
constexpr std::uint32_t kHasMap = 0x981aa0;
constexpr std::uint32_t kTravelPosition = 0xa8d700;
constexpr std::uint32_t kFlagId = 0xb690c0;

constexpr std::size_t kMenuMarkerManager = 0x140, kMenuTravelManager = 0x150;
constexpr std::size_t kMarkerManagerTileHandler = 0x20, kMarkerManagerTravelHandler = 0x28;
constexpr std::size_t kTravelManagerCollection = 0x38;
constexpr std::size_t kTravelCollectionPoints = 0x18;
constexpr std::size_t kTravelHandlerMarkers = 0x10;
constexpr std::size_t kTravelDataFlag = 0x30, kTravelDataDestination = 0x48;
constexpr std::size_t kMarkerCoordinate = 0x24, kMarkerTravelData = 0xa8;
constexpr std::size_t kListItems = 0x10, kListSize = 0x18, kArrayLength = 0x18, kArrayData = 0x20;
constexpr std::int32_t kTempleDestination = 0;

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

// The game's own removal of a custom marker: unregister from the tile address and category collections, then destroy.
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
};

// FNV-1a of only what the feature depends on: each patched instruction with the control flow around it, the methods the hook calls, and
// Select, whose refusal of a marker without FastTravelData keeps those markers out of fast travel.
constexpr Range kCode[] = {
    {kPlaceTempleMarker, 0x694b20},  // PlaceFastTravelTempleMarkerAt and PlaceFastTravelSavePointMarkerAt
    {kMapOpened, 0x984180},          // the prologue the hook replaces and copies
    {0xa9eb30, 0xa9ed70},            // MapTilemap.SetHiddenTileInHiddenTilemap
    {0xab1ff0, 0xab21d0},            // g__IsObjectiveDiscovered and g__HasUnlockedMapAltar
    {0xad61b0, 0xad63a0},            // FastTravelMapMarker.Select
    {0xc1a4c0, 0xc1a5c0},            // HasDiscoveredWorldPosition.EvaluateFilter
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

// A block within 2 GB below the module, so the relative jump reaches it. It is writable only until the stub is built.
std::uint8_t* ReserveStub(std::size_t size) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::uintptr_t step = system.dwAllocationGranularity;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(game) & ~(step - 1);
    for (std::uintptr_t distance = step; distance < 0x40000000 && distance < base; distance += step) {
        auto* block = static_cast<std::uint8_t*>(
            VirtualAlloc(reinterpret_cast<void*>(base - distance), (size + step - 1) & ~(step - 1), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (block != nullptr) return block;
    }
    return nullptr;
}

// The stub is made executable before the method jumps to it, and is kept for the process: a thread can be inside it at any time.
bool Hook(std::uint32_t method, const void* handler) {
    std::uint8_t* const at = game + method;
    if (std::memcmp(at, kPrologueBytes, kPrologue) != 0) return false;
    const std::size_t size = sizeof(kStub) + kPrologue + sizeof(kJumpBack) + sizeof(void*);
    std::uint8_t* const block = ReserveStub(size);
    if (block == nullptr) return false;
    const void* const resume = at + kPrologue;
    const std::int64_t relative = block - (at + kEntryJump);
    DWORD protection = 0;
    if (relative >= -0x7fffffff && relative <= 0x7fffffff) {
        std::memcpy(block, kStub, sizeof(kStub));
        std::memcpy(block + kStubHandler, &handler, sizeof(handler));
        std::memcpy(block + sizeof(kStub), kPrologueBytes, kPrologue);
        std::memcpy(block + sizeof(kStub) + kPrologue, kJumpBack, sizeof(kJumpBack));
        std::memcpy(block + sizeof(kStub) + kPrologue + sizeof(kJumpBack), &resume, sizeof(resume));
        std::uint8_t jump[kPrologue];
        std::memset(jump, 0x90, sizeof(jump));
        jump[0] = 0xe9;
        const auto displacement = static_cast<std::int32_t>(relative);
        std::memcpy(jump + 1, &displacement, sizeof(displacement));
        if (VirtualProtect(block, size, PAGE_EXECUTE_READ, &protection) && FlushInstructionCache(GetCurrentProcess(), block, size) &&
            VirtualProtect(at, sizeof(jump), PAGE_EXECUTE_READWRITE, &protection)) {
            std::memcpy(at, jump, sizeof(jump));
            VirtualProtect(at, sizeof(jump), protection, &protection);
            FlushInstructionCache(GetCurrentProcess(), at, sizeof(jump));
            stub = block;
            return true;
        }
    }
    VirtualFree(block, 0, MEM_RELEASE);
    return false;
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
