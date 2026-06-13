#pragma once

#include "zimagearch.h"

#if defined(ZIMG_USE_MKL) && ATLAS_IMG_ARCH_X86_64
#define ZIMG_MKL_ENABLED 1
#else
#define ZIMG_MKL_ENABLED 0
#endif

#if ZIMG_MKL_ENABLED

#include <complex>
//#define MKL_INT MKL_INT64
//#define MKL_UINT MKL_UINT64
#define MKL_Complex8 std::complex<float>
#define MKL_Complex16 std::complex<double>
#include <mkl.h>

#endif
