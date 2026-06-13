#pragma once

#include "zimginterface.h"

#include <cstddef>
#include <cstdint>

namespace nim {

uint64_t image2DResizeHighwayExtraBytes(size_t height,
                                        size_t outWidth,
                                        size_t outHeight,
                                        Interpolant interpolant,
                                        bool antialiasing,
                                        bool antialiasingForNearest,
                                        size_t maxConcurrency);

template<typename T>
uint64_t image3DResizeHighwayExtraBytes(size_t height,
                                        size_t depth,
                                        size_t outWidth,
                                        size_t outHeight,
                                        size_t outDepth,
                                        Interpolant interpolant,
                                        bool antialiasing,
                                        bool antialiasingForNearest);

template<typename T>
void image2DResizeHighway(const T* img,
                          size_t width,
                          size_t height,
                          T* imgOut,
                          size_t outWidth,
                          size_t outHeight,
                          Interpolant interpolant,
                          bool antialiasing,
                          bool antialiasingForNearest,
                          bool useMultithreading);

template<typename T>
void image3DResizeHighway(const T* img,
                          size_t width,
                          size_t height,
                          size_t depth,
                          T* imgOut,
                          size_t outWidth,
                          size_t outHeight,
                          size_t outDepth,
                          Interpolant interpolant,
                          bool antialiasing,
                          bool antialiasingForNearest,
                          bool useMultithreading);

} // namespace nim
