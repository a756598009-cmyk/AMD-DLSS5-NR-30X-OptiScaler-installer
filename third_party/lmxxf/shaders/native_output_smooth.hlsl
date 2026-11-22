// Output-side temporal smoothing (DLSS5_OUTPUT_SMOOTH=<threshold/255>,<strength>). In place on the network RGB output:
// where the output differs little from the motion-warped previous output, blend toward it; large differences pass through.
// Suppresses the few-/255 halo shimmer around jittering edges without touching real motion. Runs before the history copy,
// so the blend is recursive (IIR); strength <= 0.8 keeps the lag short.
StructuredBuffer<float4> warped : register(t0);
RWStructuredBuffer<float> rgb : register(u0);
cbuffer Params : register(b0) { float threshold; float strength; uint pixels; uint pad; }
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
#ifdef NATIVE_WIDE_ROW
    id.x += id.y * NATIVE_WIDE_ROW; // DLSS5_NETWORK_FREE_RES surfaces beyond 65535 groups: 2D dispatch
#endif
    uint p = id.x; if (p >= pixels) return;
    float4 h = warped[p]; if (h.w <= 0) return;
    float3 o = float3(rgb[p*3], rgb[p*3+1], rgb[p*3+2]);
    float d = max(max(abs(o.x - h.x), abs(o.y - h.y)), abs(o.z - h.z));
    float w = strength * saturate(1.0 - d / threshold);
    o = lerp(o, h.xyz, w);
    rgb[p*3] = o.x; rgb[p*3+1] = o.y; rgb[p*3+2] = o.z;
}
// OptiScaler, SMOOTH_RESIDUAL: the same blend on the network's change to its input (output - base) instead of the whole
// output, so the game's own samples pass through as they came. warped then holds the previous frame's change, residual
// keeps this frame's for the next; pad = 1 when warped is valid. A zero output (the fallback) or a non-finite change is
// left as it is and stores no change.
#if SMOOTH_RESIDUAL
StructuredBuffer<float4> base : register(t1);
RWStructuredBuffer<float4> residual : register(u1);
[numthreads(64,1,1)]
void residual_main(uint3 id : SV_DispatchThreadID) {
    uint p = id.x; if (p >= pixels) return;
#ifdef NATIVE_RGB_PROCESSING_WIDTH
    // Small geometry: the network's rows are wider than the colour's; residual keeps the colour's, as the history does.
    uint x = p % NATIVE_RGB_PROCESSING_WIDTH; if (x >= NATIVE_VALID_WIDTH) return;
    uint q = p / NATIVE_RGB_PROCESSING_WIDTH * NATIVE_VALID_WIDTH + x;
#else
    uint q = p;
#endif
    float3 o = float3(rgb[p*3], rgb[p*3+1], rgb[p*3+2]);
    float3 b = base[p].xyz; float3 r = o - b;
    if (all(o == 0) || any(!isfinite(r))) { residual[q] = float4(0,0,0,1); return; }
    if (pad) {
        float3 h = warped[p].xyz;
        float d = max(max(abs(r.x-h.x), abs(r.y-h.y)), abs(r.z-h.z));
        float w = strength * saturate(1.0 - d / threshold);
        if (w > 0) { r = lerp(r, h, w); o = b + r; rgb[p*3] = o.x; rgb[p*3+1] = o.y; rgb[p*3+2] = o.z; }
    }
    residual[q] = float4(r, 1);
}
#endif
