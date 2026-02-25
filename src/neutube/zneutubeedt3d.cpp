#include "zneutubeedt3d.h"

#include "zexception.h"
#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace nim {

namespace {

constexpr uint16_t kU16Inf = 0xFFFFu;
constexpr float kFloatInf = 1E20f;

void dt1d_first_m_mu16(uint16_t* d, long n, const uint16_t* f, int* v, float* z)
{
  int q = 0;
  for (q = 1; q < n; ++q) {
    if (f[q - 1] != f[q]) {
      break;
    }
  }

  if (q == n) {
    return;
  }

  if (f[0] == 0) {
    v[0] = 0;
  } else {
    v[0] = q;
  }

  int k = 0;
  z[0] = -kFloatInf;
  z[1] = +kFloatInf;

  for (q = v[0] + 1; q < n; ++q) {
    if (f[q] == 0) {
      float s = static_cast<float>(q + v[k]) * 0.5f;
      while (s <= z[k]) {
        --k;
        s = static_cast<float>(q + v[k]) * 0.5f;
      }

      ++k;
      v[k] = q;
      z[k] = s;
      z[k + 1] = +kFloatInf;
    }
  }

  k = 0;
  for (q = 0; q < n; ++q) {
    if (d[q] > 0) {
      while (z[k + 1] < q) {
        ++k;
      }

      if (f[q] > 0) {
        d[q] = static_cast<uint16_t>(std::abs(q - v[k]));
      }
    }
  }
}

void dt1d_second_m_mu16(const uint16_t* f, long n, uint16_t* d, int* v, float* z, const uint8_t* m, int sqr_field)
{
  int q = 0;
  for (q = 0; q < n; ++q) {
    if (m[q] == 0) {
      break;
    }
  }

  if (q == n) {
    return;
  }

  v[0] = q;

  int k = 0;
  z[0] = -kFloatInf;
  z[1] = +kFloatInf;

  for (q = v[0] + 1; q < n; ++q) {
    if (m[q] == 0) {
      float s = static_cast<float>(f[q]) - static_cast<float>(f[v[k]]);
      if (sqr_field == 0) {
        s *= static_cast<float>(f[q]) + static_cast<float>(f[v[k]]);
      }
      s = (s / static_cast<float>(q - v[k]) + static_cast<float>(q + v[k])) / 2.0f;

      while (s <= z[k]) {
        --k;
        s = static_cast<float>(f[q]) - static_cast<float>(f[v[k]]);
        if (sqr_field == 0) {
          s *= static_cast<float>(f[q]) + static_cast<float>(f[v[k]]);
        }
        s = (s / static_cast<float>(q - v[k]) + static_cast<float>(q + v[k])) / 2.0f;
      }

      ++k;
      v[k] = q;
      z[k] = s;
      z[k + 1] = +kFloatInf;
    }
  }

  k = 0;
  for (q = 0; q <= n - 1; ++q) {
    if (f[q] > 0) {
      while (z[++k] < q) {}
      --k;

      const int dx = q - v[k];
      int64_t df = static_cast<int64_t>(dx) * static_cast<int64_t>(dx);
      if (sqr_field == 0) {
        const int64_t fv = static_cast<int64_t>(f[v[k]]);
        df += fv * fv;
      } else {
        df += static_cast<int64_t>(f[v[k]]);
      }

      if (df > static_cast<int64_t>(kU16Inf)) {
        d[q] = static_cast<uint16_t>(kU16Inf - 1);
      } else {
        d[q] = static_cast<uint16_t>(df);
      }
    } else {
      d[q] = 0;
    }
  }
}

void dt3d_mu16(uint16_t* data, const long* sz, int pad)
{
  const long w = sz[0];
  const long h = sz[1];
  const long d = sz[2];

  const long plane = h * w;

  long len = std::max({w, h, d});
  len += pad * 2;

  std::vector<uint16_t> f(static_cast<size_t>(len));
  std::vector<uint16_t> dd(static_cast<size_t>(len));
  std::vector<uint8_t> m(static_cast<size_t>(len));
  std::vector<int> v(static_cast<size_t>(len));
  std::vector<float> z(static_cast<size_t>(len + 1));

  if (pad == 1) {
    f[0] = 0;
    f[static_cast<size_t>(w) + 1] = 0;
    dd[0] = 0;
    dd[static_cast<size_t>(w) + 1] = 0;
  }

  // X pass: per-row distance to background.
  for (long kz = 0; kz < d; ++kz) {
    const long tmp_k = kz * plane;
    for (long jy = 0; jy < h; ++jy) {
      const long tmp_j = jy * w;
      std::copy_n(data + tmp_k + tmp_j, static_cast<size_t>(w), f.begin() + pad);
      std::copy_n(data + tmp_k + tmp_j, static_cast<size_t>(w), dd.begin() + pad);
      dt1d_first_m_mu16(dd.data(), w + pad * 2L, f.data(), v.data(), z.data());
      std::copy_n(dd.begin() + pad, static_cast<size_t>(w), data + tmp_k + tmp_j);
    }
  }

  if (pad == 1) {
    f[0] = 0;
    f[static_cast<size_t>(h) + 1] = 0;
    m[0] = 0;
    m[static_cast<size_t>(h) + 1] = 0;
    dd[0] = 0;
    dd[static_cast<size_t>(h) + 1] = 0;
  }

  // Y pass: squared 2D distance field (per-slice).
  for (long kz = 0; kz < d; ++kz) {
    const long tmp_k = kz * plane;
    for (long ix = 0; ix < w; ++ix) {
      for (long jy = pad; jy < h + pad; ++jy) {
        const uint16_t val = *(data + tmp_k + (jy - pad) * w + ix);
        f[static_cast<size_t>(jy)] = val;
        m[static_cast<size_t>(jy)] = (val == kU16Inf);
      }

      dt1d_second_m_mu16(f.data(), h + pad * 2L, dd.data(), v.data(), z.data(), m.data(), 0);

      for (long jy = pad; jy < h + pad; ++jy) {
        *(data + tmp_k + (jy - pad) * w + ix) = dd[static_cast<size_t>(jy)];
      }
    }
  }

  if (pad == 1) {
    f[0] = 0;
    f[static_cast<size_t>(d) + 1] = 0;
    dd[0] = 0;
    dd[static_cast<size_t>(d) + 1] = 0;
  }

  // Z pass: squared 3D distance field.
  for (long jy = 0; jy < h; ++jy) {
    const long tmp_j = jy * w;
    for (long ix = 0; ix < w; ++ix) {
      for (long kz = pad; kz < d + pad; ++kz) {
        const uint16_t val = *(data + (kz - pad) * plane + tmp_j + ix);
        f[static_cast<size_t>(kz)] = val;
        m[static_cast<size_t>(kz)] = (val == kU16Inf);
      }

      dt1d_second_m_mu16(f.data(), d + pad * 2L, dd.data(), v.data(), z.data(), m.data(), 1);

      for (long kz = pad; kz < d + pad; ++kz) {
        *(data + (kz - pad) * plane + tmp_j + ix) = dd[static_cast<size_t>(kz)];
      }
    }
  }
}

void dt3d_binary_mu16(uint16_t* data, const long* sz, int pad)
{
  const size_t length = static_cast<size_t>(sz[0]) * static_cast<size_t>(sz[1]) * static_cast<size_t>(sz[2]);
  for (size_t i = 0; i < length; ++i) {
    data[i] = (data[i] > 0) ? kU16Inf : uint16_t{0};
  }

  dt3d_mu16(data, sz, pad);
}

} // namespace

ZImg bwdistSquaredU16LegacyLike(const ZImg& binaryMask, int pad)
{
  if (binaryMask.isEmpty()) {
    return {};
  }

  CHECK(binaryMask.numChannels() == 1);
  CHECK(binaryMask.numTimes() == 1);
  CHECK(binaryMask.isType<uint8_t>()) << "Expected uint8 input, got " << binaryMask.info();

  ZImgInfo info = binaryMask.info();
  info.setVoxelFormat<uint16_t>();
  ZImg out(info);

  const size_t voxelNumber = binaryMask.voxelNumber();
  const auto* inData = binaryMask.timeData<uint8_t>(0);
  auto* outData = out.timeData<uint16_t>(0);

  for (size_t i = 0; i < voxelNumber; ++i) {
    outData[i] = static_cast<uint16_t>(inData[i]);
  }

  long sz[3];
  sz[0] = static_cast<long>(binaryMask.width());
  sz[1] = static_cast<long>(binaryMask.height());
  sz[2] = static_cast<long>(binaryMask.depth());

  // Legacy Stack_Bwdist_L_U16(..., pad) calls dt3d_binary_mu16(..., pad=!pad).
  const int dtPad = (pad == 0) ? 1 : 0;
  dt3d_binary_mu16(outData, sz, dtPad);

  return out;
}

} // namespace nim
