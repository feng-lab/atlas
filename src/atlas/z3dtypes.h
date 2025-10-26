#pragma once

namespace nim {

/**
 * Eye type for stereo rendering
 */
enum Z3DEye
{
  LeftEye = 0,
  MonoEye = 1,
  RightEye = 2
};

/**
 * Screenshot types for different views
 */
enum class Z3DScreenShotType
{
  MonoView,
  HalfSideBySideStereoView,
  FullSideBySideStereoView
};

enum class RenderBackend
{
  OpenGL = 0,
  Vulkan
};

} // namespace nim 
