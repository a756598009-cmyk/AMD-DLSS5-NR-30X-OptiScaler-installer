// mochizuki's RenoDX stage ([DlssNr] MochizukiReno*, MochizukiNrControls::reno_*): the key, its default and range, its
// menu group and label, the controls member it fills, and its help. Keys, defaults and ranges are
// TheAutomatic/dlss-5-amd-project's, RenoDX's own (nr_renodx.hpp Settings). Included with MZR_F (float), MZR_U (choice
// or count) and MZR_B (switch) defined. Groups: 1 Network, 2 Passes, 3 Pass 2, 4 Pass 3, 5 Pass 4
// and later, 6 Composition, 7 Resolution, 8 Adjust NR's look, 9 Encoding, 10 Guides.
MZR_F(MochizukiRenoIntensity, 1.0f, 0.0f, 2.0f, 1, "Intensity", reno_intensity,
      "Pass 1's model intensity, and every pass's unless it has its own.")
MZR_U(MochizukiRenoStyle, 0u, 0, 2, 1, "Style", reno_style, "The model's Style: Standard, Natural or Cinematic.")
MZR_F(MochizukiRenoLocalTone, 1.0f, 0.0f, 2.0f, 1, "Local tone", reno_local_tone, "The model's local lighting.")
MZR_F(MochizukiRenoLocalStructure, 1.0f, 0.0f, 2.0f, 1, "Local structure", reno_local_structure,
      "The model's structure strength.")
MZR_F(MochizukiRenoSkinStructure, 1.0f, 0.0f, 1.0f, 1, "Skin structure", reno_skin_structure,
      "Structure on skin, 0 to 1 (1 is the model's default). Needs the auto mask.")
MZR_B(MochizukiRenoAutoMask, false, 0, 1, 1, "Auto mask", reno_auto_mask,
      "The model's character mask, so skin gets its own structure.")
MZR_B(MochizukiRenoSkinIndependent, true, 0, 1, 1, "Independent skin", reno_skin_independent,
      "With the mask: structure is sent at most 1 and skin as 1, so structure no longer moves faces.")
MZR_U(MochizukiRenoPasses, 1u, 1, 30, 2, "Passes", reno_passes,
      "Model passes in the stage, each on the last one's result: 1-4, or 1-30 unlocked. Each adds the network's whole"
      "\ncost. A change rebuilds the network.")
MZR_B(MochizukiRenoUnlockPasses, false, 0, 1, 2, "Unlock passes", reno_unlock_passes,
      "Raises the pass limit from 4 to 30. Passes 5 and later take pass 4's controls.")
MZR_B(MochizukiRenoChainedHistory, true, 0, 1, 2, "Chained history", reno_chained_history,
      "Passes 2 and later keep a history of their own. Off, they start again every frame.")
MZR_F(MochizukiRenoPass2Transfer, 1.0f, 0.0f, 1.0f, 3, "Transfer strength", reno_pass_transfer_0,
      "How much of this pass's brightness change is kept.")
MZR_F(MochizukiRenoPass2Color, 1.0f, 0.0f, 1.0f, 3, "Colour strength", reno_pass_color_0,
      "How much of this pass's colour change is kept.")
MZR_F(MochizukiRenoPass2Intensity, 1.0f, 0.0f, 2.0f, 3, "Intensity", reno_pass_intensity_0,
      "This pass's model intensity.")
MZR_B(MochizukiRenoPass2FollowPass1, true, 0, 1, 3, "Same as pass 1", reno_pass_follow_0,
      "Style, tone, structure, skin and mask from pass 1. Off uses the ones below.")
MZR_U(MochizukiRenoPass2Style, 0u, 0, 2, 3, "Style", reno_pass_style_0, "This pass's Style.")
MZR_F(MochizukiRenoPass2LocalTone, 1.0f, 0.0f, 2.0f, 3, "Local tone", reno_pass_local_tone_0,
      "This pass's local lighting.")
MZR_F(MochizukiRenoPass2Structure, 1.0f, 0.0f, 2.0f, 3, "Local structure", reno_pass_local_structure_0,
      "This pass's structure strength.")
MZR_F(MochizukiRenoPass2Skin, 1.0f, 0.0f, 1.0f, 3, "Skin structure", reno_pass_skin_structure_0,
      "This pass's skin structure, 0 to 1 (1 is the model's default).")
MZR_B(MochizukiRenoPass2AutoMask, false, 0, 1, 3, "Auto mask", reno_pass_auto_mask_0, "This pass's character mask.")
MZR_F(MochizukiRenoPass3Transfer, 1.0f, 0.0f, 1.0f, 4, "Transfer strength", reno_pass_transfer_1,
      "How much of this pass's brightness change is kept.")
MZR_F(MochizukiRenoPass3Color, 1.0f, 0.0f, 1.0f, 4, "Colour strength", reno_pass_color_1,
      "How much of this pass's colour change is kept.")
MZR_F(MochizukiRenoPass3Intensity, 1.0f, 0.0f, 2.0f, 4, "Intensity", reno_pass_intensity_1,
      "This pass's model intensity.")
MZR_B(MochizukiRenoPass3FollowPass1, true, 0, 1, 4, "Same as pass 1", reno_pass_follow_1,
      "Style, tone, structure, skin and mask from pass 1. Off uses the ones below.")
MZR_U(MochizukiRenoPass3Style, 0u, 0, 2, 4, "Style", reno_pass_style_1, "This pass's Style.")
MZR_F(MochizukiRenoPass3LocalTone, 1.0f, 0.0f, 2.0f, 4, "Local tone", reno_pass_local_tone_1,
      "This pass's local lighting.")
MZR_F(MochizukiRenoPass3Structure, 1.0f, 0.0f, 2.0f, 4, "Local structure", reno_pass_local_structure_1,
      "This pass's structure strength.")
MZR_F(MochizukiRenoPass3Skin, 1.0f, 0.0f, 1.0f, 4, "Skin structure", reno_pass_skin_structure_1,
      "This pass's skin structure, 0 to 1 (1 is the model's default).")
MZR_B(MochizukiRenoPass3AutoMask, false, 0, 1, 4, "Auto mask", reno_pass_auto_mask_1, "This pass's character mask.")
MZR_F(MochizukiRenoPass4Transfer, 1.0f, 0.0f, 1.0f, 5, "Transfer strength", reno_pass_transfer_2,
      "How much of the brightness change of pass 4 and later is kept.")
MZR_F(MochizukiRenoPass4Color, 1.0f, 0.0f, 1.0f, 5, "Colour strength", reno_pass_color_2,
      "How much of the colour change of pass 4 and later is kept.")
MZR_F(MochizukiRenoPass4Intensity, 1.0f, 0.0f, 2.0f, 5, "Intensity", reno_pass_intensity_2,
      "The model intensity of pass 4 and later.")
MZR_B(MochizukiRenoPass4FollowPass1, true, 0, 1, 5, "Same as pass 1", reno_pass_follow_2,
      "Style, tone, structure, skin and mask from pass 1. Off uses the ones below.")
MZR_U(MochizukiRenoPass4Style, 0u, 0, 2, 5, "Style", reno_pass_style_2, "The Style of pass 4 and later.")
MZR_F(MochizukiRenoPass4LocalTone, 1.0f, 0.0f, 2.0f, 5, "Local tone", reno_pass_local_tone_2,
      "The local lighting of pass 4 and later.")
MZR_F(MochizukiRenoPass4Structure, 1.0f, 0.0f, 2.0f, 5, "Local structure", reno_pass_local_structure_2,
      "The structure strength of pass 4 and later.")
MZR_F(MochizukiRenoPass4Skin, 1.0f, 0.0f, 1.0f, 5, "Skin structure", reno_pass_skin_structure_2,
      "The skin structure of pass 4 and later, 0 to 1 (1 is the model's default).")
MZR_B(MochizukiRenoPass4AutoMask, false, 0, 1, 5, "Auto mask", reno_pass_auto_mask_2,
      "The character mask of pass 4 and later.")
MZR_F(MochizukiRenoColorStrength, 1.0f, 0.0f, 1.0f, 6, "Colour strength", reno_color_strength,
      "How much of pass 1's colour change is kept; 0 keeps the game's colour at the new brightness.")
MZR_F(MochizukiRenoTransferStrength, 1.0f, 0.0f, 1.0f, 6, "Transfer strength", reno_transfer_strength,
      "How much of pass 1's brightness change is kept.")
MZR_F(MochizukiRenoChromaClamp, 1.0f, 0.25f, 2.0f, 6, "Chroma clamp (stops)", reno_chroma_clamp_stops,
      "The largest colour change let through, in stops.")
MZR_U(MochizukiRenoTransferMode, 0u, 0, 1, 6, "Transfer mode", reno_transfer_mode,
      "Bounded ratio, RenoDX's default, or Consistent.")
MZR_F(MochizukiRenoStyleHighlightGuard, 0.0f, 0.0f, 2.0f, 6, "Highlight guard (stops)", reno_style_highlight_guard,
      "The most the model may darken a highlight, in stops: lower holds highlights more (0.25 is strong), 2 changes "
      "nothing and 0 is off. HDR games on the Display codec only (Codec Auto picks it for most), and only where the "
      "model darkens bright areas, as the Natural and Cinematic styles do.")
MZR_U(MochizukiRenoDetailStability, 0u, 0, 2, 6, "Detail stability", reno_detail_stability,
      "Steadies fine detail between frames. Auto decides from where NR runs.")
MZR_U(MochizukiRenoSdrBlackRestore, 0u, 0, 3, 6, "SDR black restore", reno_sdr_black_restore,
      "SDR: puts back the game's black level the model lifted. Auto, in every block, smoothed, or off (upstream's default since 0.0.8.3).")
MZR_U(MochizukiRenoDisplayPedestal, 0u, 0, 3, 6, "HDR dark pedestal", reno_display_pedestal,
      "HDR: takes off the dark lift the model adds. Auto, always, smoothed, or off (upstream's default since 0.0.8.3).")
MZR_U(MochizukiRenoCodecMode, 2u, 0, 3, 6, "Codec", reno_codec_mode,
      "How HDR is encoded for the model: Classic, Anchored, Auto (default) or Display.")
MZR_B(MochizukiRenoNeuralFloorGuard, true, 0, 1, 6, "Neural floor guard", reno_neural_floor_guard,
      "Keeps the model from crushing near-black detail.")
MZR_F(MochizukiRenoResolutionScale, 1.0f, 0.33f, 1.0f, 7, "Resolution scale", reno_resolution_scale,
      "The network's resolution as a share of the frame, 0.33-1; the answer is enlarged back. A change rebuilds the"
      "\nnetwork.")
MZR_B(MochizukiRenoFollowInputRes, false, 0, 1, 7, "Follow render resolution", reno_follow_input_res,
      "The network at the motion vectors' resolution instead of the scale above.")
MZR_U(MochizukiRenoLookUpsample, 0u, 0, 3, 7, "Upsampling", reno_look_upsample,
      "How a reduced answer is enlarged: Auto, Edge-aware, Classic or Area input.")
MZR_B(MochizukiRenoLookMode, false, 0, 1, 8, "Adjust NR's look", reno_look_mode,
      "Turns on the look controls below, which edit the model's change before it is composed. Off by default.")
MZR_F(MochizukiRenoLookStrength, 1.0f, 0.0f, 2.0f, 8, "Edit strength", reno_look_strength,
      "The whole of the model's change, scaled.")
MZR_F(MochizukiRenoLookBrighten, 1.0f, 0.0f, 2.0f, 8, "Brightening", reno_look_brighten,
      "Where the model brightens, how much.")
MZR_F(MochizukiRenoLookDarken, 1.0f, 0.0f, 2.0f, 8, "Darkening", reno_look_darken, "Where the model darkens, how much.")
MZR_F(MochizukiRenoLookColour, 1.0f, 0.0f, 2.0f, 8, "Colour", reno_look_colour, "The model's colour change, scaled.")
MZR_F(MochizukiRenoLookHue, 1.0f, 0.0f, 2.0f, 8, "Hue shift", reno_look_hue, "The model's hue shift, scaled.")
MZR_F(MochizukiRenoLookShadows, 1.0f, 0.0f, 2.0f, 8, "Shadows", reno_look_shadows, "The change in the shadows.")
MZR_F(MochizukiRenoLookMidtones, 1.0f, 0.0f, 2.0f, 8, "Midtones", reno_look_midtones, "The change in the midtones.")
MZR_F(MochizukiRenoLookHighlights, 1.0f, 0.0f, 2.0f, 8, "Highlights", reno_look_highlights,
      "The change in the highlights.")
MZR_F(MochizukiRenoLookMaxBrighten, 0.0f, 0.0f, 4.0f, 8, "Max brightening (stops)", reno_look_max_brighten,
      "The most a pixel may brighten, in stops; 0 is no limit.")
MZR_F(MochizukiRenoLookMaxDarken, 0.0f, 0.0f, 4.0f, 8, "Max darkening (stops)", reno_look_max_darken,
      "The most a pixel may darken, in stops; 0 is no limit.")
MZR_F(MochizukiRenoLookMaxColour, 0.0f, 0.0f, 2.0f, 8, "Max colour change", reno_look_max_colour,
      "The largest colour change; 0 is no limit.")
MZR_F(MochizukiRenoLookTone, 1.0f, 0.0f, 2.0f, 8, "Large-scale tone", reno_look_tone,
      "The broad part of the change (lighting).")
MZR_F(MochizukiRenoLookDetail, 1.0f, 0.0f, 2.0f, 8, "Fine detail", reno_look_detail,
      "The fine part of the change (texture).")
MZR_F(MochizukiRenoLookDetailRadius, 1.0f, 0.25f, 4.0f, 8, "Detail radius (%)", reno_look_detail_radius,
      "Where tone ends and detail begins, in percent of the network's height.")
MZR_F(MochizukiRenoLookHalo, 0.0f, 0.0f, 1.0f, 8, "Halo suppression", reno_look_halo,
      "Holds back halos the change makes around edges.")
MZR_F(MochizukiRenoLookHaloBrightSide, 0.0f, 0.0f, 1.0f, 8, "Halo next to dark objects", reno_look_halo_bright,
      "The same on the bright side of an edge.")
MZR_U(MochizukiRenoLookStabilize, 0u, 0, 2, 8, "Stabilize", reno_look_stabilize,
      "Smooths the change over time: Off, Static, or along the motion vectors.")
MZR_F(MochizukiRenoLookStabilizeMs, 60.0f, 10.0f, 250.0f, 8, "Stabilize time (ms)", reno_look_stabilize_ms,
      "How long the smoothing remembers.")
MZR_B(MochizukiRenoLookStabilizeDetail, false, 0, 1, 8, "Stabilize detail", reno_look_stabilize_detail,
      "Smooths the fine detail too, not only the tone.")
MZR_U(MochizukiRenoSourceEncoding, 0u, 0, 3, 9, "Source encoding", reno_source_encoding,
      "What the game's colour is: from its format, SDR, linear or PQ. A change rebuilds the network.")
MZR_F(MochizukiRenoLinearUnitNits, 0.0f, 0.0f, 10000.0f, 9, "Linear unit (nits)", reno_linear_unit_nits,
      "Above 0, linear colour is absolute: 1.0 is this many nits. A change rebuilds the network.")
MZR_F(MochizukiRenoProxyAnchor, 4.0f, 0.5f, 32.0f, 9, "Proxy anchor (nits)", reno_proxy_anchor_nits,
      "Anchored codec: the level the model's proxy is anchored to.")
MZR_F(MochizukiRenoPaperWhiteScale, 2.5375f, 0.005f, 16.0f, 9, "Paper white scale", reno_paper_white_scale,
      "Scales the white the model sees in HDR.")
MZR_F(MochizukiRenoDiffuseWhiteNits, 203.0f, 80.0f, 1000.0f, 9, "Diffuse white (nits)", reno_diffuse_white_nits,
      "The game's diffuse white in HDR.")
MZR_F(MochizukiRenoPQCalibration, 1.0f, 0.005f, 16.0f, 9, "PQ calibration", reno_pq_calibration,
      "Scales PQ colour before it is encoded for the model.")
MZR_U(MochizukiRenoFeedMode, 0u, 0, 2, 9, "Feed", reno_feed_mode,
      "Where the HDR divisor comes from: Auto, the content meter, or the game's exposure.")
MZR_U(MochizukiRenoNormGovernor, 2u, 0, 2, 9, "Divisor governor", reno_norm_governor,
      "How the divisor may move between frames: Off, Slew or Stable.")
MZR_B(MochizukiRenoNormCutAware, true, 0, 1, 9, "Cut aware", reno_norm_cut_aware,
      "A camera cut moves the divisor at once.")
MZR_F(MochizukiRenoNormAttackStops, 4.0f, 0.1f, 64.0f, 9, "Attack (stops/s)", reno_norm_attack_stops,
      "How fast the divisor follows a brighter scene.")
MZR_F(MochizukiRenoNormReleaseStops, 0.5f, 0.05f, 16.0f, 9, "Release (stops/s)", reno_norm_release_stops,
      "How fast it follows a darker one.")
MZR_F(MochizukiRenoNormSlewStops, 0.02f, 0.0f, 0.5f, 9, "Slew (stops/frame)", reno_norm_slew_stops,
      "The most it moves in one frame.")
MZR_F(MochizukiRenoMVecScaleX, 1.0f, -2.0f, 2.0f, 10, "Motion vector scale X", reno_mvec_scale_x,
      "Scales the game's motion vectors; -1 flips a game whose vectors point the other way.")
MZR_F(MochizukiRenoMVecScaleY, 1.0f, -2.0f, 2.0f, 10, "Motion vector scale Y", reno_mvec_scale_y,
      "Scales the game's motion vectors; -1 flips a game whose vectors point the other way.")
MZR_U(MochizukiRenoPassesMode, 0u, 0, 1, 2, "Passes mode", reno_passes_mode,
      "Legacy: the network once a pass. Fast: the network once a frame, its change strengthened by the fast method"
      "\nbelow (the pass count is then 1).")
MZR_U(MochizukiRenoFastMethod, 0u, 0, 3, 2, "Fast method", reno_fast_method,
      "Passes (fast): Extrapolation, Feedback, Hybrid or Edit strength. Feedback and Hybrid need motion vectors.")
MZR_F(MochizukiRenoFastExtrapolationPasses, 3.0f, 1.0f, 8.0f, 2, "Extrapolation passes", reno_fast_passes_0,
      "Extrapolation's strength, as the legacy pass count it stands for.")
MZR_F(MochizukiRenoFastFeedbackPasses, 3.0f, 1.0f, 8.0f, 2, "Feedback passes", reno_fast_passes_1,
      "Feedback's strength, as the legacy pass count it stands for.")
MZR_F(MochizukiRenoFastHybridPasses, 3.0f, 1.0f, 8.0f, 2, "Hybrid passes", reno_fast_passes_2,
      "Hybrid's strength, as the legacy pass count it stands for.")
MZR_B(MochizukiRenoFastExtrapolationHaloLimit, false, 0, 1, 2, "Extrapolation halo limit", reno_fast_halo_limit_0,
      "Extrapolation keeps the strengthened change inside the local range.")
MZR_B(MochizukiRenoFastFeedbackHaloLimit, false, 0, 1, 2, "Feedback halo limit", reno_fast_halo_limit_1,
      "Feedback keeps the strengthened change inside the local range.")
MZR_B(MochizukiRenoFastHybridHaloLimit, false, 0, 1, 2, "Hybrid halo limit", reno_fast_halo_limit_2,
      "Hybrid keeps the strengthened change inside the local range.")
MZR_F(MochizukiRenoFastExtrapolationFine, 0.0f, 0.0f, 2.0f, 2, "Extrapolation fine detail", reno_fast_fine_0,
      "The fine-detail part of Extrapolation's strengthening (1: as made).")
MZR_F(MochizukiRenoFastFeedbackFine, 1.0f, 0.0f, 2.0f, 2, "Feedback fine detail", reno_fast_fine_1,
      "The fine-detail part of Feedback's strengthening (1: as made).")
MZR_F(MochizukiRenoFastHybridFine, 1.0f, 0.0f, 2.0f, 2, "Hybrid fine detail", reno_fast_fine_2,
      "The fine-detail part of Hybrid's strengthening (1: as made).")
MZR_F(MochizukiRenoFastEditStrength, 2.0f, 0.0f, 4.0f, 2, "Edit strength", reno_fast_edit_strength,
      "Edit strength's multiplier on the network's change, in stops of the look's edit.")
