// SPIR-V -> MSL for the Metal backend (mtl_shaders.h).
#include "mtl/mtl_shaders.h"

#include <spirv_msl.hpp>

namespace nx::mtl {

MslShader msl_from_spirv(const std::vector<uint32_t>& spirv, Stage stage) {
    MslShader out;
    if (stage == Stage::Geometry) {
        out.error = "geometry shaders have no Metal equivalent";
        return out;
    }
    try {
        spirv_cross::CompilerMSL msl(spirv);
        spirv_cross::CompilerMSL::Options o;
        o.set_msl_version(3, 0);
        o.platform = spirv_cross::CompilerMSL::Options::macOS;
        o.argument_buffers = true;  // one per descriptor set, at [[buffer(set)]]
        o.argument_buffers_tier = spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier1;
        o.force_active_argument_buffer_resources = true;  // the same layout whatever the shader reads
        o.texture_buffer_native = true;
        msl.set_msl_options(o);
        spirv_cross::CompilerGLSL::Options g = msl.get_common_options();
        g.vertex.flip_vert_y = false;  // NVN's clip space is handled by the viewport, as in the Vulkan backend
        msl.set_common_options(g);
        out.source = msl.compile();
        auto entries = msl.get_entry_points_and_stages();
        if (!entries.empty()) out.entry = msl.get_cleansed_entry_point_name(entries[0].name, entries[0].execution_model);
    } catch (const std::exception& e) {
        out.source.clear();
        out.error = e.what();
    }
    return out;
}

}  // namespace nx::mtl
