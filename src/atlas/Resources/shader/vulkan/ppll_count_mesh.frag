#version 450
#extension GL_GOOGLE_include_directive : require

layout(early_fragment_tests) in;

#include "include/ppll_common.glslinc"

void main()
{
  ppllCountFragment();
}
