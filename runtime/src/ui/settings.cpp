// The settings screen's pages: game, video, audio, controls, saves, about. Every choice is written to config.ini at
// once (config.h); the ones the runtime can change live are applied here, the others say they need a restart.
//
// Layout: a cream card over the dimmed game, the pages as round coloured buttons down its left side (along its top
// when the screen is narrow or portrait), each setting a row with its name and a short explanation on the left and
// the control on the right (under them when narrow). Sizes are in design pixels times `scale` (ui.cpp).
#include "ui/settings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

#include <nx/runtime.h>

#include "config.h"
#include "platform.h"
#include "saves.h"
#include "ui/i18n.h"
#include "ui/ui_internal.h"

namespace nx::ui {

namespace {

namespace fs = std::filesystem;

ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

struct Page {
    const char* key;
    ImU32 color;
    int icon;
};
const Page kPages[] = {
    {"tab.game", rgb(124, 194, 103), 0},  {"tab.video", rgb(79, 179, 217), 1},   {"tab.audio", rgb(245, 166, 35), 2},
    {"tab.controls", rgb(155, 125, 212), 3}, {"tab.saves", rgb(232, 115, 122), 4}, {"tab.about", rgb(176, 140, 100), 5},
};
constexpr int kPageCount = int(sizeof(kPages) / sizeof(kPages[0]));

int g_page = 0;
float g_scale = 1;
std::string g_toast;
double g_toast_until = 0;
bool g_restart_note = false;

void toast(const std::string& text) {
    g_toast = text;
    g_toast_until = ImGui::GetTime() + 4.0;
}

// Simple vector icons inside the page buttons, drawn at centre c with radius r.
void draw_icon(ImDrawList* dl, int icon, ImVec2 c, float r, ImU32 col) {
    float t = std::max(1.5f, r * 0.14f);
    switch (icon) {
        case 0: {  // leaf
            dl->AddBezierQuadratic(ImVec2(c.x - r * 0.6f, c.y + r * 0.6f), ImVec2(c.x - r * 0.7f, c.y - r * 0.8f),
                                   ImVec2(c.x + r * 0.6f, c.y - r * 0.6f), col, t);
            dl->AddBezierQuadratic(ImVec2(c.x - r * 0.6f, c.y + r * 0.6f), ImVec2(c.x + r * 0.8f, c.y + r * 0.7f),
                                   ImVec2(c.x + r * 0.6f, c.y - r * 0.6f), col, t);
            dl->AddLine(ImVec2(c.x - r * 0.6f, c.y + r * 0.6f), ImVec2(c.x + r * 0.2f, c.y - r * 0.2f), col, t);
            break;
        }
        case 1:  // screen
            dl->AddRect(ImVec2(c.x - r * 0.7f, c.y - r * 0.5f), ImVec2(c.x + r * 0.7f, c.y + r * 0.35f), col, r * 0.12f, 0, t);
            dl->AddLine(ImVec2(c.x - r * 0.3f, c.y + r * 0.65f), ImVec2(c.x + r * 0.3f, c.y + r * 0.65f), col, t);
            break;
        case 2:  // note
            dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y + r * 0.45f), r * 0.25f, col);
            dl->AddLine(ImVec2(c.x - r * 0.07f, c.y + r * 0.45f), ImVec2(c.x - r * 0.07f, c.y - r * 0.6f), col, t);
            dl->AddLine(ImVec2(c.x - r * 0.07f, c.y - r * 0.6f), ImVec2(c.x + r * 0.5f, c.y - r * 0.35f), col, t);
            break;
        case 3:  // controller
            dl->AddRect(ImVec2(c.x - r * 0.75f, c.y - r * 0.35f), ImVec2(c.x + r * 0.75f, c.y + r * 0.4f), col, r * 0.35f, 0, t);
            dl->AddLine(ImVec2(c.x - r * 0.45f, c.y), ImVec2(c.x - r * 0.15f, c.y), col, t);
            dl->AddLine(ImVec2(c.x - r * 0.3f, c.y - r * 0.15f), ImVec2(c.x - r * 0.3f, c.y + r * 0.15f), col, t);
            dl->AddCircleFilled(ImVec2(c.x + r * 0.35f, c.y), r * 0.1f, col);
            break;
        case 4:  // house
            dl->AddTriangle(ImVec2(c.x, c.y - r * 0.7f), ImVec2(c.x - r * 0.75f, c.y - r * 0.05f),
                            ImVec2(c.x + r * 0.75f, c.y - r * 0.05f), col, t);
            dl->AddRect(ImVec2(c.x - r * 0.5f, c.y - r * 0.05f), ImVec2(c.x + r * 0.5f, c.y + r * 0.6f), col, 0, 0, t);
            break;
        default:  // info
            dl->AddCircle(c, r * 0.7f, col, 0, t);
            dl->AddLine(ImVec2(c.x, c.y - r * 0.05f), ImVec2(c.x, c.y + r * 0.4f), col, t);
            dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.32f), t * 0.8f, col);
            break;
    }
}

// ---- settings helpers -----------------------------------------------------------------------------------------

void apply_live(const std::string& key, const std::string& value) {
    if (key == "video.fullscreen") plat::set_fullscreen(value == "1");
    else if (key == "audio.volume") plat::set_master_volume(std::clamp(std::atoi(value.c_str()), 0, 100) / 100.0f);
    else if (key == "ui.language") ui_set_language(value);
    if (config_needs_restart(key)) g_restart_note = true;
}

void store(const std::string& key, const std::string& value) {
    if (config_get(key) == value) return;
    config_set(key, value);
    config_save();
    apply_live(key, value);
}

// A setting row: name and explanation on the left, the control on the right (below when narrow).
void row(const char* label, const char* desc, const std::function<void(float width)>& control) {
    float avail = ImGui::GetContentRegionAvail().x;
    bool narrow = avail < 560.0f * g_scale;
    float ctrl_w = narrow ? avail : std::min(360.0f * g_scale, avail * 0.45f);
    ImGui::PushID(label);
    ImGui::BeginGroup();
    float text_w = narrow ? avail : avail - ctrl_w - 16.0f * g_scale;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_w);
    ImGui::TextUnformatted(tr(label));
    if (desc && *desc) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::SetWindowFontScale(0.82f);
        ImGui::TextWrapped("%s", tr(desc));
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    if (!narrow) {
        ImGui::SameLine(avail - ctrl_w + ImGui::GetStyle().WindowPadding.x * 0.0f);
    }
    ImGui::SetNextItemWidth(ctrl_w);
    control(ctrl_w);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, 4.0f * g_scale));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImGui::GetStyle().Colors[ImGuiCol_Border]);
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 2.0f * g_scale));
}

using Choices = std::vector<std::pair<std::string, std::string>>;  // value, shown text (already translated)

void combo(const std::string& key, const std::string& fallback, const Choices& choices, float width) {
    std::string cur = config_get(key, fallback);
    std::string shown = cur;
    for (auto& [v, t] : choices)
        if (v == cur) shown = t;
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##c", shown.c_str())) {
        for (auto& [v, t] : choices) {
            bool sel = v == cur;
            if (ImGui::Selectable(t.c_str(), sel)) store(key, v);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

// An on/off pill like the game's.
void toggle(const std::string& key, bool fallback) {
    bool on = config_get_bool(key, fallback);
    float h = ImGui::GetFrameHeight(), w = h * 1.9f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##t", ImVec2(w, h))) {
        on = !on;
        store(key, on ? "1" : "0");
    }
    bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = on ? rgb(118, 200, 170) : rgb(214, 200, 168);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, h * 0.5f);
    if (hov) dl->AddRect(p, ImVec2(p.x + w, p.y + h), rgb(246, 170, 40), h * 0.5f, 0, 2.5f * g_scale);
    float r = h * 0.5f - 3.0f * g_scale;
    ImVec2 knob(on ? p.x + w - h * 0.5f : p.x + h * 0.5f, p.y + h * 0.5f);
    dl->AddCircleFilled(knob, r, rgb(255, 255, 255));
    ImGui::SameLine();
    ImGui::TextUnformatted(tr(on ? "common.on" : "common.off"));
}

void slider(const std::string& key, int lo, int hi, int fallback, const char* fmt, float width) {
    int v = config_get_int(key, fallback);
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderInt("##s", &v, lo, hi, fmt)) {
        config_set(key, std::to_string(v));
        apply_live(key, std::to_string(v));
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) config_save();
}

bool button(const char* text, float width = 0) {
    return ImGui::Button(text, ImVec2(width, 0));
}

// ---- pages ----------------------------------------------------------------------------------------------------

bool ptbr_installed() {
    std::error_code ec;
    return fs::is_directory(fs::u8path(plat::data_root() + "/mods/ptbr/romfs"), ec);
}

void page_game() {
    Choices langs = {{"auto", tr("game.language.system")},
                     {"en-US", "English (US)"},     {"en-GB", "English (UK)"},  {"es", "Español (España)"},
                     {"es-419", "Español (Latinoamérica)"}, {"fr", "Français"},   {"fr-CA", "Français (Canada)"},
                     {"de", "Deutsch"},             {"it", "Italiano"},         {"nl", "Nederlands"},
                     {"ru", "Русский"},             {"ja", "日本語"},            {"ko", "한국어"},
                     {"zh-CN", "简体中文"},          {"zh-TW", "繁體中文"},       {"pt-BR", tr("game.language.ptbr")}};
    row("game.language", "game.language.desc", [&](float w) { combo("game.language", "auto", langs, w); });
    if (config_get("game.language") == "pt-BR") {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(0.35f, 0.55f, 0.35f, 1), "%s",
                           tr(ptbr_installed() ? "game.language.ptbr_note" : "game.language.ptbr_missing"));
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 6 * g_scale));
    }
    Choices ui_langs = {{"auto", tr("common.auto")}};
    for (const UiLanguage& l : ui_languages()) ui_langs.push_back({l.code, l.native_name});
    row("ui.language", "ui.language.desc", [&](float w) { combo("ui.language", "auto", ui_langs, w); });
}

bool lsfg_available() {
    // lsfg-vk installs an implicit Vulkan layer manifest.
    std::error_code ec;
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    for (const std::string& d : {std::string("/usr/share/vulkan/implicit_layer.d"),
                                 std::string("/etc/vulkan/implicit_layer.d"),
                                 home + "/.local/share/vulkan/implicit_layer.d"}) {
        if (!fs::is_directory(d, ec)) continue;
        for (auto& e : fs::directory_iterator(d, ec))
            if (e.path().filename().string().find("lsfg") != std::string::npos) return true;
    }
    return false;
}

void page_video() {
    row("video.display", nullptr, [&](float w) {
        combo("video.fullscreen", "0", {{"0", tr("video.display.windowed")}, {"1", tr("video.display.fullscreen")}}, w);
    });
    row("video.window_size", nullptr, [&](float w) {
        combo("video.window", "auto",
              {{"auto", tr("common.auto")}, {"1280x720", "1280 × 720"}, {"1600x900", "1600 × 900"},
               {"1920x1080", "1920 × 1080"}, {"2560x1440", "2560 × 1440"}, {"3840x2160", "3840 × 2160"}},
              w);
    });
    row("video.render", nullptr, [&](float w) {
        combo("video.mode", "docked", {{"docked", tr("video.render.docked")}, {"handheld", tr("video.render.handheld")}}, w);
    });
    row("video.upscaler", nullptr, [&](float w) {
        combo("video.upscaler", "linear", {{"linear", tr("video.upscaler.linear")}, {"fsr", tr("video.upscaler.fsr")}}, w);
    });
    if (config_get("video.upscaler") == "fsr")
        row("video.sharpness", nullptr, [&](float w) { slider("video.fsr_sharpness_pct", 0, 100, 80, "%d%%", w); });
    row("video.aspect", "video.aspect.desc", [&](float w) {
        combo("video.aspect", "16:9",
              {{"16:9", tr("video.aspect.original")}, {"auto", tr("common.auto")}, {"16:10", "16:10"}, {"21:9", "21:9"},
               {"32:9", "32:9"}, {"20:9", trf("video.aspect.phone", "20:9")}, {"19.5:9", trf("video.aspect.phone", "19.5:9")},
               {"4:3", "4:3"}},
              w);
    });
    row("video.framerate", "video.framerate.desc", [&](float w) {
        combo("video.frame_rate", "30", {{"30", tr("video.framerate.30")}, {"60", tr("video.framerate.60")}}, w);
    });
    bool lsfg = lsfg_available();
    row("video.framegen", lsfg ? "video.framegen.desc" : "video.framegen.missing", [&](float w) {
        ImGui::BeginDisabled(!lsfg);
        combo("video.frame_gen", "off", {{"off", tr("video.framegen.off")}, {"lsfg", tr("video.framegen.lsfg")}}, w);
        ImGui::EndDisabled();
    });
    row("video.gpu", nullptr, [&](float w) {
        combo("video.gpu", "auto",
              {{"auto", tr("common.auto")}, {"discrete", tr("video.gpu.discrete")}, {"integrated", tr("video.gpu.integrated")}}, w);
    });
    row("video.shaders", "video.shaders.desc", [&](float) { toggle("video.shader_prepare", true); });
}

void page_audio() {
    row("audio.volume", nullptr, [&](float w) { slider("audio.volume", 0, 100, 100, "%d%%", w); });
    row("audio.reverb", "audio.reverb.desc", [&](float) { toggle("audio.reverb", true); });
    row("audio.background", nullptr, [&](float) { toggle("audio.mute_background", false); });
}

// Keyboard bindings: the action, its config key and its default key (SDL scancode name).
struct Binding {
    const char* action;  // i18n key
    const char* key;     // input.key.<key>
    const char* def;
};
const Binding kBindings[] = {
    {"action.move", "move_up", "W"},      {"action.move", "move_down", "S"},   {"action.move", "move_left", "A"},
    {"action.move", "move_right", "D"},   {"action.camera", "cam_up", "I"},    {"action.camera", "cam_down", "K"},
    {"action.camera", "cam_left", "J"},   {"action.camera", "cam_right", "L"}, {"action.a", "a", "Space"},
    {"action.b", "b", "Left Shift"},      {"action.x", "x", "X"},              {"action.y", "y", "F"},
    {"action.l", "l", "Q"},               {"action.r", "r", "E"},              {"action.zl", "zl", "Z"},
    {"action.zr", "zr", "C"},             {"action.plus", "plus", "Return"},   {"action.minus", "minus", "Escape"},
    {"action.dpad", "dpad_up", "Up"},     {"action.dpad", "dpad_down", "Down"}, {"action.dpad", "dpad_left", "Left"},
    {"action.dpad", "dpad_right", "Right"},
};
int g_capturing = -1;

const char* arrow_for(const char* key) {
    if (std::strstr(key, "_up")) return " ↑";
    if (std::strstr(key, "_down")) return " ↓";
    if (std::strstr(key, "_left")) return " ←";
    if (std::strstr(key, "_right")) return " →";
    return "";
}

void page_controls() {
    std::string name = plat::controller_name();
    row("controls.device", nullptr, [&](float) {
        ImGui::TextWrapped("%s", name.empty() ? tr("controls.none") : name.c_str());
    });
    row("controls.layout", nullptr, [&](float w) {
        combo("input.layout", "nintendo",
              {{"nintendo", tr("controls.layout.nintendo")}, {"xbox", tr("controls.layout.xbox")}}, w);
    });
    row("controls.glyphs", nullptr, [&](float w) {
        combo("controls.glyphs", "auto",
              {{"auto", tr("controls.glyphs.auto")}, {"switch", tr("controls.glyphs.switch")},
               {"xbox", tr("controls.glyphs.xbox")}, {"playstation", tr("controls.glyphs.playstation")}},
              w);
    });
    row("controls.rumble", nullptr, [&](float) { toggle("input.rumble", true); });
    row("controls.deadzone", nullptr, [&](float w) { slider("input.deadzone", 5, 50, 24, "%d%%", w); });
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::PushFont(nullptr);
    ImGui::TextUnformatted(tr("controls.keyboard"));
    ImGui::PopFont();
    // A capture in progress: the next key pressed (ui.cpp) becomes the binding; Escape cancels.
    if (g_capturing >= 0) {
        int sc = ui_take_captured_key();
        if (sc == SDL_SCANCODE_ESCAPE) {
            g_capturing = -1;
        } else if (sc > 0) {
            store(std::string("input.key.") + kBindings[g_capturing].key, SDL_GetScancodeName(SDL_Scancode(sc)));
            g_capturing = -1;
        }
    }
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (int k = 0; k < int(sizeof(kBindings) / sizeof(kBindings[0])); k++) {
            const Binding& b = kBindings[k];
            ImGui::PushID(k);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", tr(b.action), arrow_for(b.key));
            ImGui::TableNextColumn();
            std::string cur = config_get(std::string("input.key.") + b.key, b.def);
            std::string text = g_capturing == k ? std::string(tr("controls.press_key")) : cur;
            if (ImGui::Button(text.c_str(), ImVec2(-1, 0))) {
                g_capturing = k;
                ui_begin_key_capture();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (button(tr("controls.reset_keys"))) {
        for (const Binding& b : kBindings) config_set(std::string("input.key.") + b.key, b.def);
        config_save();
        toast(tr("common.saved"));
    }
}

// Saves: what is there, backups, import/export (saves.h). Folder answers come from the platform's picker.
enum class Pick { None, Import, Export };
Pick g_pick = Pick::None;
std::string g_confirm_import;  // a path waiting for the player's yes
std::string g_confirm_restore;

// After an import or a restore the running game must not save again (it would write the old island over the new
// one): the screen then only offers to quit.
bool g_must_quit = false;

void do_import(const std::string& from) {
    std::string err;
    if (saves::import_save(from, &err))
        g_must_quit = true;
    else
        toast(trf("saves.import.fail", err));
}

void page_saves() {
    bool has = saves::has_save();
    row("saves.current", nullptr, [&](float) {
        if (has)
            ImGui::TextWrapped("%s", trf("saves.last_modified", saves::last_modified()).c_str());
        else
            ImGui::TextWrapped("%s", tr("saves.none"));
    });
    float bw = std::min(ImGui::GetContentRegionAvail().x, 300.0f * g_scale);
    if (button(tr("saves.open_folder"), bw)) {
        std::error_code ec;
        fs::create_directories(fs::u8path(saves::save_dir()), ec);
        plat::open_path(saves::save_dir());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!has);
    if (button(tr("saves.backup"), bw)) {
        std::string err, name = saves::backup_now(&err);
        toast(name.empty() ? err : trf("saves.backup.ok", name));
    }
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, 4 * g_scale));
    ImGui::TextWrapped("%s", tr("saves.import.desc"));
    ImGui::TextColored(ImVec4(0.75f, 0.35f, 0.3f, 1), "%s", tr("saves.import.warning"));
    if (button(tr("saves.import"), bw)) {
        g_pick = Pick::Import;
        plat::pick_path(false, tr("saves.pick_folder"));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!has);
    if (button(tr("saves.export"), bw)) {
        g_pick = Pick::Export;
        plat::pick_path(false, tr("saves.pick_folder"));
    }
    ImGui::EndDisabled();
    std::string picked;
    if (g_pick != Pick::None && plat::pick_result(picked)) {
        Pick what = g_pick;
        g_pick = Pick::None;
        if (!picked.empty()) {
            if (what == Pick::Import) {
                std::string why;
                if (saves::looks_like_acnh_save(picked, &why))
                    g_confirm_import = picked;
                else
                    toast(trf("saves.import.fail", why));
            } else {
                std::string written, err;
                toast(saves::export_save(picked, &written, &err) ? trf("saves.export.ok", written)
                                                                 : trf("saves.export.fail", err));
            }
        }
    }
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::TextUnformatted(tr("saves.detected"));
    static std::vector<saves::Found> found;
    static double scanned = -100;
    if (ImGui::GetTime() - scanned > 5) found = saves::detect_emulator_saves(), scanned = ImGui::GetTime();
    if (found.empty()) ImGui::TextDisabled("%s", tr("saves.detected.none"));
    for (size_t k = 0; k < found.size(); k++) {
        ImGui::PushID(int(k));
        ImGui::BulletText("%s", found[k].label.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("saves.import"))) g_confirm_import = found[k].path;
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::TextUnformatted(tr("saves.backups"));
    for (const std::string& b : saves::list_backups()) {
        ImGui::PushID(b.c_str());
        ImGui::BulletText("%s", b.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("saves.restore"))) g_confirm_restore = b;
        ImGui::PopID();
    }
    // Confirmations.
    if (!g_confirm_import.empty()) ImGui::OpenPopup("confirm_import");
    if (ImGui::BeginPopupModal("confirm_import", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextWrapped("%s", tr("saves.import.warning"));
        ImGui::TextDisabled("%s", g_confirm_import.c_str());
        if (button(tr("common.yes"))) {
            do_import(g_confirm_import);
            g_confirm_import.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button(tr("common.cancel"))) g_confirm_import.clear(), ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!g_confirm_restore.empty()) ImGui::OpenPopup("confirm_restore");
    if (ImGui::BeginPopupModal("confirm_restore", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextWrapped("%s", tr("saves.restore.confirm"));
        if (button(tr("common.yes"))) {
            std::string err;
            if (saves::restore_backup(g_confirm_restore, &err))
                g_must_quit = true;
            else
                toast(err);
            g_confirm_restore.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button(tr("common.cancel"))) g_confirm_restore.clear(), ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void page_about() {
    ImGui::TextWrapped("%s", trf("about.version", NX_VERSION_STRING).c_str());
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::TextWrapped("%s", tr("about.desc"));
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::TextWrapped("%s", tr("about.credits"));
    ImGui::TextWrapped("%s", tr("about.license"));
}

bool g_quit_confirm = false;

}  // namespace

bool settings_draw(float scale, ImFont* title) {
    g_scale = scale;
    static const bool cycle = std::getenv("NX_UI_CYCLE") != nullptr;  // tests: every page in turn, 10 s each
    if (cycle) g_page = int(double(plat::monotonic_ns()) / 1e10) % kPageCount;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize;
    bool portrait = ds.x < ds.y * 1.2f;
    // Dim the game.
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ds, rgb(40, 28, 12, 120));
    ImVec2 size = portrait ? ImVec2(ds.x * 0.96f, ds.y * 0.94f)
                           : ImVec2(std::min(ds.x * 0.92f, 1180.0f * scale), std::min(ds.y * 0.9f, 680.0f * scale));
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    bool open = true;
    ImGui::Begin("##settings", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Header: title on a green ribbon, close at the right.
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = 56.0f * scale, w = ImGui::GetContentRegionAvail().x;
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), rgb(124, 194, 103), h * 0.5f);
        draw_icon(dl, 0, ImVec2(p.x + h * 0.55f, p.y + h * 0.5f), h * 0.36f, rgb(255, 255, 255));
        ImGui::PushFont(title);
        ImVec2 ts = ImGui::CalcTextSize(tr("app.title"));
        dl->AddText(ImVec2(p.x + h * 1.1f, p.y + (h - ts.y) * 0.5f), rgb(255, 255, 255), tr("app.title"));
        ImGui::PopFont();
        float bw = std::max(120.0f * scale, ImGui::CalcTextSize(tr("common.resume")).x + 40 * scale);
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - 10 * scale, p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 255, 255, 230));
        if (ImGui::Button(tr("common.resume"), ImVec2(bw, 0))) open = false;
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 14 * scale));
    }
    // Page buttons: a column on the left, or a row on top when narrow. The footer keeps its line below both.
    float footer = ImGui::GetFrameHeightWithSpacing() + 10.0f * scale;
    float tab_w = portrait ? ImGui::GetContentRegionAvail().x : 210.0f * scale;
    ImGui::BeginChild("##tabs", ImVec2(tab_w, portrait ? 64.0f * scale : -footer), ImGuiChildFlags_None,
                      portrait ? ImGuiWindowFlags_HorizontalScrollbar : 0);
    for (int k = 0; k < kPageCount; k++) {
        if (portrait && k) ImGui::SameLine();
        ImGui::PushID(k);
        const char* label = tr(kPages[k].key);
        float bh = 52.0f * scale;
        float bw = portrait ? std::max(140.0f * scale, ImGui::CalcTextSize(label).x + bh + 24 * scale) : -1;
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool sel = g_page == k;
        if (ImGui::InvisibleButton("##p", ImVec2(bw < 0 ? ImGui::GetContentRegionAvail().x : bw, bh))) g_page = k;
        bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        ImVec2 q = ImGui::GetItemRectMax();
        ImDrawList* cl = ImGui::GetWindowDrawList();
        cl->AddRectFilled(p, q, sel ? kPages[k].color : (hov ? rgb(255, 232, 160) : rgb(248, 239, 214)), bh * 0.5f);
        cl->AddCircleFilled(ImVec2(p.x + bh * 0.5f, p.y + bh * 0.5f), bh * 0.36f,
                            sel ? rgb(255, 255, 255, 70) : kPages[k].color);
        draw_icon(cl, kPages[k].icon, ImVec2(p.x + bh * 0.5f, p.y + bh * 0.5f), bh * 0.3f, rgb(255, 255, 255));
        ImVec2 ts = ImGui::CalcTextSize(label);
        cl->AddText(ImVec2(p.x + bh + 6 * scale, p.y + (bh - ts.y) * 0.5f), sel ? rgb(255, 255, 255) : rgb(107, 79, 54),
                    label);
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (!portrait) ImGui::SameLine();
    // The page.
    ImGui::BeginChild("##page", ImVec2(0, -footer), ImGuiChildFlags_None);
    ImGui::Dummy(ImVec2(0, 4 * scale));
    switch (g_page) {
        case 0: page_game(); break;
        case 1: page_video(); break;
        case 2: page_audio(); break;
        case 3: page_controls(); break;
        case 4: page_saves(); break;
        default: page_about(); break;
    }
    ImGui::EndChild();
    // Footer: restart note or toast, and quit.
    {
        const char* msg = nullptr;
        if (ImGui::GetTime() < g_toast_until)
            msg = g_toast.c_str();
        else if (g_restart_note)
            msg = tr("common.restart_needed");
        if (msg) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4(0.30f, 0.55f, 0.45f, 1), "%s", msg);
            ImGui::SameLine();
        }
        float qw = ImGui::CalcTextSize(tr("common.quit_game")).x + 30 * scale;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - qw);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(232, 115, 122, 255));
        if (ImGui::Button(tr("common.quit_game"), ImVec2(qw, 0))) g_quit_confirm = true;
        ImGui::PopStyleColor();
    }
    if (g_quit_confirm) ImGui::OpenPopup("quit");
    if (ImGui::BeginPopupModal("quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextWrapped("%s", tr("common.quit_confirm"));
        if (ImGui::Button(tr("common.yes"))) {
            config_save();
            plat::exit_process(0);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("common.no"))) g_quit_confirm = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_must_quit) ImGui::OpenPopup("must_quit");
    if (ImGui::BeginPopupModal("must_quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextWrapped("%s", tr("saves.import.ok"));
        if (ImGui::Button(tr("common.quit_game"))) plat::exit_process(0);
        ImGui::EndPopup();
    }
    // Cancel (B / Escape) closes the screen when nothing else wants it (never after an import: see g_must_quit).
    if (g_must_quit) open = true;
    if (g_capturing < 0 && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        open = false;
    if (g_must_quit) open = true;
    ImGui::End();
    return open;
}

void settings_hint(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    const char* text = tr("app.hint_open");
    ImVec2 ts = ImGui::CalcTextSize(text);
    float pad = 14.0f * scale;
    ImVec2 p(io.DisplaySize.x - ts.x - pad * 3, io.DisplaySize.y - ts.y - pad * 3);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + ts.x + pad * 2, p.y + ts.y + pad * 2), rgb(255, 249, 232, 235), (ts.y + pad * 2) * 0.5f);
    dl->AddText(ImVec2(p.x + pad, p.y + pad), rgb(107, 79, 54), text);
}

}  // namespace nx::ui
