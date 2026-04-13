// Centralized Vulkan descriptor set and binding constants.
//
// Conventions:
// - set 0 is reserved for Atlas bindless sampled-image tables (descriptor indexing),
//   shared across Vulkan shaders via Resources/shader/vulkan/include/bindless.glslinc.
// - Graphics pipeline contexts follow a fixed set map (bindless + UBOs + optional OIT).
// - Compute helper pipelines may use their own set layouts beyond set 0.

#pragma once

#include <cstdint>

namespace nim::vkbind {

// Common graphics set indices.
inline constexpr uint32_t kSetBindlessSampledImages = 0;
inline constexpr uint32_t kSetLighting = 1;
inline constexpr uint32_t kSetTransforms = 2;
inline constexpr uint32_t kSetOIT = 3;

// ---------------------------------------------------------------------------
// Bindless sampled images (set 0)
// ---------------------------------------------------------------------------
// Atlas uses descriptor indexing to provide bindless tables for sampled images.
// These bindings are shared across graphics pipeline contexts.
inline constexpr uint32_t kBindingBindlessTexture2D = 0;
inline constexpr uint32_t kBindingBindlessTexture2DArray = 1;
inline constexpr uint32_t kBindingBindlessTexture3D = 2;
inline constexpr uint32_t kBindingBindlessUTexture2D = 3;
inline constexpr uint32_t kBindingBindlessUTexture3D = 4;
// Fixed immutable samplers used to construct sampler2D/3D objects from bindless
// sampled images in shaders. These keep sampler limits tiny (MoltenVK/Metal
// portability) while allowing large bindless sampled-image tables.
inline constexpr uint32_t kBindingBindlessSamplerLinearClamp = 5;
inline constexpr uint32_t kBindingBindlessSamplerNearestClamp = 6;
inline constexpr uint32_t kBindingBindlessSamplerLinearBorderZero3D = 7;

// ---------------------------------------------------------------------------
// OIT (set 3) bindings
// ---------------------------------------------------------------------------
// Binding 1 is preserved for the DDP "changed" flag to avoid churn in existing shaders.
inline constexpr uint32_t kBindingOITParams = 0; // OIT params SSBO (viewport/pixelCount)
inline constexpr uint32_t kBindingOITDDPFlag = 1;
inline constexpr uint32_t kBindingOITPPLLCounts = 2; // uint counts[pixel]
inline constexpr uint32_t kBindingOITPPLLOffsets = 3; // uint offsets[pixel]
inline constexpr uint32_t kBindingOITPPLLCursors = 4; // uint cursors[pixel]
inline constexpr uint32_t kBindingOITPPLLFragments = 5; // Fragment fragments[total]

} // namespace nim::vkbind
