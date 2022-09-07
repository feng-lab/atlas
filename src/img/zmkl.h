#pragma once

#ifdef ZIMG_USE_MKL
#define MKL_INT size_t
#define MKL_Complex16 std::complex<double>

#include "mkl.h"
#endif
