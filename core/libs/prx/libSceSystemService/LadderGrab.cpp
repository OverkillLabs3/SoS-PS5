#ifdef _WIN32
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <windows.h>
#include "prx/libc/include/general/VabiMacros.hpp"

namespace {

// Grab ladders with Up (SOS_LADDER_GRAB_WITH_UP=1). The game's climbable mount transitions choose between an interact press and pointing
// the stick up with BooleanEvaluator_SelectEvaluatorByBool on the Traversal Inputs setting; this accepts the stick-up branch where the game
// refused it, and the climbable the transition then looks for is narrowed to ladders. Ropes, Down and everything else about the transition,
// including where the player must stand, stay with the game. Patched into Il2cppUserAssemblies when this library loads; offsets are module
// RVAs.

std::uint8_t* game = nullptr;

constexpr std::uint32_t kCheckCircle = 0x88f140;            // PropScripts.ClimbableVolume.CheckCircleForClimbableVolume
constexpr std::uint32_t kPassesFilter = 0xc142d0;           // BooleanEvaluator.PassesFilter, which applies the evaluator's negate
constexpr std::uint32_t kClimbableAheadCall = 0xc1cceb;     // the CheckCircleForClimbableVolume call in InFrontOfClimbableVolume
constexpr std::uint32_t kIntComparisonEvaluate = 0xc1f340;  // slot 4 of BooleanEvaluator_IntComparison
constexpr std::uint32_t kGeneralSettingValue = 0xb5fee0;    // slot 4 of DynamicInt_GeneralSetting
constexpr std::uint32_t kSelectEvaluate = 0xc28b00;         // BooleanEvaluator_SelectEvaluatorByBool.EvaluateFilter
constexpr std::uint32_t kCheckInputs = 0xc28d40;            // slot 5 of BooleanEvaluator_SimpleInputCheck

// Il2CppClass vtable: the method pointer of slot n is at 0x138 + 16n.
constexpr std::size_t kSlot4 = 0x178, kSlot5 = 0x188;
constexpr std::size_t kCondition = 0x18, kButtonBranch = 0x20, kStickBranch = 0x28;  // SelectEvaluatorByBool.boolean, .ifTrue, .ifFalse
constexpr std::size_t kComparedValue = 0x18;                                        // BooleanEvaluator_IntComparison.intA
constexpr std::size_t kSetting = 0x10;                                              // DynamicInt_GeneralSetting.setting
constexpr std::size_t kInputRules = 0x18;                                           // SimpleInputCheck.inputCheckRules
constexpr std::size_t kListItems = 0x10, kListSize = 0x18, kArrayData = 0x20;
constexpr std::size_t kRuleCategory = 0x10, kRuleMinUp = 0x20;  // InputCheckRule.checkCategory, .minMoveInputThreshold.y
constexpr std::size_t kClimbableType = 0x28;                    // PropScripts.ClimbableVolume.climbableType

constexpr std::int32_t kTraversalInputs = 79;    // Settings.GeneralIntSettings.TraversalInputs
constexpr std::int32_t kMovementThreshold = 1;   // InputCheckCategory.inputMovementThreshold
constexpr std::int32_t kLadder = 1;              // ClimbableVolumeType.ladder

struct Vector2 { float x, y; };
using Evaluate = bool(APS5_VABI*)(void* evaluator, void* argument, const void* method);
using CheckCircle = void*(APS5_VABI*)(Vector2 position, float radius, const void* method);

// Set when the selector below accepted Up where the game would have refused, and taken by the climbable lookup of the very next condition
// of the same transition: Boolean_LOGICAL_AND_Multi evaluates its conditions in order and stops at the first that fails, and in both mount
// transitions the climbable check is the condition right after the input selector. Both run on the game thread.
bool grantedUp = false;

template <typename T> T& Field(void* object, std::size_t offset) { return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset); }
template <typename Function> Function Game(std::uint32_t offset) { return reinterpret_cast<Function>(game + offset); }

bool Passes(void* evaluator, void* argument) {
    return evaluator != nullptr && Game<Evaluate>(kPassesFilter)(evaluator, argument, nullptr);
}

// Whether the object's class holds that game method in the vtable slot, which names the class because nothing derives from the classes
// checked here.
bool Implements(void* object, std::size_t slot, std::uint32_t method) {
    void* const type = object != nullptr ? Field<void*>(object, 0) : nullptr;
    return type != nullptr && Field<void*>(type, slot) == game + method;
}

// An input check that is nothing but one rule asking for the stick to point up. The transitions for climbing down and for dropping through
// a platform are selected on the same setting, and their stick branches ask for it to point down.
bool AsksForUp(void* inputCheck) {
    if (!Implements(inputCheck, kSlot5, kCheckInputs)) return false;
    void* const rules = Field<void*>(inputCheck, kInputRules);
    if (rules == nullptr || Field<std::int32_t>(rules, kListSize) != 1) return false;
    void* const items = Field<void*>(rules, kListItems);
    void* const rule = items != nullptr ? Field<void*>(items, kArrayData) : nullptr;
    return rule != nullptr && Field<std::int32_t>(rule, kRuleCategory) == kMovementThreshold && Field<float>(rule, kRuleMinUp) > 0.0f;
}

bool UpGrabSelector(void* select) {
    void* const condition = Field<void*>(select, kCondition);
    if (!Implements(condition, kSlot4, kIntComparisonEvaluate)) return false;
    void* const setting = Field<void*>(condition, kComparedValue);
    if (!Implements(setting, kSlot4, kGeneralSettingValue) || Field<std::int32_t>(setting, kSetting) != kTraversalInputs) return false;
    return AsksForUp(Field<void*>(select, kStickBranch));
}

// Entry hook of the selector's EvaluateFilter: a true return leaves the method before it chooses a branch. Only an input the game refuses
// is granted, so with Traversal Inputs already on the stick scheme, and whenever the interact press is accepted, the game decides alone.
bool APS5_VABI GrabsWithUp(void* select, void* argument) {
    grantedUp = false;
    if (!UpGrabSelector(select)) return false;
    void* const branch = Field<void*>(select, Passes(Field<void*>(select, kCondition), argument) ? kButtonBranch : kStickBranch);
    if (Passes(branch, argument) || !Passes(Field<void*>(select, kStickBranch), argument)) return false;
    grantedUp = true;
    return true;
}

// Replaces the climbable lookup of BooleanEvaluator_InFrontOfClimbableVolume. A mount granted to Up sees ladders only; every other
// evaluation, and so every rope, gets the game's own result. Returning null is what the evaluator reads as nothing to climb.
void* APS5_VABI ClimbableAhead(Vector2 position, float radius, const void* method) {
    void* const volume = Game<CheckCircle>(kCheckCircle)(position, radius, method);
    if (!grantedUp) return volume;
    grantedUp = false;
    return volume != nullptr && Field<std::int32_t>(volume, kClimbableType) == kLadder ? volume : nullptr;
}

// The method starts with mov rax, trampoline; jmp rax. The trampoline calls GrabsWithUp(rdi, rsi) with the arguments kept and returns from
// the method with al set when it passes; otherwise the method's copied first instructions run and a jump that changes no register or flag
// continues it. The three pushes leave rsp aligned for the call.
constexpr std::uint8_t kTrampolineStart[] = {
    0x57, 0x56, 0x52,                    // push rdi; push rsi; push rdx
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax, GrabsWithUp (offset 5)
    0xff, 0xd0,                          // call rax
    0x5a, 0x5e, 0x5f,                    // pop rdx; pop rsi; pop rdi
    0x84, 0xc0,                          // test al, al
    0x74, 0x01,                          // je, to the method's first instructions
    0xc3,                                // ret
};
constexpr std::size_t kTrampolineHandler = 5;
constexpr std::uint8_t kJumpBack[] = {0xff, 0x25, 0, 0, 0, 0};  // jmp qword ptr [rip], followed by the address
constexpr std::size_t kEntryJump = 12;  // mov rax, trampoline; jmp rax
// push rbp; mov rbp, rsp; push r15; push r14; push rbx; push rax; mov r15, [rdi + 0x18]: whole instructions, none rip-relative and none
// jumped to from the rest of the method.
constexpr std::size_t kPrologue = 14;
static_assert(kPrologue >= kEntryJump);
// call ClimbableVolume.CheckCircleForClimbableVolume, with the position in xmm0 and the radius in xmm1 already in place.
constexpr std::uint8_t kClimbableAheadCallBytes[] = {0xe8, 0x50, 0x24, 0xc7, 0xff};

struct Range { std::uint32_t begin, end; };

// FNV-1a of the code the offsets, vtable slots and the order the conditions are evaluated in were read from, so the patch only applies to
// the analysed game build.
constexpr Range kCode[] = {
    {kCheckCircle, 0x88f470},
    {kGeneralSettingValue, 0xb5ff40},
    {kPassesFilter, 0xc14300},
    {0xc1cb40, 0xc1cdb0},  // BooleanEvaluator_InFrontOfClimbableVolume.EvaluateFilter
    {kIntComparisonEvaluate, 0xc1f400},
    {0xc27b10, 0xc27c90},  // BooleanEvaluator_PlayerInputCheck.EvaluateFilter
    {kSelectEvaluate, 0xc28b80},
    {kCheckInputs, 0xc29020},
    {0xc2a330, 0xc2a3d0},  // Boolean_LOGICAL_AND_Multi.EvaluateFilter
    {0xc41ce0, 0xc41f50},  // ClimbingContextAction.SelectVariant, the game's own ladder test
    {0xe8e3b0, 0xe8e520},  // InputCheckRule.PassesInputChecks
    {0xe8ea00, 0xe8ea80},  // InputCheckRule.PassesMovementInputThresholdCheck
};
constexpr std::uint64_t kCodeHash = 0xc20765989a3d1295;

bool Recognised(std::uint32_t imageSize) {
    std::uint64_t hash = 0xcbf29ce484222325;
    for (const Range& range : kCode) {
        if (range.end > imageSize) return false;
        for (std::uint32_t offset = range.begin; offset < range.end; ++offset) hash = (hash ^ game[offset]) * 0x100000001b3;
    }
    return hash == kCodeHash;
}

template <std::size_t size> bool Rewrite(std::uint32_t site, const std::uint8_t (&original)[size], const std::uint8_t (&code)[size]) {
    std::uint8_t* at = game + site;
    if (std::memcmp(at, original, size) != 0) return false;
    DWORD protection = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &protection)) return false;
    std::memcpy(at, code, size);
    VirtualProtect(at, size, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

// A rel32 call reaches only 2 GB, so the rewritten call goes through a jump allocated below the game module.
std::uint8_t* NearJump(std::uintptr_t target) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::uintptr_t step = system.dwAllocationGranularity, base = reinterpret_cast<std::uintptr_t>(game) & ~(step - 1);
    for (std::uintptr_t distance = step; distance < 0x40000000 && distance < base; distance += step) {
        auto* jump = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(base - distance), step, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (jump == nullptr) continue;
        std::memcpy(jump, kJumpBack, sizeof(kJumpBack));
        std::memcpy(jump + sizeof(kJumpBack), &target, sizeof(target));
        DWORD protection = 0;
        if (VirtualProtect(jump, step, PAGE_EXECUTE_READ, &protection)) {
            FlushInstructionCache(GetCurrentProcess(), jump, sizeof(kJumpBack) + sizeof(target));
            return jump;
        }
        VirtualFree(jump, 0, MEM_RELEASE);
        return nullptr;
    }
    return nullptr;
}

// Rewrites the call to go through a jump near the module; returns that jump, or null with the site unchanged.
std::uint8_t* RedirectClimbableAhead() {
    std::uint8_t* jump = NearJump(reinterpret_cast<std::uintptr_t>(&ClimbableAhead));
    if (jump == nullptr) return nullptr;
    const std::int64_t offset = jump - (game + kClimbableAheadCall + sizeof(kClimbableAheadCallBytes));
    std::uint8_t call[sizeof(kClimbableAheadCallBytes)] = {0xe8};
    if (offset >= std::numeric_limits<std::int32_t>::min() && offset <= std::numeric_limits<std::int32_t>::max()) {
        const auto relative = static_cast<std::int32_t>(offset);
        std::memcpy(call + 1, &relative, sizeof(relative));
        if (Rewrite(kClimbableAheadCall, kClimbableAheadCallBytes, call)) return jump;
    }
    VirtualFree(jump, 0, MEM_RELEASE);
    return nullptr;
}

bool HookSelector() {
    const std::size_t size = sizeof(kTrampolineStart) + kPrologue + sizeof(kJumpBack) + sizeof(void*);
    auto* trampoline = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (trampoline == nullptr) return false;
    std::uint8_t* const method = game + kSelectEvaluate;
    const void* const resume = method + kPrologue;
    const auto handler = &GrabsWithUp;
    std::memcpy(trampoline, kTrampolineStart, sizeof(kTrampolineStart));
    std::memcpy(trampoline + kTrampolineHandler, &handler, sizeof(handler));
    std::memcpy(trampoline + sizeof(kTrampolineStart), method, kPrologue);
    std::memcpy(trampoline + sizeof(kTrampolineStart) + kPrologue, kJumpBack, sizeof(kJumpBack));
    std::memcpy(trampoline + sizeof(kTrampolineStart) + kPrologue + sizeof(kJumpBack), &resume, sizeof(resume));
    std::uint8_t jump[kEntryJump] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    std::memcpy(jump + 2, &trampoline, sizeof(trampoline));
    DWORD trampolineProtection = 0, gameProtection = 0;
    if (!VirtualProtect(trampoline, size, PAGE_EXECUTE_READ, &trampolineProtection) ||
        !VirtualProtect(method, sizeof(jump), PAGE_EXECUTE_READWRITE, &gameProtection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(method, jump, sizeof(jump));
    VirtualProtect(method, sizeof(jump), gameProtection, &gameProtection);
    FlushInstructionCache(GetCurrentProcess(), trampoline, size);
    FlushInstructionCache(GetCurrentProcess(), method, sizeof(jump));
    return true;
}

// Both patches or neither: without the lookup the selector would offer Up on ropes too.
bool Install() {
    std::uint8_t* jump = RedirectClimbableAhead();
    if (jump == nullptr) return false;
    if (HookSelector()) return true;
    std::uint8_t call[sizeof(kClimbableAheadCallBytes)];
    std::memcpy(call, game + kClimbableAheadCall, sizeof(call));
    Rewrite(kClimbableAheadCall, call, kClimbableAheadCallBytes);
    VirtualFree(jump, 0, MEM_RELEASE);
    return false;
}

// Nothing is patched while the option is off, so the game keeps its own input exactly.
bool StartLadderGrab() {
    const char* option = std::getenv("SOS_LADDER_GRAB_WITH_UP");
    if (option == nullptr || std::strcmp(option, "1") != 0) return false;
    game = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(L"Il2cppUserAssemblies.prx.guest.prx"));
    const auto* headers = game != nullptr ? reinterpret_cast<const IMAGE_NT_HEADERS*>(game + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew) : nullptr;
    if (headers == nullptr || !Recognised(headers->OptionalHeader.SizeOfImage)) {
        std::fprintf(stderr, "Grab ladders with Up: the climbing code was not recognised, only the interact button grabs\n");
        return false;
    }
    if (!Install()) {
        std::fprintf(stderr, "Grab ladders with Up: the climbing code could not be patched, only the interact button grabs\n");
        return false;
    }
    std::fprintf(stderr, "Grab ladders with Up: patch applied\n");
    return true;
}

[[maybe_unused]] const bool ladderGrabStarted = StartLadderGrab();

}
#endif
