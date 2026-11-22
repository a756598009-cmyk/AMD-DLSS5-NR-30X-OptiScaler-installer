;
; Note: shader requires additional functionality:
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
; shader hash: c311d2cec1f73a5492fb9ab662a3f4ef
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
; rw_optical_flow_input                 UAV     u32          2d      U0             u0     1
; rw_optical_flow_input_level_1         UAV     u32          2d      U1             u1     1
; rw_optical_flow_input_level_2         UAV     u32          2d      U2             u2     1
; rw_optical_flow_input_level_3         UAV     u32          2d      U3             u3     1
; rw_optical_flow_input_level_4         UAV     u32          2d      U4             u4     1
; rw_optical_flow_input_level_5         UAV     u32          2d      U5             u5     1
; rw_optical_flow_input_level_6         UAV     u32          2d      U6             u6     1
;
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }
%"class.RWTexture2D<unsigned int>" = type { i32 }

@"\01?spdIntermediateR@@3PAY0BA@$$CAMA.1dim" = addrspace(3) global [256 x float] undef, align 4
@"\01?spdIntermediateG@@3PAY0BA@$$CAMA.1dim" = addrspace(3) global [256 x float] undef, align 4
@"\01?spdIntermediateB@@3PAY0BA@$$CAMA.1dim" = addrspace(3) global [256 x float] undef, align 4
@"\01?spdIntermediateA@@3PAY0BA@$$CAMA.1dim" = addrspace(3) global [256 x float] undef, align 4

define void @CS() {
  %1 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 6, i32 6, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %2 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 5, i32 5, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %3 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 4, i32 4, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %4 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 3, i32 3, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %5 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 2, i32 2, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %6 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 1, i32 1, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %7 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %8 = call i32 @dx.op.groupId.i32(i32 94, i32 0)  ; GroupId(component)
  %9 = call i32 @dx.op.groupId.i32(i32 94, i32 1)  ; GroupId(component)
  %10 = call i32 @dx.op.flattenedThreadIdInGroup.i32(i32 96)  ; FlattenedThreadIdInGroup()
  %11 = lshr i32 %10, 2
  %12 = and i32 %10, 1
  %13 = and i32 %11, 6
  %14 = or i32 %13, %12
  %15 = lshr i32 %10, 1
  %16 = and i32 %15, 3
  %17 = lshr i32 %10, 3
  %18 = and i32 %17, 4
  %19 = or i32 %16, %18
  %20 = and i32 %17, 8
  %21 = or i32 %14, %20
  %22 = lshr i32 %10, 7
  %23 = shl nuw nsw i32 %22, 3
  %24 = or i32 %19, %23
  %25 = shl i32 %8, 6
  %26 = shl i32 %9, 6
  %27 = shl nuw nsw i32 %21, 1
  %28 = shl nuw nsw i32 %24, 1
  %29 = or i32 %27, %25
  %30 = add i32 %28, %26
  %31 = shl i32 %8, 5
  %32 = shl i32 %9, 5
  %33 = or i32 %21, %31
  %34 = add i32 %24, %32
  %35 = or i32 %29, 1
  %36 = or i32 %30, 1
  %37 = sitofp i32 %29 to float
  %38 = sitofp i32 %30 to float
  %39 = fptosi float %37 to i32
  %40 = fptosi float %38 to i32
  %41 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %39, i32 %40, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %42 = extractvalue %dx.types.ResRet.i32 %41, 0
  %43 = uitofp i32 %42 to float
  %44 = sitofp i32 %36 to float
  %45 = fptosi float %44 to i32
  %46 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %39, i32 %45, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %47 = extractvalue %dx.types.ResRet.i32 %46, 0
  %48 = uitofp i32 %47 to float
  %49 = sitofp i32 %35 to float
  %50 = fptosi float %49 to i32
  %51 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %50, i32 %40, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %52 = extractvalue %dx.types.ResRet.i32 %51, 0
  %53 = uitofp i32 %52 to float
  %54 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %50, i32 %45, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %55 = extractvalue %dx.types.ResRet.i32 %54, 0
  %56 = uitofp i32 %55 to float
  %57 = fadd fast float %48, %43
  %58 = fadd fast float %57, %53
  %59 = fadd fast float %58, %56
  %60 = fmul fast float %59, 2.500000e-01
  %61 = fptoui float %60 to i32
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %6, i32 %33, i32 %34, i32 undef, i32 %61, i32 %61, i32 %61, i32 %61, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  %62 = or i32 %29, 32
  %63 = or i32 %33, 16
  %64 = or i32 %29, 33
  %65 = sitofp i32 %62 to float
  %66 = fptosi float %65 to i32
  %67 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %66, i32 %40, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %68 = extractvalue %dx.types.ResRet.i32 %67, 0
  %69 = uitofp i32 %68 to float
  %70 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %66, i32 %45, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %71 = extractvalue %dx.types.ResRet.i32 %70, 0
  %72 = uitofp i32 %71 to float
  %73 = sitofp i32 %64 to float
  %74 = fptosi float %73 to i32
  %75 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %74, i32 %40, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %76 = extractvalue %dx.types.ResRet.i32 %75, 0
  %77 = uitofp i32 %76 to float
  %78 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %74, i32 %45, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %79 = extractvalue %dx.types.ResRet.i32 %78, 0
  %80 = uitofp i32 %79 to float
  %81 = fadd fast float %72, %69
  %82 = fadd fast float %81, %77
  %83 = fadd fast float %82, %80
  %84 = fmul fast float %83, 2.500000e-01
  %85 = fptoui float %84 to i32
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %6, i32 %63, i32 %34, i32 undef, i32 %85, i32 %85, i32 %85, i32 %85, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  %86 = or i32 %26, 32
  %87 = add i32 %28, %86
  %88 = add i32 %34, 16
  %89 = or i32 %87, 1
  %90 = sitofp i32 %87 to float
  %91 = fptosi float %90 to i32
  %92 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %39, i32 %91, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %93 = extractvalue %dx.types.ResRet.i32 %92, 0
  %94 = uitofp i32 %93 to float
  %95 = sitofp i32 %89 to float
  %96 = fptosi float %95 to i32
  %97 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %39, i32 %96, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %98 = extractvalue %dx.types.ResRet.i32 %97, 0
  %99 = uitofp i32 %98 to float
  %100 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %50, i32 %91, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %101 = extractvalue %dx.types.ResRet.i32 %100, 0
  %102 = uitofp i32 %101 to float
  %103 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %50, i32 %96, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %104 = extractvalue %dx.types.ResRet.i32 %103, 0
  %105 = uitofp i32 %104 to float
  %106 = fadd fast float %99, %94
  %107 = fadd fast float %106, %102
  %108 = fadd fast float %107, %105
  %109 = fmul fast float %108, 2.500000e-01
  %110 = fptoui float %109 to i32
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %6, i32 %33, i32 %88, i32 undef, i32 %110, i32 %110, i32 %110, i32 %110, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  %111 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %66, i32 %91, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %112 = extractvalue %dx.types.ResRet.i32 %111, 0
  %113 = uitofp i32 %112 to float
  %114 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %66, i32 %96, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %115 = extractvalue %dx.types.ResRet.i32 %114, 0
  %116 = uitofp i32 %115 to float
  %117 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %74, i32 %91, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %118 = extractvalue %dx.types.ResRet.i32 %117, 0
  %119 = uitofp i32 %118 to float
  %120 = call %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32 66, %dx.types.Handle %7, i32 undef, i32 %74, i32 %96, i32 undef, i32 undef, i32 undef, i32 undef)  ; TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)
  %121 = extractvalue %dx.types.ResRet.i32 %120, 0
  %122 = uitofp i32 %121 to float
  %123 = fadd fast float %116, %113
  %124 = fadd fast float %123, %119
  %125 = fadd fast float %124, %122
  %126 = fmul fast float %125, 2.500000e-01
  %127 = fptoui float %126 to i32
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %6, i32 %63, i32 %88, i32 undef, i32 %127, i32 %127, i32 %127, i32 %127, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  %128 = call float @dx.op.quadOp.f32(i32 123, float %60, i8 0)  ; QuadOp(value,op)
  %129 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 0)  ; QuadOp(value,op)
  %130 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 0)  ; QuadOp(value,op)
  %131 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 0)  ; QuadOp(value,op)
  %132 = call float @dx.op.quadOp.f32(i32 123, float %60, i8 1)  ; QuadOp(value,op)
  %133 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 1)  ; QuadOp(value,op)
  %134 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 1)  ; QuadOp(value,op)
  %135 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 1)  ; QuadOp(value,op)
  %136 = call float @dx.op.quadOp.f32(i32 123, float %60, i8 2)  ; QuadOp(value,op)
  %137 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 2)  ; QuadOp(value,op)
  %138 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 2)  ; QuadOp(value,op)
  %139 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 2)  ; QuadOp(value,op)
  %140 = fadd fast float %128, %60
  %141 = fadd fast float %140, %132
  %142 = fadd fast float %141, %136
  %143 = fmul fast float %142, 2.500000e-01
  %144 = call float @dx.op.quadOp.f32(i32 123, float %84, i8 0)  ; QuadOp(value,op)
  %145 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 0)  ; QuadOp(value,op)
  %146 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 0)  ; QuadOp(value,op)
  %147 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 0)  ; QuadOp(value,op)
  %148 = call float @dx.op.quadOp.f32(i32 123, float %84, i8 1)  ; QuadOp(value,op)
  %149 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 1)  ; QuadOp(value,op)
  %150 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 1)  ; QuadOp(value,op)
  %151 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 1)  ; QuadOp(value,op)
  %152 = call float @dx.op.quadOp.f32(i32 123, float %84, i8 2)  ; QuadOp(value,op)
  %153 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 2)  ; QuadOp(value,op)
  %154 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 2)  ; QuadOp(value,op)
  %155 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 2)  ; QuadOp(value,op)
  %156 = fadd fast float %144, %84
  %157 = fadd fast float %156, %148
  %158 = fadd fast float %157, %152
  %159 = fmul fast float %158, 2.500000e-01
  %160 = call float @dx.op.quadOp.f32(i32 123, float %109, i8 0)  ; QuadOp(value,op)
  %161 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 0)  ; QuadOp(value,op)
  %162 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 0)  ; QuadOp(value,op)
  %163 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 0)  ; QuadOp(value,op)
  %164 = call float @dx.op.quadOp.f32(i32 123, float %109, i8 1)  ; QuadOp(value,op)
  %165 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 1)  ; QuadOp(value,op)
  %166 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 1)  ; QuadOp(value,op)
  %167 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 1)  ; QuadOp(value,op)
  %168 = call float @dx.op.quadOp.f32(i32 123, float %109, i8 2)  ; QuadOp(value,op)
  %169 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 2)  ; QuadOp(value,op)
  %170 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 2)  ; QuadOp(value,op)
  %171 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 2)  ; QuadOp(value,op)
  %172 = fadd fast float %160, %109
  %173 = fadd fast float %172, %164
  %174 = fadd fast float %173, %168
  %175 = fmul fast float %174, 2.500000e-01
  %176 = call float @dx.op.quadOp.f32(i32 123, float %126, i8 0)  ; QuadOp(value,op)
  %177 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 0)  ; QuadOp(value,op)
  %178 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 0)  ; QuadOp(value,op)
  %179 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 0)  ; QuadOp(value,op)
  %180 = call float @dx.op.quadOp.f32(i32 123, float %126, i8 1)  ; QuadOp(value,op)
  %181 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 1)  ; QuadOp(value,op)
  %182 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 1)  ; QuadOp(value,op)
  %183 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 1)  ; QuadOp(value,op)
  %184 = call float @dx.op.quadOp.f32(i32 123, float %126, i8 2)  ; QuadOp(value,op)
  %185 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 2)  ; QuadOp(value,op)
  %186 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 2)  ; QuadOp(value,op)
  %187 = call float @dx.op.quadOp.f32(i32 123, float 0.000000e+00, i8 2)  ; QuadOp(value,op)
  %188 = fadd fast float %176, %126
  %189 = fadd fast float %188, %180
  %190 = fadd fast float %189, %184
  %191 = fmul fast float %190, 2.500000e-01
  %192 = and i32 %10, 3
  %193 = icmp eq i32 %192, 0
  br i1 %193, label %194, label %294

; <label>:194                                     ; preds = %0
  %195 = fadd fast float %183, %179
  %196 = fadd fast float %195, %187
  %197 = fmul fast float %196, 2.500000e-01
  %198 = fadd fast float %182, %178
  %199 = fadd fast float %198, %186
  %200 = fmul fast float %199, 2.500000e-01
  %201 = fadd fast float %181, %177
  %202 = fadd fast float %201, %185
  %203 = fmul fast float %202, 2.500000e-01
  %204 = fadd fast float %167, %163
  %205 = fadd fast float %204, %171
  %206 = fmul fast float %205, 2.500000e-01
  %207 = fadd fast float %166, %162
  %208 = fadd fast float %207, %170
  %209 = fmul fast float %208, 2.500000e-01
  %210 = fadd fast float %165, %161
  %211 = fadd fast float %210, %169
  %212 = fmul fast float %211, 2.500000e-01
  %213 = fadd fast float %151, %147
  %214 = fadd fast float %213, %155
  %215 = fmul fast float %214, 2.500000e-01
  %216 = fadd fast float %150, %146
  %217 = fadd fast float %216, %154
  %218 = fmul fast float %217, 2.500000e-01
  %219 = fadd fast float %149, %145
  %220 = fadd fast float %219, %153
  %221 = fmul fast float %220, 2.500000e-01
  %222 = fadd fast float %135, %131
  %223 = fadd fast float %222, %139
  %224 = fmul fast float %223, 2.500000e-01
  %225 = fadd fast float %134, %130
  %226 = fadd fast float %225, %138
  %227 = fmul fast float %226, 2.500000e-01
  %228 = fadd fast float %133, %129
  %229 = fadd fast float %228, %137
  %230 = fmul fast float %229, 2.500000e-01
  %231 = shl i32 %8, 4
  %232 = shl i32 %9, 4
  %233 = lshr i32 %21, 1
  %234 = lshr i32 %24, 1
  %235 = or i32 %233, %231
  %236 = add i32 %234, %232
  %237 = fptoui float %143 to i32
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %5, i32 %235, i32 %236, i32 undef, i32 %237, i32 %237, i32 %237, i32 %237, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  %238 = mul i32 %233, 16
  %239 = add i32 %234, %238
  %240 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateR@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %239
  store float %143, float addrspace(3)* %240, align 4, !tbaa !17
  %241 = mul i32 %233, 16
  %242 = add i32 %234, %241
  %243 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateG@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %242
  store float %230, float addrspace(3)* %243, align 4, !tbaa !17
  %244 = mul i32 %233, 16
  %245 = add i32 %234, %244
  %246 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateB@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %245
  store float %227, float addrspace(3)* %246, align 4, !tbaa !17
  %247 = mul i32 %233, 16
  %248 = add i32 %234, %247
  %249 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateA@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %248
  store float %224, float addrspace(3)* %249, align 4, !tbaa !17
  %250 = or i32 %235, 8
  %251 = fptoui float %159 to i32
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %5, i32 %250, i32 %236, i32 undef, i32 %251, i32 %251, i32 %251, i32 %251, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  %252 = or i32 %233, 8
  %253 = mul i32 %252, 16
  %254 = add i32 %234, %253
  %255 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateR@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %254
  store float %159, float addrspace(3)* %255, align 4, !tbaa !17
  %256 = mul i32 %252, 16
  %257 = add i32 %234, %256
  %258 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateG@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %257
  store float %221, float addrspace(3)* %258, align 4, !tbaa !17
  %259 = mul i32 %252, 16
  %260 = add i32 %234, %259
  %261 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateB@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %260
  store float %218, float addrspace(3)* %261, align 4, !tbaa !17
  %262 = mul i32 %252, 16
  %263 = add i32 %234, %262
  %264 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateA@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %263
  store float %215, float addrspace(3)* %264, align 4, !tbaa !17
  %265 = or i32 %232, 8
  %266 = add i32 %234, %265
  %267 = fptoui float %175 to i32
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %5, i32 %235, i32 %266, i32 undef, i32 %267, i32 %267, i32 %267, i32 %267, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  %268 = add nuw nsw i32 %234, 8
  %269 = mul i32 %233, 16
  %270 = add i32 %268, %269
  %271 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateR@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %270
  store float %175, float addrspace(3)* %271, align 4, !tbaa !17
  %272 = mul i32 %233, 16
  %273 = add i32 %268, %272
  %274 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateG@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %273
  store float %212, float addrspace(3)* %274, align 4, !tbaa !17
  %275 = mul i32 %233, 16
  %276 = add i32 %268, %275
  %277 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateB@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %276
  store float %209, float addrspace(3)* %277, align 4, !tbaa !17
  %278 = mul i32 %233, 16
  %279 = add i32 %268, %278
  %280 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateA@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %279
  store float %206, float addrspace(3)* %280, align 4, !tbaa !17
  %281 = fptoui float %191 to i32
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %5, i32 %250, i32 %266, i32 undef, i32 %281, i32 %281, i32 %281, i32 %281, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  %282 = mul i32 %252, 16
  %283 = add i32 %268, %282
  %284 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateR@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %283
  store float %191, float addrspace(3)* %284, align 4, !tbaa !17
  %285 = mul i32 %252, 16
  %286 = add i32 %268, %285
  %287 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateG@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %286
  store float %203, float addrspace(3)* %287, align 4, !tbaa !17
  %288 = mul i32 %252, 16
  %289 = add i32 %268, %288
  %290 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateB@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %289
  store float %200, float addrspace(3)* %290, align 4, !tbaa !17
  %291 = mul i32 %252, 16
  %292 = add i32 %268, %291
  %293 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateA@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %292
  store float %197, float addrspace(3)* %293, align 4, !tbaa !17
  br label %294

; <label>:294                                     ; preds = %194, %0
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %295 = mul i32 %21, 16
  %296 = add i32 %24, %295
  %297 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateR@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %296
  %298 = load float, float addrspace(3)* %297, align 4, !tbaa !17
  %299 = mul i32 %21, 16
  %300 = add i32 %24, %299
  %301 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateG@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %300
  %302 = load float, float addrspace(3)* %301, align 4, !tbaa !17
  %303 = mul i32 %21, 16
  %304 = add i32 %24, %303
  %305 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateB@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %304
  %306 = load float, float addrspace(3)* %305, align 4, !tbaa !17
  %307 = mul i32 %21, 16
  %308 = add i32 %24, %307
  %309 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateA@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %308
  %310 = load float, float addrspace(3)* %309, align 4, !tbaa !17
  %311 = call float @dx.op.quadOp.f32(i32 123, float %298, i8 0)  ; QuadOp(value,op)
  %312 = call float @dx.op.quadOp.f32(i32 123, float %302, i8 0)  ; QuadOp(value,op)
  %313 = call float @dx.op.quadOp.f32(i32 123, float %306, i8 0)  ; QuadOp(value,op)
  %314 = call float @dx.op.quadOp.f32(i32 123, float %310, i8 0)  ; QuadOp(value,op)
  %315 = call float @dx.op.quadOp.f32(i32 123, float %298, i8 1)  ; QuadOp(value,op)
  %316 = call float @dx.op.quadOp.f32(i32 123, float %302, i8 1)  ; QuadOp(value,op)
  %317 = call float @dx.op.quadOp.f32(i32 123, float %306, i8 1)  ; QuadOp(value,op)
  %318 = call float @dx.op.quadOp.f32(i32 123, float %310, i8 1)  ; QuadOp(value,op)
  %319 = call float @dx.op.quadOp.f32(i32 123, float %298, i8 2)  ; QuadOp(value,op)
  %320 = call float @dx.op.quadOp.f32(i32 123, float %302, i8 2)  ; QuadOp(value,op)
  %321 = call float @dx.op.quadOp.f32(i32 123, float %306, i8 2)  ; QuadOp(value,op)
  %322 = call float @dx.op.quadOp.f32(i32 123, float %310, i8 2)  ; QuadOp(value,op)
  %323 = fadd fast float %311, %298
  %324 = fadd fast float %323, %315
  %325 = fadd fast float %324, %319
  %326 = fmul fast float %325, 2.500000e-01
  br i1 %193, label %327, label %361

; <label>:327                                     ; preds = %294
  %328 = fadd fast float %314, %310
  %329 = fadd fast float %328, %318
  %330 = fadd fast float %329, %322
  %331 = fmul fast float %330, 2.500000e-01
  %332 = fadd fast float %313, %306
  %333 = fadd fast float %332, %317
  %334 = fadd fast float %333, %321
  %335 = fmul fast float %334, 2.500000e-01
  %336 = fadd fast float %312, %302
  %337 = fadd fast float %336, %316
  %338 = fadd fast float %337, %320
  %339 = fmul fast float %338, 2.500000e-01
  %340 = shl i32 %8, 3
  %341 = shl i32 %9, 3
  %342 = lshr i32 %21, 1
  %343 = lshr i32 %24, 1
  %344 = or i32 %342, %340
  %345 = add i32 %343, %341
  %346 = fptoui float %326 to i32
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %4, i32 %344, i32 %345, i32 undef, i32 %346, i32 %346, i32 %346, i32 %346, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  %347 = and i32 %11, 1
  %348 = add nuw nsw i32 %21, %347
  %349 = mul i32 %348, 16
  %350 = add i32 %24, %349
  %351 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateR@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %350
  store float %326, float addrspace(3)* %351, align 4, !tbaa !17
  %352 = mul i32 %348, 16
  %353 = add i32 %24, %352
  %354 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateG@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %353
  store float %339, float addrspace(3)* %354, align 4, !tbaa !17
  %355 = mul i32 %348, 16
  %356 = add i32 %24, %355
  %357 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateB@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %356
  store float %335, float addrspace(3)* %357, align 4, !tbaa !17
  %358 = mul i32 %348, 16
  %359 = add i32 %24, %358
  %360 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateA@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %359
  store float %331, float addrspace(3)* %360, align 4, !tbaa !17
  br label %361

; <label>:361                                     ; preds = %327, %294
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %362 = icmp ult i32 %10, 64
  br i1 %362, label %363, label %431

; <label>:363                                     ; preds = %361
  %364 = and i32 %15, 1
  %365 = or i32 %27, %364
  %366 = mul i32 %365, 16
  %367 = add i32 %28, %366
  %368 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateR@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %367
  %369 = load float, float addrspace(3)* %368, align 4, !tbaa !17
  %370 = mul i32 %365, 16
  %371 = add i32 %28, %370
  %372 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateG@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %371
  %373 = load float, float addrspace(3)* %372, align 4, !tbaa !17
  %374 = mul i32 %365, 16
  %375 = add i32 %28, %374
  %376 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateB@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %375
  %377 = load float, float addrspace(3)* %376, align 4, !tbaa !17
  %378 = mul i32 %365, 16
  %379 = add i32 %28, %378
  %380 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateA@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %379
  %381 = load float, float addrspace(3)* %380, align 4, !tbaa !17
  %382 = call float @dx.op.quadOp.f32(i32 123, float %369, i8 0)  ; QuadOp(value,op)
  %383 = call float @dx.op.quadOp.f32(i32 123, float %373, i8 0)  ; QuadOp(value,op)
  %384 = call float @dx.op.quadOp.f32(i32 123, float %377, i8 0)  ; QuadOp(value,op)
  %385 = call float @dx.op.quadOp.f32(i32 123, float %381, i8 0)  ; QuadOp(value,op)
  %386 = call float @dx.op.quadOp.f32(i32 123, float %369, i8 1)  ; QuadOp(value,op)
  %387 = call float @dx.op.quadOp.f32(i32 123, float %373, i8 1)  ; QuadOp(value,op)
  %388 = call float @dx.op.quadOp.f32(i32 123, float %377, i8 1)  ; QuadOp(value,op)
  %389 = call float @dx.op.quadOp.f32(i32 123, float %381, i8 1)  ; QuadOp(value,op)
  %390 = call float @dx.op.quadOp.f32(i32 123, float %369, i8 2)  ; QuadOp(value,op)
  %391 = call float @dx.op.quadOp.f32(i32 123, float %373, i8 2)  ; QuadOp(value,op)
  %392 = call float @dx.op.quadOp.f32(i32 123, float %377, i8 2)  ; QuadOp(value,op)
  %393 = call float @dx.op.quadOp.f32(i32 123, float %381, i8 2)  ; QuadOp(value,op)
  %394 = fadd fast float %382, %369
  %395 = fadd fast float %394, %386
  %396 = fadd fast float %395, %390
  %397 = fmul fast float %396, 2.500000e-01
  br i1 %193, label %398, label %431

; <label>:398                                     ; preds = %363
  %399 = fadd fast float %385, %381
  %400 = fadd fast float %399, %389
  %401 = fadd fast float %400, %393
  %402 = fmul fast float %401, 2.500000e-01
  %403 = fadd fast float %384, %377
  %404 = fadd fast float %403, %388
  %405 = fadd fast float %404, %392
  %406 = fmul fast float %405, 2.500000e-01
  %407 = fadd fast float %383, %373
  %408 = fadd fast float %407, %387
  %409 = fadd fast float %408, %391
  %410 = fmul fast float %409, 2.500000e-01
  %411 = shl i32 %8, 2
  %412 = shl i32 %9, 2
  %413 = lshr i32 %21, 1
  %414 = lshr i32 %24, 1
  %415 = add i32 %413, %411
  %416 = add i32 %414, %412
  %417 = fptoui float %397 to i32
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %3, i32 %415, i32 %416, i32 undef, i32 %417, i32 %417, i32 %417, i32 %417, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  %418 = add nuw nsw i32 %27, %414
  %419 = mul i32 %418, 16
  %420 = add i32 %28, %419
  %421 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateR@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %420
  store float %397, float addrspace(3)* %421, align 4, !tbaa !17
  %422 = mul i32 %418, 16
  %423 = add i32 %28, %422
  %424 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateG@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %423
  store float %410, float addrspace(3)* %424, align 4, !tbaa !17
  %425 = mul i32 %418, 16
  %426 = add i32 %28, %425
  %427 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateB@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %426
  store float %406, float addrspace(3)* %427, align 4, !tbaa !17
  %428 = mul i32 %418, 16
  %429 = add i32 %28, %428
  %430 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateA@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %429
  store float %402, float addrspace(3)* %430, align 4, !tbaa !17
  br label %431

; <label>:431                                     ; preds = %398, %363, %361
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %432 = icmp ult i32 %10, 16
  br i1 %432, label %433, label %502

; <label>:433                                     ; preds = %431
  %434 = shl nuw nsw i32 %24, 2
  %435 = shl nuw nsw i32 %21, 2
  %436 = add nuw nsw i32 %435, %24
  %437 = mul i32 %436, 16
  %438 = add i32 %434, %437
  %439 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateR@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %438
  %440 = load float, float addrspace(3)* %439, align 4, !tbaa !17
  %441 = mul i32 %436, 16
  %442 = add i32 %434, %441
  %443 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateG@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %442
  %444 = load float, float addrspace(3)* %443, align 4, !tbaa !17
  %445 = mul i32 %436, 16
  %446 = add i32 %434, %445
  %447 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateB@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %446
  %448 = load float, float addrspace(3)* %447, align 4, !tbaa !17
  %449 = mul i32 %436, 16
  %450 = add i32 %434, %449
  %451 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateA@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %450
  %452 = load float, float addrspace(3)* %451, align 4, !tbaa !17
  %453 = call float @dx.op.quadOp.f32(i32 123, float %440, i8 0)  ; QuadOp(value,op)
  %454 = call float @dx.op.quadOp.f32(i32 123, float %444, i8 0)  ; QuadOp(value,op)
  %455 = call float @dx.op.quadOp.f32(i32 123, float %448, i8 0)  ; QuadOp(value,op)
  %456 = call float @dx.op.quadOp.f32(i32 123, float %452, i8 0)  ; QuadOp(value,op)
  %457 = call float @dx.op.quadOp.f32(i32 123, float %440, i8 1)  ; QuadOp(value,op)
  %458 = call float @dx.op.quadOp.f32(i32 123, float %444, i8 1)  ; QuadOp(value,op)
  %459 = call float @dx.op.quadOp.f32(i32 123, float %448, i8 1)  ; QuadOp(value,op)
  %460 = call float @dx.op.quadOp.f32(i32 123, float %452, i8 1)  ; QuadOp(value,op)
  %461 = call float @dx.op.quadOp.f32(i32 123, float %440, i8 2)  ; QuadOp(value,op)
  %462 = call float @dx.op.quadOp.f32(i32 123, float %444, i8 2)  ; QuadOp(value,op)
  %463 = call float @dx.op.quadOp.f32(i32 123, float %448, i8 2)  ; QuadOp(value,op)
  %464 = call float @dx.op.quadOp.f32(i32 123, float %452, i8 2)  ; QuadOp(value,op)
  %465 = fadd fast float %453, %440
  %466 = fadd fast float %465, %457
  %467 = fadd fast float %466, %461
  %468 = fmul fast float %467, 2.500000e-01
  br i1 %193, label %469, label %502

; <label>:469                                     ; preds = %433
  %470 = fadd fast float %456, %452
  %471 = fadd fast float %470, %460
  %472 = fadd fast float %471, %464
  %473 = fmul fast float %472, 2.500000e-01
  %474 = fadd fast float %455, %448
  %475 = fadd fast float %474, %459
  %476 = fadd fast float %475, %463
  %477 = fmul fast float %476, 2.500000e-01
  %478 = fadd fast float %454, %444
  %479 = fadd fast float %478, %458
  %480 = fadd fast float %479, %462
  %481 = fmul fast float %480, 2.500000e-01
  %482 = shl i32 %8, 1
  %483 = shl i32 %9, 1
  %484 = lshr i32 %21, 1
  %485 = lshr i32 %24, 1
  %486 = add i32 %484, %482
  %487 = add i32 %485, %483
  %488 = fptoui float %468 to i32
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %2, i32 %486, i32 %487, i32 undef, i32 %488, i32 %488, i32 %488, i32 %488, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  %489 = add nuw nsw i32 %484, %24
  %490 = mul i32 %489, 16
  %491 = add i32 0, %490
  %492 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateR@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %491
  store float %468, float addrspace(3)* %492, align 4, !tbaa !17
  %493 = mul i32 %489, 16
  %494 = add i32 0, %493
  %495 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateG@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %494
  store float %481, float addrspace(3)* %495, align 4, !tbaa !17
  %496 = mul i32 %489, 16
  %497 = add i32 0, %496
  %498 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateB@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %497
  store float %477, float addrspace(3)* %498, align 4, !tbaa !17
  %499 = mul i32 %489, 16
  %500 = add i32 0, %499
  %501 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateA@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %500
  store float %473, float addrspace(3)* %501, align 4, !tbaa !17
  br label %502

; <label>:502                                     ; preds = %469, %433, %431
  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)
  %503 = icmp ult i32 %10, 4
  br i1 %503, label %504, label %539

; <label>:504                                     ; preds = %502
  %505 = mul i32 %10, 16
  %506 = add i32 0, %505
  %507 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateR@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %506
  %508 = load float, float addrspace(3)* %507, align 4, !tbaa !17
  %509 = mul i32 %10, 16
  %510 = add i32 0, %509
  %511 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateG@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %510
  %512 = load float, float addrspace(3)* %511, align 4, !tbaa !17
  %513 = mul i32 %10, 16
  %514 = add i32 0, %513
  %515 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateB@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %514
  %516 = load float, float addrspace(3)* %515, align 4, !tbaa !17
  %517 = mul i32 %10, 16
  %518 = add i32 0, %517
  %519 = getelementptr [256 x float], [256 x float] addrspace(3)* @"\01?spdIntermediateA@@3PAY0BA@$$CAMA.1dim", i32 0, i32 %518
  %520 = load float, float addrspace(3)* %519, align 4, !tbaa !17
  %521 = call float @dx.op.quadOp.f32(i32 123, float %508, i8 0)  ; QuadOp(value,op)
  %522 = call float @dx.op.quadOp.f32(i32 123, float %512, i8 0)  ; QuadOp(value,op)
  %523 = call float @dx.op.quadOp.f32(i32 123, float %516, i8 0)  ; QuadOp(value,op)
  %524 = call float @dx.op.quadOp.f32(i32 123, float %520, i8 0)  ; QuadOp(value,op)
  %525 = call float @dx.op.quadOp.f32(i32 123, float %508, i8 1)  ; QuadOp(value,op)
  %526 = call float @dx.op.quadOp.f32(i32 123, float %512, i8 1)  ; QuadOp(value,op)
  %527 = call float @dx.op.quadOp.f32(i32 123, float %516, i8 1)  ; QuadOp(value,op)
  %528 = call float @dx.op.quadOp.f32(i32 123, float %520, i8 1)  ; QuadOp(value,op)
  %529 = call float @dx.op.quadOp.f32(i32 123, float %508, i8 2)  ; QuadOp(value,op)
  %530 = call float @dx.op.quadOp.f32(i32 123, float %512, i8 2)  ; QuadOp(value,op)
  %531 = call float @dx.op.quadOp.f32(i32 123, float %516, i8 2)  ; QuadOp(value,op)
  %532 = call float @dx.op.quadOp.f32(i32 123, float %520, i8 2)  ; QuadOp(value,op)
  br i1 %193, label %533, label %539

; <label>:533                                     ; preds = %504
  %534 = fadd fast float %521, %508
  %535 = fadd fast float %534, %525
  %536 = fadd fast float %535, %529
  %537 = fmul fast float %536, 2.500000e-01
  %538 = fptoui float %537 to i32
  call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %1, i32 %8, i32 %9, i32 undef, i32 %538, i32 %538, i32 %538, i32 %538, i8 15)  ; TextureStore(srv,coord0,coord1,coord2,value0,value1,value2,value3,mask)
  br label %539

; <label>:539                                     ; preds = %533, %504, %502
  ret void
}

; Function Attrs: nounwind readnone
declare i32 @dx.op.groupId.i32(i32, i32) #0

; Function Attrs: nounwind readnone
declare i32 @dx.op.flattenedThreadIdInGroup.i32(i32) #0

; Function Attrs: nounwind
declare void @dx.op.textureStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32, i8) #1

; Function Attrs: nounwind readonly
declare %dx.types.ResRet.i32 @dx.op.textureLoad.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32) #2

; Function Attrs: nounwind
declare float @dx.op.quadOp.f32(i32, float, i8) #1

; Function Attrs: noduplicate nounwind
declare void @dx.op.barrier(i32, i32) #3

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #2

attributes #0 = { nounwind readnone }
attributes #1 = { nounwind }
attributes #2 = { nounwind readonly }
attributes #3 = { noduplicate nounwind }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!2}
!dx.shaderModel = !{!3}
!dx.resources = !{!4}
!dx.entryPoints = !{!14}

!0 = !{!"dxcoob 1.8.2502.11 (239921522)"}
!1 = !{i32 1, i32 2}
!2 = !{i32 1, i32 8}
!3 = !{!"cs", i32 6, i32 2}
!4 = !{null, !5, null, null}
!5 = !{!6, !8, !9, !10, !11, !12, !13}
!6 = !{i32 0, %"class.RWTexture2D<unsigned int>"* undef, !"", i32 0, i32 0, i32 1, i32 2, i1 false, i1 false, i1 false, !7}
!7 = !{i32 0, i32 5}
!8 = !{i32 1, %"class.RWTexture2D<unsigned int>"* undef, !"", i32 0, i32 1, i32 1, i32 2, i1 true, i1 false, i1 false, !7}
!9 = !{i32 2, %"class.RWTexture2D<unsigned int>"* undef, !"", i32 0, i32 2, i32 1, i32 2, i1 true, i1 false, i1 false, !7}
!10 = !{i32 3, %"class.RWTexture2D<unsigned int>"* undef, !"", i32 0, i32 3, i32 1, i32 2, i1 true, i1 false, i1 false, !7}
!11 = !{i32 4, %"class.RWTexture2D<unsigned int>"* undef, !"", i32 0, i32 4, i32 1, i32 2, i1 true, i1 false, i1 false, !7}
!12 = !{i32 5, %"class.RWTexture2D<unsigned int>"* undef, !"", i32 0, i32 5, i32 1, i32 2, i1 true, i1 false, i1 false, !7}
!13 = !{i32 6, %"class.RWTexture2D<unsigned int>"* undef, !"", i32 0, i32 6, i32 1, i32 2, i1 true, i1 false, i1 false, !7}
!14 = !{void ()* @CS, !"CS", null, !4, !15}
!15 = !{i32 0, i64 524288, i32 4, !16}
!16 = !{i32 256, i32 1, i32 1}
!17 = !{!18, !18, i64 0}
!18 = !{!"float", !19, i64 0}
!19 = !{!"omnipotent char", !20, i64 0}
!20 = !{!"Simple C/C++ TBAA"}
