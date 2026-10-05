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

// Sons of Sparta cheats, for the player only and used while playing: F1 God Mode and F2 Infinite Spartan Spirit are switched on and off,
// F4 adds 1000 Blood Orbs and F5 adds 10 of each upgrade material (F3 is kept for a future Infinite Magic). The launcher passes the
// starting states of the two switches as SOS_GOD_MODE=1 and SOS_INFINITE_SPARTAN_SPIRIT=1. When this library loads, before any game code
// runs, each of them whose game code is recognised gets its hooks in Il2cppUserAssemblies; the hooks stay for the whole session, the keys
// only switch the states or ask for an addition, and the window title lists the cheats that are on. No two of them share a hook. Offsets
// are module RVAs.
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
// Blood Orbs (the game's item loot_orb_red) and the upgrade materials are counts in the player's PersistentData.PlayerWallet, which is
// saved with the player's data. F4 and F5 add to them with PlayerWallet.AddToWallet, as collecting Blood Orbs or opening a chest does:
// OffsetWallet adds to the count, OnAddedToWallet tells the HUD (the Blood Orb counter counts up), and the game's next save keeps the new
// counts. Its last argument, a Nullable<bool>, only decides whether PlayerHUDMenu announces the item (item ticker, Major Get or card): F4
// leaves it unset, so Blood Orbs show as when collected, and F5 sets it to true, so its nine additions queue no HUD notification. Game
// objects must not be touched on the window thread, so a key only asks for its action; the next PlayerController.Update (empty, called by
// the game every frame for the player's controller) finds the wallet of the player that controller plays and the items the way a save
// loads them, and adds. F4 and F5 do nothing while either action waits or runs, and an action no player picks up within a second fails.

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

// The one wallet action that may be outstanding, shared by F4 and F5: a key press is taken only while it is idle, so the two actions
// never wait or run at the same time. Only the window thread (keys) moves it from idle to queued; the game thread moves a queued action
// to running and, when that run ends, back to idle; the window thread gives up a queued action no game thread took in time. A running
// action belongs to the game thread until it ends.
enum : int { kIdle, kBloodOrbsQueued, kBloodOrbsRunning, kUpgradeMaterialsQueued, kUpgradeMaterialsRunning };
std::atomic<int> walletAction{kIdle};
std::atomic<std::uint64_t> walletRequestedAt{0};  // when the queued action was asked for

// What F4 or F5 asks for: the window thread queues it, the game thread does it, and the result shows in the title for a while.
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
// Why F4 and F5 do nothing in this session; null once their hook is in place.
const char* awardsUnavailable = "game code not recognised";
const char* const kUnavailable = ": unavailable";

// SDL scancodes of the cheat keys; F3 (60) is kept for Infinite Magic and F6 to F10 for later cheats, F11 is the runtime's fullscreen.
constexpr int kGodModeKey = 58, kInfiniteSpiritKey = 59, kBloodOrbsKey = 61, kUpgradeMaterialsKey = 62;
constexpr std::uint64_t kNoticeMs = 2000;
constexpr std::uint64_t kAwardWaitMs = 1000;  // how long a requested action waits for the player's controller

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint8_t* game = nullptr;
constexpr std::uint32_t kOffsetHp = 0xdab550;       // EntityComponents.Vitals.OffsetHp(float)
constexpr std::uint32_t kOffsetGreyHp = 0xdabaf0;   // EntityComponents.Vitals.OffsetGreyHp(float)
constexpr std::uint32_t kOffsetSpirit = 0xda20b0;   // EntityComponents.SpartanSpirit.OffsetSpartanSpirit(float, bool)
constexpr std::uint32_t kIsPlayer = 0x80ef70;       // Utilities.PawnUtils.IsPlayer(GameObject)
constexpr std::uint32_t kGetGameObject = 0x629c8f0; // Component.get_gameObject icall pointer, resolved by the game before it is needed
constexpr std::uint32_t kControllerUpdate = 0x5d35c0;  // PlayerController.Update(), empty
constexpr std::uint32_t kGetPawnObject = 0x5d12d0;     // Controller.GetPawnObject(), null without a living pawn
constexpr std::uint32_t kStringNew = 0x32fac0;         // il2cpp_string_new(const char*), exported by the game module
constexpr std::uint32_t kGetItem = 0x8c91b0;           // PersistentData.PlayerInventory.GetItemDefinitionByItemId(string), as saves load items
constexpr std::uint32_t kTryGetWallet = 0x8c0810;      // RuntimeData.PlayerRuntimeDataUtils.TryGetWallet(GameObject)
constexpr std::uint32_t kGetAmount = 0x8ce2b0;         // PersistentData.PlayerWallet.GetCurrentAmount(ItemDefinition)
constexpr std::uint32_t kAddToWallet = 0x8ce430;       // PersistentData.PlayerWallet.AddToWallet(ItemDefinition, long, bool?)

constexpr std::size_t kNativeObject = 0x10;                                       // UnityEngine.Object.m_CachedPtr, null once destroyed
constexpr std::size_t kMaxHp = 0x38, kCurrentHp = 0x40, kGreyHp = 0x44, kDead = 0x48;  // Vitals
constexpr std::size_t kCurrentSpirit = 0x20, kMaxSpirit = 0x24;                   // SpartanSpirit
constexpr std::size_t kWalletCount = 0x20;  // PlayerWallet.walletCount (item -> count), created by the first addition

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
const char* DEBUG_SAULO_Key(const Award& award) { return &award == &bloodOrbs ? "F4" : "F5"; }  // DEBUG_SAULO
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

// F4 or F5 on the window thread: asks for its action, unless a wallet action (either one) is queued or running. A queued action that
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

// A cheat whose game code is not recognised stays off and unavailable; the other one is not affected.
void Start(Cheat& cheat, bool recognised, std::initializer_list<Patch> patches) {
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
    Cheat* cheat = scancode == kGodModeKey ? &godMode : scancode == kInfiniteSpiritKey ? &infiniteSpirit : nullptr;
    if (cheat == nullptr) return;
    const char* notice = "unavailable";
    if (cheat->hooked) {
        const bool on = !cheat->on.load();
        cheat->on.store(on);
        notice = on ? nullptr : "OFF";
    }
    std::fprintf(stderr, "%s: %s\n", cheat->name, notice != nullptr ? notice : "ON");
    if (cheat == &godMode && cheat->hooked) std::fprintf(stderr, "[DEBUG_SAULO] GodMode F1 toggle %s\n", cheat->on.load() ? "ON" : "OFF");  // DEBUG_SAULO
    cheat->notice.store(notice);
    cheat->noticeUntil.store(notice != nullptr ? NowMs() + kNoticeMs : 0);
}

// Appended to the window title after the FPS counter (on the same window thread as the keys): every cheat that is on, for two seconds
// one that a key turned off, and for two seconds the result of F4 and F5. Requests that waited too long fail here.
void TitleStatus(char* text, std::size_t size) {
    const std::uint64_t now = NowMs();
    ExpireRequest(now);
    std::size_t used = 0;
    if (size != 0) text[0] = '\0';
    for (const Cheat* cheat : {&godMode, &infiniteSpirit}) {
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
    HostExtensionRegister_nid_no_patch(OnKey, TitleStatus);
    return true;
}

[[maybe_unused]] const bool cheatsStarted = StartCheats();

}
#endif
