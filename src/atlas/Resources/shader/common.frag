#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#endif

void fragment_func(out vec4 fragColor, out float fragDepth);

void main(void)
{
#if defined(ATLAS_DISABLE_FRAG_DEPTH_WRITE)
	float unusedDepth;
	fragment_func(FragData0, unusedDepth);
#else
	fragment_func(FragData0, gl_FragDepth);
#endif
}
