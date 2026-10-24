# Native graphics per platform: Metal on Apple, Vulkan elsewhere

Goal (2026-10-24): the game runs natively on each platform, with no translation layer between our NVN
implementation and the system's graphics API. The game's CPU code is already native (recompiled ahead of time).
Graphics today go NVN → Vulkan → MoltenVK → Metal on macOS, and the Maxwell shaders reach Metal as SPIR-V that
MoltenVK converts while the game runs. Target:

| Platform | API | Shaders |
|---|---|---|
| macOS (Apple Silicon), later iOS/iPadOS | **Metal**, no MoltenVK | MSL, compiled once by the system's Metal compiler, kept in a `MTLBinaryArchive` |
| Linux, Windows, Android | Vulkan (the native API there) | SPIR-V, as now |

The game was written for NVN (the Switch's API): without its source, some layer must implement NVN. "Native" here
means that layer talks to the system API directly.

## Why it pays off (measured)

M1, 60 FPS mode, river bank, busy CPU samples: our NVN → Vulkan layer ~33% (descriptor sets rebuilt per draw,
uploads, state tracking), MoltenVK 7%, game code 19%, rest of the runtime 14%, system and driver 27%. NVN binds
textures by handle in pools (bindless); Vulkan forces descriptor sets per draw, Metal argument buffers and
`gpuResourceID`s match NVN's model. A Metal backend built that way drops MoltenVK and most of the descriptor work.

## Plan

Built beside the Vulkan backend (`NX_GPU_BACKEND=metal`), switched on by default on macOS once it matches it.

1. **Shaders** ✅ (2026-10-24, `runtime/src/mtl/mtl_shaders.cpp`, `metal_shader_check`: ACNH's 9,452 shaders →
   9,448 converted and compiled by the system's Metal compiler on the M1 in 378 s; the 4 others are geometry
   shaders, which Metal has no stage for, as MoltenVK does not either). Details: all SPIR-V the setup produces → MSL with SPIRV-Cross; compiled by
   `MTLDevice newLibraryWithSource` on first use, cached in a binary archive. Sample of 400: 400 converted, 400
   compiled (38 ms each on the M1; the 9,454 in ~6 min, once). Left: the per-pipeline SPIR-V patches
   (`nvn_spirv_patch.cpp`: NoContraction, clip distances, R11G11B10, ...) applied before the conversion, and the
   resource layout (argument buffers matching NVN's texture/sampler pools).
2. **Device and present**: `CAMetalLayer` on the SDL window, the final image presented, FSR/frame generation as Metal
   compute.
3. **Copies, clears, uploads**: tiling and format tables shared with Vulkan (`nvn_tiling`, `nvn_format`).
4. **Draws**: render passes from NVN targets, pipelines (`MTLRenderPipelineState` + depth/stencil states), argument
   buffers for the pools, vertex fetch.
5. **Parity**: the same scenes dumped with both backends from a save (`tools/play`), compared image by image.
6. **Default on macOS**, MoltenVK dropped from the Mac build.

The code that only understands NVN (command recording, objects, formats, tiling, shader reflection) is shared; the
Vulkan-specific parts (`nvn_vk`, `nvn_pipeline`, `nvn_sets`, `nvn_present`, ...) get Metal counterparts.
