#extension GL_KHR_cooperative_matrix : require
#extension GL_KHR_memory_scope_semantics : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#extension GL_EXT_shader_16bit_storage : require
layout(local_size_x = 32) in;
#ifdef IN_F32
layout(set = 0, binding = 0) buffer I { float in_[]; };
#else
layout(set = 0, binding = 0) buffer I { float16_t in_[]; };
#endif
layout(set = 0, binding = 1) buffer O { float out_[]; };
