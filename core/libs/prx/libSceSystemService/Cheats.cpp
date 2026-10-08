#ifdef _WIN32
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <windows.h>
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/HostExtension/include/HostExtension.hpp"
#include "prx/libSceAvPlayer/include/SafeTransition.hpp"

namespace {

// Sons of Sparta player cheats, patched into Il2cppUserAssemblies when this library loads. Offsets are module RVAs.

struct Cheat {
    const char* name;
    const char* variable;
    std::atomic<bool> on{false};
    bool hooked = false;
};
Cheat godMode{"God Mode", "SOS_GOD_MODE"};
Cheat infiniteSpirit{"Infinite Spartan Spirit", "SOS_INFINITE_SPARTAN_SPIRIT"};
Cheat infiniteMagic{"Infinite Magic", "SOS_INFINITE_MAGIC"};
Cheat damageMultiplier{"Damage Multiplier", "SOS_DAMAGE_MULTIPLIER"};  // its state is damageFactor, not on
std::atomic<int> damageFactor{1};                                       // 1 (off), 2, 4 or 6
Cheat movementSpeed{"Movement Speed 2x", "SOS_MOVEMENT_SPEED"};
Cheat jumpHeight{"Jump Height 2x", "SOS_JUMP_HEIGHT"};

constexpr const char* kBloodOrb[] = {"loot_orb_red"};
// ItemGroup.Materials without its loot_boss_* trophies, which only their boss gives.
constexpr const char* kUpgradeMaterials[] = {"loot_common_shield", "loot_rare_shield", "loot_epic_shield", "loot_common_spear",
                                             "loot_rare_spear", "loot_epic_spear_grip", "loot_epic_spear_tail", "loot_epic_spear_tip",
                                             "loot_epic_spear_tip_status"};
constexpr std::size_t kMostItems = std::size(kUpgradeMaterials);
static_assert(std::size(kBloodOrb) <= kMostItems);

// The one wallet action F7 and F8 share. Only the window thread queues it or gives up a queued one; the game thread moves a queued action
// to running and back to idle, and owns it while it runs.
enum : int { kIdle, kBloodOrbsQueued, kBloodOrbsRunning, kUpgradeMaterialsQueued, kUpgradeMaterialsRunning };
std::atomic<int> walletAction{kIdle};
std::atomic<std::uint64_t> walletRequestedAt{0};
std::atomic<std::uint64_t> playerLastSeenAt{0};

struct Award {
    const char* name;
    const char* added;
    const char* const* items;
    std::size_t itemCount;
    std::int64_t amount;  // of each item
    bool announce;        // whether the HUD announces the additions as a pickup would
    int queued, running;
    std::atomic<const char*> notice{nullptr};
    std::atomic<std::uint64_t> noticeUntil{0};
};
Award bloodOrbs{"Blood Orbs", "+1000", kBloodOrb, std::size(kBloodOrb), 1000, true, kBloodOrbsQueued, kBloodOrbsRunning};
Award upgradeMaterials{"Upgrade Materials", "+10", kUpgradeMaterials, std::size(kUpgradeMaterials), 10, false, kUpgradeMaterialsQueued,
                       kUpgradeMaterialsRunning};
const char* awardsUnavailable = "game code not recognised";  // null once the wallet hook is installed

// SDL scancodes
constexpr int kGodModeKey = 58, kInfiniteSpiritKey = 59, kInfiniteMagicKey = 60, kDamageMultiplierKey = 61, kMovementSpeedKey = 62, kJumpHeightKey = 63,
              kBloodOrbsKey = 64, kUpgradeMaterialsKey = 65;
constexpr std::uint64_t kNoticeMs = 2000;
constexpr std::uint64_t kAwardWaitMs = 1000;
constexpr std::uint64_t kPlayerTimeoutMs = 500;

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint8_t* game = nullptr;
constexpr std::uint32_t kOffsetHp = 0xdab550;
constexpr std::uint32_t kOffsetGreyHp = 0xdabaf0;
constexpr std::uint32_t kOffsetSpirit = 0xda20b0;
constexpr std::uint32_t kManaClamp = 0xd95a0f;          // in ManaState.OffsetMana
constexpr std::uint32_t kHitDamageCall = 0xd91e1f;      // in Hittable.ExecuteHitEvent
constexpr std::uint32_t kIsPlayer = 0x80ef70;
constexpr std::uint32_t kGetGameObject = 0x629c8f0;     // Component.get_gameObject icall pointer, resolved by the game before it is needed
constexpr std::uint32_t kControllerUpdate = 0x5d35c0;   // PlayerController.Update(), empty
constexpr std::uint32_t kGetPawnObject = 0x5d12d0;
constexpr std::uint32_t kStringNew = 0x32fac0;
constexpr std::uint32_t kGetItem = 0x8c91b0;
constexpr std::uint32_t kTryGetWallet = 0x8c0810;
constexpr std::uint32_t kGetAmount = 0x8ce2b0;
constexpr std::uint32_t kAddToWallet = 0x8ce430;
constexpr std::uint32_t kInputDeltaCall = 0x7067a9;     // in PlayerMovement.TickPseudoPhysics
constexpr std::uint32_t kSlopeAligned = 0x6ecf20;
constexpr std::uint32_t kJumpDirectionCall = 0x6fc9c7;  // in MoveStateJump.<DelayStartJumping>d__27.MoveNext
constexpr std::uint32_t kJumpAngle = 0x6face0;

constexpr std::size_t kNativeObject = 0x10;  // UnityEngine.Object.m_CachedPtr, null once destroyed
constexpr std::size_t kMaxHp = 0x38, kCurrentHp = 0x40, kGreyHp = 0x44;
constexpr std::size_t kCurrentSpirit = 0x20, kMaxSpirit = 0x24;
constexpr std::size_t kCurrentMana = 0x28, kMaxMana = 0x2c;
constexpr std::size_t kHitSource = 0x10, kHitTarget = 0x20, kHitConfig = 0x28, kHitArgs = 0x30;
constexpr std::size_t kDamageModule = 0x10, kDamageConfig = 0x10;
constexpr std::size_t kDamage = 0x10, kUnmodifiableDamage = 0x35;
constexpr std::size_t kApplyModuleSlot = 0x178;  // vtable slot: method pointer, then MethodInfo
constexpr std::size_t kWalletCount = 0x20;
constexpr std::size_t kInputDelta = 0xe0;
constexpr std::size_t kJumpContext = 0x58;
constexpr std::size_t kForwardAdjustment = 0x9c;
// sqrt(2): a jump's height grows with the square of its upward speed, so this doubles the height.
constexpr float kJumpSpeedFactor = 1.41421356f;

using GetGameObject = void*(APS5_VABI*)(void* component);
using IsPlayer = bool(APS5_VABI*)(void* gameObject, const void* method);
using Filter = bool(APS5_VABI*)(void* self, void* argument, void* method, float* amount);
using GetPawnObject = void*(APS5_VABI*)(void* controller, const void* method);
using StringNew = void*(APS5_VABI*)(const char* text);
using GetItem = void*(APS5_VABI*)(void* id, const void* method);
using TryGetWallet = void*(APS5_VABI*)(void* gameObject, const void* method);
using GetAmount = std::int64_t(APS5_VABI*)(void* wallet, void* item, const void* method);
// System.Nullable<bool> overrideDontShowNotification: unset lets the item decide how the HUD announces it; true announces nothing.
struct NullableBool {
    bool hasValue;
    bool value;
};
using AddToWallet = void(APS5_VABI*)(void* wallet, void* item, std::int64_t count, NullableBool overrideDontShowNotification, const void* method);
struct Vector2 {
    float x, y;
};
using SlopeAligned = Vector2(APS5_VABI*)(void* movement, Vector2 moveDelta, const void* method);
using JumpAngle = Vector2(APS5_VABI*)(void* jumpState, float forwardAdjustmentFactor, const void* method);
using ApplyModule = void(APS5_VABI*)(void* module, void* args, const void* method);

template <typename T> T& Field(void* object, std::size_t offset) { return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset); }
template <typename Function> Function Game(std::uint32_t offset) { return reinterpret_cast<Function>(game + offset); }

bool Alive(void* object) { return object != nullptr && Field<void*>(object, kNativeObject) != nullptr; }

bool OnPlayer(void* component) {
    const auto gameObject = Field<GetGameObject>(game, kGetGameObject);
    return Alive(component) && gameObject != nullptr && reinterpret_cast<IsPlayer>(game + kIsPlayer)(gameObject(component), nullptr);
}

// The green health bar is current minus grey health: keep both full and drop changes that would shrink it. The method still runs.
bool HoldFull(void* vitals, float* amount, bool grey) {
    if (!godMode.on.load(std::memory_order_relaxed) || !OnPlayer(vitals)) return false;
    Field<float>(vitals, kCurrentHp) = Field<float>(vitals, kMaxHp);
    Field<float>(vitals, kGreyHp) = 0.0f;
    if (grey ? *amount > 0.0f : *amount < 0.0f) *amount = 0.0f;
    return false;
}

bool APS5_VABI HealthFilter(void* vitals, void*, void*, float* amount) { return HoldFull(vitals, amount, false); }
bool APS5_VABI GreyHealthFilter(void* vitals, void*, void*, float* amount) { return HoldFull(vitals, amount, true); }

bool APS5_VABI SpiritFilter(void* spirit, void*, void*, float* amount) {
    if (!infiniteSpirit.on.load(std::memory_order_relaxed) || !OnPlayer(spirit)) return false;
    Field<float>(spirit, kCurrentSpirit) = Field<float>(spirit, kMaxSpirit);
    *amount = 0.0f;
    return false;
}

// Replaces OffsetMana's computation of the new mana; the method starts with a rip-relative read, so it cannot take an entry hook, and it
// still stores the result and raises its events.
float APS5_VABI ManaAfterOffset(void* mana, float amount) {
    float current = Field<float>(mana, kCurrentMana);
    const float max = Field<float>(mana, kMaxMana);
    if (infiniteMagic.on.load(std::memory_order_relaxed) && OnPlayer(mana)) {
        current = max;
        if (amount < 0.0f) amount = 0.0f;
    }
    const float value = current + amount;
    const float limited = max < value ? max : value;  // vminss max, value
    return value < 0.0f ? 0.0f : limited;             // vcmpltss value, 0; vandnps
}

bool PlayerHit(void* hit, void* config) {
    if (config == nullptr || Field<bool>(config, kUnmodifiableDamage)) return false;
    const float damage = Field<float>(config, kDamage);
    if (damage == 0.0f || !std::isfinite(damage)) return false;
    void* const source = Field<void*>(hit, kHitSource);
    void* const target = Field<void*>(hit, kHitTarget);
    return Alive(source) && Game<IsPlayer>(kIsPlayer)(source, nullptr) && Alive(target) && !OnPlayer(target);
}

// Replaces ExecuteHitEvent's hit.hitConfig.damageModule.ApplyModule(hit.eventModuleArgs). The damage is multiplied for this call only, so
// a config that is applied again is never multiplied twice.
void APS5_VABI ApplyHitDamage(void* hit) {
    void* const module = Field<void*>(Field<void*>(hit, kHitConfig), kDamageModule);
    if (module == nullptr) return;
    void* const klass = Field<void*>(module, 0);
    const ApplyModule apply = Field<ApplyModule>(klass, kApplyModuleSlot);
    const void* const method = Field<void*>(klass, kApplyModuleSlot + sizeof(void*));
    void* const config = Field<void*>(module, kDamageConfig);
    const int factor = damageFactor.load(std::memory_order_relaxed);
    if (factor == 1 || !PlayerHit(hit, config) || !std::isfinite(Field<float>(config, kDamage) * static_cast<float>(factor))) {
        apply(module, Field<void*>(hit, kHitArgs), method);
        return;
    }
    float& damage = Field<float>(config, kDamage);
    const float original = damage;
    damage = original * static_cast<float>(factor);
    apply(module, Field<void*>(hit, kHitArgs), method);
    damage = original;
}

// Replaces TickPseudoPhysics's call to GetSlopeAlignedVersionOfMovementDelta, so only the input movement doubles: gravity, jumps,
// knockback and root motion are added after it.
Vector2 APS5_VABI MovementInputDelta(void* movement) {
    const Vector2 delta = Game<SlopeAligned>(kSlopeAligned)(movement, Field<Vector2>(movement, kInputDelta), nullptr);
    if (!movementSpeed.on.load(std::memory_order_relaxed) || !OnPlayer(movement)) return delta;
    return {delta.x * 2.0f, delta.y * 2.0f};
}

// Replaces the jump coroutine's call to GetJumpAngleAsForwardDirectionVector, whose result times the jump magnitude is the jump's one
// impulse. Scripted launches do not go through it.
Vector2 APS5_VABI JumpDirection(void* jumpState) {
    Vector2 direction = Game<JumpAngle>(kJumpAngle)(jumpState, Field<float>(jumpState, kForwardAdjustment), nullptr);
    if (!jumpHeight.on.load(std::memory_order_relaxed) || direction.y <= 0.0f || !OnPlayer(Field<void*>(jumpState, kJumpContext))) return direction;
    direction.y *= kJumpSpeedFactor;
    return direction;
}

void Added(Award& award) {
    std::fprintf(stderr, "%s: %s\n", award.name, award.added);
    award.notice.store(award.added);
    award.noticeUntil.store(NowMs() + kNoticeMs);
}

void Failed(const Award& award, const char* failure, const char* item) {
    if (item == nullptr) {
        std::fprintf(stderr, "%s: not added (%s)\n", award.name, failure);
    } else {
        std::fprintf(stderr, "%s: not added (%s: %s)\n", award.name, failure, item);
    }
}

// GetCurrentAmount needs the count table the wallet creates at its first addition.
std::int64_t Amount(void* wallet, void* item) {
    return Field<void*>(wallet, kWalletCount) != nullptr ? Game<GetAmount>(kGetAmount)(wallet, item, nullptr) : 0;
}

// Adds every item of the award or none: each item is resolved and checked before the first addition. Returns the failure, and in *item
// the item it is about.
const char* Give(const Award& award, void* controller, const char** item) {
    void* player = Game<GetPawnObject>(kGetPawnObject)(controller, nullptr);
    if (player == nullptr) return "no player";
    void* wallet = Game<TryGetWallet>(kTryGetWallet)(player, nullptr);
    if (wallet == nullptr) return "no player wallet";
    void* items[kMostItems];
    for (std::size_t i = 0; i < award.itemCount; ++i) {
        *item = award.items[i];
        void* id = Game<StringNew>(kStringNew)(award.items[i]);
        items[i] = id != nullptr ? Game<GetItem>(kGetItem)(id, nullptr) : nullptr;
        if (!Alive(items[i])) return "item not found";
    }
    for (std::size_t i = 0; i < award.itemCount; ++i) {
        *item = award.items[i];
        if (Amount(wallet, items[i]) > std::numeric_limits<std::int64_t>::max() - award.amount) return "count at the largest number it can hold";
    }
    *item = nullptr;
    const NullableBool notification{!award.announce, !award.announce};
    for (std::size_t i = 0; i < award.itemCount; ++i) Game<AddToWallet>(kAddToWallet)(wallet, items[i], award.amount, notification, nullptr);
    return nullptr;
}

// Gives the wallet action back (idle) however its run ends.
struct WalletActionRun {
    WalletActionRun() = default;
    WalletActionRun(const WalletActionRun&) = delete;
    WalletActionRun& operator=(const WalletActionRun&) = delete;
    ~WalletActionRun() { walletAction.store(kIdle); }
};

__attribute__((noinline)) void RunWalletAction(void* controller) {
    int state = walletAction.load(std::memory_order_relaxed);
    if (state != kBloodOrbsQueued && state != kUpgradeMaterialsQueued) return;
    Award& award = state == kBloodOrbsQueued ? bloodOrbs : upgradeMaterials;
    if (!walletAction.compare_exchange_strong(state, award.running)) return;  // given up meanwhile
    const WalletActionRun run;
    const char* item = nullptr;
    const char* failure = Give(award, controller, &item);
    if (failure == nullptr) {
        Added(award);
    } else {
        Failed(award, failure, item);
    }
}

// PlayerController.Update, every frame on the game thread. The run is a separate function because a SysV function here cannot have
// cleanups, and the release is a destructor.
bool APS5_VABI ControllerFilter(void* controller, void*, void*, float*) {
    if (Game<GetPawnObject>(kGetPawnObject)(controller, nullptr) != nullptr) playerLastSeenAt.store(NowMs());
    RunWalletAction(controller);
    return false;
}

// A queued action that no player's controller takes in time fails: no player in the game, or no frames.
void ExpireRequest(std::uint64_t now) {
    int state = walletAction.load();
    if ((state != kBloodOrbsQueued && state != kUpgradeMaterialsQueued) || now < walletRequestedAt.load() + kAwardWaitMs) return;
    if (!walletAction.compare_exchange_strong(state, kIdle)) return;  // the game thread took it meanwhile
    Award& award = state == kBloodOrbsQueued ? bloodOrbs : upgradeMaterials;
    Failed(award, "no player in the game", nullptr);
}

// The one availability rule for the item actions, shared by the menu buttons and the F7/F8 keys.
bool AwardsAvailable(std::uint64_t now) {
    return awardsUnavailable == nullptr && walletAction.load() == kIdle && now < playerLastSeenAt.load() + kPlayerTimeoutMs;
}

// F7 or F8: queues its action when AwardsAvailable allows it.
void Request(Award& award) {
    const std::uint64_t now = NowMs();
    ExpireRequest(now);
    if (!AwardsAvailable(now)) return;
    walletRequestedAt.store(now);      // before the action is queued, so the expiry never reads an older time
    walletAction.store(award.queued);  // only this thread leaves idle
}

// Entry hook: the method starts with mov rax, hook; jmp rax. The hook calls filter(this, rsi, rdx, &xmm0) with the arguments kept and
// returns from the method when it returns true; otherwise the method's copied first instructions run and a jump that changes no register or
// flag continues the method (OffsetHp's first instructions set eax and the flags it uses later).
constexpr std::uint8_t kHookStart[] = {
    0x57, 0x56, 0x52,                    // push rdi; push rsi; push rdx
    0x48, 0x83, 0xec, 0x10,              // sub rsp, 16
    0xc5, 0xfa, 0x11, 0x04, 0x24,        // vmovss dword ptr [rsp], xmm0
    0x48, 0x89, 0xe1,                    // mov rcx, rsp
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax, filter (offset 17)
    0xff, 0xd0,                          // call rax
    0xc5, 0xfa, 0x10, 0x04, 0x24,        // vmovss xmm0, dword ptr [rsp]
    0x48, 0x83, 0xc4, 0x10,              // add rsp, 16
    0x5a, 0x5e, 0x5f,                    // pop rdx; pop rsi; pop rdi
    0x84, 0xc0,                          // test al, al
    0x74, 0x01,                          // je, to the method's first instructions
    0xc3,                                // ret
};
constexpr std::size_t kHookFilter = 17;
constexpr std::uint8_t kJumpBack[] = {0xff, 0x25, 0, 0, 0, 0};  // jmp qword ptr [rip], followed by the address
constexpr std::size_t kEntryJump = 12;                         // mov rax, hook; jmp rax

// prologue: the method's first whole instructions, at least kEntryJump bytes, with no rip-relative operand and no jump into them; a
// shorter (empty) method is copied whole and the entry jump overwrites the padding after it.
struct Patch {
    std::uint32_t method;
    std::size_t prologue;
    Filter filter;
};

struct Range { std::uint32_t begin, end; };

// FNV-1a of relocation-free code, so it only matches the analysed game build; the ranges include every byte a patch replaces.
std::uint64_t Hash(std::initializer_list<Range> code) {
    std::uint64_t hash = 0xcbf29ce484222325;
    for (const auto& range : code) {
        for (std::uint32_t offset = range.begin; offset < range.end; ++offset) hash = (hash ^ game[offset]) * 0x100000001b3;
    }
    return hash;
}

bool Hook(const Patch& patch) {
    std::uint8_t* method = game + patch.method;
    const std::size_t size = sizeof(kHookStart) + patch.prologue + sizeof(kJumpBack) + sizeof(void*);
    auto* hook = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (hook == nullptr) return false;
    const void* resume = method + patch.prologue;
    std::memcpy(hook, kHookStart, sizeof(kHookStart));
    std::memcpy(hook + kHookFilter, &patch.filter, sizeof(patch.filter));
    std::memcpy(hook + sizeof(kHookStart), method, patch.prologue);
    std::memcpy(hook + sizeof(kHookStart) + patch.prologue, kJumpBack, sizeof(kJumpBack));
    std::memcpy(hook + sizeof(kHookStart) + patch.prologue + sizeof(kJumpBack), &resume, sizeof(resume));
    std::uint8_t jump[kEntryJump] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    std::memcpy(jump + 2, &hook, sizeof(hook));
    DWORD hookProtection = 0, gameProtection = 0;
    if (!VirtualProtect(hook, size, PAGE_EXECUTE_READ, &hookProtection) ||
        !VirtualProtect(method, sizeof(jump), PAGE_EXECUTE_READWRITE, &gameProtection)) {
        VirtualFree(hook, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(method, jump, sizeof(jump));
    VirtualProtect(method, sizeof(jump), gameProtection, &gameProtection);
    FlushInstructionCache(GetCurrentProcess(), hook, size);
    FlushInstructionCache(GetCurrentProcess(), method, sizeof(jump));
    return true;
}

// Call sites rewritten in place to mov rdi, rbx; mov rax, function; call rax, padded with nops. At each site the stack is aligned for the
// call, the registers the call may change are not read before being written, and no jump lands inside the bytes except on the first one.
// vmovsd xmm0, qword ptr [rbx + 0xe0]; mov rdi, rbx; call GetSlopeAlignedVersionOfMovementDelta
constexpr std::uint8_t kInputDeltaCallBytes[] = {0xc5, 0xfb, 0x10, 0x83, 0xe0, 0x00, 0x00, 0x00, 0x48, 0x89, 0xdf, 0xe8, 0x67, 0x67, 0xfe, 0xff};
// vmovss xmm0, dword ptr [rbx + 0x9c]; mov r14, qword ptr [rbx + 0x58]; mov rdi, rbx; call GetJumpAngleAsForwardDirectionVector (the mov
// r14 is kept)
constexpr std::uint8_t kJumpDirectionCallBytes[] = {0xc5, 0xfa, 0x10, 0x83, 0x9c, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x73, 0x58,
                                                    0x48, 0x89, 0xdf, 0xe8, 0x05, 0xe3, 0xff, 0xff};
// vaddss xmm0, xmm0, [rbx + 0x28]; vmovss xmm1, [rbx + 0x2c]; vxorps xmm2, xmm2, xmm2; vminss xmm1, xmm1, xmm0; vcmpltss xmm0, xmm0, xmm2;
// vandnps xmm0, xmm0, xmm1, followed by the store of xmm0 in currentMana
constexpr std::uint8_t kManaClampBytes[] = {0xc5, 0xfa, 0x58, 0x43, 0x28, 0xc5, 0xfa, 0x10, 0x4b, 0x2c, 0xc5, 0xe8, 0x57, 0xd2,
                                            0xc5, 0xf2, 0x5d, 0xc8, 0xc5, 0xfa, 0xc2, 0xc2, 0x01, 0xc5, 0xf8, 0x55, 0xc1};
// with hitConfig in rax: mov rdi, [rax + 0x10]; test rdi, rdi; je past the call; mov rax, [rdi]; mov rsi, [rbx + 0x30];
// mov rdx, [rax + 0x180]; call [rax + 0x178]
constexpr std::uint8_t kHitDamageCallBytes[] = {0x48, 0x8b, 0x78, 0x10, 0x48, 0x85, 0xff, 0x74, 0x14, 0x48, 0x8b, 0x07, 0x48, 0x8b, 0x73,
                                                0x30, 0x48, 0x8b, 0x90, 0x80, 0x01, 0x00, 0x00, 0xff, 0x90, 0x78, 0x01, 0x00, 0x00};

// Replaces the game's code at site, if it still holds original, with code of the same size.
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

bool RedirectInputDelta() {
    std::uint8_t call[sizeof(kInputDeltaCallBytes)] = {0x48, 0x89, 0xdf, 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0, 0x90};
    const auto function = &MovementInputDelta;
    std::memcpy(call + 5, &function, sizeof(function));
    return Rewrite(kInputDeltaCall, kInputDeltaCallBytes, call);
}

bool RedirectJumpDirection() {
    std::uint8_t call[sizeof(kJumpDirectionCallBytes)] = {0x4c, 0x8b, 0x73, 0x58, 0x48, 0x89, 0xdf, 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0, 0x90};
    const auto function = &JumpDirection;
    std::memcpy(call + 9, &function, sizeof(function));
    return Rewrite(kJumpDirectionCall, kJumpDirectionCallBytes, call);
}

bool RedirectManaClamp() {
    std::uint8_t call[sizeof(kManaClampBytes)] = {0x48, 0x89, 0xdf, 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0,
                                                  0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00, 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
    const auto function = &ManaAfterOffset;
    std::memcpy(call + 5, &function, sizeof(function));
    return Rewrite(kManaClamp, kManaClampBytes, call);
}

bool RedirectHitDamage() {
    std::uint8_t call[sizeof(kHitDamageCallBytes)] = {0x48, 0x89, 0xdf, 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0,
                                                      0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00};
    const auto function = &ApplyHitDamage;
    std::memcpy(call + 5, &function, sizeof(function));
    return Rewrite(kHitDamageCall, kHitDamageCallBytes, call);
}

// A cheat whose game code is not recognised or cannot be patched stays off and unavailable; the others are not affected.
bool Install(Cheat& cheat, bool recognised, std::initializer_list<Patch> patches, bool (*redirect)()) {
    if (!recognised) {
        std::fprintf(stderr, "%s: game code not recognised, cheat disabled\n", cheat.name);
        return false;
    }
    for (const Patch& patch : patches) {
        if (!Hook(patch)) {
            std::fprintf(stderr, "%s: game code could not be patched, cheat disabled\n", cheat.name);
            return false;
        }
    }
    if (redirect != nullptr && !redirect()) {
        std::fprintf(stderr, "%s: game code could not be patched, cheat disabled\n", cheat.name);
        return false;
    }
    cheat.hooked = true;
    std::fprintf(stderr, "%s: patch applied\n", cheat.name);
    return true;
}

void Start(Cheat& cheat, bool recognised, std::initializer_list<Patch> patches, bool (*redirect)() = nullptr) {
    const char* value = std::getenv(cheat.variable);
    const bool on = value != nullptr && std::strcmp(value, "1") == 0;
    std::fprintf(stderr, "%s: %s\n", cheat.name, on ? "ON" : "OFF");
    if (Install(cheat, recognised, patches, redirect)) cheat.on.store(on);
}

const char* FactorText(int factor) { return factor == 2 ? "2x" : factor == 4 ? "4x" : factor == 6 ? "6x" : "OFF"; }

// Only 2, 4 and 6 select a multiplier; anything else is normal damage.
int StartingDamageFactor() {
    const char* value = std::getenv(damageMultiplier.variable);
    if (value == nullptr) return 1;
    for (int factor : {2, 4, 6}) {
        if (value[0] == '0' + factor && value[1] == '\0') return factor;
    }
    return 1;
}

void StartDamageMultiplier(bool recognised) {
    const int factor = StartingDamageFactor();
    std::fprintf(stderr, "%s: %s\n", damageMultiplier.name, FactorText(factor));
    if (Install(damageMultiplier, recognised, {}, RedirectHitDamage)) damageFactor.store(factor);
}

void SyncSafeTransition() { SafeTransition::Set(movementSpeed.on.load() || jumpHeight.on.load()); }

// The state text of a cheat that is on, or null.
const char* ActiveState(const Cheat& cheat) {
    if (&cheat != &damageMultiplier) return cheat.on.load() ? "ON" : nullptr;
    const int factor = damageFactor.load();
    return factor == 1 ? nullptr : FactorText(factor);
}

void Announce(const Cheat& cheat, const char* notice) {
    std::fprintf(stderr, "%s: %s\n", cheat.name, notice != nullptr ? notice : ActiveState(cheat));
}

// Hotkeys and the menu both change toggles through here.
void SetToggle(Cheat& cheat, bool on) {
    if (!cheat.hooked) {
        Announce(cheat, "unavailable");
        return;
    }
    cheat.on.store(on);
    if (&cheat == &movementSpeed || &cheat == &jumpHeight) SyncSafeTransition();
    Announce(cheat, on ? nullptr : "OFF");
}

void SetDamageFactor(int factor) {
    if (!damageMultiplier.hooked) {
        Announce(damageMultiplier, "unavailable");
        return;
    }
    damageFactor.store(factor);
    Announce(damageMultiplier, factor == 1 ? "OFF" : nullptr);
}

// Runs on the window thread, where game objects must not be touched: keys only switch states or queue a wallet action.
void OnKey(int scancode) {
    if (scancode == kBloodOrbsKey || scancode == kUpgradeMaterialsKey) {
        Request(scancode == kBloodOrbsKey ? bloodOrbs : upgradeMaterials);
        return;
    }
    Cheat* cheat = scancode == kGodModeKey ? &godMode
                 : scancode == kInfiniteSpiritKey ? &infiniteSpirit
                 : scancode == kInfiniteMagicKey  ? &infiniteMagic
                 : scancode == kDamageMultiplierKey ? &damageMultiplier
                 : scancode == kMovementSpeedKey ? &movementSpeed
                 : scancode == kJumpHeightKey    ? &jumpHeight
                                                 : nullptr;
    if (cheat == nullptr) return;
    if (cheat == &damageMultiplier) {
        const int factor = damageFactor.load();
        SetDamageFactor(factor == 1 ? 2 : factor == 2 ? 4 : factor == 4 ? 6 : 1);
    } else {
        SetToggle(*cheat, !cheat->on.load());
    }
}

// The menu's view of the damage multiplier, in the order of its combo box.
constexpr int kDamageFactors[] = {1, 2, 4, 6};

void DrawToggle(const HostMenuWidgets& ui, Cheat& cheat, const char* label) {
    bool on = cheat.on.load();
    if (!ui.checkbox(label, &on, cheat.hooked)) return;
    std::fprintf(stderr, "[DEBUG_SAULO][InGameMenu] %s clicked\n", cheat.name);
    SetToggle(cheat, on);
}

void DrawDamage(const HostMenuWidgets& ui) {
    const char* items[std::size(kDamageFactors)];
    int index = 0;
    for (std::size_t i = 0; i < std::size(kDamageFactors); ++i) {
        items[i] = FactorText(kDamageFactors[i]);
        if (kDamageFactors[i] == damageFactor.load()) index = static_cast<int>(i);
    }
    if (!ui.combo("Damage Multiplier (F4)", &index, items, static_cast<int>(std::size(items)), damageMultiplier.hooked)) return;
    std::fprintf(stderr, "[DEBUG_SAULO][InGameMenu] %s changed\n", damageMultiplier.name);
    SetDamageFactor(kDamageFactors[index]);
}

void DrawAward(const HostMenuWidgets& ui, Award& award, const char* label) {
    const std::uint64_t now = NowMs();
    const char* notice = award.notice.load();
    const char* status = notice != nullptr && now < award.noticeUntil.load() ? notice : "";
    if (!ui.statusButton(label, status, AwardsAvailable(now))) return;
    std::fprintf(stderr, "[DEBUG_SAULO][InGameMenu] %s clicked\n", award.name);
    Request(award);
}

// Drawn by the host overlay inside the menu window while it is open, on the window thread.
void DrawMenu(const HostMenuWidgets& ui) {
    ui.heading("PLAYER");
    DrawToggle(ui, godMode, "God Mode (F1)");
    DrawToggle(ui, infiniteSpirit, "Infinite Spartan Spirit (F2)");
    DrawToggle(ui, infiniteMagic, "Infinite Magic (F3)");
    DrawDamage(ui);
    ui.separator();
    ui.heading("MOVEMENT");
    DrawToggle(ui, movementSpeed, "Movement Speed 2x (F5)");
    DrawToggle(ui, jumpHeight, "Jump Height 2x (F6)");
    ui.separator();
    ui.heading("ITEMS");
    DrawAward(ui, bloodOrbs, "Add 1000 Blood Orbs (F7)");
    DrawAward(ui, upgradeMaterials, "Add 10 Upgrade Materials (F8)");
}

// Once per presented frame on the window thread, so a queued wallet action that no player takes in time reports its failure.
void OnFrame() {
    ExpireRequest(NowMs());
}

bool StartCheats() {
    game = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(L"Il2cppUserAssemblies.prx.guest.prx"));
    if (game == nullptr) return false;
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(game + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const bool fits = headers->OptionalHeader.SizeOfImage >= kGetGameObject + sizeof(void*);
    const bool godModeRecognised = fits && Hash({{0x80ecd0, 0x80f0b0}, {0xdab1a0, 0xdab6b0}, {0xdabaf0, 0xdabd90}}) == 0x4b6beba820a6ebce;
    Start(godMode, godModeRecognised, {{kOffsetHp, 12, HealthFilter}, {kOffsetGreyHp, 13, GreyHealthFilter}});
    Start(infiniteSpirit, fits && Hash({{0xda20b0, 0xda2520}, {0xda2b10, 0xda2d10}, {0x80ecd0, 0x80f0b0}}) == 0xb0e9d2c9cfd2a0b1,
        {{kOffsetSpirit, 12, SpiritFilter}});
    const bool magicRecognised = fits && Hash({{0xd954f0, 0xd95bd0}, {0xdf2980, 0xdf2bc0}, {0x823d20, 0x823e30}, {0xc26550, 0xc26750},
        {0xc1af20, 0xc1afe0}, {0x80ecd0, 0x80f0b0}}) == 0xa63f97d69c46ad8d;
    Start(infiniteMagic, magicRecognised, {}, RedirectManaClamp);
    const bool damageRecognised = fits && Hash({{0xd91d00, 0xd92910}, {0xd83100, 0xd837d0}, {0xdf3900, 0xdf3be0}, {0xdf3e60, 0xdf4e00},
        {0xdf98d0, 0xdf9af0}, {0xd94d70, 0xd95120}, {0x80ecd0, 0x80f0b0}}) == 0x7b452922017e03ec;
    StartDamageMultiplier(damageRecognised);
    const bool awardsRecognised = fits && Hash({{0x32fac0, 0x32fae0}, {0x5d12d0, 0x5d1390}, {0x5d35c0, 0x5d35d0}, {0x8c0810, 0x8c0950},
        {0x8c91b0, 0x8c92b0}, {0x8ce0f0, 0x8ce6d0}, {0x996d20, 0x996e70}, {0x9971b0, 0x997390}, {0xd11230, 0xd11300}}) == 0xdcbbc10a00bb8f1e;
    if (awardsRecognised) awardsUnavailable = Hook({kControllerUpdate, 1, ControllerFilter}) ? nullptr : "game code could not be patched";
    if (awardsUnavailable != nullptr) {
        std::fprintf(stderr, "Blood Orbs and Upgrade Materials: %s, keys disabled\n", awardsUnavailable);
    } else {
        std::fprintf(stderr, "Blood Orbs and Upgrade Materials: patch applied\n");
    }
    const bool movementRecognised = fits && Hash({{0x706620, 0x706b50}, {0x6ecf20, 0x6ed280}, {0x6f02e0, 0x6f06d0}, {0x708d70, 0x709360},
        {0x80ecd0, 0x80f0b0}}) == 0xe4fb67f213ff4921;
    Start(movementSpeed, movementRecognised, {}, RedirectInputDelta);
    const bool jumpRecognised = fits && Hash({{0x6fc450, 0x6fcbf0}, {0x6face0, 0x6fb1b0}, {0x6efe80, 0x6f0180}, {0x6f0810, 0x6f08e0},
        {0x708d70, 0x709360}, {0x80ecd0, 0x80f0b0}}) == 0x6b8174068fca8cd1;
    Start(jumpHeight, jumpRecognised, {}, RedirectJumpDirection);
    SyncSafeTransition();
    HostExtensionRegister_nid_no_patch(OnKey, OnFrame);
    HostExtensionRegisterMenu_nid_no_patch(DrawMenu);
    return true;
}

[[maybe_unused]] const bool cheatsStarted = StartCheats();

}
#endif
