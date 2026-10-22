// <data root>/config.ini: see config.h.
#include "config.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace {

std::mutex g_lock;
std::vector<ConfigEntry> g_entries;  // file order
std::atomic<uint64_t> g_generation{1};
bool g_loaded = false;

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string path() { return plat::data_root() + "/config.ini"; }

ConfigEntry* find(const std::string& key) {
    for (auto& e : g_entries)
        if (e.key == key) return &e;
    return nullptr;
}

void set_env_default(const char* name, const std::string& value) {
    if (std::getenv(name)) return;  // set by hand or by a preset: it wins
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 0);
#endif
}

// Settings that become a runtime switch. `map` turns the setting's value into the variable's ("" = leave unset).
struct Mapping {
    const char* key;
    const char* env;
    std::string (*map)(const std::string&);
    bool restart;
};

std::string same(const std::string& v) { return v; }
std::string on_off(const std::string& v) { return v == "1" || v == "on" || v == "true" ? "1" : ""; }
std::string off_is_zero(const std::string& v) { return v == "0" || v == "off" || v == "false" ? "0" : ""; }
std::string auto_empty(const std::string& v) { return v == "auto" ? "" : v; }

// pt-BR is not a console language: the fan translation replaces the European Spanish files, so the console
// language becomes "es" and the translation's files are layered over the RomFS (hle_fs.cpp, game.mods).
std::string game_language(const std::string& v) {
    if (v.empty() || v == "auto") return "";
    if (v == "pt-BR") return "es";
    return v;
}
std::string handheld(const std::string& v) { return v == "handheld" ? "1" : ""; }
std::string fsr(const std::string& v) { return v == "fsr" ? "1" : ""; }
std::string fps_cap(const std::string& v) { return v == "30" ? "30" : ""; }
std::string interval(const std::string& v) { return v == "60" ? "1" : ""; }
// The screen's 0..100% sharpness -> FSR's RCAS attenuation in stops (0 sharpest, 2 softest).
std::string fsr_sharpness(const std::string& v) {
    if (v.empty()) return "";
    char b[16];
    std::snprintf(b, sizeof b, "%.2f", (100 - std::clamp(std::atoi(v.c_str()), 0, 100)) / 50.0);
    return b;
}

const Mapping kMappings[] = {
    {"game.language", "NX_LANGUAGE", game_language, true},
    {"video.fullscreen", "NX_FULLSCREEN", on_off, false},
    {"video.window", "NX_RESOLUTION", auto_empty, true},
    {"video.mode", "NX_HANDHELD", handheld, true},
    {"video.upscaler", "NX_FSR", fsr, true},
    {"video.fsr_sharpness_pct", "NX_FSR_SHARP", fsr_sharpness, true},
    {"video.aspect", "NX_ULTRAWIDE", auto_empty, true},
    {"video.gpu", "NX_GPU", auto_empty, true},
    {"video.frame_rate", "NX_PRESENT_INTERVAL", interval, true},
    {"video.frame_cap", "NX_FPS_CAP", fps_cap, true},
    {"audio.reverb", "NX_AUDIO_REVERB", off_is_zero, false},  // also read live (hle_audio.cpp)
};

// "auto" (or no setting): the system's locale picks among the game's languages; Portuguese gets the pt-BR fan
// translation when it is installed (the game itself has no Portuguese), else English.
std::string system_game_language() {
    const char* vars[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    std::string loc;
    for (const char* v : vars)
        if (const char* e = std::getenv(v); e && *e && std::string(e) != "C" && std::string(e) != "POSIX") {
            loc = e;
            break;
        }
#if defined(__APPLE__)
    if (loc.empty()) loc = "en_US";
#endif
    std::string lang = loc.substr(0, loc.find_first_of("_-.@")), region;
    size_t us = loc.find_first_of("_-");
    if (us != std::string::npos) region = loc.substr(us + 1, 2);
    for (char& c : lang) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (lang == "pt") {
        std::error_code ec;
        bool ptbr = std::filesystem::is_directory(std::filesystem::u8path(plat::data_root() + "/mods/ptbr/romfs"), ec);
        return ptbr ? "pt-BR" : "en-US";
    }
    if (lang == "en") return region == "US" || region.empty() ? "en-US" : "en-GB";
    if (lang == "fr") return region == "CA" ? "fr-CA" : "fr";
    if (lang == "es") return region == "ES" || region.empty() ? "es" : "es-419";
    if (lang == "zh") return region == "TW" || region == "HK" || region == "MO" ? "zh-TW" : "zh-CN";
    for (const char* l : {"de", "it", "nl", "ru", "ja", "ko"})
        if (lang == l) return l;
    return "en-US";
}

}  // namespace

void config_load() {
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard lk(g_lock);
    g_entries.clear();
    g_loaded = true;
    FILE* f = plat::fopen_utf8(path(), "rb");
    if (!f) return;
    std::string section, line;
    char buf[1024];
    while (std::fgets(buf, sizeof buf, f)) {
        line = trim(buf);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (!section.empty() && k.find('.') == std::string::npos) k = section + "." + k;
        if (ConfigEntry* e = find(k))
            e->value = v;
        else
            g_entries.push_back({k, v});
    }
    std::fclose(f);
}

void config_apply() {
    if (!g_loaded) config_load();
    // Game language "auto": from the system, unless a language file or NX_LANGUAGE says otherwise. Only applied:
    // the entry keeps "auto", so the choice stays automatic.
    std::string game_lang = config_get("game.language", "auto");
    {
        std::error_code ec;
        bool file = std::filesystem::exists(std::filesystem::u8path(plat::data_root() + "/language.txt"), ec);
        if ((game_lang.empty() || game_lang == "auto") && !file && !std::getenv("NX_LANGUAGE")) {
            game_lang = system_game_language();
            log("config: game language from the system: %s", game_lang.c_str());
        }
    }
    std::lock_guard lk(g_lock);
    for (const Mapping& m : kMappings) {
        ConfigEntry* e = find(m.key);
        std::string value = std::string(m.key) == "game.language" ? game_lang : e ? e->value : std::string();
        if (value.empty()) continue;
        std::string v = m.map(value);
        if (!v.empty()) set_env_default(m.env, v);
    }
    // The pt-BR translation is a mod: enable it with the language (game.mods can list more, comma separated).
    std::string mods;
    if (ConfigEntry* e = find("game.mods")) mods = e->value;
    if (game_lang == "pt-BR")
        mods = mods.empty() ? "ptbr" : (mods.find("ptbr") == std::string::npos ? mods + ",ptbr" : mods);
    if (!mods.empty()) set_env_default("NX_MODS", mods);
    log("config: %zu settings from %s", g_entries.size(), path().c_str());
}

bool config_save() {
    std::lock_guard lk(g_lock);
    // Grouped by section, sections in first-seen order.
    std::vector<std::string> sections;
    for (const auto& e : g_entries) {
        std::string s = e.key.substr(0, e.key.find('.'));
        if (std::find(sections.begin(), sections.end(), s) == sections.end()) sections.push_back(s);
    }
    std::string tmp = path() + ".tmp";
    FILE* f = plat::fopen_utf8(tmp, "wb");
    if (!f) return false;
    std::fprintf(f, "# ACNHRecomp settings (edited by the settings screen; F1 or Minus+Plus in game)\n");
    for (const auto& s : sections) {
        std::fprintf(f, "\n[%s]\n", s.c_str());
        for (const auto& e : g_entries)
            if (e.key.compare(0, s.size() + 1, s + ".") == 0)
                std::fprintf(f, "%s = %s\n", e.key.substr(s.size() + 1).c_str(), e.value.c_str());
    }
    bool ok = std::fclose(f) == 0;
    if (ok) ok = std::rename(tmp.c_str(), path().c_str()) == 0;
    return ok;
}

std::string config_get(const std::string& key, const std::string& fallback) {
    if (!g_loaded) config_load();
    std::lock_guard lk(g_lock);
    ConfigEntry* e = find(key);
    return e ? e->value : fallback;
}

int config_get_int(const std::string& key, int fallback) {
    std::string v = config_get(key);
    return v.empty() ? fallback : std::atoi(v.c_str());
}

float config_get_float(const std::string& key, float fallback) {
    std::string v = config_get(key);
    return v.empty() ? fallback : float(std::atof(v.c_str()));
}

bool config_get_bool(const std::string& key, bool fallback) {
    std::string v = config_get(key);
    if (v.empty()) return fallback;
    return v == "1" || v == "on" || v == "true" || v == "yes";
}

uint64_t config_generation() { return g_generation.load(std::memory_order_acquire); }

void config_set(const std::string& key, const std::string& value) {
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard lk(g_lock);
    if (ConfigEntry* e = find(key))
        e->value = value;
    else
        g_entries.push_back({key, value});
}

std::vector<ConfigEntry> config_all() {
    std::lock_guard lk(g_lock);
    return g_entries;
}

bool config_needs_restart(const std::string& key) {
    for (const Mapping& m : kMappings)
        if (key == m.key) return m.restart;
    return key == "game.mods" || key == "controls.glyphs" || key == "input.layout";  // icons: read at boot
}

}  // namespace nx
