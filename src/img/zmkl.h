#pragma once

#ifdef ZIMG_USE_MKL

#include <complex>
//#define MKL_INT MKL_INT64
//#define MKL_UINT MKL_UINT64
#define MKL_Complex8 std::complex<float>
#define MKL_Complex16 std::complex<double>
#include <mkl.h>

#endif
