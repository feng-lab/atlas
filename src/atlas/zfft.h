#pragma once

#include "zcompleximg.h"
#include "zimg.h"

namespace nim {

// fft only accept single channel 3D or 2D img as input, throw exception if input
// is not supported
ZComplexImg fft(const ZImg& img, size_t outWidth, size_t outHeight, size_t outDepth);

// input cimg storing the non-redundant half of a logically Hermitian array
// cimg.width() = width / 2 + 1;
// fftw always destroy input, so we take non-const reference as input
// note: ***will destroy cimg and make it empty***
// result img is double type, output img size should be smaller than actual img size
ZImg ifft(ZComplexImg& cimg, size_t width, size_t outWidth, size_t outHeight, size_t outDepth);

} // namespace nim

