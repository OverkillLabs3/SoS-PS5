#include "prx/libSceVideoOut/include/ImGuiOverlay.hpp"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"
#include "prx/libkernel/HostExtension/include/HostExtension.hpp"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

bool ShowInGameFpsAtLaunch() {
    const char* value = std::getenv("SOS_SHOW_IN_GAME_FPS");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

constexpr float kFontPixels = 18.0f;
constexpr float kReferenceHeight = 1080.0f;
constexpr float kMenuScale = 1.5f;
constexpr float kMenuWidthEm = 21.75f;
constexpr float kValueWidthEm = 5.0f;
constexpr float kDimArrowAlpha = 0.35f;
constexpr float kMarginFontRatio = 0.5f;
constexpr float kSmallFontRatio = 0.75f;
constexpr const char* kMenuTitle = "In-Game - F10 Close";
constexpr std::uint32_t kDescriptorPoolSize = 16;
// The backend rotates its vertex buffers over this many frames, so it must exceed the presentations allowed in flight.
constexpr std::uint32_t kVertexBufferRing = 3;
constexpr ImVec4 kHeadingColor{1.00f, 0.82f, 0.30f, 1.00f};
constexpr ImVec4 kFailedColor{1.00f, 0.38f, 0.28f, 1.00f};
constexpr ImVec4 kPendingColor = kHeadingColor;
constexpr const char* kFailedText = "FAILED";
constexpr const char* kPendingText = "PENDING";

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

ImU32 Color(ImVec4 color, float alpha = 1.0f) {
    color.w *= alpha;
    return ImGui::GetColorU32(color);
}

ImU32 Color(ImGuiCol color, float alpha = 1.0f) { return Color(ImGui::GetStyleColorVec4(color), alpha); }

struct Row {
    ImVec2 min, max;
    ImVec2 boxMin, boxMax;
    bool pressed;
};

// The whole row is one item, so a press reaches exactly one control.
Row MenuRow(const char* label, bool interactive, ImGuiButtonFlags buttons) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() + style.ItemSpacing.y);
    Row row{};
    // The row owns the spacing below it, so neighbouring rows touch and no strip between them ignores a click.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));
    if (interactive) row.pressed = ImGui::InvisibleButton(label, size, buttons | ImGuiButtonFlags_EnableNav);
    else ImGui::Dummy(size);
    ImGui::PopStyleVar();
    row.min = ImGui::GetItemRectMin();
    row.max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (interactive && ImGui::IsItemHovered()) {
        draw->AddRectFilled(row.min, row.max, Color(ImGui::IsItemActive() ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered), style.FrameRounding);
    }
    const float middle = (row.min.y + row.max.y) * 0.5f;
    draw->AddText(ImVec2(row.min.x + style.FramePadding.x, middle - ImGui::GetFontSize() * 0.5f), Color(ImGuiCol_Text), label);
    const float halfBox = ImGui::GetFrameHeight() * 0.5f;
    row.boxMin = ImVec2(row.max.x - ImGui::GetFontSize() * kValueWidthEm, middle - halfBox);
    row.boxMax = ImVec2(row.max.x, middle + halfBox);
    return row;
}

void DrawValue(const Row& row, const char* text, bool on, const ImVec4& offColor) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(row.boxMin, row.boxMax, Color(on ? ImGuiCol_CheckMark : ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 at((row.boxMin.x + row.boxMax.x - size.x) * 0.5f, (row.boxMin.y + row.boxMax.y - size.y) * 0.5f);
    draw->AddText(at, on ? Color(ImGuiCol_WindowBg) : Color(offColor), text);
}

void DrawArrow(const Row& row, float left, const char* arrow, bool usable, bool on) {
    const float width = ImGui::GetFrameHeight();
    const ImVec2 size = ImGui::CalcTextSize(arrow);
    const ImVec2 at(left + (width - size.x) * 0.5f, (row.boxMin.y + row.boxMax.y - size.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(at, Color(on ? ImGuiCol_WindowBg : ImGuiCol_Text, usable ? 1.0f : kDimArrowAlpha), arrow);
}

void DrawSpecialState(const Row& row, HostControlState state) {
    const bool failed = state == HostControlState::Failed;
    DrawValue(row, failed ? kFailedText : kPendingText, false, failed ? kFailedColor : kPendingColor);
}

// A click on PENDING or FAILED reports a press without a new value. A disabled control is dimmed unless it shows one of those words.
bool Toggle(const char* label, bool* value, bool enabled, HostControlState state) {
    const bool normal = state == HostControlState::Normal;
    ImGui::BeginDisabled(normal && !enabled);
    const Row row = MenuRow(label, enabled, ImGuiButtonFlags_MouseButtonLeft);
    if (normal) DrawValue(row, *value ? "ON" : "OFF", *value, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    else DrawSpecialState(row, state);
    ImGui::EndDisabled();
    if (row.pressed && normal) *value = !*value;
    return row.pressed;
}

int ChoiceStep(const Row& row, bool focused, bool& wrap) {
    wrap = false;
    if (focused && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) return -1;
    if (focused && ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) return 1;
    if (!row.pressed) return 0;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const float x = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left].x;
        const float arrowWidth = ImGui::GetFrameHeight();
        if (x >= row.boxMin.x && x < row.boxMin.x + arrowWidth) return -1;
        if (x >= row.boxMax.x - arrowWidth) return 1;
    }
    wrap = true;
    return ImGui::IsMouseReleased(ImGuiMouseButton_Right) ? -1 : 1;
}

// The < and > ends of the value and the Left and Right keys step once and stop at the first and last value; anywhere else on the row, a
// left click, Enter or Space goes forward and a right click goes back, wrapping around.
bool Choice(const char* label, int* index, const char* const* items, int count, bool enabled, bool failed) {
    ImGui::BeginDisabled(!failed && !enabled);
    const ImGuiButtonFlags buttons = failed ? ImGuiButtonFlags_MouseButtonLeft : ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight;
    const Row row = MenuRow(label, enabled, buttons);
    const bool focused = enabled && ImGui::IsItemFocused();
    const int current = *index;
    if (failed) {
        DrawSpecialState(row, HostControlState::Failed);
    } else {
        const bool on = current != 0;
        DrawValue(row, items[current], on, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        DrawArrow(row, row.boxMin.x, "<", current > 0, on);
        DrawArrow(row, row.boxMax.x - ImGui::GetFrameHeight(), ">", current < count - 1, on);
    }
    ImGui::EndDisabled();
    if (failed) return row.pressed;
    bool wrap = false;
    const int step = ChoiceStep(row, focused, wrap);
    const int next = wrap ? (current + step + count) % count : std::clamp(current + step, 0, count - 1);
    if (next == current) return false;
    *index = next;
    return true;
}

bool Action(const char* label, const char* status, bool enabled, bool failed) {
    const float statusWidth = ImGui::GetFontSize() * kValueWidthEm;
    const float buttonWidth = ImGui::GetContentRegionAvail().x - statusWidth - ImGui::GetStyle().ItemSpacing.x;
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label, ImVec2(buttonWidth, 0.0f));
    ImGui::EndDisabled();
    const char* const text = failed ? kFailedText : status;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - (statusWidth + ImGui::CalcTextSize(text).x) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, failed ? kFailedColor : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
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

const HostMenuWidgets kWidgets{Heading, Separator, Toggle, Choice, Action};

void CheckVkResult(VkResult result) {
    if (result != VK_SUCCESS) std::fprintf(stderr, "In-game overlay: Vulkan call returned %d\n", static_cast<int>(result));
}

bool Succeeded(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return true;
    std::fprintf(stderr, "In-game overlay: %s failed with %d\n", what, static_cast<int>(result));
    return false;
}

}

ImGuiOverlay::ImGuiOverlay() : showFps(ShowInGameFpsAtLaunch()) {}

void ImGuiOverlay::Bind(SDL_Window* target) {
    window = target;
}

bool ImGuiOverlay::ToggleKeyPressed(const SDL_Event& event) {
    if (event.type != SDL_KEYDOWN || event.key.repeat != 0) return false;
    const SDL_Scancode key = event.key.keysym.scancode;
    if (key != SDL_SCANCODE_F9 && key != SDL_SCANCODE_F10) return false;
    if (window == nullptr || event.key.windowID != SDL_GetWindowID(window)) return false;
    if (key == SDL_SCANCODE_F9) {
        showFps = !showFps;
    } else {
        open = !open;
        if (!open && state == State::Ready) ReleaseImGuiInput();
    }
    return true;
}

void ImGuiOverlay::ProcessEvent(const SDL_Event& event) {
    if (state == State::Ready && open) ImGui_ImplSDL2_ProcessEvent(&event);
}

bool ImGuiOverlay::Wanted(const AgcDriver::PresentationOverlayFrame& frame) {
    if (!open && !showFps) return false;
    if (state == State::Unready) {
        const bool started = window != nullptr && start(frame);
        if (!started) {
            Shutdown();
            state = State::Failed;
            std::fprintf(stderr, "In-game overlay unavailable\n");
        } else {
            state = State::Ready;
        }
    }
    return state == State::Ready && frame.device == device;
}

void ImGuiOverlay::Record(const AgcDriver::PresentationOverlayFrame& frame) {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (open) drawMenu();
    if (showFps) DrawFramesPerSecond(framesPerSecond);
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
    }
    ImGui::End();
    if (!open) ReleaseImGuiInput();
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
        if (device != VK_NULL_HANDLE && vk.deviceWaitIdle != nullptr) vk.deviceWaitIdle(device);
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
    contextCreated = false;
    sdlStarted = false;
    vulkanStarted = false;
    device = VK_NULL_HANDLE;
    swapchain = 0;
    state = State::Unready;
}
