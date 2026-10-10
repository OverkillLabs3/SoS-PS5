#ifndef CORE_LIBS_PRX_LIBKERNEL_HOSTEXTENSION_INCLUDE_HOSTEXTENSION_HPP
#define CORE_LIBS_PRX_LIBKERNEL_HOSTEXTENSION_INCLUDE_HOSTEXTENSION_HPP

#include <cstddef>

// Lets one game-specific host library react to each presented frame and fill the host's menu, without the window code knowing that library.
using HostFrameTick = void (*)();

// Pending: off, with its patch kept until the game no longer needs it. Failed: a patch behind the control could not be restored. Either
// is neither on nor off, so the control shows the word instead of its value.
enum class HostControlState { Normal, Pending, Failed };

// Immediate-mode widgets the host provides to a menu. They are only valid during the draw call they are passed to.
struct HostMenuWidgets {
    void (*heading)(const char* text);
    void (*separator)();
    bool (*checkbox)(const char* label, bool* value, bool enabled, HostControlState state);
    bool (*combo)(const char* label, int* index, const char* const* items, int count, bool enabled, bool failed);
    bool (*statusButton)(const char* label, const char* status, bool enabled, bool failed);
};
using HostMenuDraw = void (*)(const HostMenuWidgets& widgets);

extern "C" {

void HostExtensionRegisterFrameTick_nid_no_patch(HostFrameTick frameTick);
// Called by the window once per presented frame.
void HostFrameTick_nid_no_patch();
void HostExtensionRegisterMenu_nid_no_patch(HostMenuDraw draw);
// Draws the registered menu contents inside the host's menu window; does nothing when no menu is registered.
void HostMenuDraw_nid_no_patch(const HostMenuWidgets& widgets);

}

#endif
