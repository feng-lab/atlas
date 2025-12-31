#pragma once

#if defined(ZIMG_USE_MKL) && (defined(__x86_64__) || defined(_M_X64) || defined(__amd64__))
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
