#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#if defined(min) || defined(max)
#error Windows.h needs to be included by this header, or else NOMINMAX needs to be defined before including it yourself.
#endif

#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include <windows.h>

namespace nim {

UINT getActiveCodePage();

void logActiveCodePage();

} // namespace nim

#endif
