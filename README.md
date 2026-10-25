# ACNHRecomp for Windows

A native Windows build of *Animal Crossing: New Horizons*, made by static recompilation: the game's AArch64 code is
translated ahead of time into C++ and compiled for your PC, the system services it calls are reimplemented, and its
NVN graphics are drawn with Vulkan. It is not an emulator.

> [!IMPORTANT]
> **No game files are included.** This repository holds no code, assets, keys or firmware from the game. You need
> your own copy of the game dumped from your own console, with that console's keys; the build is made on your PC
> from your dump. Never share the resulting executable or data folder: they contain the recompiled game.

## Versions that work

| Version | Picture and sound | Mouse as touch screen | PC keyboard typing | 60 FPS, wide screens |
|---|---|---|---|---|
| 3.0.3 | Yes | Yes | Yes | Yes |
| 2.0.7 | Yes | Yes | Yes | No |
| 1.0.0 | Yes | Yes | No (type on the on-screen keys) | No |

Add-on content (DLC) dumped from the same console loads with any of them. Other versions build too, without the
per-version fixes ([docs/VERSIONS.md](docs/VERSIONS.md)).

## Touch with the mouse

The mouse works as the touch screen:

- a click on a window's buttons (confirmations, yes / no) presses them;
- a click on a list item (menus, dialogue choices) moves the cursor there and confirms it;
- a click anywhere while a message waits for its button advances the text.

## Requirements

- Windows 10 or 11, 64-bit, with a Vulkan GPU driver.
- Python 3.11 or newer and the Visual Studio C++ build tools ("Desktop development with C++").
- 16 GB of RAM and about 30 GB free.
- From your console: `prod.keys` (and `title.keys`), the base game NSP, optionally an update NSP and DLC NSPs.

```powershell
winget install Python.Python.3.13
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

The setup downloads the rest (LLVM, SDL3, CMake, Ninja, Vulkan headers, Dear ImGui, the shader translator) into
`private\third_party`.

## Setup

One folder per version. Keep your keys and dumps outside the repository, or in `private\` (never committed):

```powershell
python tools\setup_acnh.py --keys prod.keys --title-keys title.keys --base base.nsp --update update.nsp `
    --dlc dlc1.nsp --dlc dlc2.nsp --out private\acnh-3.0.3
```

Each stage (import, recompile, shaders, button icons, build) is skipped when its output is already there, so the
same command can be run again after a failure. Adding DLC later imports the game again: pass the base and update
NSPs with the new `--dlc` files.

## Playing

```powershell
$env:NX_DATA_ROOT = "private\acnh-3.0.3"
private\acnh-3.0.3\build\acnh.exe
```

- **F1** (or Minus + Plus on a controller) opens the settings screen: display, rendering, frame rate, controls,
  language, saves.
- **F11** or **Alt+Enter** toggles fullscreen.
- Any controller SDL3 recognises works (Xbox, PlayStation, Switch Pro Controller and Joy-Con, generic pads), with a
  Switch or Xbox confirm-button layout and matching button icons.

Default keyboard layout:

| Console | Keyboard |
|---|---|
| Left stick | W A S D |
| Right stick (camera) | I J K L |
| D-pad | Arrow keys |
| A / B | Space / Left Shift |
| X / Y | X / F |
| L / R | Q / E |
| ZL / ZR | Z / C |
| Plus / Minus | Enter / Escape |

Useful environment variables: `NX_FULLSCREEN=1`, `NX_RESOLUTION=WxH`, `NX_HANDHELD=1` (720p), `NX_FSR=1`,
`NX_GPU=integrated` / `discrete`, `NX_MUTE=1`, `NX_LOG=<file>`, `NX_MEM_LIMIT_GB=<n>`.

## Saves

The island is saved in `<data folder>\user\device_save\`. The settings screen's Saves page imports a JKSV or
Checkpoint backup, a yuzu or Ryujinx save or another ACNHRecomp folder, backs up the current save before every
import, and exports in the layout JKSV and Checkpoint restore on a console.

## Documentation

- [docs/VERSIONS.md](docs/VERSIONS.md): the versions, their build ids and how to port the fixes to another one.
- [docs/ACNH.md](docs/ACNH.md): what is known about the game's modules, data and engine.
- [docs/NATIVE60.md](docs/NATIVE60.md): 60 FPS.

## Disclaimer

ACNHRecomp is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by the game's
publisher or the console's maker. *Animal Crossing* is a trademark of its owner. The project does not provide, and
will not help you obtain, game files, keys or firmware.

## Credits

- **The engine:** the static recompilation framework this project is built on (the AArch64 recompiler, the runtime,
  NVN on Vulkan, the importer and the shader translation pipeline) is SuperMarioWonderRecomp by
  [BelmanteGu](https://github.com/BelmanteGu), MIT.
- **The ACNH port:** [ACNHRecomp](https://github.com/Nycolazs/ACNHRecomp) by
  [Nycolas Rocha](https://github.com/Nycolazs), MIT: the game's runtime support, its fixes, the settings screen, the
  launcher and the setup this Windows build continues from.
- [Ryujinx / Ryubing](https://codeberg.org/Software_arch/Ryubing) (MIT): the Maxwell shader translator, used offline.
- [ACNH 60 FPS + SpeedFix](https://github.com/tmouh/acnh-60fps) by tmouh (CC BY-NC 4.0): the 60 FPS game-speed
  rescaling, downloaded by `tools/setup_60fps.sh` and applied on your machine (not redistributed here).
- [Dear ImGui](https://github.com/ocornut/imgui), [SDL3](https://github.com/libsdl-org/SDL),
  [Capstone](https://www.capstone-engine.org/), [Unicorn](https://www.unicorn-engine.org/), libvpx, libvorbis and
  AMD FidelityFX FSR 1.

## License

Released under the [MIT License](LICENSE). The code inherited from the projects above keeps their copyright
notices (LICENSE). The license covers this project's source code only and grants no rights to the game.
