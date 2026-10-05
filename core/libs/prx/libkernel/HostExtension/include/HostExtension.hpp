#ifndef CORE_LIBS_PRX_LIBKERNEL_HOSTEXTENSION_INCLUDE_HOSTEXTENSION_HPP
#define CORE_LIBS_PRX_LIBKERNEL_HOSTEXTENSION_INCLUDE_HOSTEXTENSION_HPP

#include <cstddef>

// Lets one game-specific host library react to key presses in the game window and add its runtime state to the window title,
// without the window code knowing that library. Keys are SDL scancodes.
using HostKeyHandler = void (*)(int scancode);
using HostTitleStatus = void (*)(char* text, std::size_t size);

extern "C" {

void HostExtensionRegister_nid_no_patch(HostKeyHandler keyHandler, HostTitleStatus titleStatus);
// Called by the window for every new key press in the game window (auto-repeat excluded).
void HostKeyPressed_nid_no_patch(int scancode);
// Text the window appends to its title; empty when nothing is registered.
void HostTitleStatus_nid_no_patch(char* text, std::size_t size);

}

#endif
