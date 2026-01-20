#pragma once

#include <cstdint>

namespace nim {

// Scope ids used by the Scene RPC for view-setting / timeline operations.
//
// 0 = camera
// 1 = background
// 2 = axis
// 3 = global (lighting/cuts)
// >=4 = scene object id
inline constexpr uint64_t kZRpcScopeCamera = 0;
inline constexpr uint64_t kZRpcScopeBackground = 1;
inline constexpr uint64_t kZRpcScopeAxis = 2;
inline constexpr uint64_t kZRpcScopeGlobal = 3;

} // namespace nim

