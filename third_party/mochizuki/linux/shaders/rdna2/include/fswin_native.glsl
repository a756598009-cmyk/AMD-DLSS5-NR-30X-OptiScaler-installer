// Native RDNA2 bodies of the fused Swin layer: NR_NATIVE=1 in pipelines.json.
//   NR_HWAVES (C >= 64, fswin64/128/256): one wave a head, 32 x NR_HEADS invocations, one window a workgroup.
//   C = 32 (fswin32/fswin32nh): the one-head body, two waves of 32 tokens.
//
// Same bindings, push block, window grid, weights and output as the emulated body (fswin_body.glsl through
// coopmm.glsl), and the same arithmetic: every product is an FP16 pair summed into FP32 by NR_DOT2 (v_dot2_f32_f16)
// over the same k pairs in the same order, every rounding is the same function on the same value, and the
// reductions that are not dot products (the Q/K norms, the softmax sum) are done in the emulated body's order.
// Measured: byte-identical network output. What changes is where the data lives:
//
//   * one lane per token, so a weight is the same for every lane and is read as an aligned uvec4 at a uniform
//     address (s_buffer_load_dwordx4), going straight into v_dot2 as a scalar operand. No cross-lane traffic in
//     any projection. C >= 64: lane l owns tokens l and l + 32 of the 8x8 window (its two slots), so each scalar
//     weight feeds two v_dot2. C = 32: wave w owns tokens 32w + l, one each.
//   * C >= 64: activations in LDS as [k fragment][8-channel half][token] uvec4, so a lane's 8 channels are one
//     ds_read_b128 and a wave's 32 reads are consecutive (no bank conflict). C = 32: in registers.
//   * attention per head: K and V of the head in LDS, read by all lanes at one address (broadcast), the softmax in
//     two passes over the keys (sum, then normalise and multiply by V) so nothing is indexed dynamically.
//
// The k-pair swap of the FP16 host (`kswap`, nr_graph.cpp): the emulated body multiplies the odd rows of some
// weight tiles, stored with every k pair swapped, by operands it built in registers with the same swap, and the
// odd rows of its register-built activation x activation products have both operands swapped. NR_DOT2 is then
// given the pair in the other order; here the same pairs are given in the same order (`.yx`), so the products and
// their order are the emulated body's exactly.
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_shuffle : require
#extension GL_KHR_shader_subgroup_ballot : require

// NV_I8 (the RDNA2 INT8 network, pipelines.json "int8"): which of the body's products run in int8 (i8.glsl), a bit
// each: 1 E, 2 Ct, 4 the QKV projection, 8 Wo, 16 Q . K, 32 Wmid (C >= 64). P . V stays FP16. The weights of the int8
// products are int8 in the twin (the host's role mask, nr_graph.cpp kI8*, must name the same matrices).
#ifndef NV_I8
#define NV_I8 0
#endif
#define NV_I8E ((NV_I8 & 1) != 0)
#define NV_I8CT ((NV_I8 & 2) != 0)
#define NV_I8QKV ((NV_I8 & 4) != 0)
#define NV_I8WO ((NV_I8 & 8) != 0)
#define NV_I8QK ((NV_I8 & 16) != 0)
#define NV_I8MID ((NV_I8 & 32) != 0)
#if defined(NR_FUSED_IMAGE_INPUT)
#define NV_PRE 1                 // the pre block: x lifted from the image features, a pooled copy of the output
#else
#define NV_PRE 0
#endif
#if defined(NR_POOL_F16) && defined(NR_INPUT_F16)
#define NV_PRE_IO 1
#else
#define NV_PRE_IO 0
#endif
#if defined(NR_FUSED_IMAGE_OUTPUT)
#define NV_POST 1                // the post block: x blended from the level below and the skip, RGB out
#else
#define NV_POST 0
#endif
#if defined(NR_FUSED_POST_BLEND) && defined(NR_INPUT_F16) && NR_IMGOUT_REGS && NR_POST_BLEND_PK && !NR_IO_WMMA && \
    !NR_IO_DOT2 && !NR_IO_F32FMA && !NR_POST_REUSE && !NR_BLEND_G32 && !NR_POST_GAINBC
#define NV_POST_IO 1
#else
#define NV_POST_IO 0
#endif
#if defined(NR_FUSED_UPS_BLEND) && NR_HWAVES && defined(NR_WIDE_UPS_PROJECT) && NR_UPS_BLEND_PK == 2 && \
    !defined(NR_FUSED_UPS_PROJECT) && !defined(NR_WIDE_UPS_VIEW) && !NR_UPS_KOUTER && !NR_UPS_SPF
#define NV_UPS 1                 // the wide fused upsample: x blended from the projected level below and the skip
#else
#define NV_UPS 0
#endif
#if defined(NR_FUSED_UPS_BLEND) && defined(NR_FUSED_UPS_PROJECT) && !NR_HWAVES && defined(NR_INPUT_F16) && \
    NR_UPS_BLEND_PK == 1 && !NR_UPS_KOUTER && !NR_UPS_SPF && !defined(NR_WIDE_UPS_PROJECT)
#define NV_UPS32 1               // the C = 32 fused upsample: x = g * skip + up in FP16, up projected from the level below
#else
#define NV_UPS32 0
#endif
#if defined(NR_DS_PROJECT) && defined(NR_POOL_F16) && NR_DS_QPAIR
#define NV_DS 1                  // the fused downsample: a pooled copy of the output, projected to the level below
#else
#define NV_DS 0
#endif
#if NR_PERSIST || NR_HEADS != NR_C / 32 || (defined(NR_FUSED_UPS_BLEND) && !NV_UPS && !NV_UPS32) || \
    (defined(NR_DS_PROJECT) && !NV_DS) || (NV_DS && (NV_UPS || NV_UPS32)) || (defined(NR_FUSED_UPS_PROJECT) && !NV_UPS32) || \
    (defined(NR_WIDE_UPS_PROJECT) && !NV_UPS) || \
    (NV_PRE && !NV_PRE_IO) || (NV_POST && !NV_POST_IO) || (NV_PRE && NV_POST) || \
    (!NV_PRE && !NV_POST && ((defined(NR_INPUT_F16) && !NV_UPS32) || (defined(NR_POOL_F16) && !NV_DS))) || ((NV_PRE || NV_POST) && NR_HWAVES) || \
    NR_EDGE_BODIES || !NR_BAKED_EXP_BIAS || NR_ACC_F16 != 0 || \
    !NR_RESIDUAL_F32 || NR_QK_SCALE_FAST != 5 || NR_SWIN_BIAS_F16 || NR_WIN != 64 || \
    (NR_HWAVES && NR_TCHAIN) || (!NR_HWAVES && (NR_C != 32 || NR_FWAVES != 2))
#error "NR_NATIVE: the fused Swin bodies (fswin, fswinfusedup, fswindsp at every width) and the pre and post blocks only"
#endif

#define NV_CF (NR_C / 16)        // 16-channel fragments across C
#if NR_HWAVES
#define NV_NX (NR_C * 8)         // uvec4 entries of one window buffer: 64 tokens x C halves / 8
#else
#define NV_NX 256                // C = 32: K (64 keys x 4 uvec4) and V (32 key pairs x 8 uvec4)
#endif

// LDS. C >= 64: token T's channels 16kf + 8hx .. +7 at (2kf + hx) * 64 + T. Head h's K at nv_y4[256h + 4T + q]
// (channels 8q ..), its V at nv_x4[256h + 8kp + v/4][v%4] = (V[2kp][v], V[2kp+1][v]).
shared uvec4 nv_x4[NV_NX];
shared uvec4 nv_y4[NV_NX];

// The same buffers as uvec4 / vec4. Weights (binding 2, the FP16 twin) and activations are 256-element aligned.
layout(set = 0, binding = 0, std430) NR_COH buffer NvAct4 { uvec4 nv_a4[]; };
layout(set = 0, binding = 2, std430) readonly buffer NvWgt4 { uvec4 nv_w4[]; };
layout(set = 0, binding = 3, std430) readonly buffer NvF32x4 { vec4 nv_f4[]; };
#if NV_PRE
layout(set = 0, binding = 4, std430) readonly buffer NvWgtF16x8 { uvec4 nv_w16[]; };   // the FP16 lift matrix
#endif

#if NV_I8
#include "i8.glsl"
layout(set = 0, binding = 2, std430) readonly buffer NvWgtF { float nv_wf[]; };
#endif
#if NV_I8QK
// A token's query as int8 and its scale.
#define NV_QDECL const uint q[8], float qs
#define NV_QPASS q, qs
#else
#define NV_QDECL const uint q[16]
#define NV_QPASS q
#endif
#if NV_I8
#define NV_CDECL out float cq[32]        // the context as FP32, rounded by the caller
#else
#define NV_CDECL out uint cq[16]
#endif

f16vec2 nv_h(uint u) { return unpackFloat2x16(u); }
uint nv_p(float16_t a, float16_t b) { return packFloat2x16(f16vec2(a, b)); }

// s + sum over the 8 k pairs of (w0, w1) . (x0, x1), pair by pair in k order (NR_DOT2: coopmm.glsl).
float nv_dot8(float s, uvec4 w0, uvec4 w1, uvec4 x0, uvec4 x1) {
    s = NR_DOT2(s, nv_h(w0.x), nv_h(x0.x)); s = NR_DOT2(s, nv_h(w0.y), nv_h(x0.y));
    s = NR_DOT2(s, nv_h(w0.z), nv_h(x0.z)); s = NR_DOT2(s, nv_h(w0.w), nv_h(x0.w));
    s = NR_DOT2(s, nv_h(w1.x), nv_h(x1.x)); s = NR_DOT2(s, nv_h(w1.y), nv_h(x1.y));
    s = NR_DOT2(s, nv_h(w1.z), nv_h(x1.z)); s = NR_DOT2(s, nv_h(w1.w), nv_h(x1.w));
    return s;
}
// The same with each pair given as (high, low): an odd row against k-pair-swapped storage and operand.
float nv_dot8s(float s, uvec4 w0, uvec4 w1, uvec4 x0, uvec4 x1) {
    s = NR_DOT2(s, nv_h(w0.x), nv_h(x0.x).yx); s = NR_DOT2(s, nv_h(w0.y), nv_h(x0.y).yx);
    s = NR_DOT2(s, nv_h(w0.z), nv_h(x0.z).yx); s = NR_DOT2(s, nv_h(w0.w), nv_h(x0.w).yx);
    s = NR_DOT2(s, nv_h(w1.x), nv_h(x1.x).yx); s = NR_DOT2(s, nv_h(w1.y), nv_h(x1.y).yx);
    s = NR_DOT2(s, nv_h(w1.z), nv_h(x1.z).yx); s = NR_DOT2(s, nv_h(w1.w), nv_h(x1.w).yx);
    return s;
}

// Weight row `row`, k fragment kf of a tile-blocked [row/16][k/16] matrix of 16x16 FP16 tiles at `woff` (halves),
// KF fragments across k: the two uvec4 of its 16 k values (uniform address: scalar loads).
#define NV_WROW(woff, KF, row, kf) ((woff) / 8u + ((uint(row) >> 4u) * uint(KF) + uint(kf)) * 32u + (uint(row) & 15u) * 2u)

// e4m3 rounding of an FP32 value through FP16 (the emulated body's `nr_quant_pair(f16vec2(float16_t(a), ..))`).
float16_t nv_q16(float a) { return nr_quant_pair(f16vec2(float16_t(a), float16_t(0.0))).x; }

// Eight FP32 values as the FP16 uvec4 of 8 channels, each rounded by nv_q16.
uvec4 nv_pack_q16(const float a[16], int base) {
    return uvec4(nv_p(nv_q16(a[base]), nv_q16(a[base + 1])), nv_p(nv_q16(a[base + 2]), nv_q16(a[base + 3])),
                 nv_p(nv_q16(a[base + 4]), nv_q16(a[base + 5])), nv_p(nv_q16(a[base + 6]), nv_q16(a[base + 7])));
}

// The cosine norm of one token's 32 Q (or K) channels, in the emulated body's order: lane half hi holds the
// channels of parity hi, summed into four partials (channel 16d + 2c + hi into partial c & 3, c outer, d inner);
// the halves' sums rounded to FP16 and added in FP16.
float16_t nv_norm(const float a[32]) {
    float16_t sh[2];
    for (int hi = 0; hi < 2; ++hi) {
        float sp[4];
        for (int i = 0; i < 4; ++i) sp[i] = 0.0;
        for (int c = 0; c < 8; ++c)
            for (int d = 0; d < 2; ++d) {
                const float v = a[16 * d + 2 * c + hi];
                sp[c & 3] = fma(v, v, sp[c & 3]);
            }
        sh[hi] = f16vec2(vec2((sp[0] + sp[1]) + (sp[2] + sp[3]), 0.0)).x;
    }
    const float16_t n2 = sh[0] + sh[1];
    return float16_t(inversesqrt(float(max(n2, float16_t(0.000062)))));
}

// Logits of query q against the 16 keys 16j .. 16j+15 of head h (K in nv_y4), and the probabilities before
// normalisation. The exponent trick (nr_swin_exp_baked) works on a pair, and with NR_EXP_NOHI the second value
// depends on the first, so the pairs are the emulated body's: keys (4c + hi, 4c + 2 + hi) of the block.
void nv_probs16(uint h, uint T, uint j, NV_QDECL, out f16vec2 pr[8]) {
    float lg[16];
#if NV_I8QK
    // K int8 at nv_y4[256h + 2T + r], its scales as floats from nv_y4[256h + 128] on (nv_store_kv)
    for (int i = 0; i < 16; ++i) {
        const uint key = 16u * j + uint(i);
        const int d = i8_dot16(i8_dot16(0, nv_y4[256u * h + 2u * key], uvec4(q[0], q[1], q[2], q[3])),
                               nv_y4[256u * h + 2u * key + 1u], uvec4(q[4], q[5], q[6], q[7]));
        lg[i] = float(d) * (qs * uintBitsToFloat(nv_y4[256u * h + 128u + (key >> 2u)][key & 3u]));
    }
#else
    for (int i = 0; i < 16; ++i) {
        const uint key = 16u * j + uint(i);
        float s = 0.0;
        for (int r = 0; r < 4; ++r) {
            const uvec4 k4 = nv_y4[256u * h + 4u * key + uint(r)];
            for (int e = 0; e < 4; ++e) {
                const f16vec2 kp = nv_h(k4[e]), qp = nv_h(q[4 * r + e]);
                s = (i & 1) != 0 ? NR_DOT2(s, kp.yx, qp.yx) : NR_DOT2(s, kp, qp);
            }
        }
        lg[i] = s;
    }
#endif
    const uint bb = (pc.b_off + h * 4096u) / 4u + T * 16u + 4u * j;
    float bs[16];
    for (int r = 0; r < 4; ++r) { const vec4 b4 = nv_f4[bb + uint(r)]; for (int e = 0; e < 4; ++e) bs[4 * r + e] = b4[e]; }
    for (int c = 0; c < 4; ++c)
        for (int hi = 0; hi < 2; ++hi) {
            const int a = 4 * c + hi;
            pr[2 * c + hi] = nr_swin_exp_baked(vec2(lg[a], lg[a + 2]), vec2(bs[a], bs[a + 2]));
        }
}

// Window attention of head h for query token T: q holds its 16 quantised Q pairs, cq gets the 16 quantised
// context pairs. K and V of the head in LDS (above). NV_PCACHE (every body): pass 2 takes pass 1's 64
// probabilities from registers (32 more VGPRs) instead of computing the logits and exponents again; it picks block
// j with a uniform select. NV_PCROLL, how pass 1 fills the cache: 0 unrolled (four nv_probs16 copies, constant
// cache indices), 1 rolled (one copy; each block shifted in at the end of a 32-entry queue, so the indices stay
// constant), 2 two blocks an iteration (two copies, the queue shifted by 16). Same values, same order in all three;
// they differ in compile time (the RX 6600's driver: about 5.5 s for a C = 32 body unrolled) and registers.
#ifndef NV_PCACHE
#define NV_PCACHE 1
#endif
#ifndef NV_PCROLL
#define NV_PCROLL 1
#endif
void nv_attn(uint h, uint T, NV_QDECL, NV_CDECL) {
    const int opaque = int(gl_WorkGroupID.z);
    // pass 1: the probability sum in the emulated body's order: pair (4c + hi, 4c + 2 + hi) of block j into
    // partial [hi][c], j outer; then ((p0 + p1) + p2) + p3 a half, the halves added, then .x + .y, all FP16.
    f16vec2 part[2][4];
    for (int hi = 0; hi < 2; ++hi) for (int c = 0; c < 4; ++c) part[hi][c] = f16vec2(0.0hf);
#if NV_PCACHE
    f16vec2 prc[32];
#endif
#if NV_PCACHE && NV_PCROLL == 2
    for (int i = 0; i < 32; ++i) prc[i] = f16vec2(0.0hf);
    for (int j = 0; j < 2 + opaque; j = j + 1) {
        f16vec2 pa[8], pb[8];
        nv_probs16(h, T, uint(2 * j), NV_QPASS, pa);
        nv_probs16(h, T, uint(2 * j + 1), NV_QPASS, pb);
        for (int c = 0; c < 4; ++c) for (int hi = 0; hi < 2; ++hi) part[hi][c] = part[hi][c] + pa[2 * c + hi];
        for (int c = 0; c < 4; ++c) for (int hi = 0; hi < 2; ++hi) part[hi][c] = part[hi][c] + pb[2 * c + hi];
        for (int i = 0; i < 16; ++i) prc[i] = prc[i + 16];
        for (int i = 0; i < 8; ++i) { prc[16 + i] = pa[i]; prc[24 + i] = pb[i]; }
    }
#else
#if NV_PCACHE && NV_PCROLL == 0
    for (int j = 0; j < 4; ++j) {
#else
#if NV_PCACHE
    for (int i = 0; i < 32; ++i) prc[i] = f16vec2(0.0hf);
#endif
    for (int j = 0; j < 4 + opaque; j = j + 1) {
#endif
        f16vec2 pr[8];
        nv_probs16(h, T, uint(j), NV_QPASS, pr);
#if NV_PCACHE && NV_PCROLL == 0
        for (int i = 0; i < 8; ++i) prc[8 * j + i] = pr[i];
#elif NV_PCACHE
        for (int i = 0; i < 24; ++i) prc[i] = prc[i + 8];
        for (int i = 0; i < 8; ++i) prc[24 + i] = pr[i];
#endif
        for (int c = 0; c < 4; ++c) for (int hi = 0; hi < 2; ++hi) part[hi][c] = part[hi][c] + pr[2 * c + hi];
    }
#endif
    f16vec2 tot[2];
    for (int hi = 0; hi < 2; ++hi) tot[hi] = ((part[hi][0] + part[hi][1]) + part[hi][2]) + part[hi][3];
    const f16vec2 t2 = tot[0] + tot[1];
    const float16_t sum = t2.x + t2.y;
    const float inv = float(float16_t(1.0 / float(max(sum, float16_t(6.103515625e-05)))));
    // pass 2: the same probabilities, normalised and rounded, times V.
    float ctx[32];
    for (int v = 0; v < 32; ++v) ctx[v] = 0.0;
    for (int j = 0; j < 4 + opaque; j = j + 1) {
        f16vec2 pr[8];
#if NV_PCACHE
        for (int i = 0; i < 8; ++i) pr[i] = j == 0 ? prc[i] : j == 1 ? prc[8 + i] : j == 2 ? prc[16 + i] : prc[24 + i];
#else
        nv_probs16(h, T, uint(j), NV_QPASS, pr);
#endif
        float16_t pk[16];
        for (int c = 0; c < 4; ++c)
            for (int hi = 0; hi < 2; ++hi) {
                const f16vec2 qp = nr_quant_pair(f16vec2(float16_t(float(pr[2 * c + hi].x) * inv),
                                                         float16_t(float(pr[2 * c + hi].y) * inv)));
                pk[4 * c + hi] = qp.x; pk[4 * c + 2 + hi] = qp.y;
            }
        for (int k = 0; k < 8; ++k) {
            const uint vrow = 256u * h + (8u * uint(j) + uint(k)) * 8u;
            const f16vec2 pp = f16vec2(pk[2 * k], pk[2 * k + 1]);
            for (int r = 0; r < 8; ++r) {
                const uvec4 v4 = nv_x4[vrow + uint(r)];
                for (int e = 0; e < 4; ++e) {
                    const int v = 4 * r + e;
                    const f16vec2 vp = nv_h(v4[e]);
                    ctx[v] = (v & 1) != 0 ? NR_DOT2(ctx[v], vp.yx, pp.yx) : NR_DOT2(ctx[v], vp, pp);
                }
            }
        }
    }
#if NV_I8
    cq = ctx;
#else
    for (int p = 0; p < 16; ++p) cq[p] = nv_p(nv_q16(ctx[2 * p]), nv_q16(ctx[2 * p + 1]));
#endif
}

// K and V of head h for token T into LDS (layout above). k, v: the token's 16 quantised pairs. V goes in key
// pairs, so lanes T and T ^ 1 (one wave) exchange theirs: the even token writes channels 0..15, the odd 16..31.
#if NV_I8QK
void nv_store_kv(uint h, uint T, const uint k[8], float ks, const uint v[16]) {
    nv_y4[256u * h + 2u * T] = uvec4(k[0], k[1], k[2], k[3]);
    nv_y4[256u * h + 2u * T + 1u] = uvec4(k[4], k[5], k[6], k[7]);
    nv_y4[256u * h + 128u + (T >> 2u)][T & 3u] = floatBitsToUint(ks);
#else
void nv_store_kv(uint h, uint T, const uint k[16], const uint v[16]) {
    for (int q = 0; q < 4; ++q) nv_y4[256u * h + 4u * T + uint(q)] = uvec4(k[4 * q], k[4 * q + 1], k[4 * q + 2], k[4 * q + 3]);
#endif
    const bool odd = (T & 1u) != 0u;
    uint dw[16];
    for (int p = 0; p < 16; ++p) {
        const uint mine = v[p], other = subgroupShuffleXor(mine, 1u);
        const f16vec2 lo = nv_h(odd ? other : mine), hi = nv_h(odd ? mine : other);
        if ((p < 8) != odd) { dw[2 * (p & 7)] = nv_p(lo.x, hi.x); dw[2 * (p & 7) + 1] = nv_p(lo.y, hi.y); }
    }
    const uint vb = 256u * h + (T >> 1u) * 8u + (odd ? 4u : 0u);
    for (int q = 0; q < 4; ++q) nv_x4[vb + uint(q)] = uvec4(dw[4 * q], dw[4 * q + 1], dw[4 * q + 2], dw[4 * q + 3]);
}

// Q (part 0, times the head's logit scale) or K (part 1) of one token from its 32 FP32 channels: the cosine
// norm, then the e4m3 rounding of the scaled FP32 value (nr_qscale_fast / nr_kscale_fast).
void nv_qk(const float a[32], int part, float16_t hscale, out uint o[16]) {
    const float16_t n = nv_norm(a);
    const float f = part == 0 ? float(n) * float(hscale) : float(n);
    for (int p = 0; p < 16; ++p) o[p] = packFloat2x16(nr_quant_pair32(vec2(a[2 * p], a[2 * p + 1]) * f));
}
#if NV_I8
// The same scaled values as int8 with the token's scale (returned).
float nv_qk8(const float a[32], int part, float16_t hscale, out uint o[8]) {
    const float16_t n = nv_norm(a);
    const float f = part == 0 ? float(n) * float(hscale) : float(n);
    float v[32];
    for (int c = 0; c < 32; ++c) v[c] = a[c] * f;
    return i8_vec32(v, o);
}
#endif

#if NV_PRE || NV_DS
// The 2x2 mean of 8 FP32 values across lanes l, l^1, l^4, l^5 (one 2x2 group of a tile's 4x4 pixels), as the emulated
// body pools: the pairs along x summed in FP32 and rounded to FP16, the two rows added in FP16, a quarter of that on
// the e4m3 grid (nr_quant_pair32). Every lane of the group gets the group's values.
uvec4 nv_pool8(const float v[8]) {
    float16_t sm[8];
    for (int j = 0; j < 8; ++j) {
        const float xs = v[j] + subgroupShuffleXor(v[j], 1u);
        sm[j] = float16_t(xs) + float16_t(subgroupShuffleXor(xs, 4u));
    }
    uvec4 r;
    for (int p = 0; p < 4; ++p)
        r[p] = packFloat2x16(nr_quant_pair32(vec2(float(sm[2 * p]) * 0.25, float(sm[2 * p + 1]) * 0.25)));
    return r;
}
#endif

#if NV_DS
// The downsample projection of the pooled copy in nv_x4[(c / 8) * 16 + p]: 2C channels at each of the 16 pooled
// pixels, `rows` a wave in `nb` blocks of 8, one lane a pixel (lanes 16-31 repeat 0-15); the addressing and
// clearing of the destination are the emulated body's.
void nv_ds_project(uint lane, uint w, uint rows, int nb) {
{
    const uint t = lane & 15u;
    const int px0 = (2 * int(gl_WorkGroupID.x) + pc.shift) * 2, py0 = (2 * int(gl_WorkGroupID.y) + pc.shift_y) * 2;
    const int spx_i = px0 + int(t % 4u), spy_i = py0 + int(t / 4u);
    const bool in_grid = spx_i >= 0 && spy_i >= 0 &&
        spx_i < int(pc.pool_tiles_x * 4u) && spy_i < int(pc.pool_tiles_y * 4u);
    const uint spx = uint(max(spx_i, 0)), spy = uint(max(spy_i, 0));
    const uint writer_rows = pc.ds_writer_rows == 0u ? pc.ds_rows : pc.ds_writer_rows;
    const bool ds_identity = pc.ds_mode != 2u && pc.ds_raster == pc.ds_crow && writer_rows == pc.ds_rows;
    for (int b = 0; b < nb; ++b) {
        const uint row = rows * w + 8u * uint(b);
        float acc[8];
        for (int j = 0; j < 8; ++j) acc[j] = 0.0;
        for (int kf = 0; kf < NV_CF + int(gl_WorkGroupID.z); kf = kf + 1) {
            const uvec4 x0 = nv_x4[uint(kf) * 32u + t], x1 = nv_x4[uint(kf) * 32u + 16u + t];
            const uint wb = NV_WROW(pc.ds_w_off, NV_CF, row, kf);
            for (int j = 0; j < 8; ++j) acc[j] = nv_dot8(acc[j], nv_w4[wb + 2u * uint(j)], nv_w4[wb + 2u * uint(j) + 1u], x0, x1);
        }
        float16_t outv[8];
        for (int j = 0; j < 8; ++j) outv[j] = float16_t(acc[j]);
        const uint sg = row >> 4u;
        uint dst_group, sdx, sdy;
        if (ds_identity) {
            dst_group = sg; sdx = spx; sdy = spy;
        } else {
            const uint linear = pc.ds_mode == 2u ? (sg * pc.ds_rows + spy) * pc.ds_crow + spx
                : (sg * writer_rows + spy) * pc.ds_raster + spx;
            dst_group = linear / (pc.ds_crow * pc.ds_rows);
            const uint pixel = linear % (pc.ds_crow * pc.ds_rows);
            sdx = pixel % pc.ds_crow; sdy = pixel / pc.ds_crow;
        }
        const bool s_oob = (pc.ds_mode != 2u && (spy >= writer_rows || spx >= pc.ds_raster)) ||
            sdx >= pc.ds_otx * 4u || dst_group >= pc.ds_n / 16u;
        if (pc.ds_writer_rows != 0u && (sdy >= pc.ds_writer_rows || sdx >= pc.ds_raster))
            for (int j = 0; j < 8; ++j) outv[j] = float16_t(0.0);
        if (pc.ds_clear_x != 0u && (sdx >= pc.ds_clear_x || sdy >= pc.ds_clear_y))
            for (int j = 0; j < 8; ++j) outv[j] = float16_t(0.0);
        const uint ob = pc.ds_o_off + (((sdy / 4u) * pc.ds_otx + sdx / 4u) * (pc.ds_n / 16u) + dst_group) * 256u
                        + (4u * (sdy % 4u) + sdx % 4u) * 16u + (row & 15u);
        if (lane < 16u && in_grid && !s_oob) {
            uvec4 v;
            for (int p = 0; p < 4; ++p) v[p] = packFloat2x16(nr_quant_pair(f16vec2(outv[2 * p], outv[2 * p + 1])));
            nv_a4[ob / 8u] = v;
        }
    }
}
}
#endif

#if NR_HWAVES
// acc[8s + j] += sum_k W[row0 + j][k] X[k][T_s] over KF fragments of k, rows row0..row0+7 (8-aligned, uniform),
// X an LDS window buffer. The fragment loop is kept rolled on purpose: one iteration is 128 v_dot2 on 64 scalar
// weight dwords, and the loops unrolled (by unroll_glsl.py, or by the driver, which unrolls every constant loop
// here) make a 300 KB shader that needs every VGPR. Its header is not one unroll_glsl.py unrolls, and `nv_opaque`
// (the workgroup's z, always 0) hides the trip count from the driver; the same for the other long loops.
#define NV_GEMM8(acc, XB, woff, KF, row0) { \
    for (int kf = 0; kf < (KF) + nv_opaque; kf = kf + 1) { \
        const uint xi = uint(kf) * 128u + lane; \
        const uvec4 xa0 = XB[xi], xa1 = XB[xi + 64u], xb0 = XB[xi + 32u], xb1 = XB[xi + 96u]; \
        const uint wb = NV_WROW(woff, KF, row0, kf); \
        for (int j = 0; j < 8; ++j) { \
            const uvec4 w0 = nv_w4[wb + 2u * uint(j)], w1 = nv_w4[wb + 2u * uint(j) + 1u]; \
            acc[j] = nv_dot8(acc[j], w0, w1, xa0, xa1); \
            acc[8 + j] = nv_dot8(acc[8 + j], w0, w1, xb0, xb1); \
        } \
    } }

#if NV_I8
// The int8 body. Activations, one scale a token over its C channels (every wave reads or shares the token's largest
// value, nv_tokmax), go into nv_y4 as [k fragment][token] uvec4 of 16 int8: x and then y in the lower half (NV_X8),
// m in the upper half (NV_M8), and the context of head h over its K (NV_C8). x and y stay in nv_x4 in FP16 for the
// residuals, as in the FP16 body. acc: int sums, rows row0..row0+7 for tokens lane, lane + 32.
#define NV_X8(kf, T) (uint(kf) * 64u + (T))
#define NV_M8(kf, T) (uint(NV_CF * 64) + uint(kf) * 64u + (T))
#define NV_C8(kf, T) (256u * (uint(kf) >> 1u) + (uint(kf) & 1u) * 64u + (T))
#define NV_GEMM8I(acc, XI, woff, KF, row0) { \
    for (int kf = 0; kf < (KF) + nv_opaque; kf = kf + 1) { \
        const uvec4 xa = nv_y4[XI(kf, lane)], xb = nv_y4[XI(kf, lane + 32u)]; \
        for (int j = 0; j < 8; ++j) { \
            const uvec4 wv = nv_w4[I8_WROW(woff, KF, (row0) + uint(j), kf)]; \
            acc[j] = i8_dot16(acc[j], wv, xa); \
            acc[8 + j] = i8_dot16(acc[8 + j], wv, xb); \
        } \
    } }
// The scale exponents of tokens lane and lane + 32 from each wave's largest |value| of its own channels (a0, a1),
// exchanged through nv_y4[256h + 144 ..+16] (free in every head's block between the K scales and 160 whenever this
// runs; the barriers around it keep the LDS that was live before and the LDS written after out of it).
void nv_tokmax(uint w, uint lane, float a0, float a1, out int s0, out int s1) {
    barrier();
    nv_y4[256u * w + 144u + (lane >> 2u)][lane & 3u] = floatBitsToUint(a0);
    nv_y4[256u * w + 152u + (lane >> 2u)][lane & 3u] = floatBitsToUint(a1);
    barrier();
    float m0 = 0.0, m1 = 0.0;
    for (int h = 0; h < NR_HEADS; ++h) {
        m0 = max(m0, uintBitsToFloat(nv_y4[256u * uint(h) + 144u + (lane >> 2u)][lane & 3u]));
        m1 = max(m1, uintBitsToFloat(nv_y4[256u * uint(h) + 152u + (lane >> 2u)][lane & 3u]));
    }
    barrier();
    s0 = i8_exp(m0);
    s1 = i8_exp(m1);
}
// 16 values (from v[base]) as int8 with scale 2^s.
uvec4 nv_q16x(const float v[32], int base, int s) {
    const uint fb = i8_floor(s);
    const float inv = i8_pow2(-s);
    uvec4 r;
    for (int p = 0; p < 4; ++p)
        r[p] = i8_pack(i8_q(v[base + 4 * p], fb, inv), i8_q(v[base + 4 * p + 1], fb, inv),
                       i8_q(v[base + 4 * p + 2], fb, inv), i8_q(v[base + 4 * p + 3], fb, inv));
    return r;
}
float nv_amax(const float v[32]) { float a = 0.0; for (int i = 0; i < 32; ++i) a = max(a, abs(v[i])); return a; }

void main() {
    const int nv_opaque = int(gl_WorkGroupID.z);
    const uint tid = gl_LocalInvocationID.x, lane = gl_SubgroupInvocationID, w = subgroupBroadcastFirst(gl_SubgroupID);
#if NV_UPS
    // ---- stage 0, fused upsample: the level below (2C channels at half resolution, the 4x4 pixels under this
    //      window) projected to C channels in FP16, then x = q(g * skip + up) at each token's half-resolution
    //      pixel (the emulated body's NR_WIDE_UPS_PROJECT and NR_UPS_BLEND_PK=2 prologue) ----------------------
    {
        // the 16 source pixels into nv_y4[(2kf + hx) * 16 + t], kf < 2C/16, clamped to the level below
        for (uint i = tid; i < uint(2 * NV_CF * 32); i += uint(NR_THREADS)) {
            const uint hx = i & 1u, t = (i >> 1u) & 15u, kf = i >> 5u;
            const uint px = uint(clamp(4 * int(gl_WorkGroupID.x) + 2 * pc.shift + int(t % 4u), 0, int(pc.blend_itiles_x * 4u) - 1));
            const uint py = uint(clamp(4 * int(gl_WorkGroupID.y) + 2 * pc.shift_y + int(t / 4u), 0, int(pc.blend_itiles_y * 4u) - 1));
            const uint tile = (py / 4u) * pc.blend_itiles_x + px / 4u, islot = (py % 4u) * 4u + px % 4u;
            nv_y4[(2u * kf + hx) * 16u + t] =
                nv_a4[(pc.blend_p_off + (tile * uint(2 * NV_CF) + kf) * 256u + islot * 16u + hx * 8u) / 8u];
        }
        barrier();
        // the projection, channels 32w.. of this wave, one lane a pixel (lanes 16-31 repeat 0-15), into
        // nv_y4[NV_UPB + (c / 8) * 16 + t] in FP16
        const uint NV_UPB = uint(2 * NV_CF * 32), t = lane & 15u;
        for (int b = 0; b < 4; ++b) {
            float acc[8];
            for (int j = 0; j < 8; ++j) acc[j] = 0.0;
            for (int kf = 0; kf < 2 * NV_CF + nv_opaque; kf = kf + 1) {
                const uvec4 x0 = nv_y4[uint(kf) * 32u + t], x1 = nv_y4[uint(kf) * 32u + 16u + t];
                const uint wb = NV_WROW(pc.ups_weight_off, 2 * NV_CF, 32u * w + 8u * uint(b), kf);
                for (int j = 0; j < 8; ++j) acc[j] = nv_dot8(acc[j], nv_w4[wb + 2u * uint(j)], nv_w4[wb + 2u * uint(j) + 1u], x0, x1);
            }
            if (lane < 16u)
                nv_y4[NV_UPB + (4u * w + uint(b)) * 16u + t] = uvec4(nv_p(float16_t(acc[0]), float16_t(acc[1])),
                    nv_p(float16_t(acc[2]), float16_t(acc[3])), nv_p(float16_t(acc[4]), float16_t(acc[5])),
                    nv_p(float16_t(acc[6]), float16_t(acc[7])));
        }
        barrier();
        for (uint i = tid; i < uint(NV_CF * 128); i += uint(NR_THREADS)) {
            const uint hx = i & 1u, T = (i >> 1u) & 63u, kf = i >> 7u, q = T >> 4u, slot = T & 15u;
            const uint dx = slot % 4u, dy = slot / 4u;
            const uint tx = uint(clamp(2 * int(gl_WorkGroupID.x) + pc.shift + int(q & 1u), 0, int(pc.tiles_x) - 1));
            const uint ty = uint(clamp(2 * int(gl_WorkGroupID.y) + pc.shift_y + int(q >> 1u), 0, int(pc.tiles_y) - 1));
            const uint local = ((q >> 1u) * 2u + dy / 2u) * 4u + (q & 1u) * 2u + dx / 2u;
            const uint stile = ty * (pc.blend_stiles_x != 0u ? pc.blend_stiles_x : pc.blend_tiles_x) + tx;
            const uvec4 pv = nv_y4[NV_UPB + (2u * kf + hx) * 16u + local];
            const uvec4 sv = nv_a4[(pc.blend_s_off + (stile * uint(NV_CF) + kf) * 256u + slot * 16u + hx * 8u) / 8u];
            uvec4 v;
            for (int p = 0; p < 4; ++p) {
                const uint ch = kf * 16u + hx * 8u + uint(2 * p);
                const f16vec2 g2 = unpackFloat2x16(wgt_u32[(pc.blend_g_off + ch) >> 1u]);   // ch even, gains 16-byte aligned
                v[p] = packFloat2x16(nr_quant_pair(fma(nv_h(sv[p]), g2, nv_h(pv[p]))));
            }
            if (nr_tile_oob(q)) v = uvec4(0u);
            nv_x4[kf * 128u + hx * 64u + T] = v;
        }
    }
    barrier();
#else
    // ---- stage 0: the window's x into nv_x4; tiles outside the image are zero -------------------------
    for (uint i = tid; i < uint(NV_CF * 128); i += uint(NR_THREADS)) {
        const uint hx = i & 1u, T = (i >> 1u) & 63u, kf = i >> 7u, m = T >> 4u;
        uvec4 v = uvec4(0u);
        if (!nr_tile_oob(m)) v = nv_a4[(pc.x_off + nr_tile_base(m) + kf * 256u + (T & 15u) * 16u + hx * 8u) / 8u];
        nv_x4[kf * 128u + hx * 64u + T] = v;
    }
    barrier();
#endif

    // ---- x as int8 (NV_I8E): every wave reads its two tokens' whole x for the scale, then writes its own 32 channels
    int xs0 = 0, xs1 = 0;
    if (NV_I8E) {
        float a0 = 0.0, a1 = 0.0;
        for (int kf = 0; kf < NV_CF; ++kf)
            for (int hx = 0; hx < 2; ++hx) {
                const uvec4 u0 = nv_x4[uint(kf) * 128u + uint(hx) * 64u + lane], u1 = nv_x4[uint(kf) * 128u + uint(hx) * 64u + lane + 32u];
                for (int i = 0; i < 4; ++i) {
                    const vec2 f0 = vec2(nv_h(u0[i])), f1 = vec2(nv_h(u1[i]));
                    a0 = max(a0, max(abs(f0.x), abs(f0.y))); a1 = max(a1, max(abs(f1.x), abs(f1.y)));
                }
            }
        xs0 = i8_exp(a0); xs1 = i8_exp(a1);
        for (int s = 0; s < 2; ++s) {
            float v[32];
            for (int q = 0; q < 4; ++q) {
                const uvec4 u = nv_x4[(2u * w + uint(q >> 1)) * 128u + uint(q & 1) * 64u + lane + 32u * uint(s)];
                for (int i = 0; i < 4; ++i) { v[8 * q + 2 * i] = float(nv_h(u[i]).x); v[8 * q + 2 * i + 1] = float(nv_h(u[i]).y); }
            }
            for (int r = 0; r < 2; ++r) nv_y4[NV_X8(2u * w + uint(r), lane + 32u * uint(s))] = nv_q16x(v, 16 * r, s == 0 ? xs0 : xs1);
        }
        barrier();
    }

    // ---- stage 1: e = act(E . x) for hidden group w (128 rows), 32 rows at a time into the group's middle
    //      projection m = Wmid . e (32 rows, channels 32w..32w+31) -----------------------------------------
    float mid[64];                                    // [32 s + o]
    for (int o = 0; o < 64; ++o) mid[o] = 0.0;
    for (int g = 0; g < 4 + nv_opaque; g = g + 1) {   // rolled
        float e[2][32];
        for (int hb = 0; hb < 4; ++hb) {
            const uint row0 = w * 128u + uint(g) * 32u + uint(hb) * 8u;
            if (NV_I8E) {
                int acc[16];
                for (int j = 0; j < 16; ++j) acc[j] = 0;
                NV_GEMM8I(acc, NV_X8, pc.e_off, NV_CF, row0)
                for (int j = 0; j < 8; ++j) {
                    const float ws = I8_WSCALE(nv_wf, pc.e_off, 4 * NR_C, NR_C, row0 + uint(j));
                    e[0][8 * hb + j] = float(acc[j]) * (ws * i8_pow2(xs0));
                    e[1][8 * hb + j] = float(acc[8 + j]) * (ws * i8_pow2(xs1));
                }
            } else {
                float acc[16];
                for (int j = 0; j < 16; ++j) acc[j] = 0.0;
                NV_GEMM8(acc, nv_x4, pc.e_off, NV_CF, row0)
                for (int j = 0; j < 8; ++j) { e[0][8 * hb + j] = acc[j]; e[1][8 * hb + j] = acc[8 + j]; }
            }
        }
        for (int s = 0; s < 2; ++s)
            for (int i = 0; i < 32; ++i) e[s][i] = (i & 1) != 0 ? nr_act_horner_b(e[s][i]) : nr_act_horner(e[s][i]);
        if (NV_I8MID) {
            uvec4 eq[2][2];
            float es[2];
            for (int s = 0; s < 2; ++s) {
                const int se = i8_exp(nv_amax(e[s]));
                eq[s][0] = nv_q16x(e[s], 0, se); eq[s][1] = nv_q16x(e[s], 16, se);
                es[s] = i8_pow2(se);
            }
            for (int o = 0; o < 32; ++o) {
                const uvec4 w0 = nv_w4[I8_WROW(pc.mid_off, 8, 32u * w + uint(o), 2 * g)];
                const uvec4 w1 = nv_w4[I8_WROW(pc.mid_off, 8, 32u * w + uint(o), 2 * g + 1)];
                for (int s = 0; s < 2; ++s)
                    mid[32 * s + o] = fma(float(i8_dot16(i8_dot16(0, w0, eq[s][0]), w1, eq[s][1])), es[s], mid[32 * s + o]);
            }
        } else {
            for (int hb = 0; hb < 2; ++hb) {
                uvec4 ep[4];                              // [2 s + half]
                for (int s = 0; s < 2; ++s)
                    for (int p = 0; p < 8; ++p)
                        ep[2 * s + (p >> 2)][p & 3] = packFloat2x16(nr_quant_pair32(vec2(e[s][16 * hb + 2 * p], e[s][16 * hb + 2 * p + 1])));
                // Wmid: tile row (32w + o) / 16, k fragment 2g + hb; odd rows k-pair swapped (`kswap`).
                for (int o = 0; o < 32; ++o) {
                    const uint wb = NV_WROW(pc.mid_off, 8, 32u * w + uint(o), 2 * g + hb);
                    const uvec4 w0 = nv_w4[wb], w1 = nv_w4[wb + 1u];
                    for (int s = 0; s < 2; ++s)
                        mid[32 * s + o] = (o & 1) != 0 ? nv_dot8s(mid[32 * s + o], w0, w1, ep[2 * s], ep[2 * s + 1])
                                                       : nv_dot8(mid[32 * s + o], w0, w1, ep[2 * s], ep[2 * s + 1]);
                }
            }
        }
    }
    if (NV_I8MID)
        for (int o = 0; o < 32; ++o) {
            const float ws = I8_WSCALE(nv_wf, pc.mid_off, NR_C, 128, 32u * w + uint(o));
            mid[o] *= ws; mid[32 + o] *= ws;
        }
    // ---- stage 2: y = q(Ct . m + rs * x), channels 32w..32w+31, in place over x; m int8 (NV_M8) for an int8 Ct,
    //      else FP16 on the e4m3 grid as the FP16 body stores it ----------------------------------------------
    float yv[2][32];
    if (NV_I8CT) {
        float m0[32], m1[32];
        for (int o = 0; o < 32; ++o) { m0[o] = mid[o]; m1[o] = mid[32 + o]; }
        int ms0, ms1;
        nv_tokmax(w, lane, nv_amax(m0), nv_amax(m1), ms0, ms1);
        for (int r = 0; r < 2; ++r) {
            nv_y4[NV_M8(2u * w + uint(r), lane)] = nv_q16x(m0, 16 * r, ms0);
            nv_y4[NV_M8(2u * w + uint(r), lane + 32u)] = nv_q16x(m1, 16 * r, ms1);
        }
        barrier();
        for (int b = 0; b < 4; ++b) {
            const uint xi = (2u * w + uint(b >> 1)) * 128u + uint(b & 1) * 64u + lane, ch = 32u * w + 8u * uint(b);
            int acc[16];
            for (int j = 0; j < 16; ++j) acc[j] = 0;
            NV_GEMM8I(acc, NV_M8, pc.ct_off, NV_CF, ch)
            for (int s = 0; s < 2; ++s) {
                const uvec4 x = nv_x4[xi + 32u * uint(s)];
                const float ms = i8_pow2(s == 0 ? ms0 : ms1);
                for (int j = 0; j < 8; ++j)
                    yv[s][8 * b + j] = fma(float(acc[8 * s + j]), ms * I8_WSCALE(nv_wf, pc.ct_off, NR_C, NR_C, ch + uint(j)),
                                           float(nv_h(x[j >> 1])[j & 1]) * wgt_f32[pc.rs_off + ch + uint(j)]);
            }
        }
    } else {
        barrier();                                        // x8 read by every wave before m replaces it
        for (int s = 0; s < 2; ++s)
            for (int b = 0; b < 4; ++b) {
                float t[16];
                for (int j = 0; j < 8; ++j) t[j] = mid[32 * s + 8 * b + j];
                nv_y4[(2u * w + uint(b >> 1)) * 128u + uint(b & 1) * 64u + lane + 32u * uint(s)] = nv_pack_q16(t, 0);
            }
        barrier();
        for (int b = 0; b < 4; ++b) {
            const uint xi = (2u * w + uint(b >> 1)) * 128u + uint(b & 1) * 64u + lane, ch = 32u * w + 8u * uint(b);
            float acc[16];
            for (int s = 0; s < 2; ++s) {
                const uvec4 x = nv_x4[xi + 32u * uint(s)];
                for (int j = 0; j < 8; ++j)
                    acc[8 * s + j] = float(nv_h(x[j >> 1])[j & 1]) * wgt_f32[pc.rs_off + ch + uint(j)];
            }
            NV_GEMM8(acc, nv_y4, pc.ct_off, NV_CF, ch)
            for (int j = 0; j < 8; ++j) { yv[0][8 * b + j] = acc[j]; yv[1][8 * b + j] = acc[8 + j]; }
        }
    }
    // y in FP16 over x (each wave its own channels: no other wave reads them), int8 for an int8 QKV
    for (int s = 0; s < 2; ++s)
        for (int b = 0; b < 4; ++b) {
            float t[16];
            for (int j = 0; j < 8; ++j) t[j] = yv[s][8 * b + j];
            nv_x4[(2u * w + uint(b >> 1)) * 128u + uint(b & 1) * 64u + lane + 32u * uint(s)] = nv_pack_q16(t, 0);
        }
    if (NV_I8QKV) {
        int ys0, ys1;
        nv_tokmax(w, lane, nv_amax(yv[0]), nv_amax(yv[1]), ys0, ys1);
        for (int r = 0; r < 2; ++r) {
            nv_y4[NV_X8(2u * w + uint(r), lane)] = nv_q16x(yv[0], 16 * r, ys0);
            nv_y4[NV_X8(2u * w + uint(r), lane + 32u)] = nv_q16x(yv[1], 16 * r, ys1);
        }
        xs0 = ys0; xs1 = ys1;                         // from here the token scales of y
    }
    barrier();

    // ---- stages 3 to 5: head w. Q, K, V = Wqkv . y; cosine-normalised Q and K, attention -----------------
    uint yres[32];                                // [16 s + p]: the residual y of channels 32w..
    float co[2][32];
    {
        const float16_t hscale = float16_t(wgt_f32[pc.s_off + w]);
#if NV_I8QK
        uint qe[2][8], ke[2][8];
        float qsc[2], ksc[2];
#else
        uint qe[2][16], ke[2][16];
#endif
        uint ve[2][16];
        for (int part = 0; part < 3; ++part) {    // Q, K, V: rows 96w + 32 part ..
            float a[2][32];
            for (int b = 0; b < 4; ++b) {
                const uint row0 = 96u * w + 32u * uint(part) + 8u * uint(b);
                if (NV_I8QKV) {
                    int acc[16];
                    for (int j = 0; j < 16; ++j) acc[j] = 0;
                    NV_GEMM8I(acc, NV_X8, pc.qkv_off, NV_CF, row0)
                    for (int j = 0; j < 8; ++j) {
                        const float ws = I8_WSCALE(nv_wf, pc.qkv_off, 3 * NR_C, NR_C, row0 + uint(j));
                        a[0][8 * b + j] = float(acc[j]) * (ws * i8_pow2(xs0));
                        a[1][8 * b + j] = float(acc[8 + j]) * (ws * i8_pow2(xs1));
                    }
                } else {
                    float acc[16];
                    for (int j = 0; j < 16; ++j) acc[j] = 0.0;
                    NV_GEMM8(acc, nv_x4, pc.qkv_off, NV_CF, row0)
                    for (int s = 0; s < 2; ++s)
                        for (int j = 0; j < 8; ++j) a[s][8 * b + j] = acc[8 * s + j];
                }
            }
            for (int s = 0; s < 2; ++s) {
#if NV_I8QK
                if (part == 0) qsc[s] = nv_qk8(a[s], 0, hscale, qe[s]);
                else if (part == 1) ksc[s] = nv_qk8(a[s], 1, hscale, ke[s]);
#else
                if (part == 0) nv_qk(a[s], 0, hscale, qe[s]);
                else if (part == 1) nv_qk(a[s], 1, hscale, ke[s]);
#endif
                else for (int p = 0; p < 16; ++p) ve[s][p] = nv_p(nv_q16(a[s][2 * p]), nv_q16(a[s][2 * p + 1]));
            }
        }
        for (int s = 0; s < 2; ++s)
            for (int q = 0; q < 4; ++q) {
                const uvec4 y = nv_x4[(2u * w + uint(q >> 1)) * 128u + uint(q & 1) * 64u + lane + 32u * uint(s)];
                for (int i = 0; i < 4; ++i) yres[16 * s + 4 * q + i] = y[i];
            }
        barrier();                                // every wave is done with y: reuse its LDS per head
#if NV_I8QK
        for (int s = 0; s < 2; ++s) nv_store_kv(w, lane + 32u * uint(s), ke[s], ksc[s], ve[s]);
#else
        for (int s = 0; s < 2; ++s) nv_store_kv(w, lane + 32u * uint(s), ke[s], ve[s]);
#endif
        subgroupMemoryBarrierShared();
        // the two tokens' attention in one rolled loop (NV_PCACHE's form)
        for (int s = 0; s < 2 + nv_opaque; s = s + 1) {
            float r[32];
#if NV_I8QK
            uint qs[8];
            for (int p = 0; p < 8; ++p) qs[p] = s == 0 ? qe[0][p] : qe[1][p];
            nv_attn(w, lane + 32u * uint(s), qs, s == 0 ? qsc[0] : qsc[1], r);
#else
            uint qs[16];
            for (int p = 0; p < 16; ++p) qs[p] = s == 0 ? qe[0][p] : qe[1][p];
            nv_attn(w, lane + 32u * uint(s), qs, r);
#endif
            for (int p = 0; p < 32; ++p) { co[0][p] = s == 0 ? r[p] : co[0][p]; co[1][p] = s == 0 ? co[1][p] : r[p]; }
        }
    }
    // ---- the context: int8 with the token's scale over every head (NV_I8WO), else FP16 on the e4m3 grid ----
    int cs0 = 0, cs1 = 0;
    if (NV_I8WO) {
        nv_tokmax(w, lane, nv_amax(co[0]), nv_amax(co[1]), cs0, cs1);
        for (int r = 0; r < 2; ++r) {
            nv_y4[NV_C8(2u * w + uint(r), lane)] = nv_q16x(co[0], 16 * r, cs0);
            nv_y4[NV_C8(2u * w + uint(r), lane + 32u)] = nv_q16x(co[1], 16 * r, cs1);
        }
    } else {
        subgroupMemoryBarrierShared();            // every lane's last K read before the context replaces it
        for (int s = 0; s < 2; ++s)
            for (int q = 0; q < 4; ++q) {
                float t[16];
                for (int j = 0; j < 8; ++j) t[j] = co[s][8 * q + j];
                nv_y4[256u * w + uint(q >> 1) * 128u + uint(q & 1) * 64u + lane + 32u * uint(s)] = nv_pack_q16(t, 0);
            }
    }
    barrier();

    // ---- stage 6: out = q(Wo . context + ars * y), channels 32w..32w+31, to the arena ------------------
    for (int b = 0; b < 4; ++b) {
        const uint ch = 32u * w + 8u * uint(b);
        float acc[16];
        for (int s = 0; s < 2; ++s)
            for (int j = 0; j < 8; ++j)
                acc[8 * s + j] = float(nv_h(yres[16 * s + 4 * b + (j >> 1)])[j & 1]) * wgt_f32[pc.ars_off + ch + uint(j)];
        if (NV_I8WO) {
            int ia[16];
            for (int j = 0; j < 16; ++j) ia[j] = 0;
            NV_GEMM8I(ia, NV_C8, pc.op_off, NV_CF, ch)
            for (int j = 0; j < 8; ++j) {
                const float ws = I8_WSCALE(nv_wf, pc.op_off, NR_C, NR_C, ch + uint(j));
                acc[j] = fma(float(ia[j]), ws * i8_pow2(cs0), acc[j]);
                acc[8 + j] = fma(float(ia[8 + j]), ws * i8_pow2(cs1), acc[8 + j]);
            }
        } else {
            NV_GEMM8(acc, nv_y4, pc.op_off, NV_CF, ch)
        }
        for (int s = 0; s < 2; ++s) {
            const uint m = (lane >> 4u) + 2u * uint(s);
            if (!nr_tile_oob(m))
                nv_a4[(pc.o_off + nr_tile_base(m) + (ch >> 4u) * 256u + (lane & 15u) * 16u + (ch & 15u)) / 8u] =
                    nv_pack_q16(acc, 8 * s);
#if NV_DS
            // The pooled copy of these 8 channels (nv_pool8) into nv_x4[(c / 8) * 16 + p], p the pooled pixel of
            // the window's 4x4; nothing reads nv_x4 in this stage.
            float v8[8];
            for (int j = 0; j < 8; ++j) v8[j] = acc[8 * s + j];
            const uvec4 pq = nv_pool8(v8);
            const uint t16 = lane & 15u;
            if ((t16 & 5u) == 0u)
                nv_x4[(ch >> 3u) * 16u + (2u * (m >> 1u) + t16 / 8u) * 4u + 2u * (m & 1u) + (t16 % 4u) / 2u] = pq;
#endif
        }
    }
#if NV_DS
    barrier();
    nv_ds_project(lane, w, 64u, 8);
#endif
}
#else
void main() {
    const int nv_opaque = int(gl_WorkGroupID.z);
    // The wave index through readfirstlane: gl_SubgroupID alone stays a VGPR on gfx103x, and so would every weight
    // address derived from it (vector loads of one address instead of scalar loads).
    const uint tid = gl_LocalInvocationID.x, lane = gl_SubgroupInvocationID, w = subgroupBroadcastFirst(gl_SubgroupID);
#if NV_UPS
    // ---- stage 0, fused upsample: the level below (2C channels at half resolution, the 4x4 pixels under this
    //      window) projected to C channels in FP16, then x = q(g * skip + up) at each token's half-resolution
    //      pixel (the emulated body's NR_WIDE_UPS_PROJECT and NR_UPS_BLEND_PK=2 prologue) ----------------------
    {
        // the 16 source pixels into nv_y4[(2kf + hx) * 16 + t], kf < 2C/16, clamped to the level below
        for (uint i = tid; i < uint(2 * NV_CF * 32); i += uint(NR_THREADS)) {
            const uint hx = i & 1u, t = (i >> 1u) & 15u, kf = i >> 5u;
            const uint px = uint(clamp(4 * int(gl_WorkGroupID.x) + 2 * pc.shift + int(t % 4u), 0, int(pc.blend_itiles_x * 4u) - 1));
            const uint py = uint(clamp(4 * int(gl_WorkGroupID.y) + 2 * pc.shift_y + int(t / 4u), 0, int(pc.blend_itiles_y * 4u) - 1));
            const uint tile = (py / 4u) * pc.blend_itiles_x + px / 4u, islot = (py % 4u) * 4u + px % 4u;
            nv_y4[(2u * kf + hx) * 16u + t] =
                nv_a4[(pc.blend_p_off + (tile * uint(2 * NV_CF) + kf) * 256u + islot * 16u + hx * 8u) / 8u];
        }
        barrier();
        // the projection, channels 32w.. of this wave, one lane a pixel (lanes 16-31 repeat 0-15), into
        // nv_y4[NV_UPB + (c / 8) * 16 + t] in FP16
        const uint NV_UPB = uint(2 * NV_CF * 32), t = lane & 15u;
        for (int b = 0; b < 4; ++b) {
            float acc[8];
            for (int j = 0; j < 8; ++j) acc[j] = 0.0;
            for (int kf = 0; kf < 2 * NV_CF + nv_opaque; kf = kf + 1) {
                const uvec4 x0 = nv_y4[uint(kf) * 32u + t], x1 = nv_y4[uint(kf) * 32u + 16u + t];
                const uint wb = NV_WROW(pc.ups_weight_off, 2 * NV_CF, 32u * w + 8u * uint(b), kf);
                for (int j = 0; j < 8; ++j) acc[j] = nv_dot8(acc[j], nv_w4[wb + 2u * uint(j)], nv_w4[wb + 2u * uint(j) + 1u], x0, x1);
            }
            if (lane < 16u)
                nv_y4[NV_UPB + (4u * w + uint(b)) * 16u + t] = uvec4(nv_p(float16_t(acc[0]), float16_t(acc[1])),
                    nv_p(float16_t(acc[2]), float16_t(acc[3])), nv_p(float16_t(acc[4]), float16_t(acc[5])),
                    nv_p(float16_t(acc[6]), float16_t(acc[7])));
        }
        barrier();
        for (uint i = tid; i < uint(NV_CF * 128); i += uint(NR_THREADS)) {
            const uint hx = i & 1u, T = (i >> 1u) & 63u, kf = i >> 7u, q = T >> 4u, slot = T & 15u;
            const uint dx = slot % 4u, dy = slot / 4u;
            const uint tx = uint(clamp(2 * int(gl_WorkGroupID.x) + pc.shift + int(q & 1u), 0, int(pc.tiles_x) - 1));
            const uint ty = uint(clamp(2 * int(gl_WorkGroupID.y) + pc.shift_y + int(q >> 1u), 0, int(pc.tiles_y) - 1));
            const uint local = ((q >> 1u) * 2u + dy / 2u) * 4u + (q & 1u) * 2u + dx / 2u;
            const uint stile = ty * (pc.blend_stiles_x != 0u ? pc.blend_stiles_x : pc.blend_tiles_x) + tx;
            const uvec4 pv = nv_y4[NV_UPB + (2u * kf + hx) * 16u + local];
            const uvec4 sv = nv_a4[(pc.blend_s_off + (stile * uint(NV_CF) + kf) * 256u + slot * 16u + hx * 8u) / 8u];
            uvec4 v;
            for (int p = 0; p < 4; ++p) {
                const uint ch = kf * 16u + hx * 8u + uint(2 * p);
                const f16vec2 g2 = unpackFloat2x16(wgt_u32[(pc.blend_g_off + ch) >> 1u]);   // ch even, gains 16-byte aligned
                v[p] = packFloat2x16(nr_quant_pair(fma(nv_h(sv[p]), g2, nv_h(pv[p]))));
            }
            if (nr_tile_oob(q)) v = uvec4(0u);
            nv_x4[kf * 128u + hx * 64u + T] = v;
        }
    }
    barrier();
#else
    // ---- stage 0: the window's x into nv_x4; tiles outside the image are zero -------------------------
    for (uint i = tid; i < uint(NV_CF * 128); i += uint(NR_THREADS)) {
        const uint hx = i & 1u, T = (i >> 1u) & 63u, kf = i >> 7u, m = T >> 4u;
        uvec4 v = uvec4(0u);
        if (!nr_tile_oob(m)) v = nv_a4[(pc.x_off + nr_tile_base(m) + kf * 256u + (T & 15u) * 16u + hx * 8u) / 8u];
        nv_x4[kf * 128u + hx * 64u + T] = v;
    }
    barrier();
#endif

    // ---- stage 1: e = q(act(E . x)) for hidden group w (128 rows), streamed into the group's middle
    //      projection m = Wmid . e (32 rows, channels 32w..32w+31), which goes to nv_y4 ------------------
    {
        float mid[64];                                    // [32 s + o]
        for (int o = 0; o < 64; ++o) mid[o] = 0.0;
        for (int hf = 0; hf < 8 + nv_opaque; hf = hf + 1) {   // 16 hidden rows a step, rolled
            uvec4 ep[4];                                  // [2 s + half]: (e[2p], e[2p+1]) of rows 16 hf + ..
            for (int hb = 0; hb < 2; ++hb) {
                float acc[16];
                for (int j = 0; j < 16; ++j) acc[j] = 0.0;
                NV_GEMM8(acc, nv_x4, pc.e_off, NV_CF, w * 128u + uint(hf) * 16u + uint(hb) * 8u)
                for (int s = 0; s < 2; ++s)
                    for (int p = 0; p < 4; ++p)
                        ep[2 * s + hb][p] = packFloat2x16(nr_quant_pair32(vec2(
                            nr_act_horner(acc[8 * s + 2 * p]), nr_act_horner_b(acc[8 * s + 2 * p + 1]))));
            }
            // Wmid: tile row (32w + o) / 16, k fragment hf; odd rows k-pair swapped (`kswap`).
            for (int o = 0; o < 32; ++o) {
                const uint wb = NV_WROW(pc.mid_off, 8, 32u * w + uint(o), hf);
                const uvec4 w0 = nv_w4[wb], w1 = nv_w4[wb + 1u];
                for (int s = 0; s < 2; ++s)
                    mid[32 * s + o] = (o & 1) != 0 ? nv_dot8s(mid[32 * s + o], w0, w1, ep[2 * s], ep[2 * s + 1])
                                                   : nv_dot8(mid[32 * s + o], w0, w1, ep[2 * s], ep[2 * s + 1]);
            }
        }
        for (int s = 0; s < 2; ++s)
            for (int b = 0; b < 4; ++b) {
                float t[16];
                for (int j = 0; j < 8; ++j) t[j] = mid[32 * s + 8 * b + j];
                nv_y4[(2u * w + uint(b >> 1)) * 128u + uint(b & 1) * 64u + lane + 32u * uint(s)] = nv_pack_q16(t, 0);
            }
    }
    barrier();

    // ---- stage 2: y = q(Ct . m + rs * x), channels 32w..32w+31, in place over x ------------------------
    for (int b = 0; b < 4; ++b) {
        const uint xi = (2u * w + uint(b >> 1)) * 128u + uint(b & 1) * 64u + lane, ch = 32u * w + 8u * uint(b);
        float acc[16];
        for (int s = 0; s < 2; ++s) {
            const uvec4 x = nv_x4[xi + 32u * uint(s)];
            for (int j = 0; j < 8; ++j)
                acc[8 * s + j] = float(nv_h(x[j >> 1])[j & 1]) * wgt_f32[pc.rs_off + ch + uint(j)];
        }
        NV_GEMM8(acc, nv_y4, pc.ct_off, NV_CF, ch)
        nv_x4[xi] = nv_pack_q16(acc, 0);
        nv_x4[xi + 32u] = nv_pack_q16(acc, 8);
    }
    barrier();

    // ---- stages 3 to 5: head w. Q, K, V = Wqkv . y; cosine-normalised Q and K, attention -----------------
    uint yres[32];                                // [16 s + p]: the residual y of channels 32w..
    {
        const float16_t hscale = float16_t(wgt_f32[pc.s_off + w]);
        uint qe[2][16], ke[2][16], ve[2][16];
        for (int part = 0; part < 3; ++part) {    // Q, K, V: rows 96w + 32 part ..
            float a[2][32];
            for (int b = 0; b < 4; ++b) {
                float acc[16];
                for (int j = 0; j < 16; ++j) acc[j] = 0.0;
                NV_GEMM8(acc, nv_x4, pc.qkv_off, NV_CF, 96u * w + 32u * uint(part) + 8u * uint(b))
                for (int s = 0; s < 2; ++s)
                    for (int j = 0; j < 8; ++j) a[s][8 * b + j] = acc[8 * s + j];
            }
            for (int s = 0; s < 2; ++s) {
                if (part == 0) nv_qk(a[s], 0, hscale, qe[s]);
                else if (part == 1) nv_qk(a[s], 1, hscale, ke[s]);
                else for (int p = 0; p < 16; ++p) ve[s][p] = nv_p(nv_q16(a[s][2 * p]), nv_q16(a[s][2 * p + 1]));
            }
        }
        for (int s = 0; s < 2; ++s)
            for (int q = 0; q < 4; ++q) {
                const uvec4 y = nv_x4[(2u * w + uint(q >> 1)) * 128u + uint(q & 1) * 64u + lane + 32u * uint(s)];
                for (int i = 0; i < 4; ++i) yres[16 * s + 4 * q + i] = y[i];
            }
        barrier();                                // every wave is done with y and m: reuse their LDS per head
        for (int s = 0; s < 2; ++s) nv_store_kv(w, lane + 32u * uint(s), ke[s], ve[s]);
        subgroupMemoryBarrierShared();
        uint cq[2][16];
#if NV_PCACHE
        // The two tokens' attention in one rolled loop, the body inlined once (unrolled, the two cached probability
        // sets spill: gfx1030 .342 256 VGPRs + 3.9 KB scratch). Rolled: gfx1030 188 VGPRs, no scratch; faster on the
        // RX 6600 (kit v4), while gfx1201 (the RX 9070 XT forced) spills it (256 + 8 KB scratch, 2-6x slower).
        for (int s = 0; s < 2 + nv_opaque; s = s + 1) {
            uint qs[16], r[16];
            for (int p = 0; p < 16; ++p) qs[p] = s == 0 ? qe[0][p] : qe[1][p];
            nv_attn(w, lane + 32u * uint(s), qs, r);
            for (int p = 0; p < 16; ++p) { cq[0][p] = s == 0 ? r[p] : cq[0][p]; cq[1][p] = s == 0 ? cq[1][p] : r[p]; }
        }
#else
        for (int s = 0; s < 2; ++s) nv_attn(w, lane + 32u * uint(s), qe[s], cq[s]);
#endif
        subgroupMemoryBarrierShared();            // every lane's last K read before the context replaces it
        for (int s = 0; s < 2; ++s)
            for (int q = 0; q < 4; ++q)
                nv_y4[256u * w + uint(q >> 1) * 128u + uint(q & 1) * 64u + lane + 32u * uint(s)] =
                    uvec4(cq[s][4 * q], cq[s][4 * q + 1], cq[s][4 * q + 2], cq[s][4 * q + 3]);
    }
    barrier();

    // ---- stage 6: out = q(Wo . context + ars * y), channels 32w..32w+31, to the arena ------------------
    for (int b = 0; b < 4; ++b) {
        const uint ch = 32u * w + 8u * uint(b);
        float acc[16];
        for (int s = 0; s < 2; ++s)
            for (int j = 0; j < 8; ++j)
                acc[8 * s + j] = float(nv_h(yres[16 * s + 4 * b + (j >> 1)])[j & 1]) * wgt_f32[pc.ars_off + ch + uint(j)];
        NV_GEMM8(acc, nv_y4, pc.op_off, NV_CF, ch)
        for (int s = 0; s < 2; ++s) {
            const uint m = (lane >> 4u) + 2u * uint(s);
            if (!nr_tile_oob(m))
                nv_a4[(pc.o_off + nr_tile_base(m) + (ch >> 4u) * 256u + (lane & 15u) * 16u + (ch & 15u)) / 8u] =
                    nv_pack_q16(acc, 8 * s);
#if NV_DS
            // The pooled copy of these 8 channels (nv_pool8) into nv_x4[(c / 8) * 16 + p], p the pooled pixel of
            // the window's 4x4; nothing reads nv_x4 in this stage.
            float v8[8];
            for (int j = 0; j < 8; ++j) v8[j] = acc[8 * s + j];
            const uvec4 pq = nv_pool8(v8);
            const uint t16 = lane & 15u;
            if ((t16 & 5u) == 0u)
                nv_x4[(ch >> 3u) * 16u + (2u * (m >> 1u) + t16 / 8u) * 4u + 2u * (m & 1u) + (t16 % 4u) / 2u] = pq;
#endif
        }
    }
#if NV_DS
    barrier();
    nv_ds_project(lane, w, 64u, 8);
#endif
}
#endif   // NV_I8

#else   // C = 32, one head, two waves

// acc[j] (+)= W[row0 + j] . v for 16 rows from row0 (16-aligned) of a [rows][32] matrix (KF = 2), v the token's
// 16 pairs; odd rows swapped when `sw` (its odd rows are stored k-pair swapped against a swapped operand).
#define NV_ROWS32(acc, woff, row0, v, sw) \
    for (int j = 0; j < 16; ++j) { \
        const uint wa = NV_WROW(woff, 2, uint(row0) + uint(j), 0), wb = NV_WROW(woff, 2, uint(row0) + uint(j), 1); \
        const uvec4 v0 = uvec4(v[0], v[1], v[2], v[3]), v1 = uvec4(v[4], v[5], v[6], v[7]); \
        const uvec4 v2 = uvec4(v[8], v[9], v[10], v[11]), v3 = uvec4(v[12], v[13], v[14], v[15]); \
        if ((sw) && (j & 1) != 0) { \
            acc[j] = nv_dot8s(acc[j], nv_w4[wa], nv_w4[wa + 1u], v0, v1); \
            acc[j] = nv_dot8s(acc[j], nv_w4[wb], nv_w4[wb + 1u], v2, v3); \
        } else { \
            acc[j] = nv_dot8(acc[j], nv_w4[wa], nv_w4[wa + 1u], v0, v1); \
            acc[j] = nv_dot8(acc[j], nv_w4[wb], nv_w4[wb + 1u], v2, v3); \
        } \
    }

void main() {
    const int nv_opaque = int(gl_WorkGroupID.z);
#if NR_TCHAIN
    // The tile-counter prologue, as fswin_t.comp's main: the frame tick and, before a fused upsample, the wait
    // for the four lower-level tiles it gathers.
    NR_TC_PROLOGUE
    if (nr_tc_rec != 0xFFFFFFFFu && nr_tc_w[nr_tc_rec + 1u] != 0u && gl_WorkGroupID.x == 0u &&
        gl_WorkGroupID.y == 0u && gl_LocalInvocationIndex == 0u)
        atomicAdd(nr_tc_u[nr_tc_w[nr_tc_rec + 4u]], 1u);
#if defined(NR_FUSED_UPS_PROJECT)
    NR_TC_WAIT_BEGIN
    for (uint nr_q = 0u; nr_q < 4u; ++nr_q) {
        const int bx = clamp(4 * int(nr_wx) + 2 * pc.shift + 3 * int(nr_q & 1u), 0, int(pc.blend_itiles_x * 4u) - 1);
        const int by = clamp(4 * int(nr_wy) + 2 * pc.shift_y + 3 * int(nr_q >> 1u), 0, int(pc.blend_itiles_y * 4u) - 1);
        NR_TC_WAIT_TILE(uint(by / 4) * pc.blend_itiles_x + uint(bx / 4))
    }
    NR_TC_WAIT_END
#endif
#endif
    const uint lane = gl_SubgroupInvocationID, w = subgroupBroadcastFirst(gl_SubgroupID);
    const uint T = 32u * w + lane, m = T >> 4u;
    const bool oob = nr_tile_oob(m);
    // x: the token's 32 channels on the e4m3 grid (the MLP operand); xr: the residual's (FP16 in the pre block)
    uint x[16], xr[16];
#if NV_PRE
    // The pre block: x = lift . f, f the 16 image features of the token's own pixel (image_input.glsl), in FP16;
    // the residual takes x in FP16, the MLP its e4m3 rounding, and E is stored k-pair swapped (`kswap`).
    {
        float ff[16];
        nr_pixel_features(gl_WorkGroupID.x * 8u + (m & 1u) * 4u + (T & 3u), gl_WorkGroupID.y * 8u + (m >> 1u) * 4u + ((T >> 2u) & 3u), ff);
        uvec4 f[2];
        for (int j = 0; j < 8; ++j) f[j >> 2][j & 3] = nv_p(NR_F16(ff[2 * j]), NR_F16(ff[2 * j + 1]));
        float16_t xh[32];
        for (int r = 0; r < 32; ++r) {
            const uint lb = (pc.image_lift_off + uint(r >> 4) * 256u + uint(r & 15) * 16u) / 8u;
            xh[r] = float16_t(nv_dot8(0.0, nv_w16[lb], nv_w16[lb + 1u], f[0], f[1]));
        }
        [[dont_flatten]] if (pc.tiles_x == 0u) {   // the emulated body's keep-alive, verbatim
            for (int r = 0; r < 32; ++r) xh[r] = float16_t(0.0);
            if (gl_NumWorkGroups.x == 0xffffffffu) act_e4m3[pc.o_off] = float16_t(0.0);
        }
        for (int p = 0; p < 16; ++p) {
            xr[p] = nv_p(xh[2 * p], xh[2 * p + 1]);
            x[p] = packFloat2x16(nr_quant_pair(nv_h(xr[p])));
        }
    }
#define NV_ESWAP true
#elif NV_POST
    // The post block: x = gs * skip + gm * up, in FP16, `up` the level below at this pixel's half-resolution
    // place (the emulated body's NR_FUSED_POST_BLEND, NR_POST_BLEND_PK); then as the pre block.
    {
        const uint tx = uint(clamp(2 * int(gl_WorkGroupID.x) + pc.shift + int(m & 1u), 0, int(pc.tiles_x) - 1));
        const uint ty = uint(clamp(2 * int(gl_WorkGroupID.y) + pc.shift_y + int(m >> 1u), 0, int(pc.tiles_y) - 1));
        const uint slot = lane & 15u, dx = slot % 4u, dy = slot / 4u;
        const uint ipx = min((4u * tx + dx) / 2u, pc.blend_itiles_x * 4u - 1u);
        const uint ipy = min((4u * ty + dy) / 2u, pc.blend_itiles_y * 4u - 1u);
        const uint itile = (ipy / 4u) * pc.blend_itiles_x + ipx / 4u, islot = (ipy % 4u) * 4u + ipx % 4u;
        const uint stw = pc.blend_stiles_x != 0u ? pc.blend_stiles_x : pc.blend_tiles_x;
        const uint stile = ty * stw + tx;
        float16_t xh[32];
        for (int k = 0; k < 2; ++k) {
            const uint pa = (pc.blend_p_off + (itile * 2u + uint(k)) * 256u + islot * 16u) / 8u;
            const uint sa = (pc.blend_s_off + (stile * 2u + uint(k)) * 256u + slot * 16u) / 8u;
            const uvec4 pv[2] = uvec4[2](nv_a4[pa], nv_a4[pa + 1u]), sv[2] = uvec4[2](nv_a4[sa], nv_a4[sa + 1u]);
            for (int j = 0; j < 8; ++j) {
                const uint ch = uint(16 * k + 2 * j);
                // the gain pair as one dword (the host puts the gains 16-byte aligned): a scalar load, not two
                // 16-bit vector loads
                const f16vec2 gm2 = unpackFloat2x16(wgt_u32[(pc.blend_g_off + 32u + ch) >> 1u]);
                const f16vec2 gs2 = unpackFloat2x16(wgt_u32[(pc.blend_g_off + ch) >> 1u]);
                const f16vec2 xh2 = fma(nv_h(sv[j >> 2][j & 3]), gs2, nv_h(pv[j >> 2][j & 3]) * gm2);
                xh[16 * k + 2 * j] = xh2.x; xh[16 * k + 2 * j + 1] = xh2.y;
            }
        }
        [[dont_flatten]] if (oob) {               // the emulated body's keep-alive, verbatim
            for (int r = 0; r < 32; ++r) xh[r] = float16_t(0.0);
            if (gl_NumWorkGroups.x == 0xffffffffu) act_e4m3[pc.o_off] = float16_t(0.0);
        }
        for (int p = 0; p < 16; ++p) {
            xr[p] = nv_p(xh[2 * p], xh[2 * p + 1]);
            x[p] = packFloat2x16(nr_quant_pair(nv_h(xr[p])));
        }
    }
#define NV_ESWAP true
#elif NV_UPS32
    // The C = 32 fused upsample: up = f16(Wup . src) for the 16 half-resolution pixels under the window (2C channels
    // each, wave w the rows 16w..), staged in nv_y4[(c / 8) * 16 + t]; then x = fma(skip, g, up) in FP16 as the post
    // block (the emulated body's NR_FUSED_UPS_PROJECT, NR_UPS_BLEND_PK=1).
    {
        const uint t = lane & 15u;
        const uint px = uint(clamp(4 * int(gl_WorkGroupID.x) + 2 * pc.shift + int(t % 4u), 0, int(pc.blend_itiles_x * 4u) - 1));
        const uint py = uint(clamp(4 * int(gl_WorkGroupID.y) + 2 * pc.shift_y + int(t / 4u), 0, int(pc.blend_itiles_y * 4u) - 1));
        const uint sb = (pc.blend_p_off + ((py / 4u) * pc.blend_itiles_x + px / 4u) * 4u * 256u + ((py % 4u) * 4u + px % 4u) * 16u) / 8u;
        uvec4 src[8];
        for (int k = 0; k < 8; ++k) src[k] = nv_a4[sb + uint(k >> 1) * 32u + uint(k & 1)];
        float up[16];
        for (int j = 0; j < 16; ++j) {
            float a = 0.0;
            for (int kf = 0; kf < 4; ++kf) {
                const uint wb = NV_WROW(pc.ups_weight_off, 4, 16u * w + uint(j), kf);
                a = nv_dot8(a, nv_w4[wb], nv_w4[wb + 1u], src[2 * kf], src[2 * kf + 1]);
            }
            up[j] = a;
        }
        if (lane < 16u)
            for (int b = 0; b < 2; ++b)
                nv_y4[(2u * w + uint(b)) * 16u + t] = uvec4(nv_p(float16_t(up[8 * b]), float16_t(up[8 * b + 1])),
                    nv_p(float16_t(up[8 * b + 2]), float16_t(up[8 * b + 3])), nv_p(float16_t(up[8 * b + 4]), float16_t(up[8 * b + 5])),
                    nv_p(float16_t(up[8 * b + 6]), float16_t(up[8 * b + 7])));
        barrier();
        const uint tx = uint(clamp(2 * int(gl_WorkGroupID.x) + pc.shift + int(m & 1u), 0, int(pc.tiles_x) - 1));
        const uint ty = uint(clamp(2 * int(gl_WorkGroupID.y) + pc.shift_y + int(m >> 1u), 0, int(pc.tiles_y) - 1));
        const uint slot = lane & 15u, dx = slot % 4u, dy = slot / 4u;
        const uint local = ((m >> 1u) * 2u + dy / 2u) * 4u + (m & 1u) * 2u + dx / 2u;
        const uint stile = ty * (pc.blend_stiles_x != 0u ? pc.blend_stiles_x : pc.blend_tiles_x) + tx;
        const uint sa = (pc.blend_s_off + stile * 2u * 256u + slot * 16u) / 8u;
        float16_t xh[32];
        for (int q = 0; q < 4; ++q) {
            const uvec4 pv = nv_y4[uint(q) * 16u + local], sv = nv_a4[sa + uint(q >> 1) * 32u + uint(q & 1)];
            for (int i = 0; i < 4; ++i) {
                const int ch = 8 * q + 2 * i;
                // the gain pair as one dword (16-byte aligned gains, ch even): a scalar load
                const f16vec2 g2 = unpackFloat2x16(wgt_u32[(pc.blend_g_off + uint(ch)) >> 1u]);
                const f16vec2 xh2 = fma(nv_h(sv[i]), g2, nv_h(pv[i]));
                xh[ch] = xh2.x; xh[ch + 1] = xh2.y;
            }
        }
        if (oob) for (int r = 0; r < 32; ++r) xh[r] = float16_t(0.0);
        for (int p = 0; p < 16; ++p) {
            xr[p] = nv_p(xh[2 * p], xh[2 * p + 1]);
            x[p] = packFloat2x16(nr_quant_pair(nv_h(xr[p])));
        }
        barrier();                                // every lane's read of the projection before K goes there
    }
#define NV_ESWAP true
#else
#define NV_ESWAP false
    {
        const uint xb = (pc.x_off + nr_tile_base(m) + (lane & 15u) * 16u) / 8u;
        for (int q = 0; q < 4; ++q) {
            const uvec4 v = oob ? uvec4(0u) : nv_a4[xb + uint(q >> 1) * 32u + uint(q & 1)];
            for (int i = 0; i < 4; ++i) x[4 * q + i] = v[i];
        }
        xr = x;
    }
#endif
#if NV_I8
    // ---- the MLP, 32 hidden units at a time: y = Ct . q(act(E . x)) + rs * x. In int8 (NV_I8E / NV_I8CT) x is
    //      one vector a token, the hidden group one a group, and Ct's row scale goes on once at the end ---------
    float yi[32], yf[32];                         // the Ct sums: int8 (each group times its scale), FP16
    for (int c = 0; c < 32; ++c) { yi[c] = 0.0; yf[c] = 0.0; }
    {
        uvec4 x0 = uvec4(0u), x1 = uvec4(0u);
        float xs = 1.0;
        if (NV_I8E) {
            float xv[32];
            for (int c = 0; c < 32; ++c) xv[c] = float(nv_h(x[c >> 1])[c & 1]);
            uint xq[8];
            xs = i8_vec32(xv, xq);
            x0 = uvec4(xq[0], xq[1], xq[2], xq[3]); x1 = uvec4(xq[4], xq[5], xq[6], xq[7]);
        }
        for (int g = 0; g < 4 + nv_opaque; g = g + 1) {   // rolled
            float e[32];
            if (NV_I8E) {
                for (int j = 0; j < 32; ++j) {
                    const uint row = 32u * uint(g) + uint(j);
                    const int d = i8_dot16(i8_dot16(0, nv_w4[I8_WROW(pc.e_off, 2, row, 0)], x0), nv_w4[I8_WROW(pc.e_off, 2, row, 1)], x1);
                    e[j] = float(d) * (xs * I8_WSCALE(nv_wf, pc.e_off, 128, 32, row));
                }
            } else {
                float e0[16], e1[16];
                for (int i = 0; i < 16; ++i) { e0[i] = 0.0; e1[i] = 0.0; }
                NV_ROWS32(e0, pc.e_off, 32 * g, x, NV_ESWAP)
                NV_ROWS32(e1, pc.e_off, 32 * g + 16, x, NV_ESWAP)
                for (int i = 0; i < 16; ++i) { e[i] = e0[i]; e[16 + i] = e1[i]; }
            }
            for (int i = 0; i < 32; ++i) e[i] = (i & 1) != 0 ? nr_act_horner_b(e[i]) : nr_act_horner(e[i]);
            if (NV_I8CT) {
                uint eq[8];
                const float es = i8_vec32(e, eq);
                const uvec4 e0 = uvec4(eq[0], eq[1], eq[2], eq[3]), e1 = uvec4(eq[4], eq[5], eq[6], eq[7]);
                for (int o = 0; o < 32; ++o) {
                    const int d = i8_dot16(i8_dot16(0, nv_w4[I8_WROW(pc.ct_off, 8, o, 2 * g)], e0), nv_w4[I8_WROW(pc.ct_off, 8, o, 2 * g + 1)], e1);
                    yi[o] = fma(float(d), es, yi[o]);
                }
            } else {
                uvec4 ep[4];
                for (int p = 0; p < 16; ++p) ep[p >> 2][p & 3] = packFloat2x16(nr_quant_pair32(vec2(e[2 * p], e[2 * p + 1])));
                // Ct: [32][128], tile row o / 16, k fragment 2g + hb; odd rows k-pair swapped (`kswap`).
                for (int o = 0; o < 32; ++o)
                    for (int hb = 0; hb < 2; ++hb) {
                        const uint wb = NV_WROW(pc.ct_off, 8, o, 2 * g + hb);
                        yf[o] = (o & 1) != 0 ? nv_dot8s(yf[o], nv_w4[wb], nv_w4[wb + 1u], ep[2 * hb], ep[2 * hb + 1])
                                             : nv_dot8(yf[o], nv_w4[wb], nv_w4[wb + 1u], ep[2 * hb], ep[2 * hb + 1]);
                    }
            }
        }
    }
    float y[32];
    for (int c = 0; c < 32; ++c)
        y[c] = (NV_I8CT ? yi[c] * I8_WSCALE(nv_wf, pc.ct_off, 32, 128, c) : yf[c]) +
               float(nv_h(xr[c >> 1])[c & 1]) * wgt_f32[pc.rs_off + uint(c)];
    uint yh[16];
    for (int p = 0; p < 16; ++p) yh[p] = nv_p(float16_t(y[2 * p]), float16_t(y[2 * p + 1]));
    // ---- Q, K, V from y (FP16, then on the e4m3 grid; int8 with the token's scale for NV_I8QKV) ----------
    {
        const float16_t hscale = float16_t(wgt_f32[pc.s_off]);
        uvec4 y0 = uvec4(0u), y1 = uvec4(0u);
        float ys = 1.0;
        uint yq[16];
        if (NV_I8QKV) {
            float yv[32];
            for (int c = 0; c < 32; ++c) yv[c] = float(nv_h(yh[c >> 1])[c & 1]);
            uint q8[8];
            ys = i8_vec32(yv, q8);
            y0 = uvec4(q8[0], q8[1], q8[2], q8[3]); y1 = uvec4(q8[4], q8[5], q8[6], q8[7]);
        } else {
            for (int p = 0; p < 16; ++p) yq[p] = packFloat2x16(nr_quant_pair(nv_h(yh[p])));
        }
#if NV_I8QK
        uint qe[8], ke[8];
        float qsc = 1.0, ksc = 1.0;
#else
        uint qe[16], ke[16];
#endif
        uint ve[16];
        for (int part = 0; part < 3; ++part) {
            float a[32];
            if (NV_I8QKV) {
                for (int j = 0; j < 32; ++j) {
                    const uint row = 32u * uint(part) + uint(j);
                    const int d = i8_dot16(i8_dot16(0, nv_w4[I8_WROW(pc.qkv_off, 2, row, 0)], y0), nv_w4[I8_WROW(pc.qkv_off, 2, row, 1)], y1);
                    a[j] = float(d) * (ys * I8_WSCALE(nv_wf, pc.qkv_off, 96, 32, row));
                }
            } else {
                float a0[16], a1[16];
                for (int i = 0; i < 16; ++i) { a0[i] = 0.0; a1[i] = 0.0; }
                NV_ROWS32(a0, pc.qkv_off, 32 * part, yq, part < 2)
                NV_ROWS32(a1, pc.qkv_off, 32 * part + 16, yq, part < 2)
                for (int i = 0; i < 16; ++i) { a[i] = a0[i]; a[16 + i] = a1[i]; }
            }
#if NV_I8QK
            if (part == 0) qsc = nv_qk8(a, 0, hscale, qe);
            else if (part == 1) ksc = nv_qk8(a, 1, hscale, ke);
#else
            if (part == 0) nv_qk(a, 0, hscale, qe);
            else if (part == 1) nv_qk(a, 1, hscale, ke);
#endif
            else for (int p = 0; p < 16; ++p) ve[p] = nv_p(nv_q16(a[2 * p]), nv_q16(a[2 * p + 1]));
        }
#if NV_I8QK
        nv_store_kv(0u, T, ke, ksc, ve);
#else
        nv_store_kv(0u, T, ke, ve);
#endif
        barrier();                                // keys and values of both waves
        float co[32];
#if NV_I8QK
        nv_attn(0u, T, qe, qsc, co);
#else
        nv_attn(0u, T, qe, co);
#endif
        // ---- out = q(Wo . context + ars * yh) --------------------------------------------------------
        float o0[16], o1[16];
        for (int j = 0; j < 16; ++j) {
            o0[j] = float(nv_h(yh[j >> 1])[j & 1]) * wgt_f32[pc.ars_off + uint(j)];
            o1[j] = float(nv_h(yh[8 + (j >> 1)])[j & 1]) * wgt_f32[pc.ars_off + 16u + uint(j)];
        }
        if (NV_I8WO) {
            uint c8[8];
            const float cs = i8_vec32(co, c8);
            const uvec4 c0 = uvec4(c8[0], c8[1], c8[2], c8[3]), c1 = uvec4(c8[4], c8[5], c8[6], c8[7]);
            float ov[32];
            for (int j = 0; j < 32; ++j) {
                const int d = i8_dot16(i8_dot16(0, nv_w4[I8_WROW(pc.op_off, 2, j, 0)], c0), nv_w4[I8_WROW(pc.op_off, 2, j, 1)], c1);
                ov[j] = float(d) * (cs * I8_WSCALE(nv_wf, pc.op_off, 32, 32, j));
            }
            for (int j = 0; j < 16; ++j) { o0[j] += ov[j]; o1[j] += ov[16 + j]; }
        } else {
            uint cq[16];
            for (int p = 0; p < 16; ++p) cq[p] = nv_p(nv_q16(co[2 * p]), nv_q16(co[2 * p + 1]));
            NV_ROWS32(o0, pc.op_off, 0, cq, true)
            NV_ROWS32(o1, pc.op_off, 16, cq, true)
        }
#else
    // ---- the MLP, streamed: y = Ct . q(act(E . x)) + rs * x ----------------------------------------
    float y[32];
    for (int c = 0; c < 32; ++c) y[c] = float(nv_h(xr[c >> 1])[c & 1]) * wgt_f32[pc.rs_off + uint(c)];
    for (int hf = 0; hf < 8 + nv_opaque; hf = hf + 1) {
        float e[16];
        for (int j = 0; j < 16; ++j) e[j] = 0.0;
        NV_ROWS32(e, pc.e_off, 16 * hf, x, NV_ESWAP)
        uvec4 ep[2];
        for (int p = 0; p < 8; ++p)
            ep[p >> 2][p & 3] = packFloat2x16(nr_quant_pair32(vec2(nr_act_horner(e[2 * p]), nr_act_horner_b(e[2 * p + 1]))));
        // Ct: [32][128], tile row o / 16, k fragment hf; odd rows k-pair swapped (`kswap`).
        for (int o = 0; o < 32; ++o) {
            const uint wb = NV_WROW(pc.ct_off, 8, o, hf);
            y[o] = (o & 1) != 0 ? nv_dot8s(y[o], nv_w4[wb], nv_w4[wb + 1u], ep[0], ep[1])
                                : nv_dot8(y[o], nv_w4[wb], nv_w4[wb + 1u], ep[0], ep[1]);
        }
    }
    // yh: y in FP16 (the attention residual), yq: on the e4m3 grid (the QKV operand).
    uint yh[16], yq[16];
    for (int p = 0; p < 16; ++p) {
        yh[p] = nv_p(float16_t(y[2 * p]), float16_t(y[2 * p + 1]));
        yq[p] = packFloat2x16(nr_quant_pair(nv_h(yh[p])));
    }
    // ---- Q, K, V; Q and K rows are k-pair swapped where odd (`kswap`), V is not --------------------
    {
        const float16_t hscale = float16_t(wgt_f32[pc.s_off]);
        uint qe[16], ke[16], ve[16];
        float a[32];
        for (int part = 0; part < 3; ++part) {
            for (int c = 0; c < 32; ++c) a[c] = 0.0;
            float a0[16], a1[16];
            for (int j = 0; j < 16; ++j) { a0[j] = 0.0; a1[j] = 0.0; }
            NV_ROWS32(a0, pc.qkv_off, 32 * part, yq, part < 2)
            NV_ROWS32(a1, pc.qkv_off, 32 * part + 16, yq, part < 2)
            for (int j = 0; j < 16; ++j) { a[j] = a0[j]; a[16 + j] = a1[j]; }
            if (part == 0) nv_qk(a, 0, hscale, qe);
            else if (part == 1) nv_qk(a, 1, hscale, ke);
            else for (int p = 0; p < 16; ++p) ve[p] = nv_p(nv_q16(a[2 * p]), nv_q16(a[2 * p + 1]));
        }
        nv_store_kv(0u, T, ke, ve);
        barrier();                                // keys and values of both waves
        uint cq[16];
        nv_attn(0u, T, qe, cq);
        // ---- out = q(Wo . context + ars * yh); Wo's odd rows k-pair swapped (`kswap`) -------------------
        float o0[16], o1[16];
        for (int j = 0; j < 16; ++j) {
            o0[j] = float(nv_h(yh[j >> 1])[j & 1]) * wgt_f32[pc.ars_off + uint(j)];
            o1[j] = float(nv_h(yh[8 + (j >> 1)])[j & 1]) * wgt_f32[pc.ars_off + 16u + uint(j)];
        }
        NV_ROWS32(o0, pc.op_off, 0, cq, true)
        NV_ROWS32(o1, pc.op_off, 16, cq, true)
#endif
#if NV_POST
        // The RGB projection of the output in FP16 (four rows with the history weight), channel pairs in order,
        // then the pixel's composition (fswin_imgout.glsl).
        {
            float16_t of[32];
            for (int j = 0; j < 16; ++j) { of[j] = float16_t(o0[j]); of[16 + j] = float16_t(o1[j]); }
            vec3 o = vec3(0.0);
            float ow = 0.0;
            const uint wp = (pc.image_w_off & 0x3FFFFFFFu) >> 1u;
            for (int d = 0; d < 16; ++d) {
                const float a = float(of[2 * d]), a1 = float(of[2 * d + 1]);
                const f16vec2 wx = unpackFloat2x16(wgt_u32[wp + uint(d)]);
                const f16vec2 wy = unpackFloat2x16(wgt_u32[wp + 16u + uint(d)]);
                const f16vec2 wz = unpackFloat2x16(wgt_u32[wp + 32u + uint(d)]);
                o.x += a * float(wx.x); o.x += a1 * float(wx.y);
                o.y += a * float(wy.x); o.y += a1 * float(wy.y);
                o.z += a * float(wz.x); o.z += a1 * float(wz.y);
#ifdef NR_TEMPORAL_HISTORY
                const f16vec2 ww = unpackFloat2x16(wgt_u32[wp + 48u + uint(d)]);
                ow += a * float(ww.x); ow += a1 * float(ww.y);
#endif
            }
            const uint q = m, slot = lane & 15u;
            do {
#include "fswin_imgout.glsl"
            } while (false);
        }
#else
        if (!oob) {
            const uint ob = (pc.o_off + nr_tile_base(m) + (lane & 15u) * 16u) / 8u;
            nv_a4[ob] = nv_pack_q16(o0, 0);
            nv_a4[ob + 1u] = nv_pack_q16(o0, 8);
            nv_a4[ob + 32u] = nv_pack_q16(o1, 0);
            nv_a4[ob + 33u] = nv_pack_q16(o1, 8);
        }
#endif
#if NV_DS
        // The pooled copy into nv_x4[(c / 8) * 16 + p] once both waves are done with V there, then the downsample
        // projection: 2C channels at the 16 pooled pixels, wave w the rows 32w.., one lane a pixel.
        barrier();
        {
            const uint t16 = lane & 15u;
            for (int b = 0; b < 4; ++b) {
                float v8[8];
                for (int j = 0; j < 8; ++j) v8[j] = b < 2 ? o0[8 * (b & 1) + j] : o1[8 * (b & 1) + j];
                const uvec4 pq = nv_pool8(v8);
                if ((t16 & 5u) == 0u)
                    nv_x4[uint(b) * 16u + (2u * (m >> 1u) + t16 / 8u) * 4u + 2u * (m & 1u) + (t16 % 4u) / 2u] = pq;
            }
        }
        barrier();
        nv_ds_project(lane, w, 32u, 4);
#endif
#if NV_PRE
        // The pooled copy (the pre block is wired like a `_ds` block): the 2x2 mean of the FP32 outputs, the pairs
        // along x summed in FP32 and rounded to FP16, the two rows added in FP16, a quarter of it on the e4m3 grid.
        // Lanes l, l^1, l^4, l^5 hold one 2x2 group; lane l & 5 == 0 writes its 32 channels.
        {
            float pm[32];
            for (int c = 0; c < 32; ++c) {
                const float v = c < 16 ? o0[c & 15] : o1[c & 15];
                const float xs = v + subgroupShuffleXor(v, 1u);
                const float16_t sm = float16_t(xs) + float16_t(subgroupShuffleXor(xs, 4u));
                pm[c] = float(sm) * 0.25;
            }
            const int tx = 2 * int(gl_WorkGroupID.x) + pc.shift + int(m & 1u);
            const int ty = 2 * int(gl_WorkGroupID.y) + pc.shift_y + int(m >> 1u);
            const uint tile = uint(ty >> 1) * pc.pool_tiles_x + uint(tx >> 1);
            const uint slot = uint(ty & 1) * 8u + uint(tx & 1) * 2u + ((lane & 15u) / 8u) * 4u + (lane % 4u) / 2u;
            if ((lane & 5u) == 0u)
                for (int n = 0; n < 2; ++n) {
                    uint d[8];
                    for (int p = 0; p < 8; ++p)
                        d[p] = packFloat2x16(nr_quant_pair32(vec2(pm[16 * n + 2 * p], pm[16 * n + 2 * p + 1])));
                    const uint pb = (pc.pool_off + (tile * 2u + uint(n)) * 256u + slot * 16u) / 8u;
                    nv_a4[pb] = uvec4(d[0], d[1], d[2], d[3]);
                    nv_a4[pb + 1u] = uvec4(d[4], d[5], d[6], d[7]);
                }
        }
#endif
#if NR_TCHAIN && NV_DS
        // The next level waits for the tiles this window's downsample wrote (fswin_t.comp's end, verbatim).
        if (nr_tc_sig != 0xFFFFFFFFu) {
            memoryBarrier(gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelease);
            barrier();
            if (gl_LocalInvocationIndex < 4u) {
                uint nr_t; NR_TC_DS_TILE(gl_LocalInvocationIndex, nr_t)
                if (nr_t != 0xFFFFFFFFu)
                    atomicAdd(nr_tc_u[nr_tc_sig + nr_t], 1u, gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelease);
            }
        }
#endif
    }
}
#endif
