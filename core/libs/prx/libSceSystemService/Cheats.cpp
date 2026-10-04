#ifdef _WIN32
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <windows.h>
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/HostExtension/include/HostExtension.hpp"

namespace {

// Sons of Sparta cheats, for the player only and switched while playing: F5 God Mode, F6 Infinite Spartan Spirit (F7 is kept for a
// future Infinite Magic). The launcher passes the starting states as SOS_GOD_MODE=1 and SOS_INFINITE_SPARTAN_SPIRIT=1. When this library
// loads, before any game code runs, each cheat whose game code is recognised gets its hooks in Il2cppUserAssemblies; the hooks stay for
// the whole session, the keys only switch the states, and the window title lists the cheats that are on. The two cheats share no hook.
// Offsets are module RVAs.
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

// SDL scancodes of the cheat keys; F7 (64) is kept for Infinite Magic.
constexpr int kGodModeKey = 62, kInfiniteSpiritKey = 63;
constexpr std::uint64_t kNoticeMs = 2000;

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint8_t* game = nullptr;
constexpr std::uint32_t kOffsetHp = 0xdab550;       // EntityComponents.Vitals.OffsetHp(float)
constexpr std::uint32_t kOffsetGreyHp = 0xdabaf0;   // EntityComponents.Vitals.OffsetGreyHp(float)
constexpr std::uint32_t kOffsetSpirit = 0xda20b0;   // EntityComponents.SpartanSpirit.OffsetSpartanSpirit(float, bool)
constexpr std::uint32_t kIsPlayer = 0x80ef70;       // Utilities.PawnUtils.IsPlayer(GameObject)
constexpr std::uint32_t kGetGameObject = 0x629c8f0; // Component.get_gameObject icall pointer, resolved by the game before it is needed

constexpr std::size_t kNativeObject = 0x10;                                       // UnityEngine.Object.m_CachedPtr, null once destroyed
constexpr std::size_t kMaxHp = 0x38, kCurrentHp = 0x40, kGreyHp = 0x44, kDead = 0x48;  // Vitals
constexpr std::size_t kCurrentSpirit = 0x20, kMaxSpirit = 0x24;                   // SpartanSpirit

using GetGameObject = void*(APS5_VABI*)(void* component);
using IsPlayer = bool(APS5_VABI*)(void* gameObject, const void* method);
using Filter = bool(APS5_VABI*)(void* self, void* argument, void* method, float* amount);

template <typename T> T& Field(void* object, std::size_t offset) { return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset); }

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

// A method to hook: its first whole instructions (at least kEntryJump bytes, without rip-relative operands or jumps into them) are copied.
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
    Cheat* cheat = scancode == kGodModeKey ? &godMode : scancode == kInfiniteSpiritKey ? &infiniteSpirit : nullptr;
    if (cheat == nullptr) return;
    const char* notice = "unavailable";
    if (cheat->hooked) {
        const bool on = !cheat->on.load();
        cheat->on.store(on);
        notice = on ? nullptr : "OFF";
    }
    std::fprintf(stderr, "%s: %s\n", cheat->name, notice != nullptr ? notice : "ON");
    if (cheat == &godMode && cheat->hooked) std::fprintf(stderr, "[DEBUG_SAULO] GodMode toggle %s\n", cheat->on.load() ? "ON" : "OFF");  // DEBUG_SAULO
    cheat->notice.store(notice);
    cheat->noticeUntil.store(notice != nullptr ? NowMs() + kNoticeMs : 0);
}

// Appended to the window title after the FPS counter (on the same window thread as the keys): every cheat that is on, and for two
// seconds one that a key turned off.
void TitleStatus(char* text, std::size_t size) {
    const std::uint64_t now = NowMs();
    std::size_t used = 0;
    if (size != 0) text[0] = '\0';
    for (const Cheat* cheat : {&godMode, &infiniteSpirit}) {
        const char* state = cheat->on.load() ? "ON" : now < cheat->noticeUntil.load() ? cheat->notice.load() : nullptr;
        if (state == nullptr || used >= size) continue;
        const int written = std::snprintf(text + used, size - used, " | %s: %s", cheat->label, state);
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
    HostExtensionRegister_nid_no_patch(OnKey, TitleStatus);
    return true;
}

[[maybe_unused]] const bool cheatsStarted = StartCheats();

}
#endif
