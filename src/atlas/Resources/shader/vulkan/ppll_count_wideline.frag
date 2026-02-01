#version 450
#extension GL_GOOGLE_include_directive : require

#include "include/wideline_func1.glslinc"
#include "include/ppll_common.glslinc"

void main()
{
  fragment_discard_only();
  ppllCountFragment();
}
