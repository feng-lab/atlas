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

/**
 * Coordinate system type to handle different graphics APIs
 */
enum class Z3DCoordinateSystem
{
  OpenGL,  // Y-up, depth range [-1, 1]
  Vulkan   // Y-down, depth range [0, 1]
};

enum class RenderBackend
{
  OpenGL = 0,
  Vulkan
};

} // namespace nim 
