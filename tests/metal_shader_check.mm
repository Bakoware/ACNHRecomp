// Every SPIR-V shader of a game through the Metal backend's path: SPIR-V -> MSL (mtl_shaders.cpp) -> the system's
// Metal compiler. Reports what converts and compiles, and the errors by kind (docs/NATIVE_METAL.md, step 1).
// usage: metal_shader_check <game data dir> [max]   (reads <dir>/spirv/*.spv)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "mtl/mtl_shaders.h"

namespace fs = std::filesystem;
using nx::mtl::Stage;

static Stage stage_of(const std::vector<uint32_t>& w) {
    for (size_t i = 5; i < w.size();) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len) break;
        if (op == 15 && len > 1) {  // OpEntryPoint: execution model
            switch (w[i + 1]) {
                case 0: return Stage::Vertex;
                case 4: return Stage::Fragment;
                case 5: return Stage::Compute;
                default: return Stage::Geometry;  // geometry and tessellation: no Metal stage
            }
        }
        i += len;
    }
    return Stage::Vertex;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2) {
            std::fprintf(stderr, "usage: %s <game data dir> [max]\n", argv[0]);
            return 2;
        }
        size_t max = argc > 2 ? std::stoul(argv[2]) : SIZE_MAX;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        MTLCompileOptions* opts = [MTLCompileOptions new];
        opts.languageVersion = MTLLanguageVersion3_0;
        size_t total = 0, converted = 0, compiled = 0, geometry = 0;
        std::map<std::string, int> errors;
        auto t0 = std::chrono::steady_clock::now();
        for (const auto& e : fs::directory_iterator(fs::path(argv[1]) / "spirv")) {
            if (e.path().extension() != ".spv" || total >= max) continue;
            total++;
            std::ifstream f(e.path(), std::ios::binary);
            std::vector<uint32_t> w(fs::file_size(e.path()) / 4);
            f.read(reinterpret_cast<char*>(w.data()), std::streamsize(w.size() * 4));
            Stage st = stage_of(w);
            if (st == Stage::Geometry) {
                geometry++;
                continue;
            }
            nx::mtl::MslShader s = nx::mtl::msl_from_spirv(w, st);
            if (s.source.empty()) {
                errors["convert: " + s.error.substr(0, 80)]++;
                continue;
            }
            converted++;
            NSError* err = nil;
            id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:s.source.c_str()]
                                                   options:opts
                                                     error:&err];
            if (lib && [lib newFunctionWithName:[NSString stringWithUTF8String:s.entry.c_str()]]) {
                compiled++;
            } else {
                std::string m = err ? err.localizedDescription.UTF8String : "no entry point";
                size_t at = m.find("error:");
                errors["compile: " + m.substr(at == std::string::npos ? 0 : at, 80)]++;
            }
        }
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%zu shaders: %zu converted, %zu compiled on %s, %zu geometry (no Metal stage); %.1f s\n", total,
                    converted, compiled, dev.name.UTF8String, geometry, s);
        for (auto& [k, n] : errors) std::printf("  %5d %s\n", n, k.c_str());
        return compiled + geometry == total ? 0 : 1;
    }
}
