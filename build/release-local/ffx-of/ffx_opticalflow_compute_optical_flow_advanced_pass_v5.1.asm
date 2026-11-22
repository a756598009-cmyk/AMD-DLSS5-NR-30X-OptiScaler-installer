;
; Note: shader requires additional functionality:
;       Typed UAV Load Additional Formats
;       Wave level operations
;
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
; shader hash: 08910c2c4db2ecb4dba55707ae9b3ea2
;
; Pipeline Runtime Information: 
;
;PSVRuntimeInfo:
; Compute Shader
; NumThreads=(64,1,1)
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
; rw_optical_flow                       UAV     i32          2d      U0             u0     1
; rw_optical_flow_scd_output            UAV     u32          2d      U1             u1     1
;
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.CBufRet.i32 = type { i32, i32, i32, i32 }
%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }
%"class.Texture2D<unsigned int>" = type { i32, %"class.Texture2D<unsigned int>::mips_type" }
%"class.Texture2D<unsigned int>::mips_type" = type { i32 }
%"class.RWTexture2D<vector<int, 2> >" = type { <2 x i32> }
%"class.RWTexture2D<unsigned int>" = type { i32 }
%cbOF = type { <2 x i32>, i32, i32, i32, i32, <2 x float> }

@"\01?sWaveSad@@3PAIA" = external addrspace(3) global [2 x i32], align 4
@"\01?sWaveMin@@3PAIA" = external addrspace(3) global [2 x i32], align 4
@"\01?pixels@@3PAY01$$CAIA.1dim" = addrspace(3) global [16 x i32] undef, align 4
@"\01?searchBuffer@@3PAY0JA@$$CAIA.1dim" = addrspace(3) global [144 x i32] undef, align 4
@"\01?sadMapBuffer@@3PAY1BA@4$$CAIA.1dim" = addrspace(3) global [320 x i32] undef, align 4

define void @CS() {
  %1 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 1, i32 1, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %2 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %3 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 1, i32 1, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %4 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %5 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %6 = call i32 @dx.op.groupId.i32(i32 94, i32 0)  ; GroupId(component)
  %7 = call i32 @dx.op.groupId.i32(i32 94, i32 1)  ; GroupId(component)
  %8 = call i32 @dx.op.flattenedThreadIdInGroup.i32(i32 96)  ; FlattenedThreadIdInGroup()
  %9 = and i32 %8, 3
  %10 = lshr i32 %8, 2
  %11 = and i32 %10, 15
  %12 = lshr i32 %8, 1
  %13 = and i32 %12, 1
  %14 = lshr i32 %8, 4
  %15 = and i32 %14, 2
  %16 = or i32 %13, %15
  %17 = shl i32 %6, 4
  %18 = shl i32 %7, 4
  %19 = shl nuw nsw i32 %9, 2
  %20 = or i32 %19, %17
  %21 = or i32 %11, %18
  %22 = ashr i32 %20, 3
  %23 = ashr i32 %21, 3
  %24 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %5, i32 1)  ; CBufferLoadLegacy(handle,regIndex)
  %25 = extractvalue %dx.types.CBufRet.i32 %24, 0
  %26 = icmp ult i32 %25, 6
  br i1 %26, label %32, label %27

; <label>:27                                      ; preds = %0
  %28 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %1, i32 undef, i32 1, i32 0, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %29 = extractvalue %dx.types.ResRet.i32 %28, 0
  %30 = and i32 %29, 15
  %31 = icmp ne i32 %30, 0
  br label %32

; <label>:32                                      ; preds = %27, %0
  %33 = phi i1 [ %31, %27 ], [ true, %0 ]
  br i1 %33, label %34, label %40

; <label>:34                                      ; preds = %32
  %35 = and i32 %10, 7
  %36 = and i32 %8, 1
  %37 = or i32 %35, %36
  %38 = icmp eq i32 %37, 0
  br i1 %38, label %39, label %600

; <label>:39                                      ; preds = %34
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %2, i32 %22, i32 %23, i32 undef, i32 0, i32 0, i32 0, i32 0, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  br label %600

; <label>:40                                      ; preds = %32
  %41 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %5, i32 0)  ; CBufferLoadLegacy(handle,regIndex)
  %42 = extractvalue %dx.types.CBufRet.i32 %41, 2
  %43 = extractvalue %dx.types.CBufRet.i32 %41, 3
  %44 = add i32 %43, -1
  %45 = icmp ne i32 %42, %44
  %46 = extractvalue %dx.types.CBufRet.i32 %41, 0
  %47 = and i32 %42, 31
  %48 = ashr i32 %46, %47
  %49 = extractvalue %dx.types.CBufRet.i32 %41, 1
  %50 = ashr i32 %49, %47
  %51 = add nsw i32 %48, -4
  %52 = call i32 @dx.op.binary.i32(i32 38, i32 %51, i32 %20)  ; IMin(a,b)
  %53 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %52)  ; IMax(a,b)
  %54 = add nsw i32 %50, -1
  %55 = call i32 @dx.op.binary.i32(i32 38, i32 %54, i32 %21)  ; IMin(a,b)
  %56 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %55)  ; IMax(a,b)
  %57 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %4, i32 0, i32 %53, i32 %56, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %58 = extractvalue %dx.types.ResRet.i32 %57, 0
  %59 = add i32 %53, 1
  %60 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %4, i32 0, i32 %59, i32 %56, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %61 = extractvalue %dx.types.ResRet.i32 %60, 0
  %62 = add i32 %53, 2
  %63 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %4, i32 0, i32 %62, i32 %56, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %64 = extractvalue %dx.types.ResRet.i32 %63, 0
  %65 = add i32 %53, 3
  %66 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %4, i32 0, i32 %65, i32 %56, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %67 = extractvalue %dx.types.ResRet.i32 %66, 0
  %68 = shl i32 %61, 8
  %69 = or i32 %68, %58
  %70 = shl i32 %64, 16
  %71 = or i32 %69, %70
  %72 = shl i32 %67, 24
  %73 = or i32 %72, %71
  %74 = icmp slt i32 %20, 0
  br i1 %74, label %75, label %87

; <label>:75                                      ; preds = %40
  %76 = and i32 %58, 255
  %77 = shl i32 %71, 8
  %78 = or i32 %77, %76
  %79 = icmp slt i32 %20, -1
  %80 = shl i32 %78, 8
  %81 = or i32 %80, %76
  %82 = select i1 %79, i32 %81, i32 %78
  %83 = icmp slt i32 %20, -2
  br i1 %83, label %84, label %106

; <label>:84                                      ; preds = %75
  %85 = shl i32 %82, 8
  %86 = or i32 %85, %76
  br label %106

; <label>:87                                      ; preds = %40
  %88 = icmp sgt i32 %20, %51
  br i1 %88, label %89, label %106

; <label>:89                                      ; preds = %87
  %90 = and i32 %73, -16777216
  %91 = add nsw i32 %48, -3
  %92 = icmp sge i32 %20, %91
  %93 = lshr i32 %73, 8
  %94 = or i32 %93, %90
  %95 = select i1 %92, i32 %94, i32 %73
  %96 = add nsw i32 %48, -2
  %97 = icmp sge i32 %20, %96
  %98 = lshr i32 %95, 8
  %99 = or i32 %98, %90
  %100 = select i1 %97, i32 %99, i32 %95
  %101 = add nsw i32 %48, -1
  %102 = icmp slt i32 %20, %101
  br i1 %102, label %106, label %103

; <label>:103                                     ; preds = %89
  %104 = lshr i32 %100, 8
  %105 = or i32 %104, %90
  br label %106

; <label>:106                                     ; preds = %103, %89, %87, %84, %75
  %107 = phi i32 [ %86, %84 ], [ %82, %75 ], [ %105, %103 ], [ %100, %89 ], [ %73, %87 ]
  %108 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %3, i32 0, i32 %53, i32 %56, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %109 = extractvalue %dx.types.ResRet.i32 %108, 0
  %110 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %3, i32 0, i32 %59, i32 %56, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %111 = extractvalue %dx.types.ResRet.i32 %110, 0
  %112 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %3, i32 0, i32 %62, i32 %56, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %113 = extractvalue %dx.types.ResRet.i32 %112, 0
  %114 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %3, i32 0, i32 %65, i32 %56, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %115 = extractvalue %dx.types.ResRet.i32 %114, 0
  %116 = shl i32 %111, 8
  %117 = or i32 %116, %109
  %118 = shl i32 %113, 16
  %119 = or i32 %117, %118
  %120 = shl i32 %115, 24
  %121 = or i32 %120, %119
  br i1 %74, label %122, label %134

; <label>:122                                     ; preds = %106
  %123 = and i32 %109, 255
  %124 = shl i32 %119, 8
  %125 = or i32 %124, %123
  %126 = icmp slt i32 %20, -1
  %127 = shl i32 %125, 8
  %128 = or i32 %127, %123
  %129 = select i1 %126, i32 %128, i32 %125
  %130 = icmp slt i32 %20, -2
  br i1 %130, label %131, label %153

; <label>:131                                     ; preds = %122
  %132 = shl i32 %129, 8
  %133 = or i32 %132, %123
  br label %153

; <label>:134                                     ; preds = %106
  %135 = icmp sgt i32 %20, %51
  br i1 %135, label %136, label %153

; <label>:136                                     ; preds = %134
  %137 = and i32 %121, -16777216
  %138 = add nsw i32 %48, -3
  %139 = icmp sge i32 %20, %138
  %140 = lshr i32 %121, 8
  %141 = or i32 %140, %137
  %142 = select i1 %139, i32 %141, i32 %121
  %143 = add nsw i32 %48, -2
  %144 = icmp sge i32 %20, %143
  %145 = lshr i32 %142, 8
  %146 = or i32 %145, %137
  %147 = select i1 %144, i32 %146, i32 %142
  %148 = add nsw i32 %48, -1
  %149 = icmp slt i32 %20, %148
  br i1 %149, label %153, label %150

; <label>:150                                     ; preds = %136
  %151 = lshr i32 %147, 8
  %152 = or i32 %151, %137
  br label %153

; <label>:153                                     ; preds = %150, %136, %134, %131, %122
  %154 = phi i32 [ %133, %131 ], [ %129, %122 ], [ %152, %150 ], [ %147, %136 ], [ %121, %134 ]
  %155 = and i32 %107, 255
  %156 = and i32 %154, 255
  %157 = sub nsw i32 %155, %156
  %158 = sub nsw i32 0, %157
  %159 = call i32 @dx.op.binary.i32(i32 37, i32 %157, i32 %158)  ; IMax(a,b)
  %160 = lshr i32 %107, 8
  %161 = and i32 %160, 255
  %162 = lshr i32 %154, 8
  %163 = and i32 %162, 255
  %164 = sub nsw i32 %161, %163
  %165 = sub nsw i32 0, %164
  %166 = call i32 @dx.op.binary.i32(i32 37, i32 %164, i32 %165)  ; IMax(a,b)
  %167 = add nsw i32 %166, %159
  %168 = lshr i32 %107, 16
  %169 = and i32 %168, 255
  %170 = lshr i32 %154, 16
  %171 = and i32 %170, 255
  %172 = sub nsw i32 %169, %171
  %173 = sub nsw i32 0, %172
  %174 = call i32 @dx.op.binary.i32(i32 37, i32 %172, i32 %173)  ; IMax(a,b)
  %175 = add nsw i32 %167, %174
  %176 = lshr i32 %107, 24
  %177 = lshr i32 %154, 24
  %178 = sub nsw i32 %176, %177
  %179 = sub nsw i32 0, %178
  %180 = call i32 @dx.op.binary.i32(i32 37, i32 %178, i32 %179)  ; IMax(a,b)
  %181 = add nsw i32 %175, %180
  %182 = shl i32 %6, 1
  %183 = shl i32 %7, 1
  br label %184

; <label>:184                                     ; preds = %596, %153
  %185 = phi i32 [ 0, %153 ], [ %597, %596 ]
  br label %186

; <label>:186                                     ; preds = %584, %184
  %187 = phi i32 [ 0, %184 ], [ %594, %584 ]
  %188 = add nsw i32 %187, %182
  %189 = add nsw i32 %185, %183
  %190 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %2, i32 undef, i32 %188, i32 %189, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %191 = extractvalue %dx.types.ResRet.i32 %190, 0
  %192 = extractvalue %dx.types.ResRet.i32 %190, 1
  %193 = select i1 %45, i32 %191, i32 0
  %194 = select i1 %45, i32 %192, i32 0
  %195 = shl i32 %185, 1
  %196 = add nuw nsw i32 %187, %195
  %197 = icmp eq i32 %16, %196
  br i1 %197, label %198, label %204

; <label>:198                                     ; preds = %186
  %199 = and i32 %8, 1
  %200 = and i32 %10, 7
  %201 = mul i32 %200, 2
  %202 = add i32 %199, %201
  %203 = getelementptr [16 x i32], [16 x i32] addrspace(3)* @"\01?pixels@@3PAY01$$CAIA.1dim", i32 0, i32 %202
  store i32 %107, i32 addrspace(3)* %203, align 4, !tbaa !18
  br label %204

; <label>:204                                     ; preds = %198, %186
  %205 = shl i32 %187, 3
  %206 = shl i32 %185, 3
  %207 = icmp slt i32 %8, 144
  br i1 %207, label %208, label %291

; <label>:208                                     ; preds = %204
  br label %209

; <label>:209                                     ; preds = %284, %208
  %210 = phi i32 [ %288, %284 ], [ %8, %208 ]
  %211 = srem i32 %210, 6
  %212 = sdiv i32 %210, 6
  %213 = shl nsw i32 %211, 2
  %214 = add i32 %193, -8
  %215 = add i32 %214, %17
  %216 = add i32 %215, %205
  %217 = add i32 %216, %213
  %218 = add i32 %194, -8
  %219 = add i32 %218, %18
  %220 = add i32 %219, %206
  %221 = add i32 %220, %212
  %222 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %5, i32 0)  ; CBufferLoadLegacy(handle,regIndex)
  %223 = extractvalue %dx.types.CBufRet.i32 %222, 0
  %224 = extractvalue %dx.types.CBufRet.i32 %222, 2
  %225 = and i32 %224, 31
  %226 = ashr i32 %223, %225
  %227 = extractvalue %dx.types.CBufRet.i32 %222, 1
  %228 = ashr i32 %227, %225
  %229 = add nsw i32 %226, -4
  %230 = call i32 @dx.op.binary.i32(i32 38, i32 %229, i32 %217)  ; IMin(a,b)
  %231 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %230)  ; IMax(a,b)
  %232 = add nsw i32 %228, -1
  %233 = call i32 @dx.op.binary.i32(i32 38, i32 %232, i32 %221)  ; IMin(a,b)
  %234 = call i32 @dx.op.binary.i32(i32 37, i32 0, i32 %233)  ; IMax(a,b)
  %235 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %3, i32 0, i32 %231, i32 %234, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %236 = extractvalue %dx.types.ResRet.i32 %235, 0
  %237 = add i32 %231, 1
  %238 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %3, i32 0, i32 %237, i32 %234, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %239 = extractvalue %dx.types.ResRet.i32 %238, 0
  %240 = add i32 %231, 2
  %241 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %3, i32 0, i32 %240, i32 %234, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %242 = extractvalue %dx.types.ResRet.i32 %241, 0
  %243 = add i32 %231, 3
  %244 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %3, i32 0, i32 %243, i32 %234, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %245 = extractvalue %dx.types.ResRet.i32 %244, 0
  %246 = shl i32 %239, 8
  %247 = or i32 %246, %236
  %248 = shl i32 %242, 16
  %249 = or i32 %247, %248
  %250 = shl i32 %245, 24
  %251 = or i32 %250, %249
  %252 = icmp slt i32 %217, 0
  br i1 %252, label %253, label %265

; <label>:253                                     ; preds = %209
  %254 = and i32 %236, 255
  %255 = shl i32 %249, 8
  %256 = or i32 %255, %254
  %257 = icmp slt i32 %217, -1
  %258 = shl i32 %256, 8
  %259 = or i32 %258, %254
  %260 = select i1 %257, i32 %259, i32 %256
  %261 = icmp slt i32 %217, -2
  br i1 %261, label %262, label %284

; <label>:262                                     ; preds = %253
  %263 = shl i32 %260, 8
  %264 = or i32 %263, %254
  br label %284

; <label>:265                                     ; preds = %209
  %266 = icmp sgt i32 %217, %229
  br i1 %266, label %267, label %284

; <label>:267                                     ; preds = %265
  %268 = and i32 %251, -16777216
  %269 = add nsw i32 %226, -3
  %270 = icmp sge i32 %217, %269
  %271 = lshr i32 %251, 8
  %272 = or i32 %271, %268
  %273 = select i1 %270, i32 %272, i32 %251
  %274 = add nsw i32 %226, -2
  %275 = icmp sge i32 %217, %274
  %276 = lshr i32 %273, 8
  %277 = or i32 %276, %268
  %278 = select i1 %275, i32 %277, i32 %273
  %279 = add nsw i32 %226, -1
  %280 = icmp slt i32 %217, %279
  br i1 %280, label %284, label %281

; <label>:281                                     ; preds = %267
  %282 = lshr i32 %278, 8
  %283 = or i32 %282, %268
  br label %284

; <label>:284                                     ; preds = %281, %267, %265, %262, %253
  %285 = phi i32 [ %264, %262 ], [ %260, %253 ], [ %283, %281 ], [ %278, %267 ], [ %251, %265 ]
  %286 = add i32 %210, 0
  %287 = getelementptr [144 x i32], [144 x i32] addrspace(3)* @"\01?searchBuffer@@3PAY0JA@$$CAIA.1dim", i32 0, i32 %286
  store i32 %285, i32 addrspace(3)* %287, align 4, !tbaa !18
  %288 = add nsw i32 %210, 64
  %289 = icmp slt i32 %288, 144
  br i1 %289, label %209, label %290

; <label>:290                                     ; preds = %284
  br label %291

; <label>:291                                     ; preds = %290, %204
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  br label %292

; <label>:292                                     ; preds = %292, %291
  %293 = phi i32 [ 0, %291 ], [ %452, %292 ]
  %294 = phi i32 [ 0, %291 ], [ %460, %292 ]
  %295 = phi i32 [ 0, %291 ], [ %468, %292 ]
  %296 = phi i32 [ 0, %291 ], [ %476, %292 ]
  %297 = phi i32 [ 0, %291 ], [ %477, %292 ]
  %298 = add nuw nsw i32 %297, %11
  %299 = mul nsw i32 %298, 6
  %300 = add nsw i32 %299, %9
  %301 = add i32 %300, 0
  %302 = getelementptr [144 x i32], [144 x i32] addrspace(3)* @"\01?searchBuffer@@3PAY0JA@$$CAIA.1dim", i32 0, i32 %301
  %303 = load i32, i32 addrspace(3)* %302, align 4, !tbaa !18
  %304 = add nsw i32 %300, 1
  %305 = add i32 %304, 0
  %306 = getelementptr [144 x i32], [144 x i32] addrspace(3)* @"\01?searchBuffer@@3PAY0JA@$$CAIA.1dim", i32 0, i32 %305
  %307 = load i32, i32 addrspace(3)* %306, align 4, !tbaa !18
  %308 = add nsw i32 %300, 2
  %309 = add i32 %308, 0
  %310 = getelementptr [144 x i32], [144 x i32] addrspace(3)* @"\01?searchBuffer@@3PAY0JA@$$CAIA.1dim", i32 0, i32 %309
  %311 = load i32, i32 addrspace(3)* %310, align 4, !tbaa !18
  %312 = mul i32 %297, 2
  %313 = add i32 0, %312
  %314 = getelementptr [16 x i32], [16 x i32] addrspace(3)* @"\01?pixels@@3PAY01$$CAIA.1dim", i32 0, i32 %313
  %315 = load i32, i32 addrspace(3)* %314, align 4, !tbaa !18
  %316 = and i32 %303, 255
  %317 = and i32 %315, 255
  %318 = sub nsw i32 %316, %317
  %319 = sub nsw i32 0, %318
  %320 = call i32 @dx.op.binary.i32(i32 37, i32 %318, i32 %319)  ; IMax(a,b)
  %321 = lshr i32 %303, 8
  %322 = and i32 %321, 255
  %323 = lshr i32 %315, 8
  %324 = and i32 %323, 255
  %325 = sub nsw i32 %322, %324
  %326 = sub nsw i32 0, %325
  %327 = call i32 @dx.op.binary.i32(i32 37, i32 %325, i32 %326)  ; IMax(a,b)
  %328 = lshr i32 %303, 16
  %329 = and i32 %328, 255
  %330 = lshr i32 %315, 16
  %331 = and i32 %330, 255
  %332 = sub nsw i32 %329, %331
  %333 = sub nsw i32 0, %332
  %334 = call i32 @dx.op.binary.i32(i32 37, i32 %332, i32 %333)  ; IMax(a,b)
  %335 = lshr i32 %303, 24
  %336 = lshr i32 %315, 24
  %337 = sub nsw i32 %335, %336
  %338 = sub nsw i32 0, %337
  %339 = call i32 @dx.op.binary.i32(i32 37, i32 %337, i32 %338)  ; IMax(a,b)
  %340 = lshr i32 %307, 8
  %341 = sub nsw i32 %322, %317
  %342 = sub nsw i32 0, %341
  %343 = call i32 @dx.op.binary.i32(i32 37, i32 %341, i32 %342)  ; IMax(a,b)
  %344 = sub nsw i32 %329, %324
  %345 = sub nsw i32 0, %344
  %346 = call i32 @dx.op.binary.i32(i32 37, i32 %344, i32 %345)  ; IMax(a,b)
  %347 = sub nsw i32 %335, %331
  %348 = sub nsw i32 0, %347
  %349 = call i32 @dx.op.binary.i32(i32 37, i32 %347, i32 %348)  ; IMax(a,b)
  %350 = and i32 %307, 255
  %351 = sub nsw i32 %350, %336
  %352 = sub nsw i32 0, %351
  %353 = call i32 @dx.op.binary.i32(i32 37, i32 %351, i32 %352)  ; IMax(a,b)
  %354 = lshr i32 %307, 16
  %355 = sub nsw i32 %329, %317
  %356 = sub nsw i32 0, %355
  %357 = call i32 @dx.op.binary.i32(i32 37, i32 %355, i32 %356)  ; IMax(a,b)
  %358 = sub nsw i32 %335, %324
  %359 = sub nsw i32 0, %358
  %360 = call i32 @dx.op.binary.i32(i32 37, i32 %358, i32 %359)  ; IMax(a,b)
  %361 = sub nsw i32 %350, %331
  %362 = sub nsw i32 0, %361
  %363 = call i32 @dx.op.binary.i32(i32 37, i32 %361, i32 %362)  ; IMax(a,b)
  %364 = and i32 %340, 255
  %365 = sub nsw i32 %364, %336
  %366 = sub nsw i32 0, %365
  %367 = call i32 @dx.op.binary.i32(i32 37, i32 %365, i32 %366)  ; IMax(a,b)
  %368 = sub nsw i32 %335, %317
  %369 = sub nsw i32 0, %368
  %370 = call i32 @dx.op.binary.i32(i32 37, i32 %368, i32 %369)  ; IMax(a,b)
  %371 = sub nsw i32 %350, %324
  %372 = sub nsw i32 0, %371
  %373 = call i32 @dx.op.binary.i32(i32 37, i32 %371, i32 %372)  ; IMax(a,b)
  %374 = sub nsw i32 %364, %331
  %375 = sub nsw i32 0, %374
  %376 = call i32 @dx.op.binary.i32(i32 37, i32 %374, i32 %375)  ; IMax(a,b)
  %377 = and i32 %354, 255
  %378 = sub nsw i32 %377, %336
  %379 = sub nsw i32 0, %378
  %380 = call i32 @dx.op.binary.i32(i32 37, i32 %378, i32 %379)  ; IMax(a,b)
  %381 = mul i32 %297, 2
  %382 = add i32 1, %381
  %383 = getelementptr [16 x i32], [16 x i32] addrspace(3)* @"\01?pixels@@3PAY01$$CAIA.1dim", i32 0, i32 %382
  %384 = load i32, i32 addrspace(3)* %383, align 4, !tbaa !18
  %385 = and i32 %384, 255
  %386 = sub nsw i32 %350, %385
  %387 = sub nsw i32 0, %386
  %388 = call i32 @dx.op.binary.i32(i32 37, i32 %386, i32 %387)  ; IMax(a,b)
  %389 = lshr i32 %384, 8
  %390 = and i32 %389, 255
  %391 = sub nsw i32 %364, %390
  %392 = sub nsw i32 0, %391
  %393 = call i32 @dx.op.binary.i32(i32 37, i32 %391, i32 %392)  ; IMax(a,b)
  %394 = lshr i32 %384, 16
  %395 = and i32 %394, 255
  %396 = sub nsw i32 %377, %395
  %397 = sub nsw i32 0, %396
  %398 = call i32 @dx.op.binary.i32(i32 37, i32 %396, i32 %397)  ; IMax(a,b)
  %399 = lshr i32 %307, 24
  %400 = lshr i32 %384, 24
  %401 = sub nsw i32 %399, %400
  %402 = sub nsw i32 0, %401
  %403 = call i32 @dx.op.binary.i32(i32 37, i32 %401, i32 %402)  ; IMax(a,b)
  %404 = lshr i32 %311, 8
  %405 = sub nsw i32 %364, %385
  %406 = sub nsw i32 0, %405
  %407 = call i32 @dx.op.binary.i32(i32 37, i32 %405, i32 %406)  ; IMax(a,b)
  %408 = sub nsw i32 %377, %390
  %409 = sub nsw i32 0, %408
  %410 = call i32 @dx.op.binary.i32(i32 37, i32 %408, i32 %409)  ; IMax(a,b)
  %411 = sub nsw i32 %399, %395
  %412 = sub nsw i32 0, %411
  %413 = call i32 @dx.op.binary.i32(i32 37, i32 %411, i32 %412)  ; IMax(a,b)
  %414 = and i32 %311, 255
  %415 = sub nsw i32 %414, %400
  %416 = sub nsw i32 0, %415
  %417 = call i32 @dx.op.binary.i32(i32 37, i32 %415, i32 %416)  ; IMax(a,b)
  %418 = lshr i32 %311, 16
  %419 = sub nsw i32 %377, %385
  %420 = sub nsw i32 0, %419
  %421 = call i32 @dx.op.binary.i32(i32 37, i32 %419, i32 %420)  ; IMax(a,b)
  %422 = sub nsw i32 %399, %390
  %423 = sub nsw i32 0, %422
  %424 = call i32 @dx.op.binary.i32(i32 37, i32 %422, i32 %423)  ; IMax(a,b)
  %425 = sub nsw i32 %414, %395
  %426 = sub nsw i32 0, %425
  %427 = call i32 @dx.op.binary.i32(i32 37, i32 %425, i32 %426)  ; IMax(a,b)
  %428 = and i32 %404, 255
  %429 = sub nsw i32 %428, %400
  %430 = sub nsw i32 0, %429
  %431 = call i32 @dx.op.binary.i32(i32 37, i32 %429, i32 %430)  ; IMax(a,b)
  %432 = sub nsw i32 %399, %385
  %433 = sub nsw i32 0, %432
  %434 = call i32 @dx.op.binary.i32(i32 37, i32 %432, i32 %433)  ; IMax(a,b)
  %435 = sub nsw i32 %414, %390
  %436 = sub nsw i32 0, %435
  %437 = call i32 @dx.op.binary.i32(i32 37, i32 %435, i32 %436)  ; IMax(a,b)
  %438 = sub nsw i32 %428, %395
  %439 = sub nsw i32 0, %438
  %440 = call i32 @dx.op.binary.i32(i32 37, i32 %438, i32 %439)  ; IMax(a,b)
  %441 = and i32 %418, 255
  %442 = sub nsw i32 %441, %400
  %443 = sub nsw i32 0, %442
  %444 = call i32 @dx.op.binary.i32(i32 37, i32 %442, i32 %443)  ; IMax(a,b)
  %445 = add i32 %320, %293
  %446 = add i32 %445, %327
  %447 = add i32 %446, %334
  %448 = add i32 %447, %339
  %449 = add i32 %448, %388
  %450 = add i32 %449, %393
  %451 = add i32 %450, %398
  %452 = add i32 %451, %403
  %453 = add i32 %343, %294
  %454 = add i32 %453, %346
  %455 = add i32 %454, %349
  %456 = add i32 %455, %353
  %457 = add i32 %456, %407
  %458 = add i32 %457, %410
  %459 = add i32 %458, %413
  %460 = add i32 %459, %417
  %461 = add i32 %357, %295
  %462 = add i32 %461, %360
  %463 = add i32 %462, %363
  %464 = add i32 %463, %367
  %465 = add i32 %464, %421
  %466 = add i32 %465, %424
  %467 = add i32 %466, %427
  %468 = add i32 %467, %431
  %469 = add i32 %370, %296
  %470 = add i32 %469, %373
  %471 = add i32 %470, %376
  %472 = add i32 %471, %380
  %473 = add i32 %472, %434
  %474 = add i32 %473, %437
  %475 = add i32 %474, %440
  %476 = add i32 %475, %444
  %477 = add nuw nsw i32 %297, 1
  %478 = icmp eq i32 %477, 8
  br i1 %478, label %479, label %292

; <label>:479                                     ; preds = %292
  %480 = shl i32 %452, 16
  %481 = add nsw i32 %19, -8
  %482 = add nsw i32 %11, -8
  %483 = ashr i32 %481, 31
  %484 = ashr i32 %482, 31
  %485 = add nsw i32 %483, %481
  %486 = add nsw i32 %484, %482
  %487 = xor i32 %485, %483
  %488 = xor i32 %486, %484
  %489 = shl nsw i32 %488, 12
  %490 = shl nsw i32 %487, 8
  %491 = shl nuw nsw i32 %11, 4
  %492 = or i32 %489, %491
  %493 = or i32 %492, %19
  %494 = or i32 %493, %490
  %495 = or i32 %494, %480
  %496 = add i32 %11, 0
  %497 = mul i32 %496, 5
  %498 = add i32 %9, %497
  %499 = getelementptr [320 x i32], [320 x i32] addrspace(3)* @"\01?sadMapBuffer@@3PAY1BA@4$$CAIA.1dim", i32 0, i32 %498
  store i32 %495, i32 addrspace(3)* %499, align 4, !tbaa !18
  %500 = shl i32 %460, 16
  %501 = or i32 %19, 1
  %502 = add nsw i32 %501, -8
  %503 = ashr i32 %502, 31
  %504 = add nsw i32 %503, %502
  %505 = xor i32 %504, %503
  %506 = shl nsw i32 %505, 8
  %507 = or i32 %492, %501
  %508 = or i32 %507, %506
  %509 = or i32 %508, %500
  %510 = add i32 %11, 16
  %511 = mul i32 %510, 5
  %512 = add i32 %9, %511
  %513 = getelementptr [320 x i32], [320 x i32] addrspace(3)* @"\01?sadMapBuffer@@3PAY1BA@4$$CAIA.1dim", i32 0, i32 %512
  store i32 %509, i32 addrspace(3)* %513, align 4, !tbaa !18
  %514 = shl i32 %468, 16
  %515 = or i32 %19, 2
  %516 = add nsw i32 %515, -8
  %517 = ashr i32 %516, 31
  %518 = add nsw i32 %517, %516
  %519 = xor i32 %518, %517
  %520 = shl nsw i32 %519, 8
  %521 = or i32 %492, %515
  %522 = or i32 %521, %520
  %523 = or i32 %522, %514
  %524 = add i32 %11, 32
  %525 = mul i32 %524, 5
  %526 = add i32 %9, %525
  %527 = getelementptr [320 x i32], [320 x i32] addrspace(3)* @"\01?sadMapBuffer@@3PAY1BA@4$$CAIA.1dim", i32 0, i32 %526
  store i32 %523, i32 addrspace(3)* %527, align 4, !tbaa !18
  %528 = shl i32 %476, 16
  %529 = or i32 %19, 3
  %530 = add nsw i32 %529, -8
  %531 = ashr i32 %530, 31
  %532 = add nsw i32 %531, %530
  %533 = xor i32 %532, %531
  %534 = shl nsw i32 %533, 8
  %535 = or i32 %492, %529
  %536 = or i32 %535, %534
  %537 = or i32 %536, %528
  %538 = add i32 %11, 48
  %539 = mul i32 %538, 5
  %540 = add i32 %9, %539
  %541 = getelementptr [320 x i32], [320 x i32] addrspace(3)* @"\01?sadMapBuffer@@3PAY1BA@4$$CAIA.1dim", i32 0, i32 %540
  store i32 %537, i32 addrspace(3)* %541, align 4, !tbaa !18
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %542 = load i32, i32 addrspace(3)* %513, align 4, !tbaa !18
  %543 = load i32, i32 addrspace(3)* %499, align 4, !tbaa !18
  %544 = call i32 @dx.op.binary.i32(i32 40, i32 %543, i32 %542)  ; UMin(a,b)
  %545 = load i32, i32 addrspace(3)* %541, align 4, !tbaa !18
  %546 = load i32, i32 addrspace(3)* %527, align 4, !tbaa !18
  %547 = call i32 @dx.op.binary.i32(i32 40, i32 %546, i32 %545)  ; UMin(a,b)
  %548 = call i32 @dx.op.binary.i32(i32 40, i32 %544, i32 %547)  ; UMin(a,b)
  %549 = call i32 @dx.op.waveActiveOp.i32(i32 119, i32 %548, i8 2, i8 1)  ; WaveActiveOp(value,op,sop)
  %550 = call i32 @dx.op.waveGetLaneCount(i32 112)  ; WaveGetLaneCount()
  %551 = icmp eq i32 %550, 32
  br i1 %551, label %552, label %562

; <label>:552                                     ; preds = %479
  %553 = ashr i32 %8, 5
  %554 = call i1 @dx.op.waveIsFirstLane(i32 110)  ; WaveIsFirstLane()
  br i1 %554, label %555, label %557

; <label>:555                                     ; preds = %552
  %556 = getelementptr [2 x i32], [2 x i32] addrspace(3)* @"\01?sWaveMin@@3PAIA", i32 0, i32 %553
  store i32 %549, i32 addrspace(3)* %556, align 4, !tbaa !18
  br label %557

; <label>:557                                     ; preds = %555, %552
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %558 = xor i32 %553, 1
  %559 = getelementptr [2 x i32], [2 x i32] addrspace(3)* @"\01?sWaveMin@@3PAIA", i32 0, i32 %558
  %560 = load i32, i32 addrspace(3)* %559, align 4, !tbaa !18
  %561 = call i32 @dx.op.binary.i32(i32 40, i32 %549, i32 %560)  ; UMin(a,b)
  br label %562

; <label>:562                                     ; preds = %557, %479
  %563 = phi i32 [ %561, %557 ], [ %549, %479 ]
  %564 = and i32 %563, 15
  %565 = lshr i32 %563, 4
  %566 = and i32 %565, 15
  %567 = add i32 %193, -8
  %568 = add i32 %567, %564
  %569 = add i32 %194, -8
  %570 = add i32 %569, %566
  %571 = icmp ne i32 %16, %196
  %572 = select i1 %571, i32 0, i32 %181
  %573 = call i32 @dx.op.waveActiveOp.i32(i32 119, i32 %572, i8 0, i8 1)  ; WaveActiveOp(value,op,sop)
  br i1 %551, label %574, label %584

; <label>:574                                     ; preds = %562
  %575 = ashr i32 %8, 5
  %576 = call i1 @dx.op.waveIsFirstLane(i32 110)  ; WaveIsFirstLane()
  br i1 %576, label %577, label %579

; <label>:577                                     ; preds = %574
  %578 = getelementptr [2 x i32], [2 x i32] addrspace(3)* @"\01?sWaveSad@@3PAIA", i32 0, i32 %575
  store i32 %573, i32 addrspace(3)* %578, align 4, !tbaa !18
  br label %579

; <label>:579                                     ; preds = %577, %574
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %580 = xor i32 %575, 1
  %581 = getelementptr [2 x i32], [2 x i32] addrspace(3)* @"\01?sWaveSad@@3PAIA", i32 0, i32 %580
  %582 = load i32, i32 addrspace(3)* %581, align 4, !tbaa !18
  %583 = add i32 %582, %573
  br label %584

; <label>:584                                     ; preds = %579, %562
  %585 = phi i32 [ %583, %579 ], [ %573, %562 ]
  %586 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %5, i32 0)  ; CBufferLoadLegacy(handle,regIndex)
  %587 = extractvalue %dx.types.CBufRet.i32 %586, 2
  %588 = icmp eq i32 %587, 0
  %589 = lshr i32 %563, 16
  %590 = icmp ule i32 %585, %589
  %591 = and i1 %590, %588
  %592 = select i1 %591, i32 0, i32 %568
  %593 = select i1 %591, i32 0, i32 %570
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %2, i32 %188, i32 %189, i32 undef, i32 %592, i32 %593, i32 %592, i32 %592, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  %594 = add nuw nsw i32 %187, 1
  %595 = icmp eq i32 %594, 2
  br i1 %595, label %596, label %186

; <label>:596                                     ; preds = %584
  %597 = add nuw nsw i32 %185, 1
  %598 = icmp eq i32 %597, 2
  br i1 %598, label %599, label %184

; <label>:599                                     ; preds = %596
  br label %600

; <label>:600                                     ; preds = %599, %39, %34
  ret void
}

; Function Attrs: nounwind readnone
declare i32 @dx.op.groupId.i32(i32, i32) #0

; Function Attrs: nounwind readnone
declare i32 @dx.op.flattenedThreadIdInGroup.i32(i32) #0

; Function Attrs: nounwind readonly
declare %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32) #1

; Function Attrs: nounwind
declare void @dx.op.textureStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32, i8) #2

; Function Attrs: nounwind readnone
declare i32 @dx.op.binary.i32(i32, i32, i32) #0

; Function Attrs: noduplicate nounwind
declare void @dx.op.barrier(i32, i32) #3

; Function Attrs: nounwind
declare i32 @dx.op.waveActiveOp.i32(i32, i32, i8, i8) #2

; Function Attrs: nounwind readnone
declare i32 @dx.op.waveGetLaneCount(i32) #0

; Function Attrs: nounwind
declare i1 @dx.op.waveIsFirstLane(i32) #2

; Function Attrs: nounwind readonly
declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32) #1

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #1

attributes #0 = { nounwind readnone }
attributes #1 = { nounwind readonly }
attributes #2 = { nounwind }
attributes #3 = { noduplicate nounwind }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!2}
!dx.shaderModel = !{!3}
!dx.resources = !{!4}
!dx.entryPoints = !{!15}

!0 = !{!"dxcoob 1.8.2502.11 (239921522)"}
!1 = !{i32 1, i32 2}
!2 = !{i32 1, i32 8}
!3 = !{!"cs", i32 6, i32 2}
!4 = !{!5, !9, !13, null}
!5 = !{!6, !8}
!6 = !{i32 0, %"class.Texture2D<unsigned int>"* undef, !"", i32 0, i32 0, i32 1, i32 2, i32 0, !7}
!7 = !{i32 0, i32 5}
!8 = !{i32 1, %"class.Texture2D<unsigned int>"* undef, !"", i32 0, i32 1, i32 1, i32 2, i32 0, !7}
!9 = !{!10, !12}
!10 = !{i32 0, %"class.RWTexture2D<vector<int, 2> >"* undef, !"", i32 0, i32 0, i32 1, i32 2, i1 false, i1 false, i1 false, !11}
!11 = !{i32 0, i32 4}
!12 = !{i32 1, %"class.RWTexture2D<unsigned int>"* undef, !"", i32 0, i32 1, i32 1, i32 2, i1 false, i1 false, i1 false, !7}
!13 = !{!14}
!14 = !{i32 0, %cbOF* undef, !"", i32 0, i32 0, i32 1, i32 32, null}
!15 = !{void ()* @CS, !"CS", null, !4, !16}
!16 = !{i32 0, i64 532480, i32 4, !17}
!17 = !{i32 64, i32 1, i32 1}
!18 = !{!19, !19, i64 0}
!19 = !{!"int", !20, i64 0}
!20 = !{!"omnipotent char", !21, i64 0}
!21 = !{!"Simple C/C++ TBAA"}
