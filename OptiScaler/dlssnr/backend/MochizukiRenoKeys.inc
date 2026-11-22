// mochizuki's RenoDX stage ([DlssNr] MochizukiReno*, MochizukiNrControls::reno_*): the key, its default and range, its
// menu group and label, the controls member it fills, and its help. Keys, defaults and ranges are
// TheAutomatic/dlss-5-amd-project's, RenoDX's own (nr_renodx.hpp Settings). Included with MZR_F (float), MZR_U (choice
// or count) and MZR_B (switch) defined. Groups: 1 Network, 2 Passes, 3 Pass 2, 4 Pass 3, 5 Pass 4
// and later, 6 Composition, 7 Resolution, 8 Adjust NR's look, 9 Encoding, 10 Guides.
MZR_F(MochizukiRenoIntensity, 1.0f, 0.0f, 2.0f, 1, "强度", reno_intensity,
      "第1遍的模型强度；其他处理遍数若未单独设置，也使用此值。")
MZR_U(MochizukiRenoStyle, 0u, 0, 2, 1, "风格", reno_style, "模型风格：标准、自然或电影风格。")
MZR_F(MochizukiRenoLocalTone, 1.0f, 0.0f, 2.0f, 1, "局部色调", reno_local_tone, "模型对局部光照的调整强度。")
MZR_F(MochizukiRenoLocalStructure, 1.0f, 0.0f, 2.0f, 1, "局部结构", reno_local_structure,
      "模型对局部结构的调整强度。")
MZR_F(MochizukiRenoSkinStructure, 1.0f, 0.0f, 1.0f, 1, "皮肤结构", reno_skin_structure,
      "皮肤区域的结构强度，范围为0到1，1为模型默认值。需要启用自动蒙版。")
MZR_B(MochizukiRenoAutoMask, false, 0, 1, 1, "自动蒙版", reno_auto_mask,
      "使用模型生成人物蒙版，让皮肤区域拥有独立的结构调整。")
MZR_B(MochizukiRenoSkinIndependent, true, 0, 1, 1, "皮肤独立处理", reno_skin_independent,
      "启用蒙版后，结构强度最高按1处理，皮肤强度按1处理，避免结构调整影响面部。")
MZR_U(MochizukiRenoPasses, 1u, 1, 30, 2, "处理遍数", reno_passes,
      "当前阶段的模型处理遍数，每一遍都基于上一遍的结果：默认1～4遍，解锁后可达1～30遍。每增加一遍，都会增加完整的网络"
      "\n计算开销。修改此项会重新构建网络。")
MZR_B(MochizukiRenoUnlockPasses, false, 0, 1, 2, "解锁处理遍数", reno_unlock_passes,
      "将处理遍数上限从4提高到30。第5遍及之后的处理使用第4遍的控制参数。")
MZR_B(MochizukiRenoChainedHistory, true, 0, 1, 2, "连续历史记录", reno_chained_history,
      "第2遍及之后的处理各自保留历史记录。关闭后，每一帧都会重新开始。")
MZR_F(MochizukiRenoPass2Transfer, 1.0f, 0.0f, 1.0f, 3, "亮度传递强度", reno_pass_transfer_0,
      "保留当前处理遍数所产生亮度变化的比例。")
MZR_F(MochizukiRenoPass2Color, 1.0f, 0.0f, 1.0f, 3, "色彩强度", reno_pass_color_0,
      "保留当前处理遍数所产生色彩变化的比例。")
MZR_F(MochizukiRenoPass2Intensity, 1.0f, 0.0f, 2.0f, 3, "强度", reno_pass_intensity_0,
      "当前处理遍数的模型强度。")
MZR_B(MochizukiRenoPass2FollowPass1, true, 0, 1, 3, "与第1遍相同", reno_pass_follow_0,
      "使用第1遍的风格、色调、结构、皮肤和蒙版设置。关闭后使用下方独立参数。")
MZR_U(MochizukiRenoPass2Style, 0u, 0, 2, 3, "风格", reno_pass_style_0, "当前处理遍数使用的风格。")
MZR_F(MochizukiRenoPass2LocalTone, 1.0f, 0.0f, 2.0f, 3, "局部色调", reno_pass_local_tone_0,
      "当前处理遍数的局部光照调整。")
MZR_F(MochizukiRenoPass2Structure, 1.0f, 0.0f, 2.0f, 3, "局部结构", reno_pass_local_structure_0,
      "当前处理遍数的结构调整强度。")
MZR_F(MochizukiRenoPass2Skin, 1.0f, 0.0f, 1.0f, 3, "皮肤结构", reno_pass_skin_structure_0,
      "当前处理遍数的皮肤结构强度，范围为0到1，1为模型默认值。")
MZR_B(MochizukiRenoPass2AutoMask, false, 0, 1, 3, "自动蒙版", reno_pass_auto_mask_0, "当前处理遍数使用的人物蒙版。")
MZR_F(MochizukiRenoPass3Transfer, 1.0f, 0.0f, 1.0f, 4, "亮度传递强度", reno_pass_transfer_1,
      "保留当前处理遍数所产生亮度变化的比例。")
MZR_F(MochizukiRenoPass3Color, 1.0f, 0.0f, 1.0f, 4, "色彩强度", reno_pass_color_1,
      "保留当前处理遍数所产生色彩变化的比例。")
MZR_F(MochizukiRenoPass3Intensity, 1.0f, 0.0f, 2.0f, 4, "强度", reno_pass_intensity_1,
      "当前处理遍数的模型强度。")
MZR_B(MochizukiRenoPass3FollowPass1, true, 0, 1, 4, "与第1遍相同", reno_pass_follow_1,
      "使用第1遍的风格、色调、结构、皮肤和蒙版设置。关闭后使用下方独立参数。")
MZR_U(MochizukiRenoPass3Style, 0u, 0, 2, 4, "风格", reno_pass_style_1, "当前处理遍数使用的风格。")
MZR_F(MochizukiRenoPass3LocalTone, 1.0f, 0.0f, 2.0f, 4, "局部色调", reno_pass_local_tone_1,
      "当前处理遍数的局部光照调整。")
MZR_F(MochizukiRenoPass3Structure, 1.0f, 0.0f, 2.0f, 4, "局部结构", reno_pass_local_structure_1,
      "当前处理遍数的结构调整强度。")
MZR_F(MochizukiRenoPass3Skin, 1.0f, 0.0f, 1.0f, 4, "皮肤结构", reno_pass_skin_structure_1,
      "当前处理遍数的皮肤结构强度，范围为0到1，1为模型默认值。")
MZR_B(MochizukiRenoPass3AutoMask, false, 0, 1, 4, "自动蒙版", reno_pass_auto_mask_1, "当前处理遍数使用的人物蒙版。")
MZR_F(MochizukiRenoPass4Transfer, 1.0f, 0.0f, 1.0f, 5, "亮度传递强度", reno_pass_transfer_2,
      "保留第4遍及之后处理所产生亮度变化的比例。")
MZR_F(MochizukiRenoPass4Color, 1.0f, 0.0f, 1.0f, 5, "色彩强度", reno_pass_color_2,
      "保留第4遍及之后处理所产生色彩变化的比例。")
MZR_F(MochizukiRenoPass4Intensity, 1.0f, 0.0f, 2.0f, 5, "强度", reno_pass_intensity_2,
      "第4遍及之后处理使用的模型强度。")
MZR_B(MochizukiRenoPass4FollowPass1, true, 0, 1, 5, "与第1遍相同", reno_pass_follow_2,
      "使用第1遍的风格、色调、结构、皮肤和蒙版设置。关闭后使用下方独立参数。")
MZR_U(MochizukiRenoPass4Style, 0u, 0, 2, 5, "风格", reno_pass_style_2, "第4遍及之后处理使用的风格。")
MZR_F(MochizukiRenoPass4LocalTone, 1.0f, 0.0f, 2.0f, 5, "局部色调", reno_pass_local_tone_2,
      "第4遍及之后处理的局部光照调整。")
MZR_F(MochizukiRenoPass4Structure, 1.0f, 0.0f, 2.0f, 5, "局部结构", reno_pass_local_structure_2,
      "第4遍及之后处理的结构调整强度。")
MZR_F(MochizukiRenoPass4Skin, 1.0f, 0.0f, 1.0f, 5, "皮肤结构", reno_pass_skin_structure_2,
      "第4遍及之后处理的皮肤结构强度，范围为0到1，1为模型默认值。")
MZR_B(MochizukiRenoPass4AutoMask, false, 0, 1, 5, "自动蒙版", reno_pass_auto_mask_2,
      "第4遍及之后处理使用的人物蒙版。")
MZR_F(MochizukiRenoColorStrength, 1.0f, 0.0f, 1.0f, 6, "色彩强度", reno_color_strength,
      "保留第1遍色彩变化的比例；设为0时，在调整亮度后保留游戏原有色彩。")
MZR_F(MochizukiRenoTransferStrength, 1.0f, 0.0f, 1.0f, 6, "亮度传递强度", reno_transfer_strength,
      "保留第1遍亮度变化的比例。")
MZR_F(MochizukiRenoChromaClamp, 1.0f, 0.25f, 2.0f, 6, "色度限制（档位）", reno_chroma_clamp_stops,
      "允许通过的最大色彩变化幅度，以档位为单位。")
MZR_U(MochizukiRenoTransferMode, 0u, 0, 1, 6, "传递模式", reno_transfer_mode,
      "可选有界比例（RenoDX默认）或一致模式。")
MZR_F(MochizukiRenoStyleHighlightGuard, 0.0f, 0.0f, 2.0f, 6, "高光保护（档位）", reno_style_highlight_guard,
      "限制模型对高光区域的压暗幅度，单位为档位。数值越低，对高光的保护越强（0.25为强保护）；2表示不作限制，"
      "0表示关闭。仅适用于使用Display编码的HDR游戏（Codec设为Auto时通常会自动选择该编码），并且只在"
      "模型压暗亮部区域时生效，例如Natural和Cinematic风格。")
MZR_U(MochizukiRenoDetailStability, 0u, 0, 2, 6, "细节稳定", reno_detail_stability,
      "稳定相邻帧之间的精细细节。自动模式会根据降噪运行位置决定处理方式。")
MZR_U(MochizukiRenoSdrBlackRestore, 0u, 0, 3, 6, "SDR黑电平恢复", reno_sdr_black_restore,
      "SDR：恢复被模型抬高的游戏黑电平。可选自动、每个区块处理、平滑处理或关闭（上游自0.0.8.3起的默认设置）。")
MZR_U(MochizukiRenoDisplayPedestal, 0u, 0, 3, 6, "HDR暗部基准", reno_display_pedestal,
      "HDR：抵消模型增加的暗部抬升。可选自动、始终处理、平滑处理或关闭（上游自0.0.8.3起的默认设置）。")
MZR_U(MochizukiRenoCodecMode, 2u, 0, 3, 6, "编码方式", reno_codec_mode,
      "模型使用的HDR编码方式：Classic、Anchored、Auto（默认）或Display。")
MZR_B(MochizukiRenoNeuralFloorGuard, true, 0, 1, 6, "神经网络黑位保护", reno_neural_floor_guard,
      "防止模型压暗接近黑色区域的细节。")
MZR_F(MochizukiRenoResolutionScale, 1.0f, 0.33f, 1.0f, 7, "分辨率缩放", reno_resolution_scale,
      "网络分辨率占原始画面的比例，范围为0.33～1；处理结果会再放大回原始尺寸。修改此项会重新构建网络"
      "。")
MZR_B(MochizukiRenoFollowInputRes, false, 0, 1, 7, "跟随渲染分辨率", reno_follow_input_res,
      "使网络使用运动矢量的分辨率，而不是上方设置的缩放比例。")
MZR_U(MochizukiRenoLookUpsample, 0u, 0, 3, 7, "上采样", reno_look_upsample,
      "低分辨率结果的放大方式：自动、边缘感知、经典或区域输入。")
MZR_B(MochizukiRenoLookMode, false, 0, 1, 8, "调整降噪效果", reno_look_mode,
      "启用下方画面效果调节项，在模型结果合成前调整模型产生的变化。默认关闭。")
MZR_F(MochizukiRenoLookStrength, 1.0f, 0.0f, 2.0f, 8, "调整强度", reno_look_strength,
      "对模型产生的整体变化进行统一缩放。")
MZR_F(MochizukiRenoLookBrighten, 1.0f, 0.0f, 2.0f, 8, "提亮", reno_look_brighten,
      "控制模型提亮区域的调整幅度。")
MZR_F(MochizukiRenoLookDarken, 1.0f, 0.0f, 2.0f, 8, "压暗", reno_look_darken, "控制模型压暗区域的调整幅度。")
MZR_F(MochizukiRenoLookColour, 1.0f, 0.0f, 2.0f, 8, "色彩", reno_look_colour, "对模型产生的色彩变化进行缩放。")
MZR_F(MochizukiRenoLookHue, 1.0f, 0.0f, 2.0f, 8, "色相偏移", reno_look_hue, "对模型产生的色相偏移进行缩放。")
MZR_F(MochizukiRenoLookShadows, 1.0f, 0.0f, 2.0f, 8, "阴影", reno_look_shadows, "控制阴影区域的变化幅度。")
MZR_F(MochizukiRenoLookMidtones, 1.0f, 0.0f, 2.0f, 8, "中间调", reno_look_midtones, "控制中间调区域的变化幅度。")
MZR_F(MochizukiRenoLookHighlights, 1.0f, 0.0f, 2.0f, 8, "高光", reno_look_highlights,
      "控制高光区域的变化幅度。")
MZR_F(MochizukiRenoLookMaxBrighten, 0.0f, 0.0f, 4.0f, 8, "最大提亮幅度（档位）", reno_look_max_brighten,
      "单个像素允许提亮的最大幅度，以档位为单位；0表示不限制。")
MZR_F(MochizukiRenoLookMaxDarken, 0.0f, 0.0f, 4.0f, 8, "最大压暗幅度（档位）", reno_look_max_darken,
      "单个像素允许压暗的最大幅度，以档位为单位；0表示不限制。")
MZR_F(MochizukiRenoLookMaxColour, 0.0f, 0.0f, 2.0f, 8, "最大色彩变化", reno_look_max_colour,
      "允许的最大色彩变化幅度；0表示不限制。")
MZR_F(MochizukiRenoLookTone, 1.0f, 0.0f, 2.0f, 8, "大尺度色调", reno_look_tone,
      "控制整体大范围变化（光照）的部分。")
MZR_F(MochizukiRenoLookDetail, 1.0f, 0.0f, 2.0f, 8, "精细细节", reno_look_detail,
      "控制精细变化（纹理）的部分。")
MZR_F(MochizukiRenoLookDetailRadius, 1.0f, 0.25f, 4.0f, 8, "细节半径（%）", reno_look_detail_radius,
      "色调处理与细节处理的分界半径，以网络高度的百分比表示。")
MZR_F(MochizukiRenoLookHalo, 0.0f, 0.0f, 1.0f, 8, "光晕抑制", reno_look_halo,
      "抑制模型调整在边缘周围产生的光晕。")
MZR_F(MochizukiRenoLookHaloBrightSide, 0.0f, 0.0f, 1.0f, 8, "暗部物体边缘光晕抑制", reno_look_halo_bright,
      "对边缘的亮侧进行相同的光晕抑制。")
MZR_U(MochizukiRenoLookStabilize, 0u, 0, 2, 8, "稳定", reno_look_stabilize,
      "随时间平滑模型产生的变化：关闭、静态平滑或沿运动矢量平滑。")
MZR_F(MochizukiRenoLookStabilizeMs, 60.0f, 10.0f, 250.0f, 8, "稳定时间（毫秒）", reno_look_stabilize_ms,
      "平滑处理保留历史信息的时间长度。")
MZR_B(MochizukiRenoLookStabilizeDetail, false, 0, 1, 8, "稳定细节", reno_look_stabilize_detail,
      "同时平滑精细细节，而不仅仅处理色调。")
MZR_U(MochizukiRenoSourceEncoding, 0u, 0, 3, 9, "源颜色编码", reno_source_encoding,
      "指定游戏颜色的来源格式：跟随原始格式、SDR、线性或PQ。修改此项会重新构建网络。")
MZR_F(MochizukiRenoLinearUnitNits, 0.0f, 0.0f, 10000.0f, 9, "线性单位（尼特）", reno_linear_unit_nits,
      "大于0时，线性颜色采用绝对亮度：1.0对应此处设置的尼特值。修改此项会重新构建网络。")
MZR_F(MochizukiRenoProxyAnchor, 4.0f, 0.5f, 32.0f, 9, "代理锚点（尼特）", reno_proxy_anchor_nits,
      "Anchored编码模式下，模型代理信号所锚定的亮度级别。")
MZR_F(MochizukiRenoPaperWhiteScale, 2.5375f, 0.005f, 16.0f, 9, "纸白缩放", reno_paper_white_scale,
      "缩放模型在HDR中看到的白色亮度。")
MZR_F(MochizukiRenoDiffuseWhiteNits, 203.0f, 80.0f, 1000.0f, 9, "漫反射白（尼特）", reno_diffuse_white_nits,
      "设置游戏HDR中的漫反射白亮度。")
MZR_F(MochizukiRenoPQCalibration, 1.0f, 0.005f, 16.0f, 9, "PQ校准", reno_pq_calibration,
      "在将PQ颜色编码供模型使用前，对其进行缩放。")
MZR_U(MochizukiRenoFeedMode, 0u, 0, 2, 9, "输入来源", reno_feed_mode,
      "HDR除数的来源：自动、内容亮度测量值或游戏曝光值。")
MZR_U(MochizukiRenoNormGovernor, 2u, 0, 2, 9, "除数控制方式", reno_norm_governor,
      "控制除数在帧与帧之间的变化方式：关闭、渐进变化或稳定。")
MZR_B(MochizukiRenoNormCutAware, true, 0, 1, 9, "场景切换感知", reno_norm_cut_aware,
      "检测到镜头切换时，立即调整除数。")
MZR_F(MochizukiRenoNormAttackStops, 4.0f, 0.1f, 64.0f, 9, "上升速度（档位/秒）", reno_norm_attack_stops,
      "除数跟随场景变亮的速度。")
MZR_F(MochizukiRenoNormReleaseStops, 0.5f, 0.05f, 16.0f, 9, "下降速度（档位/秒）", reno_norm_release_stops,
      "除数跟随场景变暗的速度。")
MZR_F(MochizukiRenoNormSlewStops, 0.02f, 0.0f, 0.5f, 9, "单帧变化限制（档位/帧）", reno_norm_slew_stops,
      "限制除数在单帧内允许变化的最大幅度。")
MZR_F(MochizukiRenoMVecScaleX, 1.0f, -2.0f, 2.0f, 10, "运动矢量缩放 X", reno_mvec_scale_x,
      "缩放游戏的运动矢量；如果游戏的矢量方向相反，设为-1可将其反转。")
MZR_F(MochizukiRenoMVecScaleY, 1.0f, -2.0f, 2.0f, 10, "运动矢量缩放 Y", reno_mvec_scale_y,
      "缩放游戏的运动矢量；如果游戏的矢量方向相反，设为-1可将其反转。")
MZR_U(MochizukiRenoPassesMode, 0u, 0, 1, 2, "处理遍数模式", reno_passes_mode,
      "传统模式：每一遍处理都运行一次网络。快速模式：每帧只运行一次网络，再通过下方快速算法增强其变化"
      "\n（此时处理遍数固定为1）。")
MZR_U(MochizukiRenoFastMethod, 0u, 0, 3, 2, "快速模式算法", reno_fast_method,
      "快速模式算法：外推、反馈、混合或编辑强度。反馈和混合模式需要运动矢量。")
MZR_F(MochizukiRenoFastExtrapolationPasses, 3.0f, 1.0f, 8.0f, 2, "外推遍数", reno_fast_passes_0,
      "外推模式的强度，以其对应的传统处理遍数表示。")
MZR_F(MochizukiRenoFastFeedbackPasses, 3.0f, 1.0f, 8.0f, 2, "反馈遍数", reno_fast_passes_1,
      "反馈模式的强度，以其对应的传统处理遍数表示。")
MZR_F(MochizukiRenoFastHybridPasses, 3.0f, 1.0f, 8.0f, 2, "混合遍数", reno_fast_passes_2,
      "混合模式的强度，以其对应的传统处理遍数表示。")
MZR_B(MochizukiRenoFastExtrapolationHaloLimit, false, 0, 1, 2, "外推光晕限制", reno_fast_halo_limit_0,
      "外推模式会将增强后的变化限制在局部范围内。")
MZR_B(MochizukiRenoFastFeedbackHaloLimit, false, 0, 1, 2, "反馈光晕限制", reno_fast_halo_limit_1,
      "反馈模式会将增强后的变化限制在局部范围内。")
MZR_B(MochizukiRenoFastHybridHaloLimit, false, 0, 1, 2, "混合光晕限制", reno_fast_halo_limit_2,
      "混合模式会将增强后的变化限制在局部范围内。")
MZR_F(MochizukiRenoFastExtrapolationFine, 0.0f, 0.0f, 2.0f, 2, "外推精细细节", reno_fast_fine_0,
      "外推模式增强效果中的精细细节部分（1：保持原样）。")
MZR_F(MochizukiRenoFastFeedbackFine, 1.0f, 0.0f, 2.0f, 2, "反馈精细细节", reno_fast_fine_1,
      "反馈模式增强效果中的精细细节部分（1：保持原样）。")
MZR_F(MochizukiRenoFastHybridFine, 1.0f, 0.0f, 2.0f, 2, "混合精细细节", reno_fast_fine_2,
      "混合模式增强效果中的精细细节部分（1：保持原样）。")
MZR_F(MochizukiRenoFastEditStrength, 2.0f, 0.0f, 4.0f, 2, "调整强度", reno_fast_edit_strength,
      "编辑强度对网络变化量的倍率，以画面效果编辑档位表示。")
