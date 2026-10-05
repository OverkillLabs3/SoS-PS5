#include "prx/libkernel/HostExtension/include/HostExtension.hpp"
#include <atomic>

namespace {

std::atomic<HostKeyHandler> registeredKeyHandler{nullptr};
std::atomic<HostTitleStatus> registeredTitleStatus{nullptr};

}

extern "C" {

void HostExtensionRegister_nid_no_patch(HostKeyHandler keyHandler, HostTitleStatus titleStatus) {
    registeredKeyHandler.store(keyHandler);
    registeredTitleStatus.store(titleStatus);
}

void HostKeyPressed_nid_no_patch(int scancode) {
    if (const auto handler = registeredKeyHandler.load()) handler(scancode);
}

void HostTitleStatus_nid_no_patch(char* text, std::size_t size) {
    if (size == 0) return;
    text[0] = '\0';
    if (const auto status = registeredTitleStatus.load()) status(text, size);
}

}
