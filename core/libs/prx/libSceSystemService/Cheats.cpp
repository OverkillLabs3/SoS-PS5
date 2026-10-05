#ifdef _WIN32
#include <atomic>
#include <cstdarg>  // DEBUG_SAULO
#include <chrono>
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

namespace {

// Sons of Sparta cheats, for the player only and used while playing: F1 God Mode, F2 Infinite Spartan Spirit, F3 Infinite Magic,
// F5 Movement Speed 2x and F6 Jump Height 2x are switched on and off, F7 adds 1000 Blood Orbs and F8 adds 10 of each upgrade material (F4
// is kept for a future Damage 2x). The launcher passes the starting states of the five switches as SOS_GOD_MODE=1,
// SOS_INFINITE_SPARTAN_SPIRIT=1, SOS_INFINITE_MAGIC=1, SOS_MOVEMENT_SPEED=1 and SOS_JUMP_HEIGHT=1. When this library loads, before any game code runs, each of them whose game code is recognised gets its hooks in Il2cppUserAssemblies;
// the hooks stay for the whole session, the keys only switch the states or ask for an addition, and the window title lists the cheats
// that are on. No two of them share a hook. Offsets are module RVAs.
//
// God Mode: health only changes through EntityComponents.Vitals.OffsetHp, and grey health through Vitals.OffsetGreyHp. Grey health is
// the part of the health bar shown as lost until it recovers: the green bar is health minus grey health. LivingDamageable.ReceiveDamage
// (hits, contact damage, projectiles, damage over time) turns a hit on the player into grey health, and some enemy attacks add grey
// health directly (EventModule_OffsetGrayHP). For the player's Vitals both hooks set health to its maximum and grey health to 0, and turn
// a change that would shrink the green bar (losing health, gaining grey health) into 0; the method then runs as usual and updates the
// health bar from those values. The rest of a hit (hit reaction, knockback, blocking, events) is not touched, and nothing is kept between
// calls, so a respawned player is handled like the first one. Turning it on does not refill the bar at once: the key arrives on the
// window thread, where game objects must not be touched, so the bar fills at the player's next health change. Instant kills that empty
// the health directly (Vitals.MarkAsDead) still kill.
// Infinite Spartan Spirit: every change of the meter (spending, block costs, regeneration every frame) goes through
// EntityComponents.SpartanSpirit.OffsetSpartanSpirit; for the player the meter is set to its maximum and the change to 0.
// Infinite Magic: the game calls magic mana. Every change of the player's mana goes through EntityComponents.ManaState.OffsetMana: what
// abilities spend (EventModule_AffectMana), changes per second (TickModule_OffsetManaPerSec), recovery and the pit's refill. It sets
// currentMana to currentMana + the change kept within 0 and currentMaxMana, then raises OnManaChanged (and, unless the change is
// regeneration, OnManaChangedExcludingRegeneration), which the mana bar follows; the game's mana checks read currentMana. The method's
// first instructions read a rip-relative flag, so it cannot get the entry hook God Mode and Spirit use; instead its computation of the new value
// is replaced by a call to ManaAfterOffset, which computes the same value and, for the player while the cheat is on, starts from the
// maximum and makes a spending 0, so the result is the maximum. The method then stores it and raises its events as usual. Turning it on
// fills the bar at the player's next mana change.
// Blood Orbs (the game's item loot_orb_red) and the upgrade materials are counts in the player's PersistentData.PlayerWallet, which is
// saved with the player's data. F7 and F8 add to them with PlayerWallet.AddToWallet, as collecting Blood Orbs or opening a chest does:
// OffsetWallet adds to the count, OnAddedToWallet tells the HUD (the Blood Orb counter counts up), and the game's next save keeps the new
// counts. Its last argument, a Nullable<bool>, only decides whether PlayerHUDMenu announces the item (item ticker, Major Get or card): F7
// leaves it unset, so Blood Orbs show as when collected, and F8 sets it to true, so its nine additions queue no HUD notification. Game
// objects must not be touched on the window thread, so a key only asks for its action; the next PlayerController.Update (empty, called by
// the game every frame for the player's controller) finds the wallet of the player that controller plays and the items the way a save
// loads them, and adds. F7 and F8 do nothing while either action waits or runs, and an action no player picks up within a second fails.
// Movement Speed 2x: walking, running and steering in the air move a character by a delta its movement state sets every physics step from
// the movement input (CharacterMovement.UseAsMovementDelta: direction * fixedDeltaTime * movementSpeed * the state's factor), which
// PlayerMovement.MovementTick may then scale (in the air, under root motion). PlayerMovement.TickPseudoPhysics, which only the player's
// movement runs (enemies run CharacterMovement's), aligns that delta with the slope through one call to
// CharacterMovement.GetSlopeAlignedVersionOfMovementDelta, then adds the physics velocity (gravity, jumps, knockback) and root motion.
// That call is redirected to MovementInputDelta, which makes it and doubles its result for the player while the cheat is on. Nothing is
// stored, so every step moves exactly twice the game's own amount, and switching it off gives the game's amount from the next step; the
// physics velocity, root motion, other characters and the game's time are not touched.
// Jump Height 2x: a jump the player's jump state (MoveStateJump) starts runs its DelayStartJumping coroutine, which zeroes the player's
// velocity (ZeroOutVelocity) and applies one force, the jump magnitude (with its stat modifier) times the direction
// GetJumpAngleAsForwardDirectionVector gives, through context.ApplyForce. CharacterMovement.CalcVelocity adds that force to the velocity
// once, as an impulse (force * receivedPhysicsImpulseMultiplier / mass), and while the character rises pulls it down with constant gravity
// and nothing caps the upward speed, so the jump's height grows with the square of its upward speed. The coroutine's one call to
// GetJumpAngleAsForwardDirectionVector is redirected to JumpDirection, which makes it and, for the player while the cheat is on,
// multiplies the direction's upward part by sqrt(2): sqrt(2) times the upward speed, twice the height. The sideways part, gravity, falling,
// knockback, root motion, scripted launches (which do not go through this call) and other characters are not touched, and nothing is
// stored, so every jump starts from the game's own value.

struct Cheat {
    const char* name;      // in the log
    const char* label;     // in the window title
    const char* variable;  // starting state from the launcher
    std::atomic<bool> on{false};
    bool hooked = false;
    // A key that turns the cheat off (or finds it unavailable) shows that in the title until this tick count.
    std::atomic<const char*> notice{nullptr};
    std::atomic<std::uint64_t> noticeUntil{0};
};
Cheat godMode{"God Mode", "God Mode", "SOS_GOD_MODE"};
Cheat infiniteSpirit{"Infinite Spartan Spirit", "Spartan Spirit", "SOS_INFINITE_SPARTAN_SPIRIT"};
Cheat infiniteMagic{"Infinite Magic", "Magic", "SOS_INFINITE_MAGIC"};
Cheat movementSpeed{"Movement Speed 2x", "Movement Speed", "SOS_MOVEMENT_SPEED"};
Cheat jumpHeight{"Jump Height 2x", "Jump Height", "SOS_JUMP_HEIGHT"};

// A wallet item an action adds.
struct WalletItem {
    const char* id;    // the game's item, by the name saves use
    const char* name;  // its English name
};
constexpr WalletItem kBloodOrb[] = {{"loot_orb_red", "Blood Orb"}};
// The game's Materials (ItemGroup.Materials, spent to enhance Spartan Aspis rims and Spartan Dory tips, grips and tails at a campsite),
// each given by several chests or challenges; its four boss trophies (loot_boss_*), which only their boss gives, are left out.
constexpr WalletItem kUpgradeMaterials[] = {
    {"loot_common_shield", "Crude Ore"},
    {"loot_rare_shield", "Durable Plate"},
    {"loot_epic_shield", "Elysian Alloy"},
    {"loot_common_spear", "Rusty Scrap"},
    {"loot_rare_spear", "Lustrous Metal"},
    {"loot_epic_spear_grip", "Corinthian Hide"},
    {"loot_epic_spear_tail", "Ionian Salt"},
    {"loot_epic_spear_tip", "Cretan Whetstone"},
    {"loot_epic_spear_tip_status", "Aegean Crystal"},
};
constexpr std::size_t kMostItems = std::size(kUpgradeMaterials);
static_assert(std::size(kBloodOrb) <= kMostItems);

// The one wallet action that may be outstanding, shared by F7 and F8: a key press is taken only while it is idle, so the two actions
// never wait or run at the same time. Only the window thread (keys) moves it from idle to queued; the game thread moves a queued action
// to running and, when that run ends, back to idle; the window thread gives up a queued action no game thread took in time. A running
// action belongs to the game thread until it ends.
enum : int { kIdle, kBloodOrbsQueued, kBloodOrbsRunning, kUpgradeMaterialsQueued, kUpgradeMaterialsRunning };
std::atomic<int> walletAction{kIdle};
std::atomic<std::uint64_t> walletRequestedAt{0};  // when the queued action was asked for

// What F7 or F8 asks for: the window thread queues it, the game thread does it, and the result shows in the title for a while.
struct Award {
    const char* name;   // in the log and the window title
    const char* added;  // in the window title after the name
    const WalletItem* items;
    std::size_t itemCount;
    std::int64_t amount;  // of each item
    bool announce;        // whether the HUD announces the additions as a pickup would
    int queued, running;  // its wallet action states
    std::atomic<const char*> notice{nullptr};
    std::atomic<std::uint64_t> noticeUntil{0};
};
Award bloodOrbs{"Blood Orbs", " +1000", kBloodOrb, std::size(kBloodOrb), 1000, true, kBloodOrbsQueued, kBloodOrbsRunning};
Award upgradeMaterials{"Upgrade Materials", " +10", kUpgradeMaterials, std::size(kUpgradeMaterials), 10, false, kUpgradeMaterialsQueued,
                       kUpgradeMaterialsRunning};
// Why F7 and F8 do nothing in this session; null once their hook is in place.
const char* awardsUnavailable = "game code not recognised";
const char* const kUnavailable = ": unavailable";

// SDL scancodes of the cheat keys; F4 (61) is kept for Damage 2x, F9 and F10 are free, F11 is the runtime's fullscreen.
constexpr int kGodModeKey = 58, kInfiniteSpiritKey = 59, kInfiniteMagicKey = 60, kMovementSpeedKey = 62, kJumpHeightKey = 63,
              kBloodOrbsKey = 64, kUpgradeMaterialsKey = 65;
constexpr std::uint64_t kNoticeMs = 2000;
constexpr std::uint64_t kAwardWaitMs = 1000;  // how long a requested action waits for the player's controller

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint8_t* game = nullptr;
constexpr std::uint32_t kOffsetHp = 0xdab550;       // EntityComponents.Vitals.OffsetHp(float)
constexpr std::uint32_t kOffsetGreyHp = 0xdabaf0;   // EntityComponents.Vitals.OffsetGreyHp(float)
constexpr std::uint32_t kOffsetSpirit = 0xda20b0;   // EntityComponents.SpartanSpirit.OffsetSpartanSpirit(float, bool)
constexpr std::uint32_t kOffsetMana = 0xd959d0;     // EntityComponents.ManaState.OffsetMana(float, bool)
constexpr std::uint32_t kManaClamp = 0xd95a0f;      // OffsetMana: currentMana + the change kept within 0 and currentMaxMana
constexpr std::uint32_t kIsPlayer = 0x80ef70;       // Utilities.PawnUtils.IsPlayer(GameObject)
constexpr std::uint32_t kGetGameObject = 0x629c8f0; // Component.get_gameObject icall pointer, resolved by the game before it is needed
constexpr std::uint32_t kControllerUpdate = 0x5d35c0;  // PlayerController.Update(), empty
constexpr std::uint32_t kGetPawnObject = 0x5d12d0;     // Controller.GetPawnObject(), null without a living pawn
constexpr std::uint32_t kStringNew = 0x32fac0;         // il2cpp_string_new(const char*), exported by the game module
constexpr std::uint32_t kGetItem = 0x8c91b0;           // PersistentData.PlayerInventory.GetItemDefinitionByItemId(string), as saves load items
constexpr std::uint32_t kTryGetWallet = 0x8c0810;      // RuntimeData.PlayerRuntimeDataUtils.TryGetWallet(GameObject)
constexpr std::uint32_t kGetAmount = 0x8ce2b0;         // PersistentData.PlayerWallet.GetCurrentAmount(ItemDefinition)
constexpr std::uint32_t kAddToWallet = 0x8ce430;       // PersistentData.PlayerWallet.AddToWallet(ItemDefinition, long, bool?)
constexpr std::uint32_t kInputDeltaCall = 0x7067a9;    // PlayerMovement.TickPseudoPhysics: the slope-aligned movement input delta
constexpr std::uint32_t kSlopeAligned = 0x6ecf20;      // CharacterMovement.GetSlopeAlignedVersionOfMovementDelta(Vector2)
constexpr std::uint32_t kJumpDirectionCall = 0x6fc9c7; // <DelayStartJumping>d__27.MoveNext: the direction of the jump's force
constexpr std::uint32_t kJumpAngle = 0x6face0;         // MoveStateJump.GetJumpAngleAsForwardDirectionVector(float)

constexpr std::size_t kNativeObject = 0x10;                                       // UnityEngine.Object.m_CachedPtr, null once destroyed
constexpr std::size_t kMaxHp = 0x38, kCurrentHp = 0x40, kGreyHp = 0x44, kDead = 0x48;  // Vitals
constexpr std::size_t kCurrentSpirit = 0x20, kMaxSpirit = 0x24;                   // SpartanSpirit
constexpr std::size_t kCurrentMana = 0x28, kMaxMana = 0x2c;                       // ManaState.currentMana, currentMaxMana
constexpr std::size_t kWalletCount = 0x20;  // PlayerWallet.walletCount (item -> count), created by the first addition
constexpr std::size_t kInputDelta = 0xe0;   // CharacterMovement.deltaFromMovementInputsThisFrame (Vector2)
constexpr std::size_t kJumpContext = 0x58;          // MoveStateAerialBase.context (the PlayerMovement it moves)
constexpr std::size_t kForwardAdjustment = 0x9c;    // MoveStateJump.forwardAdjustmentFactor
constexpr float kJumpSpeedFactor = 1.41421356f;     // sqrt(2): twice the height of a jump whose height grows with its speed squared

using GetGameObject = void*(APS5_VABI*)(void* component);
using IsPlayer = bool(APS5_VABI*)(void* gameObject, const void* method);
using Filter = bool(APS5_VABI*)(void* self, void* argument, void* method, float* amount);
using GetPawnObject = void*(APS5_VABI*)(void* controller, const void* method);
using StringNew = void*(APS5_VABI*)(const char* text);
using GetItem = void*(APS5_VABI*)(void* id, const void* method);
using TryGetWallet = void*(APS5_VABI*)(void* gameObject, const void* method);
using GetAmount = std::int64_t(APS5_VABI*)(void* wallet, void* item, const void* method);
// System.Nullable<bool>: AddToWallet's overrideDontShowNotification. Unset, the item's own settings decide how the HUD announces it; true
// announces nothing (PlayerHUDMenu.TryQueueItemNotification returns at once), as for a chest reward set not to show in the ticker.
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

template <typename T> T& Field(void* object, std::size_t offset) { return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset); }
template <typename Function> Function Game(std::uint32_t offset) { return reinterpret_cast<Function>(game + offset); }

bool Alive(void* object) { return object != nullptr && Field<void*>(object, kNativeObject) != nullptr; }

// The game's own player test: the component's GameObject has a Pawn controlled by a PlayerController.
bool OnPlayer(void* component) {
    const auto gameObject = Field<GetGameObject>(game, kGetGameObject);
    return Alive(component) && gameObject != nullptr && reinterpret_cast<IsPlayer>(game + kIsPlayer)(gameObject(component), nullptr);
}

// DEBUG_SAULO: temporary God Mode health diagnostics, remove before commit. Every log line starts with [DEBUG_SAULO]. The pointers kept
// here are only compared with the current ones and printed, never dereferenced, and the cheat never uses them.
struct DEBUG_SAULO_Player {
    void* gameObject = nullptr;
    void* vitals = nullptr;
    int dead = -1;
    float hp = -1.0f, grey = -1.0f;
};
DEBUG_SAULO_Player DEBUG_SAULO_last;  // DEBUG_SAULO

struct DEBUG_SAULO_Call {  // DEBUG_SAULO
    bool logged;
    float delta, hp, grey, max;
};

unsigned long long DEBUG_SAULO_Address(const void* pointer) { return reinterpret_cast<std::uintptr_t>(pointer); }  // DEBUG_SAULO
const char* DEBUG_SAULO_Method(bool grey) { return grey ? "OffsetGreyHp" : "OffsetHp"; }  // DEBUG_SAULO

// DEBUG_SAULO: a health call God Mode does not treat as the player's is logged only when it is on the last player Vitals.
void DEBUG_SAULO_NotPlayer(void* vitals, float delta, bool grey) {
    if (DEBUG_SAULO_last.vitals == nullptr || vitals != DEBUG_SAULO_last.vitals) return;
    std::fprintf(stderr, "[DEBUG_SAULO] Health %s on the last player Vitals NOT classified as player vitals=0x%llx delta=%.1f alive=%d\n",
        DEBUG_SAULO_Method(grey), DEBUG_SAULO_Address(vitals), delta, Alive(vitals) ? 1 : 0);
}

// DEBUG_SAULO: logs a player health call with God Mode on (only one that changes something), and what changed about the player's
// objects or health since the last one.
DEBUG_SAULO_Call DEBUG_SAULO_Before(void* vitals, float delta, bool grey) {
    DEBUG_SAULO_Player& last = DEBUG_SAULO_last;
    void* gameObject = Field<GetGameObject>(game, kGetGameObject)(vitals);
    const DEBUG_SAULO_Call call{(grey ? delta > 0.0f : delta < 0.0f) || Field<float>(vitals, kCurrentHp) < Field<float>(vitals, kMaxHp) ||
                                    Field<float>(vitals, kGreyHp) > 0.0f,
        delta, Field<float>(vitals, kCurrentHp), Field<float>(vitals, kGreyHp), Field<float>(vitals, kMaxHp)};
    const int dead = Field<bool>(vitals, kDead) ? 1 : 0;
    if (last.vitals != nullptr) {
        if (gameObject != last.gameObject) {
            std::fprintf(stderr, "[DEBUG_SAULO] Player GameObject changed old=0x%llx new=0x%llx\n", DEBUG_SAULO_Address(last.gameObject),
                DEBUG_SAULO_Address(gameObject));
        }
        if (vitals != last.vitals) {
            std::fprintf(stderr, "[DEBUG_SAULO] Player Vitals changed old=0x%llx new=0x%llx\n", DEBUG_SAULO_Address(last.vitals), DEBUG_SAULO_Address(vitals));
        }
        if (gameObject != last.gameObject || vitals != last.vitals) {
            std::fprintf(stderr, "[DEBUG_SAULO] Player lifecycle replacement detected previousDead=%d\n", last.dead);
        }
        if (last.dead != -1 && dead != last.dead) std::fprintf(stderr, "[DEBUG_SAULO] Player dead state changed %d -> %d\n", last.dead, dead);
        if (vitals == last.vitals && (call.hp != last.hp || call.grey != last.grey)) {
            std::fprintf(stderr, "[DEBUG_SAULO] Player health changed outside the hooks since the last call current %.1f -> %.1f grey %.1f -> %.1f\n",
                last.hp, call.hp, last.grey, call.grey);
        }
    }
    last.gameObject = gameObject;
    last.vitals = vitals;
    last.dead = dead;
    if (call.logged) {
        std::fprintf(stderr, "[DEBUG_SAULO] Health %s vitals=0x%llx gameObject=0x%llx isPlayer=1 delta=%.1f current=%.1f grey=%.1f max=%.1f dead=%d godMode=1\n",
            DEBUG_SAULO_Method(grey), DEBUG_SAULO_Address(vitals), DEBUG_SAULO_Address(gameObject), delta, call.hp, call.grey, call.max, dead);
    }
    return call;
}

// DEBUG_SAULO: logs what God Mode changed, and the values the method now passes to the health bar (green = (current - grey) / max).
void DEBUG_SAULO_After(const DEBUG_SAULO_Call& call, void* vitals, float delta, bool grey) {
    const float hp = Field<float>(vitals, kCurrentHp), greyHp = Field<float>(vitals, kGreyHp), max = Field<float>(vitals, kMaxHp);
    DEBUG_SAULO_last.hp = hp;
    DEBUG_SAULO_last.grey = greyHp;
    if (!call.logged) return;
    if (call.hp < call.max) std::fprintf(stderr, "[DEBUG_SAULO] Health restored before=%.1f after=%.1f max=%.1f\n", call.hp, hp, max);
    if (call.grey > 0.0f) std::fprintf(stderr, "[DEBUG_SAULO] Health grey cleared before=%.1f after=%.1f\n", call.grey, greyHp);
    if (delta != call.delta) {
        std::fprintf(stderr, "[DEBUG_SAULO] Health %s blocked delta=%.1f current=%.1f max=%.1f\n", grey ? "grey increase" : "loss", call.delta, hp, max);
    }
    std::fprintf(stderr, "[DEBUG_SAULO] Health HUD current=%.1f grey=%.1f max=%.1f green=%.3f\n", hp, greyHp, max, max > 0.0f ? (hp - greyHp) / max : 0.0f);
}

// Vitals.OffsetHp(this, amount) and Vitals.OffsetGreyHp(this, amount) with God Mode on: the player's Vitals get full health and no grey
// health, and a change that would shrink the green bar becomes 0. The method always continues.
bool HoldFull(void* vitals, float* amount, bool grey) {
    if (!godMode.on.load(std::memory_order_relaxed)) return false;
    if (!OnPlayer(vitals)) {
        DEBUG_SAULO_NotPlayer(vitals, *amount, grey);  // DEBUG_SAULO
        return false;
    }
    const DEBUG_SAULO_Call DEBUG_SAULO_call = DEBUG_SAULO_Before(vitals, *amount, grey);  // DEBUG_SAULO
    Field<float>(vitals, kCurrentHp) = Field<float>(vitals, kMaxHp);
    Field<float>(vitals, kGreyHp) = 0.0f;
    if (grey ? *amount > 0.0f : *amount < 0.0f) *amount = 0.0f;
    DEBUG_SAULO_After(DEBUG_SAULO_call, vitals, *amount, grey);  // DEBUG_SAULO
    return false;
}

bool APS5_VABI HealthFilter(void* vitals, void*, void*, float* amount) { return HoldFull(vitals, amount, false); }
bool APS5_VABI GreyHealthFilter(void* vitals, void*, void*, float* amount) { return HoldFull(vitals, amount, true); }

// SpartanSpirit.OffsetSpartanSpirit(this, amount, regeneration), which Spirit regeneration calls every frame for the player.
bool APS5_VABI SpiritFilter(void* spirit, void*, void*, float* amount) {
    if (!infiniteSpirit.on.load(std::memory_order_relaxed) || !OnPlayer(spirit)) return false;
    Field<float>(spirit, kCurrentSpirit) = Field<float>(spirit, kMaxSpirit);
    *amount = 0.0f;
    return false;
}

// DEBUG_SAULO: temporary Infinite Magic diagnostics, remove before commit: one line for the first player mana change the cheat holds
// after it is switched on (a spending, or mana below the maximum), not one per regeneration tick.
std::atomic<bool> DEBUG_SAULO_magicLogged{false};
// DEBUG_SAULO
__attribute__((noinline)) void DEBUG_SAULO_ManaChange(float current, float max, float delta) {
    if (DEBUG_SAULO_magicLogged.exchange(true)) return;
    std::fprintf(stderr, "[DEBUG_SAULO] Infinite Magic player current=%.2f max=%.2f delta=%.2f result=%.2f\n", static_cast<double>(current),
        static_cast<double>(max), static_cast<double>(delta), static_cast<double>(max));
    std::fflush(stderr);
}

// Called by ManaState.OffsetMana(this, amount, isRegeneration) in place of its own computation of the new mana: currentMana + amount
// kept within 0 and currentMaxMana, with the game's comparisons. OffsetMana stores the result and raises its events. With Infinite Magic
// on, the player's mana starts from the maximum and a spending becomes 0, so the result is the maximum; a gain is kept, and is capped
// at the maximum as usual.
float APS5_VABI ManaAfterOffset(void* mana, float amount) {
    float current = Field<float>(mana, kCurrentMana);
    const float max = Field<float>(mana, kMaxMana);
    if (infiniteMagic.on.load(std::memory_order_relaxed) && OnPlayer(mana)) {
        if (!DEBUG_SAULO_magicLogged.load(std::memory_order_relaxed) && (amount < 0.0f || current < max)) DEBUG_SAULO_ManaChange(current, max, amount);  // DEBUG_SAULO
        current = max;
        if (amount < 0.0f) amount = 0.0f;
    }
    const float value = current + amount;
    const float limited = max < value ? max : value;  // vminss max, value
    return value < 0.0f ? 0.0f : limited;             // vcmpltss value, 0; vandnps
}

// DEBUG_SAULO: temporary Movement Speed diagnostics, remove before commit: one line for the first non-zero player movement the cheat doubles
// after it is switched on, so the log shows the game's value and the doubled one without a line per physics step.
std::atomic<bool> DEBUG_SAULO_movementLogged{false};
// DEBUG_SAULO
__attribute__((noinline)) void DEBUG_SAULO_MovementDelta(Vector2 original) {
    if (DEBUG_SAULO_movementLogged.exchange(true)) return;
    std::fprintf(stderr, "[DEBUG_SAULO] Movement Speed player delta original=(%.6f, %.6f) modified=(%.6f, %.6f)\n", static_cast<double>(original.x),
        static_cast<double>(original.y), static_cast<double>(original.x * 2.0f), static_cast<double>(original.y * 2.0f));
    std::fflush(stderr);
}

// Called by PlayerMovement.TickPseudoPhysics(this, ...) every physics step in place of
// GetSlopeAlignedVersionOfMovementDelta(this, deltaFromMovementInputsThisFrame): the same result, doubled for the player with Movement
// Speed 2x on.
Vector2 APS5_VABI MovementInputDelta(void* movement) {
    const Vector2 delta = Game<SlopeAligned>(kSlopeAligned)(movement, Field<Vector2>(movement, kInputDelta), nullptr);
    if (!movementSpeed.on.load(std::memory_order_relaxed) || !OnPlayer(movement)) return delta;
    if (!DEBUG_SAULO_movementLogged.load(std::memory_order_relaxed) && (delta.x != 0.0f || delta.y != 0.0f)) DEBUG_SAULO_MovementDelta(delta);  // DEBUG_SAULO
    return {delta.x * 2.0f, delta.y * 2.0f};
}

// DEBUG_SAULO: temporary Jump Height diagnostics, remove before commit: one line for the first player jump the cheat raises after it is
// switched on.
std::atomic<bool> DEBUG_SAULO_jumpLogged{false};
// DEBUG_SAULO
__attribute__((noinline)) void DEBUG_SAULO_JumpDirection(Vector2 original, Vector2 modified) {
    if (DEBUG_SAULO_jumpLogged.exchange(true)) return;
    std::fprintf(stderr, "[DEBUG_SAULO] Jump Height player direction original=(%.6f, %.6f) modified=(%.6f, %.6f) upward factor=%.6f\n",
        static_cast<double>(original.x), static_cast<double>(original.y), static_cast<double>(modified.x), static_cast<double>(modified.y),
        static_cast<double>(kJumpSpeedFactor));
    std::fflush(stderr);
}

// Called once at the start of each jump the jump state starts, by MoveStateJump's DelayStartJumping coroutine (this = the MoveStateJump), in
// place of GetJumpAngleAsForwardDirectionVector(this, forwardAdjustmentFactor): the same direction, its upward part raised for the player
// with Jump Height 2x on. The coroutine multiplies it by the jump magnitude and applies it to context as the jump's impulse.
Vector2 APS5_VABI JumpDirection(void* jumpState) {
    Vector2 direction = Game<JumpAngle>(kJumpAngle)(jumpState, Field<float>(jumpState, kForwardAdjustment), nullptr);
    if (!jumpHeight.on.load(std::memory_order_relaxed) || direction.y <= 0.0f || !OnPlayer(Field<void*>(jumpState, kJumpContext))) return direction;
    const Vector2 DEBUG_SAULO_original = direction;  // DEBUG_SAULO
    direction.y *= kJumpSpeedFactor;
    if (!DEBUG_SAULO_jumpLogged.load(std::memory_order_relaxed)) DEBUG_SAULO_JumpDirection(DEBUG_SAULO_original, direction);  // DEBUG_SAULO
    return direction;
}

// Logs the result of one action and shows it in the window title for two seconds; another result restarts that time. item names the
// wallet item a failure is about.
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

// DEBUG_SAULO: temporary wallet action diagnostics, remove before commit. Only key presses and the steps of an action are logged, each
// line in one write and flushed at once, so after a hard stall game.err ends with the last step that was reached.
const char* DEBUG_SAULO_Key(const Award& award) { return &award == &bloodOrbs ? "F7" : "F8"; }  // DEBUG_SAULO
const char* DEBUG_SAULO_Action(const Award& award) { return &award == &bloodOrbs ? "blood_orbs" : "upgrade_materials"; }  // DEBUG_SAULO
// DEBUG_SAULO
const char* DEBUG_SAULO_State(int state) {
    switch (state) {
        case kBloodOrbsQueued: return "blood_orbs_queued";
        case kBloodOrbsRunning: return "blood_orbs_running";
        case kUpgradeMaterialsQueued: return "upgrade_materials_queued";
        case kUpgradeMaterialsRunning: return "upgrade_materials_running";
        default: return "idle";
    }
}
// DEBUG_SAULO
__attribute__((format(gnu_printf, 1, 2))) void DEBUG_SAULO_Log(const char* format, ...) {
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    std::fputs(line, stderr);
    std::fflush(stderr);
}
// DEBUG_SAULO
void DEBUG_SAULO_Resolved(const Award& award, void* const* items) {
    if (award.itemCount == 1) {
        DEBUG_SAULO_Log("[DEBUG_SAULO] Wallet action item resolved item=%s itemDefinition=0x%llx\n", award.items[0].id, DEBUG_SAULO_Address(items[0]));
    } else {
        DEBUG_SAULO_Log("[DEBUG_SAULO] Wallet action material definitions resolved count=%zu\n", award.itemCount);
    }
}
// DEBUG_SAULO
void DEBUG_SAULO_Added(const Award& award, std::size_t index, std::int64_t before, std::int64_t after) {
    if (&award == &bloodOrbs) {
        DEBUG_SAULO_Log("[DEBUG_SAULO] %s %s current=%lld add=%lld result=%lld\n", award.name, DEBUG_SAULO_Key(award), static_cast<long long>(before),
            static_cast<long long>(award.amount), static_cast<long long>(after));
    } else {
        DEBUG_SAULO_Log("[DEBUG_SAULO] %s add item=%s name=%s before=%lld add=%lld after=%lld\n", award.name, award.items[index].id,
            award.items[index].name, static_cast<long long>(before), static_cast<long long>(award.amount), static_cast<long long>(after));
    }
}
// DEBUG_SAULO
void DEBUG_SAULO_Finished(const Award& award, const char* failure) {
    if (failure == nullptr) {
        DEBUG_SAULO_Log("[DEBUG_SAULO] Wallet action complete action=%s\n", DEBUG_SAULO_Action(award));
    } else {
        DEBUG_SAULO_Log("[DEBUG_SAULO] Wallet action failed action=%s reason=%s\n", DEBUG_SAULO_Action(award), failure);
    }
}

// The count of an item in a wallet; the wallet makes its table at the first addition, and GetCurrentAmount needs it.
std::int64_t Amount(void* wallet, void* item) {
    return Field<void*>(wallet, kWalletCount) != nullptr ? Game<GetAmount>(kGetAmount)(wallet, item, nullptr) : 0;
}

// Adds every item of the award to the wallet of the player the controller plays, or nothing at all: each item is found and checked before
// the first addition. Returns why it could not, and in *item the wallet item that is about.
const char* Give(const Award& award, void* controller, const char** item) {
    void* player = Game<GetPawnObject>(kGetPawnObject)(controller, nullptr);
    if (player == nullptr) return "no player";
    DEBUG_SAULO_Log("[DEBUG_SAULO] Wallet action player resolved player=0x%llx\n", DEBUG_SAULO_Address(player));  // DEBUG_SAULO
    void* wallet = Game<TryGetWallet>(kTryGetWallet)(player, nullptr);
    if (wallet == nullptr) return "no player wallet";
    DEBUG_SAULO_Log("[DEBUG_SAULO] Wallet action wallet resolved wallet=0x%llx\n", DEBUG_SAULO_Address(wallet));  // DEBUG_SAULO
    void* items[kMostItems];
    for (std::size_t i = 0; i < award.itemCount; ++i) {
        *item = award.items[i].id;
        void* id = Game<StringNew>(kStringNew)(award.items[i].id);
        items[i] = id != nullptr ? Game<GetItem>(kGetItem)(id, nullptr) : nullptr;
        if (!Alive(items[i])) return "item not found";
    }
    DEBUG_SAULO_Resolved(award, items);  // DEBUG_SAULO
    std::int64_t before[kMostItems];
    for (std::size_t i = 0; i < award.itemCount; ++i) {
        *item = award.items[i].id;
        before[i] = Amount(wallet, items[i]);
        if (before[i] > std::numeric_limits<std::int64_t>::max() - award.amount) return "count at the largest number it can hold";
    }
    *item = nullptr;
    DEBUG_SAULO_Log("[DEBUG_SAULO] Wallet action validation complete\n");  // DEBUG_SAULO
    const NullableBool notification{!award.announce, !award.announce};
    DEBUG_SAULO_Log("[DEBUG_SAULO] Wallet action native %s begin\n", award.itemCount == 1 ? "addition" : "additions");  // DEBUG_SAULO
    for (std::size_t i = 0; i < award.itemCount; ++i) {
        Game<AddToWallet>(kAddToWallet)(wallet, items[i], award.amount, notification, nullptr);
        DEBUG_SAULO_Added(award, i, before[i], Amount(wallet, items[i]));  // DEBUG_SAULO
    }
    DEBUG_SAULO_Log("[DEBUG_SAULO] Wallet action native %s complete\n", award.itemCount == 1 ? "addition" : "additions");  // DEBUG_SAULO
    return nullptr;
}

// Gives the wallet action back (idle) when the game thread's run of it ends, whichever way it ends.
struct WalletActionRun {
    WalletActionRun() = default;
    WalletActionRun(const WalletActionRun&) = delete;
    WalletActionRun& operator=(const WalletActionRun&) = delete;
    ~WalletActionRun() { walletAction.store(kIdle); }
};

// Takes the queued wallet action, if there is one, and does it (game thread).
__attribute__((noinline)) void RunWalletAction(void* controller) {
    int state = walletAction.load(std::memory_order_relaxed);
    if (state != kBloodOrbsQueued && state != kUpgradeMaterialsQueued) return;
    Award& award = state == kBloodOrbsQueued ? bloodOrbs : upgradeMaterials;
    if (!walletAction.compare_exchange_strong(state, award.running)) return;  // given up meanwhile
    const WalletActionRun run;
    DEBUG_SAULO_Log("[DEBUG_SAULO] Wallet action processing begin action=%s\n", DEBUG_SAULO_Action(award));  // DEBUG_SAULO
    const char* item = nullptr;
    const char* failure = Give(award, controller, &item);
    Notify(award, failure, item);
    DEBUG_SAULO_Finished(award, failure);  // DEBUG_SAULO
}

// PlayerController.Update(this), every frame on the game thread. The work is in a separate function of this library's own calling
// convention, where the release of the wallet action can be a destructor (a SysV function here cannot have cleanups).
bool APS5_VABI ControllerFilter(void* controller, void*, void*, float*) {
    RunWalletAction(controller);
    return false;
}

// A queued wallet action no player's controller took within a second fails: no player in the game (title screen, loading), its controller
// not updated, or no frames at all. Only a queued action is given up; a running one belongs to the game thread until it ends.
void ExpireRequest(std::uint64_t now) {
    int state = walletAction.load();
    if ((state != kBloodOrbsQueued && state != kUpgradeMaterialsQueued) || now < walletRequestedAt.load() + kAwardWaitMs) return;
    if (!walletAction.compare_exchange_strong(state, kIdle)) return;  // the game thread took it meanwhile
    Award& award = state == kBloodOrbsQueued ? bloodOrbs : upgradeMaterials;
    Notify(award, "no player in the game", nullptr);
    DEBUG_SAULO_Log("[DEBUG_SAULO] Wallet action expired action=%s\n", DEBUG_SAULO_Action(award));  // DEBUG_SAULO
}

// F7 or F8 on the window thread: asks for its action, unless a wallet action (either one) is queued or running. A queued action that
// waited too long is given up first, here as in the title, which is only updated while frames are presented.
void Request(Award& award) {
    if (awardsUnavailable != nullptr) {
        Notify(award, awardsUnavailable, nullptr);
        return;
    }
    const std::uint64_t now = NowMs();
    ExpireRequest(now);
    if (const int busy = walletAction.load(); busy != kIdle) {
        DEBUG_SAULO_Log("[DEBUG_SAULO] Wallet action %s ignored busy=%s\n", DEBUG_SAULO_Key(award), DEBUG_SAULO_State(busy));  // DEBUG_SAULO
        return;
    }
    walletRequestedAt.store(now);    // before the action is queued, so the expiry never reads an older time
    walletAction.store(award.queued);  // only this thread leaves idle, so nothing else took the action since the check
    DEBUG_SAULO_Log("[DEBUG_SAULO] Wallet action %s queued action=%s\n", DEBUG_SAULO_Key(award), DEBUG_SAULO_Action(award));  // DEBUG_SAULO
}

// A hooked method starts with a jump to its hook (mov rax, hook; jmp rax over its first instructions, rdi = this, xmm0 = a float
// argument). The hook calls filter(this, rsi, rdx, &xmm0) with the arguments kept and returns from the method when it returns true;
// otherwise the method's first instructions, copied behind it, run and a jump that changes no register or flag continues the method
// after them (OffsetHp's first instructions set eax and the flags it uses later).
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

// A method to hook: its first whole instructions (at least kEntryJump bytes, without rip-relative operands or jumps into them) are copied;
// a method shorter than kEntryJump is copied whole, and the entry jump also covers the padding after it.
struct Patch {
    std::uint32_t method;
    std::size_t prologue;
    Filter filter;
};

struct Range { std::uint32_t begin, end; };

// FNV-1a of code ranges that have no relocations, so it only matches the analysed game build; they include the replaced instructions.
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

// The call MovementInputDelta replaces: vmovsd xmm0, qword ptr [rbx + 0xe0]; mov rdi, rbx; call GetSlopeAlignedVersionOfMovementDelta. It
// becomes mov rdi, rbx; mov rax, MovementInputDelta; call rax; nop, at the same stack depth. rax is free there (the call returns in xmm0
// and rax is written before it is read again), and the only jump into these bytes lands on the first one.
constexpr std::uint8_t kInputDeltaCallBytes[] = {0xc5, 0xfb, 0x10, 0x83, 0xe0, 0x00, 0x00, 0x00, 0x48, 0x89, 0xdf, 0xe8, 0x67, 0x67, 0xfe, 0xff};

// The call JumpDirection replaces: vmovss xmm0, dword ptr [rbx + 0x9c]; mov r14, qword ptr [rbx + 0x58]; mov rdi, rbx; call
// GetJumpAngleAsForwardDirectionVector. It becomes mov r14, qword ptr [rbx + 0x58]; mov rdi, rbx; mov rax, JumpDirection; call rax; nop, at
// the same stack depth (JumpDirection reads forwardAdjustmentFactor itself). rax is free there for the same reason, and no jump lands
// inside these bytes.
constexpr std::uint8_t kJumpDirectionCallBytes[] = {0xc5, 0xfa, 0x10, 0x83, 0x9c, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x73, 0x58,
                                                    0x48, 0x89, 0xdf, 0xe8, 0x05, 0xe3, 0xff, 0xff};

// The computation ManaAfterOffset replaces, with this = rbx and the change in xmm0: vaddss xmm0, xmm0, dword ptr [rbx + 0x28]; vmovss
// xmm1, dword ptr [rbx + 0x2c]; vxorps xmm2, xmm2, xmm2; vminss xmm1, xmm1, xmm0; vcmpltss xmm0, xmm0, xmm2; vandnps xmm0, xmm0, xmm1. It
// becomes mov rdi, rbx; mov rax, ManaAfterOffset; call rax; and 12 bytes of nop, and the game's next instruction stores xmm0 in
// currentMana. The stack is 16-byte aligned there, nothing the call may change is read after it before being written (rbx and r14, which
// the method keeps, survive the call), and the only jump into these bytes lands on the first one.
constexpr std::uint8_t kManaClampBytes[] = {0xc5, 0xfa, 0x58, 0x43, 0x28, 0xc5, 0xfa, 0x10, 0x4b, 0x2c, 0xc5, 0xe8, 0x57, 0xd2,
                                            0xc5, 0xf2, 0x5d, 0xc8, 0xc5, 0xfa, 0xc2, 0xc2, 0x01, 0xc5, 0xf8, 0x55, 0xc1};

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

// A cheat whose game code is not recognised stays off and unavailable; the others are not affected. redirect, if any, patches a call.
void Start(Cheat& cheat, bool recognised, std::initializer_list<Patch> patches, bool (*redirect)() = nullptr) {
    const char* value = std::getenv(cheat.variable);
    const bool on = value != nullptr && std::strcmp(value, "1") == 0;
    std::fprintf(stderr, "%s: %s\n", cheat.name, on ? "ON" : "OFF");
    if (!recognised) {
        std::fprintf(stderr, "%s: game code not recognised, cheat disabled\n", cheat.name);
        return;
    }
    for (const Patch& patch : patches) {
        if (!Hook(patch)) {
            std::fprintf(stderr, "%s: game code could not be patched, cheat disabled\n", cheat.name);
            return;
        }
    }
    if (redirect != nullptr && !redirect()) {
        std::fprintf(stderr, "%s: game code could not be patched, cheat disabled\n", cheat.name);
        return;
    }
    cheat.hooked = true;
    cheat.on.store(on);
    std::fprintf(stderr, "%s: patch applied\n", cheat.name);
}

// Runs on the window thread for every key press in the game window.
void OnKey(int scancode) {
    if (scancode == kBloodOrbsKey || scancode == kUpgradeMaterialsKey) {
        Request(scancode == kBloodOrbsKey ? bloodOrbs : upgradeMaterials);
        return;
    }
    Cheat* cheat = scancode == kGodModeKey ? &godMode
                 : scancode == kInfiniteSpiritKey ? &infiniteSpirit
                 : scancode == kInfiniteMagicKey  ? &infiniteMagic
                 : scancode == kMovementSpeedKey ? &movementSpeed
                 : scancode == kJumpHeightKey    ? &jumpHeight
                                                 : nullptr;
    if (cheat == nullptr) return;
    const char* notice = "unavailable";
    if (cheat->hooked) {
        const bool on = !cheat->on.load();
        cheat->on.store(on);
        notice = on ? nullptr : "OFF";
    }
    std::fprintf(stderr, "%s: %s\n", cheat->name, notice != nullptr ? notice : "ON");
    if (cheat == &godMode && cheat->hooked) std::fprintf(stderr, "[DEBUG_SAULO] GodMode F1 toggle %s\n", cheat->on.load() ? "ON" : "OFF");  // DEBUG_SAULO
    // DEBUG_SAULO
    if (cheat == &infiniteMagic && cheat->hooked) {
        DEBUG_SAULO_magicLogged.store(false);
        DEBUG_SAULO_Log("[DEBUG_SAULO] Infinite Magic F3 toggle %s\n", cheat->on.load() ? "ON" : "OFF");
    }
    // DEBUG_SAULO
    if (cheat == &movementSpeed && cheat->hooked) {
        DEBUG_SAULO_movementLogged.store(false);
        DEBUG_SAULO_Log("[DEBUG_SAULO] Movement Speed F5 toggle %s\n", cheat->on.load() ? "ON" : "OFF");
    }
    // DEBUG_SAULO
    if (cheat == &jumpHeight && cheat->hooked) {
        DEBUG_SAULO_jumpLogged.store(false);
        DEBUG_SAULO_Log("[DEBUG_SAULO] Jump Height F6 toggle %s\n", cheat->on.load() ? "ON" : "OFF");
    }
    cheat->notice.store(notice);
    cheat->noticeUntil.store(notice != nullptr ? NowMs() + kNoticeMs : 0);
}

// Appended to the window title after the FPS counter (on the same window thread as the keys): every cheat that is on, for two seconds
// one that a key turned off, and for two seconds the result of F7 and F8. Requests that waited too long fail here.
void TitleStatus(char* text, std::size_t size) {
    const std::uint64_t now = NowMs();
    ExpireRequest(now);
    std::size_t used = 0;
    if (size != 0) text[0] = '\0';
    for (const Cheat* cheat : {&godMode, &infiniteSpirit, &infiniteMagic, &movementSpeed, &jumpHeight}) {
        const char* state = cheat->on.load() ? "ON" : now < cheat->noticeUntil.load() ? cheat->notice.load() : nullptr;
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

bool StartCheats() {
    game = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(L"Il2cppUserAssemblies.prx.guest.prx"));
    if (game == nullptr) return false;
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(game + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const bool fits = headers->OptionalHeader.SizeOfImage >= kGetGameObject + sizeof(void*);
    // God Mode: PawnUtils.GetMyController and IsPlayer, Vitals.InvokeUpdateEvents to OffsetHp, OffsetGreyHp and the grey damage check it calls.
    const bool godModeRecognised = fits && Hash({{0x80ecd0, 0x80f0b0}, {0xdab1a0, 0xdab6b0}, {0xdabaf0, 0xdabd90}}) == 0x4b6beba820a6ebce;
    // DEBUG_SAULO: temporary hook install details, remove before commit.
    std::fprintf(stderr, "[DEBUG_SAULO] GodMode hook install moduleBase=0x%llx offsetHp=0x%llx offsetGreyHp=0x%llx isPlayer=0x%llx getGameObjectSlot=0x%llx "
        "fields maxHp=+0x%zx currentHp=+0x%zx greyHp=+0x%zx dead=+0x%zx\n", DEBUG_SAULO_Address(game), DEBUG_SAULO_Address(game + kOffsetHp),
        DEBUG_SAULO_Address(game + kOffsetGreyHp), DEBUG_SAULO_Address(game + kIsPlayer), DEBUG_SAULO_Address(game + kGetGameObject),  // DEBUG_SAULO
        kMaxHp, kCurrentHp, kGreyHp, kDead);  // DEBUG_SAULO
    std::fprintf(stderr, "[DEBUG_SAULO] GodMode fingerprint %s\n", godModeRecognised ? "OK" : "FAILED");  // DEBUG_SAULO
    Start(godMode, godModeRecognised, {{kOffsetHp, 12, HealthFilter}, {kOffsetGreyHp, 13, GreyHealthFilter}});
    for (std::uint32_t DEBUG_SAULO_method : {kOffsetHp, kOffsetGreyHp}) {  // DEBUG_SAULO: the hook addresses now written into the methods
        if (!godMode.hooked) break;  // DEBUG_SAULO
        void* thunk = nullptr;  // DEBUG_SAULO
        std::memcpy(&thunk, game + DEBUG_SAULO_method + 2, sizeof(thunk));  // DEBUG_SAULO
        std::fprintf(stderr, "[DEBUG_SAULO] GodMode hook %s thunk=0x%llx\n", DEBUG_SAULO_Method(DEBUG_SAULO_method == kOffsetGreyHp),  // DEBUG_SAULO
            DEBUG_SAULO_Address(thunk));  // DEBUG_SAULO
    }
    // Infinite Spartan Spirit: SpartanSpirit.OffsetSpartanSpirit and Update, PawnUtils.GetMyController and IsPlayer.
    Start(infiniteSpirit, fits && Hash({{0xda20b0, 0xda2520}, {0xda2b10, 0xda2d10}, {0x80ecd0, 0x80f0b0}}) == 0xb0e9d2c9cfd2a0b1,
        {{kOffsetSpirit, 12, SpiritFilter}});
    // Infinite Magic: ManaState.Awake to Validate (OffsetMana, with the computation it redirects, and what sets currentMaxMana), what
    // spends and changes mana (EventModule_AffectMana.ApplyModule and AffectMana, TickModule_OffsetManaPerSec.Tick), the mana checks that
    // read currentMana (BooleanEvaluator_ManaCheck and BooleanEvaluator_HasNoMana), PawnUtils.GetMyController and IsPlayer.
    const bool magicRecognised = fits && Hash({{0xd954f0, 0xd95bd0}, {0xdf2980, 0xdf2bc0}, {0x823d20, 0x823e30}, {0xc26550, 0xc26750},
        {0xc1af20, 0xc1afe0}, {0x80ecd0, 0x80f0b0}}) == 0xa63f97d69c46ad8d;
    DEBUG_SAULO_Log("[DEBUG_SAULO] Infinite Magic hook install offsetMana=0x%llx clamp=0x%llx fields currentMana=+0x%zx maxMana=+0x%zx fingerprint %s\n",  // DEBUG_SAULO
        DEBUG_SAULO_Address(game + kOffsetMana), DEBUG_SAULO_Address(game + kManaClamp), kCurrentMana, kMaxMana, magicRecognised ? "OK" : "FAILED");  // DEBUG_SAULO
    Start(infiniteMagic, magicRecognised, {}, RedirectManaClamp);
    // DEBUG_SAULO
    if (infiniteMagic.hooked) {
        void* target = nullptr;
        std::memcpy(&target, game + kManaClamp + 5, sizeof(target));
        DEBUG_SAULO_Log("[DEBUG_SAULO] Infinite Magic patch applied clamp=0x%llx now calls 0x%llx\n", DEBUG_SAULO_Address(game + kManaClamp),
            DEBUG_SAULO_Address(target));
    }
    // Blood Orbs and upgrade materials: il2cpp_string_new, Controller.GetPawnObject, PlayerController.Update and the padding after it,
    // PlayerRuntimeDataUtils.TryGetWallet, PlayerInventory.GetItemDefinitionByItemId, PlayerWallet.OffsetWallet to TrySubtractFromWallet,
    // PlayerHUDMenu.OnWalletItemAdded and TryQueueItemNotification (what the Nullable<bool> means), and
    // StaticDependencySubsystem.ResolveItemDataDependency.
    const bool awardsRecognised = fits && Hash({{0x32fac0, 0x32fae0}, {0x5d12d0, 0x5d1390}, {0x5d35c0, 0x5d35d0}, {0x8c0810, 0x8c0950},
        {0x8c91b0, 0x8c92b0}, {0x8ce0f0, 0x8ce6d0}, {0x996d20, 0x996e70}, {0x9971b0, 0x997390}, {0xd11230, 0xd11300}}) == 0xdcbbc10a00bb8f1e;
    std::fprintf(stderr, "[DEBUG_SAULO] Awards hook install controllerUpdate=0x%llx getPawnObject=0x%llx tryGetWallet=0x%llx getItem=0x%llx "  // DEBUG_SAULO
        "addToWallet=0x%llx fingerprint %s\n", DEBUG_SAULO_Address(game + kControllerUpdate), DEBUG_SAULO_Address(game + kGetPawnObject),  // DEBUG_SAULO
        DEBUG_SAULO_Address(game + kTryGetWallet), DEBUG_SAULO_Address(game + kGetItem), DEBUG_SAULO_Address(game + kAddToWallet),  // DEBUG_SAULO
        awardsRecognised ? "OK" : "FAILED");  // DEBUG_SAULO
    if (awardsRecognised) awardsUnavailable = Hook({kControllerUpdate, 1, ControllerFilter}) ? nullptr : "game code could not be patched";
    if (awardsUnavailable != nullptr) {
        std::fprintf(stderr, "Blood Orbs and Upgrade Materials: %s, keys disabled\n", awardsUnavailable);
    } else {
        std::fprintf(stderr, "Blood Orbs and Upgrade Materials: patch applied\n");
    }
    // Movement Speed 2x: PlayerMovement.TickPseudoPhysics (with the call it redirects), CharacterMovement.GetSlopeAlignedVersionOfMovementDelta,
    // the CharacterMovement.UseAsMovementDelta overloads that set the delta, PlayerMovement.MovementTick that scales it and calls
    // TickPseudoPhysics, PawnUtils.GetMyController and IsPlayer.
    const bool movementRecognised = fits && Hash({{0x706620, 0x706b50}, {0x6ecf20, 0x6ed280}, {0x6f02e0, 0x6f06d0}, {0x708d70, 0x709360},
        {0x80ecd0, 0x80f0b0}}) == 0xe4fb67f213ff4921;
    DEBUG_SAULO_Log("[DEBUG_SAULO] Movement Speed hook install call=0x%llx slopeAligned=0x%llx inputDelta=+0x%zx fingerprint %s\n",  // DEBUG_SAULO
        DEBUG_SAULO_Address(game + kInputDeltaCall), DEBUG_SAULO_Address(game + kSlopeAligned), kInputDelta, movementRecognised ? "OK" : "FAILED");  // DEBUG_SAULO
    Start(movementSpeed, movementRecognised, {}, RedirectInputDelta);
    // DEBUG_SAULO
    if (movementSpeed.hooked) {
        void* target = nullptr;
        std::memcpy(&target, game + kInputDeltaCall + 5, sizeof(target));
        DEBUG_SAULO_Log("[DEBUG_SAULO] Movement Speed patch applied call=0x%llx now calls 0x%llx\n", DEBUG_SAULO_Address(game + kInputDeltaCall),
            DEBUG_SAULO_Address(target));
    }
    // Jump Height 2x: <DelayStartJumping>d__27.MoveNext (with the call it redirects), MoveStateJump.GetJumpAngleAsForwardDirectionVector,
    // CharacterMovement.CalcVelocity and ApplyForce (the force is one impulse, gravity is constant while rising, the upward speed is not
    // capped), PlayerMovement.MovementTick that clears the force after each step, PawnUtils.GetMyController and IsPlayer.
    const bool jumpRecognised = fits && Hash({{0x6fc450, 0x6fcbf0}, {0x6face0, 0x6fb1b0}, {0x6efe80, 0x6f0180}, {0x6f0810, 0x6f08e0},
        {0x708d70, 0x709360}, {0x80ecd0, 0x80f0b0}}) == 0x6b8174068fca8cd1;
    DEBUG_SAULO_Log("[DEBUG_SAULO] Jump Height hook install call=0x%llx jumpAngle=0x%llx context=+0x%zx fingerprint %s\n",  // DEBUG_SAULO
        DEBUG_SAULO_Address(game + kJumpDirectionCall), DEBUG_SAULO_Address(game + kJumpAngle), kJumpContext, jumpRecognised ? "OK" : "FAILED");  // DEBUG_SAULO
    Start(jumpHeight, jumpRecognised, {}, RedirectJumpDirection);
    // DEBUG_SAULO
    if (jumpHeight.hooked) {
        void* target = nullptr;
        std::memcpy(&target, game + kJumpDirectionCall + 9, sizeof(target));
        DEBUG_SAULO_Log("[DEBUG_SAULO] Jump Height patch applied call=0x%llx now calls 0x%llx\n", DEBUG_SAULO_Address(game + kJumpDirectionCall),
            DEBUG_SAULO_Address(target));
    }
    HostExtensionRegister_nid_no_patch(OnKey, TitleStatus);
    return true;
}

[[maybe_unused]] const bool cheatsStarted = StartCheats();

}
#endif
