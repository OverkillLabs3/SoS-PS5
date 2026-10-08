#include "prx/libSceVideoOut/include/ImGuiOverlay.hpp"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"
#include "prx/libkernel/HostExtension/include/HostExtension.hpp"
#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace {

constexpr float kFontPixels = 18.0f;
constexpr float kReferenceHeight = 1080.0f;
constexpr float kMenuScale = 1.5f;
constexpr float kMenuWidthEm = 21.75f;
constexpr float kStatusWidthEm = 3.5f;
constexpr float kComboWidthEm = 5.0f;
constexpr float kMarginFontRatio = 0.5f;
constexpr float kSmallFontRatio = 0.75f;
constexpr const char* kMenuTitle = "In-Game - F10 Close";
constexpr std::uint32_t kDescriptorPoolSize = 16;
// The backend rotates its vertex buffers over this many frames, so it must exceed the presentations allowed in flight.
constexpr std::uint32_t kVertexBufferRing = 3;
constexpr ImVec4 kHeadingColor{1.00f, 0.82f, 0.30f, 1.00f};
constexpr ImVec4 kFailedColor{1.00f, 0.38f, 0.28f, 1.00f};
constexpr const char* kFailedText = "FAILED";

void ApplyStyle(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(10.0f, 6.0f);
    style.FramePadding = ImVec2(6.0f, 2.0f);
    style.ItemSpacing = ImVec2(6.0f, 3.0f);
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.96f, 0.93f, 0.84f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.50f, 0.40f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.06f, 0.04f, 0.90f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.07f, 0.04f, 0.97f);
    colors[ImGuiCol_Border] = ImVec4(0.62f, 0.48f, 0.14f, 0.70f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.13f, 0.07f, 0.95f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.27f, 0.21f, 0.09f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.36f, 0.28f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.14f, 0.11f, 0.05f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.30f, 0.23f, 0.08f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.80f, 0.25f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.26f, 0.20f, 0.08f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.42f, 0.33f, 0.12f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.58f, 0.45f, 0.14f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.30f, 0.24f, 0.09f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.42f, 0.33f, 0.12f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.55f, 0.43f, 0.14f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.62f, 0.48f, 0.14f, 0.45f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.85f, 0.65f, 0.20f, 0.70f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(1.00f, 0.80f, 0.25f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.04f, 0.03f, 0.02f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.35f, 0.27f, 0.10f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.50f, 0.39f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.65f, 0.50f, 0.18f, 1.00f);
    colors[ImGuiCol_NavCursor] = ImVec4(1.00f, 0.80f, 0.25f, 1.00f);
    style.ScaleAllSizes(scale);
}

// Keys and buttons held across a close must not stay pressed in ImGui, so the close is announced as a focus change.
void ReleaseImGuiInput() {
    ImGui::GetIO().AddFocusEvent(false);
    ImGui::GetIO().AddFocusEvent(true);
}

void Heading(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kHeadingColor);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void Separator() {
    ImGui::Separator();
}

// FAILED in red at the right end of the row: a button while a retry can be made, plain text once it cannot.
bool FailedMark(float width, bool retry) {
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - width);
    ImGui::PushStyleColor(ImGuiCol_Text, kFailedColor);
    bool clicked = false;
    if (retry) {
        clicked = ImGui::Button(kFailedText, ImVec2(width, 0.0f));
    } else {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - ImGui::CalcTextSize(kFailedText).x) * 0.5f);
        ImGui::TextUnformatted(kFailedText);
    }
    ImGui::PopStyleColor();
    return clicked;
}

bool Checkbox(const char* label, bool* value, bool enabled, bool failed) {
    if (!failed) {
        ImGui::BeginDisabled(!enabled);
        const bool changed = ImGui::Checkbox(label, value);
        ImGui::EndDisabled();
        return changed;
    }
    // No box, so the row cannot read as off; the label keeps the place it has next to one.
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    const bool retried = FailedMark(ImGui::CalcTextSize(kFailedText).x + 2.0f * ImGui::GetStyle().FramePadding.x, enabled);
    ImGui::PopID();
    return retried;
}

bool Combo(const char* label, int* index, const char* const* items, int count, bool enabled, bool failed) {
    const float width = ImGui::GetFontSize() * kComboWidthEm;
    ImGui::PushID(label);
    bool changed = false;
    if (failed) {
        ImGui::TextUnformatted(label);
        changed = FailedMark(width, enabled);
    } else {
        ImGui::BeginDisabled(!enabled);
        ImGui::TextUnformatted(label);
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - width);
        ImGui::SetNextItemWidth(width);
        changed = ImGui::Combo("##value", index, items, count);
        ImGui::EndDisabled();
    }
    ImGui::PopID();
    return changed;
}

bool StatusButton(const char* label, const char* status, bool enabled, bool failed) {
    const float statusWidth = ImGui::GetFontSize() * kStatusWidthEm;
    const float buttonWidth = ImGui::GetContentRegionAvail().x - statusWidth - ImGui::GetStyle().ItemSpacing.x;
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label, ImVec2(buttonWidth, 0.0f));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (failed) ImGui::PushStyleColor(ImGuiCol_Text, kFailedColor);
    ImGui::TextUnformatted(failed ? kFailedText : status);
    if (failed) ImGui::PopStyleColor();
    return clicked;
}

void DrawFramesPerSecond(double fps) {
    const float margin = ImGui::GetFontSize() * kMarginFontRatio;
    ImGui::SetNextWindowPos(ImVec2(margin, margin), ImGuiCond_Always);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kSmallFontRatio);
    if (ImGui::Begin("##FramesPerSecond", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::Text("FPS: %.2f", fps);
    }
    ImGui::End();
    ImGui::PopFont();
}

const HostMenuWidgets kWidgets{Heading, Separator, Checkbox, Combo, StatusButton};

void CheckVkResult(VkResult result) {
    if (result != VK_SUCCESS) std::fprintf(stderr, "[DEBUG_SAULO][ImGui] Vulkan call returned %d\n", static_cast<int>(result));
}

bool Succeeded(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return true;
    std::fprintf(stderr, "[DEBUG_SAULO][ImGui] %s failed with %d\n", what, static_cast<int>(result));
    return false;
}

}

void ImGuiOverlay::Bind(SDL_Window* target) {
    window = target;
}

bool ImGuiOverlay::ToggleKeyPressed(const SDL_Event& event) {
    if (event.type != SDL_KEYDOWN || event.key.repeat != 0 || event.key.keysym.scancode != SDL_SCANCODE_F10) return false;
    if (window == nullptr || event.key.windowID != SDL_GetWindowID(window)) return false;
    open = !open;
    if (!open && state == State::Ready) ReleaseImGuiInput();
    std::fprintf(stderr, "[DEBUG_SAULO][InGameMenu] %s\n", open ? "open" : "closed");
    return true;
}

void ImGuiOverlay::ProcessEvent(const SDL_Event& event) {
    if (state == State::Ready && open) ImGui_ImplSDL2_ProcessEvent(&event);
}

bool ImGuiOverlay::Wanted(const AgcDriver::PresentationOverlayFrame& frame) {
    if (state == State::Unready) {
        const bool started = window != nullptr && start(frame);
        if (!started) {
            Shutdown();
            state = State::Failed;
        } else {
            state = State::Ready;
        }
        std::fprintf(stderr, "[DEBUG_SAULO][ImGui] %s\n", started ? "overlay ready" : "overlay unavailable");
    }
    return state == State::Ready && frame.device == device && (open || framesPerSecondShown);
}

void ImGuiOverlay::Record(const AgcDriver::PresentationOverlayFrame& frame) {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (open) drawMenu();
    if (framesPerSecondShown) DrawFramesPerSecond(framesPerSecond);
    ImGui::Render();
    const Target* target = targetFor(frame);
    if (target == nullptr) return;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = renderPass;
    begin.framebuffer = target->framebuffer;
    begin.renderArea = {{0, 0}, frame.extent};
    vk.cmdBeginRenderPass(frame.commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commands);
    vk.cmdEndRenderPass(frame.commands);
}

void ImGuiOverlay::drawMenu() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float margin = ImGui::GetFontSize() * kMarginFontRatio;
    const float width = ImGui::GetFontSize() * kMenuWidthEm;
    ImGui::SetNextWindowPos(ImVec2(display.x - margin, margin), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, std::max(display.y - 2.0f * margin, 0.0f)));
    if (ImGui::Begin(kMenuTitle, &open,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        HostMenuDraw_nid_no_patch(kWidgets);
        Separator();
        Heading("OVERLAY");
        if (Checkbox("Show FPS", &framesPerSecondShown, true, false)) {
            std::fprintf(stderr, "[DEBUG_SAULO][InGameMenu] Show FPS %s\n", framesPerSecondShown ? "on" : "off");
        }
    }
    ImGui::End();
    if (!open) {
        std::fprintf(stderr, "[DEBUG_SAULO][InGameMenu] closed by X\n");
        ReleaseImGuiInput();
    }
}

bool ImGuiOverlay::start(const AgcDriver::PresentationOverlayFrame& frame) {
    instanceProc = frame.instanceProc;
    deviceProc = frame.deviceProc;
    instance = frame.instance;
    device = frame.device;
    if (!loadDeviceFunctions()) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    contextCreated = true;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
    const float scale = kMenuScale * std::clamp(static_cast<float>(frame.extent.height) / kReferenceHeight, 1.0f, 2.0f);
    ImFontConfig fontConfig;
    fontConfig.SizePixels = kFontPixels * scale;
    io.Fonts->AddFontDefault(&fontConfig);
    ApplyStyle(scale);

    if (!ImGui_ImplSDL2_InitForVulkan(window)) return false;
    sdlStarted = true;
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, loadFunction, this)) return false;
    if (!createRenderPass(frame.format)) return false;

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_0;
    info.Instance = instance;
    info.PhysicalDevice = frame.physical;
    info.Device = device;
    info.QueueFamily = frame.queueFamily;
    info.Queue = frame.queue;
    info.DescriptorPoolSize = kDescriptorPoolSize;
    info.MinImageCount = 2;
    info.ImageCount = kVertexBufferRing;
    info.PipelineInfoMain.RenderPass = renderPass;
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.MinAllocationSize = 1024 * 1024;
    info.CheckVkResultFn = CheckVkResult;
    if (!ImGui_ImplVulkan_Init(&info)) return false;
    vulkanStarted = true;
    return true;
}

bool ImGuiOverlay::loadDeviceFunctions() {
    const auto load = [this](const char* name) { return deviceProc(device, name); };
    vk.deviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(load("vkDeviceWaitIdle"));
    vk.createRenderPass = reinterpret_cast<PFN_vkCreateRenderPass>(load("vkCreateRenderPass"));
    vk.destroyRenderPass = reinterpret_cast<PFN_vkDestroyRenderPass>(load("vkDestroyRenderPass"));
    vk.createImageView = reinterpret_cast<PFN_vkCreateImageView>(load("vkCreateImageView"));
    vk.destroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(load("vkDestroyImageView"));
    vk.createFramebuffer = reinterpret_cast<PFN_vkCreateFramebuffer>(load("vkCreateFramebuffer"));
    vk.destroyFramebuffer = reinterpret_cast<PFN_vkDestroyFramebuffer>(load("vkDestroyFramebuffer"));
    vk.cmdBeginRenderPass = reinterpret_cast<PFN_vkCmdBeginRenderPass>(load("vkCmdBeginRenderPass"));
    vk.cmdEndRenderPass = reinterpret_cast<PFN_vkCmdEndRenderPass>(load("vkCmdEndRenderPass"));
    return vk.deviceWaitIdle != nullptr && vk.createRenderPass != nullptr && vk.destroyRenderPass != nullptr && vk.createImageView != nullptr &&
           vk.destroyImageView != nullptr && vk.createFramebuffer != nullptr && vk.destroyFramebuffer != nullptr && vk.cmdBeginRenderPass != nullptr &&
           vk.cmdEndRenderPass != nullptr;
}

bool ImGuiOverlay::createRenderPass(VkFormat format) {
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    return Succeeded(vk.createRenderPass(device, &info, nullptr, &renderPass), "vkCreateRenderPass");
}

const ImGuiOverlay::Target* ImGuiOverlay::targetFor(const AgcDriver::PresentationOverlayFrame& frame) {
    if (frame.swapchain != swapchain) {
        destroyTargets();
        swapchain = frame.swapchain;
    }
    for (const Target& target : targets) {
        if (target.image == frame.image) return &target;
    }
    Target target{frame.image, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = frame.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = frame.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!Succeeded(vk.createImageView(device, &view, nullptr, &target.view), "vkCreateImageView")) return nullptr;
    VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = renderPass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &target.view;
    framebuffer.width = frame.extent.width;
    framebuffer.height = frame.extent.height;
    framebuffer.layers = 1;
    if (!Succeeded(vk.createFramebuffer(device, &framebuffer, nullptr, &target.framebuffer), "vkCreateFramebuffer")) {
        vk.destroyImageView(device, target.view, nullptr);
        return nullptr;
    }
    targets.push_back(target);
    return &targets.back();
}

void ImGuiOverlay::destroyTargets() noexcept {
    for (const Target& target : targets) {
        vk.destroyFramebuffer(device, target.framebuffer, nullptr);
        vk.destroyImageView(device, target.view, nullptr);
    }
    targets.clear();
}

PFN_vkVoidFunction ImGuiOverlay::loadFunction(const char* name, void* user) {
    const auto* overlay = static_cast<const ImGuiOverlay*>(user);
    PFN_vkVoidFunction function = overlay->deviceProc != nullptr && overlay->device != VK_NULL_HANDLE ? overlay->deviceProc(overlay->device, name) : nullptr;
    if (function == nullptr && overlay->instanceProc != nullptr) function = overlay->instanceProc(overlay->instance, name);
    return function;
}

void ImGuiOverlay::Shutdown() noexcept {
    if (device != VK_NULL_HANDLE && vk.deviceWaitIdle != nullptr) vk.deviceWaitIdle(device);
    if (vulkanStarted) ImGui_ImplVulkan_Shutdown();
    destroyTargets();
    if (renderPass != VK_NULL_HANDLE && vk.destroyRenderPass != nullptr) vk.destroyRenderPass(device, renderPass, nullptr);
    renderPass = VK_NULL_HANDLE;
    if (sdlStarted) ImGui_ImplSDL2_Shutdown();
    if (contextCreated) ImGui::DestroyContext();
    if (contextCreated || vulkanStarted || sdlStarted) std::fprintf(stderr, "[DEBUG_SAULO][ImGui] shut down\n");
    contextCreated = false;
    sdlStarted = false;
    vulkanStarted = false;
    device = VK_NULL_HANDLE;
    swapchain = 0;
    state = State::Unready;
}
