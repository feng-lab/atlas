#version 450

// Depth-only fragment shader that writes gl_FragDepth as a vertical ramp
// from 0.0 at the bottom to 1.0 at the top of the viewport.

layout(push_constant) uniform Push
{
  float invHeight; // 1.0 / renderArea.height
} pc;

void main()
{
  // Use pixel center for smoother gradient
  float y = gl_FragCoord.y + 0.5;
  float d = clamp(y * pc.invHeight, 0.0, 1.0);
  gl_FragDepth = d;
}

