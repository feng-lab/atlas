#pragma once

#include "zglobal.h"
#include <cstddef>
#include <vector>

namespace nim {

// if width == -1 then default size if sigma * 6 and it is returned in kWidth
template<typename TFloat>
std::vector<TFloat> create1DGaussianKernel(TFloat sigmaX,
                                           index_t width = -1,
                                           size_t* kWidth = nullptr);

// same as fspecial('gaussian', sz, sigma)
// if width == -1 then default size if sigma * 6 and it is returned in kWidth
template<typename TFloat>
std::vector<TFloat> create2DGaussianKernel(TFloat sigmaX, TFloat sigmaY,
                                           index_t width = -1, index_t height = -1,
                                           size_t* kWidth = nullptr, size_t* kHeight = nullptr);

// same as fspecial('gaussian', sz, sigma)
// if width == -1 then default size if sigma * 6 and it is returned in kWidth
template<typename TFloat>
std::vector<TFloat> create3DGaussianKernel(TFloat sigmaX, TFloat sigmaY, TFloat sigmaZ,
                                           index_t width = -1, index_t height = -1, index_t depth = -1,
                                           size_t* kWidth = nullptr, size_t* kHeight = nullptr,
                                           size_t* kDepth = nullptr);

//LoG  = G"
//2D: G"(x,y,sx,sy) = LoG(x,sx)G(y,sy) + G(x,sz)LoG(y,sy)
//3D: G"(x,y,z,sx,sy,sz) = LoG(x,sx)G(y,sy)G(z,sz) +
//                         G(x,sx)LoG(y,sy)G(z,sz) +
//                         G(x,sx)G(y,sy)LoG(z,sz)
// LoG kernel sum to 0
// if width == -1 then default size if sigma * 6 and it is returned in kWidth
template<typename TFloat>
std::vector<TFloat> create1DLoGKernel(TFloat sigmaX,
                                      index_t width = -1,
                                      size_t* kWidth = nullptr);

} // namespace nim
