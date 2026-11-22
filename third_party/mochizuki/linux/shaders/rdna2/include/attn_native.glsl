// Native RDNA2 body of the C=512 window attention (attn.comp, CCSplitSwin16HQKVAttn): NR_NATIVE=1 in pipelines.json.
//
// Same bindings, push block, grid (one workgroup a window and head, the remap), tile-counter wait and signal, weights
// (the FP16 twin, N-pair packed) and output as the emulated kernel, and the same arithmetic: every product goes
// through NR_DOT2 on the same pairs in the same order from 0, the Q/K norms, the exponent pairs, the softmax sums and
// every rounding are the emulated ones (attn_qkv_epi.glsl and the NR_ATTN_TRANSPOSE body) element by element.
//
//   1. QKV projection: lane l of wave w owns tokens l and l + 32 of the window and the 48 rows 48w .. 48w + 47 of the
//      head's 96 (Q 0-31, K 32-63, V 64-95). The rows are the same for every lane: weights as scalar uvec4 into v_dot2,
//      each feeding two tokens. Q and K go to LDS as FP16 ([row pair][token]), V quantised and transposed ([channel][token]).
//   2. Norms: token 32w + l normalises its Q and K (the emulated tree, both halves of the fragment in one lane) and
//      stores the quantised q and k rows.
//   3. Attention: query 32w + l holds its q in registers and reads every key's k row and V's rows at one address
//      (LDS broadcast): 64 logits, the exponent pairs, the sums, the probabilities, the context, one row of output.
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
#extension GL_KHR_shader_subgroup_shuffle : require
#extension GL_EXT_control_flow_attributes : require

#if NR_AWAVES != 2 || !NR_SHARE_HEAD || NR_HSPLIT != 16 || !NR_ATTN_TRANSPOSE || !NR_ATTN_EXP2 || NR_ATTN_BIAS_SEED || \
    !NR_ATTN_PAIR_QUANT || !NR_IMAGE || !NR_ATTN_OOB_BRANCH || !NR_USE_FUSED_QK_NORM || !NR_ATTN_QKSWAP || \
    !NR_ATTN_WPAIR || !NR_ATTN_KOFF || NR_ACC_F16 != 0 || NR_ATTN_EDGE || NR_ATTN_BPF || NR_OOB_SKIP || NR_ATTN_I4 || \
    NR_LOGIT_ACC_F16 != 0
#error "NR_NATIVE: the shipping attn shape only (see pipelines.json)"
#endif

// Q (rows 0-31) and K (rows 32-63) in FP16, [row pair][token] (word (R / 2) * 64 + t, row R in the low half when
// even); once every lane has read its token's rows, the same 8 KB hold q quantised ([token][8 channels], uvec4 T * 4 ..)
// and k (256 + T * 4 ..). 12 KB in all, not 20: on gfx1030 5 waves a SIMD (163 VGPRs) instead of 3 (LDS).
shared uvec4 na_qk4[512];
shared uint na_vt[32 * 32];            // V quantised, [channel][token pair]
#define na_qn(i) na_qk4[i]
#define na_kn(i) na_qk4[256u + (i)]

#ifndef NV_I8
#define NV_I8 0
#endif
#if NV_I8
// NV_I8 (the RDNA2 INT8 network): the QKV projection in int8 (i8.glsl): x one scale a token over its C channels (a first
// pass over x for it), the head's rows int8 in the twin (host role kI8Attn). Q . K and P . V stay FP16.
#include "i8.glsl"
layout(set = 0, binding = 2, std430) readonly buffer NaWgtF { float na_wf[]; };
#endif

layout(set = 0, binding = 4, std430) readonly buffer NaAct4 { uvec4 na_x4[]; };
layout(set = 0, binding = 2, std430) readonly buffer NaWgt4 { uvec4 na_w4[]; };
layout(set = 0, binding = 0, std430) writeonly buffer NaOut4 { uvec4 na_o4[]; };

f16vec2 na_h(uint u) { return unpackFloat2x16(u); }
uint na_p(f16vec2 v) { return packFloat2x16(v); }

float na_dot8(float s, uvec4 x0, uvec4 x1, uvec4 w0, uvec4 w1) {
    s = NR_DOT2(s, na_h(x0.x), na_h(w0.x)); s = NR_DOT2(s, na_h(x0.y), na_h(w0.y));
    s = NR_DOT2(s, na_h(x0.z), na_h(w0.z)); s = NR_DOT2(s, na_h(x0.w), na_h(w0.w));
    s = NR_DOT2(s, na_h(x1.x), na_h(w1.x)); s = NR_DOT2(s, na_h(x1.y), na_h(w1.y));
    s = NR_DOT2(s, na_h(x1.z), na_h(w1.z)); s = NR_DOT2(s, na_h(x1.w), na_h(w1.w));
    return s;
}

#define NA_R48(X) NR_K_32(X) X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) \
                  X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47)
// One k fragment of the projection: row R = 48 wave + r of the head (record R / 32, half (R / 16) & 1, row R % 16).
#define NA_ROW(r) { const uint R_ = r0 + uint(r); \
    const uint wq_ = wrec + (R_ >> 5u) * KF * 64u + (R_ & 15u) * 2u + ((R_ >> 4u) & 1u) + na_kf * 64u; \
    const uvec4 w0_ = na_w4[wq_], w1_ = na_w4[wq_ + 32u]; \
    acc[0][r] = na_dot8(acc[0][r], NA_XS[0][0], NA_XS[0][1], w0_, w1_); \
    acc[1][r] = na_dot8(acc[1][r], NA_XS[1][0], NA_XS[1][1], w0_, w1_); }
#define NA_XLOAD(XD) { const uint kn_ = min(na_kf + 1u, KF - 1u) * 32u; \
    XD[0][0] = na_x4[xq[0] + kn_]; XD[0][1] = na_x4[xq[0] + kn_ + 1u]; \
    XD[1][0] = na_x4[xq[1] + kn_]; XD[1][1] = na_x4[xq[1] + kn_ + 1u]; }

void main() {
#if NR_ATTN_REMAP
    {
        const uint nx = gl_NumWorkGroups.x, ny = gl_NumWorkGroups.y, nw = nx * ny;
        const uint lin = gl_WorkGroupID.x + nx * (gl_WorkGroupID.y + ny * gl_WorkGroupID.z);
        const uint G = uint(NR_ATTN_REMAP);
        const uint w = (lin / G) % nw, h = lin % G + G * (lin / (G * nw));
        nr_wgid = uvec3(w % nx, w / nx, h);
    }
#endif
    NR_PROF_BEGIN()
    nr_chain_wait()
    NR_TC_PROLOGUE
    NR_TC_WAIT_BEGIN
    for (uint nr_q = 0u; nr_q < 4u; ++nr_q) {
        const ivec2 nr_t = nr_tile(nr_q);
        if (all(greaterThanEqual(nr_t, ivec2(0))) && all(lessThan(nr_t, ivec2(pc.tiles_x, pc.tiles_y))))
            NR_TC_WAIT_TILE(uint(nr_t.y) * pc.tiles_x + uint(nr_t.x))
    }
    NR_TC_WAIT_END
    const uint lane = gl_SubgroupInvocationID;
    const uint wave = subgroupBroadcastFirst(gl_SubgroupID);
    const uint win = NR_WGID.y * pc.wins_x + NR_WGID.x;
    const uint KF = pc.C / 16u;
    for (uint hg = NR_WGID.z; hg < uint(NR_HSPLIT); hg += gl_NumWorkGroups.z) {
        const uint h = hg;
        const f16vec2 scale = f16vec2(NR_F16(wgt_f32[pc.s_off + h]), NR_F16(1.0));
        precise f16vec2 zs = f16vec2(0.0hf) * scale;
        const fe4m3vec2 zq = nr_attn_quant(zs), zv = nr_attn_quant(f16vec2(0.0hf));

        // ---- 1. projection: tokens lane and lane + 32, rows r0 .. r0 + 47
        {
            const uint r0 = 48u * wave;
            const uint wrec = pc.w_off / 8u + 3u * h * KF * 64u;
            uint xq[2];
            for (int j = 0; j < 2; ++j) {
                const uint t = lane + 32u * uint(j);
                xq[j] = (pc.x_off + nr_tile_base(t >> 4u) + (t & 15u) * 16u) / 8u;
            }
            float acc[2][48];
#if NV_I8
            int sx[2];
            for (int j = 0; j < 2; ++j) {
                float amax = 0.0;
                for (uint kf = 0u; kf < KF; kf = kf + 1u) {
                    const uvec4 x0 = na_x4[xq[j] + kf * 32u], x1 = na_x4[xq[j] + kf * 32u + 1u];
                    for (int e = 0; e < 4; ++e) {
                        const vec2 f0 = vec2(na_h(x0[e])), f1 = vec2(na_h(x1[e]));
                        amax = max(amax, max(max(abs(f0.x), abs(f0.y)), max(abs(f1.x), abs(f1.y))));
                    }
                }
                sx[j] = i8_exp(amax);
            }
            int ai[2][48];
            for (int j = 0; j < 2; ++j) for (int r = 0; r < 48; ++r) ai[j][r] = 0;
            for (uint kf = 0u; kf < KF; kf = kf + 1u) {
                uvec4 q[2];
                for (int j = 0; j < 2; ++j) {
                    const uint fb = i8_floor(sx[j]);
                    const float inv = i8_pow2(-sx[j]);
                    const uvec4 x0 = na_x4[xq[j] + kf * 32u], x1 = na_x4[xq[j] + kf * 32u + 1u];
                    for (int e = 0; e < 2; ++e) {
                        const vec2 f0 = vec2(na_h(x0[2 * e])), f1 = vec2(na_h(x0[2 * e + 1]));
                        q[j][e] = i8_pack(i8_q(f0.x, fb, inv), i8_q(f0.y, fb, inv), i8_q(f1.x, fb, inv), i8_q(f1.y, fb, inv));
                        const vec2 g0 = vec2(na_h(x1[2 * e])), g1 = vec2(na_h(x1[2 * e + 1]));
                        q[j][2 + e] = i8_pack(i8_q(g0.x, fb, inv), i8_q(g0.y, fb, inv), i8_q(g1.x, fb, inv), i8_q(g1.y, fb, inv));
                    }
                }
                for (int r = 0; r < 48; ++r) {
                    const uvec4 wv = na_w4[I8_WROW(pc.w_off, KF, 96u * h + r0 + uint(r), kf)];
                    ai[0][r] = i8_dot16(ai[0][r], wv, q[0]);
                    ai[1][r] = i8_dot16(ai[1][r], wv, q[1]);
                }
            }
            for (int r = 0; r < 48; ++r) {
                const float ws = I8_WSCALE(na_wf, pc.w_off, 1536u, pc.C, 96u * h + r0 + uint(r));
                for (int j = 0; j < 2; ++j) acc[j][r] = float(ai[j][r]) * (ws * i8_pow2(sx[j]));
            }
#else
            for (int j = 0; j < 2; ++j) for (int r = 0; r < 48; ++r) acc[j][r] = 0.0;
            uvec4 xa[2][2], xb[2][2];
            { const uint na_kf = 0u; xa[0][0] = na_x4[xq[0]]; xa[0][1] = na_x4[xq[0] + 1u];
              xa[1][0] = na_x4[xq[1]]; xa[1][1] = na_x4[xq[1] + 1u]; }
            for (uint kf = 0u; kf < KF; kf = kf + 2u) {
                { const uint na_kf = kf; NA_XLOAD(xb)
#define NA_XS xa
                  NA_R48(NA_ROW)
#undef NA_XS
                }
                { const uint na_kf = kf + 1u; NA_XLOAD(xa)
#define NA_XS xb
                  NA_R48(NA_ROW)
#undef NA_XS
                }
            }
#endif
            // Q and K rows FP16 as the emulated NR_F16(qacc); V rows quantised (zero for a token of a tile outside the
            // image), packed two tokens a word with the neighbouring lane.
            for (int j = 0; j < 2; ++j) {
                const uint t = lane + 32u * uint(j);
                const bool oob = nr_tile_oob(t >> 4u);
                for (int r = 0; r < 48; ++r) {
                    const uint R = r0 + uint(r);
                    if (R < 64u) {
                        if ((r & 1) == 0) {
                            const uint u = (R >> 1u) * 64u + t;
                            na_qk4[u >> 2u][u & 3u] = na_p(f16vec2(NR_F16(acc[j][r]), NR_F16(acc[j][r | 1])));
                        }
                    } else {
                        const float16_t v = oob ? zv.x : nr_attn_quant(f16vec2(acc[j][r], 0.0)).x;
                        const float16_t vn = unpackFloat2x16(subgroupShuffleXor(na_p(f16vec2(v, v)), 1u)).x;
                        if ((lane & 1u) == 0u) na_vt[(R - 64u) * 32u + (t >> 1u)] = na_p(f16vec2(v, vn));
                    }
                }
            }
        }
        barrier();

        // ---- 2. norms of token T (attn_qkv_epi.glsl): channel 2c + hh and 16 + 2c + hh in t[hh][c], Q in .x, K in .y
        {
            const uint T = 32u * wave + lane;
            uvec4 qo[4], ko[4];
            f16vec2 qk[32];
            for (int c = 0; c < 32; c += 2) {
                const uint uq = uint(c >> 1) * 64u + T, uk = uint(16 + (c >> 1)) * 64u + T;
                const f16vec2 qw = na_h(na_qk4[uq >> 2u][uq & 3u]), kw = na_h(na_qk4[uk >> 2u][uk & 3u]);
                qk[c] = f16vec2(qw.x, kw.x); qk[c + 1] = f16vec2(qw.y, kw.y);
            }
            barrier();                         // every lane's rows read before q and k replace them
            if (nr_tile_oob(T >> 4u)) {
                const uint zqq = na_p(f16vec2(zq.x, zq.x)), zkk = na_p(f16vec2(zq.y, zq.y));
                for (int i = 0; i < 4; ++i) { qo[i] = uvec4(zqq); ko[i] = uvec4(zkk); }
            } else {
                f16vec2 e2[2];
                for (int hh = 0; hh < 2; ++hh) {
                    precise f16vec2 t[8];
                    for (int c = 0; c < 8; ++c) {
                        precise f16vec2 sq0 = qk[2 * c + hh] * qk[2 * c + hh], sq1 = qk[16 + 2 * c + hh] * qk[16 + 2 * c + hh];
                        t[c] = sq0 + sq1;
                    }
                    precise f16vec2 u0 = t[0] + t[4], u1 = t[1] + t[5], u2 = t[2] + t[6], u3 = t[3] + t[7];
                    precise f16vec2 v0 = u0 + u2, v1 = u1 + u3;
                    precise f16vec2 e = v0 + v1;
                    e2[hh] = e;
                }
                precise f16vec2 w = e2[0] + e2[1];
                const f16vec2 norm = f16vec2(
                    NR_F16(inversesqrt(float(max(w.x, NR_F16(0.000062))))),
                    NR_F16(inversesqrt(float(max(w.y, NR_F16(0.000062))))));
                f16vec2 qkq[32];
                for (int c = 0; c < 32; ++c) {
                    precise f16vec2 n0 = qk[c] * norm;
                    precise f16vec2 s0 = n0 * scale;
                    qkq[c] = nr_attn_quant(s0);
                }
                for (int i = 0; i < 4; ++i) {
                    qo[i] = uvec4(na_p(f16vec2(qkq[8 * i].x, qkq[8 * i + 1].x)), na_p(f16vec2(qkq[8 * i + 2].x, qkq[8 * i + 3].x)),
                                  na_p(f16vec2(qkq[8 * i + 4].x, qkq[8 * i + 5].x)), na_p(f16vec2(qkq[8 * i + 6].x, qkq[8 * i + 7].x)));
                    ko[i] = uvec4(na_p(f16vec2(qkq[8 * i].y, qkq[8 * i + 1].y)), na_p(f16vec2(qkq[8 * i + 2].y, qkq[8 * i + 3].y)),
                                  na_p(f16vec2(qkq[8 * i + 4].y, qkq[8 * i + 5].y)), na_p(f16vec2(qkq[8 * i + 6].y, qkq[8 * i + 7].y)));
                }
            }
            for (int i = 0; i < 4; ++i) { na_qn(T * 4u + uint(i)) = qo[i]; na_kn(T * 4u + uint(i)) = ko[i]; }
        }
        barrier();

        // ---- 3. attention of query Q (the NR_ATTN_TRANSPOSE body)
        {
            const uint Q = 32u * wave + lane;
            uvec4 q[4];
            for (int i = 0; i < 4; ++i) q[i] = na_qn(Q * 4u + uint(i));
            // logit of key k: the K row against the Q row, channel pairs in order (NR_MMA(lg, kf, qf))
            float lg[64];
            for (int k = 0; k < 64; ++k) {
                float s = 0.0;
                for (int i = 0; i < 4; ++i) {
                    const uvec4 k4 = na_kn(uint(k) * 4u + uint(i));
                    for (int e = 0; e < 4; ++e) s = NR_DOT2(s, na_h(k4[e]), na_h(q[i][e]));
                }
                lg[k] = s;
            }
            // exponents in the emulated pairs: keys (16n + 4m + hh, 16n + 4m + 2 + hh); den[hh][2m], den[hh][2m + 1]
            const uint bq = pc.b_off + (h * 64u + Q) * 64u;
            float den[2][8];
            for (int hh = 0; hh < 2; ++hh) for (int c = 0; c < 8; ++c) den[hh][c] = 0.0;
            for (int n = 0; n < 4; ++n)
                for (int m = 0; m < 4; ++m)
                    for (int hh = 0; hh < 2; ++hh) {
                        const int ka = 16 * n + 4 * m + hh, kb = ka + 2;
                        const f16vec2 e2 = nr_swin_exp2(vec2(lg[ka] + wgt_f32[bq + uint(ka)], lg[kb] + wgt_f32[bq + uint(kb)]));
                        const float ex = float(e2.x), ey = float(e2.y);
                        lg[ka] = ex; lg[kb] = ey;
                        den[hh][2 * m] += ex; den[hh][2 * m + 1] += ey;
                    }
            float dd[8];
            for (int c = 0; c < 8; ++c) dd[c] = den[0][c] + den[1][c];
            precise float sum = ((dd[0] + dd[1]) + (dd[2] + dd[3])) + ((dd[4] + dd[5]) + (dd[6] + dd[7]));
            const float inv = 1.0 / max(sum, float(NR_SUM_FLOOR));
            uint pp[32];
            for (int n = 0; n < 4; ++n)
                for (int m = 0; m < 4; ++m)
                    for (int hh = 0; hh < 2; ++hh) {
                        const int ka = 16 * n + 4 * m + hh, kb = ka + 2;
                        const f16vec2 pq = nr_quant_pair(f16vec2(NR_F16(lg[ka] * inv), NR_F16(lg[kb] * inv)));
                        lg[ka] = float(pq.x); lg[kb] = float(pq.y);
                    }
            for (int p = 0; p < 32; ++p) pp[p] = na_p(f16vec2(NR_F16(lg[2 * p]), NR_F16(lg[2 * p + 1])));
            // context: channel d against the probabilities, key pairs in order (NR_MMA(ctx, vf, pf))
            uint outw[16];
            for (int d = 0; d < 32; d += 2) {
                float c2[2];
                for (int dd2 = 0; dd2 < 2; ++dd2) {
                    float s = 0.0;
                    for (int p = 0; p < 32; ++p) s = NR_DOT2(s, na_h(na_vt[uint(d + dd2) * 32u + uint(p)]), na_h(pp[p]));
                    c2[dd2] = s;
                }
                outw[d / 2] = na_p(nr_attn_quant(f16vec2(c2[0], c2[1])));
            }
            if (!nr_tile_oob(Q >> 4u)) {
                const uint ob = (pc.o_off + nr_tile_base(Q >> 4u) + 2u * h * 256u + (Q & 15u) * 16u) / 8u;
                na_o4[ob]       = uvec4(outw[0], outw[1], outw[2], outw[3]);
                na_o4[ob + 1u]  = uvec4(outw[4], outw[5], outw[6], outw[7]);
                na_o4[ob + 32u] = uvec4(outw[8], outw[9], outw[10], outw[11]);
                na_o4[ob + 33u] = uvec4(outw[12], outw[13], outw[14], outw[15]);
            }
        }
        barrier();
    }
    nr_chain_signal()
    nr_tchain_signal(win)
    NR_PROF_END(pc.o_off)
}
