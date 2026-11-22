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
; shader hash: 190e676731093287e57c429c51798012
;
; Pipeline Runtime Information: 
;
;PSVRuntimeInfo:
; Compute Shader
; NumThreads=(4,4,4)
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
; r_optical_flow_previous_input     texture     u32          2d      T1             t1     1
; r_optical_flow                    texture     i32          2d      T2             t2     1
; rw_optical_flow_next_level            UAV     i32          2d      U0             u0     1
; rw_optical_flow_scd_output            UAV     u32          2d      U1             u1     1
;
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.CBufRet.i32 = type { i32, i32, i32, i32 }
%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }
%"class.Texture2D<unsigned int>" = type { i32, %"class.Texture2D<unsigned int>::mips_type" }
%"class.Texture2D<unsigned int>::mips_type" = type { i32 }
%"class.Texture2D<vector<int, 2> >" = type { <2 x i32>, %"class.Texture2D<vector<int, 2> >::mips_type" }
%"class.Texture2D<vector<int, 2> >::mips_type" = type { i32 }
%"class.RWTexture2D<vector<int, 2> >" = type { <2 x i32> }
%"class.RWTexture2D<unsigned int>" = type { i32 }
%cbOF = type { <2 x i32>, i32, i32, i32, i32, <2 x float> }

@"\01?localRegion@@3PAY133$$CAIA.1dim" = addrspace(3) global [64 x i32] undef, align 4
@"\01?sads@@3PAY133$$CAIA.1dim" = addrspace(3) global [64 x i32] undef, align 4
@"\01?nearestVectors@@3PAY133$$CAV?$vector@H$01@@A.v.1dim" = addrspace(3) global [128 x i32] undef, align 4

define void @CS() {
  %1 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 1, i32 1, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %2 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %3 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 2, i32 2, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %4 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 1, i32 1, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %5 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %6 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %7 = call i32 @dx.op.threadId.i32(i32 93, i32 0)  ; ThreadId(component)
  %8 = call i32 @dx.op.threadId.i32(i32 93, i32 1)  ; ThreadId(component)
  %9 = call i32 @dx.op.threadIdInGroup.i32(i32 95, i32 0)  ; ThreadIdInGroup(component)
  %10 = call i32 @dx.op.threadIdInGroup.i32(i32 95, i32 1)  ; ThreadIdInGroup(component)
  %11 = call i32 @dx.op.threadIdInGroup.i32(i32 95, i32 2)  ; ThreadIdInGroup(component)
  %12 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %6, i32 1)  ; CBufferLoadLegacy(handle,regIndex)
  %13 = extractvalue %dx.types.CBufRet.i32 %12, 0
  %14 = icmp ult i32 %13, 6
  br i1 %14, label %20, label %15

; <label>:15                                      ; preds = %0
  %16 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %1, i32 undef, i32 1, i32 0, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %17 = extractvalue %dx.types.ResRet.i32 %16, 0
  %18 = and i32 %17, 15
  %19 = icmp ne i32 %18, 0
  br label %20

; <label>:20                                      ; preds = %15, %0
  %21 = phi i1 [ %19, %15 ], [ true, %0 ]
  br i1 %21, label %22, label %23

; <label>:22                                      ; preds = %20
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %2, i32 %7, i32 %8, i32 undef, i32 0, i32 0, i32 0, i32 0, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  br label %295

; <label>:23                                      ; preds = %20
  %24 = srem i32 %11, 2
  %25 = add nsw i32 %24, -1
  %26 = srem i32 %7, 2
  %27 = add nsw i32 %25, %26
  %28 = sdiv i32 %11, 2
  %29 = add nsw i32 %28, -1
  %30 = srem i32 %8, 2
  %31 = add nsw i32 %29, %30
  %32 = sdiv i32 %7, 2
  %33 = add nsw i32 %27, %32
  %34 = sdiv i32 %8, 2
  %35 = add nsw i32 %31, %34
  %36 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %3, i32 0, i32 %33, i32 %35, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %37 = extractvalue %dx.types.ResRet.i32 %36, 0
  %38 = extractvalue %dx.types.ResRet.i32 %36, 1
  %39 = shl i32 %37, 1
  %40 = shl i32 %38, 1
  %41 = mul i32 %11, 4
  %42 = add i32 %10, %41
  %43 = mul i32 %42, 4
  %44 = add i32 %9, %43
  %45 = mul i32 %44, 2
  %46 = add i32 0, %45
  %47 = getelementptr [128 x i32], [128 x i32] addrspace(3)* @"\01?nearestVectors@@3PAY133$$CAV?$vector@H$01@@A.v.1dim", i32 0, i32 %46
  store i32 %39, i32 addrspace(3)* %47, align 4
  %48 = mul i32 %11, 4
  %49 = add i32 %10, %48
  %50 = mul i32 %49, 4
  %51 = add i32 %9, %50
  %52 = mul i32 %51, 2
  %53 = add i32 1, %52
  %54 = getelementptr [128 x i32], [128 x i32] addrspace(3)* @"\01?nearestVectors@@3PAY133$$CAV?$vector@H$01@@A.v.1dim", i32 0, i32 %53
  store i32 %40, i32 addrspace(3)* %54, align 4
  %55 = icmp slt i32 %11, 4
  br i1 %55, label %56, label %134

; <label>:56                                      ; preds = %23
  br label %57

; <label>:57                                      ; preds = %124, %56
  %58 = phi i32 [ %131, %124 ], [ %11, %56 ]
  %59 = shl nsw i32 %7, 2
  %60 = shl i32 %8, 2
  %61 = add nsw i32 %58, %60
  %62 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %6, i32 0)  ; CBufferLoadLegacy(handle,regIndex)
  %63 = extractvalue %dx.types.CBufRet.i32 %62, 0
  %64 = extractvalue %dx.types.CBufRet.i32 %62, 2
  %65 = and i32 %64, 31
  %66 = ashr i32 %63, %65
  %67 = extractvalue %dx.types.CBufRet.i32 %62, 1
  %68 = ashr i32 %67, %65
  %69 = add nsw i32 %66, -4
  %70 = call i32 @dx.op.binary.i32(i32 38, i32 %69, i32 %59)  ; IMin(a,b)
  %71 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %70)  ; IMax(a,b)
  %72 = add nsw i32 %68, -1
  %73 = call i32 @dx.op.binary.i32(i32 38, i32 %72, i32 %61)  ; IMin(a,b)
  %74 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %73)  ; IMax(a,b)
  %75 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %5, i32 0, i32 %71, i32 %74, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %76 = extractvalue %dx.types.ResRet.i32 %75, 0
  %77 = add i32 %71, 1
  %78 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %5, i32 0, i32 %77, i32 %74, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %79 = extractvalue %dx.types.ResRet.i32 %78, 0
  %80 = add i32 %71, 2
  %81 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %5, i32 0, i32 %80, i32 %74, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %82 = extractvalue %dx.types.ResRet.i32 %81, 0
  %83 = add i32 %71, 3
  %84 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %5, i32 0, i32 %83, i32 %74, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %85 = extractvalue %dx.types.ResRet.i32 %84, 0
  %86 = shl i32 %79, 8
  %87 = or i32 %86, %76
  %88 = shl i32 %82, 16
  %89 = or i32 %87, %88
  %90 = shl i32 %85, 24
  %91 = or i32 %90, %89
  %92 = icmp slt i32 %7, 0
  br i1 %92, label %93, label %105

; <label>:93                                      ; preds = %57
  %94 = and i32 %76, 255
  %95 = shl i32 %89, 8
  %96 = or i32 %95, %94
  %97 = icmp slt i32 %59, -1
  %98 = shl i32 %96, 8
  %99 = or i32 %98, %94
  %100 = select i1 %97, i32 %99, i32 %96
  %101 = icmp slt i32 %59, -2
  br i1 %101, label %102, label %124

; <label>:102                                     ; preds = %93
  %103 = shl i32 %100, 8
  %104 = or i32 %103, %94
  br label %124

; <label>:105                                     ; preds = %57
  %106 = icmp sgt i32 %59, %69
  br i1 %106, label %107, label %124

; <label>:107                                     ; preds = %105
  %108 = and i32 %91, -16777216
  %109 = add nsw i32 %66, -3
  %110 = icmp sge i32 %59, %109
  %111 = lshr i32 %91, 8
  %112 = or i32 %111, %108
  %113 = select i1 %110, i32 %112, i32 %91
  %114 = add nsw i32 %66, -2
  %115 = icmp sge i32 %59, %114
  %116 = lshr i32 %113, 8
  %117 = or i32 %116, %108
  %118 = select i1 %115, i32 %117, i32 %113
  %119 = add nsw i32 %66, -1
  %120 = icmp slt i32 %59, %119
  br i1 %120, label %124, label %121

; <label>:121                                     ; preds = %107
  %122 = lshr i32 %118, 8
  %123 = or i32 %122, %108
  br label %124

; <label>:124                                     ; preds = %121, %107, %105, %102, %93
  %125 = phi i32 [ %104, %102 ], [ %100, %93 ], [ %123, %121 ], [ %118, %107 ], [ %91, %105 ]
  %126 = mul i32 %58, 4
  %127 = add i32 %10, %126
  %128 = mul i32 %127, 4
  %129 = add i32 %9, %128
  %130 = getelementptr [64 x i32], [64 x i32] addrspace(3)* @"\01?localRegion@@3PAY133$$CAIA.1dim", i32 0, i32 %129
  store i32 %125, i32 addrspace(3)* %130, align 4, !tbaa !19
  %131 = add nsw i32 %58, 4
  %132 = icmp slt i32 %58, 0
  br i1 %132, label %57, label %133

; <label>:133                                     ; preds = %124
  br label %134

; <label>:134                                     ; preds = %133, %23
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  br label %135

; <label>:135                                     ; preds = %205, %134
  %136 = phi i32 [ 0, %134 ], [ %240, %205 ]
  %137 = phi i32 [ 0, %134 ], [ %241, %205 ]
  %138 = shl nsw i32 %7, 2
  %139 = shl i32 %8, 2
  %140 = add i32 %37, %138
  %141 = add i32 %38, %139
  %142 = add i32 %141, %137
  %143 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %6, i32 0)  ; CBufferLoadLegacy(handle,regIndex)
  %144 = extractvalue %dx.types.CBufRet.i32 %143, 0
  %145 = extractvalue %dx.types.CBufRet.i32 %143, 2
  %146 = and i32 %145, 31
  %147 = ashr i32 %144, %146
  %148 = extractvalue %dx.types.CBufRet.i32 %143, 1
  %149 = ashr i32 %148, %146
  %150 = add nsw i32 %147, -4
  %151 = call i32 @dx.op.binary.i32(i32 38, i32 %150, i32 %140)  ; IMin(a,b)
  %152 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %151)  ; IMax(a,b)
  %153 = add nsw i32 %149, -1
  %154 = call i32 @dx.op.binary.i32(i32 38, i32 %153, i32 %142)  ; IMin(a,b)
  %155 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %154)  ; IMax(a,b)
  %156 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %4, i32 0, i32 %152, i32 %155, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %157 = extractvalue %dx.types.ResRet.i32 %156, 0
  %158 = add i32 %152, 1
  %159 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %4, i32 0, i32 %158, i32 %155, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %160 = extractvalue %dx.types.ResRet.i32 %159, 0
  %161 = add i32 %152, 2
  %162 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %4, i32 0, i32 %161, i32 %155, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %163 = extractvalue %dx.types.ResRet.i32 %162, 0
  %164 = add i32 %152, 3
  %165 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %4, i32 0, i32 %164, i32 %155, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %166 = extractvalue %dx.types.ResRet.i32 %165, 0
  %167 = shl i32 %160, 8
  %168 = or i32 %167, %157
  %169 = shl i32 %163, 16
  %170 = or i32 %168, %169
  %171 = shl i32 %166, 24
  %172 = or i32 %171, %170
  %173 = icmp slt i32 %140, 0
  br i1 %173, label %174, label %186

; <label>:174                                     ; preds = %135
  %175 = and i32 %157, 255
  %176 = shl i32 %170, 8
  %177 = or i32 %176, %175
  %178 = icmp slt i32 %140, -1
  %179 = shl i32 %177, 8
  %180 = or i32 %179, %175
  %181 = select i1 %178, i32 %180, i32 %177
  %182 = icmp slt i32 %140, -2
  br i1 %182, label %183, label %205

; <label>:183                                     ; preds = %174
  %184 = shl i32 %181, 8
  %185 = or i32 %184, %175
  br label %205

; <label>:186                                     ; preds = %135
  %187 = icmp sgt i32 %140, %150
  br i1 %187, label %188, label %205

; <label>:188                                     ; preds = %186
  %189 = and i32 %172, -16777216
  %190 = add nsw i32 %147, -3
  %191 = icmp sge i32 %140, %190
  %192 = lshr i32 %172, 8
  %193 = or i32 %192, %189
  %194 = select i1 %191, i32 %193, i32 %172
  %195 = add nsw i32 %147, -2
  %196 = icmp sge i32 %140, %195
  %197 = lshr i32 %194, 8
  %198 = or i32 %197, %189
  %199 = select i1 %196, i32 %198, i32 %194
  %200 = add nsw i32 %147, -1
  %201 = icmp slt i32 %140, %200
  br i1 %201, label %205, label %202

; <label>:202                                     ; preds = %188
  %203 = lshr i32 %199, 8
  %204 = or i32 %203, %189
  br label %205

; <label>:205                                     ; preds = %202, %188, %186, %183, %174
  %206 = phi i32 [ %185, %183 ], [ %181, %174 ], [ %204, %202 ], [ %199, %188 ], [ %172, %186 ]
  %207 = mul i32 %137, 4
  %208 = add i32 %10, %207
  %209 = mul i32 %208, 4
  %210 = add i32 %9, %209
  %211 = getelementptr [64 x i32], [64 x i32] addrspace(3)* @"\01?localRegion@@3PAY133$$CAIA.1dim", i32 0, i32 %210
  %212 = load i32, i32 addrspace(3)* %211, align 4, !tbaa !19
  %213 = and i32 %212, 255
  %214 = and i32 %206, 255
  %215 = sub nsw i32 %213, %214
  %216 = sub nsw i32 0, %215
  %217 = call i32 @dx.op.binary.i32(i32 37, i32 %215, i32 %216)  ; IMax(a,b)
  %218 = lshr i32 %212, 8
  %219 = and i32 %218, 255
  %220 = lshr i32 %206, 8
  %221 = and i32 %220, 255
  %222 = sub nsw i32 %219, %221
  %223 = sub nsw i32 0, %222
  %224 = call i32 @dx.op.binary.i32(i32 37, i32 %222, i32 %223)  ; IMax(a,b)
  %225 = lshr i32 %212, 16
  %226 = and i32 %225, 255
  %227 = lshr i32 %206, 16
  %228 = and i32 %227, 255
  %229 = sub nsw i32 %226, %228
  %230 = sub nsw i32 0, %229
  %231 = call i32 @dx.op.binary.i32(i32 37, i32 %229, i32 %230)  ; IMax(a,b)
  %232 = lshr i32 %212, 24
  %233 = lshr i32 %206, 24
  %234 = sub nsw i32 %232, %233
  %235 = sub nsw i32 0, %234
  %236 = call i32 @dx.op.binary.i32(i32 37, i32 %234, i32 %235)  ; IMax(a,b)
  %237 = add i32 %217, %136
  %238 = add i32 %237, %224
  %239 = add i32 %238, %231
  %240 = add i32 %239, %236
  %241 = add nuw nsw i32 %137, 1
  %242 = icmp eq i32 %241, 4
  br i1 %242, label %243, label %135

; <label>:243                                     ; preds = %205
  %244 = mul i32 %11, 4
  %245 = add i32 %10, %244
  %246 = mul i32 %245, 4
  %247 = add i32 %9, %246
  %248 = getelementptr [64 x i32], [64 x i32] addrspace(3)* @"\01?sads@@3PAY133$$CAIA.1dim", i32 0, i32 %247
  store i32 %240, i32 addrspace(3)* %248, align 4, !tbaa !19
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %249 = icmp eq i32 %11, 0
  br i1 %249, label %250, label %295

; <label>:250                                     ; preds = %243
  %251 = add i32 %10, 0
  %252 = mul i32 %251, 4
  %253 = add i32 %9, %252
  %254 = getelementptr [64 x i32], [64 x i32] addrspace(3)* @"\01?sads@@3PAY133$$CAIA.1dim", i32 0, i32 %253
  %255 = load i32, i32 addrspace(3)* %254, align 4, !tbaa !19
  %256 = add i32 %10, 4
  %257 = mul i32 %256, 4
  %258 = add i32 %9, %257
  %259 = getelementptr [64 x i32], [64 x i32] addrspace(3)* @"\01?sads@@3PAY133$$CAIA.1dim", i32 0, i32 %258
  %260 = load i32, i32 addrspace(3)* %259, align 4, !tbaa !19
  %261 = icmp ult i32 %260, %255
  %262 = select i1 %261, i32 %260, i32 %255
  %263 = zext i1 %261 to i32
  %264 = add i32 %10, 8
  %265 = mul i32 %264, 4
  %266 = add i32 %9, %265
  %267 = getelementptr [64 x i32], [64 x i32] addrspace(3)* @"\01?sads@@3PAY133$$CAIA.1dim", i32 0, i32 %266
  %268 = load i32, i32 addrspace(3)* %267, align 4, !tbaa !19
  %269 = icmp ult i32 %268, %262
  %270 = select i1 %269, i32 %268, i32 %262
  %271 = select i1 %269, i32 2, i32 %263
  %272 = add i32 %10, 12
  %273 = mul i32 %272, 4
  %274 = add i32 %9, %273
  %275 = getelementptr [64 x i32], [64 x i32] addrspace(3)* @"\01?sads@@3PAY133$$CAIA.1dim", i32 0, i32 %274
  %276 = load i32, i32 addrspace(3)* %275, align 4, !tbaa !19
  %277 = icmp ult i32 %276, %270
  %278 = select i1 %277, i32 3, i32 %271
  %279 = mul i32 %278, 4
  %280 = add i32 %10, %279
  %281 = mul i32 %280, 4
  %282 = add i32 %9, %281
  %283 = mul i32 %282, 2
  %284 = add i32 0, %283
  %285 = getelementptr [128 x i32], [128 x i32] addrspace(3)* @"\01?nearestVectors@@3PAY133$$CAV?$vector@H$01@@A.v.1dim", i32 0, i32 %284
  %286 = load i32, i32 addrspace(3)* %285, align 4
  %287 = mul i32 %278, 4
  %288 = add i32 %10, %287
  %289 = mul i32 %288, 4
  %290 = add i32 %9, %289
  %291 = mul i32 %290, 2
  %292 = add i32 1, %291
  %293 = getelementptr [128 x i32], [128 x i32] addrspace(3)* @"\01?nearestVectors@@3PAY133$$CAV?$vector@H$01@@A.v.1dim", i32 0, i32 %292
  %294 = load i32, i32 addrspace(3)* %293, align 4
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %2, i32 %7, i32 %8, i32 undef, i32 %286, i32 %294, i32 %286, i32 %286, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  br label %295

; <label>:295                                     ; preds = %250, %243, %22
  ret void
}

; Function Attrs: nounwind readnone
declare i32 @dx.op.threadId.i32(i32, i32) #0

; Function Attrs: nounwind readnone
declare i32 @dx.op.threadIdInGroup.i32(i32, i32) #0

; Function Attrs: noduplicate nounwind
declare void @dx.op.barrier(i32, i32) #1

; Function Attrs: nounwind readonly
declare %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32) #2

; Function Attrs: nounwind
declare void @dx.op.textureStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32, i8) #3

; Function Attrs: nounwind readnone
declare i32 @dx.op.binary.i32(i32, i32, i32) #0

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
!dx.entryPoints = !{!16}

!0 = !{!"dxcoob 1.8.2502.11 (239921522)"}
!1 = !{i32 1, i32 2}
!2 = !{i32 1, i32 8}
!3 = !{!"cs", i32 6, i32 2}
!4 = !{!5, !11, !14, null}
!5 = !{!6, !8, !9}
!6 = !{i32 0, %"class.Texture2D<unsigned int>"* undef, !"", i32 0, i32 0, i32 1, i32 2, i32 0, !7}
!7 = !{i32 0, i32 5}
!8 = !{i32 1, %"class.Texture2D<unsigned int>"* undef, !"", i32 0, i32 1, i32 1, i32 2, i32 0, !7}
!9 = !{i32 2, %"class.Texture2D<vector<int, 2> >"* undef, !"", i32 0, i32 2, i32 1, i32 2, i32 0, !10}
!10 = !{i32 0, i32 4}
!11 = !{!12, !13}
!12 = !{i32 0, %"class.RWTexture2D<vector<int, 2> >"* undef, !"", i32 0, i32 0, i32 1, i32 2, i1 false, i1 false, i1 false, !10}
!13 = !{i32 1, %"class.RWTexture2D<unsigned int>"* undef, !"", i32 0, i32 1, i32 1, i32 2, i1 false, i1 false, i1 false, !7}
!14 = !{!15}
!15 = !{i32 0, %cbOF* undef, !"", i32 0, i32 0, i32 1, i32 32, null}
!16 = !{void ()* @CS, !"CS", null, !4, !17}
!17 = !{i32 4, !18}
!18 = !{i32 4, i32 4, i32 4}
!19 = !{!20, !20, i64 0}
!20 = !{!"int", !21, i64 0}
!21 = !{!"omnipotent char", !22, i64 0}
!22 = !{!"Simple C/C++ TBAA"}
