// The setup as a child process, and starting the game (launcher.h).
//
// tools/setup_acnh.sh runs in its own process group with its output (stdout and stderr) on a pipe: a reader thread
// splits it into lines, keeps them for the log view, writes them to <install>/logs/launcher.log and follows the
// stage lines ("== <stage>") and ninja's "[n/total]" counter. Cancel sends SIGTERM to the whole group (bash, python,
// ninja and its compilers), then SIGKILL if something is still there 5 s later.
#include "launcher.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace launcher {

namespace {

using clock = std::chrono::steady_clock;

std::filesystem::path utf8_path(const std::string& p) { return std::filesystem::path(reinterpret_cast<const char8_t*>(p.c_str())); }

std::mutex g_lock;
std::vector<std::string> g_lines;
SetupState g_state;
clock::time_point g_t0, g_build_t0;
std::string g_log_path;

// Share of each stage in the progress bar: the first build's compile is most of the time.
const float kWeight[kStageCount] = {0.06f, 0.08f, 0.01f, 0.12f, 0.04f, 0.01f, 0.68f};

int stage_of(const std::string& name) {
    static const std::pair<const char*, int> names[] = {
        {"tools", kStageTools},     {"import", kStageImport},   {"mod ", kStageMods},  {"recompile", kStageRecompile},
        {"shaders", kStageShaders}, {"fonts", kStageFonts},     {"build", kStageBuild}, {"done", kStageCount},
    };
    for (const auto& [n, s] : names)
        if (name.rfind(n, 0) == 0) return s;
    return -1;
}

// Under g_lock.
void on_line(const std::string& line) {
    if (line.rfind("== ", 0) == 0) {
        int s = stage_of(line.substr(3));
        if (s >= 0) {
            for (int k = 0; k < s && k < kStageCount; k++) g_state.done[k] = true;
            g_state.stage = s < kStageCount ? s : g_state.stage;
            if (s == kStageBuild) g_build_t0 = clock::now();
        }
    } else if (g_state.stage == kStageBuild && line.size() > 3 && line[0] == '[') {
        unsigned n = 0, total = 0;
        if (std::sscanf(line.c_str(), "[%u/%u]", &n, &total) == 2 && total) g_state.build_fraction = float(n) / float(total);
    }
    g_state.last_line = line;
}

#if !defined(_WIN32)
pid_t g_pid = -1;
std::atomic<bool> g_cancel{false};

void reader(int fd, FILE* log) {
    std::string pending;
    char buf[8192];
    auto flush = [&] {
        if (pending.empty()) return;
        if (log) std::fprintf(log, "%s\n", pending.c_str()), std::fflush(log);
        std::lock_guard lk(g_lock);
        if (g_lines.size() < 200000) g_lines.push_back(pending);
        on_line(pending);
        pending.clear();
    };
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r')
                flush();
            else if (pending.size() < 4096)
                pending += c;
        }
    }
    flush();
    close(fd);
    int status = 0;
    while (waitpid(g_pid, &status, 0) < 0 && errno == EINTR) {
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    std::lock_guard lk(g_lock);
    g_state.exit_code = code;
    g_state.running = false;
    g_state.finished = true;
    g_state.cancelled = g_cancel;
    g_state.elapsed = std::chrono::duration<double>(clock::now() - g_t0).count();
    if (code == 0)
        for (bool& d : g_state.done) d = true;
    if (log) std::fprintf(log, "\n[launcher] exit code %d\n", code), std::fclose(log);
    g_pid = -1;
}
#endif

}  // namespace

bool setup_supported() {
#if defined(_WIN32)
    return false;
#else
    return true;
#endif
}

bool setup_start(const SetupArgs& a, std::string& error) {
#if defined(_WIN32)
    (void)a;
    error = "Windows";
    return false;
#else
    {
        std::lock_guard lk(g_lock);
        if (g_state.running) return error = "already running", false;
    }
    std::string root = repo_root();
    std::string script = std::getenv("NX_LAUNCHER_SCRIPT") ? std::getenv("NX_LAUNCHER_SCRIPT") : "";  // tests
    if (script.empty()) {
        if (root.empty()) return error = "tools/setup_acnh.sh not found", false;
        script = root + "/tools/setup_acnh.sh";
    }
    std::vector<std::string> args = {"/bin/bash", script, "--out", a.install_dir};
    auto opt = [&](const char* name, const std::string& v) {
        if (!v.empty()) args.push_back(name), args.push_back(v);
    };
    opt("--keys", a.keys);
    opt("--title-keys", a.title_keys);
    opt("--base", a.base);
    opt("--update", a.update);
    for (const std::string& d : a.dlc) opt("--dlc", d);
    if (!a.ptbr_zip.empty()) opt("--mod", "ptbr=" + a.ptbr_zip);

    std::error_code ec;
    std::filesystem::create_directories(utf8_path(a.install_dir + "/logs"), ec);
    g_log_path = a.install_dir + "/logs/launcher.log";
    FILE* log = std::fopen(g_log_path.c_str(), "w");
    if (log) {
        std::fprintf(log, "[launcher]");
        for (const std::string& s : args) std::fprintf(log, " %s", s.c_str());
        std::fprintf(log, "\n");
    }

    // Everything the child needs is made before fork: only async-signal-safe calls happen between fork and exec.
    std::vector<std::string> envs;
    for (char** e = environ; *e; e++)
        if (std::strncmp(*e, "PYTHONUNBUFFERED=", 17) != 0) envs.push_back(*e);
    envs.push_back("PYTHONUNBUFFERED=1");
    std::vector<char*> argv, envp;
    for (std::string& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    for (std::string& s : envs) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::string cwd = root.empty() ? std::string("/") : root;

    int fds[2];
    if (pipe(fds) != 0) return error = std::strerror(errno), false;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]), close(fds[1]);
        return error = std::strerror(errno), false;
    }
    if (pid == 0) {
        setpgid(0, 0);
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) dup2(null, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        if (chdir(cwd.c_str()) != 0) _exit(126);
        execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }
    setpgid(pid, pid);  // also from here: whichever runs first
    close(fds[1]);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    {
        std::lock_guard lk(g_lock);
        g_lines.clear();
        g_state = SetupState();
        g_state.running = true;
        g_t0 = clock::now();
        g_pid = pid;
        g_cancel = false;
    }
    std::thread(reader, fds[0], log).detach();
    return true;
#endif
}

void setup_cancel() {
#if !defined(_WIN32)
    pid_t pid;
    {
        std::lock_guard lk(g_lock);
        if (!g_state.running || g_pid <= 0) return;
        pid = g_pid;
    }
    g_cancel = true;
    kill(-pid, SIGTERM);
    std::thread([pid] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::lock_guard lk(g_lock);
        if (g_state.running && g_pid == pid) kill(-pid, SIGKILL);
    }).detach();
#endif
}

SetupState setup_state() {
    std::lock_guard lk(g_lock);
    SetupState s = g_state;
    if (s.running) {
        auto now = clock::now();
        s.elapsed = std::chrono::duration<double>(now - g_t0).count();
        double tb = std::chrono::duration<double>(now - g_build_t0).count();
        if (s.stage == kStageBuild && s.build_fraction > 0.03f && tb > 30)
            s.eta = tb * (1.0 - s.build_fraction) / s.build_fraction;
    }
    return s;
}

float setup_progress(const SetupState& s) {
    float p = 0;
    for (int k = 0; k < kStageCount; k++)
        if (s.done[k]) p += kWeight[k];
    if (s.stage == kStageBuild && !s.done[kStageBuild]) p += kWeight[kStageBuild] * s.build_fraction;
    return std::min(p, 1.0f);
}

void setup_lines(size_t from, std::vector<std::string>& out) {
    std::lock_guard lk(g_lock);
    for (size_t i = from; i < g_lines.size(); i++) out.push_back(g_lines[i]);
}

std::string setup_log_path() { return g_log_path; }

// ---- the game -------------------------------------------------------------------------------------------------

bool launch_game(const Build& b, std::string& error) {
    SDL_Environment* env = SDL_CreateEnvironment(true);
    if (!env) return error = SDL_GetError(), false;
    SDL_SetEnvironmentVariable(env, "NX_DATA_ROOT", b.data.c_str(), true);
    SDL_SetEnvironmentVariable(env, "NX_LOG", (b.data + "/nx_play.log").c_str(), false);
#if defined(__linux__)
    SDL_SetEnvironmentVariable(env, "NX_MEM_LIMIT_GB", "10", false);  // as tools/acnh_linux.sh
#endif
    const char* argv[] = {b.exe.c_str(), nullptr};
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<char**>(argv));
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
#if defined(SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING)
    SDL_SetStringProperty(p, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, b.data.c_str());
#endif
    SDL_SetBooleanProperty(p, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process* proc = SDL_CreateProcessWithProperties(p);
    SDL_DestroyProperties(p);
    SDL_DestroyEnvironment(env);
    if (!proc) return error = SDL_GetError(), false;
    SDL_DestroyProcess(proc);  // a background process keeps running
    return true;
}

void open_folder(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(utf8_path(path), ec);
    SDL_OpenURL(("file://" + path).c_str());
}

}  // namespace launcher
