// The settings screen's frame: Dear ImGui over the presented picture (ui.h). The pages are in settings.cpp.
//
// Drawn with dynamic rendering straight into the swapchain image after the game's frame was blitted there, so it
// costs nothing while closed and needs no extra image. Input does not go through ImGui's SDL backend (it calls SDL
// from the thread that draws, and SDL wants its video calls on the main thread on macOS): the main thread queues
// what happened and this file feeds ImGui on the presenter's thread.
//
// Size: everything is laid out in "design pixels" for a 1280x720 window and scaled by the window's size, so the
// same screen reads the same on a 13" laptop, a 4K monitor or (later) a phone; below an aspect of 1.2 the pages
// stack vertically (settings.cpp).
#include "ui/ui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include <nx/runtime.h>

#include "config.h"
#include "platform.h"
#include "ui/i18n.h"
#include "ui/settings.h"
#include "ui/ui_internal.h"

namespace nx::ui {

namespace {

// ---- input queue (main thread -> presenter) -------------------------------------------------------------------

struct Input {
    enum Kind { Key, Char, MousePos, MouseButton, Wheel, Focus } kind;
    int key = 0;  // ImGuiKey or mouse button
    bool down = false;
    float x = 0, y = 0;
    char text[32] = {};
};

std::mutex g_in_lock;
std::deque<Input> g_in;
std::atomic<bool> g_visible{std::getenv("NX_UI_OPEN") != nullptr};  // NX_UI_OPEN=1: open from the start (tests)
std::atomic<bool> g_first_hint{true};  // the "F1: settings" hint at startup
std::atomic<bool> g_capture{false};     // a key binding is being chosen (settings.cpp)
std::atomic<int> g_captured{0};

void push(const Input& in) {
    std::lock_guard lk(g_in_lock);
    if (g_in.size() < 512) g_in.push_back(in);
}

ImGuiKey imgui_key(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
        case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
        case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
        case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
        case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
        case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
        case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
        case SDL_SCANCODE_HOME: return ImGuiKey_Home;
        case SDL_SCANCODE_END: return ImGuiKey_End;
        case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
        case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
        case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
        case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
        case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
        case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
        case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
        case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
        case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
        case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
        case SDL_SCANCODE_A: return ImGuiKey_A;
        case SDL_SCANCODE_C: return ImGuiKey_C;
        case SDL_SCANCODE_V: return ImGuiKey_V;
        case SDL_SCANCODE_X: return ImGuiKey_X;
        default: return ImGuiKey_None;
    }
}

// ---- Vulkan ---------------------------------------------------------------------------------------------------

struct Vk {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdBeginRendering CmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering CmdEndRendering = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
} g_vk;

bool g_vk_ok = false;           // functions loaded
bool g_imgui_ok = false;        // ImGui's Vulkan backend initialized
VkFormat g_format = VK_FORMAT_UNDEFINED;
std::map<VkImage, VkImageView> g_views;

PFN_vkVoidFunction load(const char* name, void* user) {
    (void)user;
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(g_vk.gipa(g_vk.instance, "vkGetDeviceProcAddr"));
    if (PFN_vkVoidFunction f = gdpa ? gdpa(g_vk.device, name) : nullptr) return f;
    return g_vk.gipa(g_vk.instance, name);
}

template <typename F>
F dev_fn(const char* name) {
    return reinterpret_cast<F>(load(name, nullptr));
}

VkImageView view_for(VkImage image, VkFormat format) {
    auto it = g_views.find(image);
    if (it != g_views.end()) return it->second;
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = format;
    ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (g_vk.CreateImageView(g_vk.device, &ci, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    g_views[image] = v;
    return v;
}

void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
             VkAccessFlags dst, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from, b.newLayout = to;
    b.srcAccessMask = src, b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    g_vk.CmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ---- fonts ----------------------------------------------------------------------------------------------------

// A rounded sans font from the system for the menus, plus a CJK one when the menu language needs it. Searched in
// the usual places of each OS; ImGui's built-in font when none is found.
std::string find_font(bool cjk) {
    namespace fs = std::filesystem;
    std::vector<std::string> c;
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    if (cjk) {
        c = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc",
             "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Bold.ttc",
             "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/System/Library/Fonts/PingFang.ttc",
             "/System/Library/Fonts/Hiragino Sans GB.ttc", "C:\\Windows\\Fonts\\msyhbd.ttc",
             "C:\\Windows\\Fonts\\YuGothB.ttc", "C:\\Windows\\Fonts\\malgunbd.ttf"};
    } else {
        c = {plat::data_root() + "/fonts/ui.ttf",  // a font of the player's choice
             "/usr/share/fonts/truetype/nunito/Nunito-Bold.ttf", "/usr/share/fonts/truetype/quicksand/Quicksand-Bold.ttf",
             "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
             "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf", "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf",
             "/System/Library/Fonts/Helvetica.ttc", "C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf"};
    }
    std::error_code ec;
    for (const auto& p : c)
        if (fs::is_regular_file(fs::u8path(p), ec)) return p;
    return "";
}

float g_font_scale = 0;
std::string g_font_lang;
ImFont* g_font_body = nullptr;
ImFont* g_font_title = nullptr;

void build_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const std::string& lang = ui_language();
    bool cjk = lang == "ja" || lang == "ko" || lang == "zh-CN" || lang == "zh-TW";
    std::string latin = find_font(false), extra = cjk ? find_font(true) : "";
    static ImVector<ImWchar> ranges;  // Latin + Latin-1/Extended + Cyrillic + general punctuation + arrows/dashes
    ranges.clear();
    ImFontGlyphRangesBuilder rb;
    rb.AddRanges(io.Fonts->GetGlyphRangesDefault());
    rb.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    static const ImWchar more[] = {0x0100, 0x024F, 0x2000, 0x206F, 0x2190, 0x21FF, 0x2212, 0x2212, 0x25A0, 0x25FF, 0};
    rb.AddRanges(more);
    rb.BuildRanges(&ranges);
    const ImWchar* cjk_ranges = lang == "ja"      ? io.Fonts->GetGlyphRangesJapanese()
                                : lang == "ko"    ? io.Fonts->GetGlyphRangesKorean()
                                : lang == "zh-TW" ? io.Fonts->GetGlyphRangesChineseFull()
                                                  : io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
    auto add = [&](float px) -> ImFont* {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        ImFont* f = latin.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(latin.c_str(), px, &cfg, ranges.Data);
        if (!f) {
            ImFontConfig d;
            d.SizePixels = px;
            f = io.Fonts->AddFontDefault(&d);
        }
        if (!extra.empty()) {
            ImFontConfig m;
            m.MergeMode = true;
            m.OversampleH = 1;
            io.Fonts->AddFontFromFileTTF(extra.c_str(), px, &m, cjk_ranges);
        }
        return f;
    };
    g_font_body = add(std::round(20.0f * scale));
    g_font_title = add(std::round(30.0f * scale));
    g_font_scale = scale, g_font_lang = lang;
    if (g_imgui_ok) ImGui_ImplVulkan_CreateFontsTexture();
}

// ---- the Animal Crossing look ---------------------------------------------------------------------------------

void apply_style(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 28.0f;
    s.ChildRounding = 22.0f;
    s.FrameRounding = 14.0f;
    s.PopupRounding = 16.0f;
    s.GrabRounding = 14.0f;
    s.TabRounding = 14.0f;
    s.ScrollbarRounding = 12.0f;
    s.WindowPadding = ImVec2(26, 22);
    s.FramePadding = ImVec2(14, 8);
    s.ItemSpacing = ImVec2(12, 12);
    s.ItemInnerSpacing = ImVec2(10, 8);
    s.ScrollbarSize = 14.0f;
    s.GrabMinSize = 22.0f;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.FrameBorderSize = 0;
    s.PopupBorderSize = 0;
    s.ScaleAllSizes(scale);
    // Cream paper, brown ink, mint and yellow accents: the game's menus and NookPhone.
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = rgb(107, 79, 54);
    c[ImGuiCol_TextDisabled] = rgb(170, 150, 120);
    c[ImGuiCol_WindowBg] = rgb(255, 249, 232);
    c[ImGuiCol_ChildBg] = rgb(248, 239, 214);
    c[ImGuiCol_PopupBg] = rgb(255, 251, 240);
    c[ImGuiCol_Border] = rgb(230, 214, 178);
    c[ImGuiCol_FrameBg] = rgb(240, 228, 196);
    c[ImGuiCol_FrameBgHovered] = rgb(248, 226, 150);
    c[ImGuiCol_FrameBgActive] = rgb(255, 217, 102);
    c[ImGuiCol_TitleBg] = rgb(124, 194, 103);
    c[ImGuiCol_TitleBgActive] = rgb(124, 194, 103);
    c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(214, 196, 158);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(196, 176, 136);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(170, 150, 110);
    c[ImGuiCol_CheckMark] = rgb(255, 255, 255);
    c[ImGuiCol_SliderGrab] = rgb(78, 186, 160);
    c[ImGuiCol_SliderGrabActive] = rgb(56, 160, 136);
    c[ImGuiCol_Button] = rgb(118, 200, 170);
    c[ImGuiCol_ButtonHovered] = rgb(255, 217, 102);
    c[ImGuiCol_ButtonActive] = rgb(246, 196, 64);
    c[ImGuiCol_Header] = rgb(255, 232, 160);
    c[ImGuiCol_HeaderHovered] = rgb(255, 222, 128);
    c[ImGuiCol_HeaderActive] = rgb(255, 210, 90);
    c[ImGuiCol_Separator] = rgb(230, 214, 178);
    c[ImGuiCol_Tab] = rgb(240, 228, 196);
    c[ImGuiCol_TabHovered] = rgb(255, 222, 128);
    c[ImGuiCol_TabSelected] = rgb(124, 194, 103);
    c[ImGuiCol_NavCursor] = rgb(246, 170, 40);
    c[ImGuiCol_ModalWindowDimBg] = rgb(60, 40, 20, 0.45f);
}

bool init_imgui(VkFormat format) {
    if (!g_vk_ok) return false;
    if (g_imgui_ok && format == g_format) return true;
    if (g_imgui_ok) {
        g_vk.DeviceWaitIdle(g_vk.device);
        ImGui_ImplVulkan_Shutdown();
        g_imgui_ok = false;
    }
    if (!ImGui::GetCurrentContext()) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;  // our own config.ini keeps what matters
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        io.ConfigNavMoveSetMousePos = false;
        ui_set_language(config_get("ui.language", "auto"));
    }
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, load, nullptr)) {
        log("ui: Vulkan functions for ImGui missing: no settings screen");
        return false;
    }
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = g_vk.instance;
    ii.PhysicalDevice = g_vk.physical;
    ii.Device = g_vk.device;
    ii.QueueFamily = g_vk.family;
    ii.Queue = g_vk.queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = 3;
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    static VkFormat fmt;
    fmt = format;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &fmt;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        log("ui: ImGui Vulkan backend failed: no settings screen");
        return false;
    }
    g_imgui_ok = true;
    g_format = format;
    g_font_scale = 0;  // fonts rebuilt for the new backend
    log("ui: settings screen ready (format %d)", int(format));
    return true;
}

void feed_input(ImGuiIO& io) {
    std::deque<Input> in;
    {
        std::lock_guard lk(g_in_lock);
        in.swap(g_in);
    }
    for (const Input& e : in) switch (e.kind) {
            case Input::Key: io.AddKeyEvent(ImGuiKey(e.key), e.down); break;
            case Input::Char: io.AddInputCharactersUTF8(e.text); break;
            case Input::MousePos: io.AddMousePosEvent(e.x, e.y); break;
            case Input::MouseButton: io.AddMouseButtonEvent(e.key, e.down); break;
            case Input::Wheel: io.AddMouseWheelEvent(e.x, e.y); break;
            case Input::Focus: io.AddFocusEvent(e.down); break;
        }
}

}  // namespace

void ui_vulkan_ready(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                     uint32_t family, VkQueue queue) {
    g_vk.gipa = gipa, g_vk.instance = instance, g_vk.physical = physical, g_vk.device = device;
    g_vk.family = family, g_vk.queue = queue;
    g_vk.CreateImageView = dev_fn<PFN_vkCreateImageView>("vkCreateImageView");
    g_vk.DestroyImageView = dev_fn<PFN_vkDestroyImageView>("vkDestroyImageView");
    g_vk.CmdPipelineBarrier = dev_fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    g_vk.CmdBeginRendering = dev_fn<PFN_vkCmdBeginRendering>("vkCmdBeginRendering");
    if (!g_vk.CmdBeginRendering) g_vk.CmdBeginRendering = dev_fn<PFN_vkCmdBeginRendering>("vkCmdBeginRenderingKHR");
    g_vk.CmdEndRendering = dev_fn<PFN_vkCmdEndRendering>("vkCmdEndRendering");
    if (!g_vk.CmdEndRendering) g_vk.CmdEndRendering = dev_fn<PFN_vkCmdEndRendering>("vkCmdEndRenderingKHR");
    g_vk.DeviceWaitIdle = dev_fn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
    g_vk_ok = g_vk.CreateImageView && g_vk.CmdPipelineBarrier && g_vk.CmdBeginRendering && g_vk.CmdEndRendering;
    if (!g_vk_ok) log("ui: no dynamic rendering on this device: no settings screen");
}

void ui_record(VkCommandBuffer cb, VkImage image, VkFormat format, VkExtent2D extent) {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    const bool hint = g_first_hint && clock::now() - t0 < std::chrono::seconds(12);
    if (!hint) g_first_hint = false;
    if ((!g_visible && !hint) || !extent.width || !extent.height) return;
    if (!init_imgui(format)) return;
    ImGuiIO& io = ImGui::GetIO();
    // Design size 1280x720: the shorter side decides (a portrait phone is laid out on its width).
    float scale = std::clamp(std::min(extent.width / 1280.0f, extent.height / 720.0f), 0.6f, 4.0f);
    if (extent.height > extent.width) scale = std::clamp(extent.width / 720.0f, 0.6f, 4.0f);
    if (std::fabs(scale - g_font_scale) > 0.04f || g_font_lang != ui_language()) {
        build_fonts(scale);
        apply_style(scale);
    }
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    static auto last = clock::now();
    auto now = clock::now();
    io.DeltaTime = std::clamp(std::chrono::duration<float>(now - last).count(), 1.0f / 240.0f, 0.25f);
    last = now;
    feed_input(io);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    ImGui::PushFont(g_font_body);
    if (g_visible) {
        if (!settings_draw(scale, g_font_title)) g_visible = false;  // closed from the screen
    } else {
        settings_hint(scale);
    }
    ImGui::PopFont();
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount == 0) return;
    VkImageView view = view_for(image, format);
    if (!view) return;
    barrier(cb, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    g_vk.CmdBeginRendering(cb, &ri);
    ImGui_ImplVulkan_RenderDrawData(dd, cb);
    g_vk.CmdEndRendering(cb);
    barrier(cb, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

void ui_swapchain_changed() {
    if (!g_vk_ok) return;
    for (auto& [image, view] : g_views) g_vk.DestroyImageView(g_vk.device, view, nullptr);
    g_views.clear();
}

void ui_shutdown() {
    if (g_imgui_ok) {
        g_vk.DeviceWaitIdle(g_vk.device);
        ImGui_ImplVulkan_Shutdown();
        g_imgui_ok = false;
    }
    ui_swapchain_changed();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
}

// ---- main thread ----------------------------------------------------------------------------------------------

void ui_event(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            bool down = e.type == SDL_EVENT_KEY_DOWN;
            if (down && !e.key.repeat && g_visible && g_capture.exchange(false)) {
                g_captured = int(e.key.scancode);
                return;
            }
            if (down && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F1) {
                ui_toggle();
                return;
            }
            if (!g_visible) return;
            if (ImGuiKey k = imgui_key(e.key.scancode); k != ImGuiKey_None) push({Input::Key, k, down});
            Input mod{Input::Key, ImGuiMod_Ctrl, (e.key.mod & SDL_KMOD_CTRL) != 0};
            push(mod);
            Input sh{Input::Key, ImGuiMod_Shift, (e.key.mod & SDL_KMOD_SHIFT) != 0};
            push(sh);
            break;
        }
        case SDL_EVENT_TEXT_INPUT: {
            if (!g_visible) return;
            Input in{Input::Char};
            std::snprintf(in.text, sizeof in.text, "%s", e.text.text);
            push(in);
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (g_visible) push({Input::MousePos, 0, false, e.motion.x, e.motion.y});
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (!g_visible) return;
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            push({Input::MousePos, 0, false, e.button.x, e.button.y});
            push({Input::MouseButton, b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN});
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            if (g_visible) push({Input::Wheel, 0, false, e.wheel.x, e.wheel.y});
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            push({Input::Focus, 0, e.type == SDL_EVENT_WINDOW_FOCUS_GAINED});
            break;
        default: break;
    }
}

bool ui_pad(uint64_t buttons, int32_t lx, int32_t ly) {
    // NpadButton bits (platform.h): A 0, B 1, X 2, Y 3, L 6, R 7, ZL 8, ZR 9, Plus 10, Minus 11, D-pad 12..15.
    static uint64_t prev = 0;
    uint64_t pressed = buttons & ~prev, released = prev & ~buttons;
    prev = buttons;
    constexpr uint64_t kPlus = 1ull << 10, kMinus = 1ull << 11;
    if ((buttons & (kPlus | kMinus)) == (kPlus | kMinus) && (pressed & (kPlus | kMinus))) {
        ui_toggle();
        return true;
    }
    if (!g_visible) return false;
    struct Map {
        uint64_t bit;
        ImGuiKey key;
    };
    // The game's A confirms whatever controller it is (the platform already mapped the physical button to it).
    static const Map map[] = {
        {1ull << 0, ImGuiKey_GamepadFaceDown},  {1ull << 1, ImGuiKey_GamepadFaceRight},
        {1ull << 2, ImGuiKey_GamepadFaceUp},    {1ull << 3, ImGuiKey_GamepadFaceLeft},
        {1ull << 6, ImGuiKey_GamepadL1},        {1ull << 7, ImGuiKey_GamepadR1},
        {1ull << 12, ImGuiKey_GamepadDpadLeft}, {1ull << 13, ImGuiKey_GamepadDpadUp},
        {1ull << 14, ImGuiKey_GamepadDpadRight}, {1ull << 15, ImGuiKey_GamepadDpadDown},
    };
    for (const Map& m : map) {
        if (pressed & m.bit) push({Input::Key, m.key, true});
        if (released & m.bit) push({Input::Key, m.key, false});
    }
    // The left stick as a D-pad with hysteresis.
    static int sx = 0, sy = 0;
    auto stick = [&](int32_t v, int& state, ImGuiKey neg, ImGuiKey pos) {
        int now = v > 20000 ? 1 : v < -20000 ? -1 : (std::abs(v) < 12000 ? 0 : state);
        if (now == state) return;
        if (state < 0) push({Input::Key, neg, false});
        if (state > 0) push({Input::Key, pos, false});
        if (now < 0) push({Input::Key, neg, true});
        if (now > 0) push({Input::Key, pos, true});
        state = now;
    };
    stick(lx, sx, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight);
    stick(ly, sy, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadUp);
    return true;
}

bool ui_visible() { return g_visible; }

void ui_begin_key_capture() {
    g_captured = 0;
    g_capture = true;
}

int ui_take_captured_key() { return g_captured.exchange(0); }

void ui_toggle() {
    g_visible = !g_visible;
    g_first_hint = false;
    log("ui: settings %s", g_visible ? "opened" : "closed");
}

}  // namespace nx::ui
