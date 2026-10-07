#ifdef _WIN32
#include <atomic>
#include <cctype>
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
#include <mutex>
#include <windows.h>
#include <tlhelp32.h>
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/HostExtension/include/HostExtension.hpp"
#include "prx/libSceAvPlayer/include/SafeTransition.hpp"

namespace {

// Sons of Sparta player cheats. A cheat's patches are in Il2cppUserAssemblies only while it is on: off is the game's own code, byte
// for byte. Offsets are module RVAs.

struct Site;

struct Cheat {
    const char* name;
    const char* label;
    const char* variable;
    std::atomic<int>* factor = nullptr;  // a multiplier's magnitude, used instead of active: 1 (off), 2, 4 or 6
    Site* sites[3] = {};
    std::size_t siteCount = 0;
    std::atomic<bool> active{false};
    bool available = false;  // its game code is recognised and its patches are built
    bool failed = false;     // it is off but its patch is still in the game
    std::atomic<const char*> notice{nullptr};
    std::atomic<std::uint64_t> noticeUntil{0};
};
std::atomic<int> damageFactor{1}, jumpFactor{1};
Cheat godMode{"God Mode", "God Mode", "SOS_GOD_MODE"};
Cheat infiniteSpirit{"Infinite Spartan Spirit", "Spartan Spirit", "SOS_INFINITE_SPARTAN_SPIRIT"};
Cheat infiniteMagic{"Infinite Magic", "Magic", "SOS_INFINITE_MAGIC"};
Cheat damageMultiplier{"Damage Multiplier", "Damage", "SOS_DAMAGE_MULTIPLIER", &damageFactor};
Cheat movementSpeed{"Movement Speed 2x", "Movement Speed", "SOS_MOVEMENT_SPEED"};
Cheat jumpHeight{"Jump Height", "Jump Height", "SOS_JUMP_HEIGHT_MULTIPLIER", &jumpFactor};
Cheat gatePass{"Pass Through Gates", "Pass Through Gates", "SOS_PASS_THROUGH_GATES"};
Cheat noKnockback{"No Knockback", "No Knockback", "SOS_NO_KNOCKBACK"};

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
Award bloodOrbs{"Blood Orbs", " +1000", kBloodOrb, std::size(kBloodOrb), 1000, true, kBloodOrbsQueued, kBloodOrbsRunning};
Award upgradeMaterials{"Upgrade Materials", " +10", kUpgradeMaterials, std::size(kUpgradeMaterials), 10, false, kUpgradeMaterialsQueued,
                       kUpgradeMaterialsRunning};
// Null while the keys work; written by whichever thread removes the wallet hook and read by the window thread.
std::atomic<const char*> awardsUnavailable{"game code not recognised"};
const char* const kUnavailable = ": unavailable";

// SDL scancodes
constexpr int kGodModeKey = 58, kInfiniteSpiritKey = 59, kInfiniteMagicKey = 60, kDamageMultiplierKey = 61, kMovementSpeedKey = 62, kJumpHeightKey = 63,
              kBloodOrbsKey = 64, kUpgradeMaterialsKey = 65, kGatePassKey = 66, kNoKnockbackKey = 67;
constexpr std::uint64_t kNoticeMs = 2000;
constexpr std::uint64_t kAwardWaitMs = 1000;

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint8_t* game = nullptr;
constexpr std::uint32_t kOffsetHp = 0xdab550;
constexpr std::uint32_t kOffsetGreyHp = 0xdabaf0;
constexpr std::uint32_t kOffsetSpirit = 0xda20b0;
constexpr std::uint32_t kManaClamp = 0xd95a0f;          // in ManaState.OffsetMana
constexpr std::uint32_t kHitDamageCall = 0xd91e1f;      // in Hittable.ExecuteHitEvent
constexpr std::uint32_t kHitImpulseCall = 0xd9249c;     // in Hittable.ExecuteHitEvent
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
constexpr std::uint32_t kRootMotionDeltaCall = 0x70693f;  // in PlayerMovement.TickPseudoPhysics
constexpr std::uint32_t kSlopeAligned = 0x6ecf20;
constexpr std::uint32_t kJumpDirectionCall = 0x6fc9c7;  // in MoveStateJump.<DelayStartJumping>d__27.MoveNext
constexpr std::uint32_t kJumpAngle = 0x6face0;
constexpr std::uint32_t kGateMoveCall = 0x706b02;        // in PlayerMovement.TickPseudoPhysics
constexpr std::uint32_t kMovePosition = 0x6edc60;
constexpr std::uint32_t kBlockedAheadHitCall = 0xc30dc2;  // in BooleanEvaluator_CollisionBlockedAhead.EvaluateFilter
constexpr std::uint32_t kHitCollider = 0x443bb00;
constexpr std::uint32_t kIgnoreCollision = 0x44273f0, kGetIgnoreCollision = 0x4427460, kGetBounds = 0x4441ab0;
constexpr std::uint32_t kGetName = 0x4378a50, kIsTrigger = 0x4441290, kEnabled = 0x436cf80, kMyPosition2D = 0x6ef180;
constexpr std::uint32_t kGameObjectOf = 0x436e240, kTransformOf = 0x436e1f0, kParentOf = 0x4384a10, kChildCount = 0x438a190, kChild = 0x438aa20;
constexpr std::uint32_t kComponentCount = 0x43724d0, kComponentAt = 0x4372580, kInstanceId = 0x4378440, kGetLayer = 0x4372720;
constexpr std::uint32_t kAttachedRigidbody = 0x443be00, kBodyType = 0x443d620;
constexpr std::uint32_t kPhysicsLock = 0x6ebd10;  // CharacterMovement.SetPhysicsLockState(lockId, enable)

constexpr std::size_t kNativeObject = 0x10;  // UnityEngine.Object.m_CachedPtr, null once destroyed
constexpr std::size_t kMaxHp = 0x38, kCurrentHp = 0x40, kGreyHp = 0x44;
constexpr std::size_t kCurrentSpirit = 0x20, kMaxSpirit = 0x24;
constexpr std::size_t kCurrentMana = 0x28, kMaxMana = 0x2c;
constexpr std::size_t kHitSource = 0x10, kHitTarget = 0x20, kHitConfig = 0x28, kHitArgs = 0x30;
constexpr std::size_t kDamageModule = 0x10, kDamageConfig = 0x10;
constexpr std::size_t kDamage = 0x10, kUnmodifiableDamage = 0x35;
constexpr std::size_t kApplyModuleSlot = 0x178;  // vtable slot: method pointer, then MethodInfo
constexpr std::size_t kApplyForceSlot = 0x238;   // MovementComponent.ApplyForce(Vector2)
constexpr std::size_t kWalletCount = 0x20;
constexpr std::size_t kInputDelta = 0xe0;
constexpr std::size_t kJumpContext = 0x58;
constexpr std::size_t kForwardAdjustment = 0x9c;
constexpr std::size_t kRawInputX = 0x2c, kMainCollider = 0x90, kFallingCollider = 0x168, kContactMap = 0x180, kVelocityFromDelta = 0xf8;
constexpr std::size_t kMovementStateMachine = 0x170, kActiveState = 0xe8, kClassName = 0x10;
constexpr std::size_t kLeftEdge = 0x18, kRightEdge = 0x20, kEdgeHits = 0x10;
constexpr std::size_t kHitSize = 0x24, kHitNormal = 0x10, kHitColliderId = 0x20;
// BlockedAheadHit gets the evaluator's RaycastHit2D at [rbp - 0x70]; its TryGetComponent<PlayerMovement> result is at [rbp - 0x48].
constexpr std::size_t kHitToPlayerMovement = 0x28;
constexpr float kGateInput = 0.5f;           // stick deflection that counts as moving toward a gate
constexpr float kGateClearance = 0.01f;      // how far past a gate edge the player must be
constexpr float kGateAbandonDistance = 1.0f;
constexpr float kTeleportDistance = 1.0f;
constexpr int kGateAwayTicks = 30;
constexpr std::uint64_t kGateRequestMs = 250, kGateStaleMs = 500;
constexpr int kMaxPassMembers = 8, kMaxPassDecisions = 32, kPassDepth = 4, kDefaultLayer = 0;

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
struct Bounds {
    float centerX, centerY, centerZ, extentX, extentY, extentZ;
};
using HitCollider = void*(APS5_VABI*)(void* hit, const void* method);
using IgnoreCollision = void(APS5_VABI*)(void* collider, void* other, bool ignore, const void* method);
using GetIgnoreCollision = bool(APS5_VABI*)(void* collider, void* other, const void* method);
using GetBounds = Bounds(APS5_VABI*)(void* collider, const void* method);
using ObjectGetter = void*(APS5_VABI*)(void* self, const void* method);
using BoolGetter = bool(APS5_VABI*)(void* self, const void* method);
using IntGetter = int(APS5_VABI*)(void* self, const void* method);
using ObjectAt = void*(APS5_VABI*)(void* self, int index, const void* method);
using Position = Vector2(APS5_VABI*)(void* movement, const void* method);
using MovePosition = void(APS5_VABI*)(void* movement, Vector2 delta, const void* method);
using ApplyForce = void(APS5_VABI*)(void* movement, Vector2 force, const void* method);

template <typename T> T& Field(void* object, std::size_t offset) { return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset); }
template <typename Function> Function Game(std::uint32_t offset) { return reinterpret_cast<Function>(game + offset); }

bool Alive(void* object) { return object != nullptr && Field<void*>(object, kNativeObject) != nullptr; }

bool OnPlayer(void* component) {
    const auto gameObject = Field<GetGameObject>(game, kGetGameObject);
    return Alive(component) && gameObject != nullptr && reinterpret_cast<IsPlayer>(game + kIsPlayer)(gameObject(component), nullptr);
}

// A patch is in the game only while its cheat is on, so the state checks below only matter for a call already in flight when it goes off.

// The green health bar is current minus grey health: keep both full and drop changes that would shrink it. The method still runs.
bool HoldFull(void* vitals, float* amount, bool grey) {
    if (!godMode.active.load(std::memory_order_relaxed) || !OnPlayer(vitals)) return false;
    Field<float>(vitals, kCurrentHp) = Field<float>(vitals, kMaxHp);
    Field<float>(vitals, kGreyHp) = 0.0f;
    if (grey ? *amount > 0.0f : *amount < 0.0f) *amount = 0.0f;
    return false;
}

bool APS5_VABI HealthFilter(void* vitals, void*, void*, float* amount) { return HoldFull(vitals, amount, false); }
bool APS5_VABI GreyHealthFilter(void* vitals, void*, void*, float* amount) { return HoldFull(vitals, amount, true); }

bool APS5_VABI SpiritFilter(void* spirit, void*, void*, float* amount) {
    if (!infiniteSpirit.active.load(std::memory_order_relaxed) || !OnPlayer(spirit)) return false;
    Field<float>(spirit, kCurrentSpirit) = Field<float>(spirit, kMaxSpirit);
    *amount = 0.0f;
    return false;
}

// Replaces OffsetMana's computation of the new mana; the method starts with a rip-relative read, so it cannot take an entry hook, and it
// still stores the result and raises its events.
float APS5_VABI ManaAfterOffset(void* mana, float amount) {
    float current = Field<float>(mana, kCurrentMana);
    const float max = Field<float>(mana, kMaxMana);
    if (infiniteMagic.active.load(std::memory_order_relaxed) && OnPlayer(mana)) {
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

// Replaces ExecuteHitEvent's ApplyForce(direction * strength) on the hit's target, its knockback. Only the horizontal push is removed, so a
// hit that launches upward still does.
void APS5_VABI HitImpulse(void* movement, Vector2 force, const void* method) {
    if (noKnockback.active.load(std::memory_order_relaxed) && OnPlayer(movement)) force.x = 0.0f;
    Field<ApplyForce>(Field<void*>(movement, 0), kApplyForceSlot)(movement, force, method);
}

bool InHitReaction(void* movement) {
    void* machine = Field<void*>(movement, kMovementStateMachine);
    void* state = Alive(machine) ? Field<void*>(machine, kActiveState) : nullptr;
    return Alive(state) && std::strcmp(Field<const char*>(Field<void*>(state, 0), kClassName), "CommonState_HitReaction") == 0;
}

// Replaces TickPseudoPhysics's slope alignment of the root motion delta. During the player's hit reaction that root motion is the
// reaction's own knockback, such as the contact-damage slide.
Vector2 APS5_VABI RootMotionDelta(void* movement, Vector2 delta) {
    if (noKnockback.active.load(std::memory_order_relaxed) && InHitReaction(movement) && OnPlayer(movement)) delta.x = 0.0f;
    return Game<SlopeAligned>(kSlopeAligned)(movement, delta, nullptr);
}

// Replaces TickPseudoPhysics's call to GetSlopeAlignedVersionOfMovementDelta, so only the input movement doubles: gravity, jumps,
// knockback and root motion are added after it.
Vector2 APS5_VABI MovementInputDelta(void* movement) {
    const Vector2 delta = Game<SlopeAligned>(kSlopeAligned)(movement, Field<Vector2>(movement, kInputDelta), nullptr);
    if (!movementSpeed.active.load(std::memory_order_relaxed) || !OnPlayer(movement)) return delta;
    return {delta.x * 2.0f, delta.y * 2.0f};
}

// Replaces the jump coroutine's call to GetJumpAngleAsForwardDirectionVector, whose result times the jump magnitude is the jump's one
// impulse. Scripted launches do not go through it.
Vector2 APS5_VABI JumpDirection(void* jumpState) {
    Vector2 direction = Game<JumpAngle>(kJumpAngle)(jumpState, Field<float>(jumpState, kForwardAdjustment), nullptr);
    const int factor = jumpFactor.load(std::memory_order_relaxed);
    if (factor == 1 || direction.y <= 0.0f || !OnPlayer(Field<void*>(jumpState, kJumpContext))) return direction;
    // A jump's height grows with the square of its upward speed, so the speed is scaled by the square root of the height factor.
    direction.y *= std::sqrt(static_cast<float>(factor));
    return direction;
}

void NameOf(void* object, char* out, std::size_t size) {
    std::size_t used = 0;
    void* name = Alive(object) ? Game<ObjectGetter>(kGetName)(object, nullptr) : nullptr;
    if (name != nullptr) {
        const auto* chars = reinterpret_cast<const char16_t*>(static_cast<std::uint8_t*>(name) + 0x14);
        for (int i = 0; i < Field<int>(name, 0x10) && used + 1 < size; ++i) out[used++] = chars[i] >= 32 && chars[i] < 127 ? static_cast<char>(chars[i]) : '?';
    }
    out[used] = '\0';
}

void* TransformOf(void* component) { return Alive(component) ? Game<ObjectGetter>(kTransformOf)(component, nullptr) : nullptr; }
void* ParentOf(void* transform) { return Alive(transform) ? Game<ObjectGetter>(kParentOf)(transform, nullptr) : nullptr; }
int InstanceId(void* object) { return Alive(object) ? Game<IntGetter>(kInstanceId)(object, nullptr) : 0; }

// RaycastHit2D.collider looks m_Collider up with Object.FindObjectFromInstanceID and returns it only as a Collider2D: null for any other
// object and once the collider is destroyed.
void* ColliderById(int id) {
    alignas(8) std::uint8_t hit[kHitSize] = {};
    std::memcpy(hit + kHitColliderId, &id, sizeof(id));
    return Game<HitCollider>(kHitCollider)(hit, nullptr);
}

bool Solid(void* collider) { return Alive(collider) && !Game<BoolGetter>(kIsTrigger)(collider, nullptr); }

enum : int { kNoPass, kGateFamily, kBarrierFamily, kBlockFamily, kColliderFamily };  // in priority order
enum : int { kNoBody = -1, kKinematicBody = 1 };                                      // RigidbodyType2D

int BodyOf(void* collider) {
    void* body = Alive(collider) ? Game<ObjectGetter>(kAttachedRigidbody)(collider, nullptr) : nullptr;
    return Alive(body) ? Game<IntGetter>(kBodyType)(body, nullptr) : kNoBody;
}

// Calls visit(word, length) for each word of an object name: separators split it, as does a change from lower to upper case or between
// letters and digits.
template <typename Visit> void ForEachWord(const char* name, Visit&& visit) {
    std::size_t start = 0, length = 0;
    for (std::size_t i = 0;; ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        const bool alphanumeric = std::isalnum(c) != 0;
        if (length > 0) {
            const auto previous = static_cast<unsigned char>(name[i - 1]);
            if (!alphanumeric || (std::islower(previous) && std::isupper(c)) || (std::isdigit(previous) != 0) != (std::isdigit(c) != 0)) {
                visit(name + start, length);
                length = 0;
            }
        }
        if (c == '\0') return;
        if (alphanumeric && length++ == 0) start = i;
    }
}

bool WordIs(const char* word, std::size_t length, const char* expected) {
    return std::strlen(expected) == length && _strnicmp(word, expected, length) == 0;
}

bool WordEndsWith(const char* word, std::size_t length, const char* suffix) {
    const std::size_t size = std::strlen(suffix);
    return length >= size && _strnicmp(word + length - size, suffix, size) == 0;
}

bool GateWord(const char* word, std::size_t length) { return WordIs(word, length, "gate") || WordIs(word, length, "gates"); }

bool BarrierWord(const char* word, std::size_t length) { return WordIs(word, length, "barrier") || WordIs(word, length, "barriers"); }

bool BlockWord(const char* word, std::size_t length) {
    return WordIs(word, length, "block") || WordIs(word, length, "blocks") || WordIs(word, length, "blocking") ||
           WordEndsWith(word, length, "blocker") || WordEndsWith(word, length, "blockers");
}

// Collision geometry is never named Collider, so this word names a barrier even on an object that holds nothing else.
bool ColliderWord(const char* word, std::size_t length) { return WordIs(word, length, "collider") || WordIs(word, length, "colliders"); }

bool PluralWord(const char* word, std::size_t length) {
    return WordIs(word, length, "gates") || WordIs(word, length, "barriers") || WordIs(word, length, "blocks") || WordEndsWith(word, length, "blockers") ||
           WordIs(word, length, "colliders");
}

bool StructuralWord(const char* word, std::size_t length) {
    for (const char* kind : {"wall", "walls", "ceiling", "ceilings", "floor", "floors", "ramp", "ramps"})
        if (WordIs(word, length, kind)) return true;
    return WordEndsWith(word, length, "ground") || WordEndsWith(word, length, "grounds");
}

struct NameSignal {
    int family = kNoPass;
    bool plural = false;       // Gates, Blocks, Colliders: a container of several barriers
    bool colliderOnly = true;  // no word but Collider / Colliders and numbers
    bool excluded = false;     // Collision, drop-through, hazard, ground, wall, ceiling, floor, ramp or volume
    bool dropthrough = false;
};

NameSignal Signal(const char* name) {
    NameSignal signal;
    char lower[64];
    std::size_t used = 0;
    for (; name[used] != '\0' && used + 1 < sizeof(lower); ++used) lower[used] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[used])));
    lower[used] = '\0';
    signal.dropthrough = std::strstr(lower, "dropthrough") != nullptr;
    signal.excluded = signal.dropthrough || std::strstr(lower, "collision") != nullptr;
    ForEachWord(name, [&](const char* word, std::size_t length) {
        const int family = GateWord(word, length)       ? kGateFamily
                           : BarrierWord(word, length)  ? kBarrierFamily
                           : BlockWord(word, length)    ? kBlockFamily
                           : ColliderWord(word, length) ? kColliderFamily
                                                        : kNoPass;
        if (family != kNoPass && (signal.family == kNoPass || family < signal.family)) {
            signal.family = family;
            signal.plural = PluralWord(word, length);
        }
        signal.colliderOnly = signal.colliderOnly && (family == kColliderFamily || std::isdigit(static_cast<unsigned char>(word[0])) != 0);
        signal.excluded = signal.excluded || StructuralWord(word, length) || WordIs(word, length, "hazard") || WordIs(word, length, "hazards") ||
                          WordIs(word, length, "volume") || WordIs(word, length, "volumes");
    });
    return signal;
}

struct PassDecision {
    int id = 0, family = kNoPass, key = 0, rootLevel = 0;
};

// A kinematic block or collider is typically a moving platform: ignoring it can take the ground from under the player.
bool KinematicRejected(int family, int body) { return (family == kBlockFamily || family == kColliderFamily) && body == kKinematicBody; }

// Gate, barrier and block names count on the collider's object or on a singular ancestor; a collider name only on the object itself or on a
// parent named nothing but Collider(s), because level geometry sits under containers such as Sparta_colliders. The root is the highest
// object of the matching chain; key is its instance ID.
PassDecision Identify(void* collider, int id) {
    PassDecision decision{id};
    void* transforms[kPassDepth] = {};
    NameSignal signals[kPassDepth];
    int levels = 0;
    for (void* transform = TransformOf(collider); levels < kPassDepth && Alive(transform); transform = ParentOf(transform), ++levels) {
        char name[64];
        NameOf(transform, name, sizeof(name));
        transforms[levels] = transform;
        signals[levels] = Signal(name);
    }
    void* gameObject = Game<ObjectGetter>(kGameObjectOf)(collider, nullptr);
    if (levels == 0 || !Alive(gameObject) || Game<IntGetter>(kGetLayer)(gameObject, nullptr) != kDefaultLayer || signals[0].excluded) return decision;
    for (int level = 1; level < levels; ++level)
        if (signals[level].dropthrough) return decision;
    int family = kNoPass, anchor = -1;
    for (int level = 0; level < levels; ++level) {
        const NameSignal& signal = signals[level];
        const bool counts = level == 0 || (!signal.excluded && (signal.family == kColliderFamily ? level == 1 && signal.colliderOnly : !signal.plural));
        if (signal.family != kNoPass && counts && (anchor < 0 || signal.family < family)) {
            family = signal.family;
            anchor = level;
        }
    }
    if (anchor < 0 || KinematicRejected(family, BodyOf(collider))) return decision;
    int root = anchor;
    while (family != kColliderFamily && root + 1 < levels && signals[root + 1].family == family && !signals[root + 1].plural && !signals[root + 1].excluded) {
        ++root;
    }
    decision.family = family;
    decision.rootLevel = root;
    decision.key = InstanceId(transforms[root]);
    return decision;
}

// What each collider was found to be in this scene, so a wall pressed against every frame is named once. Cleared at every world
// transition, because instance IDs may name other objects in the next scene.
PassDecision decisions[kMaxPassDecisions];
int nextDecision = 0;

PassDecision Decide(void* collider, int id) {
    if (!Solid(collider) || id == 0) return {id};
    for (const PassDecision& decision : decisions)
        if (decision.id == id) return decision;
    const PassDecision decision = Identify(collider, id);
    decisions[nextDecision] = decision;
    nextDecision = (nextDecision + 1) % kMaxPassDecisions;
    return decision;
}

void Reject(int id) {
    for (PassDecision& decision : decisions)
        if (decision.id == id) decision.family = kNoPass;
}

struct Span {
    float min, max;
};

Span HorizontalSpan(void* collider) {
    const Bounds bounds = Game<GetBounds>(kGetBounds)(collider, nullptr);
    return {bounds.centerX - bounds.extentX, bounds.centerX + bounds.extentX};
}

Span Union(Span a, Span b) { return {b.min < a.min ? b.min : a.min, b.max > a.max ? b.max : a.max}; }

// Game-thread state of Pass Through Gates. Players and gate parts are kept as identities and instance IDs, never as object pointers.
struct GateCrossing {
    std::uintptr_t player = 0;
    int family = kNoPass;
    int key = 0;
    int direction = 0;    // -1 to the left, 1 to the right
    int players[2] = {};  // main and falling collider
    int members[kMaxPassMembers] = {};
    int memberCount = 0;
    bool original[2][kMaxPassMembers] = {};  // each pair's ignore state before the crossing
    std::uint64_t lastTick = 0;
    Vector2 position{}, delta{};
    int awayTicks = 0;
};
GateCrossing crossing;
struct GateRequest {
    std::uintptr_t player = 0;
    int seed = 0;
    int direction = 0;
    std::uint64_t at = 0;
};
GateRequest request;

bool CrossingMember(int id) {
    for (int m = 0; m < crossing.memberCount; ++m)
        if (crossing.members[m] == id) return true;
    return false;
}

// Colliders beyond the limit stay solid.
void AddMember(int id) {
    if (id != 0 && !CrossingMember(id) && crossing.memberCount < kMaxPassMembers) crossing.members[crossing.memberCount++] = id;
}

void AddSolidColliders(void* transform, int family) {
    void* gameObject = Alive(transform) ? Game<ObjectGetter>(kGameObjectOf)(transform, nullptr) : nullptr;
    const int components = Alive(gameObject) ? Game<IntGetter>(kComponentCount)(gameObject, nullptr) : 0;
    for (int i = 0; i < components; ++i) {
        const int id = InstanceId(Game<ObjectAt>(kComponentAt)(gameObject, i, nullptr));
        void* member = id != 0 ? ColliderById(id) : nullptr;
        if (Solid(member) && Game<BoolGetter>(kEnabled)(member, nullptr) && !KinematicRejected(family, BodyOf(member))) AddMember(id);
    }
}

// The root's own colliders and those of its direct children, except children named as level geometry.
void AddFamily(void* root, int family) {
    AddSolidColliders(root, family);
    const int children = Alive(root) ? Game<IntGetter>(kChildCount)(root, nullptr) : 0;
    for (int i = 0; i < children; ++i) {
        void* child = Game<ObjectAt>(kChild)(root, i, nullptr);
        char name[64];
        NameOf(child, name, sizeof(name));
        if (!Signal(name).excluded) AddSolidColliders(child, family);
    }
}

// The live player colliders' X extent; the falling collider counts only while enabled.
bool CrossingPlayerSpan(Span* span) {
    void* main = ColliderById(crossing.players[0]);
    if (!Alive(main)) return false;
    *span = HorizontalSpan(main);
    void* falling = ColliderById(crossing.players[1]);
    if (Alive(falling) && Game<BoolGetter>(kEnabled)(falling, nullptr)) *span = Union(*span, HorizontalSpan(falling));
    return true;
}

// The X extent of the gate's live parts; returns how many parts are live.
int FamilySpan(Span* family) {
    int live = 0;
    for (int m = 0; m < crossing.memberCount; ++m) {
        void* member = ColliderById(crossing.members[m]);
        if (!Alive(member)) continue;
        const Span span = HorizontalSpan(member);
        *family = live == 0 ? span : Union(*family, span);
        ++live;
    }
    return live;
}

// Sets every live player / gate pair of the crossing to ignored, or back to its state before the crossing.
void SetPairs(bool ignore) {
    for (int p = 0; p < 2; ++p) {
        void* collider = ColliderById(crossing.players[p]);
        for (int m = 0; Alive(collider) && m < crossing.memberCount; ++m) {
            void* member = ColliderById(crossing.members[m]);
            if (Alive(member)) Game<IgnoreCollision>(kIgnoreCollision)(collider, member, ignore || crossing.original[p][m], nullptr);
        }
    }
}

// Ignored pairs of one player collider, or -1 without that collider.
int IgnoredPairs(int p) {
    void* collider = ColliderById(crossing.players[p]);
    if (!Alive(collider)) return -1;
    int ignored = 0;
    for (int m = 0; m < crossing.memberCount; ++m) {
        void* member = ColliderById(crossing.members[m]);
        if (Alive(member) && Game<GetIgnoreCollision>(kGetIgnoreCollision)(collider, member, nullptr)) ++ignored;
    }
    return ignored;
}

// Replaces CollisionBlockedAhead's hit.collider. For the player with Pass Through Gates on and the stick pointing into it, a gate hit reads
// as no hit, so the movement states keep moving toward the gate; any other evaluation sees the original collider.
void* APS5_VABI BlockedAheadHit(std::uint8_t* hit) {
    void* collider = Game<HitCollider>(kHitCollider)(hit, nullptr);
    void* movement = Field<void*>(hit, kHitToPlayerMovement);
    if (collider == nullptr || movement == nullptr || !gatePass.active.load(std::memory_order_relaxed)) return collider;
    const int id = Field<int>(hit, kHitColliderId);
    const auto player = reinterpret_cast<std::uintptr_t>(movement);
    if (crossing.player == player && CrossingMember(id)) return nullptr;
    if (!Alive(movement)) return collider;
    const float input = Field<float>(movement, kRawInputX);
    if (std::fabs(input) < kGateInput || input * Field<Vector2>(hit, kHitNormal).x >= 0.0f || !OnPlayer(movement)) return collider;
    if (Decide(collider, id).family == kNoPass) return collider;
    request = {player, id, input < 0.0f ? -1 : 1, NowMs()};
    return nullptr;
}

// The first collider the game's contact map finds on the side the stick points to, e.g. a gate the player jumped into.
int SideHit(void* movement, float input) {
    void* map = Field<void*>(movement, kContactMap);
    void* edge = map != nullptr ? Field<void*>(map, input < 0.0f ? kLeftEdge : kRightEdge) : nullptr;
    void* hits = edge != nullptr ? Field<void*>(edge, kEdgeHits) : nullptr;
    auto* items = hits != nullptr ? Field<std::uint8_t*>(hits, 0x10) : nullptr;
    const int count = hits != nullptr ? Field<int>(hits, 0x18) : 0;
    for (int i = 0; items != nullptr && i < count; ++i) {
        std::uint8_t* hit = items + 0x20 + static_cast<std::size_t>(i) * kHitSize;
        const int id = Field<int>(hit, kHitColliderId);
        if (id != 0) return input * Field<Vector2>(hit, kHitNormal).x < 0.0f ? id : 0;
    }
    return 0;
}

void BeginCrossing(void* movement, int seed, int direction, std::uint64_t now) {
    void* seedCollider = ColliderById(seed);
    const PassDecision decision = Decide(seedCollider, seed);
    if (decision.family == kNoPass || !Alive(Field<void*>(movement, kMainCollider))) return;
    if (KinematicRejected(decision.family, BodyOf(seedCollider))) {
        Reject(seed);
        return;
    }
    void* root = TransformOf(seedCollider);
    for (int level = 0; level < decision.rootLevel; ++level) root = ParentOf(root);
    if (InstanceId(root) != decision.key) return;
    crossing = GateCrossing{};
    crossing.player = reinterpret_cast<std::uintptr_t>(movement);
    crossing.family = decision.family;
    crossing.key = decision.key;
    crossing.direction = direction;
    crossing.lastTick = now;
    crossing.position = Game<Position>(kMyPosition2D)(movement, nullptr);
    crossing.players[0] = InstanceId(Field<void*>(movement, kMainCollider));
    crossing.players[1] = InstanceId(Field<void*>(movement, kFallingCollider));
    AddMember(seed);
    AddFamily(root, decision.family);
    for (int p = 0; p < 2; ++p) {
        void* collider = ColliderById(crossing.players[p]);
        for (int m = 0; Alive(collider) && m < crossing.memberCount; ++m) {
            void* member = ColliderById(crossing.members[m]);
            crossing.original[p][m] = Alive(member) && Game<GetIgnoreCollision>(kGetIgnoreCollision)(collider, member, nullptr);
        }
    }
    SetPairs(true);
}

void EndCrossing() {
    SetPairs(false);
    crossing = GateCrossing{};
}

void ContinueCrossing(void* movement, Vector2 delta, std::uint64_t now, bool on) {
    const Vector2 position = Game<Position>(kMyPosition2D)(movement, nullptr);
    Span player{}, family{};
    const bool playerLive = InstanceId(Field<void*>(movement, kMainCollider)) == crossing.players[0] &&
                            InstanceId(Field<void*>(movement, kFallingCollider)) == crossing.players[1] && CrossingPlayerSpan(&player);
    const int live = playerLive ? FamilySpan(&family) : 0;
    const bool teleported = std::fabs(position.x - crossing.position.x - crossing.delta.x) > kTeleportDistance ||
                            std::fabs(position.y - crossing.position.y - crossing.delta.y) > kTeleportDistance;
    // A tick gap means a world transition, physics lock or pause.
    bool end = !on || now - crossing.lastTick > kGateStaleMs || !playerLive || live < crossing.memberCount || teleported;
    if (!end) {
        const bool past = crossing.direction < 0 ? player.max < family.min - kGateClearance : player.min > family.max + kGateClearance;
        const float before = crossing.direction < 0 ? player.min - family.max : family.min - player.max;
        if (past) {
            end = true;
        } else if (before > kGateClearance) {
            const bool toward = Field<float>(movement, kRawInputX) * static_cast<float>(crossing.direction) >= kGateInput;
            crossing.awayTicks = toward ? 0 : crossing.awayTicks + 1;
            end = crossing.awayTicks >= kGateAwayTicks || before > kGateAbandonDistance;
        } else {
            crossing.awayTicks = 0;
        }
    }
    if (end) {
        EndCrossing();
        return;
    }
    const int falling = IgnoredPairs(1);
    if (IgnoredPairs(0) < live || (falling >= 0 && falling < live)) SetPairs(true);
    crossing.lastTick = now;
    crossing.position = position;
    crossing.delta = delta;
}

void GatePassTick(void* movement, Vector2 delta) {
    const auto player = reinterpret_cast<std::uintptr_t>(movement);
    const std::uint64_t now = NowMs();
    if (crossing.family != kNoPass && crossing.player != player) {
        if (now - crossing.lastTick < kGateStaleMs) return;
        EndCrossing();
    }
    const bool on = gatePass.active.load(std::memory_order_relaxed);
    if (crossing.family != kNoPass) {
        ContinueCrossing(movement, delta, now, on);
        return;
    }
    const GateRequest pending = request;
    request = GateRequest{};
    if (!on) return;
    const float input = Field<float>(movement, kRawInputX);
    if (pending.seed != 0 && pending.player == player && now - pending.at <= kGateRequestMs) {
        BeginCrossing(movement, pending.seed, pending.direction, now);
    } else if (std::fabs(input) >= kGateInput && OnPlayer(movement)) {
        const int id = SideHit(movement, input);
        if (id != 0 && Decide(ColliderById(id), id).family != kNoPass) BeginCrossing(movement, id, input < 0.0f ? -1 : 1, now);
    }
    if (crossing.family != kNoPass) crossing.delta = delta;
}

// Replaces TickPseudoPhysics's MovePosition(delta) and its following lastFrameVelocityFromInverseDelta = delta / deltaTime.
void APS5_VABI GateMove(void* movement, Vector2 delta, float deltaTime) {
    GatePassTick(movement, delta);
    Game<MovePosition>(kMovePosition)(movement, delta, nullptr);
    Field<Vector2>(movement, kVelocityFromDelta) = {delta.x / deltaTime, delta.y / deltaTime};
}

// Instance IDs may name other objects in the next scene: the crossing ends while the old scene is still intact.
void ForgetGates() {
    if (crossing.family != kNoPass) EndCrossing();
    request = GateRequest{};
    for (PassDecision& decision : decisions) decision = PassDecision{};
    nextDecision = 0;
}

bool WorldTransitionLock(void* lockId) {
    constexpr char kLock[] = "WorldTransitioning";
    if (lockId == nullptr || Field<int>(lockId, 0x10) != static_cast<int>(sizeof(kLock) - 1)) return false;
    const auto* chars = reinterpret_cast<const char16_t*>(static_cast<std::uint8_t*>(lockId) + 0x14);
    for (std::size_t i = 0; i + 1 < sizeof(kLock); ++i)
        if (chars[i] != static_cast<char16_t>(kLock[i])) return false;
    return true;
}

// Observes SetPhysicsLockState: the player's WorldTransitioning lock turns on before the old scene is torn down.
void APS5_VABI PhysicsLockChanged(void* movement, void* lockId, std::uintptr_t enable) {
    if ((enable & 0xff) != 0 && WorldTransitionLock(lockId) && OnPlayer(movement)) ForgetGates();
}

void Notify(Award& award, const char* failure, const char* item) {
    if (failure == nullptr) {
        std::fprintf(stderr, "%s:%s\n", award.name, award.added);
    } else if (item == nullptr) {
        std::fprintf(stderr, "%s: unavailable (%s)\n", award.name, failure);
    } else {
        std::fprintf(stderr, "%s: unavailable (%s: %s)\n", award.name, failure, item);
    }
    award.notice.store(failure != nullptr ? kUnavailable : award.added);
    award.noticeUntil.store(NowMs() + kNoticeMs);
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

// The hook that services an action is installed only while one is pending, so PlayerController.Update is the game's own empty method at
// rest. Both are defined with the patch mechanism, below.
const char* InstallWalletHook();
void RemoveWalletHook();

__attribute__((noinline)) void RunWalletAction(void* controller) {
    int state = walletAction.load(std::memory_order_relaxed);
    if (state != kBloodOrbsQueued && state != kUpgradeMaterialsQueued) return;
    Award& award = state == kBloodOrbsQueued ? bloodOrbs : upgradeMaterials;
    if (!walletAction.compare_exchange_strong(state, award.running)) return;  // given up meanwhile
    const char* item = nullptr;
    const char* const failure = Give(award, controller, &item);
    RemoveWalletHook();                // before the action is given back, so a new request cannot install while this one is removed
    walletAction.store(kIdle);
    Notify(award, failure, item);
}

// PlayerController.Update, on the game thread, for as long as one action is pending. The run is a separate function because a SysV
// function here cannot have cleanups.
bool APS5_VABI ControllerFilter(void* controller, void*, void*, float*) {
    RunWalletAction(controller);
    return false;
}

// A queued action that no player's controller takes in time fails: no player in the game, or no frames.
void ExpireRequest(std::uint64_t now) {
    int state = walletAction.load();
    if ((state != kBloodOrbsQueued && state != kUpgradeMaterialsQueued) || now < walletRequestedAt.load() + kAwardWaitMs) return;
    if (!walletAction.compare_exchange_strong(state, kIdle)) return;  // the game thread took it meanwhile
    RemoveWalletHook();
    Award& award = state == kBloodOrbsQueued ? bloodOrbs : upgradeMaterials;
    Notify(award, "no player in the game", nullptr);
}

// F7 or F8: queues its action unless a wallet action is queued or running. The expiry also runs here because the title, which runs it
// otherwise, is only updated while frames are presented.
void Request(Award& award) {
    if (const char* const unavailable = awardsUnavailable.load()) {
        Notify(award, unavailable, nullptr);
        return;
    }
    const std::uint64_t now = NowMs();
    ExpireRequest(now);
    if (walletAction.load() != kIdle) return;
    walletRequestedAt.store(now);      // before the action is queued, so the expiry never reads an older time
    walletAction.store(award.queued);  // only this thread leaves idle
    if (const char* const failure = InstallWalletHook()) {
        walletAction.store(kIdle);
        Notify(award, failure, nullptr);
    }
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

struct Range { std::uint32_t begin, end; };

// FNV-1a of relocation-free code, so it only matches the analysed game build; the ranges include every byte a patch replaces.
std::uint64_t Hash(std::initializer_list<Range> code) {
    std::uint64_t hash = 0xcbf29ce484222325;
    for (const auto& range : code) {
        for (std::uint32_t offset = range.begin; offset < range.end; ++offset) hash = (hash ^ game[offset]) * 0x100000001b3;
    }
    return hash;
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
// vmovaps xmm0, [rbp - 0x20]; mov rdi, rbx; call MovePosition; vbroadcastss xmm0, [rbp - 0x50]; vmovaps xmm1, [rbp - 0x20]; vdivps;
// vmovlps [rbx + 0xf8], xmm0, rewritten to load deltaTime into xmm1 instead and call GateMove
constexpr std::uint8_t kGateMoveCallBytes[] = {0xc5, 0xf8, 0x28, 0x45, 0xe0, 0x48, 0x89, 0xdf, 0xe8, 0x51, 0x71, 0xfe, 0xff, 0xc4, 0xe2, 0x79, 0x18, 0x45,
                                               0xb0, 0xc5, 0xf8, 0x28, 0x4d, 0xe0, 0xc5, 0xf0, 0x5e, 0xc0, 0xc5, 0xf8, 0x13, 0x83, 0xf8, 0x00, 0x00, 0x00};
// call RaycastHit2D.get_collider: only the call's target changes, so it is redirected through a jump near the module
constexpr std::uint8_t kBlockedAheadHitBytes[] = {0xe8, 0x39, 0xad, 0x80, 0x03};
// call qword ptr [rax + 0x238], the target's ApplyForce with its arguments in place: redirected through a jump near the module as well
constexpr std::uint8_t kHitImpulseCallBytes[] = {0xff, 0x90, 0x38, 0x02, 0x00, 0x00};
// call GetSlopeAlignedVersionOfMovementDelta(rootMotion.netVelocity * deltaTime)
constexpr std::uint8_t kRootMotionDeltaCallBytes[] = {0xe8, 0xdc, 0x65, 0xfe, 0xff};

constexpr std::size_t kMaxPatch = 40;

// One stretch of game code a cheat replaces. original and patched are built once, while the game code is recognised; nothing is written to
// the game until the cheat is turned on, and turning it off puts original back.
struct Site {
    std::uint32_t rva;
    std::uint8_t size;
    std::uint8_t returnTo = 0;  // offset the installed call returns into; 0 when nothing returns into the site
    bool installed = false;
    std::uint8_t original[kMaxPatch] = {};
    std::uint8_t patched[kMaxPatch] = {};
};
Site healthSite{kOffsetHp, kEntryJump};
Site greyHealthSite{kOffsetGreyHp, kEntryJump};
Site spiritSite{kOffsetSpirit, kEntryJump};
Site manaSite{kManaClamp, sizeof(kManaClampBytes)};
Site hitDamageSite{kHitDamageCall, sizeof(kHitDamageCallBytes)};
Site hitImpulseSite{kHitImpulseCall, sizeof(kHitImpulseCallBytes)};
Site rootMotionSite{kRootMotionDeltaCall, sizeof(kRootMotionDeltaCallBytes)};
Site inputDeltaSite{kInputDeltaCall, sizeof(kInputDeltaCallBytes)};
Site jumpDirectionSite{kJumpDirectionCall, sizeof(kJumpDirectionCallBytes)};
Site gateMoveSite{kGateMoveCall, sizeof(kGateMoveCallBytes)};
Site blockedAheadSite{kBlockedAheadHitCall, sizeof(kBlockedAheadHitBytes)};
Site physicsLockSite{kPhysicsLock, kEntryJump};
Site walletSite{kControllerUpdate, kEntryJump};
Site* const kWalletSites[] = {&walletSite};

// Every site above, so that no cheat can restore bytes that belong to another one.
struct Patched { std::uint32_t begin, size; };
constexpr Patched kPatched[] = {
    {kOffsetHp, kEntryJump}, {kOffsetGreyHp, kEntryJump}, {kOffsetSpirit, kEntryJump}, {kManaClamp, sizeof(kManaClampBytes)},
    {kHitDamageCall, sizeof(kHitDamageCallBytes)}, {kHitImpulseCall, sizeof(kHitImpulseCallBytes)},
    {kRootMotionDeltaCall, sizeof(kRootMotionDeltaCallBytes)}, {kInputDeltaCall, sizeof(kInputDeltaCallBytes)},
    {kJumpDirectionCall, sizeof(kJumpDirectionCallBytes)}, {kGateMoveCall, sizeof(kGateMoveCallBytes)},
    {kBlockedAheadHitCall, sizeof(kBlockedAheadHitBytes)}, {kPhysicsLock, kEntryJump}, {kControllerUpdate, kEntryJump}};
constexpr bool Valid() {
    for (std::size_t i = 0; i < std::size(kPatched); ++i) {
        if (kPatched[i].size > kMaxPatch) return false;
        for (std::size_t j = i + 1; j < std::size(kPatched); ++j) {
            if (kPatched[i].begin < kPatched[j].begin + kPatched[j].size && kPatched[j].begin < kPatched[i].begin + kPatched[i].size) return false;
        }
    }
    return true;
}
static_assert(Valid(), "cheat patches must fit in a site and must not overlap");

void GateMoveCall(std::uint8_t (&call)[sizeof(kGateMoveCallBytes)]) {
    // vmovaps xmm0, [rbp - 0x20]; vmovss xmm1, [rbp - 0x50] (deltaTime); mov rdi, rbx; mov rax, GateMove; call rax; nops
    constexpr std::uint8_t code[sizeof(kGateMoveCallBytes)] = {0xc5, 0xf8, 0x28, 0x45, 0xe0, 0xc5, 0xfa, 0x10, 0x4d, 0xb0, 0x48, 0x89, 0xdf, 0x48, 0xb8, 0, 0, 0,
                                                               0,    0,    0,    0,    0,    0xff, 0xd0, 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x1f, 0x40, 0x00};
    std::memcpy(call, code, sizeof(code));
    const auto function = &GateMove;
    std::memcpy(call + 15, &function, sizeof(function));
}

// push rbp; mov rbp, rsp; push r15; push r14; push r13; push r12; push rbx; push rax
constexpr std::uint8_t kPhysicsLockPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50};

using Observer = void(APS5_VABI*)(void* self, void* argument, std::uintptr_t argument2);

// Entry hook that only observes: every argument register is saved around observer(rdi, rsi, rdx), then the method's copied first
// instructions run and the method continues after them, so those must be whole instructions with no rip-relative operand.
constexpr std::uint8_t kObserveStart[] = {
    0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51,                          // push rdi, rsi, rdx, rcx, r8, r9
    0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00,                                // sub rsp, 0x88
    0xc5, 0xfa, 0x7f, 0x04, 0x24, 0xc5, 0xfa, 0x7f, 0x4c, 0x24, 0x10,        // vmovdqu [rsp], xmm0; [rsp + 0x10], xmm1
    0xc5, 0xfa, 0x7f, 0x54, 0x24, 0x20, 0xc5, 0xfa, 0x7f, 0x5c, 0x24, 0x30,  // xmm2, xmm3
    0xc5, 0xfa, 0x7f, 0x64, 0x24, 0x40, 0xc5, 0xfa, 0x7f, 0x6c, 0x24, 0x50,  // xmm4, xmm5
    0xc5, 0xfa, 0x7f, 0x74, 0x24, 0x60, 0xc5, 0xfa, 0x7f, 0x7c, 0x24, 0x70,  // xmm6, xmm7
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,                                      // mov rax, observer (offset 64)
    0xff, 0xd0,                                                              // call rax
    0xc5, 0xfa, 0x6f, 0x04, 0x24, 0xc5, 0xfa, 0x6f, 0x4c, 0x24, 0x10,        // restore xmm0..xmm7
    0xc5, 0xfa, 0x6f, 0x54, 0x24, 0x20, 0xc5, 0xfa, 0x6f, 0x5c, 0x24, 0x30,
    0xc5, 0xfa, 0x6f, 0x64, 0x24, 0x40, 0xc5, 0xfa, 0x6f, 0x6c, 0x24, 0x50,
    0xc5, 0xfa, 0x6f, 0x74, 0x24, 0x60, 0xc5, 0xfa, 0x6f, 0x7c, 0x24, 0x70,
    0x48, 0x81, 0xc4, 0x88, 0x00, 0x00, 0x00,                                // add rsp, 0x88
    0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f,                          // pop r9, r8, rcx, rdx, rsi, rdi
};
constexpr std::size_t kObserveFunction = 64;
static_assert(kObserveStart[kObserveFunction - 2] == 0x48 && kObserveStart[kObserveFunction - 1] == 0xb8 && sizeof(kObserveStart) == 136);

// A rel32 call reaches only 2 GB, so the evaluator's call goes through a jump allocated below the game module.
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

constexpr std::size_t kMaxPrologue = 14;
constexpr std::size_t kMaxStart = sizeof(kObserveStart) > sizeof(kHookStart) ? sizeof(kObserveStart) : sizeof(kHookStart);
constexpr std::size_t kTrampolineSlot = kMaxStart + kMaxPrologue + sizeof(kJumpBack) + sizeof(void*);
constexpr std::size_t kHooks = 5;  // health, grey health, spirit, the physics lock observer and the wallet actions
std::uint8_t* trampolines = nullptr;
std::size_t trampolinesUsed = 0;

constexpr std::size_t kMaxStopped = 256;
constexpr std::size_t kMaxStackScan = 1 << 20;  // a backstop; the scan normally ends at the top of the thread's stack
constexpr int kTransitionTries = 20;
constexpr DWORD kTransitionWaitMs = 2;

struct Stopped {
    HANDLE threads[kMaxStopped];
    std::size_t count = 0;
};

void Resume(Stopped& stopped) {
    while (stopped.count > 0) {
        const HANDLE thread = stopped.threads[--stopped.count];
        ResumeThread(thread);
        CloseHandle(thread);
    }
}

// Game code is rewritten only while every other thread is stopped. Nothing that takes a user-mode lock may run before Resume, because a
// stopped thread can hold the heap or stdio lock: the snapshot is taken and read before the first thread is stopped, and only system calls
// that stay in the kernel follow.
bool Stop(Stopped& stopped) {
    DWORD ids[kMaxStopped];
    std::size_t count = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    bool room = true;
    for (BOOL more = Thread32First(snapshot, &entry); more && room; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        room = count < kMaxStopped;
        if (room) ids[count++] = entry.th32ThreadID;
    }
    CloseHandle(snapshot);
    if (!room) return false;
    for (std::size_t i = 0; i < count; ++i) {
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, ids[i]);
        if (thread == nullptr) continue;  // ended between the snapshot and here, so it is not in the code either
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
            CloseHandle(thread);
            return false;
        }
        stopped.threads[stopped.count++] = thread;
    }
    return true;
}

bool Inside(std::uintptr_t address, Site* const sites[], std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const auto begin = reinterpret_cast<std::uintptr_t>(game + sites[i]->rva);
        if (address >= begin && address < begin + sites[i]->size) return true;
    }
    return false;
}

bool Returns(std::uintptr_t address, Site* const sites[], std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (sites[i]->returnTo != 0 && address == reinterpret_cast<std::uintptr_t>(game + sites[i]->rva + sites[i]->returnTo)) return true;
    }
    return false;
}

// A thread stopped inside the bytes about to be replaced would resume in the middle of an instruction that no longer exists, and so would
// one inside a replacement called from a rewritten site, because the call returns into the site at an offset that is not an instruction
// boundary of the original sequence. Such a thread is found by the one return address it must hold, in the live stack above its stack
// pointer; an entry hook returns through its trampoline instead, so there is nothing to look for.
bool Outside(const Stopped& stopped, Site* const sites[], std::size_t count) {
    bool returned = false;
    for (std::size_t s = 0; s < count; ++s) returned = returned || sites[s]->returnTo != 0;
    for (std::size_t i = 0; i < stopped.count; ++i) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(stopped.threads[i], &context)) return false;
        if (Inside(static_cast<std::uintptr_t>(context.Rip), sites, count)) return false;
        if (!returned) continue;
        const std::uintptr_t stack = static_cast<std::uintptr_t>(context.Rsp) & ~std::uintptr_t{sizeof(void*) - 1};
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<void*>(stack), &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        const auto committed = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        const std::uintptr_t end = committed < stack + kMaxStackScan ? committed : stack + kMaxStackScan;
        for (std::uintptr_t slot = stack; slot + sizeof(void*) <= end; slot += sizeof(void*)) {
            if (Returns(*reinterpret_cast<const std::uintptr_t*>(slot), sites, count)) return false;
        }
    }
    return true;
}

// Writes to is refused unless the site still holds from: bytes that are neither the game's nor ours must not be overwritten.
bool Write(Site& site, const std::uint8_t* from, const std::uint8_t* to) {
    std::uint8_t* const at = game + site.rva;
    if (std::memcmp(at, from, site.size) != 0) return false;
    DWORD protection = 0;
    if (!VirtualProtect(at, site.size, PAGE_EXECUTE_READWRITE, &protection)) return false;
    std::memcpy(at, to, site.size);
    const bool written = std::memcmp(at, to, site.size) == 0;
    VirtualProtect(at, site.size, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), at, site.size);
    return written;
}

// Busy is worth another try later; Mismatch means the code at the site is not the code this cheat knows, so it is left alone.
enum class Result { Done, Busy, Mismatch };

// Only one thread may be between Stop and Resume: a key press and a finished wallet action can arrive together, and two threads stopping
// each other would both stay stopped. It is taken before any thread is stopped, so a stopped thread never holds it.
std::mutex transitions;

// Installs or removes every site of one cheat together: a thread in any of them postpones the whole transition, and a site that cannot be
// written rolls the earlier ones back. stop is false only before the game runs, when no thread can be in the code yet.
Result Transition(Site* const sites[], std::size_t count, bool install, bool stop) {
    const std::lock_guard lock(transitions);
    for (int attempt = 0; attempt < (stop ? kTransitionTries : 1); ++attempt) {
        Stopped stopped;
        const bool ready = !stop || (Stop(stopped) && Outside(stopped, sites, count));
        std::size_t written = 0;
        if (ready) {
            for (; written < count; ++written) {
                Site& site = *sites[written];
                if (!Write(site, install ? site.original : site.patched, install ? site.patched : site.original)) break;
            }
            if (written < count) {
                for (std::size_t i = 0; i < written; ++i) {
                    Site& site = *sites[i];
                    Write(site, install ? site.patched : site.original, install ? site.original : site.patched);
                }
            }
        }
        Resume(stopped);
        if (ready) {
            if (written < count) return Result::Mismatch;
            for (std::size_t i = 0; i < count; ++i) sites[i]->installed = install;
            return Result::Done;
        }
        Sleep(kTransitionWaitMs);
    }
    return Result::Busy;
}

// Builds a trampoline and the entry jump to it without touching the game. start is the preamble that calls function, at is where its
// address goes, and prologue the method's first whole instructions, at least kEntryJump bytes, with no rip-relative operand and no jump
// into them; a shorter (empty) method is copied whole and the entry jump overwrites the padding after it.
template <typename Function>
bool PrepareEntry(Site& site, const std::uint8_t* start, std::size_t startSize, std::size_t at, Function function, std::size_t prologue) {
    if (trampolines == nullptr || trampolinesUsed == kHooks || prologue > kMaxPrologue) return false;
    std::uint8_t* const trampoline = trampolines + kTrampolineSlot * trampolinesUsed++;
    std::uint8_t* const method = game + site.rva;
    const void* const resume = method + prologue;
    std::memcpy(trampoline, start, startSize);
    std::memcpy(trampoline + at, &function, sizeof(function));
    std::memcpy(trampoline + startSize, method, prologue);
    std::memcpy(trampoline + startSize + prologue, kJumpBack, sizeof(kJumpBack));
    std::memcpy(trampoline + startSize + prologue + sizeof(kJumpBack), &resume, sizeof(resume));
    const std::uint8_t jump[kEntryJump] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    std::memcpy(site.original, method, kEntryJump);
    std::memcpy(site.patched, jump, kEntryJump);
    std::memcpy(site.patched + 2, &trampoline, sizeof(trampoline));
    return true;
}

bool PrepareFilter(Site& site, std::size_t prologue, Filter filter) {
    return PrepareEntry(site, kHookStart, sizeof(kHookStart), kHookFilter, filter, prologue);
}

bool PrepareObserver(Site& site, std::size_t prologue, Observer observer) {
    return PrepareEntry(site, kObserveStart, sizeof(kObserveStart), kObserveFunction, observer, prologue);
}

// code is the site rewritten to call the replacement; returnTo is taken from where that call ends.
template <std::size_t size> bool PrepareRewrite(Site& site, const std::uint8_t (&original)[size], const std::uint8_t (&code)[size]) {
    if (site.size != size || std::memcmp(game + site.rva, original, size) != 0) return false;
    for (std::size_t i = 0; i + 12 <= size; ++i) {
        if (code[i] == 0x48 && code[i + 1] == 0xb8 && code[i + 10] == 0xff && code[i + 11] == 0xd0) site.returnTo = static_cast<std::uint8_t>(i + 12);
    }
    if (site.returnTo == 0) return false;
    std::memcpy(site.original, original, size);
    std::memcpy(site.patched, code, size);
    return true;
}

// A call whose only change is its target: rewritten to a rel32 call to a jump near the module, padded with nops. The jump is kept for the
// process, because a thread can be inside it when the cheat goes off. Only a call that was longer than the rel32 one is returned into.
template <std::size_t size> bool PrepareNearCall(Site& site, const std::uint8_t (&original)[size], std::uintptr_t target) {
    static_assert(size >= 5);
    if (site.size != size || std::memcmp(game + site.rva, original, size) != 0) return false;
    std::uint8_t* const jump = NearJump(target);
    if (jump == nullptr) return false;
    const std::int64_t offset = jump - (game + site.rva + 5);
    if (offset < std::numeric_limits<std::int32_t>::min() || offset > std::numeric_limits<std::int32_t>::max()) {
        VirtualFree(jump, 0, MEM_RELEASE);
        return false;
    }
    std::memset(site.patched, 0x90, size);
    site.patched[0] = 0xe8;
    const auto relative = static_cast<std::int32_t>(offset);
    std::memcpy(site.patched + 1, &relative, sizeof(relative));
    std::memcpy(site.original, original, size);
    site.returnTo = size > 5 ? 5 : 0;
    return true;
}

bool PrepareInputDelta() {
    std::uint8_t call[sizeof(kInputDeltaCallBytes)] = {0x48, 0x89, 0xdf, 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0, 0x90};
    const auto function = &MovementInputDelta;
    std::memcpy(call + 5, &function, sizeof(function));
    return PrepareRewrite(inputDeltaSite, kInputDeltaCallBytes, call);
}

bool PrepareJumpDirection() {
    std::uint8_t call[sizeof(kJumpDirectionCallBytes)] = {0x4c, 0x8b, 0x73, 0x58, 0x48, 0x89, 0xdf, 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0, 0x90};
    const auto function = &JumpDirection;
    std::memcpy(call + 9, &function, sizeof(function));
    return PrepareRewrite(jumpDirectionSite, kJumpDirectionCallBytes, call);
}

bool PrepareManaClamp() {
    std::uint8_t call[sizeof(kManaClampBytes)] = {0x48, 0x89, 0xdf, 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0,
                                                  0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00, 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
    const auto function = &ManaAfterOffset;
    std::memcpy(call + 5, &function, sizeof(function));
    return PrepareRewrite(manaSite, kManaClampBytes, call);
}

bool PrepareHitDamage() {
    std::uint8_t call[sizeof(kHitDamageCallBytes)] = {0x48, 0x89, 0xdf, 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0,
                                                      0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00};
    const auto function = &ApplyHitDamage;
    std::memcpy(call + 5, &function, sizeof(function));
    return PrepareRewrite(hitDamageSite, kHitDamageCallBytes, call);
}

bool PrepareGateMove() {
    std::uint8_t move[sizeof(kGateMoveCallBytes)];
    GateMoveCall(move);
    return PrepareRewrite(gateMoveSite, kGateMoveCallBytes, move);
}

bool PrepareBlockedAhead() { return PrepareNearCall(blockedAheadSite, kBlockedAheadHitBytes, reinterpret_cast<std::uintptr_t>(&BlockedAheadHit)); }

// SetPhysicsLockState is only observed, so the bytes the trampoline copies must be the instructions it was analysed with.
bool PreparePhysicsLock() {
    return std::memcmp(game + kPhysicsLock, kPhysicsLockPrologue, sizeof(kPhysicsLockPrologue)) == 0 &&
           PrepareObserver(physicsLockSite, sizeof(kPhysicsLockPrologue), &PhysicsLockChanged);
}

bool PrepareHitImpulse() { return PrepareNearCall(hitImpulseSite, kHitImpulseCallBytes, reinterpret_cast<std::uintptr_t>(&HitImpulse)); }

bool PrepareRootMotion() { return PrepareNearCall(rootMotionSite, kRootMotionDeltaCallBytes, reinterpret_cast<std::uintptr_t>(&RootMotionDelta)); }

void SyncSafeTransition() { SafeTransition::Set(movementSpeed.active.load() || jumpFactor.load() != 1); }

// AvPlayer source opening is serialized for the whole time a movement patch is installed: set before it goes in, cleared after it is out.
bool SerializesAvPlayer(const Cheat& cheat) { return &cheat == &movementSpeed || &cheat == &jumpHeight; }

bool Enable(Cheat& cheat, bool stop) {
    if (SerializesAvPlayer(cheat)) SafeTransition::Set(true);
    const Result result = Transition(cheat.sites, cheat.siteCount, true, stop);
    if (result != Result::Done) {
        if (SerializesAvPlayer(cheat)) SyncSafeTransition();
        if (result == Result::Mismatch) cheat.available = false;
        std::fprintf(stderr, result == Result::Mismatch ? "%s: unavailable, its game code could not be patched\n"
                                                        : "%s: not turned on, the game was running this code\n", cheat.name);
        return false;
    }
    cheat.active.store(true);
    return true;
}

// The cheat stops taking effect before its code is removed, so a call already in flight behaves as off. A restore the game was too busy
// for is tried again on the next key press; code that is no longer ours is left as it is for good.
void Disable(Cheat& cheat, bool stop) {
    cheat.active.store(false);
    const Result result = Transition(cheat.sites, cheat.siteCount, false, stop);
    cheat.failed = result != Result::Done;
    if (cheat.failed) {
        if (result == Result::Mismatch) cheat.available = false;
        std::fprintf(stderr, result == Result::Mismatch
                                 ? "%s: still patched, its original game code could not be restored; the cheat has no effect\n"
                                 : "%s: still patched, the game was running this code; the cheat has no effect, press again\n", cheat.name);
    }
    if (SerializesAvPlayer(cheat)) SyncSafeTransition();
}

const char* InstallWalletHook() {
    const Result result = Transition(kWalletSites, std::size(kWalletSites), true, true);
    if (result == Result::Done) return nullptr;
    return result == Result::Mismatch ? "game code could not be patched" : "the game was running this code";
}

// Removing the entry jump from inside the trampoline is safe: what follows the filter is the copied first instruction of the method, never
// the entry again.
void RemoveWalletHook() {
    const Result result = Transition(kWalletSites, std::size(kWalletSites), false, true);
    if (result == Result::Done) return;
    awardsUnavailable.store("PlayerController.Update is still patched");
    std::fprintf(stderr, result == Result::Mismatch
                             ? "Blood Orbs and Upgrade Materials: the game's own PlayerController.Update could not be restored, keys disabled\n"
                             : "Blood Orbs and Upgrade Materials: the game was running PlayerController.Update, keys disabled\n");
}

const char* FactorText(int factor) { return factor == 2 ? "2x" : factor == 4 ? "4x" : factor == 6 ? "6x" : "OFF"; }

// Only 2, 4 and 6 select a multiplier; anything else is off.
int StartingFactor(const Cheat& cheat) {
    const char* value = std::getenv(cheat.variable);
    if (value == nullptr) return 1;
    for (int factor : {2, 4, 6}) {
        if (value[0] == '0' + factor && value[1] == '\0') return factor;
    }
    return 1;
}

// A cheat whose game code is not recognised stays unavailable; the others are not affected.
void Bind(Cheat& cheat, bool ready, Site* first, Site* second, Site* third) {
    cheat.sites[0] = first;
    cheat.sites[1] = second;
    cheat.sites[2] = third;
    cheat.siteCount = third != nullptr ? 3 : second != nullptr ? 2 : 1;
    cheat.available = ready;
    if (!ready) std::fprintf(stderr, "%s: unavailable (game code not recognised)\n", cheat.name);
}

// Nothing is written to the game for a cheat the launcher leaves off.
void Start(Cheat& cheat, bool ready, Site* first, Site* second = nullptr, Site* third = nullptr) {
    Bind(cheat, ready, first, second, third);
    if (!cheat.available) return;
    const char* const value = std::getenv(cheat.variable);
    if (value == nullptr || std::strcmp(value, "1") != 0) {
        std::fprintf(stderr, "%s: OFF\n", cheat.name);
    } else if (Enable(cheat, false)) {
        std::fprintf(stderr, "%s: ON\n", cheat.name);
    }
}

void StartMultiplier(Cheat& cheat, bool ready, Site* first, Site* second = nullptr) {
    Bind(cheat, ready, first, second, nullptr);
    if (!cheat.available) return;
    const int factor = StartingFactor(cheat);
    if (factor == 1) {
        std::fprintf(stderr, "%s: OFF\n", cheat.name);
    } else if (Enable(cheat, false)) {
        cheat.factor->store(factor);
        std::fprintf(stderr, "%s: %s\n", cheat.name, FactorText(factor));
    }
}

// The title text of a cheat whose patch is installed or stuck, or null.
const char* ActiveState(const Cheat& cheat) {
    if (cheat.failed) return "FAILED";
    if (cheat.factor == nullptr) return cheat.active.load() ? "ON" : nullptr;
    const int factor = cheat.factor->load();
    return factor == 1 ? nullptr : FactorText(factor);
}

// Runs on the window thread, where game objects must not be touched: keys only install or remove patches and queue a wallet action.
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
                 : scancode == kGatePassKey      ? &gatePass
                 : scancode == kNoKnockbackKey   ? &noKnockback
                                                 : nullptr;
    if (cheat == nullptr) return;
    const char* notice = nullptr;
    if (cheat->failed) {
        if (cheat->available) Disable(*cheat, true);  // a restore the game was too busy for
        notice = cheat->failed ? nullptr : "OFF";
    } else if (!cheat->available) {
        notice = "unavailable";
    } else if (cheat->factor != nullptr) {
        const int factor = cheat->factor->load();
        const int next = factor == 1 ? 2 : factor == 2 ? 4 : factor == 4 ? 6 : 1;
        if (factor == 1) {
            if (Enable(*cheat, true)) cheat->factor->store(next);
            else notice = cheat->available ? "busy" : "unavailable";
        } else if (next == 1) {
            cheat->factor->store(1);  // before the patch goes out, so a call already inside it leaves the value alone
            Disable(*cheat, true);
            notice = cheat->failed ? nullptr : "OFF";
        } else {
            cheat->factor->store(next);
        }
    } else if (cheat->active.load()) {
        Disable(*cheat, true);
        notice = cheat->failed ? nullptr : "OFF";
    } else if (!Enable(*cheat, true)) {
        notice = cheat->available ? "busy" : "unavailable";
    }
    const char* const state = notice != nullptr ? notice : ActiveState(*cheat);
    std::fprintf(stderr, "%s: %s\n", cheat->name, state != nullptr ? state : "OFF");
    cheat->notice.store(notice);
    cheat->noticeUntil.store(notice != nullptr ? NowMs() + kNoticeMs : 0);
}

// Appended to the window title after the FPS counter, on the same thread as the keys.
void TitleStatus(char* text, std::size_t size) {
    const std::uint64_t now = NowMs();
    ExpireRequest(now);
    std::size_t used = 0;
    if (size != 0) text[0] = '\0';
    for (const Cheat* cheat : {&godMode, &infiniteSpirit, &infiniteMagic, &damageMultiplier, &movementSpeed, &jumpHeight, &gatePass, &noKnockback}) {
        const char* state = ActiveState(*cheat);
        if (state == nullptr && now < cheat->noticeUntil.load()) state = cheat->notice.load();
        if (state == nullptr || used >= size) continue;
        const int written = std::snprintf(text + used, size - used, " | %s: %s", cheat->label, state);
        if (written > 0) used += static_cast<std::size_t>(written);
    }
    for (const Award* award : {&bloodOrbs, &upgradeMaterials}) {
        if (now >= award->noticeUntil.load() || used >= size) continue;
        const int written = std::snprintf(text + used, size - used, " | %s%s", award->name, award->notice.load());
        if (written > 0) used += static_cast<std::size_t>(written);
    }
}

bool ArmTrampolines() {
    DWORD protection = 0;
    if (trampolines == nullptr || !VirtualProtect(trampolines, kTrampolineSlot * kHooks, PAGE_EXECUTE_READ, &protection)) return false;
    FlushInstructionCache(GetCurrentProcess(), trampolines, kTrampolineSlot * kHooks);
    return true;
}

bool StartCheats() {
    game = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(L"Il2cppUserAssemblies.prx.guest.prx"));
    if (game == nullptr) return false;
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(game + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const bool fits = headers->OptionalHeader.SizeOfImage >= kGetGameObject + sizeof(void*);
    trampolines = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kTrampolineSlot * kHooks, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

    // Recognising the game code and building the patches leaves the game untouched; only a cheat the launcher turns on is installed below.
    // The hashes read the game's own bytes, so the ranges that leave out what another cheat rewrites are part of their constant and must
    // stay as they are.
    const bool godModeReady = fits && Hash({{0x80ecd0, 0x80f0b0}, {0xdab1a0, 0xdab6b0}, {0xdabaf0, 0xdabd90}}) == 0x4b6beba820a6ebce &&
        PrepareFilter(healthSite, 12, HealthFilter) && PrepareFilter(greyHealthSite, 13, GreyHealthFilter);
    const bool spiritReady = fits && Hash({{0xda20b0, 0xda2520}, {0xda2b10, 0xda2d10}, {0x80ecd0, 0x80f0b0}}) == 0xb0e9d2c9cfd2a0b1 &&
        PrepareFilter(spiritSite, 12, SpiritFilter);
    const bool magicReady = fits && Hash({{0xd954f0, 0xd95bd0}, {0xdf2980, 0xdf2bc0}, {0x823d20, 0x823e30}, {0xc26550, 0xc26750},
        {0xc1af20, 0xc1afe0}, {0x80ecd0, 0x80f0b0}}) == 0xa63f97d69c46ad8d && PrepareManaClamp();
    const bool damageReady = fits && Hash({{0xd91d00, 0xd92910}, {0xd83100, 0xd837d0}, {0xdf3900, 0xdf3be0}, {0xdf3e60, 0xdf4e00},
        {0xdf98d0, 0xdf9af0}, {0xd94d70, 0xd95120}, {0x80ecd0, 0x80f0b0}}) == 0x7b452922017e03ec && PrepareHitDamage();
    const bool awardsReady = fits && Hash({{0x32fac0, 0x32fae0}, {0x5d12d0, 0x5d1390}, {0x5d35c0, 0x5d35d0}, {0x8c0810, 0x8c0950},
        {0x8c91b0, 0x8c92b0}, {0x8ce0f0, 0x8ce6d0}, {0x996d20, 0x996e70}, {0x9971b0, 0x997390}, {0xd11230, 0xd11300}}) == 0xdcbbc10a00bb8f1e &&
        PrepareFilter(walletSite, 1, ControllerFilter);
    const bool movementReady = fits && Hash({{0x706620, 0x706b50}, {0x6ecf20, 0x6ed280}, {0x6f02e0, 0x6f06d0}, {0x708d70, 0x709360},
        {0x80ecd0, 0x80f0b0}}) == 0xe4fb67f213ff4921 && PrepareInputDelta();
    const bool jumpReady = fits && Hash({{0x6fc450, 0x6fcbf0}, {0x6face0, 0x6fb1b0}, {0x6efe80, 0x6f0180}, {0x6f0810, 0x6f08e0},
        {0x708d70, 0x709360}, {0x80ecd0, 0x80f0b0}}) == 0x6b8174068fca8cd1 && PrepareJumpDirection();
    const bool gateReady = fits && Hash({{0x706a5b, 0x706b50}, {0x6edc60, 0x6edf30}, {0xc309a0, 0xc30f00}, {0x443bb00, 0x443bbe0}, {0x44273f0, 0x4427460},
        {0x4427460, 0x44274c0}, {0x4441ab0, 0x4441b60}, {0x4378a50, 0x4378ae0}, {0x4441290, 0x44412e0}, {0x436cf80, 0x436cfd0},
        {0x436e240, 0x436e290}, {0x436e1f0, 0x436e240}, {0x4384a10, 0x4384a60}, {0x438a190, 0x438a1e0}, {0x438aa20, 0x438aa80},
        {0x43724d0, 0x4372520}, {0x4372580, 0x4372670}, {0x4378440, 0x4378540}, {0x6ef180, 0x6ef350}, {0x6f8c00, 0x6f8cd0}, {0x708d70, 0x709360},
        {0x705110, 0x705320}, {0x7099c0, 0x709ae0}, {0x6fa970, 0x6faa60}, {0x80ecd0, 0x80f0b0}, {0x6ebd10, 0x6ec080},
        {0x4372720, 0x4372770}, {0x443be00, 0x443be50}, {0x443d620, 0x443d670}}) == 0x4eb00aedc4d92eff &&
        PrepareGateMove() && PrepareBlockedAhead() && PreparePhysicsLock();
    const bool knockbackReady = fits && Hash({{0xd91d00, 0xd91e1f}, {0xd91e3c, 0xd92910}, {0xdf5250, 0xdf5c10}, {0x6f0810, 0x6f08e0}, {0x6efe80, 0x6f0180},
        {0x80ecd0, 0x80f0b0}, {0x706620, 0x7067a9}, {0x7067b9, 0x706b02}, {0x706b26, 0x706b50}, {0x6ecf20, 0x6ed280}, {0x7b8560, 0x7b86b0},
        {0xdb8320, 0xdb8640}}) == 0x6910b1dbd7438b36 && PrepareHitImpulse() && PrepareRootMotion();
    const bool armed = ArmTrampolines();

    Start(godMode, godModeReady && armed, &healthSite, &greyHealthSite);
    Start(infiniteSpirit, spiritReady && armed, &spiritSite);
    Start(infiniteMagic, magicReady, &manaSite);
    StartMultiplier(damageMultiplier, damageReady, &hitDamageSite);
    if (awardsReady && armed) awardsUnavailable.store(nullptr);
    if (const char* const unavailable = awardsUnavailable.load()) {
        std::fprintf(stderr, "Blood Orbs and Upgrade Materials: unavailable (%s)\n", unavailable);
    } else {
        std::fprintf(stderr, "Blood Orbs and Upgrade Materials: ready\n");
    }
    Start(movementSpeed, movementReady, &inputDeltaSite);
    StartMultiplier(jumpHeight, jumpReady, &jumpDirectionSite);
    // All three sites or none: with only the collision bypass the run states would still stop short of the gate, with only the movement
    // bypass the player would run into it, and without the lock observer gate IDs would outlive a world transition.
    Start(gatePass, gateReady && armed, &gateMoveSite, &blockedAheadSite, &physicsLockSite);
    // Both or neither: without the root motion one, a contact-damage hit still slides the player back.
    Start(noKnockback, knockbackReady, &hitImpulseSite, &rootMotionSite);
    SyncSafeTransition();
    HostExtensionRegister_nid_no_patch(OnKey, TitleStatus);
    return true;
}

[[maybe_unused]] const bool cheatsStarted = StartCheats();

}
#endif
