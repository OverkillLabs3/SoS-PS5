#include "prx/libkernel/HostExtension/include/HostExtension.hpp"
#include <atomic>

namespace {

std::atomic<HostFrameTick> registeredFrameTick{nullptr};
std::atomic<HostMenuDraw> registeredMenuDraw{nullptr};

}

extern "C" {

void HostExtensionRegisterFrameTick_nid_no_patch(HostFrameTick frameTick) {
    registeredFrameTick.store(frameTick);
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
