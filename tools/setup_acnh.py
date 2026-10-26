#!/usr/bin/env python3
"""From your own dump to a native build, every stage in order, on Windows.
Each stage is skipped when its output is already there, so it can be run again after a failure or an update of the
repository. Lines starting with "== " name the stage that begins: the launcher (launcher/pipeline.cpp) follows them.

usage: python tools/setup_acnh.py --keys <prod.keys> [--title-keys <title.keys>] --base <base.nsp> [--update <update.nsp>]
                                  [--dlc <add-on.nsp>]... [--mod <name>=<zip>]... [--out <folder>]
       python tools/setup_acnh.py [--out <folder>]           (later runs: the import is done, the rest is checked)
  Any version of the game: without --update the base game is built as dumped (games/acnh/versions.txt,
  docs/VERSIONS.md). One version per folder: another base/update pair in a folder that holds a game stops, unless
  FORCE=import (which replaces it); give each version its own --out folder to keep several.
  env: JOBS=<compile jobs> (default: free RAM / 700 MB, at most the core count)   FORCE=<stage> to redo one stage
       (import, recomp, shaders, harness, glyphs)

Stages: tools -> import -> version -> recompile -> shaders -> fonts -> build. Everything it writes stays in private/
(gitignored) and the --out folder. On Windows the tools stage also fetches LLVM (clang-cl, lld) and SDL3 into
private/third_party and uses the Visual Studio C++ build tools found by vswhere (the Windows SDK and C runtime).
"""
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P = os.path.join(ROOT, "private")
T = os.path.join(P, "third_party")
MOVIE_LIBS = os.path.join(T, "vcpkg", "installed", "x64-windows-static-md")  # libvpx, libvorbis (vcpkg)
WIN = os.name == "nt"
MAC = sys.platform == "darwin"

VK_HEADERS = "1.4.357"
LLVM = "23.1.3"  # Windows only: clang-cl and lld-link (Linux and macOS use the system's clang)
SDL3 = "3.4.16"  # Windows only: SDL's VC development package (Linux and macOS: the system's SDL3)
IMGUI = "1.91.9"
RYUBING = "a82350bb77"
DOTNET = "10.0.401"


def say(stage):
    print(f"\n== {stage}", flush=True)


def fail(msg):
    print(msg, flush=True)
    sys.exit(1)


def force(stage):
    return os.environ.get("FORCE") == stage


def run(cmd, log=None, env=None, cwd=ROOT, check=True):
    """Runs cmd (a list); output to the log file when given (its last lines are shown on failure)."""
    exe = shutil.which(cmd[0], path=(env or os.environ).get("PATH")) or cmd[0]
    if log:
        with open(log, "w", encoding="utf-8", errors="replace") as f:
            rc = subprocess.call([exe] + cmd[1:], stdout=f, stderr=subprocess.STDOUT, env=env, cwd=cwd)
        if rc and check:
            with open(log, encoding="utf-8", errors="replace") as f:
                print("".join(f.readlines()[-20:]))
            fail(f"failed: {' '.join(cmd[:3])} ... (log: {log})")
        return rc
    rc = subprocess.call([exe] + cmd[1:], env=env, cwd=cwd)
    if rc and check:
        fail(f"failed: {' '.join(cmd[:3])} ...")
    return rc


def fetch(url, dest_dir, strip=True):
    """Downloads a .tar.gz / .tar.xz / .zip and extracts it into dest_dir (its single top folder removed if strip)."""
    os.makedirs(dest_dir, exist_ok=True)
    tmp = dest_dir + ".download"
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
    if url.endswith(".zip"):
        with zipfile.ZipFile(tmp) as z:
            names = z.namelist()
            top = names[0].split("/")[0] + "/"
            for i in z.infolist():
                n = i.filename[len(top):] if strip and i.filename.startswith(top) else i.filename
                if not n or n.startswith("/") or ".." in n.split("/"):
                    continue
                out = os.path.join(dest_dir, n)
                if i.is_dir():
                    os.makedirs(out, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with z.open(i) as a, open(out, "wb") as b:
                    shutil.copyfileobj(a, b)
    else:
        with tarfile.open(tmp) as t:
            members = []
            for m in t.getmembers():
                parts = m.name.split("/", 1) if strip else [None, m.name]
                if len(parts) < 2 or not parts[1]:
                    continue
                m.name = parts[1]
                if m.islnk() and strip:  # hard links name their target with the top folder too
                    m.linkname = m.linkname.split("/", 1)[-1]
                members.append(m)
            t.extractall(dest_dir, members=members, filter="data")
    os.remove(tmp)


def free_mb():
    if WIN:
        import ctypes

        class MS(ctypes.Structure):
            _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                        ("avail", ctypes.c_ulonglong), ("tpf", ctypes.c_ulonglong), ("apf", ctypes.c_ulonglong),
                        ("tv", ctypes.c_ulonglong), ("av", ctypes.c_ulonglong), ("aev", ctypes.c_ulonglong)]

        ms = MS()
        ms.len = ctypes.sizeof(MS)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(ms))
        return ms.avail // (1 << 20)
    if MAC:
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") // (1 << 21)
    with open("/proc/meminfo") as f:
        return next(int(l.split()[1]) for l in f if l.startswith("MemAvailable:")) // 1024


def msvc_env():
    """The environment of a "x64 Native Tools" prompt (Visual Studio's vcvars64.bat), LLVM first on PATH."""
    vswhere = os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe")
    inst = ""
    if os.path.exists(vswhere):
        inst = subprocess.run([vswhere, "-latest", "-products", "*", "-requires",
                               "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                              capture_output=True, text=True).stdout.strip()
    bat = os.path.join(inst, r"VC\Auxiliary\Build\vcvars64.bat") if inst else ""
    if not bat or not os.path.exists(bat):
        fail("missing: the Visual Studio C++ build tools (the Windows SDK and C runtime clang-cl links against).\n"
             "Install them with:\n  winget install Microsoft.VisualStudio.2022.BuildTools --override "
             "\"--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended\"")
    out = subprocess.run(f'cmd /s /c ""{bat}" >nul 2>&1 && set"', capture_output=True, text=True, errors="replace").stdout
    env = dict(l.split("=", 1) for l in out.splitlines() if "=" in l and not l.startswith("="))
    # Windows spells it "Path" (from Explorer and the launcher), shells often "PATH": one key, whatever the case.
    paths = [env.pop(k) for k in [k for k in env if k.upper() == "PATH"]]
    path = paths[0] if paths else ""
    env["PATH"] = os.pathsep.join([os.path.join(T, f"llvm-{LLVM}", "bin"), os.path.join(P, "venv", "Scripts"), path])
    return env


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--keys", default="")
    ap.add_argument("--title-keys", default="")
    ap.add_argument("--base", default="")
    ap.add_argument("--update", default="")
    ap.add_argument("--dlc", action="append", default=[])
    ap.add_argument("--mod", action="append", default=[])
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    G = os.path.abspath(a.out) if a.out else os.path.join(P, "acnh")
    os.makedirs(os.path.join(G, "logs"), exist_ok=True)
    os.makedirs(T, exist_ok=True)
    logs = os.path.join(G, "logs")

    cores = os.cpu_count() or 1
    jobs = int(os.environ.get("JOBS") or max(1, min(cores, (free_mb() - 1000) // 700)))

    # ---- tools --------------------------------------------------------------------------------------------------
    say("tools")
    py = os.path.join(P, "venv", "Scripts" if WIN else "bin", "python.exe" if WIN else "python")
    if not os.path.exists(py):
        run([sys.executable, "-m", "venv", os.path.join(P, "venv")])
    if subprocess.call([py, "-c", "import capstone, lz4, zstandard, fontTools, cmake, ninja"],
                       stderr=subprocess.DEVNULL):
        run([py, "-m", "pip", "install", "-q", "capstone", "lz4", "zstandard", "fonttools", "cmake", "ninja", "unicorn"])
    bindir = os.path.dirname(py)
    cmake = os.path.join(bindir, "cmake.exe" if WIN else "cmake")
    ninja = os.path.join(bindir, "ninja.exe" if WIN else "ninja")
    vkh = os.path.join(T, f"Vulkan-Headers-{VK_HEADERS}")
    if not os.path.isdir(vkh):
        fetch(f"https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v{VK_HEADERS}.tar.gz", vkh)
    if not os.path.exists(os.path.join(T, "imgui", "imgui.cpp")):  # the settings screen (Dear ImGui, MIT)
        fetch(f"https://github.com/ocornut/imgui/archive/refs/tags/v{IMGUI}.tar.gz", os.path.join(T, "imgui"))
    if not os.path.exists(os.path.join(T, "ryubing", "Ryujinx.sln")):
        fetch(f"https://codeberg.org/Software_arch/Ryubing/archive/{RYUBING}.tar.gz", os.path.join(T, "ryubing"))
    if WIN:
        llvm = os.path.join(T, f"llvm-{LLVM}")
        if not os.path.exists(os.path.join(llvm, "bin", "clang-cl.exe")):
            print(f"downloading LLVM {LLVM} (about 860 MB)", flush=True)
            fetch(f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM}/"
                  f"clang+llvm-{LLVM}-x86_64-pc-windows-msvc.tar.xz", llvm)
        sdl = os.path.join(T, f"SDL3-{SDL3}")
        if not os.path.isdir(os.path.join(sdl, "cmake")):
            fetch(f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL3}/SDL3-devel-{SDL3}-VC.zip", sdl)
        env = msvc_env()
        cc = ["-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
        # libvpx and libvorbis decode the game's movies (runtime/src/hle_movie.cpp; the flight to the island is a VP9
        # movie): static libraries built by vcpkg the first time, a few minutes. Without git the movies are black.
        vcpkg = os.path.join(T, "vcpkg")
        if not os.path.exists(os.path.join(MOVIE_LIBS, "lib", "vpx.lib")):
            if not os.path.exists(os.path.join(vcpkg, "vcpkg.exe")) and shutil.which("git"):
                if not os.path.isdir(vcpkg):
                    run(["git", "clone", "-q", "--depth", "1", "https://github.com/microsoft/vcpkg.git", vcpkg])
                run([os.path.join(vcpkg, "bootstrap-vcpkg.bat"), "-disableMetrics"], log=os.path.join(logs, "vcpkg.log"))
            if os.path.exists(os.path.join(vcpkg, "vcpkg.exe")):
                print("building libvpx and libvorbis (the game's movies; a few minutes, once)", flush=True)
                run([os.path.join(vcpkg, "vcpkg.exe"), "install", "libvpx:x64-windows-static-md",
                     "libvorbis:x64-windows-static-md", "--disable-metrics"], log=os.path.join(logs, "vcpkg.log"),
                    env=env, cwd=vcpkg)
            else:
                print("git not found: no libvpx, the game's movies will be black", flush=True)
    else:
        for tool in ("clang", "clang++", "curl"):
            if not shutil.which(tool):
                fail(f"missing: {tool} (see README, Requirements)")
        env = dict(os.environ)
        cc = []
        os.environ["CC"], os.environ["CXX"] = "clang", "clang++"
        env["CC"], env["CXX"] = "clang", "clang++"
    # .NET 10 for the shader translator: the system's when it has SDK 10, else a private copy.
    dotnet = shutil.which("dotnet")
    has10 = dotnet and any(l.startswith("10.") for l in subprocess.run([dotnet, "--list-sdks"], capture_output=True,
                                                                        text=True).stdout.splitlines())
    if not has10:
        dn = os.path.join(T, "dotnet")
        dotnet = os.path.join(dn, "dotnet.exe" if WIN else "dotnet")
        if not os.path.exists(dotnet):
            rid = {("Windows", "AMD64"): "win-x64", ("Linux", "x86_64"): "linux-x64", ("Linux", "aarch64"): "linux-arm64",
                   ("Darwin", "arm64"): "osx-arm64", ("Darwin", "x86_64"): "osx-x64"}[(platform.system(), platform.machine())]
            ext = "zip" if WIN else "tar.gz"
            fetch(f"https://builds.dotnet.microsoft.com/dotnet/Sdk/{DOTNET}/dotnet-sdk-{DOTNET}-{rid}.{ext}", dn, strip=False)
        env["DOTNET_ROOT"] = dn
        env["PATH"] = dn + os.pathsep + env["PATH"]
    env["DOTNET_CLI_TELEMETRY_OPTOUT"] = "1"
    harness = os.path.join(P, "build", "spirv_harness", "bin", "Release", "net10.0", "spirv_harness.dll")
    if not os.path.exists(harness) or force("harness"):
        run([dotnet, "build", "tools/maxwell/spirv_harness/SpirvHarness.csproj", "-c", "Release", "-m:4"],
            log=os.path.join(logs, "harness.log"), env=env)
    imp_build = os.path.join(P, "build", "importer")
    if not os.path.exists(os.path.join(imp_build, "build.ninja")):
        run([cmake, "-S", "tools/importer", "-B", imp_build, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}",
             "-DCMAKE_BUILD_TYPE=Release"] + cc, log=os.path.join(logs, "importer_cmake.log"), env=env)
    run([ninja, "-C", imp_build, "nximport"], log=os.path.join(logs, "importer_build.log"), env=env)
    nximport = os.path.join(imp_build, "nximport.exe" if WIN else "nximport")

    # ---- import -------------------------------------------------------------------------------------------------
    say("import (ExeFS + RomFS with the update applied, add-on content)")
    # The pair of NSPs this folder was imported from (names and sizes): another pair is another version of the game.
    sources = ""
    if a.base:
        sources = f"{os.path.basename(a.base)} {os.path.getsize(a.base)} " + (
            f"{os.path.basename(a.update)} {os.path.getsize(a.update)}" if a.update else "-")
    src_file = os.path.join(G, "sources.txt")
    if sources and os.path.exists(src_file):
        old = open(src_file, encoding="utf-8").read().strip()
        if old != sources:
            if not force("import"):
                fail(f"{G} holds another version of the game ({old}):\n"
                     "give this one its own --out folder to keep both, or run again with FORCE=import to replace it")
            for d in ("exefs", "analysis", "extract", "gen", "spirv", "shaders_extracted", ".import"):
                shutil.rmtree(os.path.join(G, d), ignore_errors=True)
            for f in ("version.ini", "speedfix_main.pchtxt"):
                if os.path.exists(os.path.join(G, f)):
                    os.remove(os.path.join(G, f))
    if (not os.path.exists(os.path.join(G, "analysis", "main.bin")) or not os.path.isdir(os.path.join(G, "extract", "romfs", "Shader"))
            or a.dlc or force("import")):
        if not (a.keys and a.base):
            fail("first run: pass --keys and --base (and --update for an update)")
        # Some key dumps carry entries with odd lengths that the importer does not need: keep only well-formed ones.
        os.makedirs(os.path.join(P, "keys"), exist_ok=True)
        keys = os.path.join(P, "keys", "prod.keys")
        with open(a.keys, errors="replace") as f, open(keys, "w") as o:
            for l in f:
                m = re.match(r"\s*(\w+)\s*=\s*([0-9a-fA-F]+)\s*$", l)
                if m and len(m.group(2)) % 32 == 0:
                    o.write(f"{m.group(1)} = {m.group(2)}\n")
        args = [keys] + ([a.title_keys] if a.title_keys else []) + [a.base] + ([a.update] if a.update else []) + a.dlc
        run([nximport, "--exefs", "--title", "01006F8002326000", "--out", G] + args, log=os.path.join(logs, "import.log"))
        with open(os.path.join(logs, "import.log"), encoding="utf-8", errors="replace") as f:
            print("".join(l for l in f.read().replace("\r", "\n").splitlines(True) if l.startswith("import complete")).strip())
        shutil.copy(os.path.join(G, "analysis", "main.npdm"), os.path.join(G, "exefs", "main.npdm"))
        if sources:
            open(src_file, "w", encoding="utf-8").write(sources + "\n")
    print(f"main.bin {os.path.getsize(os.path.join(G, 'analysis', 'main.bin'))} bytes")

    # ---- mods: a zip with romfs/ at its root (or one folder holding it), extracted to <folder>/mods/<name>/ ---------
    for m in a.mod:
        name, _, zpath = m.partition("=")
        if not name or not os.path.isfile(zpath):
            fail("--mod wants <name>=<zip>")
        say(f"mod {name} ({zpath})")
        dst = os.path.join(G, "mods", name)
        with zipfile.ZipFile(zpath) as z:
            names = [i.filename for i in z.infolist()]
            if any(n.startswith("/") or ".." in n.split("/") for n in names):
                fail("unsafe path in the zip")
            prefix = ""
            if not any(n.startswith("romfs/") for n in names):  # one wrapping folder
                tops = {n.split("/")[0] for n in names if "/romfs/" in n}
                if len(tops) != 1:
                    fail("no romfs/ folder in the zip")
                prefix = tops.pop() + "/"
            shutil.rmtree(dst, ignore_errors=True)
            for i in z.infolist():
                if i.is_dir() or not i.filename.startswith(prefix + "romfs/"):
                    continue
                out = os.path.join(dst, i.filename[len(prefix):])
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with z.open(i) as s, open(out, "wb") as d:
                    shutil.copyfileobj(s, d)

    # ---- version ------------------------------------------------------------------------------------------------
    say("version")
    # Main's build id picks the profile (games/acnh/versions.txt): the hooks file of a known build, none for the others.
    vargs = ["--update", a.update] if a.update else ["--no-update"] if a.base else []
    run([py, "-I", "tools/acnh_version.py", G] + vargs)
    get = lambda k: subprocess.run([py, "-I", "tools/acnh_version.py", G, "--get", k], capture_output=True,
                                   text=True, cwd=ROOT).stdout.strip()
    hooks, version = get("hooks"), get("version")

    # ---- recompile ----------------------------------------------------------------------------------------------
    say("recompile (main, sdk, subsdk0)")
    for mod, base in (("sdk", "0x4010000000"), ("subsdk0", "0x4018000000"), ("main", "0x4000000000")):
        h = []
        if mod == "main" and hooks != "-":
            h = ["--hooks", hooks]  # instructions that call the runtime first, at the addresses of this build only
        pch = os.path.join(G, "speedfix_main.pchtxt")
        if mod == "main" and version == "3.0.2" and os.path.exists(pch):
            h += ["--variant", pch]  # the native 60 FPS variant (tools/setup_60fps.sh, 3.0.2 only)
        profile = " ".join(h) or "none"
        gen = os.path.join(G, "gen", mod)
        table, prof_file = os.path.join(gen, "table.cpp"), os.path.join(gen, "profile.txt")
        if os.path.exists(table) and not os.path.exists(prof_file):  # recompiled before profiles existed: 3.0.2 as is
            open(prof_file, "w").write(profile + "\n")
        stale = (not os.path.exists(table) or force("recomp")
                 or os.path.getmtime(os.path.join(ROOT, "recomp", "emit.py")) > os.path.getmtime(table)
                 or open(prof_file).read().strip() != profile
                 or (hooks != "-" and mod == "main" and os.path.getmtime(os.path.join(ROOT, hooks)) > os.path.getmtime(table)))
        log = os.path.join(logs, f"recomp_{mod}.log")
        if stale:
            run([py, "-m", "recomp", os.path.join(G, "exefs", mod), gen, "--name", mod, "--base", base,
                 "--jobs", str(min(jobs, cores))] + h, log=log)
            open(prof_file, "w").write(profile + "\n")
        print(open(log, encoding="utf-8", errors="replace").read().strip().splitlines()[-1] if os.path.exists(log) else "")

    # ---- shaders ------------------------------------------------------------------------------------------------
    say("shaders (Maxwell -> SPIR-V)")
    summary = os.path.join(G, "spirv", "summary.txt")
    if not os.path.exists(summary) or force("shaders"):
        run([py, "tools/extract_shaders.py", G, "--jobs", str(cores)])
        val = ["--spirv-val", shutil.which("spirv-val")] if shutil.which("spirv-val") else ["--no-val"]
        run([dotnet, harness, "--batch", os.path.join(G, "shaders_extracted"), "--out", os.path.join(G, "spirv"),
             "--jobs", str(cores)] + val, log=os.path.join(logs, "spirv.log"), env=env)
    print(open(summary, encoding="utf-8", errors="replace").readline().strip())

    # ---- fonts --------------------------------------------------------------------------------------------------
    say("fonts (button icons)")
    fonts = os.path.join(G, "fonts")
    os.makedirs(fonts, exist_ok=True)
    # Stand-in for the Switch's button-icon font (type 5), drawn over a host font. The other shared fonts fall back
    # to host fonts (runtime/src/hle_pl.cpp); ACNH ships its own fonts in its RomFS for almost all text.
    if not os.path.exists(os.path.join(fonts, "shared_5.ttf")):
        for f in (r"C:\Windows\Fonts\arial.ttf", "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
                  "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                  "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"):
            if os.path.exists(f):
                run([py, "tools/fonts/make_button_font.py", "--base", f, "--out", os.path.join(fonts, "shared_5.ttf")],
                    log=os.path.join(logs, "button_font.log"))
                break
    # ACNH draws its button prompts with its own ParkExt font (Font/ScalableFont.sarc.zs): a copy per controller
    # style and face-button layout, chosen at boot by controls.glyphs (runtime/src/glyphs.cpp). About 13 MB each.
    if not os.path.exists(os.path.join(fonts, "glyphs_xbox", "console", "romfs", "Font", "ScalableFont.sarc.zs")) or force("glyphs"):
        run([py, "tools/make_button_glyphs.py", "--romfs", os.path.join(G, "extract", "romfs"), "--out", fonts],
            log=os.path.join(logs, "glyphs.log"))

    # ---- build --------------------------------------------------------------------------------------------------
    say(f"build (-j{jobs}; the first build compiles ~5 CPU-hours of code, about {300 // jobs + 10} minutes here)")
    extra = [f"-DNX_GEN_DIR={os.path.join(G, 'gen')}"]
    if WIN:
        extra.append(f"-DCMAKE_PREFIX_PATH={os.path.join(T, f'SDL3-{SDL3}')};{MOVIE_LIBS}")
    elif MAC:
        brew = subprocess.run(["brew", "--prefix"], capture_output=True, text=True).stdout.strip() or "/opt/homebrew"
        extra.append(f"-DCMAKE_PREFIX_PATH={brew}")
    build = os.path.join(G, "build")
    run([cmake, "-S", ".", "-B", build, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}", "-DNX_GAME=acnh",
         "-DCMAKE_BUILD_TYPE=Release", f"-DNX_VULKAN_INCLUDE={os.path.join(vkh, 'include')}"] + cc + extra,
        log=os.path.join(logs, "cmake.log"), env=env)
    run([ninja, "-C", build, f"-j{jobs}", "acnh"], env=env)
    exe = os.path.join(build, "acnh.exe" if WIN else "acnh")
    say(f"done: {exe}   (NX_DATA_ROOT={G})")


if __name__ == "__main__":
    main()
