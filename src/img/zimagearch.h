#pragma once

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || defined(__amd64__)
#define ATLAS_IMG_ARCH_X86_64 1
#else
#define ATLAS_IMG_ARCH_X86_64 0
#endif

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#define ATLAS_IMG_ARCH_ARM64 1
#else
#define ATLAS_IMG_ARCH_ARM64 0
#endif
