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

// PromptTextSwitch.UpdatePromptDescriptions, which every Settings confirmation dialog calls when it opens. The dialog's two glyphs are
// plain Images, so its labels (textList[2] and [3]) are found first and each glyph is the only sibling that has an Image.
constexpr std::uint32_t kUpdatePromptDescriptions = 0xd68950;
constexpr std::uint8_t kPromptEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x80, 0x3d};
constexpr std::size_t kPromptStolen = 18;  // through cmp byte ptr [rip + flag], 0
constexpr std::size_t kPromptKept = 11;    // the pushes; the cmp is rebuilt with an absolute address
constexpr std::uint32_t kImageSlot = 0x5e66588, kOverseerSlot = 0x5e6d880;
constexpr std::uint32_t kGetComponent = 0x1565c30, kGetOverseer = 0x15e14f0, kSetOverrideSprite = 0x44e77e0;
constexpr std::uint32_t kTransformOf = 0x436e1f0, kParentOf = 0x4384a10, kChildCount = 0x438a190, kChildAt = 0x438aa20;
constexpr std::size_t kTextList = 0x20, kListItems = 0x10, kListSize = 0x18, kArrayItems = 0x20;
constexpr std::size_t kNativeObject = 0x10;
constexpr std::size_t kIconData = 0x20, kXboxConfirm = 0xc8, kXboxBack = 0xd8;
constexpr std::size_t kOverrideSprite = 0xe0;
constexpr std::size_t kConfirmLabel = 2, kBackLabel = 3;
constexpr std::uint64_t kPromptCodeHash = 0x4e2571a86bc81d5c;
struct CodeRange {
    std::uint32_t begin, end;
};
constexpr CodeRange kPromptRanges[] = {{0xd68950, 0xd68b3e}, {0x678d00, 0x678d82}, {0x67b770, 0x67b7b9}, {0x1565c30, 0x1565d04}, {0x15e14f0, 0x15e155b},
                                       {0x44e77e0, 0x44e7873}, {0x436e1f0, 0x436e240}, {0x4384a10, 0x4384a60}, {0x438a190, 0x438a1e0}, {0x438aa20, 0x438aa80}};

// Saves the argument registers, calls handler(rdi) with the stack aligned, restores them and runs the copied entry instructions.
constexpr std::uint8_t kTrampolineStart[] = {
    0x57,                                 // push rdi
    0x56,                                 // push rsi
    0x52,                                 // push rdx
    0x51,                                 // push rcx
    0x41, 0x50,                           // push r8
    0x41, 0x51,                           // push r9
    0x48, 0x83, 0xec, 0x08,               // sub rsp, 8
    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,   // mov rax, handler (offset 14)
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

bool XboxGlyphsActive(void* overseer) {
    return UsesXboxIcons(buttonIcons, PadControllerChannel::Current()) && *Int32(overseer, kInputDevice) == Gamepad;
}

// The controller type the game had before the override, restored once no Xbox prompts apply.
bool replaced = false;
std::int32_t gameControllerUsed = 0;

// Entered with the InputOverseer in rdi, on the game thread.
void APS5_VABI SelectPrompts(void* overseer) {
    const bool override = XboxGlyphsActive(overseer);
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

bool PromptCodeRecognised(std::uint32_t imageSize) {
    std::uint64_t hash = 0xcbf29ce484222325;
    for (const CodeRange& range : kPromptRanges) {
        if (range.end > imageSize) return false;
        for (std::uint32_t offset = range.begin; offset < range.end; ++offset) hash = (hash ^ game[offset]) * 0x100000001b3;
    }
    return hash == kPromptCodeHash;
}

template <typename T> T& Field(void* object, std::size_t offset) { return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset); }
template <typename Function> Function Game(std::uint32_t offset) { return reinterpret_cast<Function>(game + offset); }

bool Alive(void* object) { return object != nullptr && Field<void*>(object, kNativeObject) != nullptr; }

// A metadata slot holds the game's resolved MethodInfo, or a tagged index until its first use.
const void* ResolvedSlot(std::uint32_t offset) {
    const auto value = *reinterpret_cast<const std::uintptr_t*>(game + offset);
    return (value & 1) == 0 ? reinterpret_cast<const void*>(value) : nullptr;
}

using GetObject = void*(APS5_VABI*)(void* self, const void* method);
using GetCount = int(APS5_VABI*)(void* self, const void* method);
using GetObjectAt = void*(APS5_VABI*)(void* self, int index, const void* method);
using SetSprite = void(APS5_VABI*)(void* image, void* sprite, const void* method);
using GetSubsystem = void*(APS5_VABI*)(const void* method);

// The glyph Image next to a dialog label: the one sibling that has an Image.
void* GlyphImageOf(void* label, const void* imageMethod) {
    void* const transform = Alive(label) ? Game<GetObject>(kTransformOf)(label, nullptr) : nullptr;
    void* const parent = Alive(transform) ? Game<GetObject>(kParentOf)(transform, nullptr) : nullptr;
    if (!Alive(parent)) return nullptr;
    void* found = nullptr;
    const int children = Game<GetCount>(kChildCount)(parent, nullptr);
    for (int i = 0; i < children; ++i) {
        void* const child = Game<GetObjectAt>(kChildAt)(parent, i, nullptr);
        void* const image = Alive(child) ? Game<GetObject>(kGetComponent)(child, imageMethod) : nullptr;
        if (image == nullptr) continue;
        if (found != nullptr) return nullptr;
        found = image;
    }
    return Alive(found) ? found : nullptr;
}

void* LabelAt(void* prompt, std::size_t index) {
    void* const list = Field<void*>(prompt, kTextList);
    if (list == nullptr || index >= static_cast<std::size_t>(Field<std::int32_t>(list, kListSize))) return nullptr;
    void* const items = Field<void*>(list, kListItems);
    return items != nullptr ? Field<void*>(items, kArrayItems + index * sizeof(void*)) : nullptr;
}

// The glyphs are set as Image.overrideSprite, so clearing it brings back the dialog's own PlayStation sprite.
bool glyphsOverridden = false;

void ShowGlyph(void* prompt, std::size_t label, void* sprite, const void* imageMethod) {
    void* const image = GlyphImageOf(LabelAt(prompt, label), imageMethod);
    if (image == nullptr) return;
    if (sprite != nullptr) {
        Game<SetSprite>(kSetOverrideSprite)(image, sprite, nullptr);
        glyphsOverridden = true;
    } else if (Field<void*>(image, kOverrideSprite) != nullptr) {
        Game<SetSprite>(kSetOverrideSprite)(image, nullptr, nullptr);
    }
}

// Entered with the PromptTextSwitch in rdi, on the game thread, before the dialog's text is set.
void APS5_VABI ShowDialogGlyphs(void* prompt) {
    if (!glyphsOverridden && !UsesXboxIcons(buttonIcons, PadControllerChannel::Current())) return;
    const void* const overseerMethod = ResolvedSlot(kOverseerSlot);
    const void* const imageMethod = ResolvedSlot(kImageSlot);
    if (!Alive(prompt) || overseerMethod == nullptr || imageMethod == nullptr) return;
    void* const overseer = Game<GetSubsystem>(kGetOverseer)(overseerMethod);
    void* const icons = overseer != nullptr ? Field<void*>(overseer, kIconData) : nullptr;
    if (!Alive(icons)) return;
    const bool xbox = XboxGlyphsActive(overseer);
    void* const confirm = xbox ? Field<void*>(icons, kXboxConfirm) : nullptr;
    void* const back = xbox ? Field<void*>(icons, kXboxBack) : nullptr;
    if (xbox && (!Alive(confirm) || !Alive(back))) return;
    ShowGlyph(prompt, kConfirmLabel, confirm, imageMethod);
    ShowGlyph(prompt, kBackLabel, back, imageMethod);
}

// body holds the instructions copied from the method entry, relocated to run from the trampoline.
bool InstallEntryHook(std::uint32_t methodOffset, std::size_t stolen, const std::uint8_t* body, std::size_t bodySize, const void* handler) {
    const std::size_t size = sizeof(kTrampolineStart) + bodySize + sizeof(kJumpBack) + sizeof(void*);
    auto* trampoline = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (trampoline == nullptr) return false;
    std::uint8_t* const method = game + methodOffset;
    const void* const resume = method + stolen;
    std::memcpy(trampoline, kTrampolineStart, sizeof(kTrampolineStart));
    std::memcpy(trampoline + kTrampolineHandler, &handler, sizeof(handler));
    std::memcpy(trampoline + sizeof(kTrampolineStart), body, bodySize);
    std::memcpy(trampoline + sizeof(kTrampolineStart) + bodySize, kJumpBack, sizeof(kJumpBack));
    std::memcpy(trampoline + sizeof(kTrampolineStart) + bodySize + sizeof(kJumpBack), &resume, sizeof(resume));
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

bool InstallPromptHook() {
    return InstallEntryHook(kResolveIconsForCurrentInput, sizeof(kPrologue), kPrologue, sizeof(kPrologue), reinterpret_cast<const void*>(&SelectPrompts));
}

bool InstallDialogHook() {
    const std::uint8_t* const entry = game + kUpdatePromptDescriptions;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, entry + kPromptKept + 2, sizeof(displacement));
    const std::uint8_t* const flag = entry + kPromptStolen + displacement;
    const std::uint8_t loadFlag[] = {0x48, 0xb8};
    const std::uint8_t compareFlag[] = {0x80, 0x38, 0x00};
    std::uint8_t body[kPromptKept + sizeof(loadFlag) + sizeof(flag) + sizeof(compareFlag)];
    std::memcpy(body, entry, kPromptKept);
    std::memcpy(body + kPromptKept, loadFlag, sizeof(loadFlag));
    std::memcpy(body + kPromptKept + sizeof(loadFlag), &flag, sizeof(flag));
    std::memcpy(body + kPromptKept + sizeof(loadFlag) + sizeof(flag), compareFlag, sizeof(compareFlag));
    return InstallEntryHook(kUpdatePromptDescriptions, kPromptStolen, body, sizeof(body), reinterpret_cast<const void*>(&ShowDialogGlyphs));
}

bool DialogEntryMatches() {
    const std::uint8_t* const entry = game + kUpdatePromptDescriptions;
    return std::memcmp(entry, kPromptEntry, sizeof(kPromptEntry)) == 0 && entry[kPromptStolen - 1] == 0x00;
}

bool StartPromptHook(std::uint32_t imageSize) {
    if (!Recognised(imageSize) || std::memcmp(game + kResolveIconsForCurrentInput, kPrologue, sizeof(kPrologue)) != 0) {
        std::fprintf(stderr, "Xbox prompts: the prompt code was not recognised, prompts keep the PlayStation set\n");
        return false;
    }
    if (!InstallPromptHook()) {
        std::fprintf(stderr, "Xbox prompts: the prompt code could not be patched, prompts keep the PlayStation set\n");
        return false;
    }
    std::fprintf(stderr, "Xbox prompts: patch applied\n");
    return true;
}

bool StartDialogHook(std::uint32_t imageSize) {
    if (!PromptCodeRecognised(imageSize) || !DialogEntryMatches()) {
        std::fprintf(stderr, "Xbox prompts: the Settings dialog code was not recognised, dialogs keep the PlayStation glyphs\n");
        return false;
    }
    if (!InstallDialogHook()) {
        std::fprintf(stderr, "Xbox prompts: the Settings dialog code could not be patched, dialogs keep the PlayStation glyphs\n");
        return false;
    }
    std::fprintf(stderr, "Xbox prompts: Settings dialog glyphs applied\n");
    return true;
}

bool StartXboxPrompts() {
    buttonIcons = ParseButtonIcons(std::getenv("SOS_CONTROLLER_BUTTON_ICONS"));
    game = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(L"Il2cppUserAssemblies.prx.guest.prx"));
    const auto* headers = game != nullptr ? reinterpret_cast<const IMAGE_NT_HEADERS*>(game + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew) : nullptr;
    if (headers == nullptr) {
        std::fprintf(stderr, "Xbox prompts: the prompt code was not recognised, prompts keep the PlayStation set\n");
        return false;
    }
    const std::uint32_t imageSize = headers->OptionalHeader.SizeOfImage;
    const bool prompts = StartPromptHook(imageSize);
    return StartDialogHook(imageSize) && prompts;
}

[[maybe_unused]] const bool xboxPromptsStarted = StartXboxPrompts();

}
#endif
