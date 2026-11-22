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
; shader hash: 458f66c553aec01e19b7e6b445e1382a
;
; Pipeline Runtime Information: 
;
;PSVRuntimeInfo:
; Compute Shader
; NumThreads=(32,8,1)
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
; r_optical_flow_input              texture     u32          2d      T0             t0     1
; rw_optical_flow_scd_histogram         UAV     u32          2d      U0             u0     1
;
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.CBufRet.i32 = type { i32, i32, i32, i32 }
%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }
%"class.Texture2D<unsigned int>" = type { i32, %"class.Texture2D<unsigned int>::mips_type" }
%"class.Texture2D<unsigned int>::mips_type" = type { i32 }
%"class.RWTexture2D<unsigned int>" = type { i32 }
%cbOF = type { <2 x i32>, i32, i32, i32, i32, <2 x float> }

@"\01?scdBuffer@@3PAIA" = external addrspace(3) global [2560 x i32], align 4

define void @CS() {
  %1 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %2 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %3 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %4 = call i32 @dx.op.threadId.i32(i32 93, i32 0)  ; ThreadId(component)
  %5 = call i32 @dx.op.threadId.i32(i32 93, i32 1)  ; ThreadId(component)
  %6 = call i32 @dx.op.threadId.i32(i32 93, i32 2)  ; ThreadId(component)
  %7 = call i32 @dx.op.flattenedThreadIdInGroup.i32(i32 96)  ; FlattenedThreadIdInGroup()
  %8 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %3, i32 0)  ; CBufferLoadLegacy(handle,regIndex)
  %9 = extractvalue %dx.types.CBufRet.i32 %8, 0
  %10 = udiv i32 %9, 3
  %11 = extractvalue %dx.types.CBufRet.i32 %8, 1
  %12 = udiv i32 %11, 3
  %13 = urem i32 %6, 3
  %14 = udiv i32 %6, 3
  %15 = mul i32 %13, %10
  %16 = mul i32 %14, %12
  %17 = add i32 %15, %10
  %18 = add i32 %16, %12
  %19 = mul nsw i32 %7, 10
  %20 = getelementptr [2560 x i32], [2560 x i32] addrspace(3)* @"\01?scdBuffer@@3PAIA", i32 0, i32 %19
  store i32 0, i32 addrspace(3)* %20, align 4, !tbaa !15
  %21 = or i32 %19, 1
  %22 = getelementptr [2560 x i32], [2560 x i32] addrspace(3)* @"\01?scdBuffer@@3PAIA", i32 0, i32 %21
  store i32 0, i32 addrspace(3)* %22, align 4, !tbaa !15
  %23 = add i32 %19, 2
  %24 = getelementptr [2560 x i32], [2560 x i32] addrspace(3)* @"\01?scdBuffer@@3PAIA", i32 0, i32 %23
  store i32 0, i32 addrspace(3)* %24, align 4, !tbaa !15
  %25 = add i32 %19, 3
  %26 = getelementptr [2560 x i32], [2560 x i32] addrspace(3)* @"\01?scdBuffer@@3PAIA", i32 0, i32 %25
  store i32 0, i32 addrspace(3)* %26, align 4, !tbaa !15
  %27 = add i32 %19, 4
  %28 = getelementptr [2560 x i32], [2560 x i32] addrspace(3)* @"\01?scdBuffer@@3PAIA", i32 0, i32 %27
  store i32 0, i32 addrspace(3)* %28, align 4, !tbaa !15
  %29 = add i32 %19, 5
  %30 = getelementptr [2560 x i32], [2560 x i32] addrspace(3)* @"\01?scdBuffer@@3PAIA", i32 0, i32 %29
  store i32 0, i32 addrspace(3)* %30, align 4, !tbaa !15
  %31 = add i32 %19, 6
  %32 = getelementptr [2560 x i32], [2560 x i32] addrspace(3)* @"\01?scdBuffer@@3PAIA", i32 0, i32 %31
  store i32 0, i32 addrspace(3)* %32, align 4, !tbaa !15
  %33 = add i32 %19, 7
  %34 = getelementptr [2560 x i32], [2560 x i32] addrspace(3)* @"\01?scdBuffer@@3PAIA", i32 0, i32 %33
  store i32 0, i32 addrspace(3)* %34, align 4, !tbaa !15
  %35 = add i32 %19, 8
  %36 = getelementptr [2560 x i32], [2560 x i32] addrspace(3)* @"\01?scdBuffer@@3PAIA", i32 0, i32 %35
  store i32 0, i32 addrspace(3)* %36, align 4, !tbaa !15
  %37 = add i32 %19, 9
  %38 = getelementptr [2560 x i32], [2560 x i32] addrspace(3)* @"\01?scdBuffer@@3PAIA", i32 0, i32 %37
  store i32 0, i32 addrspace(3)* %38, align 4, !tbaa !15
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %39 = shl nsw i32 %4, 2
  %40 = add i32 %15, %39
  %41 = add i32 %16, %5
  %42 = icmp ult i32 %40, %17
  %43 = icmp ult i32 %41, %18
  %44 = and i1 %42, %43
  br i1 %44, label %45, label %79

; <label>:45                                      ; preds = %0
  br label %46

; <label>:46                                      ; preds = %46, %45
  %47 = phi i32 [ %76, %46 ], [ %41, %45 ]
  %48 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %2, i32 0, i32 %40, i32 %47, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %49 = extractvalue %dx.types.ResRet.i32 %48, 0
  %50 = add i32 %40, 1
  %51 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %2, i32 0, i32 %50, i32 %47, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %52 = extractvalue %dx.types.ResRet.i32 %51, 0
  %53 = add i32 %40, 2
  %54 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %2, i32 0, i32 %53, i32 %47, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %55 = extractvalue %dx.types.ResRet.i32 %54, 0
  %56 = add i32 %40, 3
  %57 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %2, i32 0, i32 %56, i32 %47, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %58 = extractvalue %dx.types.ResRet.i32 %57, 0
  %59 = mul i32 %49, 10
  %60 = mul i32 %52, 10
  %61 = mul i32 %55, 10
  %62 = mul i32 %58, 10
  %63 = srem i32 %7, 10
  %64 = add i32 %63, %59
  %65 = getelementptr [2560 x i32], [2560 x i32] addrspace(3)* @"\01?scdBuffer@@3PAIA", i32 0, i32 %64
  %66 = atomicrmw add i32 addrspace(3)* %65, i32 1 seq_cst
  %67 = add i32 %63, %60
  %68 = getelementptr [2560 x i32], [2560 x i32] addrspace(3)* @"\01?scdBuffer@@3PAIA", i32 0, i32 %67
  %69 = atomicrmw add i32 addrspace(3)* %68, i32 1 seq_cst
  %70 = add i32 %63, %61
  %71 = getelementptr [2560 x i32], [2560 x i32] addrspace(3)* @"\01?scdBuffer@@3PAIA", i32 0, i32 %70
  %72 = atomicrmw add i32 addrspace(3)* %71, i32 1 seq_cst
  %73 = add i32 %62, %63
  %74 = getelementptr [2560 x i32], [2560 x i32] addrspace(3)* @"\01?scdBuffer@@3PAIA", i32 0, i32 %73
  %75 = atomicrmw add i32 addrspace(3)* %74, i32 1 seq_cst
  %76 = add nsw i32 %47, 128
  %77 = icmp ult i32 %76, %18
  br i1 %77, label %46, label %78

; <label>:78                                      ; preds = %46
  br label %79

; <label>:79                                      ; preds = %78, %0
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %80 = load i32, i32 addrspace(3)* %20, align 4, !tbaa !15
  %81 = load i32, i32 addrspace(3)* %22, align 4, !tbaa !15
  %82 = add i32 %81, %80
  %83 = load i32, i32 addrspace(3)* %24, align 4, !tbaa !15
  %84 = add i32 %82, %83
  %85 = load i32, i32 addrspace(3)* %26, align 4, !tbaa !15
  %86 = add i32 %84, %85
  %87 = load i32, i32 addrspace(3)* %28, align 4, !tbaa !15
  %88 = add i32 %86, %87
  %89 = load i32, i32 addrspace(3)* %30, align 4, !tbaa !15
  %90 = add i32 %88, %89
  %91 = load i32, i32 addrspace(3)* %32, align 4, !tbaa !15
  %92 = add i32 %90, %91
  %93 = load i32, i32 addrspace(3)* %34, align 4, !tbaa !15
  %94 = add i32 %92, %93
  %95 = load i32, i32 addrspace(3)* %36, align 4, !tbaa !15
  %96 = add i32 %94, %95
  %97 = load i32, i32 addrspace(3)* %38, align 4, !tbaa !15
  %98 = add i32 %96, %97
  %99 = shl i32 %6, 8
  %100 = add i32 %99, %7
  %101 = call i32 @dx.op.atomicBinOp.i32(i32 78, %dx.types.Handle %1, i32 0, i32 %100, i32 0, i32 undef, i32 %98)  ; AtomicBinOp(handle,atomicOp,offset0,offset1,offset2,newValue)
  ret void
}

; Function Attrs: nounwind readnone
declare i32 @dx.op.threadId.i32(i32, i32) #0

; Function Attrs: nounwind readnone
declare i32 @dx.op.flattenedThreadIdInGroup.i32(i32) #0

; Function Attrs: noduplicate nounwind
declare void @dx.op.barrier(i32, i32) #1

; Function Attrs: nounwind readonly
declare %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32) #2

; Function Attrs: nounwind
declare i32 @dx.op.atomicBinOp.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32) #3

; Function Attrs: nounwind readonly
declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32) #2

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #2

attributes #0 = { nounwind readnone }
attributes #1 = { noduplicate nounwind }
attributes #2 = { nounwind readonly }
attributes #3 = { nounwind }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!2}
!dx.shaderModel = !{!3}
!dx.resources = !{!4}
!dx.entryPoints = !{!12}

!0 = !{!"dxcoob 1.8.2502.11 (239921522)"}
!1 = !{i32 1, i32 2}
!2 = !{i32 1, i32 8}
!3 = !{!"cs", i32 6, i32 2}
!4 = !{!5, !8, !10, null}
!5 = !{!6}
!6 = !{i32 0, %"class.Texture2D<unsigned int>"* undef, !"", i32 0, i32 0, i32 1, i32 2, i32 0, !7}
!7 = !{i32 0, i32 5}
!8 = !{!9}
!9 = !{i32 0, %"class.RWTexture2D<unsigned int>"* undef, !"", i32 0, i32 0, i32 1, i32 2, i1 false, i1 false, i1 false, !7}
!10 = !{!11}
!11 = !{i32 0, %cbOF* undef, !"", i32 0, i32 0, i32 1, i32 32, null}
!12 = !{void ()* @CS, !"CS", null, !4, !13}
!13 = !{i32 4, !14}
!14 = !{i32 32, i32 8, i32 1}
!15 = !{!16, !16, i64 0}
!16 = !{!"int", !17, i64 0}
!17 = !{!"omnipotent char", !18, i64 0}
!18 = !{!"Simple C/C++ TBAA"}
