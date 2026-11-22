// Native RDNA2 body of the grouped C=512 feed-forward (ffwd3_t.comp, CCSplitSwin16HFfwd): NR_NATIVE=1 in pipelines.json.
//
// Same bindings, push block, grid (group-major: a workgroup is one of the eight 64-channel groups over 64 tokens),
// weights (the FP16 twin, two 16-row tiles a record) and output, the same tile-counter wait and the same signal count,
// and the same arithmetic: each of the three products is one FP32 sum through NR_DOT2 over the k pairs in k order from
// 0, every rounding the emulated one element by element. One lane a token, NR_FFWD_WGW * NR_FFWD_FM / 2 waves (the
// workgroup's 16 x NR_FFWD_WGW x NR_FFWD_FM tokens); the group's weights
// are the same for every lane: scalar uvec4 loads into v_dot2.
//
//   1. r[64]   = e4m3(W1 . x), K = C, x read per lane (tile-blocked rows)
//   2. h[256]  = e4m3(act(W2 . r)), 16 hidden at a time ...
//   3. y[64]  += W3 . h, ... streamed into the 64 sums (the hidden index is the k of this product, in order)
//   out        = e4m3(y)
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require

#if (NR_FFWD_FM != 1 && NR_FFWD_FM != 2) || !NR_FFWD_KOFF || !NR_FFWD_PACK || !NR_FFWD_PAIR || NR_FFWD_F32 || !NR_FFWD_GMAJOR || \
    NR_FFWD_WGW != 4 || NR_DIAG_FFWD_HALFW
#error "NR_NATIVE: ffwd3 and ffwd3w (FM=1/2, packed, paired, group-major, four waves) only"
#endif

#ifndef NV_I8
#define NV_I8 0
#endif
#if NV_I8
#include "i8.glsl"
#endif

layout(binding = 1, std430) readonly buffer NfAct4 { uvec4 nf_x4[]; };
layout(binding = 3, std430) readonly buffer NfWgt4 { uvec4 nf_w4[]; };
layout(binding = 1, std430) writeonly buffer NfOut4 { uvec4 nf_o4[]; };

f16vec2 nf_h(uint u) { return unpackFloat2x16(u); }
uint nf_p(f16vec2 v) { return packFloat2x16(v); }

float nf_dot8(float s, uvec4 x0, uvec4 x1, uvec4 w0, uvec4 w1) {
    s = NR_DOT2(s, nf_h(w0.x), nf_h(x0.x)); s = NR_DOT2(s, nf_h(w0.y), nf_h(x0.y));
    s = NR_DOT2(s, nf_h(w0.z), nf_h(x0.z)); s = NR_DOT2(s, nf_h(w0.w), nf_h(x0.w));
    s = NR_DOT2(s, nf_h(w1.x), nf_h(x1.x)); s = NR_DOT2(s, nf_h(w1.y), nf_h(x1.y));
    s = NR_DOT2(s, nf_h(w1.z), nf_h(x1.z)); s = NR_DOT2(s, nf_h(w1.w), nf_h(x1.w));
    return s;
}
// Row `row`, k fragment kf of a packed [rows][K] matrix at `base` (halves): two 16-row tiles a 32-row record.
uint nf_wrow(uint base, uint K, uint row, uint kf) {
    return base / 8u + ((row >> 5u) * (K / 16u) + kf) * 64u + ((row >> 4u) & 1u) * 32u + (row & 15u) * 2u;
}

#if NV_I8
// NV_I8 (the RDNA2 INT8 network): the three products in int8 (i8.glsl), a bit each: 1 W1 (x one scale a token over its C
// channels, a first pass over x for it), 2 W2 (r one scale a token), 4 W3 (the hidden units one scale per 32). The
// weights of the int8 products are int8 in the twin (host roles kI8Ffwd1/2/3); the output as the FP16 body's.
#define NF_I8W1 ((NV_I8 & 1) != 0)
#define NF_I8W2 ((NV_I8 & 2) != 0)
#define NF_I8W3 ((NV_I8 & 4) != 0)
layout(binding = 3, std430) readonly buffer NfWgtF { float nf_wf[]; };

void main() {
    NR_PROF_BEGIN()
    nr_chain_wait()
    NR_TC_PROLOGUE
    nr_tchain_wait(NR_FFWD_T0(16u * NR_FFWD_FM), NR_FFWD_NT(16u * NR_FFWD_FM))
    const uint lane = gl_SubgroupInvocationID;
    const uint wave = subgroupBroadcastFirst(gl_SubgroupID);
    const uint group = gl_WorkGroupID.x % 8u;
    const uint tile0 = (gl_WorkGroupID.x / 8u) * uint(NR_FFWD_WGW * NR_FFWD_FM) + 2u * wave;   // the wave's first 16-token tile
    const uint tok = tile0 * 16u + lane;
    const bool live = (tok & ~15u) < pc.M;          // the emulated wave of this tile exists
    const uint tl = live ? tok : 0u;                // reads stay inside the tensor
    const uint agrp = pc.a_off + group * 64u * 512u, q0grp = pc.q0_off + group * 256u * 64u,
               q2grp = pc.q2_off + group * 64u * 256u;
    const uint KF = pc.C / 16u;
    const uint xq = (pc.x_off + (tl >> 4u) * KF * 256u + (tl & 15u) * 16u) / 8u;

    // 1. r = W1 . x: 64 rows, k over C
    float a1[64];
    if (NF_I8W1) {
        float amax = 0.0;
        for (uint kf = 0u; kf < KF; kf = kf + 1u) {
            const uvec4 x0 = nf_x4[xq + kf * 32u], x1 = nf_x4[xq + kf * 32u + 1u];
            for (int e = 0; e < 4; ++e) {
                const vec2 f0 = vec2(nf_h(x0[e])), f1 = vec2(nf_h(x1[e]));
                amax = max(amax, max(max(abs(f0.x), abs(f0.y)), max(abs(f1.x), abs(f1.y))));
            }
        }
        const int sx = i8_exp(amax);
        const uint fb = i8_floor(sx);
        const float inv = i8_pow2(-sx);
        int ai[64];
        for (int n = 0; n < 64; ++n) ai[n] = 0;
        for (uint kf = 0u; kf < KF; kf = kf + 1u) {
            const uvec4 x0 = nf_x4[xq + kf * 32u], x1 = nf_x4[xq + kf * 32u + 1u];
            uvec4 q;
            for (int e = 0; e < 2; ++e) {
                const vec2 f0 = vec2(nf_h(x0[2 * e])), f1 = vec2(nf_h(x0[2 * e + 1]));
                q[e] = i8_pack(i8_q(f0.x, fb, inv), i8_q(f0.y, fb, inv), i8_q(f1.x, fb, inv), i8_q(f1.y, fb, inv));
                const vec2 g0 = vec2(nf_h(x1[2 * e])), g1 = vec2(nf_h(x1[2 * e + 1]));
                q[2 + e] = i8_pack(i8_q(g0.x, fb, inv), i8_q(g0.y, fb, inv), i8_q(g1.x, fb, inv), i8_q(g1.y, fb, inv));
            }
            for (int n = 0; n < 64; ++n)
                ai[n] = i8_dot16(ai[n], nf_w4[I8_WROW(pc.a_off, KF, group * 64u + uint(n), kf)], q);
        }
        for (int n = 0; n < 64; ++n)
            a1[n] = float(ai[n]) * (i8_pow2(sx) * I8_WSCALE(nf_wf, pc.a_off, 512u, pc.C, group * 64u + uint(n)));
    } else {
        for (int n = 0; n < 64; ++n) a1[n] = 0.0;
        for (uint kf = 0u; kf < KF; kf = kf + 1u) {
            const uvec4 x0 = nf_x4[xq + kf * 32u], x1 = nf_x4[xq + kf * 32u + 1u];
            for (int n = 0; n < 64; ++n) {
                const uint wq = nf_wrow(agrp, pc.C, uint(n), kf);
                a1[n] = nf_dot8(a1[n], x0, x1, nf_w4[wq], nf_w4[wq + 1u]);
            }
        }
    }
    // r on the e4m3 grid (FP16), or int8 with the token's scale
    uvec4 r[8];
    int sr = 0;
    if (NF_I8W2) {
        float amax = 0.0;
        for (int n = 0; n < 64; ++n) amax = max(amax, abs(a1[n]));
        sr = i8_exp(amax);
        const uint fb = i8_floor(sr);
        const float inv = i8_pow2(-sr);
        for (int i = 0; i < 4; ++i)
            for (int e = 0; e < 4; ++e)
                r[i][e] = i8_pack(i8_q(a1[16 * i + 4 * e], fb, inv), i8_q(a1[16 * i + 4 * e + 1], fb, inv),
                                  i8_q(a1[16 * i + 4 * e + 2], fb, inv), i8_q(a1[16 * i + 4 * e + 3], fb, inv));
    } else {
        for (int i = 0; i < 8; ++i) {
            uint u[4];
            for (int e = 0; e < 4; ++e) u[e] = nf_p(nr_ffwd_quant(f16vec2(a1[8 * i + 2 * e], a1[8 * i + 2 * e + 1])));
            r[i] = uvec4(u[0], u[1], u[2], u[3]);
        }
    }

    // 2 and 3, 32 hidden units at a time
    float a3[64];
    for (int n = 0; n < 64; ++n) a3[n] = 0.0;
    const uint HID = pc.C / 2u;                     // 256
    for (uint jb = 0u; jb < HID; jb = jb + 32u) {
        float hv[32];                               // act(W2 . r) of the 32 units, FP16 as the FP16 body has it
        for (int jj = 0; jj < 32; jj += 2) {
            float a2[2];
            for (int e = 0; e < 2; ++e) {
                const uint row = jb + uint(jj + e);
                if (NF_I8W2) {
                    int d = 0;
                    for (int kf = 0; kf < 4; ++kf) d = i8_dot16(d, nf_w4[I8_WROW(pc.q0_off, 4u, group * 256u + row, uint(kf))], r[kf]);
                    a2[e] = float(d) * (i8_pow2(sr) * I8_WSCALE(nf_wf, pc.q0_off, 2048u, 64u, group * 256u + row));
                } else {
                    float s = 0.0;
                    for (int kf = 0; kf < 4; ++kf) {
                        const uint wq = nf_wrow(q0grp, 64u, row, uint(kf));
                        s = nf_dot8(s, r[2 * kf], r[2 * kf + 1], nf_w4[wq], nf_w4[wq + 1u]);
                    }
                    a2[e] = s;
                }
            }
            const vec2 h2 = vec2(nr_swin_act2(f16vec2(a2[0], a2[1])));
            hv[jj] = h2.x; hv[jj + 1] = h2.y;
        }
        if (NF_I8W3) {
            uint hq[8];
            const float hs = i8_vec32(hv, hq);
            const uvec4 h0 = uvec4(hq[0], hq[1], hq[2], hq[3]), h1 = uvec4(hq[4], hq[5], hq[6], hq[7]);
            for (int n = 0; n < 64; ++n) {
                const uint row = group * 64u + uint(n);
                const int d = i8_dot16(i8_dot16(0, nf_w4[I8_WROW(pc.q2_off, 16u, row, jb / 16u)], h0),
                                       nf_w4[I8_WROW(pc.q2_off, 16u, row, jb / 16u + 1u)], h1);
                a3[n] = fma(float(d), hs, a3[n]);
            }
        } else {
            for (int hb = 0; hb < 2; ++hb) {
                uint hq[8];
                for (int p = 0; p < 8; ++p) hq[p] = nf_p(nr_ffwd_quant(f16vec2(hv[16 * hb + 2 * p], hv[16 * hb + 2 * p + 1])));
                const uvec4 h0 = uvec4(hq[0], hq[1], hq[2], hq[3]), h1 = uvec4(hq[4], hq[5], hq[6], hq[7]);
                for (int n = 0; n < 64; ++n) {
                    const uint wq = nf_wrow(q2grp, HID, uint(n), jb / 16u + uint(hb));
                    a3[n] = nf_dot8(a3[n], h0, h1, nf_w4[wq], nf_w4[wq + 1u]);
                }
            }
        }
    }
    if (NF_I8W3)
        for (int n = 0; n < 64; ++n) a3[n] *= I8_WSCALE(nf_wf, pc.q2_off, 512u, 256u, group * 64u + uint(n));

    if (live) {
        const uint ob = (pc.o_off + (tok >> 4u) * KF * 256u + group * 4u * 256u + (tok & 15u) * 16u) / 8u;
        for (int n = 0; n < 4; ++n) {
            uint u[8];
            for (int e = 0; e < 8; ++e) u[e] = nf_p(nr_ffwd_quant(f16vec2(a3[16 * n + 2 * e], a3[16 * n + 2 * e + 1])));
            nf_o4[ob + uint(n) * 32u]      = uvec4(u[0], u[1], u[2], u[3]);
            nf_o4[ob + uint(n) * 32u + 1u] = uvec4(u[4], u[5], u[6], u[7]);
        }
    }
    nr_chain_signal()
    // the emulated waves signal each of their tiles once: so do these, the live ones of their two
    const uint nlive = uint((tile0 * 16u) < pc.M) + uint((tile0 * 16u + 16u) < pc.M);
    if (nlive != 0u) nr_tchain_signal_wave(tile0, nlive)
    NR_PROF_END(pc.o_off)
}
#else

void main() {
    NR_PROF_BEGIN()
    nr_chain_wait()
    NR_TC_PROLOGUE
    nr_tchain_wait(NR_FFWD_T0(16u * NR_FFWD_FM), NR_FFWD_NT(16u * NR_FFWD_FM))
    const uint lane = gl_SubgroupInvocationID;
    const uint wave = subgroupBroadcastFirst(gl_SubgroupID);
    const uint group = gl_WorkGroupID.x % 8u;
    const uint tile0 = (gl_WorkGroupID.x / 8u) * uint(NR_FFWD_WGW * NR_FFWD_FM) + 2u * wave;   // the wave's first 16-token tile
    const uint tok = tile0 * 16u + lane;
    const bool live = (tok & ~15u) < pc.M;          // the emulated wave of this tile exists
    const uint tl = live ? tok : 0u;                // reads stay inside the tensor
    const uint agrp = pc.a_off + group * 64u * 512u, q0grp = pc.q0_off + group * 256u * 64u,
               q2grp = pc.q2_off + group * 64u * 256u;
    const uint KF = pc.C / 16u;
    const uint xq = (pc.x_off + (tl >> 4u) * KF * 256u + (tl & 15u) * 16u) / 8u;

    // 1. r = e4m3(W1 . x): 64 rows, k over C
    float a1[64];
    for (int n = 0; n < 64; ++n) a1[n] = 0.0;
    for (uint kf = 0u; kf < KF; kf = kf + 1u) {
        const uvec4 x0 = nf_x4[xq + kf * 32u], x1 = nf_x4[xq + kf * 32u + 1u];
        for (int n = 0; n < 64; ++n) {
            const uint wq = nf_wrow(agrp, pc.C, uint(n), kf);
            a1[n] = nf_dot8(a1[n], x0, x1, nf_w4[wq], nf_w4[wq + 1u]);
        }
    }
    uvec4 r[8];
    for (int i = 0; i < 8; ++i) {
        uint u[4];
        for (int e = 0; e < 4; ++e) u[e] = nf_p(nr_ffwd_quant(f16vec2(a1[8 * i + 2 * e], a1[8 * i + 2 * e + 1])));
        r[i] = uvec4(u[0], u[1], u[2], u[3]);
    }

    // 2 and 3, sixteen hidden units at a time
    float a3[64];
    for (int n = 0; n < 64; ++n) a3[n] = 0.0;
    const uint HID = pc.C / 2u;                     // 256
    for (uint jb = 0u; jb < HID; jb = jb + 16u) {
        uint hq[8];
        for (int jj = 0; jj < 16; jj += 2) {
            float a2[2];
            for (int e = 0; e < 2; ++e) {
                float s = 0.0;
                for (int kf = 0; kf < 4; ++kf) {
                    const uint wq = nf_wrow(q0grp, 64u, jb + uint(jj + e), uint(kf));
                    s = nf_dot8(s, r[2 * kf], r[2 * kf + 1], nf_w4[wq], nf_w4[wq + 1u]);
                }
                a2[e] = s;
            }
            hq[jj / 2] = nf_p(nr_ffwd_quant(nr_swin_act2(f16vec2(a2[0], a2[1]))));
        }
        const uvec4 h0 = uvec4(hq[0], hq[1], hq[2], hq[3]), h1 = uvec4(hq[4], hq[5], hq[6], hq[7]);
        for (int n = 0; n < 64; ++n) {
            const uint wq = nf_wrow(q2grp, HID, uint(n), jb / 16u);
            a3[n] = nf_dot8(a3[n], h0, h1, nf_w4[wq], nf_w4[wq + 1u]);
        }
    }

    if (live) {
        const uint ob = (pc.o_off + (tok >> 4u) * KF * 256u + group * 4u * 256u + (tok & 15u) * 16u) / 8u;
        for (int n = 0; n < 4; ++n) {
            uint u[8];
            for (int e = 0; e < 8; ++e) u[e] = nf_p(nr_ffwd_quant(f16vec2(a3[16 * n + 2 * e], a3[16 * n + 2 * e + 1])));
            nf_o4[ob + uint(n) * 32u]      = uvec4(u[0], u[1], u[2], u[3]);
            nf_o4[ob + uint(n) * 32u + 1u] = uvec4(u[4], u[5], u[6], u[7]);
        }
    }
    nr_chain_signal()
    // the emulated waves signal each of their tiles once: so do these, the live ones of their two
    const uint nlive = uint((tile0 * 16u) < pc.M) + uint((tile0 * 16u + 16u) < pc.M);
    if (nlive != 0u) nr_tchain_signal_wave(tile0, nlive)
    NR_PROF_END(pc.o_off)
}
#endif
