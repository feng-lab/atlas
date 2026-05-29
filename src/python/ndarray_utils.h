// Utilities for handling nanobind ndarrays in zimg bindings
#pragma once

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <algorithm>
#include <stdexcept>
#include <string>

namespace nb = nanobind;

namespace zpy {

// DLPack CPU device code used by nanobind
inline constexpr int kDLCPU = 1;

struct LayoutMap
{
  int c = -1;
  int z = -1;
  int y = -1;
  int x = -1;
};

inline std::string toUpper(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) {
    return (char)std::toupper(ch);
  });
  return s;
}

// Parse a layout string describing axis order of the input array.
// Supported examples: "CZYX" (default), "ZYXC" (channel-last), "ZYX" (no channel)
inline LayoutMap parseLayout(const std::string& layout_in, int ndim)
{
  auto layout = toUpper(layout_in);
  LayoutMap lm;
  // The layout string orders axes from 0..ndim-1
  if ((int)layout.size() != ndim && !((int)layout.size() == 3 && ndim == 3)) {
    throw std::invalid_argument("layout length must match ndarray ndim (or 3 for single-channel 3D)");
  }
  for (int i = 0; i < (int)layout.size(); ++i) {
    char ax = layout[i];
    if (ax == 'C') {
      lm.c = i;
    } else if (ax == 'Z') {
      lm.z = i;
    } else if (ax == 'Y') {
      lm.y = i;
    } else if (ax == 'X') {
      lm.x = i;
    }
  }
  if (lm.y < 0 || lm.x < 0) {
    throw std::invalid_argument("layout must contain Y and X axes");
  }
  return lm;
}

// Return true if array is row-major contiguous in its provided axis order.
template<typename... Args>
inline bool isContiguousC(const nb::ndarray<Args...>& arr)
{
  size_t ndim = arr.ndim();
  const int64_t* shape = arr.shape_ptr();
  const int64_t* strides = arr.stride_ptr();
  // compute expected strides in elements for C-order
  int64_t expect = 1;
  for (int i = (int)ndim - 1; i >= 0; --i) {
    int64_t dim = shape[i];
    if (dim > 1 && strides[i] != expect) {
      return false;
    }
    expect *= (dim > 0 ? dim : 1);
  }
  return true;
}

template<typename... Args>
inline bool isCPU(const nb::ndarray<Args...>& arr)
{
  return arr.device_type() == kDLCPU;
}

// Map dtype to (voxelFormatCode, bytes). Codes: 0=Unsigned, 1=Signed, 2=Float; bytes in {1,2,4,8}.
inline std::pair<int, int> mapDType(const nb::ndarray<>& arr)
{
  auto dt = arr.dtype();
  using Code = nb::dlpack::dtype_code;
  auto code = (Code)dt.code;
  int bytes = (dt.bits + 7) / 8;
  int vf = -1;
  if (code == Code::UInt) {
    vf = 0;
  } else if (code == Code::Int) {
    vf = 1;
  } else if (code == Code::Float) {
    vf = 2;
  }
  if (vf < 0) {
    throw std::invalid_argument("Unsupported ndarray dtype (only signed/unsigned/float supported)");
  }
  if (!(bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8)) {
    throw std::invalid_argument("Unsupported dtype size (must be 1/2/4/8 bytes)");
  }
  return {vf, bytes};
}

// Compute (c,z,y,x) sizes from layout map and ndarray shape.
template<typename... Args>
inline std::tuple<size_t, size_t, size_t, size_t> dimsFromLayout(const nb::ndarray<Args...>& arr, const LayoutMap& lm)
{
  auto idx = [&](int axis) -> int64_t {
    return axis >= 0 ? arr.shape((size_t)axis) : 1;
  };
  size_t c = (size_t)idx(lm.c);
  size_t z = (size_t)idx(lm.z);
  size_t y = (size_t)idx(lm.y);
  size_t x = (size_t)idx(lm.x);
  return {c, z, y, x};
}

// Compute byte strides for each logical axis given a layout.
template<typename... Args>
inline std::tuple<int64_t, int64_t, int64_t, int64_t> stridesFromLayout(const nb::ndarray<Args...>& arr,
                                                                        const LayoutMap& lm)
{
  auto sidx = [&](int axis) -> int64_t {
    return axis >= 0 ? arr.stride((size_t)axis) : 0;
  };
  int64_t sc = sidx(lm.c);
  int64_t sz = sidx(lm.z);
  int64_t sy = sidx(lm.y);
  int64_t sx = sidx(lm.x);
  int64_t item = (arr.dtype().bits + 7) / 8;
  // convert to bytes
  sc *= item;
  sz *= item;
  sy *= item;
  sx *= item;
  return {sc, sz, sy, sx};
}

} // namespace zpy
