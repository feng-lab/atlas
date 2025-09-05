# Vulkan Migration Guidelines

This document describes the changes made to support both OpenGL and Vulkan rendering in the atlas codebase.

## Code Organization

### Shared Types

We've extracted common rendering types into a separate header file:

- `z3dtypes.h`: Contains enums and other types shared between OpenGL and Vulkan renderers
  - `Z3DEye`: Defines eye positions for stereo rendering
  - `Z3DScreenShotType`: Defines screenshot types
  - `Z3DCoordinateSystem`: Specifies the coordinate system (OpenGL or Vulkan)

### Camera System

We've enhanced the camera system to work with both OpenGL and Vulkan:

- `Z3DCamera` now supports both OpenGL and Vulkan coordinate systems
  - Added a `setCoordinateSystem(Z3DCoordinateSystem)` method to switch between coordinate systems
  - Adjusted projection matrix calculations to handle Vulkan's Y-down and [0,1] depth range
  - No need for a separate `ZVulkanCamera` class

### Renderer Base Classes

We've created Vulkan equivalents for the OpenGL renderer base classes:

- `ZVulkanRendererBase`: Equivalent to `Z3DRendererBase`
  - Uses the same `Z3DCamera` class with `Z3DCoordinateSystem::Vulkan`
  - Manages viewport matrices, push constants, and other global state

- `ZVulkanRenderer`: Equivalent to `Z3DPrimitiveRenderer`
  - Base class for all Vulkan renderers
  - Provides common functionality like lighting and coordinate transform support

### Primitive Renderers

We've implemented the first Vulkan primitive renderer:

- `ZVulkanLineRenderer`: Equivalent to `Z3DLineRenderer`
  - Supports smooth lines with accurate widths
  - Handles line strips and various visual options
  - Uses geometry shaders when available, with fallback for devices that don't support them

## Key Differences Between OpenGL and Vulkan

1. **Coordinate System**:
   - OpenGL: Y-up, depth range [-1, 1]
   - Vulkan: Y-down, depth range [0, 1]

2. **Resource Management**:
   - OpenGL: Global state machine
   - Vulkan: Explicit resource management

3. **Command Submission**:
   - OpenGL: Immediate mode rendering
   - Vulkan: Command buffers and queues

## Migration Strategy

When migrating renderers from OpenGL to Vulkan:

1. Create a Vulkan equivalent class
2. Use the shared `Z3DCamera` with `Z3DCoordinateSystem::Vulkan`
3. Create Vulkan shaders that handle the coordinate system differences
4. Handle resources explicitly with proper lifetime management

## Shader Pipeline

Vulkan requires explicit compilation of shaders to SPIR-V. The shaders are stored in:

- `src/atlas/Resources/shader/vulkan/`: Source shaders for Vulkan
- `src/atlas/Resources/shader/vulkan/spv/`: Compiled SPIR-V shaders

Use the `build_shaders.sh` script to compile the shaders with glslangValidator.

## Adding New Renderers

When adding a new renderer:

1. Start with the OpenGL implementation as a reference
2. Create a Vulkan subclass of `ZVulkanRenderer`
3. Implement the required virtual methods
4. Handle proper resource management and cleanup

## Testing

Test both rendering backends by:

1. Setting the appropriate coordinate system
2. Comparing visual output between OpenGL and Vulkan
3. Verifying behavior in stereo modes 