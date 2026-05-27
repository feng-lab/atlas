#pragma once

#include <cstdint>

#if !defined(__SIZEOF_INT128__)
#include <absl/numeric/int128.h>
#endif

namespace nim::detail {

#if defined(__SIZEOF_INT128__)
using int128 = __int128;
using uint128 = unsigned __int128;
#else
using int128 = absl::int128;
using uint128 = absl::uint128;
#endif

} // namespace nim::detail
