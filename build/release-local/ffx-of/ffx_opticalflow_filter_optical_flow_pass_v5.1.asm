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
; shader hash: 8625718a9de121f8f76f5cecc331ba7e
;
; Pipeline Runtime Information: 
;
;PSVRuntimeInfo:
; Compute Shader
; NumThreads=(16,4,1)
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
;
; Resource Bindings:
;
; Name                                 Type  Format         Dim      ID      HLSL Bind  Count
; ------------------------------ ---------- ------- ----------- ------- -------------- ------
; r_optical_flow_previous           texture     i32          2d      T0             t0     1
; rw_optical_flow                       UAV     i32          2d      U0             u0     1
;
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }
%"class.Texture2D<vector<int, 2> >" = type { <2 x i32>, %"class.Texture2D<vector<int, 2> >::mips_type" }
%"class.Texture2D<vector<int, 2> >::mips_type" = type { i32 }
%"class.RWTexture2D<vector<int, 2> >" = type { <2 x i32> }

define void @CS() {
  %1 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %2 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %3 = call i32 @dx.op.threadId.i32(i32 93, i32 0)  ; ThreadId(component)
  %4 = call i32 @dx.op.threadId.i32(i32 93, i32 1)  ; ThreadId(component)
  %5 = alloca [9 x i32], align 4
  %6 = alloca [9 x i32], align 4
  br label %7

; <label>:7                                       ; preds = %7, %0
  %8 = phi i32 [ -1, %0 ], [ %31, %7 ]
  %9 = phi i32 [ 0, %0 ], [ %30, %7 ]
  %10 = add i32 %8, %3
  %11 = add i32 %4, -1
  %12 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %2, i32 0, i32 %10, i32 %11, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %13 = extractvalue %dx.types.ResRet.i32 %12, 0
  %14 = extractvalue %dx.types.ResRet.i32 %12, 1
  %15 = getelementptr [9 x i32], [9 x i32]* %5, i32 0, i32 %9
  %16 = getelementptr [9 x i32], [9 x i32]* %6, i32 0, i32 %9
  store i32 %13, i32* %15, align 4
  store i32 %14, i32* %16, align 4
  %17 = add nuw nsw i32 %9, 1
  %18 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %2, i32 0, i32 %10, i32 %4, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %19 = extractvalue %dx.types.ResRet.i32 %18, 0
  %20 = extractvalue %dx.types.ResRet.i32 %18, 1
  %21 = getelementptr [9 x i32], [9 x i32]* %5, i32 0, i32 %17
  %22 = getelementptr [9 x i32], [9 x i32]* %6, i32 0, i32 %17
  store i32 %19, i32* %21, align 4
  store i32 %20, i32* %22, align 4
  %23 = add nuw nsw i32 %9, 2
  %24 = add i32 %4, 1
  %25 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %2, i32 0, i32 %10, i32 %24, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %26 = extractvalue %dx.types.ResRet.i32 %25, 0
  %27 = extractvalue %dx.types.ResRet.i32 %25, 1
  %28 = getelementptr [9 x i32], [9 x i32]* %5, i32 0, i32 %23
  %29 = getelementptr [9 x i32], [9 x i32]* %6, i32 0, i32 %23
  store i32 %26, i32* %28, align 4
  store i32 %27, i32* %29, align 4
  %30 = add nuw nsw i32 %9, 3
  %31 = add nsw i32 %8, 1
  %32 = icmp eq i32 %31, 2
  br i1 %32, label %33, label %7

; <label>:33                                      ; preds = %7
  %34 = getelementptr [9 x i32], [9 x i32]* %5, i32 0, i32 0
  %35 = load i32, i32* %34, align 4
  %36 = getelementptr [9 x i32], [9 x i32]* %6, i32 0, i32 0
  %37 = load i32, i32* %36, align 4
  %38 = getelementptr [9 x i32], [9 x i32]* %5, i32 0, i32 1
  %39 = load i32, i32* %38, align 4
  %40 = getelementptr [9 x i32], [9 x i32]* %6, i32 0, i32 1
  %41 = load i32, i32* %40, align 4
  %42 = getelementptr [9 x i32], [9 x i32]* %5, i32 0, i32 2
  %43 = load i32, i32* %42, align 4
  %44 = getelementptr [9 x i32], [9 x i32]* %6, i32 0, i32 2
  %45 = load i32, i32* %44, align 4
  %46 = getelementptr [9 x i32], [9 x i32]* %5, i32 0, i32 3
  %47 = load i32, i32* %46, align 4
  %48 = getelementptr [9 x i32], [9 x i32]* %6, i32 0, i32 3
  %49 = load i32, i32* %48, align 4
  %50 = getelementptr [9 x i32], [9 x i32]* %5, i32 0, i32 4
  %51 = load i32, i32* %50, align 4
  %52 = getelementptr [9 x i32], [9 x i32]* %6, i32 0, i32 4
  %53 = load i32, i32* %52, align 4
  %54 = getelementptr [9 x i32], [9 x i32]* %5, i32 0, i32 5
  %55 = load i32, i32* %54, align 4
  %56 = getelementptr [9 x i32], [9 x i32]* %6, i32 0, i32 5
  %57 = load i32, i32* %56, align 4
  %58 = getelementptr [9 x i32], [9 x i32]* %5, i32 0, i32 6
  %59 = load i32, i32* %58, align 4
  %60 = getelementptr [9 x i32], [9 x i32]* %6, i32 0, i32 6
  %61 = load i32, i32* %60, align 4
  %62 = getelementptr [9 x i32], [9 x i32]* %5, i32 0, i32 7
  %63 = load i32, i32* %62, align 4
  %64 = getelementptr [9 x i32], [9 x i32]* %6, i32 0, i32 7
  %65 = load i32, i32* %64, align 4
  %66 = getelementptr [9 x i32], [9 x i32]* %5, i32 0, i32 8
  %67 = load i32, i32* %66, align 4
  %68 = getelementptr [9 x i32], [9 x i32]* %6, i32 0, i32 8
  %69 = load i32, i32* %68, align 4
  br label %70

; <label>:70                                      ; preds = %133, %33
  %71 = phi i32 [ %137, %133 ], [ %37, %33 ]
  %72 = phi i32 [ %135, %133 ], [ %35, %33 ]
  %73 = phi i32 [ %130, %133 ], [ -1, %33 ]
  %74 = phi i32 [ %131, %133 ], [ 0, %33 ]
  %75 = sub i32 %72, %35
  %76 = sub i32 %71, %37
  %77 = mul nsw i32 %75, %75
  %78 = mul nsw i32 %76, %76
  %79 = sub i32 %72, %39
  %80 = sub i32 %71, %41
  %81 = mul nsw i32 %79, %79
  %82 = mul nsw i32 %80, %80
  %83 = sub i32 %72, %43
  %84 = sub i32 %71, %45
  %85 = mul nsw i32 %83, %83
  %86 = mul nsw i32 %84, %84
  %87 = sub i32 %72, %47
  %88 = sub i32 %71, %49
  %89 = mul nsw i32 %87, %87
  %90 = mul nsw i32 %88, %88
  %91 = sub i32 %72, %51
  %92 = sub i32 %71, %53
  %93 = mul nsw i32 %91, %91
  %94 = mul nsw i32 %92, %92
  %95 = sub i32 %72, %55
  %96 = sub i32 %71, %57
  %97 = mul nsw i32 %95, %95
  %98 = mul nsw i32 %96, %96
  %99 = sub i32 %72, %59
  %100 = sub i32 %71, %61
  %101 = mul nsw i32 %99, %99
  %102 = mul nsw i32 %100, %100
  %103 = sub i32 %72, %63
  %104 = sub i32 %71, %65
  %105 = mul nsw i32 %103, %103
  %106 = mul nsw i32 %104, %104
  %107 = sub i32 %72, %67
  %108 = sub i32 %71, %69
  %109 = mul nsw i32 %107, %107
  %110 = mul nsw i32 %108, %108
  %111 = add nuw i32 %82, %78
  %112 = add i32 %111, %86
  %113 = add i32 %112, %90
  %114 = add i32 %113, %94
  %115 = add i32 %114, %98
  %116 = add i32 %115, %102
  %117 = add i32 %116, %106
  %118 = add i32 %117, %110
  %119 = add i32 %118, %77
  %120 = add i32 %119, %81
  %121 = add i32 %120, %85
  %122 = add i32 %121, %89
  %123 = add i32 %122, %93
  %124 = add i32 %123, %97
  %125 = add i32 %124, %101
  %126 = add i32 %125, %105
  %127 = add i32 %126, %109
  %128 = shl i32 %127, 4
  %129 = or i32 %128, %74
  %130 = call i32 @dx.op.binary.i32(i32 40, i32 %129, i32 %73)  ; UMin(a,b)
  %131 = add nuw nsw i32 %74, 1
  %132 = icmp eq i32 %131, 9
  br i1 %132, label %138, label %133

; <label>:133                                     ; preds = %70
  %134 = getelementptr [9 x i32], [9 x i32]* %5, i32 0, i32 %131
  %135 = load i32, i32* %134, align 4
  %136 = getelementptr [9 x i32], [9 x i32]* %6, i32 0, i32 %131
  %137 = load i32, i32* %136, align 4
  br label %70

; <label>:138                                     ; preds = %70
  %139 = and i32 %130, 15
  %140 = getelementptr [9 x i32], [9 x i32]* %5, i32 0, i32 %139
  %141 = getelementptr [9 x i32], [9 x i32]* %6, i32 0, i32 %139
  %142 = load i32, i32* %140, align 4
  %143 = load i32, i32* %141, align 4
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %1, i32 %3, i32 %4, i32 undef, i32 %142, i32 %143, i32 %142, i32 %142, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  ret void
}

; Function Attrs: nounwind readnone
declare i32 @dx.op.threadId.i32(i32, i32) #0

; Function Attrs: nounwind readnone
declare i32 @dx.op.binary.i32(i32, i32, i32) #0

; Function Attrs: nounwind readonly
declare %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32) #1

; Function Attrs: nounwind
declare void @dx.op.textureStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32, i8) #2

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
!dx.entryPoints = !{!10}

!0 = !{!"dxcoob 1.8.2502.11 (239921522)"}
!1 = !{i32 1, i32 2}
!2 = !{i32 1, i32 8}
!3 = !{!"cs", i32 6, i32 2}
!4 = !{!5, !8, null, null}
!5 = !{!6}
!6 = !{i32 0, %"class.Texture2D<vector<int, 2> >"* undef, !"", i32 0, i32 0, i32 1, i32 2, i32 0, !7}
!7 = !{i32 0, i32 4}
!8 = !{!9}
!9 = !{i32 0, %"class.RWTexture2D<vector<int, 2> >"* undef, !"", i32 0, i32 0, i32 1, i32 2, i1 false, i1 false, i1 false, !7}
!10 = !{void ()* @CS, !"CS", null, !4, !11}
!11 = !{i32 4, !12}
!12 = !{i32 16, i32 4, i32 1}
