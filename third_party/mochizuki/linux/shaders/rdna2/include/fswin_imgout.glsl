// One output pixel of the post block (NR_FUSED_IMAGE_OUTPUT): the composition with the source and the history and
// the image stores. Included inside the per-token loop of fswin_t.comp and by fswin_native.glsl, with `q` (the
// token's tile), `slot` (its place in the tile), the projection `o` and, with NR_TEMPORAL_HISTORY, `ow` in scope;
// `continue` skips a pixel outside the image.
    const int tx=2*int(nr_wx)+pc.shift+int(q&1u);
    const int ty=2*int(nr_wy)+pc.shift_y+int(q>>1u);
    const ivec2 pixel=ivec2(tx*4+int(slot%4u),ty*4+int(slot/4u));
    const ivec2 extent=imageSize(nr_out0);
    if(any(lessThan(pixel,ivec2(0))) || any(greaterThanEqual(pixel,extent)))continue;
    // **Priced and rejected: there is no packed f32 on this part.** The
    // three projection rows are three independent accumulator chains over the
    // same 32 features, so writing them as `vec2` pairs changes nothing about
    // the order any one of them sums in - it is the same f32 arithmetic asked
    // for two lanes at a time, which is what `v_pk_fma_f32` exists for. ACO
    // scalarises it straight back: **+2 instructions, VALU unchanged, and
    // `v_pk_fma_f32`, `v_pk_mul_f32` and `v_pk_add_f32` all appear exactly zero
    // times** in the emitted ISA. gfx1201 packs f16, not f32. Every "pair the
    // f32 arithmetic" idea in this project is dead on that measurement, here
    // and anywhere else.
#if !NR_IMGOUT_REGS
    vec3 o=vec3(0.0);
#ifdef NR_TEMPORAL_HISTORY
    // Row 3 of the same projection: the channel the original passes through a
    // sigmoid to weight its history blend. Rows 0..2 are the residual.
    float ow=0.0;
#endif
    // Two channels a step, with the four weight rows read as uints. The
    // accumulation order is unchanged - each product is still added to its own
    // `o` component in channel order - so this is the same arithmetic; only
    // where the weights live changes. See the WgtU32 alias above.
    const uint wpair=NR_IMG_WOFF>>1u;
    for(uint c=0u;c<32u;c+=2u) {
        const float a=float(image_features[token*32u+c]);
        const float a1=float(image_features[token*32u+c+1u]);
        const f16vec2 wx=unpackFloat2x16(wgt_u32[wpair+(c>>1u)]);
        const f16vec2 wy=unpackFloat2x16(wgt_u32[wpair+16u+(c>>1u)]);
        const f16vec2 wz=unpackFloat2x16(wgt_u32[wpair+32u+(c>>1u)]);
        o.x+=a*float(wx.x); o.x+=a1*float(wx.y);
        o.y+=a*float(wy.x); o.y+=a1*float(wy.y);
        o.z+=a*float(wz.x); o.z+=a1*float(wz.y);
#ifdef NR_TEMPORAL_HISTORY
        const f16vec2 ww=unpackFloat2x16(wgt_u32[wpair+48u+(c>>1u)]);
        ow+=a*float(ww.x); ow+=a1*float(ww.y);
#endif
        // Preserve the current FP32 output: ACO folds both narrowing casts in
        // img_out.comp. Keep the accumulation explicitly FP32 here, verified
        // against that original shader and an independent CPU oracle.
    }
#endif
    const vec2 uv=(vec2(pixel)+0.5)/vec2(textureSize(nr_src,0));
#if NR_POST_ALPHA
    // One fetch for the base colour and, when the alpha is restored, its alpha:
    // uv is the texel centre, so this is the texel texelFetch would return.
#if NR_DIAG_NOBASE
    const vec4 nr_base4=vec4(uv.x,uv.y,0.5,1.0);   // diagnostic (wrong picture): no base colour fetch
#else
    const vec4 nr_base4=textureLod(nr_src,uv,0.0);
#endif
    const vec3 base=nr_base4.rgb;
#else
    const vec3 base=textureLod(nr_src,uv,0.0).rgb;
#endif
    vec3 rgb=clamp(base+o*0.25,vec3(0.0),vec3(1.0));
#ifdef NR_TEMPORAL_HISTORY
    // The original's temporal blend, gated by the same host-resolved term the
    // pre block uses: with the gate closed this is exactly the production
    // arithmetic, which is what lets the first frame stay byte-identical.
    float w=0.0;
    if(nr_temporal[0].x!=0.0) {
#if NR_TEMPORAL_HPASS
        const vec3 hist=imageLoad(nr_hpass,pixel).rgb;
#else
        const vec2 mv=textureLod(nr_motion,uv*nr_temporal[2].xy+nr_temporal[2].zw,0.0).xy*nr_temporal[0].yz;
        const vec2 extent=nr_temporal[1].xy;
        const vec3 hist=nr_history_5tap(nr_history,(uv+mv)*extent,vec2(0.5),
                                        extent-0.5,1.0/extent);
#endif
        // ex2(x * -log2 e) then rcp(1 + that): the original's sigmoid, verbatim.
        w=clamp(1.0/(1.0+exp(-ow))*clamp(nr_temporal[0].w,0.0,1.0),0.0,1.0);
        rgb=mix(rgb,hist,w);
#ifdef NR_DIAG_HISTOUT
        rgb=hist;   // diagnostic: the post's history reconstruction as the answer
#endif
    }
    // The network's own image - the blended one the original stores - before
    // the application strength. This distinction is read out of the original's
    // Evaluate: history is a copy of the buffer the *model* wrote, while the
    // composition that produces the application output is a separate pass. Its
    // alpha carries the blend weight so a test can look at what the network
    // asked for; nothing downstream reads the history's alpha.
    const vec4 network=vec4(rgb,w);
#endif
    // **Unconditional, because the DLL's is.** The blend variant's epilogue is
    // `out2 = sat(src + sat(Intensity) * (nr - src))` (artifacts/ptx/000df0e0.ptx,
    // read 2026-09-16) - the saturate is part of the compose, not something that
    // only happens when the lerp is not the identity. Skipping it at the default
    // Intensity of 1 shipped the network's overshoot out of range, and that is
    // what breaks the picture when the host chains passes: OptiScaler's
    // ClampProxy between passes is a PER-CHANNEL saturate, so an edge texel that
    // undershoots below zero in one channel only has its hue permanently moved
    // before the next pass sees it, and the next pass sharpens the moved colour
    // and undershoots again. Measured on a game frame with the passes harness:
    // texels with some channel below zero ran 0.151% / 0.419% / 0.735% at one,
    // two and three passes, 97% of them the hue-changing kind. At Intensity 1 the
    // lerp below is the identity, so this is exactly `sat(rgb)`.
    //
    // **Above 1 the lerp is not the answer.** Extrapolating `base + k(nr-base)`
    // in RGB spreads the channels apart faster than luminance moves, which is
    // the doubled colour shift that made skin red at k=2. NVIDIA's
    // own control does reach 2 - the DLL copies Intensity out of its settings
    // *unsaturated* into the structure it hands on (settings+0xE0 at
    // 18001cf07), the DLSS 5 Toolkit documents the range as 0..2, and forks
    // measure 1.0 and 1.5 apart - so clamping it to 1 here was wrong, and it is
    // what made the slider inert on the OptiScaler route.
    //
    // 0..1 stays the DLL's own lerp. Past it the luminance *ratio* is raised to
    // the strength and the picture rescaled to sit at it, which is the same
    // form `linux/shaders/passes/runtime_transfer.comp` already uses on the other route and
    // the same one OptiScaler's resolve calls "where detail strength above 1
    // goes". One scalar from luminance applied to the whole triple, so the hue
    // is kept; bounded by the same guard of 2. At exactly 1 `pow(ratio,1)/ratio`
    // is one, so this is bit-identical to what shipped.
    rgb=clamp(fma(vec3(min(pc.image_intensity,1.0)),rgb-base,base),vec3(0.0),vec3(1.0));
    if(pc.image_intensity>1.0){
        const float fl=1.0/512.0, guard=2.0;
        const vec3 luma_w=vec3(0.2126,0.7152,0.0722);
        const float ratio=(dot(rgb,luma_w)+fl)/(dot(base,luma_w)+fl);
        const float amp=clamp(pow(max(ratio,1e-6),pc.image_intensity),1.0/guard,guard);
        rgb=clamp(rgb*(amp/max(ratio,1e-6)),vec3(0.0),vec3(1.0));
    }
#if NR_STYLE_GRADE
    // Natural and Cinematic: the DLL grades the composed answer and blends it in from the source at the
    // Intensity again (cg2r_post_process_kernel, include/nr_style_grade.glsl). Standard does not run it.
    {
        const uint nr_style=(pc.image_w_off>>28u)&3u;
        if(nr_style!=0u) rgb=nr_style_grade(nr_style,base,rgb,pc.image_intensity);
    }
#endif
#if NR_DIAG_POSTSTORE == 3
    if (rgb.x == pc.image_intensity * 1234.5)   // diagnostic: never true at runtime
#endif
#if NR_DIAG_POSTSTORE != 2
#if NR_POST_ALPHA
    {
        // linux/shaders/passes/runtime_alpha.comp's arithmetic. Its rounding is
        // floor(p + 0.5) / 255 with p = clamp(x) * 255 rounded on its own, which
        // it keeps unfused with `precise`. Here `precise` would propagate back
        // into the whole projection and unfuse ACO's v_fma_mix (a different
        // picture), so the same number is spelled without an add after the
        // multiply: p <= 255, so p + 0.5 is exact and its floor is
        // floor(p) + (fract(p) >= 0.5).
        vec4 nr_o0=vec4(rgb,1.0);
        if((pc.image_w_off&0x80000000u)!=0u) {
            vec3 nr_r=rgb;
            if((pc.image_w_off&0x40000000u)!=0u) {
                const vec3 nr_p=clamp(nr_r,vec3(0),vec3(1))*255.0;
                nr_r=(floor(nr_p)+step(vec3(0.5),fract(nr_p)))/255.0;
            }
            nr_o0=vec4(nr_r,nr_base4.a);
        }
        imageStore(nr_out0,pixel,nr_o0);
    }
#else
    imageStore(nr_out0,pixel,vec4(rgb,1.0));
#endif
#endif
#ifdef NR_TEMPORAL_HISTORY
    imageStore(nr_out1,pixel,network);
#elif NR_DIAG_POSTSTORE == 0 && !NR_POST_NO_OUT1
    // NR_POST_NO_OUT1: nothing reads this copy of the answer - every consumer
    // takes surf0, and the temporal variant writes its own history above.
    imageStore(nr_out1,pixel,vec4(rgb,1.0));
#endif
