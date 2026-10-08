#ifndef CORE_LIBS_PRX_LIBKERNEL_HOSTEXTENSION_INCLUDE_HOSTEXTENSION_HPP
#define CORE_LIBS_PRX_LIBKERNEL_HOSTEXTENSION_INCLUDE_HOSTEXTENSION_HPP

#include <cstddef>

// Lets one game-specific host library react to key presses in the game window and to each presented frame, without the window code
// knowing that library. Keys are SDL scancodes.
using HostKeyHandler = void (*)(int scancode);
using HostFrameTick = void (*)();

// Immediate-mode widgets the host provides to a menu. They are only valid during the draw call they are passed to.
struct HostMenuWidgets {
    void (*heading)(const char* text);
    void (*separator)();
    // failed: a patch behind the control could not be restored, so it is neither on nor off and shows FAILED.
    bool (*checkbox)(const char* label, bool* value, bool enabled, bool failed);
    bool (*combo)(const char* label, int* index, const char* const* items, int count, bool enabled, bool failed);
    bool (*statusButton)(const char* label, const char* status, bool enabled, bool failed);
};
using HostMenuDraw = void (*)(const HostMenuWidgets& widgets);

extern "C" {

void HostExtensionRegister_nid_no_patch(HostKeyHandler keyHandler, HostFrameTick frameTick);
// Called by the window for every new key press in the game window (auto-repeat excluded).
void HostKeyPressed_nid_no_patch(int scancode);
// Called by the window once per presented frame.
void HostFrameTick_nid_no_patch();
void HostExtensionRegisterMenu_nid_no_patch(HostMenuDraw draw);
// Draws the registered menu contents inside the host's menu window; does nothing when no menu is registered.
void HostMenuDraw_nid_no_patch(const HostMenuWidgets& widgets);

}

#endif
