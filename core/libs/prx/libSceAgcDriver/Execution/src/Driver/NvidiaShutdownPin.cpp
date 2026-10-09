#include "prx/libSceAgcDriver/Execution/include/Driver/NvidiaShutdownPin.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

namespace AgcDriver::DriverDetail {

void PinD3D12RuntimeForNvidiaShutdown(std::uint32_t vendorId) {
#ifdef _WIN32
    constexpr std::uint32_t NvidiaVendorId = 0x10de;
    constexpr const char* runtime[] = {"D3D12Core.dll", "d3d12.dll"};
    if (vendorId != NvidiaVendorId) return;
    for (const char* name : runtime) {
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, name, &module);
    }
#else
    (void)vendorId;
#endif
}

}
