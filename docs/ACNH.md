# Animal Crossing: New Horizons: what we know about the game

Facts measured on our own dump: base 1.0.0 (`bd68bf29….nca`, SDK 9.3.1) plus the 3.0.2 update (`fd0780de….nca`,
SDK 20.5.17, version `0x210000` = 2162688). Everything here can be reproduced with the tools named next to it.
Derived files stay in `private/acnh/` (gitignored).

## Title

| | |
|---|---|
| Program id | `01006F8002326000` (update `01006F8002326800`) |
| Version targeted | **3.0.2** (base + update). The update's ExeFS is complete; its RomFS is a BKTR patch over the base's |
| main.npdm | main thread priority 44, core 0, stack 1 MiB, **no system resource** (META +0x14 = 0) |
| Engine | Nintendo EPD's in-house engine, called **Park** in file names (`Park_UBER_All`, `Park_Idr`) |

## Modules (`tools/probe.py private/acnh/exefs`)

| Module | Size | Imports / exports | What | Base used |
|---|---|---|---|---|
| `main` | 0x4b57930 (75 MiB) | 1,015 / 52 | the game, Havok Animation + Cloth 2019.1, NEX 4.6.11, Pia 5.31.5, NintendoWare Atk/Font/G3d/Ui2d 9.3.1 and Vfx2 20.5.4, iWnn 1.9.0 (Omron's IME), libcurl, zlib | `0x40_0000_0000` |
| `sdk` | 0xb834e8 | 8 / 23,025 | nnSdk 20.5.4 with NVN 20.5.4 | `0x40_1000_0000` |
| `subsdk0` | 0x552b88 | 663 / 9,446 | `NintendoSDK_movie` 20.5.4 (`nn::movie`) | `0x40_1800_0000` |

`subsdk0` is new compared with Wonder: main imports `nn::movie` from it, so it is recompiled as a third module
(`NX_HAS_SUBSDK0`, `runtime/src/main.cpp`). The three bases fit the runtime's 512 MiB module reservation.

Imports of main + subsdk0 per service: libc 389, c++ 375, nn::audio 186, nn::os 103, nn::hid 60, nn::socket 38,
nn::util 33, nn::fs 32, nn::time 25, nn::ldn 25, nn::fontll 24, nn::prepo 23, nn::account 23, nn::oe 22,
nn::crypto 22, nn::friends 16, nn::bcat 16, nn::sf 13, nn::image 12, nn::nfp 12 (amiibo), nn::nifm 11, ...

## Recompilation (`python -m recomp`)

| Module | Functions | Jump tables | Instructions | Time | Generated |
|---|---|---|---|---|---|
| main | 178,127 | 4,190 | 16,445,201 | ~7 min, ~3 GB RAM | 1,321 files, 1.4 GB |
| sdk | 40,491 | 610 | 1,674,143 | 40 s | 138 files |
| subsdk0 | 13,954 | 334 | 900,179 | 20 s | 73 files |

All with **0 unhandled instructions**. Instructions added for SDK 20 / ACNH (`recomp/emit_sdk20.py`), each checked
against Unicorn with `tests/difftest.py --filter ...` (0 failing shapes):

- `sqshl` (vector immediate, scalar and vector register), `sqrshl`, `uqshl`, `sqabs`, `sqneg`, `saddlv`
- `sli`, `sri` (shift and insert), `pmull`/`pmull2` on bytes (8B to 8H), `tbx` (table lookup that keeps lanes)
- `bfc`, `umnegl`, `smnegl`
- `csdb` (speculation barrier) and MTE's `stg` as no-ops

Analysis fix found on ACNH (it applies to every game): functions that share a tail share its jump tables, and a
table can reach back before a later entry (`main` 0x3aeaf14 runs into `br` 0x3aeb000, whose case 0 is 0x3aeaf0c,
in the function before). Those targets are now promoted to entries and tail-called (`recomp/analysis.py`,
`recomp/emit.py` `i_br`). Without it the C++ did not compile (`goto` to a label of another function); the `sdk`
had 6 such targets too.

## RomFS (3.0.2 applied)

103,400 files, 242 folders, 10.5 GB. Compression is **zstd** (`.zs`, 61k files) and **Yaz0** (`.szs`):
`tools/maxwell/yaz0.py` (with a small C decoder built on first use) handles Yaz0, including Yaz0 members nested in
SARC archives.

## Shaders (`tools/extract_shaders.sh`, `tools/maxwell/containers.py`)

| Where | Container chain | Notes |
|---|---|---|
| `Shader/Park_UBER_All.Nin_NX_NVN.szs` | Yaz0 → SARC → FSHA → BNSH | 6,520 programs, the main material shader |
| `Shader/Park_Proc.Nin_NX_NVN.release.szs` | Yaz0 → SARC → BAHS (agl) | |
| `Shader/apkg.Nin_NX_NVN.release.szs` | Yaz0 → SARC → BNSH | |
| `Effect/Park_{Idr,Static}.Nin_NX_NVN.szs` | Yaz0 → SARC → VFXB (`0x26`) → **GRSN / GRSC → FSHA** → BNSH | VFXB sections hold a shader *archive* here, not a bare BNSH as in Wonder |
| `System/GSys/gsys_resource.Nin_NX_NVN.release.szs` | Yaz0 → SARC → BAHS | |
| `Swkbd/shader/*_Nx.arc.szs` | Yaz0 → SARC → BNSH | the game's own software keyboard |
| `Layout/*.Nin_NX_NVN.zs` | zstd → SARC → BNSH (`bgsh`) | 934 of 960 layouts |
| `main` | raw blocks (`tools/maxwell/exefs_shaders.py`) | 35 |

**SDK 20 compute shaders have no NVN header**: the code block starts straight with Maxwell code (the control
section's program start, +0x708, is 0), where SDK 16 had the `0x12345679` header and a start of 0x100. The
container check and `spirv_harness` recognise both. A few **geometry** stages exist (16 stages, 4 blocks).

Result of `spirv_harness --batch`: **6,200 distinct blocks (23,455 stages), 6,172 ok, 28 ok-retry, 0 invalid,
0 exceptions**, validated with `spirv-val` (vulkan1.2), in about 70 s on 6 jobs.

## Game-specific notes

- **Keyboard:** ACNH ships its own software keyboard (`Swkbd/`: iWnn dictionaries `NJDC`/`NJAD`, layouts, shaders),
  so naming the island and the player does not go through the system's swkbd applet.
- **Clock:** the game's day and season come from `nn::time` (the user system clock). The runtime answers it from the
  host clock (`runtime/src/svc_time.cpp`), so the island follows the computer's date and time.
- **Online:** NEX/Pia/friends/bcat/nifm are answered offline (no network connection), as in Hades and Wonder.
