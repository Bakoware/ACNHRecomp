// Paths, launcher.ini, the checks of the player's files and build folders, and the prerequisites (launcher.h).
#include "launcher.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#undef near  // windows.h's 16-bit leftovers: "near" names a list of paths here
#undef far
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

#include "ui/i18n.h"

namespace launcher {

namespace fs = std::filesystem;
using nx::ui::tr;
using nx::ui::trf;

namespace {

// UTF-8 strings <-> paths on every OS (Windows paths are UTF-16).
fs::path u8(const std::string& p) { return fs::path(reinterpret_cast<const char8_t*>(p.c_str())); }
std::string str(const fs::path& p) {
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::string home() {
#if defined(_WIN32)
    return env("USERPROFILE");
#else
    return env("HOME");
#endif
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ext(const std::string& path) { return lower(str(u8(path).extension())); }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool read_at(std::ifstream& f, uint64_t off, void* buf, size_t size) {
    f.clear();
    f.seekg(std::streamoff(off));
    f.read(static_cast<char*>(buf), std::streamsize(size));
    return size_t(f.gcount()) == size;
}

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; }

// ---- NSP metadata ---------------------------------------------------------------------------------------------

constexpr uint64_t kBaseTitle = 0x01006F8002326000ull;
constexpr uint64_t kUpdateTitle = kBaseTitle + 0x800;
constexpr uint32_t kVersion302 = 2162688;  // 3.0.2's title version (release 33 << 16)
constexpr uint32_t kVersion303 = 2228224;  // 3.0.3 (release 34): 3.0.2 a little moved, every fix ported

// An update's version as players know it, where the number is known (games/acnh/versions.txt lists the builds).
std::string version_name(uint32_t v) {
    if (v == kVersion302) return "3.0.2";
    if (v == kVersion303) return "3.0.3";
    if (v == 29u << 16) return "2.0.7";
    return "#" + std::to_string(v >> 16);
}

bool is_dlc_title(uint64_t id) { return (id & ~0xFFFull) == kBaseTitle + 0x1000 && (id & 0xFFF); }

struct Meta {
    bool pfs0 = false, xci = false;
    uint64_t title = 0;
    bool has_version = false;
    uint32_t version = 0;
};

std::string xml_tag(const std::string& xml, const char* tag) {
    std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
    size_t a = xml.find(open);
    if (a == std::string::npos) return "";
    a += open.size();
    size_t b = xml.find(close, a);
    return b == std::string::npos ? "" : trim(xml.substr(a, b - a));
}

// PFS0: "PFS0", file count, string table size; 0x18-byte entries {offset, size, name offset}; data after the table.
// The title comes from <name>.cnmt.xml when the dump kept it, else from the ticket ("<rights id>.tik", whose first
// 16 hex digits are the title).
Meta read_meta(const std::string& path) {
    Meta m;
    std::ifstream f(u8(path), std::ios::binary);
    uint8_t h[16];
    if (!f || !read_at(f, 0, h, sizeof h)) return m;
    if (std::memcmp(h, "PFS0", 4) != 0) {
        uint8_t head[4];
        m.xci = read_at(f, 0x100, head, 4) && std::memcmp(head, "HEAD", 4) == 0;
        return m;
    }
    uint32_t count = rd32(h + 4), strings = rd32(h + 8);
    if (count == 0 || count > 4096 || strings > (1u << 20)) return m;
    m.pfs0 = true;
    std::vector<uint8_t> table(size_t(count) * 0x18 + strings);
    if (!read_at(f, 16, table.data(), table.size())) return m;
    const uint64_t data = 16 + table.size();
    const char* names = reinterpret_cast<const char*>(table.data() + size_t(count) * 0x18);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = table.data() + size_t(i) * 0x18;
        uint64_t off = rd64(e), size = rd64(e + 8);
        uint32_t name_off = rd32(e + 16);
        if (name_off >= strings) continue;
        std::string name(names + name_off, strnlen(names + name_off, strings - name_off));
        if (name.size() > 9 && name.compare(name.size() - 9, 9, ".cnmt.xml") == 0 && size < (1u << 16)) {
            std::string xml(size_t(size), '\0');
            if (!read_at(f, data + off, xml.data(), xml.size())) continue;
            std::string id = xml_tag(xml, "Id"), ver = xml_tag(xml, "Version");
            if (id.size() > 2) m.title = std::strtoull(id.c_str() + 2, nullptr, 16);
            if (!ver.empty()) m.has_version = true, m.version = uint32_t(std::strtoul(ver.c_str(), nullptr, 10));
            return m;
        }
        if (name.size() == 36 && name.compare(32, 4, ".tik") == 0 && !m.title)
            m.title = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
    }
    return m;
}

std::string title_hex(uint64_t id) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llX", static_cast<unsigned long long>(id));
    return b;
}

uint64_t file_size(const std::string& path) {
    std::error_code ec;
    uint64_t n = fs::file_size(u8(path), ec);
    return ec ? 0 : n;
}

FileCheck ok(Status s, std::string text) { return {s, std::move(text)}; }

}  // namespace

// ---- Ini ------------------------------------------------------------------------------------------------------

void Ini::load(const std::string& path) {
    path_ = path;
    entries_.clear();
    std::ifstream f(u8(path));
    std::string line, section;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (!section.empty() && k.find('.') == std::string::npos) k = section + "." + k;
        set(k, v);
    }
}

bool Ini::save() const {
    std::error_code ec;
    fs::create_directories(u8(parent_dir(path_)), ec);
    std::vector<std::string> sections;
    for (const auto& [k, v] : entries_) {
        std::string s = k.substr(0, k.find('.'));
        if (std::find(sections.begin(), sections.end(), s) == sections.end()) sections.push_back(s);
    }
    std::ofstream f(u8(path_ + ".tmp"), std::ios::trunc);
    if (!f) return false;
    f << "# ACNHRecomp settings\n";
    for (const auto& s : sections) {
        f << "\n[" << s << "]\n";
        for (const auto& [k, v] : entries_)
            if (k.compare(0, s.size() + 1, s + ".") == 0) f << k.substr(s.size() + 1) << " = " << v << "\n";
    }
    f.close();
    if (!f) return false;
    fs::rename(u8(path_ + ".tmp"), u8(path_), ec);
    return !ec;
}

std::string Ini::get(const std::string& key, const std::string& fallback) const {
    for (const auto& [k, v] : entries_)
        if (k == key) return v;
    return fallback;
}

void Ini::set(const std::string& key, const std::string& value) {
    for (auto& [k, v] : entries_)
        if (k == key) {
            v = value;
            return;
        }
    entries_.emplace_back(key, value);
}

Ini& settings() {
    static Ini ini;
    return ini;
}

// ---- paths ----------------------------------------------------------------------------------------------------

std::string launcher_ini_path() {
    if (std::string p = env("NX_LAUNCHER_INI"); !p.empty()) return p;  // tests
    std::string dir;
    if (char* pref = SDL_GetPrefPath("", "ACNHRecomp")) {
        dir = pref;
        SDL_free(pref);
    }
    if (dir.empty()) dir = home() + "/.ACNHRecomp/";
    return dir + "launcher.ini";
}

std::string default_install_dir() {
#if defined(_WIN32)
    std::string base = env("LOCALAPPDATA");
    return (base.empty() ? home() + "\\AppData\\Local" : base) + "\\ACNHRecomp";
#elif defined(__APPLE__)
    return home() + "/Library/Application Support/ACNHRecomp";
#else
    std::string base = env("XDG_DATA_HOME");
    return (base.empty() ? home() + "/.local/share" : base) + "/ACNHRecomp";
#endif
}

std::string repo_root() {
    std::vector<std::string> starts;
    if (const char* base = SDL_GetBasePath()) starts.push_back(base);  // the launcher's own folder
#if defined(NX_LAUNCHER_SOURCE_DIR)
    starts.push_back(NX_LAUNCHER_SOURCE_DIR);
#endif
    std::error_code ec;
    for (const std::string& s : starts)
        for (fs::path p = u8(s); !p.empty(); p = p.parent_path()) {
            if (fs::is_regular_file(p / "tools" / "setup_acnh.py", ec)) return str(p);
            if (p == p.parent_path()) break;
        }
    return "";
}

std::string human_size(uint64_t bytes) {
    char b[32];
    if (bytes >= 1000000000ull)
        std::snprintf(b, sizeof b, "%.1f GB", double(bytes) / 1e9);
    else if (bytes >= 1000000ull)
        std::snprintf(b, sizeof b, "%.0f MB", double(bytes) / 1e6);
    else
        std::snprintf(b, sizeof b, "%.0f KB", double(bytes) / 1e3);
    return b;
}

uint64_t free_space(const std::string& dir) {
    std::error_code ec;
    for (fs::path p = u8(dir); !p.empty(); p = p.parent_path()) {
        if (fs::exists(p, ec)) {
            fs::space_info s = fs::space(p, ec);
            return ec ? 0 : s.available;
        }
        if (p == p.parent_path()) break;
    }
    return 0;
}

std::string parent_dir(const std::string& path) { return str(u8(path).parent_path()); }
std::string file_name(const std::string& path) { return str(u8(path).filename()); }

bool is_file(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(u8(path), ec);
}

bool is_dir(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::is_directory(u8(path), ec);
}

// ---- the player's files ---------------------------------------------------------------------------------------

FileCheck check_file(Kind kind, const std::string& path) {
    if (path.empty()) return {};
    std::ifstream probe(u8(path), std::ios::binary);
    if (!probe) return ok(Status::Error, tr("launcher.err.unreadable"));
    probe.close();
    const uint64_t size = file_size(path);
    const std::string sz = human_size(size), e = ext(path);
    if (kind == Kind::Keys) {
        std::ifstream f(u8(path));
        std::string line;
        bool header = false;
        for (int n = 0; n < 4000 && std::getline(f, line); n++)
            if (trim(line).rfind("header_key", 0) == 0) header = true;
        return header ? ok(Status::Ok, tr("launcher.ok.keys")) : ok(Status::Error, tr("launcher.err.keys"));
    }
    if (kind == Kind::TitleKeys) {  // "<rights id> = <title key>", 32 hex digits each
        std::ifstream f(u8(path));
        std::string line;
        auto hex32 = [](const std::string& v) {
            return v.size() == 32 && std::all_of(v.begin(), v.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
        };
        for (int n = 0; n < 50 && std::getline(f, line); n++) {
            size_t eq = line.find('=');
            if (trim(line).empty()) continue;
            if (eq != std::string::npos && hex32(trim(line.substr(0, eq))) && hex32(trim(line.substr(eq + 1))))
                return ok(Status::Ok, tr("launcher.ok.title_keys"));
            break;
        }
        return ok(Status::Error, tr("launcher.err.title_keys"));
    }
    if (kind == Kind::Mod) {
        std::ifstream f(u8(path), std::ios::binary);
        char m[4] = {};
        f.read(m, 4);
        return std::memcmp(m, "PK\x03\x04", 4) == 0 ? ok(Status::Ok, trf("launcher.ok.zip", sz))
                                                    : ok(Status::Error, tr("launcher.err.zip"));
    }
    if (e == ".nsz" || e == ".xcz") return ok(Status::Error, tr("launcher.err.compressed"));
    Meta m = read_meta(path);
    if (m.xci) {
        if (kind == Kind::Base) return ok(Status::Warning, trf("launcher.ok.xci", sz));
        return ok(Status::Error, tr("launcher.err.xci_not_base"));
    }
    if (!m.pfs0) return ok(Status::Error, tr("launcher.err.not_dump"));
    if (!m.title) return ok(Status::Warning, trf("launcher.ok.unverified", sz));
    const bool base = m.title == kBaseTitle, update = m.title == kUpdateTitle, dlc = is_dlc_title(m.title);
    if (!base && !update && !dlc) return ok(Status::Error, trf("launcher.err.other_game", title_hex(m.title)));
    switch (kind) {
        case Kind::Base:
            if (update) return ok(Status::Error, tr("launcher.err.is_update"));
            if (dlc) return ok(Status::Error, tr("launcher.err.is_dlc"));
            return ok(Status::Ok, trf("launcher.ok.base", sz));
        case Kind::Update:
            if (base) return ok(Status::Error, tr("launcher.err.is_base"));
            if (dlc) return ok(Status::Error, tr("launcher.err.is_dlc"));
            if (!m.has_version) return ok(Status::Warning, trf("launcher.warn.update_version", sz));
            // Any version builds (docs/VERSIONS.md); only 3.0.2 and 3.0.3 get the fixes that sit at their addresses.
            if (m.version != kVersion302 && m.version != kVersion303) return ok(Status::Warning, trf("launcher.ok.update_other", version_name(m.version)));
            return ok(Status::Ok, trf("launcher.ok.update", sz));
        default:
            if (base) return ok(Status::Error, tr("launcher.err.is_base"));
            if (update) return ok(Status::Error, tr("launcher.err.is_update"));
            return ok(Status::Ok, trf("launcher.ok.dlc", sz));
    }
}

bool classify_file(const std::string& path, Kind& kind) {
    std::string e = ext(path), name = lower(file_name(path));
    if (name == "title.keys") return kind = Kind::TitleKeys, true;
    if (e == ".keys" || e == ".txt" || e == ".prod") return kind = Kind::Keys, true;
    if (e == ".zip") return kind = Kind::Mod, true;
    if (e != ".nsp" && e != ".xci" && e != ".nsz" && e != ".xcz") return false;
    Meta m = read_meta(path);
    kind = m.title == kUpdateTitle ? Kind::Update : is_dlc_title(m.title) ? Kind::Dlc : Kind::Base;
    return true;
}

namespace {

// Where prod.keys usually is, the first places first.
std::vector<fs::path> key_places(const std::vector<std::string>& near) {
    std::vector<fs::path> c;
    const fs::path h = u8(home());
    c.push_back(h / ".switch" / "prod.keys");
    const char* yuzus[] = {"yuzu", "suyu", "sudachi", "citron", "eden", "torzu"};
#if defined(_WIN32)
    fs::path app = u8(env("APPDATA"));
    for (const char* y : yuzus) c.push_back(app / y / "keys" / "prod.keys");
    c.push_back(app / "Ryujinx" / "system" / "prod.keys");
#elif defined(__APPLE__)
    fs::path app = h / "Library" / "Application Support";
    for (const char* y : yuzus) c.push_back(app / y / "keys" / "prod.keys");
    c.push_back(app / "Ryujinx" / "system" / "prod.keys");
#else
    std::string xd = env("XDG_DATA_HOME"), xc = env("XDG_CONFIG_HOME");
    fs::path data = xd.empty() ? h / ".local" / "share" : u8(xd), config = xc.empty() ? h / ".config" : u8(xc);
    for (const char* y : yuzus) c.push_back(data / y / "keys" / "prod.keys");
    c.push_back(config / "Ryujinx" / "system" / "prod.keys");
    // Flatpak emulators keep theirs under ~/.var/app/<app id>/{data,config}/.
    std::error_code dir_ec;
    for (const auto& a : fs::directory_iterator(h / ".var" / "app", dir_ec)) {
        for (const char* y : yuzus) c.push_back(a.path() / "data" / y / "keys" / "prod.keys");
        c.push_back(a.path() / "config" / "Ryujinx" / "system" / "prod.keys");
    }
#endif
    for (const std::string& n : near) {
        if (n.empty()) continue;
        fs::path d = u8(n).parent_path();
        c.push_back(d / "prod.keys");
        c.push_back(d.parent_path() / "prod.keys");
    }
    return c;
}

}  // namespace

std::string find_keys(const std::vector<std::string>& near) {
    std::error_code ec;
    for (const fs::path& p : key_places(near))
        if (fs::is_regular_file(p, ec)) return str(p);
    return "";
}

std::string title_keys_for(const std::string& prod_keys) {
    if (prod_keys.empty()) return "";
    fs::path t = u8(prod_keys).parent_path() / "title.keys";
    std::error_code ec;
    return fs::is_regular_file(t, ec) ? str(t) : std::string();
}

std::string short_dir(const std::string& path) {
    std::string d = parent_dir(path), h = home();
    if (!h.empty() && d.compare(0, h.size(), h) == 0 && (d.size() == h.size() || d[h.size()] == '/' || d[h.size()] == '\\'))
        return "~" + d.substr(h.size());
    return d;
}

namespace {

// Whether a file's name makes it worth checking as `kind` (the pt-BR package: a .zip named like a translation).
bool search_match(Kind kind, const std::string& path) {
    std::string name = lower(file_name(path)), e = ext(path);
    switch (kind) {
        case Kind::Keys: return e == ".keys" && name != "title.keys";
        case Kind::TitleKeys: return name == "title.keys";
        case Kind::Mod:
            if (e != ".zip") return false;
            for (const char* w : {"ptbr", "pt-br", "pt_br", "pt br", "tradu", "portug", "brasil", "brazil"})
                if (name.find(w) != std::string::npos) return true;
            return false;
        default: {
            if (e != ".nsp" && e != ".xci" && e != ".nsz" && e != ".xcz") return false;
            Kind k;
            return classify_file(path, k) && k == kind;
        }
    }
}

}  // namespace

std::vector<Found> search_files(Kind kind, const std::vector<std::string>& near) {
    // {folder, how deep}: a few levels under the user's folders (Downloads/ACNH/...), only the top of the home folder.
    std::vector<std::pair<fs::path, int>> roots;
    std::error_code ec;
    auto root = [&](const fs::path& p, int depth) {
        if (p.empty() || !fs::is_directory(p, ec)) return;
        for (const auto& [q, d] : roots)
            if (fs::equivalent(p, q, ec)) return;
        roots.emplace_back(p, depth);
    };
    for (SDL_Folder f : {SDL_FOLDER_DOWNLOADS, SDL_FOLDER_DESKTOP, SDL_FOLDER_DOCUMENTS})
        if (const char* p = SDL_GetUserFolder(f)) root(u8(p), 3);
    const fs::path h = u8(home());
    for (const char* n : {"Downloads", "Desktop", "Documents"}) root(h / n, 3);
    std::vector<std::string> near_files;
    for (const std::string& n : near) {
        if (is_file(n)) near_files.push_back(n);
        root(is_dir(n) ? u8(n) : u8(n).parent_path(), 2);
    }
    root(h, 1);
    std::vector<fs::path> files;
    if (kind == Kind::Keys || kind == Kind::TitleKeys)  // the emulators' folders too
        for (const fs::path& p : key_places(near_files))
            files.push_back(kind == Kind::Keys ? p : p.parent_path() / "title.keys");
    for (const auto& [r, depth] : roots) {
        int budget = 20000;  // entries per folder: a huge Downloads folder costs a moment, never a hang
        for (auto it = fs::recursive_directory_iterator(r, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator() && budget-- > 0; it.increment(ec)) {
            if (ec) break;
            const fs::path& p = it->path();
            if (it->is_directory(ec)) {
                std::string n = str(p.filename());
                if (it.depth() + 1 >= depth || n.empty() || n[0] == '.' || n == "node_modules") it.disable_recursion_pending();
                continue;
            }
            files.push_back(p);
        }
    }
    std::vector<Found> out;
    std::vector<fs::path> seen;
    for (const fs::path& p : files) {
        std::string s = str(p);
        if (!fs::is_regular_file(p, ec) || !search_match(kind, s)) continue;
        fs::path c = fs::weakly_canonical(p, ec);
        if (std::find(seen.begin(), seen.end(), c) != seen.end()) continue;
        seen.push_back(c);
        out.push_back({s, kind, file_size(s), check_file(kind, s)});
        if (out.size() >= 40) break;
    }
    // The usable ones first, then by name.
    std::stable_sort(out.begin(), out.end(), [](const Found& a, const Found& b) {
        if (a.check.status != b.check.status) return int(a.check.status) < int(b.check.status);
        return lower(file_name(a.path)) < lower(file_name(b.path));
    });
    return out;
}

// ---- builds ---------------------------------------------------------------------------------------------------

BuildContents build_contents(const std::string& data) {
    BuildContents c;
    // version.ini (tools/acnh_version.py): "version = 2.0.7"
    std::ifstream ini(u8(data + "/version.ini"));
    for (std::string l; std::getline(ini, l);) {
        size_t eq = l.find('=');
        if (eq != std::string::npos && trim(l.substr(0, eq)) == "version") c.version = trim(l.substr(eq + 1));
    }
    // The add-on content the importer wrote: aoc/<index>/ (runtime/src/hle_fs.cpp mounts the same folders).
    std::error_code ec;
    for (auto& e : fs::directory_iterator(u8(data + "/aoc"), ec)) {
        if (!e.is_directory(ec)) continue;
        std::string name = str(e.path().filename());
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
        AddOn a;
        a.index = std::atoi(name.c_str());
        for (auto& f : fs::recursive_directory_iterator(e.path(), ec))
            if (f.is_regular_file(ec)) a.files++, a.bytes += f.file_size(ec);
        c.addons.push_back(a);
    }
    std::sort(c.addons.begin(), c.addons.end(), [](const AddOn& x, const AddOn& y) { return x.index < y.index; });
    return c;
}

const char* find_build(const std::string& folder, Build& out) {
#if defined(_WIN32)
    const char* exe = "acnh.exe";
#else
    const char* exe = "acnh";
#endif
    const fs::path f = u8(folder);
    // {executable, data root} in the layouts a build can have.
    const std::pair<fs::path, fs::path> layouts[] = {
        {f / "build" / exe, f},                                     // the launcher's install folder, private/acnh
        {f / "private" / "acnh" / "build" / exe, f / "private" / "acnh"},  // the repository
        {f / exe, f.parent_path()},                                 // its build/ folder
        {f / exe, f / "data"},                                      // a portable folder
    };
    std::error_code ec;
    bool any_exe = false, any_data = false;
    for (const auto& [x, d] : layouts) {
        bool has_exe = fs::is_regular_file(x, ec);
        bool has_data = fs::is_regular_file(d / "analysis" / "main.bin", ec) && fs::is_directory(d / "extract" / "romfs", ec);
        any_exe |= has_exe, any_data |= has_data;
        if (has_exe && has_data) {
            out = {str(fs::absolute(x, ec)), str(fs::absolute(d, ec))};
            return "";
        }
    }
    return any_data ? "launcher.reason.no_exe" : any_exe ? "launcher.reason.no_data" : "launcher.reason.none";
}

// ---- this computer --------------------------------------------------------------------------------------------

namespace {

// The first folder of PATH holding tool, "" if none. Microsoft Store's python.exe alias (WindowsApps) is not one: it
// has no Python behind it, and started it opens the Store. (The py.exe there is the Python install manager's: real.)
std::string path_of(const char* tool) {
    std::string path = env("PATH");
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::stringstream ss(path);
    std::string dir;
    std::error_code ec;
    while (std::getline(ss, dir, sep)) {
        if (dir.empty()) continue;
        if (lower(tool).rfind("python", 0) == 0 && lower(dir).find("\\windowsapps") != std::string::npos) continue;
        if (fs::is_regular_file(u8(dir) / tool, ec)) return str(u8(dir) / tool);
    }
    return "";
}

bool in_path(const char* tool) { return !path_of(tool).empty(); }

bool sdl3_dev_files() {
    std::vector<fs::path> c = {"/usr/lib/cmake/SDL3", "/usr/lib64/cmake/SDL3", "/usr/local/lib/cmake/SDL3",
                               "/usr/local/lib64/cmake/SDL3", "/usr/share/cmake/SDL3", "/opt/homebrew/lib/cmake/SDL3",
                               "/usr/local/opt/sdl3/lib/cmake/SDL3"};
    std::error_code ec;
    for (const auto& d : fs::directory_iterator("/usr/lib", ec))  // multiarch: /usr/lib/x86_64-linux-gnu/cmake/SDL3
        c.push_back(d.path() / "cmake" / "SDL3");
    std::stringstream ss(env("CMAKE_PREFIX_PATH"));
    std::string p;
    while (std::getline(ss, p, ':'))
        if (!p.empty()) c.push_back(u8(p) / "lib" / "cmake" / "SDL3");
    for (const auto& d : c)
        if (fs::is_regular_file(d / "SDL3Config.cmake", ec)) return true;
    return false;
}

#if defined(_WIN32)
// The Visual Studio C++ build tools (Windows SDK, C runtime) the setup's clang-cl links against: vswhere lists them.
bool msvc_tools() {
    std::string pf = env("ProgramFiles(x86)");
    fs::path vswhere = u8(pf) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    std::error_code ec;
    if (!fs::is_regular_file(vswhere, ec)) return false;
    std::string cmd = "\"\"" + str(vswhere) + "\" -latest -products * -requires "
                      "Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath\"";
    FILE* p = _popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[512];
    std::string out;
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    _pclose(p);
    return !trim(out).empty();
}

// PATH as Windows has it now (the machine's and the user's, from the registry): a program installed while the launcher
// is open (winget) is found by "Check again" and by the setup it starts, without restarting the launcher. Entries only
// this process has (from whoever started it) stay, after them.
void refresh_path() {
    auto read = [](HKEY root, const wchar_t* key) -> std::wstring {
        const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
        DWORD type = 0, size = 0;
        if (RegGetValueW(root, key, L"Path", flags, &type, nullptr, &size) != ERROR_SUCCESS || size < 2) return L"";
        std::wstring v(size / sizeof(wchar_t), L'\0');
        if (RegGetValueW(root, key, L"Path", flags, &type, v.data(), &size) != ERROR_SUCCESS) return L"";
        v.resize(wcslen(v.c_str()));
        if (type == REG_EXPAND_SZ) {
            std::wstring out(32768, L'\0');
            DWORD n = ExpandEnvironmentStringsW(v.c_str(), out.data(), DWORD(out.size()));
            if (n && n <= out.size()) v = out.c_str();
        }
        return v;
    };
    std::wstring merged = read(HKEY_LOCAL_MACHINE, LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)");
    std::wstring user = read(HKEY_CURRENT_USER, L"Environment");
    if (merged.empty() && user.empty()) return;
    if (!user.empty()) merged += (merged.empty() ? L"" : L";") + user;
    auto key = [](std::wstring s) {
        while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
        for (wchar_t& c : s) c = wchar_t(towlower(c));
        return s;
    };
    std::vector<std::wstring> have;
    {
        std::wstringstream ss(merged);
        for (std::wstring e; std::getline(ss, e, L';');)
            if (!e.empty()) have.push_back(key(e));
    }
    const wchar_t* cur = _wgetenv(L"PATH");
    std::wstringstream ss(std::wstring(cur ? cur : L""));
    for (std::wstring e; std::getline(ss, e, L';');)
        if (!e.empty() && std::find(have.begin(), have.end(), key(e)) == have.end())
            merged += L";" + e, have.push_back(key(e));
    _wputenv_s(L"PATH", merged.c_str());
}

std::wstring wide_of(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string utf8_of(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Runs a program directly (no shell, no console window) and returns what it printed, "" if it did not start or did
// not finish within timeout_ms. No cmd.exe in between: no quoting or %-expansion surprises, any path works.
std::string run_output(const std::vector<std::string>& args, DWORD timeout_ms = 20000) {
    std::wstring line;
    for (const std::string& a : args) line += (line.empty() ? L"\"" : L" \"") + wide_of(a) + L"\"";
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return "";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    std::string out;
    if (ok) {
        // Read while it runs (a full pipe would block it), until it ends or the time is up.
        const ULONGLONG until = GetTickCount64() + timeout_ms;
        for (;;) {
            DWORD avail = 0;
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
                char buf[512];
                DWORD got = 0;
                if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || !got) break;
                out.append(buf, got);
            }
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
                while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
                    char buf[512];
                    DWORD got = 0;
                    if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || !got) break;
                    out.append(buf, got);
                }
                break;
            }
            if (GetTickCount64() > until) {
                TerminateProcess(pi.hProcess, 1);
                out.clear();
                break;
            }
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(rd);
    return out;
}

// Every folder of PATH holding tool. Microsoft Store's python.exe alias (WindowsApps) is not taken: it has no Python
// behind it, and started it opens the Store. (The py.exe there is the Python install manager's: real.)
std::vector<std::string> all_on_path(const char* tool) {
    std::vector<std::string> r;
    std::stringstream ss(env("PATH"));
    std::string dir;
    std::error_code ec;
    while (std::getline(ss, dir, ';')) {
        if (dir.empty()) continue;
        if (lower(tool).rfind("python", 0) == 0 && lower(dir).find("\\windowsapps") != std::string::npos) continue;
        if (fs::is_regular_file(u8(dir) / tool, ec)) r.push_back(str(u8(dir) / tool));
    }
    return r;
}

// Pythons Windows knows of (PEP 514: HKCU / HKLM\Software\Python\PythonCore\<version>\InstallPath), wherever they were
// installed, the newest first.
std::vector<std::string> registered_pythons() {
    std::vector<std::pair<int, std::string>> found;
    const struct { HKEY root; REGSAM view; } places[] = {
        {HKEY_CURRENT_USER, 0}, {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY}, {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY}};
    for (const auto& pl : places) {
        HKEY core;
        if (RegOpenKeyExW(pl.root, LR"(Software\Python\PythonCore)", 0, KEY_READ | pl.view, &core) != ERROR_SUCCESS)
            continue;
        wchar_t name[64];
        for (DWORD i = 0;; i++) {
            DWORD len = 64;
            if (RegEnumKeyExW(core, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            int major = 0, minor = 0;
            if (swscanf_s(name, L"%d.%d", &major, &minor) != 2 || major != 3) continue;
            std::wstring key = std::wstring(name) + L"\\InstallPath";
            wchar_t val[1024];
            DWORD size = sizeof val;
            std::wstring exe;
            if (RegGetValueW(core, key.c_str(), L"ExecutablePath", RRF_RT_REG_SZ | pl.view, nullptr, val, &size) ==
                ERROR_SUCCESS)
                exe = val;
            size = sizeof val;
            if (exe.empty() &&
                RegGetValueW(core, key.c_str(), nullptr, RRF_RT_REG_SZ | pl.view, nullptr, val, &size) == ERROR_SUCCESS)
                exe = (fs::path(val) / L"python.exe").wstring();
            std::error_code ec;
            if (!exe.empty() && fs::is_regular_file(fs::path(exe), ec)) found.push_back({minor, utf8_of(exe)});
        }
        RegCloseKey(core);
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> r;
    for (auto& f : found) r.push_back(f.second);
    return r;
}

// python.org's installer puts each version in Python3<minor>: per user under LOCALAPPDATA (the default, and what
// winget installs), for everyone under Program Files. The Python install manager (3.14 on) uses
// LOCALAPPDATA\Python\pythoncore-3.<minor>-64. The newest first.
std::vector<std::string> folder_pythons() {
    std::vector<std::pair<int, std::string>> found;
    std::error_code ec;
    for (const fs::path& base : {u8(env("LOCALAPPDATA")) / "Programs" / "Python", u8(env("ProgramFiles")),
                                 u8(env("LOCALAPPDATA")) / "Python", fs::path("C:\\")}) {
        for (const auto& d : fs::directory_iterator(base, ec)) {
            std::string n = lower(str(d.path().filename()));
            int minor = 0;
            bool named = (n.rfind("python3", 0) == 0 && std::sscanf(n.c_str() + 7, "%d", &minor) == 1) ||
                         (n.rfind("pythoncore-3.", 0) == 0 && std::sscanf(n.c_str() + 13, "%d", &minor) == 1);
            if (named && fs::is_regular_file(d.path() / "python.exe", ec))
                found.push_back({minor, str(d.path() / "python.exe")});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> r;
    for (auto& f : found) r.push_back(f.second);
    return r;
}

struct PythonFound {
    std::vector<std::string> cmd;    // program, then arguments; empty if none started
    std::string version;             // "3.13"
    std::vector<std::string> tried;  // what was tried and did not answer, for the install page
};

// Each Python this computer may have, asked its version: the first 3.11 or newer wins, else the newest that answered
// (the page then says it is too old). A py.exe left by an uninstalled Python, or the install manager without any
// runtime, no longer hides a working python.exe found further on.
PythonFound find_python_now() {
    std::vector<std::vector<std::string>> cands;
    if (const char* p = std::getenv("NX_LAUNCHER_PYTHON"); p && *p) {
        cands.push_back({p});
    } else {
        for (const std::string& py : all_on_path("py.exe")) cands.push_back({py, "-3"});
        for (const std::string& py : all_on_path("python.exe")) cands.push_back({py});
        for (const std::string& py : registered_pythons()) cands.push_back({py});
        for (const std::string& py : folder_pythons()) cands.push_back({py});
    }
    PythonFound best, r;
    int best_minor = -1;
    std::vector<std::string> seen;
    for (const auto& c : cands) {
        std::string id = lower(c[0]) + (c.size() > 1 ? " " + c[1] : "");
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
        seen.push_back(id);
        std::vector<std::string> ask = c;
        ask.insert(ask.end(), {"-c", "import sys; print('PYVER %d.%d' % sys.version_info[:2])"});
        std::string out = run_output(ask);
        int major = 0, minor = 0;
        size_t at = out.find("PYVER ");
        if (at == std::string::npos || std::sscanf(out.c_str() + at + 6, "%d.%d", &major, &minor) != 2 || major != 3) {
            r.tried.push_back(c[0]);
            continue;
        }
        if (minor >= 11) {
            r.cmd = c, r.version = "3." + std::to_string(minor);
            return r;
        }
        if (minor > best_minor) best.cmd = c, best.version = "3." + std::to_string(minor), best_minor = minor;
    }
    if (!best.cmd.empty()) best.tried = r.tried;
    return best.cmd.empty() ? r : best;
}

// A Windows proxy set for the browser: the downloads go through it and it looks the sites up, not this computer.
bool system_proxy() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Internet Settings)", 0,
                      KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    DWORD on = 0, size = sizeof on;
    bool r = RegQueryValueExW(k, L"ProxyEnable", nullptr, nullptr, reinterpret_cast<BYTE*>(&on), &size) == ERROR_SUCCESS && on;
    RegCloseKey(k);
    return r;
}
#endif

// The sites tools/setup_acnh.py downloads from, looked up all at once: the first one this computer cannot find, ""
// when it finds them all (or a proxy looks them up).
std::string unreachable_site() {
    static const char* const sites[] = {"pypi.org", "files.pythonhosted.org", "github.com", "codeberg.org",
                                        "builds.dotnet.microsoft.com", "api.nuget.org"};
#if defined(_WIN32)
    if (system_proxy() || !env("HTTPS_PROXY").empty() || !env("https_proxy").empty()) return "";
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return "";
#endif
    std::vector<std::future<bool>> found;
    for (const char* s : sites)
        found.push_back(std::async(std::launch::async, [s] {
            addrinfo hints{}, *res = nullptr;
            hints.ai_socktype = SOCK_STREAM;
            bool ok = getaddrinfo(s, "443", &hints, &res) == 0;
            if (res) freeaddrinfo(res);
            return ok;
        }));
    std::string bad;
    for (size_t i = 0; i < found.size(); i++)
        if (!found[i].get() && bad.empty()) bad = sites[i];
#if defined(_WIN32)
    WSACleanup();
#endif
    return bad;
}

bool vulkan_loader() {
    if (!SDL_Vulkan_LoadLibrary(nullptr)) return false;
    SDL_Vulkan_UnloadLibrary();
    return true;
}

}  // namespace

#if defined(_WIN32)
std::vector<std::string> python_command(const std::string& root) {
    // Never the repository's venv (private\venv): the setup makes it again when it was moved from another folder, and
    // Windows does not let it delete the python.exe it runs on.
    (void)root;
    return find_python_now().cmd;
}
#endif

std::vector<Prereq> check_prerequisites(const std::string& install_dir) {
    std::vector<Prereq> r;
    auto add = [&](const char* label, bool good, Status bad, std::string detail_good, std::string detail_bad) {
        r.push_back({label, good ? Status::Ok : bad, good ? std::move(detail_good) : std::move(detail_bad)});
    };
    const std::string found = tr("launcher.check.found"), missing = tr("launcher.check.missing");
#if defined(_WIN32)
    // tools/setup_acnh.py fetches LLVM (clang-cl), SDL3, CMake and Ninja itself: Python, Visual Studio's C++ tools and
    // a Vulkan driver are what it needs from the system.
    refresh_path();
    const std::string root = repo_root();
    add("launcher.check.scripts", !root.empty(), Status::Error, found, tr("launcher.err.no_repo"));
    // The version the setup will really run with: one too old, or the Store's alias, would stop it at its first step.
    PythonFound py = find_python_now();
    int major = 0, minor = 0;
    std::sscanf(py.version.c_str(), "%d.%d", &major, &minor);
    // Not found: the Pythons that were there but did not answer are named, so a screenshot says why.
    std::string none = missing;
    if (!py.tried.empty()) {
        none += ":";
        for (const std::string& t : py.tried) none += " " + t + (&t == &py.tried.back() ? "" : ",");
    }
    add("launcher.check.python", major == 3 && minor >= 11, Status::Error, "Python " + py.version,
        py.version.empty() ? none : trf("launcher.check.python_old", "Python " + py.version));
    add("launcher.check.compiler", msvc_tools(), Status::Error, found, missing);
    add("launcher.check.vulkan", vulkan_loader(), Status::Error, found, missing);
    // vcpkg builds the movie decoders from a git clone: without git the setup goes on, the movies are black.
    std::error_code ec;
    bool movie_libs = !root.empty() && fs::exists(u8(root) / "private" / "third_party" / "vcpkg" / "vcpkg.exe", ec);
    add("launcher.check.movie", movie_libs || in_path("git.exe"), Status::Warning, found, tr("launcher.check.git_missing"));
    // The tools come from the internet the first time (about 1.5 GB): without it the setup stops at its first step.
    // Once they are here a rebuild mostly does without, so then it is only a warning.
    std::string site = unreachable_site();
    bool have_tools = !root.empty() && fs::exists(u8(root) / "private" / "venv", ec) &&
                      fs::exists(u8(root) / "private" / "build" / "spirv_harness", ec);
    add("launcher.check.internet", site.empty(), have_tools ? Status::Warning : Status::Error, found,
        trf("launcher.check.internet_missing", site));
#else
    add("launcher.check.scripts", !repo_root().empty(), Status::Error, found, tr("launcher.err.no_repo"));
    add("launcher.check.compiler", in_path("clang") && in_path("clang++"), Status::Error, found, missing);
    add("launcher.check.python", in_path("python3"), Status::Error, found, missing);
    add("launcher.check.tools", in_path("curl") && in_path("tar") && in_path("bash"), Status::Error, found, missing);
    add("launcher.check.sdl", sdl3_dev_files(), Status::Error, found, missing);
    add("launcher.check.vulkan", vulkan_loader(), Status::Error, found, missing);
    std::string site = unreachable_site();
    add("launcher.check.internet", site.empty(), Status::Warning, found, trf("launcher.check.internet_missing", site));
#if defined(__APPLE__)
    std::error_code ec;
    bool movie = (std::system("pkg-config --exists vpx vorbis >/dev/null 2>&1") == 0) ||
                 (fs::exists("/opt/homebrew/include/vpx/vpx_decoder.h", ec) && fs::exists("/opt/homebrew/include/vorbis/codec.h", ec)) ||
                 (fs::exists("/usr/local/include/vpx/vpx_decoder.h", ec) && fs::exists("/usr/local/include/vorbis/codec.h", ec));
#else
    bool movie = std::system("pkg-config --exists vpx vorbis >/dev/null 2>&1") == 0;
#endif
    add("launcher.check.movie", movie, Status::Warning, found, tr("launcher.check.movie_missing"));
#endif
    uint64_t free = free_space(install_dir);
    const uint64_t GB = 1000000000ull;
    r.push_back({"launcher.check.disk", free >= 30 * GB ? Status::Ok : free >= 20 * GB ? Status::Warning : Status::Error,
                 free >= 30 * GB ? trf("launcher.install.free", human_size(free))
                                 : trf("launcher.check.disk_low", human_size(free))});
    // 4 GB is enough: the setup runs fewer compile jobs at once on less memory, so it only takes longer. A "4 GB"
    // computer reports a little less (memory kept for the firmware or the integrated GPU).
    int ram_mb = SDL_GetSystemRAM();
    char ram[32];
    std::snprintf(ram, sizeof ram, "%.0f GB", ram_mb / 1024.0);
    r.push_back({"launcher.check.ram", ram_mb >= 3500 ? Status::Ok : Status::Warning,
                 ram_mb >= 15 * 1024 ? std::string(ram)
                 : ram_mb >= 3500    ? trf("launcher.check.ram_small", ram)
                                     : trf("launcher.check.ram_low", ram)});
    return r;
}

std::string install_command(const std::vector<Prereq>& checks) {
#if defined(_WIN32)
    // Only what is missing: a winget install of something already there fails (Visual Studio's installer exits with
    // 1), and looks as if it still had to be installed.
    auto bad = [&](const char* label) {
        for (const Prereq& p : checks)
            if (std::string(p.label) == label) return p.status != Status::Ok;
        return false;
    };
    std::string cmd;
    if (bad("launcher.check.python")) cmd += "winget install Python.Python.3.13\n";
    if (bad("launcher.check.movie")) cmd += "winget install Git.Git\n";
    if (bad("launcher.check.compiler"))
        cmd += "winget install Microsoft.VisualStudio.2022.BuildTools --override "
               "\"--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended\"\n";
    if (!cmd.empty()) cmd.pop_back();
    return cmd;
#elif defined(__APPLE__)
    return "xcode-select --install\nbrew install llvm cmake ninja sdl3 molten-vk vulkan-loader libvpx libvorbis";
#else
    std::ifstream f("/etc/os-release");
    std::string line, ids;
    while (std::getline(f, line))
        if (line.rfind("ID=", 0) == 0 || line.rfind("ID_LIKE=", 0) == 0) ids += " " + lower(line.substr(line.find('=') + 1));
    if (ids.find("fedora") != std::string::npos || ids.find("rhel") != std::string::npos)
        return "sudo dnf install clang lld cmake ninja-build patch curl python3 spirv-tools pkgconf-pkg-config \\\n"
               "     vulkan-loader-devel SDL3-devel libvpx-devel libvorbis-devel mesa-vulkan-drivers vulkan-tools";
    if (ids.find("arch") != std::string::npos)
        return "sudo pacman -S --needed base-devel clang lld cmake ninja patch curl python spirv-tools pkgconf \\\n"
               "     vulkan-icd-loader vulkan-headers sdl3 libvpx libvorbis vulkan-tools";
    return "sudo apt install build-essential clang lld cmake ninja-build patch curl python3-venv spirv-tools pkg-config \\\n"
           "     libvulkan-dev libsdl3-dev libvpx-dev libvorbis-dev mesa-vulkan-drivers vulkan-tools";
#endif
}

}  // namespace launcher
