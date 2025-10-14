//----------------------------------------------------------------------------------
// File:        gl4-kepler\WeightedBlendedOIT\assets\shaders/weighted_final_fragment.glsl
// SDK Version: v3.00
// Email:       gameworks@nvidia.com
// Site:        http://developer.nvidia.com/
//
// Copyright (c) 2014-2015, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------------------------------------------------------------

#ifdef USE_RECT_TEX
#if GLSL_VERSION >= 140
#define texture2DRect texture
#else
#extension GL_ARB_texture_rectangle : enable
#endif
uniform sampler2DRect ColorTex0;
uniform sampler2DRect ColorTex1;
#else
uniform sampler2D ColorTex0;
uniform sampler2D ColorTex1;
uniform vec2 screen_dim_RCP;
#endif

uniform float ze_to_zw_a;
uniform float ze_to_zw_b;
uniform float weighted_blended_depth_scale;

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#endif

#if GLSL_VERSION < 130
#define texture texture2D
#endif

const float kWeightNumerator = 0.03;
const float kWeightClampMin = 1e-2;
const float kWeightClampMax = 3e3;
const float kAlphaEpsilon = 1e-6;
const float kWeightEpsilon = 1e-5;

void main(void)
{
#ifdef USE_RECT_TEX
  vec4 sumColor = texture2DRect(ColorTex0, gl_FragCoord.xy);
  float transmittance = texture2DRect(ColorTex1, gl_FragCoord.xy).r;
#else
  vec4 sumColor = texture(ColorTex0, gl_FragCoord.xy * screen_dim_RCP);
  float transmittance = texture(ColorTex1, gl_FragCoord.xy * screen_dim_RCP).r;
#endif

  float resolvedAlpha = clamp(1.0 - transmittance, 0.0, 1.0);
  if (resolvedAlpha <= kAlphaEpsilon) {
    discard;
  }

  float accumWeightedAlpha = sumColor.a;
  float weight = accumWeightedAlpha / resolvedAlpha;
  weight = clamp(weight, kWeightClampMin, kWeightClampMax);

  float depthTerm = kWeightNumerator / weight - kWeightEpsilon;
  float fragDepth = 1.0;
  if (depthTerm > 0.0 && weighted_blended_depth_scale > 0.0) {
    float viewDepth = pow(depthTerm, 0.25) / (0.005 * weighted_blended_depth_scale);
    if (viewDepth > 1e-5) {
      fragDepth = clamp(ze_to_zw_a / viewDepth + ze_to_zw_b, 0.0, 1.0);
    }
  }

  float denom = clamp(sumColor.a, 1e-4, 5e4);
  vec3 resolvedColor = sumColor.rgb / denom * resolvedAlpha;
  FragData0 = vec4(resolvedColor, resolvedAlpha);
  gl_FragDepth = fragDepth;
}
