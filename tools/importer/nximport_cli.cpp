// PC test harness of the on-device importer: the same code the APK runs, fed with files from the command line.
//   nximport --out <data folder> [--title 010015100B514000] [--verify] [--exefs] <prod.keys> [title.keys] <x.nsp|x.xci> ...
// Prints progress and log lines (never key values).
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nximport.h"
#include "nxposix_win.h"

#ifdef _WIN32
#include <shellapi.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    // The arguments as UTF-8 (paths with accents), not in the ANSI code page.
    std::vector<std::string> utf8;
    std::vector<char*> ptrs;
    if (wchar_t** w = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 0; i < argc; i++) utf8.push_back(nxwin::narrow(w[i]));
        LocalFree(w);
        for (auto& s : utf8) ptrs.push_back(s.data());
        argv = ptrs.data();
    }
#endif
    nxi::Options opt;
    std::vector<nxi::InputFile> inputs;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) {
            opt.out_dir = argv[++i];
        } else if (a == "--title" && i + 1 < argc) {
            opt.title_id = strtoull(argv[++i], nullptr, 16);
        } else if (a == "--exefs") {
            opt.keep_exefs = true;
        } else if (a == "--verify") {
            opt.verify_only = true;
        } else {
            int fd = open(argv[i], O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                fprintf(stderr, "cannot open %s\n", argv[i]);
                return 2;
            }
            const char* slash = strrchr(argv[i], '/');
#ifdef _WIN32
            if (const char* bs = strrchr(argv[i], '\\'); bs && (!slash || bs > slash)) slash = bs;
#endif
            inputs.push_back({slash ? slash + 1 : argv[i], fd});
        }
    }
    if (opt.out_dir.empty()) {
        fprintf(stderr, "usage: nximport --out <dir> [--title <id>] [--verify] [--exefs] <prod.keys> [title.keys] <nsp|xci>...\n");
        return 2;
    }
    auto t0 = std::chrono::steady_clock::now();
    std::string last_phase;
    opt.on_progress = [&](const nxi::Progress& p) {
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (p.phase != last_phase) fprintf(stderr, "\n"), last_phase = p.phase;
        fprintf(stderr, "\r[%7.1fs] %-6s %6.2f%% %10.1f MB  %-60.60s", s, p.phase.c_str(),
                p.total ? 100.0 * double(p.done) / double(p.total) : 100.0, double(p.done) / 1e6, p.item.c_str());
    };
    opt.on_log = [](const std::string& s) { fprintf(stderr, "\n%s", s.c_str()); };
    std::string err;
    int rc = nxi::run_import(inputs, opt, err);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "\n%s after %.1f s%s%s\n", rc ? "FAILED" : "OK", s, rc ? ": " : "", err.c_str());
    for (auto& in : inputs) close(in.fd);
    return rc;
}
