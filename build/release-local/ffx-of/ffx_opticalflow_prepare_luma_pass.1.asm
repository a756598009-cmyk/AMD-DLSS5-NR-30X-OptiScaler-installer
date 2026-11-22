;
; Input signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; no parameters
;
; Output signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; no parameters
; shader hash: 7ae91c6892f9e14ffac087b6fc4111de
;
; Pipeline Runtime Information: 
;
;PSVRuntimeInfo:
; Compute Shader
; NumThreads=(16,16,1)
; MinimumExpectedWaveLaneCount: 0
; MaximumExpectedWaveLaneCount: 4294967295
; UsesViewID: false
; SigInputElements: 0
; SigOutputElements: 0
; SigPatchConstOrPrimElements: 0
; SigInputVectors: 0
; SigOutputVectors[0]: 0
; SigOutputVectors[1]: 0
; SigOutputVectors[2]: 0
; SigOutputVectors[3]: 0
; EntryFunctionName: CS
;
;
; Buffer Definitions:
;
; cbuffer cbOF
; {
;
;   struct cbOF
;   {
;
;       int2 iInputLumaResolution;                    ; Offset:    0
;       uint uOpticalFlowPyramidLevel;                ; Offset:    8
;       uint uOpticalFlowPyramidLevelCount;           ; Offset:   12
;       uint iFrameIndex;                             ; Offset:   16
;       uint backbufferTransferFunction;              ; Offset:   20
;       float2 minMaxLuminance;                       ; Offset:   24
;   
;   } cbOF;                                           ; Offset:    0 Size:    32
;
; }
;
;
; Resource Bindings:
;
; Name                                 Type  Format         Dim      ID      HLSL Bind  Count
; ------------------------------ ---------- ------- ----------- ------- -------------- ------
; cbOF                              cbuffer      NA          NA     CB0            cb0     1
; r_input_color                     texture     f32          2d      T0             t0     1
; rw_optical_flow_input                 UAV     u32          2d      U0             u0     1
;
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.ResRet.f32 = type { float, float, float, float, i32 }
%dx.types.CBufRet.i32 = type { i32, i32, i32, i32 }
%dx.types.CBufRet.f32 = type { float, float, float, float }
%"class.Texture2D<vector<float, 4> >" = type { <4 x float>, %"class.Texture2D<vector<float, 4> >::mips_type" }
%"class.Texture2D<vector<float, 4> >::mips_type" = type { i32 }
%"class.RWTexture2D<unsigned int>" = type { i32 }
%cbOF = type { <2 x i32>, i32, i32, i32, i32, <2 x float> }

define void @CS() {
  %1 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %2 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %3 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %4 = call i32 @dx.op.threadId.i32(i32 93, i32 0)  ; ThreadId(component)
  %5 = call i32 @dx.op.threadId.i32(i32 93, i32 1)  ; ThreadId(component)
  br label %6

; <label>:6                                       ; preds = %122, %0
  %7 = phi i32 [ 0, %0 ], [ %123, %122 ]
  br label %8

; <label>:8                                       ; preds = %116, %6
  %9 = phi i32 [ 0, %6 ], [ %120, %116 ]
  %10 = shl i32 %4, 1
  %11 = shl i32 %5, 1
  %12 = add nsw i32 %9, %10
  %13 = add nsw i32 %7, %11
  %14 = call %dx.types.ResRet.f32 @dx.op.textureLoad.f32(i32 66, %dx.types.Handle %2, i32 0, i32 %12, i32 %13, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %15 = extractvalue %dx.types.ResRet.f32 %14, 0
  %16 = extractvalue %dx.types.ResRet.f32 %14, 1
  %17 = extractvalue %dx.types.ResRet.f32 %14, 2
  %18 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %3, i32 1)  ; CBufferLoadLegacy(handle,regIndex)
  %19 = extractvalue %dx.types.CBufRet.i32 %18, 1
  %20 = icmp eq i32 %19, 0
  br i1 %20, label %21, label %27

; <label>:21                                      ; preds = %8
  %22 = fmul fast float %15, 0x3FCB367A00000000
  %23 = fmul fast float %16, 0x3FE6E2EB20000000
  %24 = fadd fast float %22, %23
  %25 = fmul fast float %17, 0x3FB27BB300000000
  %26 = fadd fast float %24, %25
  br label %116

; <label>:27                                      ; preds = %8
  %28 = icmp eq i32 %19, 1
  br i1 %28, label %29, label %84

; <label>:29                                      ; preds = %27
  %30 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %3, i32 1)  ; CBufferLoadLegacy(handle,regIndex)
  %31 = extractvalue %dx.types.CBufRet.f32 %30, 3
  %32 = call float @dx.op.unary.f32(i32 23, float %15)  ; Log(value)
  %33 = call float @dx.op.unary.f32(i32 23, float %16)  ; Log(value)
  %34 = call float @dx.op.unary.f32(i32 23, float %17)  ; Log(value)
  %35 = fmul fast float %32, 0x3F89F9B3C0000000
  %36 = fmul fast float %33, 0x3F89F9B3C0000000
  %37 = fmul fast float %34, 0x3F89F9B3C0000000
  %38 = call float @dx.op.unary.f32(i32 21, float %35)  ; Exp(value)
  %39 = call float @dx.op.unary.f32(i32 21, float %36)  ; Exp(value)
  %40 = call float @dx.op.unary.f32(i32 21, float %37)  ; Exp(value)
  %41 = fadd fast float %38, 0xBFEAC00100000000
  %42 = fadd fast float %39, 0xBFEAC00100000000
  %43 = fadd fast float %40, 0xBFEAC00100000000
  %44 = call float @dx.op.unary.f32(i32 7, float %41)  ; Saturate(value)
  %45 = call float @dx.op.unary.f32(i32 7, float %42)  ; Saturate(value)
  %46 = call float @dx.op.unary.f32(i32 7, float %43)  ; Saturate(value)
  %47 = fmul fast float %38, 1.868750e+01
  %48 = fmul fast float %39, 1.868750e+01
  %49 = fmul fast float %40, 1.868750e+01
  %50 = fsub fast float 0x4032DA0280000000, %47
  %51 = fsub fast float 0x4032DA0280000000, %48
  %52 = fsub fast float 0x4032DA0280000000, %49
  %53 = fdiv fast float %44, %50
  %54 = fdiv fast float %45, %51
  %55 = fdiv fast float %46, %52
  %56 = call float @dx.op.unary.f32(i32 23, float %53)  ; Log(value)
  %57 = call float @dx.op.unary.f32(i32 23, float %54)  ; Log(value)
  %58 = call float @dx.op.unary.f32(i32 23, float %55)  ; Log(value)
  %59 = fmul fast float %56, 0x40191C0C20000000
  %60 = fmul fast float %57, 0x40191C0C20000000
  %61 = fmul fast float %58, 0x40191C0C20000000
  %62 = call float @dx.op.unary.f32(i32 21, float %59)  ; Exp(value)
  %63 = call float @dx.op.unary.f32(i32 21, float %60)  ; Exp(value)
  %64 = call float @dx.op.unary.f32(i32 21, float %61)  ; Exp(value)
  %65 = fdiv fast float 1.000000e+04, %31
  %66 = fmul fast float %62, 0x3FD0D013A0000000
  %67 = fmul fast float %63, 0x3FE5B22D00000000
  %68 = fmul fast float %64, 0x3FAE5C91E0000000
  %69 = fadd fast float %67, %66
  %70 = fadd fast float %69, %68
  %71 = fmul fast float %70, %65
  %72 = fcmp fast ugt float %71, 0x3F822354E0000000
  br i1 %72, label %75, label %73

; <label>:73                                      ; preds = %29
  %74 = fmul fast float %71, 0x408C3A5EE0000000
  br label %81

; <label>:75                                      ; preds = %29
  %76 = call float @dx.op.unary.f32(i32 23, float %71)  ; Log(value)
  %77 = fmul fast float %76, 0x3FD5555560000000
  %78 = call float @dx.op.unary.f32(i32 21, float %77)  ; Exp(value)
  %79 = fmul fast float %78, 1.160000e+02
  %80 = fadd fast float %79, -1.600000e+01
  br label %81

; <label>:81                                      ; preds = %75, %73
  %82 = phi float [ %74, %73 ], [ %80, %75 ]
  %83 = fmul fast float %82, 0x3F847AE140000000
  br label %116

; <label>:84                                      ; preds = %27
  %85 = icmp eq i32 %19, 2
  br i1 %85, label %86, label %116

; <label>:86                                      ; preds = %84
  %87 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %3, i32 1)  ; CBufferLoadLegacy(handle,regIndex)
  %88 = extractvalue %dx.types.CBufRet.f32 %87, 3
  %89 = extractvalue %dx.types.CBufRet.f32 %87, 2
  %90 = fmul fast float %89, 0x3F899999A0000000
  %91 = fsub fast float %15, %90
  %92 = fsub fast float %16, %90
  %93 = fsub fast float %17, %90
  %94 = fsub fast float %88, %89
  %95 = fmul fast float %94, 0x3F899999A0000000
  %96 = fdiv fast float %91, %95
  %97 = fdiv fast float %92, %95
  %98 = fdiv fast float %93, %95
  %99 = fmul fast float %96, 0x3FCB367A00000000
  %100 = fmul fast float %97, 0x3FE6E2EB20000000
  %101 = fadd fast float %100, %99
  %102 = fmul fast float %98, 0x3FB27BB300000000
  %103 = fadd fast float %101, %102
  %104 = fcmp fast ugt float %103, 0x3F822354E0000000
  br i1 %104, label %107, label %105

; <label>:105                                     ; preds = %86
  %106 = fmul fast float %103, 0x408C3A5EE0000000
  br label %113

; <label>:107                                     ; preds = %86
  %108 = call float @dx.op.unary.f32(i32 23, float %103)  ; Log(value)
  %109 = fmul fast float %108, 0x3FD5555560000000
  %110 = call float @dx.op.unary.f32(i32 21, float %109)  ; Exp(value)
  %111 = fmul fast float %110, 1.160000e+02
  %112 = fadd fast float %111, -1.600000e+01
  br label %113

; <label>:113                                     ; preds = %107, %105
  %114 = phi float [ %106, %105 ], [ %112, %107 ]
  %115 = fmul fast float %114, 0x3F847AE140000000
  br label %116

; <label>:116                                     ; preds = %113, %84, %81, %21
  %117 = phi float [ %26, %21 ], [ %83, %81 ], [ %115, %113 ], [ 0.000000e+00, %84 ]
  %118 = fmul fast float %117, 2.550000e+02
  %119 = fptoui float %118 to i32
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %1, i32 %12, i32 %13, i32 undef, i32 %119, i32 %119, i32 %119, i32 %119, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  %120 = add nuw nsw i32 %9, 1
  %121 = icmp eq i32 %120, 2
  br i1 %121, label %122, label %8

; <label>:122                                     ; preds = %116
  %123 = add nuw nsw i32 %7, 1
  %124 = icmp eq i32 %123, 2
  br i1 %124, label %125, label %6

; <label>:125                                     ; preds = %122
  ret void
}

; Function Attrs: nounwind readnone
declare i32 @dx.op.threadId.i32(i32, i32) #0

; Function Attrs: nounwind readonly
declare %dx.types.ResRet.f32 @dx.op.textureLoad.f32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32) #1

; Function Attrs: nounwind readnone
declare float @dx.op.unary.f32(i32, float) #0

; Function Attrs: nounwind
declare void @dx.op.textureStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32, i8) #2

; Function Attrs: nounwind readonly
declare %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32, %dx.types.Handle, i32) #1

; Function Attrs: nounwind readonly
declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32) #1

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #1

attributes #0 = { nounwind readnone }
attributes #1 = { nounwind readonly }
attributes #2 = { nounwind }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!2}
!dx.shaderModel = !{!3}
!dx.resources = !{!4}
!dx.entryPoints = !{!13}

!0 = !{!"dxcoob 1.8.2502.11 (239921522)"}
!1 = !{i32 1, i32 2}
!2 = !{i32 1, i32 8}
!3 = !{!"cs", i32 6, i32 2}
!4 = !{!5, !8, !11, null}
!5 = !{!6}
!6 = !{i32 0, %"class.Texture2D<vector<float, 4> >"* undef, !"", i32 0, i32 0, i32 1, i32 2, i32 0, !7}
!7 = !{i32 0, i32 9}
!8 = !{!9}
!9 = !{i32 0, %"class.RWTexture2D<unsigned int>"* undef, !"", i32 0, i32 0, i32 1, i32 2, i1 false, i1 false, i1 false, !10}
!10 = !{i32 0, i32 5}
!11 = !{!12}
!12 = !{i32 0, %cbOF* undef, !"", i32 0, i32 0, i32 1, i32 32, null}
!13 = !{void ()* @CS, !"CS", null, !4, !14}
!14 = !{i32 4, !15}
!15 = !{i32 16, i32 16, i32 1}
