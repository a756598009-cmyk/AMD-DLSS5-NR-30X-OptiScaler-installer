// The V part of attn_qkv_epi.glsl, unchanged, for the constant heads (attn.comp NR_CONSTH): no Q/K norm, no
// Q/K stores.
            for (int i = 0; i < NR_AM; ++i) {
                NR_FRAG_E4M3 vfr0, vfr1;
#if NR_ATTN_EDGE
                [[dont_flatten]] if (NR_QDEAD(i) || nr_tile_oob(mtok0 / 16u + uint(i))) {
#else
                [[dont_flatten]] if (nr_tile_oob(mtok0 / 16u + uint(i))) {
#endif
                    const fe4m3vec2 zv = nr_attn_quant(f16vec2(0.0hf));
                    for (int cc = 0; cc < 8; ++cc) { vfr0[cc] = zv.x; vfr1[cc] = zv.y; }
                } else {
                    for (int cc = 0; cc < 8; ++cc) {
                        const fe4m3vec2 v = nr_attn_quant(f16vec2(qacc[i][4][cc], qacc[i][5][cc]));
                        vfr0[cc] = v.x; vfr1[cc] = v.y;
                    }
                }
                const uint tb = mtok0 + uint(i) * 16u;
                NR_STORE_ACC_COL(vfr0, lds_v, vb + NR_VINDEX(tb, 0u), uint(NR_VS));
                NR_STORE_ACC_COL(vfr1, lds_v, vb + NR_VINDEX(tb, 16u), uint(NR_VS));
            }
