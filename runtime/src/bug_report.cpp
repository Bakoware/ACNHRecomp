// Problem reports (bug_report.h).
#include "bug_report.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if !defined(_WIN32)
#include <sys/utsname.h>
#endif

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {  // u8string is char8_t from C++20
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string home() {
#if defined(_WIN32)
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    return h ? h : "";
}

std::string read_file(const std::string& path) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The home folder's path appears in the logs (the data folder, the pipeline cache): replaced by "~".
std::string scrub(std::string text) {
    std::string h = home();
    if (h.size() < 2) return text;
    for (size_t at = text.find(h); at != std::string::npos; at = text.find(h, at + 1)) text.replace(at, h.size(), "~");
    return text;
}

bool copy_scrubbed(const std::string& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::exists(fs::u8path(from), ec)) return false;
    std::ofstream out(to, std::ios::binary);
    out << scrub(read_file(from));
    return bool(out);
}

#if defined(__APPLE__)
std::string sysctl_string(const char* name) {
    char buf[256] = {};
    size_t n = sizeof buf - 1;
    return sysctlbyname(name, buf, &n, nullptr, 0) == 0 ? std::string(buf) : std::string("?");
}
#endif

std::string system_lines() {
    std::ostringstream s;
    s << "ACNH Recomp " << NX_VERSION_STRING << "\n";
#if defined(__APPLE__)
    uint64_t mem = 0;
    size_t n = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &n, nullptr, 0);
    s << "macOS " << sysctl_string("kern.osproductversion") << " (" << sysctl_string("kern.osversion") << ")\n";
    s << "CPU: " << sysctl_string("machdep.cpu.brand_string") << ", " << std::thread::hardware_concurrency()
      << " threads\n";
    s << "Model: " << sysctl_string("hw.model") << "\n";
    s << "RAM: " << (mem >> 20) << " MiB\n";
#elif defined(_WIN32)
    s << "Windows\nCPU threads: " << std::thread::hardware_concurrency() << "\n";
#else
    utsname u{};
    if (uname(&u) == 0) s << u.sysname << " " << u.release << " " << u.machine << "\n";
    s << "CPU threads: " << std::thread::hardware_concurrency() << "\n";
#endif
    s << "The GPU, the settings in effect and any crash are in nx.log (and nx-previous.log for the session before).\n";
    return s.str();
}

// A folder on the desktop when there is one, else next to the logs.
fs::path destination() {
    std::error_code ec;
    fs::path desk = fs::u8path(home()) / "Desktop";
    if (!home().empty() && fs::is_directory(desk, ec)) return desk;
    return fs::u8path(log_file_path()).parent_path();
}

bool zip_folder(const fs::path& dir, const fs::path& zip) {
    std::string d = utf8(dir), z = utf8(zip);
#if defined(__APPLE__)
    std::string cmd = "ditto -c -k --norsrc --keepParent \"" + d + "\" \"" + z + "\"";
#elif defined(_WIN32)
    std::string cmd = "powershell -NoProfile -Command \"Compress-Archive -Path '" + d + "' -DestinationPath '" + z + "'\"";
#elif defined(__ANDROID__)
    std::string cmd;
#else
    std::string cmd = "cd \"" + utf8(dir.parent_path()) + "\" && zip -qr \"" + z + "\" \"" + utf8(dir.filename()) + "\"";
#endif
    std::error_code ec;
    return !cmd.empty() && std::system(cmd.c_str()) == 0 && fs::exists(zip, ec);
}

}  // namespace

bool previous_session_failed() {
    std::string prev = log_file_path() + ".prev";
    std::ifstream f(fs::u8path(prev));
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("CRASH:", 0) == 0 || line.rfind("FATAL:", 0) == 0 || line.rfind("hang: no frame", 0) == 0)
            return true;
    return false;
}

std::string write_bug_report(std::string& error) {
    nx::log("report: creating a problem report");  // opens the log (and keeps the last one as .prev) when not yet
    std::string log = log_file_path();
    if (log.empty()) return error = "no log file", "";
    char stamp[32];
    std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&t));
    fs::path dir = destination() / (std::string("ACNH-Recomp-report-") + stamp);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return error = ec.message(), "";
    copy_scrubbed(log, dir / "nx.log");
    copy_scrubbed(log + ".prev", dir / "nx-previous.log");
    copy_scrubbed(plat::data_root() + "/config.ini", dir / "config.ini");
    {
        std::ofstream f(dir / "system.txt");
        f << system_lines();
    }
    fs::path zip = dir;
    zip += ".zip";
    if (zip_folder(dir, zip)) {
        fs::remove_all(dir, ec);
        return utf8(zip);
    }
    return utf8(dir);
}

bool report_mode() {
    if (!std::getenv("NX_REPORT")) return false;
    std::string error, path = write_bug_report(error);
    std::printf("%s\n", path.empty() ? ("report failed: " + error).c_str() : path.c_str());
    return true;
}

}  // namespace nx
