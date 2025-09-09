#version 450

layout(location = 0) in vec3 texCoord0;
layout(location = 1) in vec4 eyeCoord;

layout(location = 0) out vec4 FragData0;

void main()
{
  FragData0 = vec4(texCoord0, eyeCoord.z);
}

