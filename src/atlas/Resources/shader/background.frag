uniform vec2 screen_dim_RCP;

uniform vec4 color1;
#if !defined(UNIFORM)
uniform vec4 color2;
#endif

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#endif

void main(void)
{
#ifdef UNIFORM
	FragData0 = color1;
#endif

#ifdef GRADIENT_LEFT_TO_RIGHT
	FragData0 = mix(color1, color2, gl_FragCoord.x * screen_dim_RCP.x);
#endif

#ifdef GRADIENT_RIGHT_TO_LEFT
	FragData0 = mix(color2, color1, gl_FragCoord.x * screen_dim_RCP.x);
#endif

#ifdef GRADIENT_TOP_TO_BOTTOM
	FragData0 = mix(color2, color1, gl_FragCoord.y * screen_dim_RCP.y);
#endif

#ifdef GRADIENT_BOTTOM_TO_TOP
	FragData0 = mix(color1, color2, gl_FragCoord.y * screen_dim_RCP.y);
#endif
}

