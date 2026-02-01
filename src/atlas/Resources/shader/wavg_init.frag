//--------------------------------------------------------------------------------------
// Order Independent Transparency with Average Color
//
// Author: Louis Bavoil
// Email: sdkfeedback@nvidia.com
//
// Copyright (c) NVIDIA Corporation. All rights reserved.
//--------------------------------------------------------------------------------------

//#extension ARB_draw_buffers : require

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;
#elif GLSL_VERSION >= 130
out vec4 FragData0;
out vec4 FragData1;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#define FragData1 gl_FragData[1]
#endif

void fragment_func(out vec4 fragColor, out float fragDepth);

void main(void)
{
	float fragDepth;
	vec4 color;
	fragment_func(color, fragDepth);
#ifndef ATLAS_DISABLE_FRAG_DEPTH_WRITE
  gl_FragDepth = fragDepth;
#endif
		FragData0 = color;
		FragData1 = vec4(1.0, fragDepth, 0.0, 0.0);
	}
