// Centralized Vulkan descriptor set and binding constants for OIT/composite paths.
// Keep these aligned across all ZVulkan* pipeline contexts.

#pragma once

namespace nim::vkbind {

// Set indices
inline constexpr uint32_t kSetInputs = 0;       // Primary sampled inputs for a pass
inline constexpr uint32_t kSetOITParams = 3;    // OIT (DDP flag) set

// OIT DDP flag SSBO (set 3)
inline constexpr uint32_t kBindingOITDDPFlag = 1;

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
