// RDNA3 copy of the RDNA2 tree's include/vit_native.glsl (mochizuki-rdna2 22abb593d), the RDNA3 network's vitattn
// (NR_NATIVE=1 in pipelines.json): the ViT attention without cooperative matrices. Body unchanged; NR_DOT2
// and the round-to-nearest-even FP16 mode added as in attn_native.glsl. The RDNA3 host keeps the barrier between
// the QKV GEMM and this dispatch, as the RDNA2 one does.
#ifndef NR_DOT2
#define NR_DOT2(s, a, b) fma(float((a).x), float((b).x), fma(float((a).y), float((b).y), (s)))
#endif
#extension GL_EXT_spirv_intrinsics : require
spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], capabilities = [4467], 4462, 16);
//
// Native RDNA2 body of the ViT global attention (vit_attn.comp, NR_VTRANS path): NR_NATIVE=1 in pipelines.json.
//
// Same bindings, push block, grid (32 queries of one head a workgroup, one wave), tile-counter signal and output as the
// emulated kernel, and the same arithmetic: the logits and the context are FP32 sums through NR_DOT2 over the same
// pairs in the same order from 0 (the context one chain over every key), the exponent pairs, the FP16 partial sums of
// the denominator, the padding correction and every rounding are vit_attn_vt_chunk.glsl's element by element.
//
// One lane a query, its Q row in registers. The keys are the same for every lane, so each key's K and V rows are read
// at a uniform address (s_buffer_load_dwordx4) and go straight into v_dot2 as scalar operands; V pairs (two keys, one
// channel) are packed from two rows in scalar registers. Nothing in this dispatch writes the QKV tensor, and the RDNA2
// host keeps the barrier before it (no tile counters), so the scalar cache sees it whole.
#extension GL_KHR_shader_subgroup_basic : require

#if !NR_VTRANS || NR_VWAVES != 1 || NR_QT != 32 || !NR_VPRENORMALIZED || !NR_VSPLIT_TAIL || NR_VLATE_SHUFFLE || \
    NR_VKMASK || NR_VQP || !NR_VQCLAMP || !NR_VOUT_VEC || !NR_VOUT_PAIR || NR_VDUMP || NR_ACC_F16 != 0 || NR_HD != 32
#error "NR_NATIVE: the shipping vitattn shape only (see pipelines.json)"
#endif

layout(set = 0, binding = 0, std430) readonly buffer NvtAct4 { uvec4 nt_x4[]; };
layout(set = 0, binding = 2, std430) writeonly buffer NvtOut4 { uvec4 nt_o4[]; };

f16vec2 nt_h(uint u) { return unpackFloat2x16(u); }

// The 32-channel row of `ch0` (a multiple of 16) for token t of a [tokens][C] tile-blocked tensor: uvec4 index of
// channels 0-7 (+1: 8-15, +32: 16-23, +33: 24-31).
uint nt_row(uint base, uint t, uint ch0, uint C) {
    return (base + ((t >> 4u) * (C / 16u) + ch0 / 16u) * 256u + (t & 15u) * 16u) / 8u;
}
float nt_dot16(uvec4 k[4], uvec4 q[4]) {
    float s = 0.0;
    for (int i = 0; i < 4; ++i)
        for (int e = 0; e < 4; ++e) s = NR_DOT2(s, nt_h(k[i][e]), nt_h(q[i][e]));
    return s;
}

void main() {
    NR_PROF_BEGIN()
    nr_chain_wait()
    NR_TC_PROLOGUE
    const uint q0 = gl_WorkGroupID.x * uint(NR_QT);
    const uint head = gl_WorkGroupID.y;
    const uint lane = gl_SubgroupInvocationID;
    if (q0 >= pc.tokens) { nr_chain_signal() return; }
    const uint qbase = head * uint(NR_HD);
    const uint kbase = uint(NR_C) + qbase;
    const uint vbase = 2u * uint(NR_C) + qbase;
    const uint X3 = 3u * uint(NR_C);
    const uint jlast = (pc.tokens - 1u) & ~15u;
    // query row: the emulated kernel clamps a whole 16-query block to the last one
    const uint q = q0 + lane;
    const uint qr = min(q0 + (lane & ~15u), jlast) + (lane & 15u);
    uvec4 qv[4];
    {
        const uint b = nt_row(pc.x_off, qr, qbase, X3);
        qv[0] = nt_x4[b]; qv[1] = nt_x4[b + 1u]; qv[2] = nt_x4[b + 32u]; qv[3] = nt_x4[b + 33u];
    }
    float ctx[32];
    for (int d = 0; d < 32; ++d) ctx[d] = 0.0;
    float den = 0.0;
    for (uint j0 = 0u; j0 < pc.tokens; j0 = j0 + uint(NR_KC)) {
        f16vec2 part[2][2];      // [lane half hh][xa, xz]
        for (int kb = 0; kb < 4; ++kb) {
            const uint blk = j0 + uint(kb) * 16u;
            const bool valid = blk < pc.tokens;
            const uint vblk = min(blk, jlast);
            f16vec2 pv[2][4];
            for (int m = 0; m < 4; ++m) {
                float lg[4];
                uint pq[4];
                for (int r = 0; r < 4; ++r) {
                    uvec4 kr[4];
                    if (valid) {
                        const uint b = nt_row(pc.x_off, blk + uint(4 * m + r), kbase, X3);
                        kr[0] = nt_x4[b]; kr[1] = nt_x4[b + 1u]; kr[2] = nt_x4[b + 32u]; kr[3] = nt_x4[b + 33u];
                    } else {
                        for (int i = 0; i < 4; ++i) kr[i] = uvec4(0u);
                    }
                    lg[r] = nt_dot16(kr, qv);
                }
                // exponent pairs: keys (4m + hh, 4m + 2 + hh) of the block
                for (int hh = 0; hh < 2; ++hh) {
                    const f16vec2 pp = nr_vit_exp2(vec2(lg[hh], lg[hh + 2]));
                    pv[hh][m] = pp;
                    const f16vec2 pz = valid ? nr_quant_pair(pp) : f16vec2(0.0hf);
                    pq[hh] = packFloat2x16(f16vec2(pz.x, 0.0hf)) & 0xFFFFu;
                    pq[hh + 2] = packFloat2x16(f16vec2(pz.y, 0.0hf)) & 0xFFFFu;
                }
                // context: key pairs (4m, 4m + 1) and (4m + 2, 4m + 3), channel by channel
                for (int pr = 0; pr < 2; ++pr) {
                    const uint ba = nt_row(pc.x_off, vblk + uint(4 * m + 2 * pr), vbase, X3);
                    const uint bb = nt_row(pc.x_off, vblk + uint(4 * m + 2 * pr + 1), vbase, X3);
                    uvec4 va[4], vb[4];
                    va[0] = nt_x4[ba]; va[1] = nt_x4[ba + 1u]; va[2] = nt_x4[ba + 32u]; va[3] = nt_x4[ba + 33u];
                    vb[0] = nt_x4[bb]; vb[1] = nt_x4[bb + 1u]; vb[2] = nt_x4[bb + 32u]; vb[3] = nt_x4[bb + 33u];
                    const f16vec2 pp2 = nt_h(pq[2 * pr] | (pq[2 * pr + 1] << 16u));
                    for (int i = 0; i < 4; ++i)
                        for (int e = 0; e < 4; ++e) {
                            const uint a = va[i][e], b = vb[i][e];
                            const int d = 8 * i + 2 * e;
                            ctx[d]     = NR_DOT2(ctx[d],     nt_h((a & 0xFFFFu) | (b << 16u)), pp2);
                            ctx[d + 1] = NR_DOT2(ctx[d + 1], nt_h((a >> 16u) | (b & 0xFFFF0000u)), pp2);
                        }
                }
            }
            for (int hh = 0; hh < 2; ++hh) {
                const f16vec2 pa = pv[hh][0] + pv[hh][2], pz = pv[hh][1] + pv[hh][3];
                part[hh][0] = (kb == 0) ? pa : part[hh][0] + pa;
                part[hh][1] = (kb == 0) ? pz : part[hh][1] + pz;
            }
        }
        NR_F16 mine[2];
        for (int hh = 0; hh < 2; ++hh)
            mine[hh] = ((part[hh][0].x + part[hh][0].y) + part[hh][1].x) + part[hh][1].y;
        den = float(NR_F16(NR_F16(den) + NR_F16(mine[0] + mine[1])));
    }
    const uint pad_keys = ((pc.tokens + uint(NR_KC) - 1u) / uint(NR_KC)) * uint(NR_KC) - pc.tokens;
    const NR_F16 padding = NR_F16(float(pad_keys) * nr_vit_exp(0.0));
    den = float(NR_F16(NR_F16(den) - padding));
    const float floor_l = 6.198883056640625e-05 * (((pc.mode & 12u) == 12u) ? 256.0f : 1.0f);
    if (q < pc.tokens) {
        const NR_F16 inv = NR_F16(1.0 / max(den, floor_l));
        uint o[16];
        for (int c = 0; c < 32; c += 2) {
            const f16vec2 p = f16vec2(NR_F16(ctx[c]), NR_F16(ctx[c + 1])) * f16vec2(inv);
            o[c / 2] = packFloat2x16(nr_quant_pair(clamp(p, f16vec2(-448.0hf), f16vec2(448.0hf))));
        }
        const uint ob = nt_row(pc.o_off, q, qbase, uint(NR_C));
        nt_o4[ob]       = uvec4(o[0], o[1], o[2], o[3]);
        nt_o4[ob + 1u]  = uvec4(o[4], o[5], o[6], o[7]);
        nt_o4[ob + 32u] = uvec4(o[8], o[9], o[10], o[11]);
        nt_o4[ob + 33u] = uvec4(o[12], o[13], o[14], o[15]);
    }
    nr_chain_signal()
    nr_tchain_signal_wave(q0 / 16u, min(uint(NR_QT) / 16u, (pc.tokens + 15u) / 16u - q0 / 16u))
    NR_PROF_END(pc.o_off)
}
