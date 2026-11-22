#pragma once
#include <cstdint>

namespace nr {

// Which network a runtime runs (local patch, see UPSTREAM.md): one DLL, chosen at run time by the host.
//   rdna4  the FP8 network (VK_EXT_shader_float8), dlssnr-amd/shaders, pipeline.cache
//   rdna3  mauri870's port for gfx11: e4m3 values held as FP16 in the FP16 x FP16 -> FP32 16x16x16 subgroup
//          matrices (linux/shaders/rdna3), dlssnr-amd/shaders-rdna3, pipeline-rdna3.cache
//   rdna2  the same network with its matrix fragments emulated in ALU for parts with no matrix instructions
//          (linux/shaders/rdna2), dlssnr-amd/shaders-rdna2, pipeline-rdna2.cache; no cooperative matrices
enum class Arch : uint8_t { rdna4, rdna3, rdna2 };

// The FP16 networks: no FP8 on the device, so the e4m3 values live in FP16 twins of the arenas, the graph takes
// the Linux network's kernel set, and there is no ACO (but RDNA3's own gfx11 records) and no int4 mixed.
constexpr bool fp16_network(Arch a) { return a != Arch::rdna4; }

constexpr const char* arch_name(Arch a) {
    return a == Arch::rdna3 ? "RDNA3" : a == Arch::rdna2 ? "RDNA2" : "RDNA4";
}
// RDNA2's INT8 network (linux/shaders/rdna2 built with build_network.py --int8): dlssnr-amd/shaders-rdna2-int8,
// pipeline-rdna2-int8.cache. The host sets it before the runtime starts, for an RDNA2 device that has the folder,
// offers shaderIntegerDotProduct and was asked for it (MZ_RDNA2_INT8=1, OptiScaler's MochizukiRdna2Int8).
inline bool rdna2_int8 = false;

inline const char* arch_shaders(Arch a) {
    return a == Arch::rdna3 ? "shaders-rdna3"
         : a == Arch::rdna2 ? (rdna2_int8 ? "shaders-rdna2-int8" : "shaders-rdna2") : "shaders";
}
inline const char* arch_cache(Arch a) {
    return a == Arch::rdna3 ? "pipeline-rdna3.cache"
         : a == Arch::rdna2 ? (rdna2_int8 ? "pipeline-rdna2-int8.cache" : "pipeline-rdna2.cache") : "pipeline.cache";
}

}  // namespace nr
