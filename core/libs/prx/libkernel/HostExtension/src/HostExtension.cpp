#include "prx/libkernel/HostExtension/include/HostExtension.hpp"
#include <atomic>

namespace {

std::atomic<HostKeyHandler> registeredKeyHandler{nullptr};
std::atomic<HostFrameTick> registeredFrameTick{nullptr};
std::atomic<HostMenuDraw> registeredMenuDraw{nullptr};

}

extern "C" {

void HostExtensionRegister_nid_no_patch(HostKeyHandler keyHandler, HostFrameTick frameTick) {
    registeredKeyHandler.store(keyHandler);
    registeredFrameTick.store(frameTick);
}

void HostKeyPressed_nid_no_patch(int scancode) {
    if (const auto handler = registeredKeyHandler.load()) handler(scancode);
}

void HostFrameTick_nid_no_patch() {
    if (const auto tick = registeredFrameTick.load()) tick();
}

void HostExtensionRegisterMenu_nid_no_patch(HostMenuDraw draw) {
    registeredMenuDraw.store(draw);
}

void HostMenuDraw_nid_no_patch(const HostMenuWidgets& widgets) {
    if (const auto draw = registeredMenuDraw.load()) draw(widgets);
}

}
