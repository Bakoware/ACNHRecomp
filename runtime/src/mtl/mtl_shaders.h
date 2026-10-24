// Shaders for the native Metal backend (docs/NATIVE_METAL.md): the SPIR-V the setup translated from Maxwell (after
// the runtime's per-pipeline patches, nvn_spirv_patch.cpp) becomes Metal Shading Language with SPIRV-Cross, one
// argument buffer per descriptor set, and the system's Metal compiler builds it once; MTLBinaryArchive keeps the
// result for the next run. No Vulkan or MoltenVK involved.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nx::mtl {

enum class Stage { Vertex, Fragment, Compute, Geometry };

struct MslShader {
    std::string source;      // MSL 3.0
    std::string entry;       // the entry point's name in `source` (SPIRV-Cross renames main)
    std::string error;       // set when the conversion failed (source then empty)
};

// SPIR-V -> MSL with the backend's resource layout: descriptor set k -> argument buffer [[buffer(k)]].
// Geometry and tessellation stages have no Metal equivalent: an error (MoltenVK lacks them as well).
MslShader msl_from_spirv(const std::vector<uint32_t>& spirv, Stage stage);

}  // namespace nx::mtl
