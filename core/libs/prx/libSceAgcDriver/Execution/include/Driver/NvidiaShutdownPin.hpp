#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_NVIDIASHUTDOWNPIN_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_NVIDIASHUTDOWNPIN_HPP

#include <cstdint>

namespace AgcDriver::DriverDetail {

// NVIDIA's vkDestroyDevice can unload the D3D12 runtime and fault inside that nested unload; an already-loaded
// runtime is kept mapped until process exit instead. Takes the Windows loader lock, so call it without GpuMutex.
void PinD3D12RuntimeForNvidiaShutdown(std::uint32_t vendorId);

}

#endif
