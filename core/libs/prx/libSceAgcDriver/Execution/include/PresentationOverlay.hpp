#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PRESENTATIONOVERLAY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PRESENTATIONOVERLAY_HPP

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdint>

namespace AgcDriver {

struct PresentationOverlayFrame {
    VkCommandBuffer commands;
    VkImage image;
    VkExtent2D extent;
    VkFormat format;
    std::uint64_t swapchain;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    std::uint32_t queueFamily;
    PFN_vkGetInstanceProcAddr instanceProc;
    PFN_vkGetDeviceProcAddr deviceProc;
};

// Wanted() is asked before the presented image leaves TRANSFER_DST_OPTIMAL. When it returns true, Record() runs with the image in
// COLOR_ATTACHMENT_OPTIMAL and must leave it there. Record() only records into frame.commands; anything it submits completes before it returns.
class PresentationOverlay {
public:
    virtual ~PresentationOverlay() = default;
    virtual bool Wanted(const PresentationOverlayFrame& frame) = 0;
    virtual void Record(const PresentationOverlayFrame& frame) = 0;
};

}

#endif
