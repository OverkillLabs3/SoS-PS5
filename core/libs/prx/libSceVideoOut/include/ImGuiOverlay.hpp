#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_IMGUIOVERLAY_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_IMGUIOVERLAY_HPP

#include <cstdint>
#include <vector>
#include "SDL.h"
#include "prx/libSceAgcDriver/Execution/include/PresentationOverlay.hpp"

class ImGuiOverlay final : public AgcDriver::PresentationOverlay {
public:
    ImGuiOverlay() = default;
    ImGuiOverlay(const ImGuiOverlay&) = delete;
    ImGuiOverlay& operator=(const ImGuiOverlay&) = delete;

    void Bind(SDL_Window* target);
    bool ToggleKeyPressed(const SDL_Event& event);
    void ProcessEvent(const SDL_Event& event);
    bool IsOpen() const { return open && state != State::Failed; }
    void SetFramesPerSecond(double fps) { framesPerSecond = fps; }
    void Shutdown() noexcept;

    bool Wanted(const AgcDriver::PresentationOverlayFrame& frame) override;
    void Record(const AgcDriver::PresentationOverlayFrame& frame) override;

private:
    enum class State { Unready, Ready, Failed };

    struct Target {
        VkImage image;
        VkImageView view;
        VkFramebuffer framebuffer;
    };

    struct DeviceFunctions {
        PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
        PFN_vkCreateRenderPass createRenderPass = nullptr;
        PFN_vkDestroyRenderPass destroyRenderPass = nullptr;
        PFN_vkCreateImageView createImageView = nullptr;
        PFN_vkDestroyImageView destroyImageView = nullptr;
        PFN_vkCreateFramebuffer createFramebuffer = nullptr;
        PFN_vkDestroyFramebuffer destroyFramebuffer = nullptr;
        PFN_vkCmdBeginRenderPass cmdBeginRenderPass = nullptr;
        PFN_vkCmdEndRenderPass cmdEndRenderPass = nullptr;
    };

    bool start(const AgcDriver::PresentationOverlayFrame& frame);
    void drawMenu();
    bool loadDeviceFunctions();
    bool createRenderPass(VkFormat format);
    const Target* targetFor(const AgcDriver::PresentationOverlayFrame& frame);
    void destroyTargets() noexcept;
    static PFN_vkVoidFunction loadFunction(const char* name, void* user);

    SDL_Window* window = nullptr;
    bool open = false;
    State state = State::Unready;
    bool contextCreated = false;
    bool sdlStarted = false;
    bool vulkanStarted = false;
    double framesPerSecond = 0.0;
    bool framesPerSecondShown = true;
    PFN_vkGetInstanceProcAddr instanceProc = nullptr;
    PFN_vkGetDeviceProcAddr deviceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    DeviceFunctions vk;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<Target> targets;
    std::uint64_t swapchain = 0;
};

#endif
