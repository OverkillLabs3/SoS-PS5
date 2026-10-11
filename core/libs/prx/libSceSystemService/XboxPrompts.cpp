#ifdef _WIN32
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libScePad/include/PadControllerChannel.hpp"

namespace {

// Offsets are RVAs in Il2cppUserAssemblies.
std::uint8_t* game = nullptr;

constexpr std::uint32_t kResolveIconsForCurrentInput = 0x67ae10;
constexpr std::uint32_t kResolveIconsForCurrentInputEnd = 0x67b120;

// Whole instructions with no rip-relative operand, so they can run from the trampoline.
constexpr std::uint8_t kPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xd0, 0x01, 0x00, 0x00};

// InputOverseer fields: m_currentInputDevice and m_currentControllerUsed.
constexpr std::size_t kInputDevice = 0x30;
constexpr std::size_t kControllerUsed = 0x34;

enum ControlDevice : std::int32_t { Keyboard, Mouse, Gamepad };
enum GamepadType : std::int32_t { NONE, XboxOne, XboxSeries, PS4, PS5, Switch };

enum class ButtonIcons : std::uint8_t { Automatic, PlayStation, Xbox };

// FNV-1a of the code the offsets and the prologue were read from, so only the analysed game build is patched.
constexpr std::uint64_t kCodeHash = 0x18a26483313682af;

// Saves the argument registers, calls SelectPrompts(this) with the stack aligned, restores them and runs the copied prologue.
constexpr std::uint8_t kTrampolineStart[] = {
    0x57,                                 // push rdi
    0x56,                                 // push rsi
    0x52,                                 // push rdx
    0x51,                                 // push rcx
    0x41, 0x50,                           // push r8
    0x41, 0x51,                           // push r9
    0x48, 0x83, 0xec, 0x08,               // sub rsp, 8
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,   // mov rax, SelectPrompts (offset 14)
    0xff, 0xd0,                           // call rax
    0x48, 0x83, 0xc4, 0x08,               // add rsp, 8
    0x41, 0x59,                           // pop r9
    0x41, 0x58,                           // pop r8
    0x59,                                 // pop rcx
    0x5a,                                 // pop rdx
    0x5e,                                 // pop rsi
    0x5f,                                 // pop rdi
};
constexpr std::size_t kTrampolineHandler = 14;
constexpr std::uint8_t kJumpBack[] = {0xff, 0x25, 0, 0, 0, 0};  // jmp qword ptr [rip], followed by the address
constexpr std::size_t kEntryJump = 12;  // mov rax, trampoline; jmp rax
static_assert(sizeof(kPrologue) >= kEntryJump);

ButtonIcons buttonIcons = ButtonIcons::Automatic;

// SOS_CONTROLLER_BUTTON_ICONS from the launcher; no variable or any other value is Automatic.
ButtonIcons ParseButtonIcons(const char* value) {
    if (value != nullptr && std::strcmp(value, "playstation") == 0) return ButtonIcons::PlayStation;
    if (value != nullptr && std::strcmp(value, "xbox") == 0) return ButtonIcons::Xbox;
    return ButtonIcons::Automatic;
}

bool UsesXboxIcons(ButtonIcons setting, PadControllerKind physical) {
    return setting == ButtonIcons::Xbox || (setting == ButtonIcons::Automatic && physical == PadControllerKind::Xbox);
}

std::int32_t* Int32(void* object, std::size_t offset) { return reinterpret_cast<std::int32_t*>(static_cast<std::uint8_t*>(object) + offset); }

// The controller type the game had before the override, restored once no Xbox prompts apply.
bool replaced = false;
std::int32_t gameControllerUsed = 0;

// Entered with the InputOverseer in rdi, on the game thread.
void APS5_VABI SelectPrompts(void* overseer) {
    const bool xbox = UsesXboxIcons(buttonIcons, PadControllerChannel::Current());
    const bool override = xbox && *Int32(overseer, kInputDevice) == Gamepad;
    std::int32_t& controllerUsed = *Int32(overseer, kControllerUsed);
    if (override) {
        if (controllerUsed != XboxSeries) {
            if (!replaced) gameControllerUsed = controllerUsed;
            replaced = true;
            controllerUsed = XboxSeries;
        }
    } else if (replaced) {
        if (controllerUsed == XboxSeries) controllerUsed = gameControllerUsed;
        replaced = false;
    }
}

bool Recognised(std::uint32_t imageSize) {
    if (kResolveIconsForCurrentInputEnd > imageSize) return false;
    std::uint64_t hash = 0xcbf29ce484222325;
    for (std::uint32_t offset = kResolveIconsForCurrentInput; offset < kResolveIconsForCurrentInputEnd; ++offset) hash = (hash ^ game[offset]) * 0x100000001b3;
    return hash == kCodeHash;
}

bool Install() {
    const std::size_t size = sizeof(kTrampolineStart) + sizeof(kPrologue) + sizeof(kJumpBack) + sizeof(void*);
    auto* trampoline = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (trampoline == nullptr) return false;
    std::uint8_t* const method = game + kResolveIconsForCurrentInput;
    const void* const resume = method + sizeof(kPrologue);
    const auto handler = &SelectPrompts;
    std::memcpy(trampoline, kTrampolineStart, sizeof(kTrampolineStart));
    std::memcpy(trampoline + kTrampolineHandler, &handler, sizeof(handler));
    std::memcpy(trampoline + sizeof(kTrampolineStart), kPrologue, sizeof(kPrologue));
    std::memcpy(trampoline + sizeof(kTrampolineStart) + sizeof(kPrologue), kJumpBack, sizeof(kJumpBack));
    std::memcpy(trampoline + sizeof(kTrampolineStart) + sizeof(kPrologue) + sizeof(kJumpBack), &resume, sizeof(resume));
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

bool StartXboxPrompts() {
    buttonIcons = ParseButtonIcons(std::getenv("SOS_CONTROLLER_BUTTON_ICONS"));
    game = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(L"Il2cppUserAssemblies.prx.guest.prx"));
    const auto* headers = game != nullptr ? reinterpret_cast<const IMAGE_NT_HEADERS*>(game + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew) : nullptr;
    if (headers == nullptr || !Recognised(headers->OptionalHeader.SizeOfImage) ||
        std::memcmp(game + kResolveIconsForCurrentInput, kPrologue, sizeof(kPrologue)) != 0) {
        std::fprintf(stderr, "Xbox prompts: the prompt code was not recognised, prompts keep the PlayStation set\n");
        return false;
    }
    if (!Install()) {
        std::fprintf(stderr, "Xbox prompts: the prompt code could not be patched, prompts keep the PlayStation set\n");
        return false;
    }
    std::fprintf(stderr, "Xbox prompts: patch applied\n");
    return true;
}

[[maybe_unused]] const bool xboxPromptsStarted = StartXboxPrompts();

}
#endif
