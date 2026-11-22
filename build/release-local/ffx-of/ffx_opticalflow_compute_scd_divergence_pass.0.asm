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
; shader hash: 0730cda7836e8ceb566a25fec957eb42
;
; Pipeline Runtime Information: 
;
;PSVRuntimeInfo:
; Compute Shader
; NumThreads=(256,1,1)
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
; rw_optical_flow_scd_histogram         UAV     u32          2d      U0             u0     1
; rw_optical_flow_scd_previous_histogram       UAV     f32          2d      U1             u1     1
; rw_optical_flow_scd_temp              UAV     u32          2d      U2             u2     1
; rw_optical_flow_scd_output            UAV     u32          2d      U3             u3     1
;
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }
%dx.types.ResRet.f32 = type { float, float, float, float, i32 }
%"class.RWTexture2D<unsigned int>" = type { i32 }
%"class.RWTexture2D<float>" = type { float }

@"\01?sourceHistogram@@3PAMA" = external addrspace(3) global [256 x float], align 4
@"\01?filteredHistogram@@3PAMA" = external addrspace(3) global [256 x float], align 4
@"\01?tempBuffer@@3PAMA" = external addrspace(3) global [256 x float], align 4
@"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim" = addrspace(3) global [512 x float] undef, align 4

define void @CS() {
  %1 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 3, i32 3, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %2 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 2, i32 2, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %3 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 1, i32 1, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %4 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %5 = call i32 @dx.op.threadId.i32(i32 93, i32 0)  ; ThreadId(component)
  %6 = call i32 @dx.op.threadId.i32(i32 93, i32 1)  ; ThreadId(component)
  %7 = call i32 @dx.op.flattenedThreadIdInGroup.i32(i32 96)  ; FlattenedThreadIdInGroup()
  %8 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %4, i32 undef, i32 %5, i32 0, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %9 = extractvalue %dx.types.ResRet.i32 %8, 0
  %10 = uitofp i32 %9 to float
  %11 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?sourceHistogram@@3PAMA", i32 0, i32 %7
  store float %10, float addrspace(3)* %11, align 4, !tbaa !15
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %12 = add nsw i32 %7, -5
  %13 = call i32 @dx.op.binary.i32(i32 38, i32 255, i32 %12)  ; IMin(a,b)
  %14 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %13)  ; IMax(a,b)
  %15 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?sourceHistogram@@3PAMA", i32 0, i32 %14
  %16 = load float, float addrspace(3)* %15, align 4, !tbaa !15
  %17 = add nsw i32 %7, -4
  %18 = call i32 @dx.op.binary.i32(i32 38, i32 255, i32 %17)  ; IMin(a,b)
  %19 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %18)  ; IMax(a,b)
  %20 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?sourceHistogram@@3PAMA", i32 0, i32 %19
  %21 = load float, float addrspace(3)* %20, align 4, !tbaa !15
  %22 = add nsw i32 %7, -3
  %23 = call i32 @dx.op.binary.i32(i32 38, i32 255, i32 %22)  ; IMin(a,b)
  %24 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %23)  ; IMax(a,b)
  %25 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?sourceHistogram@@3PAMA", i32 0, i32 %24
  %26 = load float, float addrspace(3)* %25, align 4, !tbaa !15
  %27 = add nsw i32 %7, -2
  %28 = call i32 @dx.op.binary.i32(i32 38, i32 255, i32 %27)  ; IMin(a,b)
  %29 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %28)  ; IMax(a,b)
  %30 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?sourceHistogram@@3PAMA", i32 0, i32 %29
  %31 = load float, float addrspace(3)* %30, align 4, !tbaa !15
  %32 = add nsw i32 %7, -1
  %33 = call i32 @dx.op.binary.i32(i32 38, i32 255, i32 %32)  ; IMin(a,b)
  %34 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %33)  ; IMax(a,b)
  %35 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?sourceHistogram@@3PAMA", i32 0, i32 %34
  %36 = load float, float addrspace(3)* %35, align 4, !tbaa !15
  %37 = call i32 @dx.op.binary.i32(i32 38, i32 255, i32 %7)  ; IMin(a,b)
  %38 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %37)  ; IMax(a,b)
  %39 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?sourceHistogram@@3PAMA", i32 0, i32 %38
  %40 = load float, float addrspace(3)* %39, align 4, !tbaa !15
  %41 = fmul fast float %40, 0x3FC9AC20A0000000
  %42 = add nsw i32 %7, 1
  %43 = call i32 @dx.op.binary.i32(i32 38, i32 255, i32 %42)  ; IMin(a,b)
  %44 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %43)  ; IMax(a,b)
  %45 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?sourceHistogram@@3PAMA", i32 0, i32 %44
  %46 = load float, float addrspace(3)* %45, align 4, !tbaa !15
  %47 = add nsw i32 %7, 2
  %48 = call i32 @dx.op.binary.i32(i32 38, i32 255, i32 %47)  ; IMin(a,b)
  %49 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %48)  ; IMax(a,b)
  %50 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?sourceHistogram@@3PAMA", i32 0, i32 %49
  %51 = load float, float addrspace(3)* %50, align 4, !tbaa !15
  %52 = add nsw i32 %7, 3
  %53 = call i32 @dx.op.binary.i32(i32 38, i32 255, i32 %52)  ; IMin(a,b)
  %54 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %53)  ; IMax(a,b)
  %55 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?sourceHistogram@@3PAMA", i32 0, i32 %54
  %56 = load float, float addrspace(3)* %55, align 4, !tbaa !15
  %57 = add nsw i32 %7, 4
  %58 = call i32 @dx.op.binary.i32(i32 38, i32 255, i32 %57)  ; IMin(a,b)
  %59 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %58)  ; IMax(a,b)
  %60 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?sourceHistogram@@3PAMA", i32 0, i32 %59
  %61 = load float, float addrspace(3)* %60, align 4, !tbaa !15
  %62 = add nsw i32 %7, 5
  %63 = call i32 @dx.op.binary.i32(i32 38, i32 255, i32 %62)  ; IMin(a,b)
  %64 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %63)  ; IMax(a,b)
  %65 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?sourceHistogram@@3PAMA", i32 0, i32 %64
  %66 = load float, float addrspace(3)* %65, align 4, !tbaa !15
  %67 = fadd fast float %46, %36
  %68 = fmul fast float %67, 0x3FC6A7E1C0000000
  %69 = fadd fast float %51, %31
  %70 = fmul fast float %69, 0x3FBF2464C0000000
  %71 = fadd fast float %56, %26
  %72 = fmul fast float %71, 0x3FB0AB50A0000000
  %73 = fadd fast float %61, %21
  %74 = fmul fast float %73, 0x3F9BCB86A0000000
  %75 = fadd fast float %66, %16
  %76 = fmul fast float %75, 0x3F820C2560000000
  %77 = fadd fast float %41, 1.000000e+00
  %78 = fadd fast float %77, %68
  %79 = fadd fast float %78, %70
  %80 = fadd fast float %79, %72
  %81 = fadd fast float %80, %74
  %82 = fadd fast float %81, %76
  %83 = icmp eq i32 %6, 0
  br i1 %83, label %84, label %89

; <label>:84                                      ; preds = %0
  %85 = icmp eq i32 %7, 0
  br i1 %85, label %86, label %87

; <label>:86                                      ; preds = %84
  store float 1.000000e+00, float addrspace(3)* getelementptr inbounds ([256 x float], [256 x float] addrspace(3)* @"\01?filteredHistogram@@3PAMA", i32 0, i32 255), align 4, !tbaa !15
  br label %100

; <label>:87                                      ; preds = %84
  %88 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?filteredHistogram@@3PAMA", i32 0, i32 %32
  store float %82, float addrspace(3)* %88, align 4, !tbaa !15
  br label %100

; <label>:89                                      ; preds = %0
  %90 = icmp eq i32 %6, 1
  br i1 %90, label %91, label %93

; <label>:91                                      ; preds = %89
  %92 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?filteredHistogram@@3PAMA", i32 0, i32 %7
  store float %82, float addrspace(3)* %92, align 4, !tbaa !15
  br label %100

; <label>:93                                      ; preds = %89
  %94 = icmp eq i32 %6, 2
  br i1 %94, label %95, label %100

; <label>:95                                      ; preds = %93
  %96 = icmp eq i32 %7, 255
  br i1 %96, label %97, label %98

; <label>:97                                      ; preds = %95
  store float 1.000000e+00, float addrspace(3)* getelementptr inbounds ([256 x float], [256 x float] addrspace(3)* @"\01?filteredHistogram@@3PAMA", i32 0, i32 0), align 4, !tbaa !15
  br label %100

; <label>:98                                      ; preds = %95
  %99 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?filteredHistogram@@3PAMA", i32 0, i32 %42
  store float %82, float addrspace(3)* %99, align 4, !tbaa !15
  br label %100

; <label>:100                                     ; preds = %98, %97, %93, %91, %87, %86
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %101 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?filteredHistogram@@3PAMA", i32 0, i32 %7
  %102 = load float, float addrspace(3)* %101, align 4, !tbaa !15
  %103 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?tempBuffer@@3PAMA", i32 0, i32 %7
  store float %102, float addrspace(3)* %103, align 4, !tbaa !15
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %104 = icmp slt i32 %7, 128
  br i1 %104, label %105, label %111

; <label>:105                                     ; preds = %100
  %106 = add nsw i32 %7, 128
  %107 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?tempBuffer@@3PAMA", i32 0, i32 %106
  %108 = load float, float addrspace(3)* %107, align 4, !tbaa !15
  %109 = load float, float addrspace(3)* %103, align 4, !tbaa !15
  %110 = fadd fast float %109, %108
  store float %110, float addrspace(3)* %103, align 4, !tbaa !15
  br label %111

; <label>:111                                     ; preds = %105, %100
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %112 = icmp slt i32 %7, 64
  br i1 %112, label %113, label %119

; <label>:113                                     ; preds = %111
  %114 = add nsw i32 %7, 64
  %115 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?tempBuffer@@3PAMA", i32 0, i32 %114
  %116 = load float, float addrspace(3)* %115, align 4, !tbaa !15
  %117 = load float, float addrspace(3)* %103, align 4, !tbaa !15
  %118 = fadd fast float %117, %116
  store float %118, float addrspace(3)* %103, align 4, !tbaa !15
  br label %119

; <label>:119                                     ; preds = %113, %111
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %120 = icmp slt i32 %7, 32
  br i1 %120, label %121, label %127

; <label>:121                                     ; preds = %119
  %122 = add nsw i32 %7, 32
  %123 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?tempBuffer@@3PAMA", i32 0, i32 %122
  %124 = load float, float addrspace(3)* %123, align 4, !tbaa !15
  %125 = load float, float addrspace(3)* %103, align 4, !tbaa !15
  %126 = fadd fast float %125, %124
  store float %126, float addrspace(3)* %103, align 4, !tbaa !15
  br label %127

; <label>:127                                     ; preds = %121, %119
  %128 = icmp slt i32 %7, 16
  br i1 %128, label %129, label %135

; <label>:129                                     ; preds = %127
  %130 = add nsw i32 %7, 16
  %131 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?tempBuffer@@3PAMA", i32 0, i32 %130
  %132 = load float, float addrspace(3)* %131, align 4, !tbaa !15
  %133 = load float, float addrspace(3)* %103, align 4, !tbaa !15
  %134 = fadd fast float %133, %132
  store float %134, float addrspace(3)* %103, align 4, !tbaa !15
  br label %135

; <label>:135                                     ; preds = %129, %127
  %136 = icmp slt i32 %7, 8
  br i1 %136, label %137, label %143

; <label>:137                                     ; preds = %135
  %138 = add nsw i32 %7, 8
  %139 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?tempBuffer@@3PAMA", i32 0, i32 %138
  %140 = load float, float addrspace(3)* %139, align 4, !tbaa !15
  %141 = load float, float addrspace(3)* %103, align 4, !tbaa !15
  %142 = fadd fast float %141, %140
  store float %142, float addrspace(3)* %103, align 4, !tbaa !15
  br label %143

; <label>:143                                     ; preds = %137, %135
  %144 = icmp slt i32 %7, 4
  br i1 %144, label %145, label %150

; <label>:145                                     ; preds = %143
  %146 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?tempBuffer@@3PAMA", i32 0, i32 %57
  %147 = load float, float addrspace(3)* %146, align 4, !tbaa !15
  %148 = load float, float addrspace(3)* %103, align 4, !tbaa !15
  %149 = fadd fast float %148, %147
  store float %149, float addrspace(3)* %103, align 4, !tbaa !15
  br label %150

; <label>:150                                     ; preds = %145, %143
  %151 = icmp slt i32 %7, 2
  br i1 %151, label %152, label %157

; <label>:152                                     ; preds = %150
  %153 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?tempBuffer@@3PAMA", i32 0, i32 %47
  %154 = load float, float addrspace(3)* %153, align 4, !tbaa !15
  %155 = load float, float addrspace(3)* %103, align 4, !tbaa !15
  %156 = fadd fast float %155, %154
  store float %156, float addrspace(3)* %103, align 4, !tbaa !15
  br label %157

; <label>:157                                     ; preds = %152, %150
  %158 = icmp slt i32 %7, 1
  br i1 %158, label %159, label %164

; <label>:159                                     ; preds = %157
  %160 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?tempBuffer@@3PAMA", i32 0, i32 %42
  %161 = load float, float addrspace(3)* %160, align 4, !tbaa !15
  %162 = load float, float addrspace(3)* %103, align 4, !tbaa !15
  %163 = fadd fast float %162, %161
  store float %163, float addrspace(3)* %103, align 4, !tbaa !15
  br label %164

; <label>:164                                     ; preds = %159, %157
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %165 = load float, float addrspace(3)* getelementptr inbounds ([256 x float], [256 x float] addrspace(3)* @"\01?tempBuffer@@3PAMA", i32 0, i32 0), align 4, !tbaa !15
  %166 = load float, float addrspace(3)* %101, align 4, !tbaa !15
  %167 = fdiv fast float %166, %165
  store float %167, float addrspace(3)* %101, align 4, !tbaa !15
  %168 = call %dx.types.ResRet.f32 @dx.op.textureLoad.f32(i32 66, %dx.types.Handle %3, i32 undef, i32 %5, i32 0, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %169 = extractvalue %dx.types.ResRet.f32 %168, 0
  %170 = fdiv fast float %167, %169
  %171 = call float @dx.op.unary.f32(i32 23, float %170)  ; Log(value)
  %172 = fmul fast float %167, 0x3FE62E4300000000
  %173 = fmul fast float %172, %171
  %174 = fdiv fast float %169, %167
  %175 = call float @dx.op.unary.f32(i32 23, float %174)  ; Log(value)
  %176 = fmul fast float %169, 0x3FE62E4300000000
  %177 = fmul fast float %176, %175
  %178 = mul i32 %7, 2
  %179 = add i32 0, %178
  %180 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %179
  store float %173, float addrspace(3)* %180, align 4
  %181 = mul i32 %7, 2
  %182 = add i32 1, %181
  %183 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %182
  store float %177, float addrspace(3)* %183, align 4
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  br i1 %104, label %184, label %201

; <label>:184                                     ; preds = %164
  %185 = add nsw i32 %7, 128
  %186 = mul i32 %185, 2
  %187 = add i32 0, %186
  %188 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %187
  %189 = load float, float addrspace(3)* %188, align 4
  %190 = mul i32 %185, 2
  %191 = add i32 1, %190
  %192 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %191
  %193 = load float, float addrspace(3)* %192, align 4
  %194 = mul i32 %7, 2
  %195 = add i32 0, %194
  %196 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %195
  %197 = load float, float addrspace(3)* %196, align 4
  %198 = load float, float addrspace(3)* %183, align 4
  %199 = fadd fast float %197, %189
  %200 = fadd fast float %198, %193
  store float %199, float addrspace(3)* %196, align 4
  store float %200, float addrspace(3)* %183, align 4
  br label %201

; <label>:201                                     ; preds = %184, %164
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  br i1 %112, label %202, label %219

; <label>:202                                     ; preds = %201
  %203 = add nsw i32 %7, 64
  %204 = mul i32 %203, 2
  %205 = add i32 0, %204
  %206 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %205
  %207 = load float, float addrspace(3)* %206, align 4
  %208 = mul i32 %203, 2
  %209 = add i32 1, %208
  %210 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %209
  %211 = load float, float addrspace(3)* %210, align 4
  %212 = mul i32 %7, 2
  %213 = add i32 0, %212
  %214 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %213
  %215 = load float, float addrspace(3)* %214, align 4
  %216 = load float, float addrspace(3)* %183, align 4
  %217 = fadd fast float %215, %207
  %218 = fadd fast float %216, %211
  store float %217, float addrspace(3)* %214, align 4
  store float %218, float addrspace(3)* %183, align 4
  br label %219

; <label>:219                                     ; preds = %202, %201
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  br i1 %120, label %220, label %237

; <label>:220                                     ; preds = %219
  %221 = add nsw i32 %7, 32
  %222 = mul i32 %221, 2
  %223 = add i32 0, %222
  %224 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %223
  %225 = load float, float addrspace(3)* %224, align 4
  %226 = mul i32 %221, 2
  %227 = add i32 1, %226
  %228 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %227
  %229 = load float, float addrspace(3)* %228, align 4
  %230 = mul i32 %7, 2
  %231 = add i32 0, %230
  %232 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %231
  %233 = load float, float addrspace(3)* %232, align 4
  %234 = load float, float addrspace(3)* %183, align 4
  %235 = fadd fast float %233, %225
  %236 = fadd fast float %234, %229
  store float %235, float addrspace(3)* %232, align 4
  store float %236, float addrspace(3)* %183, align 4
  br label %237

; <label>:237                                     ; preds = %220, %219
  br i1 %128, label %238, label %255

; <label>:238                                     ; preds = %237
  %239 = add nsw i32 %7, 16
  %240 = mul i32 %239, 2
  %241 = add i32 0, %240
  %242 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %241
  %243 = load float, float addrspace(3)* %242, align 4
  %244 = mul i32 %239, 2
  %245 = add i32 1, %244
  %246 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %245
  %247 = load float, float addrspace(3)* %246, align 4
  %248 = mul i32 %7, 2
  %249 = add i32 0, %248
  %250 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %249
  %251 = load float, float addrspace(3)* %250, align 4
  %252 = load float, float addrspace(3)* %183, align 4
  %253 = fadd fast float %251, %243
  %254 = fadd fast float %252, %247
  store float %253, float addrspace(3)* %250, align 4
  store float %254, float addrspace(3)* %183, align 4
  br label %255

; <label>:255                                     ; preds = %238, %237
  br i1 %136, label %256, label %273

; <label>:256                                     ; preds = %255
  %257 = add nsw i32 %7, 8
  %258 = mul i32 %257, 2
  %259 = add i32 0, %258
  %260 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %259
  %261 = load float, float addrspace(3)* %260, align 4
  %262 = mul i32 %257, 2
  %263 = add i32 1, %262
  %264 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %263
  %265 = load float, float addrspace(3)* %264, align 4
  %266 = mul i32 %7, 2
  %267 = add i32 0, %266
  %268 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %267
  %269 = load float, float addrspace(3)* %268, align 4
  %270 = load float, float addrspace(3)* %183, align 4
  %271 = fadd fast float %269, %261
  %272 = fadd fast float %270, %265
  store float %271, float addrspace(3)* %268, align 4
  store float %272, float addrspace(3)* %183, align 4
  br label %273

; <label>:273                                     ; preds = %256, %255
  br i1 %144, label %274, label %290

; <label>:274                                     ; preds = %273
  %275 = mul i32 %57, 2
  %276 = add i32 0, %275
  %277 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %276
  %278 = load float, float addrspace(3)* %277, align 4
  %279 = mul i32 %57, 2
  %280 = add i32 1, %279
  %281 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %280
  %282 = load float, float addrspace(3)* %281, align 4
  %283 = mul i32 %7, 2
  %284 = add i32 0, %283
  %285 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %284
  %286 = load float, float addrspace(3)* %285, align 4
  %287 = load float, float addrspace(3)* %183, align 4
  %288 = fadd fast float %286, %278
  %289 = fadd fast float %287, %282
  store float %288, float addrspace(3)* %285, align 4
  store float %289, float addrspace(3)* %183, align 4
  br label %290

; <label>:290                                     ; preds = %274, %273
  br i1 %151, label %291, label %307

; <label>:291                                     ; preds = %290
  %292 = mul i32 %47, 2
  %293 = add i32 0, %292
  %294 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %293
  %295 = load float, float addrspace(3)* %294, align 4
  %296 = mul i32 %47, 2
  %297 = add i32 1, %296
  %298 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %297
  %299 = load float, float addrspace(3)* %298, align 4
  %300 = mul i32 %7, 2
  %301 = add i32 0, %300
  %302 = getelementptr [512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 %301
  %303 = load float, float addrspace(3)* %302, align 4
  %304 = load float, float addrspace(3)* %183, align 4
  %305 = fadd fast float %303, %295
  %306 = fadd fast float %304, %299
  store float %305, float addrspace(3)* %302, align 4
  store float %306, float addrspace(3)* %183, align 4
  br label %307

; <label>:307                                     ; preds = %291, %290
  %308 = icmp eq i32 %7, 0
  br i1 %308, label %309, label %345

; <label>:309                                     ; preds = %307
  %310 = load float, float addrspace(3)* getelementptr inbounds ([512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 0), align 4
  %311 = load float, float addrspace(3)* getelementptr inbounds ([512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 1), align 4
  %312 = load float, float addrspace(3)* getelementptr inbounds ([512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 2), align 4
  %313 = load float, float addrspace(3)* getelementptr inbounds ([512 x float], [512 x float] addrspace(3)* @"\01?tempBuffer2@@3PAV?$vector@M$01@@A.v.1dim", i32 0, i32 3), align 4
  %314 = fadd fast float %312, %310
  %315 = fadd fast float %313, %311
  %316 = call float @dx.op.unary.f32(i32 6, float %314)  ; FAbs(value)
  %317 = call float @dx.op.unary.f32(i32 6, float %315)  ; FAbs(value)
  %318 = fadd fast float %317, %316
  %319 = fmul fast float %318, 0xBFF7154760000000
  %320 = call float @dx.op.unary.f32(i32 21, float %319)  ; Exp(value)
  %321 = fsub fast float 1.000000e+00, %320
  %322 = fmul fast float %321, 0x40FB2071C0000000
  %323 = fptoui float %322 to i32
  %324 = call i32 @dx.op.atomicBinOp.i32(i32 78, %dx.types.Handle %2, i32 0, i32 %6, i32 0, i32 undef, i32 %323)  ; AtomicBinOp(handle,atomicOp,offset0,offset1,offset2,newValue)
  %325 = call i32 @dx.op.atomicBinOp.i32(i32 78, %dx.types.Handle %1, i32 0, i32 2, i32 0, i32 undef, i32 1)  ; AtomicBinOp(handle,atomicOp,offset0,offset1,offset2,newValue)
  %326 = icmp eq i32 %325, 26
  br i1 %326, label %327, label %345

; <label>:327                                     ; preds = %309
  %328 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %2, i32 undef, i32 0, i32 0, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %329 = extractvalue %dx.types.ResRet.i32 %328, 0
  %330 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %2, i32 undef, i32 1, i32 0, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %331 = extractvalue %dx.types.ResRet.i32 %330, 0
  %332 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %2, i32 undef, i32 2, i32 0, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %333 = extractvalue %dx.types.ResRet.i32 %332, 0
  %334 = call i32 @dx.op.binary.i32(i32 40, i32 %331, i32 %333)  ; UMin(a,b)
  %335 = call i32 @dx.op.binary.i32(i32 40, i32 %329, i32 %334)  ; UMin(a,b)
  %336 = uitofp i32 %335 to float
  %337 = fmul fast float %336, 0x3EB0C6F7A0000000
  %338 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %1, i32 undef, i32 1, i32 0, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %339 = extractvalue %dx.types.ResRet.i32 %338, 0
  %340 = shl i32 %339, 1
  %341 = fcmp fast ogt float %337, 0x3FDCCCCCC0000000
  %342 = zext i1 %341 to i32
  %343 = or i32 %340, %342
  %344 = bitcast float %337 to i32
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %1, i32 0, i32 0, i32 undef, i32 %344, i32 %344, i32 %344, i32 %344, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %1, i32 1, i32 0, i32 undef, i32 %343, i32 %343, i32 %343, i32 %343, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %1, i32 2, i32 0, i32 undef, i32 0, i32 0, i32 0, i32 0, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %2, i32 0, i32 0, i32 undef, i32 0, i32 0, i32 0, i32 0, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %2, i32 1, i32 0, i32 undef, i32 0, i32 0, i32 0, i32 0, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %2, i32 2, i32 0, i32 undef, i32 0, i32 0, i32 0, i32 0, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  br label %345

; <label>:345                                     ; preds = %327, %309, %307
  %346 = icmp eq i32 %6, 1
  br i1 %346, label %347, label %348

; <label>:347                                     ; preds = %345
  call void @dx.op.textureStore.f32(i32 67, %dx.types.Handle %3, i32 %5, i32 0, i32 undef, float %167, float %167, float %167, float %167, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %4, i32 %5, i32 0, i32 undef, i32 0, i32 0, i32 0, i32 0, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  br label %348

; <label>:348                                     ; preds = %347, %345
  ret void
}

; Function Attrs: nounwind readnone
declare i32 @dx.op.threadId.i32(i32, i32) #0

; Function Attrs: nounwind readnone
declare i32 @dx.op.flattenedThreadIdInGroup.i32(i32) #0

; Function Attrs: noduplicate nounwind
declare void @dx.op.barrier(i32, i32) #1

; Function Attrs: nounwind readnone
declare float @dx.op.unary.f32(i32, float) #0

; Function Attrs: nounwind
declare void @dx.op.textureStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32, i8) #2

; Function Attrs: nounwind readonly
declare %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32) #3

; Function Attrs: nounwind
declare i32 @dx.op.atomicBinOp.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32) #2

; Function Attrs: nounwind readnone
declare i32 @dx.op.binary.i32(i32, i32, i32) #0

; Function Attrs: nounwind
declare void @dx.op.textureStore.f32(i32, %dx.types.Handle, i32, i32, i32, float, float, float, float, i8) #2

; Function Attrs: nounwind readonly
declare %dx.types.ResRet.f32 @dx.op.textureLoad.f32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32) #3

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #3

attributes #0 = { nounwind readnone }
attributes #1 = { noduplicate nounwind }
attributes #2 = { nounwind }
attributes #3 = { nounwind readonly }

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
!4 = !{null, !5, null, null}
!5 = !{!6, !8, !10, !11}
!6 = !{i32 0, %"class.RWTexture2D<unsigned int>"* undef, !"", i32 0, i32 0, i32 1, i32 2, i1 false, i1 false, i1 false, !7}
!7 = !{i32 0, i32 5}
!8 = !{i32 1, %"class.RWTexture2D<float>"* undef, !"", i32 0, i32 1, i32 1, i32 2, i1 false, i1 false, i1 false, !9}
!9 = !{i32 0, i32 9}
!10 = !{i32 2, %"class.RWTexture2D<unsigned int>"* undef, !"", i32 0, i32 2, i32 1, i32 2, i1 false, i1 false, i1 false, !7}
!11 = !{i32 3, %"class.RWTexture2D<unsigned int>"* undef, !"", i32 0, i32 3, i32 1, i32 2, i1 false, i1 false, i1 false, !7}
!12 = !{void ()* @CS, !"CS", null, !4, !13}
!13 = !{i32 4, !14}
!14 = !{i32 256, i32 1, i32 1}
!15 = !{!16, !16, i64 0}
!16 = !{!"float", !17, i64 0}
!17 = !{!"omnipotent char", !18, i64 0}
!18 = !{!"Simple C/C++ TBAA"}
