// RDNA2 INT8 products: v_dot4_i32_i8 (dotPacked4x8AccSatEXT on signed packed ints, the AccSat form: the saturation
// is the instruction's clamp bit), i32 sums, one scale multiply after each sum.
//
// Weights: the host (nr_graph.cpp, g_i8) writes a matrix of an int8 role into the FP16 weight twin at the twin's own
// byte offset of the matrix (twice its e4m3 offset): int8 in plain tile-blocked [row/16][k/16][16 row][16 k] order,
// then one fp32 scale a row, the int8 values taken from the e4m3 ones.
//
// Activations: one power-of-two scale 2^s a vector (a token's channels, or a group of them), s from the vector's
// largest e4m3 value so that every value fits in +-127. Each value goes onto the e4m3 grid (the rounding the FP16
// body does) and onto the grid of 2^s in one rounding: the coarser of the two steps. The four octaves below the
// largest value keep every e4m3 value exactly; smaller ones round to a multiple of 2^s.
#extension GL_EXT_integer_dot_product : require
#ifndef NV_I8_RNE
#define NV_I8_RNE 0
#endif

// The uvec4 (16 k values) of row `row`, k fragment kf, of the [rows][16 KF] int8 matrix at e4m3 offset woff, and its
// row scale (`wf`: the twin as float[]).
#define I8_WROW(woff, KF, row, kf) ((woff) / 8u + ((uint(row) >> 4u) * uint(KF) + uint(kf)) * 16u + (uint(row) & 15u))
#define I8_WSCALE(wf, woff, rows, K, row) wf[(woff) / 2u + uint(rows) * uint(K) / 4u + uint(row)]

int i8_dot16(int s, uvec4 a, uvec4 b) {
    s = dotPacked4x8AccSatEXT(int(a.x), int(b.x), s);
    s = dotPacked4x8AccSatEXT(int(a.y), int(b.y), s);
    s = dotPacked4x8AccSatEXT(int(a.z), int(b.z), s);
    s = dotPacked4x8AccSatEXT(int(a.w), int(b.w), s);
    return s;
}

// s for a vector whose largest |value| is amax: floor(log2(e4m3(amax))) - 6, so e4m3(amax) / 2^s < 128.
int i8_exp(float amax) {
    const float m = nr_e4m3_round(amax);
    return m > 0.0 ? int(bitfieldExtract(floatBitsToUint(m), 23, 8)) - 133 : 0;
}

// v rounded once onto the coarser of the e4m3 grid and 2^s's (`fb`: the floor of the rounding constant's exponent,
// i8_floor), divided by 2^s (`inv`); q is the low byte of the result. Same constant trick as nr_e4m3_round.
uint i8_q(float v, uint fb, float inv) {
#if NV_I8_RNE
    // NV_I8_RNE: the int8 grid alone, round to nearest even in the fma, clamped to +-127: two instructions a value
    // instead of seven (on RDNA2 the conversions, not the dot4, decide whether int8 is faster).
    return floatBitsToUint(clamp(fma(v, inv, 12582912.0), 12582785.0, 12583039.0));
#endif
    precise float c = clamp(v, -448.0, 448.0);
    const uint magic = max(floatBitsToUint(c) & 0x7F800000u, fb) + 0x0A400000u;
    precise float sum = c + uintBitsToFloat(magic);
    precise float r = sum - uintBitsToFloat(magic);
    precise float qf = fma(r, inv, 12582912.0);   // 1.5 * 2^23: the integer r / 2^s in the low mantissa bits
    return floatBitsToUint(qf);
}
uint i8_floor(int s) { return max(0x3C800000u, uint(s + 130) << 23u); }
float i8_pow2(int s) { return uintBitsToFloat(uint(127 + s) << 23u); }
uint i8_pack(uint a, uint b, uint c, uint d) {
    return (a & 0xFFu) | ((b & 0xFFu) << 8u) | ((c & 0xFFu) << 16u) | (d << 24u);
}

// A 32-value vector as eight packed int8 dwords; returns its scale 2^s.
float i8_vec32(const float v[32], out uint q[8]) {
    float amax = 0.0;
    for (int i = 0; i < 32; ++i) amax = max(amax, abs(v[i]));
    const int s = i8_exp(amax);
    const uint fb = i8_floor(s);
    const float inv = i8_pow2(-s);
    for (int p = 0; p < 8; ++p)
        q[p] = i8_pack(i8_q(v[4 * p], fb, inv), i8_q(v[4 * p + 1], fb, inv), i8_q(v[4 * p + 2], fb, inv),
                       i8_q(v[4 * p + 3], fb, inv));
    return i8_pow2(s);
}
