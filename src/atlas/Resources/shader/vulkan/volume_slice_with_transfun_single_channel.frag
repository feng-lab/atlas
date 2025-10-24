#version 450

layout(constant_id = 51) const bool RESULT_OPAQUE = false;

layout(set = 0, binding = 0) uniform sampler3D volume_1;
layout(set = 0, binding = 1) uniform sampler2D transfer_function_1;

layout(location = 0) in vec3 texCoord0;
layout(location = 0) out vec4 FragData0;

void main()
{
  vec4 color = texture(transfer_function_1, vec2(texture(volume_1, texCoord0).r, 0.5));
  if (color.a == 0.0) {
    color = vec4(0.0);
  }
  if (RESULT_OPAQUE) {
    color.a = 1.0;
  } else {
    color.rgb *= color.a;
  }
  FragData0 = color;
}
