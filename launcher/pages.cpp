// The launcher's pages (launcher.h), in the settings screen's style (ui/theme.h).
//
//   welcome   two ways in: set up from the game files, or pick a build folder made before
//   files     step 1: keys, base game, update 3.0.2, DLC, the pt-BR translation; each checked when chosen
//   install   step 2: where the build goes, and what this computer has (with the command to install the rest)
//   build     step 3: tools/setup_acnh.sh running: stages, progress, log, stop; the error state
//   done      step 4: Play
//   home      later starts, with a valid build remembered: Play, Settings, Rebuild, Change build folder
//   settings  a few of the game's config.ini settings (the rest is in the game, F1)
//
// Layout: a cream card over a mint background, a green ribbon with the title and the language at the top, the
// page under it and its buttons in a footer line. Sizes are design pixels times `scale` (main.cpp); below an aspect
// of 1.2 the two-column parts stack.
#include "launcher.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>

#include "imgui.h"

#include "ui/i18n.h"
#include "ui/theme.h"

namespace launcher {

namespace {

using nx::ui::theme_icon;
using nx::ui::tr;
using nx::ui::trf;

ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
const ImU32 kInk = rgb(107, 79, 54), kInkSoft = rgb(150, 128, 100), kPaper = rgb(248, 239, 214);
const ImU32 kGreen = rgb(124, 194, 103), kMint = rgb(118, 200, 170), kYellow = rgb(255, 217, 102);
const ImU32 kOrange = rgb(245, 166, 35), kRed = rgb(232, 115, 122), kBlue = rgb(79, 179, 217);
const ImU32 kPurple = rgb(155, 125, 212), kBrown = rgb(176, 140, 100), kGrey = rgb(214, 200, 168);
const ImU32 kWhite = rgb(255, 255, 255);

ImVec4 col4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

enum class Page { Welcome, Files, Install, Build, Done, Home, Settings };

Page g_page = Page::Welcome;
SDL_Window* g_window = nullptr;
float g_scale = 1;
ImFont* g_title = nullptr;
ImFont* g_body = nullptr;
bool g_portrait = false;
bool g_exit = false;

struct Item {
    std::string path;
    FileCheck check;
};
Item g_keys, g_base, g_update, g_ptbr;
std::vector<Item> g_dlc;
bool g_keys_auto = false;
std::string g_install;
std::vector<Prereq> g_prereqs;
Build g_build;
const char* g_build_problem = "launcher.reason.none";  // "" when g_build is valid
std::string g_banner;  // an error under the page title (a refused file or folder)
std::string g_toast;
double g_toast_until = 0;

// File dialogs answer on their own time (and maybe on another thread): queued here, applied in the frame.
enum Target { kPickKeys, kPickBase, kPickUpdate, kPickDlc, kPickMod, kPickInstall, kPickBuild };
struct Picked {
    int target;
    std::vector<std::string> paths;
};
std::mutex g_pick_lock;
std::vector<Picked> g_picked;

// Build page.
std::vector<std::string> g_log;
bool g_rebuild = false;  // started from home: no files page to go back to
bool g_confirm_stop = false, g_confirm_quit = false;
bool g_quit_after_stop = false;

// The game's config.ini (settings page).
Ini g_game_ini;

float S(float v) { return v * g_scale; }

void toast(const std::string& t) {
    g_toast = t;
    g_toast_until = ImGui::GetTime() + 4.0;
}

// ---- persistence ----------------------------------------------------------------------------------------------

void save_files() {
    Ini& s = settings();
    s.set("files.keys", g_keys_auto ? "" : g_keys.path);
    s.set("files.base", g_base.path);
    s.set("files.update", g_update.path);
    std::string dlc;
    for (const Item& d : g_dlc) dlc += (dlc.empty() ? "" : "|") + d.path;
    s.set("files.dlc", dlc);
    s.set("files.ptbr", g_ptbr.path);
    s.set("launcher.install", g_install);
    s.save();
}

void set_item(Item& it, Kind kind, const std::string& path) {
    it.path = path;
    it.check = check_file(kind, path);
}

void refresh_keys_auto() {
    if (!g_keys.path.empty() && !g_keys_auto) return;
    std::string k = find_keys({g_base.path, g_update.path});
    g_keys_auto = !k.empty();
    set_item(g_keys, Kind::Keys, k);
}

void set_build(const std::string& folder, bool remember) {
    g_build = Build();
    g_build_problem = find_build(folder, g_build);
    if (*g_build_problem == 0 && remember) {
        settings().set("launcher.build", folder);
        settings().save();
    }
}

void load_game_ini() { g_game_ini.load(g_build.data + "/config.ini"); }

// ---- dialogs --------------------------------------------------------------------------------------------------

void SDLCALL on_picked(void* user, const char* const* list, int) {
    Picked p{int(reinterpret_cast<intptr_t>(user)), {}};
    if (list)
        for (; *list; list++) p.paths.push_back(*list);
    std::lock_guard lk(g_pick_lock);
    g_picked.push_back(std::move(p));
}

void pick(Target t) {
    static const SDL_DialogFileFilter dumps[] = {{"NSP / XCI", "nsp;xci"}, {"*", "*"}};
    static const SDL_DialogFileFilter nsp[] = {{"NSP", "nsp"}, {"*", "*"}};
    static const SDL_DialogFileFilter keys[] = {{"prod.keys", "keys"}, {"*", "*"}};
    static const SDL_DialogFileFilter zip[] = {{"ZIP", "zip"}};
    std::string last = settings().get("files.last_dir");
    const char* where = last.empty() ? nullptr : last.c_str();
    void* user = reinterpret_cast<void*>(intptr_t(t));
    switch (t) {
        case kPickKeys: SDL_ShowOpenFileDialog(on_picked, user, g_window, keys, 2, where, false); break;
        case kPickBase: SDL_ShowOpenFileDialog(on_picked, user, g_window, dumps, 2, where, false); break;
        case kPickUpdate: SDL_ShowOpenFileDialog(on_picked, user, g_window, nsp, 2, where, false); break;
        case kPickDlc: SDL_ShowOpenFileDialog(on_picked, user, g_window, nsp, 2, where, true); break;
        case kPickMod: SDL_ShowOpenFileDialog(on_picked, user, g_window, zip, 1, where, false); break;
        case kPickInstall:
            SDL_ShowOpenFolderDialog(on_picked, user, g_window, is_dir(g_install) ? g_install.c_str() : nullptr, false);
            break;
        case kPickBuild: SDL_ShowOpenFolderDialog(on_picked, user, g_window, nullptr, false); break;
    }
}

void add_dlc(const std::string& path) {
    for (const Item& d : g_dlc)
        if (d.path == path) return;
    Item it;
    set_item(it, Kind::Dlc, path);
    g_dlc.push_back(it);
}

void use_build_folder(const std::string& folder) {
    Build b;
    const char* why = find_build(folder, b);
    if (*why) {
        g_banner = trf("launcher.build_invalid", tr(why));
        return;
    }
    g_banner.clear();
    set_build(folder, true);
    load_game_ini();
    g_page = Page::Home;
}

void apply_picks() {
    std::vector<Picked> picked;
    {
        std::lock_guard lk(g_pick_lock);
        picked.swap(g_picked);
    }
    for (const Picked& p : picked) {
        if (p.paths.empty()) continue;  // cancelled
        const std::string& first = p.paths[0];
        switch (p.target) {
            case kPickKeys: set_item(g_keys, Kind::Keys, first), g_keys_auto = false; break;
            case kPickBase: set_item(g_base, Kind::Base, first); break;
            case kPickUpdate: set_item(g_update, Kind::Update, first); break;
            case kPickDlc:
                for (const std::string& d : p.paths) add_dlc(d);
                break;
            case kPickMod: set_item(g_ptbr, Kind::Mod, first); break;
            case kPickInstall: g_install = first, g_prereqs = check_prerequisites(g_install); break;
            case kPickBuild: use_build_folder(first); break;
        }
        if (p.target <= kPickMod) {
            settings().set("files.last_dir", parent_dir(first));
            refresh_keys_auto();
        }
        save_files();
    }
}

// ---- drawing helpers ------------------------------------------------------------------------------------------

float font_size(ImFont* f) { return f->FontSize * ImGui::GetIO().FontGlobalScale; }

ImVec2 measure(ImFont* f, float size, const char* text, float wrap) {
    return f->CalcTextSizeA(size, FLT_MAX, wrap, text);
}

// The background: mint to leaf green with a soft dot pattern, as the game's menus.
void draw_background(ImVec2 ds) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ds, rgb(160, 222, 200), rgb(160, 222, 200), rgb(196, 230, 160),
                                rgb(196, 230, 160));
    float step = S(46), r = S(5);
    int row = 0;
    for (float y = step * 0.5f; y < ds.y + step; y += step * 0.866f, row++)
        for (float x = (row & 1) ? step * 0.5f : 0; x < ds.x + step; x += step)
            dl->AddCircleFilled(ImVec2(x, y), r, rgb(255, 255, 255, 46));
}

ImU32 status_color(Status s) {
    switch (s) {
        case Status::Ok: return kGreen;
        case Status::Warning: return kOrange;
        case Status::Error: return kRed;
        default: return kGrey;
    }
}

// A small round status mark at centre c.
void status_mark(ImDrawList* dl, ImVec2 c, float r, Status s) {
    dl->AddCircleFilled(c, r, status_color(s));
    int icon = s == Status::Ok ? nx::ui::kIconCheck : s == Status::Error ? nx::ui::kIconCross
             : s == Status::Warning ? nx::ui::kIconWarning : -1;
    if (icon >= 0) theme_icon(dl, icon, c, r * 0.8f, kWhite);
}

float button_width(const char* label) { return std::max(S(120), ImGui::CalcTextSize(label).x + S(40)); }

bool button(const char* label, ImU32 color = kMint, bool enabled = true, float w = 0) {
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, col4(color));
    ImGui::PushStyleColor(ImGuiCol_Text, col4(color == kMint || color == kWhite ? kInk : kWhite));
    bool r = ImGui::Button(label, ImVec2(w > 0 ? w : button_width(label), 0));
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    return r;
}

// Puts the next `count` buttons at the right of the current line.
void align_right(std::initializer_list<const char*> labels) {
    float w = 0;
    for (const char* l : labels) w += button_width(l);
    w += ImGui::GetStyle().ItemSpacing.x * float(labels.size() - 1);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - w));
}

void text_small(const char* text, ImU32 color = kInkSoft) {
    ImGui::PushStyleColor(ImGuiCol_Text, col4(color));
    ImGui::SetWindowFontScale(0.84f);
    ImGui::TextWrapped("%s", text);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
}

void heading(const char* title, const char* text) {
    if (*title) {
        ImGui::PushFont(g_title);
        ImGui::TextWrapped("%s", title);
        ImGui::PopFont();
    }
    if (text && *text) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextWrapped("%s", text);
        ImGui::PopTextWrapPos();
    }
    if (!g_banner.empty()) {
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(rgb(253, 226, 222)));
        ImGui::BeginChild("##banner", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushStyleColor(ImGuiCol_Text, col4(rgb(170, 70, 70)));
        ImGui::TextWrapped("%s", g_banner.c_str());
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, S(4)));
}

std::string clock_text(double seconds) {
    int s = int(std::max(0.0, seconds));
    char b[32];
    std::snprintf(b, sizeof b, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    return b;
}

// ---- the frame: ribbon and steps ------------------------------------------------------------------------------

std::string language_shown() {
    std::string cur = settings().get("launcher.language", "auto");
    std::string shown = tr("common.auto");
    for (const nx::ui::UiLanguage& l : nx::ui::ui_languages()) {
        if (cur == l.code) shown = l.native_name;
        if (cur == "auto" && nx::ui::ui_language() == l.code) shown += std::string(" · ") + l.native_name;
    }
    return shown;
}

void language_combo(float width) {
    std::string cur = settings().get("launcher.language", "auto");
    std::string shown = language_shown();
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col4(rgb(255, 255, 255, 235)));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col4(kYellow));
    ImGui::PushStyleColor(ImGuiCol_Button, col4(rgb(255, 255, 255, 235)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col4(kYellow));
    if (ImGui::BeginCombo("##lang", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
        auto choose = [&](const char* code, const char* name) {
            bool sel = cur == code;
            if (ImGui::Selectable(name, sel)) {
                settings().set("launcher.language", code);
                settings().save();
                nx::ui::ui_set_language(code);
                for (Item* it : {&g_keys, &g_base, &g_update, &g_ptbr})  // their texts are translated
                    if (!it->path.empty()) it->check = check_file(it == &g_keys ? Kind::Keys : it == &g_base ? Kind::Base
                                                                  : it == &g_update ? Kind::Update : Kind::Mod, it->path);
                for (Item& d : g_dlc) d.check = check_file(Kind::Dlc, d.path);
                if (!g_prereqs.empty()) g_prereqs = check_prerequisites(g_install);
            }
            if (sel) ImGui::SetItemDefaultFocus();
        };
        choose("auto", tr("common.auto"));
        for (const nx::ui::UiLanguage& l : nx::ui::ui_languages()) choose(l.code, l.native_name);
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor(4);
}

void ribbon() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = S(64), w = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kGreen, h * 0.5f);
    dl->AddCircleFilled(ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.36f, rgb(255, 255, 255, 60));
    theme_icon(dl, nx::ui::kIconLeaf, ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.3f, kWhite);
    // Measured in the combo's own font, with room for the frame padding and the arrow.
    float combo_w = measure(g_body, font_size(g_body), language_shown().c_str(), -1).x + ImGui::GetFrameHeight() +
                    ImGui::GetStyle().FramePadding.x * 2 + S(24);
    combo_w = std::clamp(combo_w, S(150), g_portrait ? w * 0.5f : S(420));
    float tsz = font_size(g_title);
    ImVec2 ts = measure(g_title, tsz, "ACNHRecomp", -1);
    float x = p.x + h * 1.0f;
    dl->AddText(g_title, tsz, ImVec2(x, p.y + (h - ts.y) * 0.5f), kWhite, "ACNHRecomp");
    const char* sub = tr("launcher.subtitle");
    float ssz = font_size(g_body) * 0.82f;
    ImVec2 ss = measure(g_body, ssz, sub, -1);
    float room = w - (x - p.x) - ts.x - combo_w - h * 0.9f;
    if (ss.x < room)
        dl->AddText(g_body, ssz, ImVec2(x + ts.x + S(16), p.y + (h - ss.y) * 0.5f + S(2)), rgb(255, 255, 255, 215), sub);
    // The language, always reachable.
    float gx = p.x + w - combo_w - h * 0.35f - S(30);
    theme_icon(dl, nx::ui::kIconGlobe, ImVec2(gx, p.y + h * 0.5f), S(14), kWhite);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - combo_w - h * 0.25f, p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
    language_combo(combo_w);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + S(12)));
}

// Step 1..4 of the setup, the current one highlighted.
void steps(int current) {
    const char* labels[] = {tr("launcher.step.files"), tr("launcher.step.install"), tr("launcher.step.build"),
                            tr("launcher.step.done")};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, r = S(15), h = S(40);
    float seg = w / 4;
    const float fs = font_size(g_body) * 0.85f;
    for (int k = 0; k < 4; k++) {
        float cx = p.x + seg * k + r + S(2), cy = p.y + h * 0.5f;
        bool label = !g_portrait || k == current;
        float lw = label ? measure(g_body, fs, labels[k], -1).x + S(16) : 0;
        float x0 = cx + r + S(6) + lw, x1 = p.x + seg * (k + 1) - S(6);
        if (k < 3 && x1 > x0)
            dl->AddRectFilled(ImVec2(x0, cy - S(1.5f)), ImVec2(x1, cy + S(1.5f)), k < current ? kGreen : kGrey, S(1.5f));
        ImU32 c = k < current ? kGreen : k == current ? kYellow : kGrey;
        dl->AddCircleFilled(ImVec2(cx, cy), r, c);
        if (k < current) {
            theme_icon(dl, nx::ui::kIconCheck, ImVec2(cx, cy), r * 0.75f, kWhite);
        } else {
            char n[4];
            std::snprintf(n, sizeof n, "%d", k + 1);
            ImVec2 ns = measure(g_body, fs, n, -1);
            dl->AddText(g_body, fs, ImVec2(cx - ns.x * 0.5f, cy - ns.y * 0.5f), k == current ? kInk : kWhite, n);
        }
        if (label) {
            ImVec2 ls = measure(g_body, fs, labels[k], -1);
            dl->AddText(g_body, fs, ImVec2(cx + r + S(8), cy - ls.y * 0.5f), k == current ? kInk : kInkSoft, labels[k]);
        }
    }
    ImGui::Dummy(ImVec2(w, h + S(4)));
}

// ---- welcome --------------------------------------------------------------------------------------------------

struct Card {
    int icon;
    ImU32 color;
    const char* title;
    const char* desc;
    const char* note;  // in orange under the text, or nullptr
    bool enabled;
};

float card_height(const Card& c, float w) {
    float pad = S(24), r = S(34), inner = w - 2 * pad;
    float hgt = pad + 2 * r + S(14) + measure(g_title, font_size(g_title) * 0.85f, c.title, inner).y + S(8) +
                measure(g_body, font_size(g_body), c.desc, inner).y + pad;
    if (c.note) hgt += S(10) + measure(g_body, font_size(g_body) * 0.85f, c.note, inner).y;
    return hgt;
}

bool card(const char* id, const Card& c, ImVec2 size) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!c.enabled);
    bool clicked = ImGui::InvisibleButton(id, size);
    ImGui::EndDisabled();
    bool hov = c.enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, q, hov ? rgb(255, 240, 196) : kPaper, S(26));
    if (hov) dl->AddRect(p, q, rgb(246, 170, 40), S(26), 0, S(3));
    float pad = S(24), r = S(34), inner = size.x - 2 * pad;
    ImU32 icol = c.enabled ? c.color : kGrey;
    dl->AddCircleFilled(ImVec2(p.x + pad + r, p.y + pad + r), r, icol);
    theme_icon(dl, c.icon, ImVec2(p.x + pad + r, p.y + pad + r), r * 0.6f, kWhite);
    float y = p.y + pad + 2 * r + S(14);
    float ts = font_size(g_title) * 0.85f;
    dl->AddText(g_title, ts, ImVec2(p.x + pad, y), c.enabled ? kInk : kInkSoft, c.title, nullptr, inner);
    y += measure(g_title, ts, c.title, inner).y + S(8);
    dl->AddText(g_body, font_size(g_body), ImVec2(p.x + pad, y), kInkSoft, c.desc, nullptr, inner);
    y += measure(g_body, font_size(g_body), c.desc, inner).y + S(10);
    if (c.note) dl->AddText(g_body, font_size(g_body) * 0.85f, ImVec2(p.x + pad, y), rgb(200, 120, 20), c.note, nullptr, inner);
    // An arrow in a circle, bottom right: the card leads somewhere.
    if (c.enabled) {
        ImVec2 a(q.x - pad - S(16), p.y + pad + r);
        dl->AddCircleFilled(a, S(16), hov ? kYellow : rgb(240, 228, 196));
        theme_icon(dl, nx::ui::kIconPlay, a, S(11), kInk);
    }
    return clicked && c.enabled;
}

void go_files() {
    g_banner.clear();
    refresh_keys_auto();
    g_page = Page::Files;
}

void page_welcome() {
    heading(tr("launcher.welcome.title"), tr("launcher.welcome.text"));
    Card setup{nx::ui::kIconLeaf, kGreen, tr("launcher.welcome.setup"), tr("launcher.welcome.setup.desc"),
               setup_supported() ? nullptr : tr("launcher.welcome.windows"), setup_supported()};
    Card existing{nx::ui::kIconFolder, kBlue, tr("launcher.welcome.existing"), tr("launcher.welcome.existing.desc"),
                  nullptr, true};
    float avail = ImGui::GetContentRegionAvail().x, gap = S(20);
    if (g_portrait) {
        if (card("##setup", setup, ImVec2(avail, card_height(setup, avail)))) go_files();
        ImGui::Dummy(ImVec2(0, S(2)));
        if (card("##existing", existing, ImVec2(avail, card_height(existing, avail)))) pick(kPickBuild);
        ImGui::Dummy(ImVec2(0, S(4)));
        text_small(tr("launcher.welcome.legal"));
    } else {
        float w = (avail - gap) * 0.5f;
        float h = std::max(card_height(setup, w), card_height(existing, w));
        if (card("##setup", setup, ImVec2(w, h))) go_files();
        ImGui::SameLine(0, gap);
        if (card("##existing", existing, ImVec2(w, h))) pick(kPickBuild);
    }
}

void footer_welcome() {
    if (!settings().get("launcher.build").empty()) {
        if (button(tr("common.back"), kWhite)) g_banner.clear(), g_page = Page::Home;
        ImGui::SameLine();
    }
    if (g_portrait) return;  // the note is on the page
    ImGui::AlignTextToFramePadding();
    ImGui::SetWindowFontScale(0.84f);
    ImGui::TextColored(col4(kInkSoft), "%s", tr("launcher.welcome.legal"));
    ImGui::SetWindowFontScale(1.0f);
}

// ---- files ----------------------------------------------------------------------------------------------------

// A file row: a coloured circle with the item's icon (its colour is the check's), the name and an explanation, each
// chosen file with its check, and the choose button at the right (under the text when narrow).
void item_card(const char* id, int icon, const char* title, const char* desc, bool optional, std::vector<Item*> files,
               const char* button_label, Target target, const std::function<void(size_t)>& remove) {
    Status st = Status::Empty;
    for (Item* f : files) st = std::max(st, f->check.status);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(14)));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), S(5)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float avail = ImGui::GetContentRegionAvail().x;
    bool narrow = avail < S(600);
    float bw = button_width(button_label);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float r = S(24);
    ImVec2 c(p0.x + r, p0.y + r);
    dl->AddCircleFilled(c, r, st == Status::Empty ? (optional ? kGrey : kBrown) : status_color(st));
    theme_icon(dl, icon, c, r * 0.6f, kWhite);
    if (st != Status::Empty) status_mark(dl, ImVec2(c.x + r * 0.75f, c.y + r * 0.75f), S(9), st);
    float tx = 2 * r + S(16);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + tx);
    ImGui::BeginGroup();
    float text_w = narrow ? avail - tx : avail - tx - bw - S(20);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_w);
    ImGui::TextUnformatted(title);
    if (optional) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, col4(kInkSoft));
        ImGui::SetWindowFontScale(0.84f);
        ImGui::TextUnformatted(tr("launcher.optional"));
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
    }
    text_small(desc);
    if (files.empty() || (files.size() == 1 && files[0]->path.empty())) {
        text_small(tr(optional ? "launcher.status.none" : "launcher.status.missing"),
                   optional ? kInkSoft : rgb(190, 120, 60));
    }
    for (size_t k = 0; k < files.size(); k++) {
        Item* f = files[k];
        if (f->path.empty()) continue;
        ImGui::PushID(int(k));
        ImGui::PushStyleColor(ImGuiCol_Button, col4(rgb(240, 228, 196)));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(6), S(2)));
        if (ImGui::SmallButton("×")) remove(k);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("launcher.remove"));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImU32 tc = f->check.status == Status::Ok ? rgb(70, 140, 60)
                 : f->check.status == Status::Warning ? rgb(190, 120, 20) : rgb(190, 80, 80);
        ImGui::PushStyleColor(ImGuiCol_Text, col4(tc));
        ImGui::TextWrapped("%s", f->check.text.c_str());
        ImGui::PopStyleColor();
        // The file's name after the check when both fit on the line.
        std::string shown = f == &g_keys && g_keys_auto ? trf("launcher.status.found", file_name(f->path)) : file_name(f->path);
        float used = ImGui::GetItemRectSize().x + S(14) + ImGui::CalcTextSize(shown.c_str()).x * 0.84f;
        if (used < text_w - S(40)) ImGui::SameLine(0, S(14));
        text_small(shown.c_str());
        ImGui::EndGroup();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f->path.c_str());
        ImGui::PopID();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    float group_h = ImGui::GetItemRectSize().y;
    if (!narrow) {
        float y = p0.y + std::max(0.0f, (std::max(group_h, 2 * r) - ImGui::GetFrameHeight()) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + avail - bw, y));
    } else {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + tx);
    }
    if (button(button_label, kMint, true, bw)) pick(target);
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

bool files_ready() {
    for (const Item* it : {&g_keys, &g_base, &g_update})
        if (it->path.empty() || it->check.status == Status::Error) return false;
    if (!g_ptbr.path.empty() && g_ptbr.check.status == Status::Error) return false;
    for (const Item& d : g_dlc)
        if (d.check.status == Status::Error) return false;
    return true;
}

void page_files() {
    steps(0);
    heading(tr("launcher.files.title"), tr("launcher.files.text"));
    auto label = [](const Item& it) { return tr(it.path.empty() ? "launcher.choose" : "launcher.change"); };
    auto clear = [](Item& it, bool keys) {
        return [&it, keys](size_t) {
            it = Item();
            if (keys) g_keys_auto = false;
            save_files();
        };
    };
    item_card("##keys", nx::ui::kIconKey, tr("launcher.item.keys"), tr("launcher.item.keys.desc"), false, {&g_keys},
              label(g_keys), kPickKeys, clear(g_keys, true));
    item_card("##base", nx::ui::kIconLeaf, tr("launcher.item.base"), tr("launcher.item.base.desc"), false, {&g_base},
              label(g_base), kPickBase, clear(g_base, false));
    item_card("##update", nx::ui::kIconHouse, tr("launcher.item.update"), tr("launcher.item.update.desc"), false,
              {&g_update}, label(g_update), kPickUpdate, clear(g_update, false));
    std::vector<Item*> dlc;
    for (Item& d : g_dlc) dlc.push_back(&d);
    item_card("##dlc", nx::ui::kIconBox, tr("launcher.item.dlc"), tr("launcher.item.dlc.desc"), true, dlc,
              tr("launcher.add"), kPickDlc, [](size_t k) {
                  g_dlc.erase(g_dlc.begin() + long(k));
                  save_files();
              });
    item_card("##ptbr", nx::ui::kIconText, tr("launcher.item.ptbr"), tr("launcher.item.ptbr.desc"), true, {&g_ptbr},
              label(g_ptbr), kPickMod, clear(g_ptbr, false));
}

void footer_files() {
    if (button(tr("common.back"), kWhite)) g_banner.clear(), g_page = Page::Welcome;
    bool ready = files_ready();
    if (!ready) {
        bool chosen = !g_keys.path.empty() && !g_base.path.empty() && !g_update.path.empty();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::SetWindowFontScale(0.84f);
        ImGui::TextColored(col4(kInkSoft), "%s", tr(chosen ? "launcher.files.fix_errors" : "launcher.files.missing_required"));
        ImGui::SetWindowFontScale(1.0f);
    }
    align_right({tr("launcher.next")});
    if (button(tr("launcher.next"), kGreen, ready)) {
        g_banner.clear();
        g_prereqs = check_prerequisites(g_install);
        g_page = Page::Install;
    }
}

// ---- install --------------------------------------------------------------------------------------------------

bool prereqs_block() {
    for (const Prereq& p : g_prereqs)
        if (p.status == Status::Error) return true;
    return false;
}

void page_install() {
    steps(1);
    heading(tr("launcher.install.title"), tr("launcher.install.text"));
    // The folder.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::BeginChild("##where", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        float avail = ImGui::GetContentRegionAvail().x, bw = button_width(tr("launcher.change"));
        bool narrow = avail < S(560);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = S(22);
        dl->AddCircleFilled(ImVec2(p.x + r, p.y + r), r, kBlue);
        theme_icon(dl, nx::ui::kIconFolder, ImVec2(p.x + r, p.y + r), r * 0.6f, kWhite);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2 * r + S(14));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (narrow ? avail - 2 * r - S(14) : avail - 2 * r - bw - S(34)));
        ImGui::TextWrapped("%s", g_install.c_str());
        text_small(trf("launcher.install.free", human_size(free_space(g_install))).c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        if (!narrow) ImGui::SetCursorScreenPos(ImVec2(p.x + avail - bw, p.y + std::max(0.0f, r - ImGui::GetFrameHeight() * 0.5f)));
        if (button(tr("launcher.change"), kMint, true, bw)) pick(kPickInstall);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(4)));
    // This computer.
    ImGui::PushFont(g_title);
    ImGui::SetWindowFontScale(0.75f);
    ImGui::TextUnformatted(tr("launcher.checks.title"));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    bool missing_tools = false;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::BeginChild("##checks", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    float col = g_portrait ? 0 : std::min(S(380), ImGui::GetContentRegionAvail().x * 0.45f);
    for (const Prereq& p : g_prereqs) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 c = ImGui::GetCursorScreenPos();
        float r = S(11), lh = ImGui::GetTextLineHeight();
        status_mark(dl, ImVec2(c.x + r, c.y + lh * 0.5f), r, p.status);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2 * r + S(10));
        ImGui::BeginGroup();
        ImGui::TextUnformatted(tr(p.label));
        if (col > 0) ImGui::SameLine(col);
        ImU32 tc = p.status == Status::Ok ? rgb(70, 140, 60) : p.status == Status::Warning ? rgb(190, 120, 20) : rgb(190, 80, 80);
        ImGui::PushStyleColor(ImGuiCol_Text, col4(tc));
        ImGui::TextWrapped("%s", p.detail.c_str());
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        std::string l = p.label;
        if (p.status != Status::Ok && l != "launcher.check.disk" && l != "launcher.check.ram" && l != "launcher.check.scripts")
            missing_tools = true;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    std::string cmd = install_command();
    if (missing_tools && !cmd.empty()) {
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::TextWrapped("%s", tr("launcher.install.hint"));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(rgb(92, 70, 50)));
        ImGui::BeginChild("##cmd", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushStyleColor(ImGuiCol_Text, col4(rgb(255, 244, 214)));
        ImGui::SetWindowFontScale(0.8f);
        ImGui::TextWrapped("%s", cmd.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        if (button(tr("launcher.copy"))) SDL_SetClipboardText(cmd.c_str()), toast(tr("launcher.copied"));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        text_small(tr("launcher.install.other_distro"));
    }
    ImGui::Dummy(ImVec2(0, S(2)));
    text_small(tr("launcher.install.download"));
}

void start_setup(bool rebuild) {
    SetupArgs a;
    a.install_dir = rebuild ? g_build.data : g_install;
    auto keep = [](const Item& it) { return is_file(it.path) ? it.path : std::string(); };
    a.keys = keep(g_keys);
    a.title_keys = title_keys_for(a.keys);
    a.base = keep(g_base);
    a.update = keep(g_update);
    for (const Item& d : g_dlc)
        if (is_file(d.path)) a.dlc.push_back(d.path);
    a.ptbr_zip = keep(g_ptbr);
    std::string err;
    g_log.clear();
    g_rebuild = rebuild;
    if (!setup_start(a, err)) {
        g_banner = trf("launcher.build.start_failed", err);
        return;
    }
    g_banner.clear();
    g_page = Page::Build;
}

void footer_install() {
    if (button(tr("common.back"), kWhite)) g_page = Page::Files;
    align_right({tr("launcher.recheck"), tr("launcher.start")});
    if (button(tr("launcher.recheck"), kWhite)) g_prereqs = check_prerequisites(g_install);
    ImGui::SameLine();
    if (button(tr("launcher.start"), kGreen, !prereqs_block())) {
        save_files();
        start_setup(false);
    }
}

// ---- build ----------------------------------------------------------------------------------------------------

const char* stage_label(int s) {
    static const char* keys[] = {"launcher.stage.tools",   "launcher.stage.import", "launcher.stage.mods",
                                 "launcher.stage.recompile", "launcher.stage.shaders", "launcher.stage.fonts",
                                 "launcher.stage.build"};
    return s >= 0 && s < kStageCount ? tr(keys[s]) : tr("launcher.stage.prepare");
}

void progress_bar(float fraction, bool failed) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = S(30);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), rgb(240, 228, 196), h * 0.5f);
    float fw = std::max(h, w * fraction);
    if (fraction > 0) dl->AddRectFilled(p, ImVec2(p.x + fw, p.y + h), failed ? kRed : kGreen, h * 0.5f);
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d%%", int(fraction * 100 + 0.5f));
    ImVec2 ts = ImGui::CalcTextSize(pct);
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), fraction > 0.52f ? kWhite : kInk, pct);
    ImGui::Dummy(ImVec2(w, h));
}

void stage_list(const SetupState& s, bool failed) {
    const bool has_mod = !g_ptbr.path.empty() && !g_rebuild;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::BeginChild("##stages", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    for (int k = 0; k < kStageCount; k++) {
        if (k == kStageMods && !has_mod) continue;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 c = ImGui::GetCursorScreenPos();
        float r = S(11), lh = ImGui::GetTextLineHeight();
        ImVec2 m(c.x + r, c.y + lh * 0.5f);
        bool current = s.stage == k && !s.done[k];
        if (s.done[k]) {
            status_mark(dl, m, r, Status::Ok);
        } else if (current && failed) {
            status_mark(dl, m, r, Status::Error);
        } else if (current && s.running) {  // a turning arc
            float t = float(ImGui::GetTime());
            dl->AddCircle(m, r, kGrey, 0, S(3));
            dl->PathArcTo(m, r, t * 5.0f, t * 5.0f + 2.0f, 16);
            dl->PathStroke(kOrange, 0, S(3));
        } else {
            dl->AddCircle(m, r, kGrey, 0, S(2));
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2 * r + S(10));
        ImGui::PushStyleColor(ImGuiCol_Text, col4(s.done[k] || current ? kInk : kInkSoft));
        ImGui::TextUnformatted(stage_label(k));
        ImGui::PopStyleColor();
        if (current && s.running && k == kStageBuild && s.build_fraction > 0) {
            ImGui::SameLine();
            ImGui::TextColored(col4(kInkSoft), "%d%%", int(s.build_fraction * 100));
        }
        if (current && !s.last_line.empty()) {  // what it is doing, or the error
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2 * r + S(10));
            ImGui::PushTextWrapPos(0);
            text_small(s.last_line.c_str(), failed ? rgb(190, 80, 80) : kInkSoft);
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// The setup's output, following the end unless scrolled up.
void log_view(float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(rgb(92, 70, 50)));
    ImGui::BeginChild("##log", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, col4(rgb(255, 244, 214)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(2)));
    ImGui::SetWindowFontScale(0.78f);
    bool follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
    ImGuiListClipper clip;
    clip.Begin(int(g_log.size()));
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) ImGui::TextUnformatted(g_log[size_t(i)].c_str());
    if (follow) ImGui::SetScrollHereY(1.0f);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void page_build() {
    SetupState s = setup_state();
    setup_lines(g_log.size(), g_log);
    if (s.finished && s.exit_code == 0 && !s.cancelled) {
        set_build(g_rebuild ? g_build.data : g_install, true);
        load_game_ini();
        g_page = Page::Done;
        return;
    }
    const bool failed = s.finished && !s.cancelled, stopped = s.finished && s.cancelled;
    steps(2);
    if (failed)
        heading(tr("launcher.build.failed_title"),
                (trf("launcher.build.failed", stage_label(s.stage)) + " " + tr("launcher.build.failed_hint")).c_str());
    else if (stopped)
        heading(tr("launcher.build.stopped_title"), tr("launcher.build.cancelled"));
    else
        heading(tr("launcher.build.title"), tr("launcher.build.text"));
    progress_bar(setup_progress(s), failed);
    std::string times = trf("launcher.build.elapsed", clock_text(s.elapsed));
    if (s.running && s.eta >= 0) times += "   ·   " + trf("launcher.build.eta", clock_text(s.eta));
    text_small(times.c_str());
    ImGui::Dummy(ImVec2(0, S(2)));
    // Stages and log side by side, or stacked when narrow (the log then folded unless something failed).
    float avail = ImGui::GetContentRegionAvail().x;
    if (!g_portrait && avail > S(820)) {
        float left = std::min(S(400), avail * 0.42f);
        ImGui::BeginChild("##left", ImVec2(left, 0), ImGuiChildFlags_AutoResizeY);
        stage_list(s, failed);
        ImGui::EndChild();
        ImGui::SameLine(0, S(16));
        ImGui::BeginGroup();
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader(tr("launcher.build.details")))
            log_view(std::max(S(160), ImGui::GetContentRegionAvail().y - S(4)));
        ImGui::EndGroup();
    } else {
        stage_list(s, failed);
        ImGui::Dummy(ImVec2(0, S(2)));
        if (failed) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        if (ImGui::CollapsingHeader(tr("launcher.build.details")))
            log_view(std::max(S(200), ImGui::GetContentRegionAvail().y - S(4)));
    }
}

std::string log_text() {
    std::string t;
    for (const std::string& l : g_log) t += l + "\n";
    return t;
}

void footer_build() {
    SetupState s = setup_state();
    if (s.running) {
        align_right({tr("launcher.stop")});
        if (button(tr("launcher.stop"), kRed)) g_confirm_stop = true;
        return;
    }
    if (button(tr("common.back"), kWhite)) g_page = g_rebuild ? Page::Home : Page::Install;
    const char* logs = tr("launcher.open_logs");
    align_right({tr("launcher.copy_log"), logs, tr("launcher.retry")});
    if (button(tr("launcher.copy_log"), kWhite)) SDL_SetClipboardText(log_text().c_str()), toast(tr("launcher.copied"));
    ImGui::SameLine();
    if (button(logs, kWhite)) open_folder(parent_dir(setup_log_path()));
    ImGui::SameLine();
    if (button(tr("launcher.retry"), kGreen)) start_setup(g_rebuild);
}

// ---- done and home --------------------------------------------------------------------------------------------

void play() {
    std::string err;
    if (launch_game(g_build, err))
        g_exit = true;
    else
        g_banner = trf("launcher.launch_failed", err);
}

bool big_button(const char* label, int icon, ImU32 color, float w, float h) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
    bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, p.y + S(4)), ImVec2(p.x + w, p.y + h + S(4)), rgb(80, 140, 60, 120), h * 0.5f);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? kYellow : color, h * 0.5f);
    float fs = font_size(g_title);
    ImVec2 ts = measure(g_title, fs, label, -1);
    float ir = h * 0.22f, total = ts.x + ir * 2 + S(12);
    float x = p.x + (w - total) * 0.5f;
    theme_icon(dl, icon, ImVec2(x + ir, p.y + h * 0.5f), ir, hov ? kInk : kWhite);
    dl->AddText(g_title, fs, ImVec2(x + ir * 2 + S(12), p.y + (h - ts.y) * 0.5f), hov ? kInk : kWhite, label);
    return clicked;
}

void centered(const char* text, ImFont* font, float size_mul, ImU32 color) {
    float w = ImGui::GetContentRegionAvail().x;
    float fs = font_size(font) * size_mul;
    float wrap = std::min(w, S(760));
    ImVec2 ts = measure(font, fs, text, wrap);
    ImVec2 p = ImGui::GetCursorScreenPos();
    // Line by line, each centred.
    const char* s = text;
    float y = p.y;
    while (*s) {
        const char* e = font->CalcWordWrapPositionA(fs / font->FontSize, s, s + std::strlen(s), wrap);
        if (e == s) e++;
        ImVec2 ls = font->CalcTextSizeA(fs, FLT_MAX, 0, s, e);
        ImGui::GetWindowDrawList()->AddText(font, fs, ImVec2(p.x + (w - ls.x) * 0.5f, y), color, s, e);
        y += fs * 1.25f;
        s = e;
        while (*s == ' ' || *s == '\n') s++;
    }
    ImGui::Dummy(ImVec2(w, std::max(ts.y, y - p.y)));
}

void hero(int icon, ImU32 color, float r) {
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + w * 0.5f, p.y + r);
    dl->AddCircleFilled(c, r + S(10), rgb(255, 255, 255, 140));
    dl->AddCircleFilled(c, r, color);
    theme_icon(dl, icon, c, r * 0.6f, kWhite);
    ImGui::Dummy(ImVec2(w, 2 * r + S(14)));
}

void play_block() {
    float w = std::min(ImGui::GetContentRegionAvail().x, S(360));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - w) * 0.5f);
    if (big_button(tr("launcher.play"), nx::ui::kIconPlay, kGreen, w, S(72))) play();
    ImGui::Dummy(ImVec2(0, S(8)));
}

void page_done() {
    steps(3);
    if (!g_banner.empty()) heading("", "");
    ImGui::Dummy(ImVec2(0, S(10)));
    hero(nx::ui::kIconCheck, kGreen, S(48));
    centered(tr("launcher.done.title"), g_title, 1.15f, kInk);
    ImGui::Dummy(ImVec2(0, S(4)));
    centered(trf("launcher.done.text", g_build.data).c_str(), g_body, 1.0f, kInkSoft);
    ImGui::Dummy(ImVec2(0, S(18)));
    play_block();
}

void footer_done() {
    align_right({tr("launcher.home")});
    if (button(tr("launcher.home"), kWhite)) g_page = Page::Home;
}

// A secondary action as a small card: icon, title, one line.
bool action_card(const char* id, int icon, ImU32 color, const char* title, const char* desc, ImVec2 size, bool enabled) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton(id, size);
    ImGui::EndDisabled();
    bool hov = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, q, hov ? rgb(255, 240, 196) : kPaper, S(22));
    if (hov) dl->AddRect(p, q, rgb(246, 170, 40), S(22), 0, S(3));
    float pad = S(16), r = S(22);
    dl->AddCircleFilled(ImVec2(p.x + pad + r, p.y + pad + r), r, enabled ? color : kGrey);
    theme_icon(dl, icon, ImVec2(p.x + pad + r, p.y + pad + r), r * 0.6f, kWhite);
    float x = p.x + pad + 2 * r + S(12), inner = q.x - pad - x;
    float fs = font_size(g_body);
    dl->AddText(g_body, fs, ImVec2(x, p.y + pad + S(2)), enabled ? kInk : kInkSoft, title, nullptr, inner);
    float ty = measure(g_body, fs, title, inner).y;
    dl->AddText(g_body, fs * 0.82f, ImVec2(x, p.y + pad + S(6) + ty), kInkSoft, desc, nullptr, inner);
    return clicked && enabled;
}

float action_height(const char* title, const char* desc, float w) {
    float pad = S(16), r = S(22), inner = w - 2 * pad - 2 * r - S(12);
    float fs = font_size(g_body);
    return std::max(2 * r, S(8) + measure(g_body, fs, title, inner).y + measure(g_body, fs * 0.82f, desc, inner).y) + 2 * pad;
}

void page_home() {
    const bool valid = *g_build_problem == 0;
    if (!valid) {
        heading(tr("launcher.home.title_missing"),
                trf("launcher.home.missing", tr(g_build_problem)).c_str());
    } else if (!g_banner.empty()) {
        heading("", "");
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    if (valid) {
        hero(nx::ui::kIconHouse, kGreen, S(46));
        centered(tr("launcher.home.title"), g_title, 1.1f, kInk);
        centered(trf("launcher.home.build", g_build.data).c_str(), g_body, 0.84f, kInkSoft);
        ImGui::Dummy(ImVec2(0, S(14)));
        play_block();
    }
    // Settings, Rebuild, Change build folder.
    const bool can_rebuild = valid && setup_supported() &&
                             g_build.exe.rfind(g_build.data, 0) == 0 && g_build.exe.find("build") != std::string::npos;
    struct A {
        const char* id;
        int icon;
        ImU32 color;
        const char* title;
        const char* desc;
        bool enabled;
        int action;
    };
    std::vector<A> acts;
    if (valid) acts.push_back({"##settings", nx::ui::kIconGear, kPurple, tr("launcher.home.settings"),
                               tr("launcher.home.settings.desc"), true, 0});
    if (setup_supported())
        acts.push_back({"##rebuild", nx::ui::kIconLeaf, kOrange, tr("launcher.home.rebuild"),
                        tr(can_rebuild ? "launcher.home.rebuild.desc" : "launcher.home.rebuild.unavailable"), can_rebuild, 1});
    acts.push_back({"##change", nx::ui::kIconFolder, kBlue, tr("launcher.home.change"), tr("launcher.home.change.desc"),
                    true, 2});
    float avail = ImGui::GetContentRegionAvail().x, gap = S(14);
    bool stack = g_portrait || avail < S(700);
    float w = stack ? avail : (avail - gap * float(acts.size() - 1)) / float(acts.size());
    float h = 0;
    for (const A& a : acts) h = std::max(h, action_height(a.title, a.desc, w));
    for (size_t k = 0; k < acts.size(); k++) {
        if (k && !stack) ImGui::SameLine(0, gap);
        const A& a = acts[k];
        if (action_card(a.id, a.icon, a.color, a.title, a.desc, ImVec2(w, h), a.enabled)) {
            if (a.action == 0) load_game_ini(), g_page = Page::Settings;
            if (a.action == 1) start_setup(true);
            if (a.action == 2) pick(kPickBuild);
        }
    }
}

void footer_home() {
    if (button(tr("launcher.home.start_over"), kWhite)) g_banner.clear(), g_page = Page::Welcome;
}

// ---- settings: a few of the game's config.ini keys ------------------------------------------------------------

using Choices = std::vector<std::pair<std::string, std::string>>;

void setting_row(const char* label, const char* desc, const char* key, const char* fallback, const Choices& choices) {
    float avail = ImGui::GetContentRegionAvail().x;
    bool narrow = avail < S(560);
    float ctrl_w = narrow ? avail : std::min(S(360), avail * 0.45f);
    ImGui::PushID(key);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (narrow ? avail : avail - ctrl_w - S(16)));
    ImGui::TextUnformatted(tr(label));
    if (desc) text_small(tr(desc));
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    if (!narrow) ImGui::SameLine(avail - ctrl_w + ImGui::GetStyle().WindowPadding.x * 0.0f);
    std::string cur = g_game_ini.get(key, fallback), shown = cur;
    for (auto& [v, t] : choices)
        if (v == cur) shown = t;
    ImGui::SetNextItemWidth(ctrl_w);
    if (ImGui::BeginCombo("##c", shown.c_str())) {
        for (auto& [v, t] : choices)
            if (ImGui::Selectable(t.c_str(), v == cur) && v != cur) {
                g_game_ini.set(key, v);
                if (!g_game_ini.save()) toast(tr("launcher.settings.save_failed"));
            }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, S(2)));
}

void page_settings() {
    heading(tr("app.title"), tr("launcher.settings.text"));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(kPaper));
    ImGui::BeginChild("##rows", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    setting_row("game.language", "game.language.desc", "game.language", "auto",
                {{"auto", tr("game.language.system")}, {"en-US", "English (US)"}, {"en-GB", "English (UK)"},
                 {"es", "Español (España)"}, {"es-419", "Español (Latinoamérica)"}, {"fr", "Français"},
                 {"fr-CA", "Français (Canada)"}, {"de", "Deutsch"}, {"it", "Italiano"}, {"nl", "Nederlands"},
                 {"ru", "Русский"}, {"ja", "日本語"}, {"ko", "한국어"}, {"zh-CN", "简体中文"}, {"zh-TW", "繁體中文"},
                 {"pt-BR", tr("game.language.ptbr")}});
    if (g_game_ini.get("game.language") == "pt-BR" && !is_dir(g_build.data + "/mods/ptbr/romfs")) {
        ImGui::TextColored(col4(rgb(190, 120, 20)), "%s", tr("launcher.settings.ptbr_missing"));
        ImGui::Separator();
    }
    setting_row("video.display", nullptr, "video.fullscreen", "0",
                {{"0", tr("video.display.windowed")}, {"1", tr("video.display.fullscreen")}});
    setting_row("video.window_size", nullptr, "video.window", "auto",
                {{"auto", tr("common.auto")}, {"1280x720", "1280 × 720"}, {"1600x900", "1600 × 900"},
                 {"1920x1080", "1920 × 1080"}, {"2560x1440", "2560 × 1440"}, {"3840x2160", "3840 × 2160"}});
    setting_row("video.render", nullptr, "video.mode", "docked",
                {{"docked", tr("video.render.docked")}, {"handheld", tr("video.render.handheld")}});
    setting_row("video.upscaler", nullptr, "video.upscaler", "linear",
                {{"linear", tr("video.upscaler.linear")}, {"fsr", tr("video.upscaler.fsr")}});
    setting_row("video.framerate", "video.framerate.desc", "video.frame_rate", "30",
                {{"30", tr("video.framerate.30")}, {"60", tr("video.framerate.60")}});
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void footer_settings() {
    if (button(tr("common.back"), kWhite)) g_page = Page::Home;
    align_right({tr("launcher.settings.open_data")});
    if (button(tr("launcher.settings.open_data"), kWhite)) open_folder(g_build.data);
}

// ---- popups ---------------------------------------------------------------------------------------------------

void popups() {
    if (g_confirm_stop) ImGui::OpenPopup("stop");
    if (g_confirm_quit) ImGui::OpenPopup("quit");
    ImGui::SetNextWindowSize(ImVec2(std::min(ImGui::GetIO().DisplaySize.x * 0.9f, S(560)), 0));
    if (ImGui::BeginPopupModal("stop", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", tr("launcher.build.cancel_confirm"));
        ImGui::Dummy(ImVec2(0, S(6)));
        if (button(tr("launcher.stop"), kRed)) setup_cancel(), g_confirm_stop = false, ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (button(tr("common.no"), kWhite)) g_confirm_stop = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSize(ImVec2(std::min(ImGui::GetIO().DisplaySize.x * 0.9f, S(560)), 0));
    if (ImGui::BeginPopupModal("quit", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", tr("launcher.quit_confirm"));
        ImGui::Dummy(ImVec2(0, S(6)));
        if (button(tr("launcher.stop_quit"), kRed)) {
            setup_cancel();
            g_quit_after_stop = true;
            g_confirm_quit = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button(tr("common.no"), kWhite)) g_confirm_quit = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_quit_after_stop && !setup_state().running) g_exit = true;
}

}  // namespace

// ---- entry points ---------------------------------------------------------------------------------------------

void pages_init(SDL_Window* window) {
    g_window = window;
    Ini& s = settings();
    g_install = s.get("launcher.install", default_install_dir());
    auto load = [](Item& it, Kind kind, const std::string& path) {
        if (is_file(path)) set_item(it, kind, path);
    };
    load(g_keys, Kind::Keys, s.get("files.keys"));
    load(g_base, Kind::Base, s.get("files.base"));
    load(g_update, Kind::Update, s.get("files.update"));
    load(g_ptbr, Kind::Mod, s.get("files.ptbr"));
    std::string dlc = s.get("files.dlc");
    for (size_t a = 0; a < dlc.size();) {
        size_t b = dlc.find('|', a);
        std::string d = dlc.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (is_file(d)) add_dlc(d);
        if (b == std::string::npos) break;
        a = b + 1;
    }
    refresh_keys_auto();
    std::string build = s.get("launcher.build");
    if (!build.empty()) {
        set_build(build, false);
        load_game_ini();
        g_page = Page::Home;
    }
    // Tests: a page directly (main.cpp).
    if (const char* p = std::getenv("NX_LAUNCHER_PAGE")) {
        std::string n = p;
        if (n == "welcome") g_page = Page::Welcome;
        if (n == "files") g_page = Page::Files;
        if (n == "install") g_prereqs = check_prerequisites(g_install), g_page = Page::Install;
        if (n == "build") start_setup(false);
        if (n == "done") g_page = Page::Done;
        if (n == "home") g_page = Page::Home;
        if (n == "settings") g_page = Page::Settings;
    }
}

void pages_drop(const std::string& path) {
    if (g_page == Page::Build) return;
    if (is_dir(path)) {
        if (g_page == Page::Welcome || g_page == Page::Home) use_build_folder(path);
        return;
    }
    Kind k;
    if (!classify_file(path, k)) {
        g_banner = trf("launcher.err.drop", file_name(path));
        return;
    }
    g_banner.clear();
    switch (k) {
        case Kind::Keys: set_item(g_keys, k, path), g_keys_auto = false; break;
        case Kind::Base: set_item(g_base, k, path); break;
        case Kind::Update: set_item(g_update, k, path); break;
        case Kind::Dlc: add_dlc(path); break;
        case Kind::Mod: set_item(g_ptbr, k, path); break;
    }
    settings().set("files.last_dir", parent_dir(path));
    refresh_keys_auto();
    save_files();
    if (g_page != Page::Files && setup_supported()) g_page = Page::Files;
}

void pages_close_requested() {
    if (setup_state().running)
        g_confirm_quit = true;
    else
        g_exit = true;
}

bool pages_busy() { return setup_state().running || ImGui::GetTime() < g_toast_until; }

bool pages_draw(float scale, ImFont* title) {
    g_scale = scale, g_title = title, g_body = ImGui::GetFont();
    apply_picks();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize;
    g_portrait = ds.x < ds.y * 1.2f;
    draw_background(ds);
    ImVec2 size = g_portrait ? ImVec2(ds.x - S(16), ds.y - S(16))
                             : ImVec2(std::min(ds.x - S(32), S(1180)), std::min(ds.y - S(32), S(700)));
    ImVec2 pos((ds.x - size.x) * 0.5f, (ds.y - size.y) * 0.5f);
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(pos.x, pos.y + S(6)), ImVec2(pos.x + size.x, pos.y + size.y + S(6)),
                                                  rgb(60, 110, 70, 60), S(28));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(g_portrait ? 14 : 22), S(g_portrait ? 14 : 20)));
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    ribbon();
    float footer = ImGui::GetFrameHeight() + S(20);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##page", ImVec2(0, -footer), ImGuiChildFlags_None);
    ImGui::PopStyleColor();
    // The short pages sit in the middle of the card: their height from the last frame.
    static float content_h[8] = {};
    const int pi = int(g_page);
    const bool center = g_page == Page::Welcome || g_page == Page::Done || g_page == Page::Home;
    const float avail_h = ImGui::GetContentRegionAvail().y, top = ImGui::GetCursorPosY();
    if (center && content_h[pi] > 0 && content_h[pi] < avail_h)
        ImGui::SetCursorPosY(top + (avail_h - content_h[pi]) * 0.4f);
    const float start = ImGui::GetCursorPosY();
    switch (g_page) {
        case Page::Welcome: page_welcome(); break;
        case Page::Files: page_files(); break;
        case Page::Install: page_install(); break;
        case Page::Build: page_build(); break;
        case Page::Done: page_done(); break;
        case Page::Home: page_home(); break;
        case Page::Settings: page_settings(); break;
    }
    if (int(g_page) == pi) content_h[pi] = ImGui::GetCursorPosY() - start;
    static const char* scroll = std::getenv("NX_LAUNCHER_SCROLL");  // tests: 0..1 of the page's scroll range
    if (scroll) ImGui::SetScrollY(ImGui::GetScrollMaxY() * float(std::atof(scroll)));
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, S(2)));
    if (ImGui::GetTime() < g_toast_until) {  // a short confirmation, in place of the footer's own text
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(col4(rgb(70, 140, 100)), "%s", g_toast.c_str());
        ImGui::SameLine();
    }
    switch (g_page) {
        case Page::Welcome: footer_welcome(); break;
        case Page::Files: footer_files(); break;
        case Page::Install: footer_install(); break;
        case Page::Build: footer_build(); break;
        case Page::Done: footer_done(); break;
        case Page::Home: footer_home(); break;
        case Page::Settings: footer_settings(); break;
    }
    popups();
    ImGui::End();
    return !g_exit;
}

}  // namespace launcher
