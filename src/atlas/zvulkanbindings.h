// Centralized Vulkan descriptor set and binding constants for OIT/composite paths.
// Keep these aligned across all ZVulkan* pipeline contexts.

#pragma once

#include <cstdint>

namespace nim::vkbind {

// Set indices
inline constexpr uint32_t kSetInputs = 0;       // Primary sampled inputs for a pass
inline constexpr uint32_t kSetOITParams = 3;    // OIT (DDP flag) set

// OIT (set 3) bindings
// Binding 1 is preserved for the DDP "changed" flag to avoid churn in existing shaders.
inline constexpr uint32_t kBindingOITParams = 0; // OIT params SSBO (viewport/pixelCount)
inline constexpr uint32_t kBindingOITDDPFlag = 1;
inline constexpr uint32_t kBindingOITPPLLCounts = 2; // uint counts[pixel]
inline constexpr uint32_t kBindingOITPPLLOffsets = 3; // uint offsets[pixel]
inline constexpr uint32_t kBindingOITPPLLCursors = 4; // uint cursors[pixel]
inline constexpr uint32_t kBindingOITPPLLFragments = 5; // Fragment fragments[total]

// Dual Depth Peeling (geometry peel) sampled inputs (set 0)
inline constexpr uint32_t kBindingDDPDepthBlender = 0;     // depth blender
inline constexpr uint32_t kBindingDDPFrontBlender = 1;     // front blender

// Dual Depth Peeling (mesh peel) bindings avoid collisions with mesh_func.glslinc
// texture bindings at set 0 (0,1,2). Use 3,4 for mesh-only DDP samplers.
inline constexpr uint32_t kBindingDDPMeshDepthBlender = 3; // mesh-only depth blender
inline constexpr uint32_t kBindingDDPMeshFrontBlender = 4; // mesh-only front blender

// Dual Depth Peeling final composition inputs (set 0)
inline constexpr uint32_t kBindingDDPFinalDepth = 0;
inline constexpr uint32_t kBindingDDPFinalFront = 1;
inline constexpr uint32_t kBindingDDPFinalBack  = 2;

// Weighted Average resolve inputs (set 0)
inline constexpr uint32_t kBindingWAAccum   = 0;
inline constexpr uint32_t kBindingWAMoments = 1;

// Weighted Blended resolve inputs (set 0)
inline constexpr uint32_t kBindingWBAccum         = 0;
inline constexpr uint32_t kBindingWBTransmittance = 1;

// Glow pipelines
inline constexpr uint32_t kGlowBindingColorIn  = 0;
inline constexpr uint32_t kGlowBindingDepthIn  = 1;
inline constexpr uint32_t kGlowBindingBlurIn0  = 2;
inline constexpr uint32_t kGlowBindingBlurIn1  = 3;

// Simple blur pipeline inputs
inline constexpr uint32_t kBlurBindingColorIn = 0;
inline constexpr uint32_t kBlurBindingDepthIn = 1;

} // namespace nim::vkbind
