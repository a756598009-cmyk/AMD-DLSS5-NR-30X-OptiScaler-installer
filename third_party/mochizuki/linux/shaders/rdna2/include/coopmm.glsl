// The only file in this project that defines what a matrix fragment is.
//
// RDNA2 (gfx103x, RX 6000) counterpart of linux/shaders/rdna3/include/coopmm.glsl. gfx103x has
// neither FP8 nor matrix instructions, and its drivers expose no VK_KHR_cooperative_matrix. The
// network is the RDNA3 one (e4m3 values held as FP16, see rdna3/include/coopmm.glsl), and this
// header gives it the same macro interface with no cooperative matrices: a 16x16 fragment is a
// plain array in each lane of a 32-lane subgroup, and the multiply is FP16 products summed in FP32.
//
// The fragment layout is ours to choose (nothing in hardware fixes it), so it is the one the RDNA3
// kernels were written against, which lets them compile as they are:
//
//     A   (16 components)  component c of lane l = element (l % 16, c)          [m][k]
//     B   (16 components)  component c of lane l = element (c, l % 16)          [k][n]
//     Acc ( 8 components)  component c of lane l = element (2c + l / 16, l % 16)
//
// Lanes l and l ^ 16 load the same A row and B column. As on gfx11, the product's row m is formed
// from lane half m % 2 of both operands alone, so a kernel that builds operands in registers
// differently in the two halves (the k-pair swap of the upper half, `kswap`) gets the products it
// gets on an RX 7000.
//
// Layer A of the RDNA2 port (RDNA2-MOCHIZUKI-PORT.md 5.1): correct first, slow. The A rows a lane
// needs are fetched from the other lanes of its half by subgroup shuffles; native kernels replace
// the hot ones later.
#include "coherent_act.glsl"
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_shuffle : require
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require

#define NR_ARCH_RDNA2_EMUL 1

// f32 -> f16 conversions round to nearest even in every RDNA2 entry point. Undecorated, gfx103x narrows with
// v_cvt_pkrtz_f16_f32 (toward zero) where gfx11/gfx12 round to nearest, and every FP16 value of the network would
// drift from theirs (phase 0, RDNA2-MOCHIZUKI-PORT.md). The kernels that already ask for it (NR_HALF_RTE) declare it
// themselves; the NR_HALF_RTZ experiment is left alone. Needs shaderRoundingModeRTEFloat16 (the host checks it).
#if !(defined(NR_HALF_RTE) && NR_HALF_RTE) && !(defined(NR_HALF_RTZ) && NR_HALF_RTZ)
#extension GL_EXT_spirv_intrinsics : require
spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], capabilities = [4467], 4462, 16);
#endif

#define NR_MMA_M 16
#define NR_MMA_N 16
#define NR_MMA_K 16

#define NR_E4M3 float16_t
#define NR_F16  float16_t
#ifndef NR_ROUND_HALF_UP
#define NR_ROUND_HALF_UP 0
#endif

#define NR_OPERAND NR_F16
#define NR_FRAG_A     float16_t[16]
#define NR_FRAG_B     float16_t[16]
#define NR_FRAG_A16   float16_t[16]
#define NR_FRAG_B16   float16_t[16]
#define NR_FRAG_ACC   float[8]
#define NR_FRAG_ACC16 float16_t[8]
// An Accumulator holding e4m3-grid values, FP16 here as on RDNA3 (NR_QUANT_FRAG rounds onto the grid).
#define NR_FRAG_E4M3  float16_t[8]

// A fragment with every component x. The cooperative matrix spelling, NR_FRAG_ACC(0.0), is a
// constructor an array does not have, so the few sites that fill a fragment use these.
float16_t[16] nr_fill16(float16_t x) {
    return float16_t[16](x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x);
}
float16_t[8] nr_fill8h(float16_t x) { return float16_t[8](x, x, x, x, x, x, x, x); }
float[8] nr_fill8(float x) { return float[8](x, x, x, x, x, x, x, x); }
#define NR_FILL_A(x)     nr_fill16(float16_t(x))
#define NR_FILL_B(x)     nr_fill16(float16_t(x))
#define NR_FILL_ACC(x)   nr_fill8(float(x))
#define NR_FILL_ACC16(x) nr_fill8h(float16_t(x))
#define NR_FILL_E4M3(x)  nr_fill8h(float16_t(x))
#define NR_ACC_ZERO NR_FILL_ACC(0.0)

// Accumulator to FP16 and back, round to nearest even, component by component.
float16_t[8] nr_narrow(const float a[8]) {
    float16_t r[8];
    for (int i = 0; i < 8; ++i) r[i] = float16_t(a[i]);
    return r;
}
float16_t[8] nr_narrow(const float16_t a[8]) { return a; }
float[8] nr_widen(const float16_t a[8]) {
    float r[8];
    for (int i = 0; i < 8; ++i) r[i] = float(a[i]);
    return r;
}
float[8] nr_widen(const float a[8]) { return a; }
#define NR_NARROW_ACC(acc) nr_narrow(acc)
#define NR_WIDEN_ACC(acc)  nr_widen(acc)
#define NR_WIDEN_A16(a)    (a)

// ---- loads and stores ------------------------------------------------------
//
// `arr` is indexed in its own elements, which are the fragment's component type at every site of
// this network (a cooperative matrix load would reinterpret another type). Lanes l and l ^ 16 read
// the same A row / B column; an accumulator element is written by one lane.
// A cooperative matrix load or store is one operation of the whole subgroup; here it is each lane's own loads
// and stores, and a lane reads elements other lanes wrote (an accumulator stored to LDS and read back as an
// operand by the same wave, with nothing in between). NR_FRAG_SYNC orders them as the collective operation did:
// the stores before it are visible to the subgroup's loads after it, and the compiler cannot move a lane's load
// above another lane's store.
#define NR_FRAG_SYNC() subgroupMemoryBarrier()
#define NR_LANE16 (gl_SubgroupInvocationID & 15u)
#define NR_HALF   (gl_SubgroupInvocationID >> 4)

// A [M][K] row major: lane row l % 16, components along K.
#define NR_LOAD_A(frag, arr, off, stride) do { NR_FRAG_SYNC(); \
    const uint nr_b = uint(off) + NR_LANE16 * uint(stride); \
    for (uint nr_c = 0u; nr_c < 16u; ++nr_c) (frag)[nr_c] = (arr)[nr_b + nr_c]; } while (false)
// A read from a [K][M] array.
#define NR_LOAD_A_COL(frag, arr, off, stride) do { NR_FRAG_SYNC(); \
    const uint nr_b = uint(off) + NR_LANE16; \
    for (uint nr_c = 0u; nr_c < 16u; ++nr_c) (frag)[nr_c] = (arr)[nr_b + nr_c * uint(stride)]; } while (false)
// B held [N][K] (column major): lane column l % 16, components along K.
#define NR_LOAD_B(frag, arr, off, stride) do { NR_FRAG_SYNC(); \
    const uint nr_b = uint(off) + NR_LANE16 * uint(stride); \
    for (uint nr_c = 0u; nr_c < 16u; ++nr_c) (frag)[nr_c] = (arr)[nr_b + nr_c]; } while (false)
// B held [K][N] (row major).
#define NR_LOAD_B_ROW(frag, arr, off, stride) do { NR_FRAG_SYNC(); \
    const uint nr_b = uint(off) + NR_LANE16; \
    for (uint nr_c = 0u; nr_c < 16u; ++nr_c) (frag)[nr_c] = (arr)[nr_b + nr_c * uint(stride)]; } while (false)
// Accumulator [M][N] row major: component c is row 2c + l / 16, column l % 16.
#define NR_LOAD_ACC(frag, arr, off, stride) do { NR_FRAG_SYNC(); \
    const uint nr_b = uint(off) + NR_HALF * uint(stride) + NR_LANE16; \
    for (uint nr_c = 0u; nr_c < 8u; ++nr_c) (frag)[nr_c] = (arr)[nr_b + 2u * nr_c * uint(stride)]; } while (false)
#define NR_STORE_ACC(frag, arr, off, stride) do { \
    const uint nr_b = uint(off) + NR_HALF * uint(stride) + NR_LANE16; \
    for (uint nr_c = 0u; nr_c < 8u; ++nr_c) (arr)[nr_b + 2u * nr_c * uint(stride)] = (frag)[nr_c]; NR_FRAG_SYNC(); } while (false)
// Accumulator held [N][M] (column major).
#define NR_LOAD_ACC_COL(frag, arr, off, stride) do { NR_FRAG_SYNC(); \
    const uint nr_b = uint(off) + NR_LANE16 * uint(stride) + NR_HALF; \
    for (uint nr_c = 0u; nr_c < 8u; ++nr_c) (frag)[nr_c] = (arr)[nr_b + 2u * nr_c]; } while (false)
#define NR_STORE_ACC_COL(frag, arr, off, stride) do { \
    const uint nr_b = uint(off) + NR_LANE16 * uint(stride) + NR_HALF; \
    for (uint nr_c = 0u; nr_c < 8u; ++nr_c) (arr)[nr_b + 2u * nr_c] = (frag)[nr_c]; NR_FRAG_SYNC(); } while (false)

// The e4m3 vector element types of the RDNA4 shaders are FP16 vectors here.
#define fe4m3vec2 f16vec2
#define fe4m3vec4 f16vec4

// No int4 on RDNA2 (its pipelines.json has none): NR_FRAG_I8A, NR_FRAG_I8B and NR_FRAG_IACC are not defined.

// ---- the fragment layout (RDNA3's) -----------------------------------------
#define NR_ACC_ROW_LANES 16u
#define NR_ACC_ROW(lane, c) (2u * uint(c) + ((lane) >> 4))
#define NR_ACC_COL(lane)    ((lane) & 15u)
#define NR_ACC_ROW_REDUCE(v) { \
    (v) += subgroupShuffleXor((v), 1u); \
    (v) += subgroupShuffleXor((v), 2u); \
    (v) += subgroupShuffleXor((v), 4u); \
    (v) += subgroupShuffleXor((v), 8u); }

// ---- the multiply ----------------------------------------------------------
//
// The inner product every product of the network goes through: two FP16 products added to an FP32
// sum. A product of two FP16 values is exact in FP32, so this is a sum of exact products. On gfx103x
// the instruction wanted here is v_dot2_f32_f16 (two MACs a lane a clock); which GLSL spelling the
// AMD compiler turns into it is phase 0's question, and this macro is the one place its answer goes.
#define NR_DOT2(s, a, b) fma(float((a).x), float((b).x), fma(float((a).y), float((b).y), (s)))

// acc[c] += sum_k A[2c + h][k] B[k][n] in lane l = 16h + n. Row 2c + h of A is held by lane
// 16h + 2c + h (the same half), and is fetched from it pairwise, as packed FP16, by shuffle.
void nr_mma(inout float acc[8], const float16_t a[16], const float16_t b[16]) {
    const uint h = NR_HALF;
    uint ap[8];
    f16vec2 bp[8];
    for (int k = 0; k < 8; ++k) {
        ap[k] = packFloat2x16(f16vec2(a[2 * k], a[2 * k + 1]));
        bp[k] = f16vec2(b[2 * k], b[2 * k + 1]);
    }
    for (int c = 0; c < 8; ++c) {
        const uint src = 17u * h + 2u * uint(c);
        float s = acc[c];
        for (int k = 0; k < 8; ++k) s = NR_DOT2(s, unpackFloat2x16(subgroupShuffle(ap[k], src)), bp[k]);
        acc[c] = s;
    }
}
// An FP16 accumulator: the sum in FP32, rounded to FP16 once.
void nr_mma(inout float16_t acc[8], const float16_t a[16], const float16_t b[16]) {
    float w[8];
    for (int c = 0; c < 8; ++c) w[c] = float(acc[c]);
    nr_mma(w, a, b);
    for (int c = 0; c < 8; ++c) acc[c] = float16_t(w[c]);
}
#define NR_MMA(acc, a, b) nr_mma(acc, a, b)

// Compile-time unrolling, because nothing else does it. Measured on the first
// kernel written against this header: a `for (int i = 0; i < 4; ++i)` over an
// array of accumulator fragments is left as a real loop by glslang, by
// glslang's own -Os, and by ACO, so the array is dynamically indexed and lands
// in **scratch** - 98 scratch_store_b128 in a 465-instruction shader, the whole
// register-blocking scheme silently undone. Constant indices are the entire
// difference, so the indices are generated here rather than hoped for.
//
// Two independent families so a row loop can contain a column loop; a macro
// cannot nest inside itself. Bodies take the index as a literal, and an inner
// body picks up the outer index from a `const int` in the enclosing block,
// which GLSL treats as a constant expression.
#define NR_PASTE2(a, b) a##b
#define NR_PASTE(a, b) NR_PASTE2(a, b)

// 6 is here because the QKV projection tiles 96 rows as one N block of six
// fragments; the counts are not required to be powers of two.
#define NR_I_1(X)  X(0)
#define NR_I_2(X)  X(0) X(1)
#define NR_I_3(X)  X(0) X(1) X(2)
#define NR_I_4(X)  X(0) X(1) X(2) X(3)
#define NR_I_6(X)  X(0) X(1) X(2) X(3) X(4) X(5)
#define NR_I_8(X)  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)
#define NR_J_1(X)  X(0)
#define NR_J_2(X)  X(0) X(1)
#define NR_J_3(X)  X(0) X(1) X(2)
#define NR_J_4(X)  X(0) X(1) X(2) X(3)
#define NR_J_6(X)  X(0) X(1) X(2) X(3) X(4) X(5)
#define NR_J_8(X)  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)
#define NR_K_1(X)  X(0)
#define NR_K_2(X)  X(0) X(1)
#define NR_K_3(X)  X(0) X(1) X(2)
#define NR_K_4(X)  X(0) X(1) X(2) X(3)
#define NR_K_6(X)  X(0) X(1) X(2) X(3) X(4) X(5)
#define NR_K_8(X)  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)
#define NR_K_16(X) NR_K_8(X) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
#define NR_K_32(X) NR_K_16(X) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) \
                              X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

#define NR_V_1(X)  X(0)
#define NR_V_2(X)  X(0) X(1)
#define NR_V_4(X)  X(0) X(1) X(2) X(3)
#define NR_V_8(X)  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)
#define NR_V_16(X) NR_V_8(X) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
#define NR_S_1(X)  X(0)
#define NR_S_2(X)  X(0) X(1)
#define NR_S_4(X)  X(0) X(1) X(2) X(3)
#define NR_S_8(X)  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)
#define NR_S_16(X) NR_S_8(X) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
#define NR_T_1(X)  X(0)
#define NR_T_2(X)  X(0) X(1)
#define NR_T_4(X)  X(0) X(1) X(2) X(3)
#define NR_T_8(X)  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)
#define NR_T_16(X) NR_T_8(X) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)

#define NR_FOR_I(n, X) NR_PASTE(NR_I_, n)(X)
#define NR_FOR_J(n, X) NR_PASTE(NR_J_, n)(X)
#define NR_FOR_K(n, X) NR_PASTE(NR_K_, n)(X)
#define NR_FOR_V(n, X) NR_PASTE(NR_V_, n)(X)
#define NR_FOR_S(n, X) NR_PASTE(NR_S_, n)(X)
#define NR_FOR_T(n, X) NR_PASTE(NR_T_, n)(X)

// How many contiguous elements one lane moves per memory instruction group.
// Eight f16 is 16 bytes, which is a full buffer_load_b128 per lane and 512 B
// per wave; eight e4m3 is one ds_write_b64. The number that matters is not the
// width but that the addresses are compile-time adjacent, so the loads issue
// together instead of the loop waiting once per element.
#define NR_VEC 8


// ---- quantisation onto the e4m3 grid --------------------------------------
//
// Round to nearest even, saturate at +-448 (infinity included), NaN stays NaN - the semantics
// of `cvt.rn.satfinite.e4m3x2.f16x2`, in integer and f32 arithmetic. Checked exhaustively
// against the host's reference over all 65536 f16 inputs. The f32 entry points round
// directly; an f16 input is already on f16's grid, so it is the same function.
#include "e4m3_emul.glsl"

f16vec2 nr_quant_pair(f16vec2 v) { return nr_e4m3_round_pair(v); }
f16vec2 nr_quant_pair32(vec2 x)  { return f16vec2(nr_quant_e4m3(x.x), nr_quant_e4m3(x.y)); }
f16vec2 nr_quant_pair_bare(f16vec2 v) { return nr_quant_pair(v); }
#define NR_HAVE_QUANT_PAIR32 1
#define NR_QUANT_PAIRED (NR_ACC_F16 == 0)

// Whole-fragment form of the above, for a conversion that was `NR_FRAG_E4M3(acc)`. Per-component
// work is coordinate-free, so it is valid whatever the layout.
#define NR_QUANT_FRAG(dst, src) { \
    for (uint nr_qi = 0u; nr_qi < uint((dst).length()); ++nr_qi) \
        (dst)[nr_qi] = nr_quant_e4m3(float((src)[nr_qi])); }

// `cc_vit_1d_ffn_expand_fp8`, where removing it takes the gold score from
// 98.35% to 0.06%.
float nr_mp_cubic_silu(float x) {
    const float t = clamp(x, -4.0, 4.0);
    return x * (-0.055908203125 * abs(t) * t + 0.447265625 * t + 0.89453125);
}

// Round an f32 to f16 precision and keep it in f32, round-to-nearest-even.
//
// Written on the bits because **the natural spellings do not survive this
// toolchain**. `float(float16_t(x))` is deleted outright, and so is
// `float(uint16BitsToFloat16(float16BitsToUint16(float16_t(x))))` - the
// bitcasts are no-ops in NIR, so ACO still sees f2f32(f2f16(x)) and folds it.
// A cooperative matrix conversion down and back is folded too, which is worth
// knowing because it looks like it should survive - an f16-accumulator probe
// reports it identical to the unrounded accumulator in 256 of 256 components.
// This is ours, not the hardware's: ROCm's clang compiled the same round trip
// for gfx1201 as `s_cvt_f16_f32` followed by `s_cvt_f32_f16`, both present.
// Measured, not suspected: two builds of gemm1x1 differing only in three such
// round trips produced **byte-identical ISA and byte-identical output**. A
// reference model that leans on an intermediate rounding therefore stops being
// that model without anything in the source changing.
//
// This is validation machinery. Our own paths deliberately carry f32 to the
// single narrowing the trained graph asks for; this exists to reproduce
// NVIDIA's f16 accumulator when the question is what *they* computed - the
// split-K f16 atomics and the attention softmax will both want it.
//
// Overflow past f16's range goes to infinity rather than saturating, which is
// what an f16 rounding does; the e4m3 quantiser saturates separately.
// The same rounding in four instructions, for the one place that can prove it
// does not need the other twenty-six: an FP8 matmul accumulator on its way to
// an e4m3 output.
//
// Adding 0x0FFF plus the surviving bit's own lsb and then clearing the low
// thirteen bits is exact round-to-nearest-even onto f16's ten mantissa bits,
// including the carry into the exponent. What it does not do is renormalise
// into f16's denormals or overflow to infinity, and here neither can be
// observed:
//
//   denormals  f16's smallest normal is 6.1e-05 and e4m3's smallest denormal is
//              1.95e-03, so everything this path keeps more precisely than f16
//              would is quantised to an e4m3 zero either way.
//   overflow   f16 stops at 65504 and e4m3 saturates at 448, so an infinity and
//              a large finite value both leave as 448.
//
// So this is not an approximation of nr_round_f16 in this context - it is the
// same function on the reachable domain. Anywhere the result is not immediately
// quantised to e4m3, use nr_round_f16 instead.
float nr_round_f16_e4m3(float x) {
    const uint u = floatBitsToUint(x);
#if NR_ROUND_HALF_UP
    // Two instructions instead of three: drop the `(u >> 13) & 1` term, which
    // exists only to break exact ties toward even. Round-half-away-from-zero
    // and round-half-even differ on precisely the inputs whose low thirteen
    // bits are 0x1000 and nowhere else - about one accumulator component in
    // 8192 for a sum with no particular alignment. The claim that this is free
    // is a claim about a score, so it is measured rather than argued.
    //
    // **It is measured, and it is not free. Default 0 and leave it there.**
    // The tie-break is biased - it always rounds away from zero - so unlike an
    // unbiased error it accumulates along a reduction instead of cancelling,
    // and the cost therefore grows with how much summing a kernel does:
    //
    //   fswin_t C=32   kernel/gold 91.3% -> 91.4%   (2 k-steps a rounding)
    //   fswin_t C=64               83.9% -> 83.8%
    //   fswin_t C=128              75.0% -> 74.6%
    //   ffwd3   C=512   per group  95.1/92.4/95.0/95.6/91.0/94.8/94.5/95.1
    //                          ->  93.2/89.1/92.8/93.2/87.0/92.8/92.4/93.2
    //                              and over-4-ulp components 602 -> 1327
    //
    // Two points a group on the three-stage chain is not a rounding detail.
    // What makes it tempting is that it is worth **1.88x on fswin_t at C=256**
    // (309.8 us -> 164.5 at the 1080p window count) and 1.25x at C=64, because
    // that kernel is dominated by this function. But C=256 is the one width
    // with no whole-block gold in the corpus, so the width where the win is
    // largest is the width where the error cannot be checked - and the trend
    // above says a longer reduction is where it gets worse, not better.
    // **Revisit only once an NVIDIA capture has a C=256 fused-Swin reference.**
    return uintBitsToFloat((u + 0x1000u) & 0xFFFFE000u);
#else
    return uintBitsToFloat((u + 0x0FFFu + ((u >> 13) & 1u)) & 0xFFFFE000u);
#endif
}

// The same rounding the hardware already has, in two instructions.
//
// gfx1201 has `v_cvt_f16_f32` and `v_cvt_f32_f16`, both round-to-nearest-even
// and both handling denormals and overflow exactly as f16 defines them - which
// is `nr_round_f16`'s entire 26-instruction body. The reason this project has
// a hand-written one at all is that **ACO deletes the round trip**: an f32 ->
// f16 -> f32 sequence is a no-op to an optimiser that is not told the narrowing
// is semantically required, and two builds of `gemm1x1` differing only in three
// such round trips came out byte-identical in ISA.
//
// `OpQuantizeToF16` is the SPIR-V operation that means exactly "round this f32
// to f16 precision and keep it an f32", and it is defined to be non-removable.
// glslang emits it for `unpackHalf2x16(packHalf2x16(v)).x`... but `packHalf2x16`
// on this part is `v_cvt_pkrtz_f16_f32`, round-toward-**zero**, so that spelling
// is the wrong function. The one that works is a round trip through a variable
// the optimiser cannot see through.
//
// Whether any spelling survives is a codegen question, not a language one, so
// NR_ROUND_MODE selects it and the fswin benchmark measures the result:
//   0  nr_round_f16_e4m3, the 3-instruction bit twiddle (default, measured)
//   1  the hardware converter via float16_t
//   2  nr_round_f16, the exact 26-instruction reference
//   3  a round trip through the **f16 Accumulator fragment type**, which is
//      packed two components to a VGPR - so if ACO emits it at all it is four
//      instructions for eight components rather than forty. Mode 1 showed the
//      scalar round trip gets deleted; a coopmat type conversion is a different
//      thing and may not be. NR_RND_FRAG below is the whole-fragment form,
//      because this one cannot be expressed per component.
#ifndef NR_ROUND_MODE
#define NR_ROUND_MODE 0
#endif
float nr_round_f16_hw(float x) {
    return float(float16_t(x));
}

float nr_round_f16(float x) {
    const uint u = floatBitsToUint(x);
    const uint sgn = u & 0x80000000u;
    const int  e   = int((u >> 23) & 0xFFu) - 127;
    if (e >= 128) return x;                                   // inf or NaN
    if (e > 15)   return uintBitsToFloat(sgn | 0x7F800000u);  // out of f16 range
    const int shift = (e < -14) ? (-1 - e) : 13;              // 13 + (-14 - e)
    if (shift > 24) return uintBitsToFloat(sgn);              // rounds to zero
    const uint m24  = (u & 0x7FFFFFu) | 0x800000u;
    const uint keep = m24 >> shift;
    const uint rest = m24 & ((1u << uint(shift)) - 1u);
    const uint tie = 1u << uint(shift - 1);
    const uint r = keep + ((rest > tie || (rest == tie && (keep & 1u) != 0u)) ? 1u : 0u);
    // Rounding a finite value can carry into the half infinity encoding.
    if (e == 15 && r == 2048u) return uintBitsToFloat(sgn | 0x7F800000u);
    // r has at most 12 bits and ldexp by a small exponent is exact, so the
    // reconstruction introduces no rounding of its own. A carry out of the
    // significand needs no renormalisation: the value it denotes is unchanged.
    const float v = ldexp(float(r), e - 23 + shift);
    return sgn != 0u ? -v : v;
}
