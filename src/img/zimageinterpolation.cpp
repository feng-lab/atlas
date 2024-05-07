#include "zimageinterpolation.h"

#include "zimage2dutils.h"
#include "zimage3dutils.h"
#include <cmath>

namespace nim {

ZImageInterpolation::ZImageInterpolation(Interpolant interp, PadOption padOption, double fillValue)
  : m_interpolant(interp)
  , m_boundInBorder(false)
  , m_padOption(padOption)
  , m_fillValue(fillValue)
{}

double ZImageInterpolation::cubicInterpolate(double p[4], double x)
{
  return p[1] +
         0.5 * x *
           (p[2] - p[0] + x * (2.0 * p[0] - 5.0 * p[1] + 4.0 * p[2] - p[3] + x * (3.0 * (p[1] - p[2]) + p[3] - p[0])));
}

double ZImageInterpolation::bicubicInterpolate(double p[4][4], double x, double y)
{
  double arr[4];
  arr[0] = cubicInterpolate(p[0], y);
  arr[1] = cubicInterpolate(p[1], y);
  arr[2] = cubicInterpolate(p[2], y);
  arr[3] = cubicInterpolate(p[3], y);
  return cubicInterpolate(arr, x);
}

double ZImageInterpolation::tricubicInterpolate(double p[4][4][4], double x, double y, double z)
{
  double arr[4];
  arr[0] = bicubicInterpolate(p[0], y, z);
  arr[1] = bicubicInterpolate(p[1], y, z);
  arr[2] = bicubicInterpolate(p[2], y, z);
  arr[3] = bicubicInterpolate(p[3], y, z);
  return cubicInterpolate(arr, x);
}

double ZImageInterpolation::nCubicInterpolate(int32_t n, double* p, double coordinates[])
{
  CHECK(n > 0);
  if (n == 1) {
    return cubicInterpolate(p, *coordinates);
  }
  double arr[4];
  int32_t skip = 1 << (n - 1) * 2;
  arr[0] = nCubicInterpolate(n - 1, p, coordinates + 1);
  arr[1] = nCubicInterpolate(n - 1, p + skip, coordinates + 1);
  arr[2] = nCubicInterpolate(n - 1, p + 2 * skip, coordinates + 1);
  arr[3] = nCubicInterpolate(n - 1, p + 3 * skip, coordinates + 1);
  return cubicInterpolate(arr, *coordinates);
}

template<typename TPixel>
double ZImageInterpolation::sample(const TPixel* img, size_t width, size_t height, double x, double y) const
{
  if (m_boundInBorder && m_padOption == PadOption::Replicate && !inBound(width, height, x, y)) {
    return m_fillValue;
  }

  if (m_interpolant == Interpolant::Nearest) {
    return getImage2DPixelValue(img,
                                width,
                                height,
                                roundTo<index_t>(x),
                                roundTo<index_t>(y),
                                m_padOption,
                                TPixel(m_fillValue));
  } else if (m_interpolant == Interpolant::Linear) {
    double perc[4];
    index_t xBas0 = std::floor(x);
    index_t xBas1 = xBas0 + 1;
    index_t yBas0 = std::floor(y);
    index_t yBas1 = yBas0 + 1;

    // Linear interpolation constants (percentages)
    double xCom = x - xBas0;
    double yCom = y - yBas0;
    perc[0] = (1 - xCom) * (1 - yCom);
    perc[1] = (1 - xCom) * yCom;
    perc[2] = xCom * (1 - yCom);
    perc[3] = xCom * yCom;

    return perc[0] * getImage2DPixelValue(img, width, height, xBas0, yBas0, m_padOption, TPixel(m_fillValue)) +
           perc[1] * getImage2DPixelValue(img, width, height, xBas0, yBas1, m_padOption, TPixel(m_fillValue)) +
           perc[2] * getImage2DPixelValue(img, width, height, xBas1, yBas0, m_padOption, TPixel(m_fillValue)) +
           perc[3] * getImage2DPixelValue(img, width, height, xBas1, yBas1, m_padOption, TPixel(m_fillValue));
  } else if (m_interpolant == Interpolant::Cubic) {
    // http://en.wikipedia.org/wiki/Bicubic_interpolation
    index_t xn[4], yn[4];
    double vecTx[4], vecTy[4];
    double vecPTx[4], vecPTy[4];

    // Determine of the zero neighbor
    index_t xBas0 = std::floor(x);
    index_t yBas0 = std::floor(y);

    // Determine the location in between the pixels 0..1
    double tx = x - xBas0;
    double ty = y - yBas0;

    // Determine the t vectors
    vecTx[0] = 0.5;
    vecTx[1] = 0.5 * tx;
    vecTx[2] = 0.5 * tx * tx;
    vecTx[3] = 0.5 * tx * tx * tx;
    vecTy[0] = 0.5;
    vecTy[1] = 0.5 * ty;
    vecTy[2] = 0.5 * ty * ty;
    vecTy[3] = 0.5 * ty * ty * ty;

    // t vector multiplied with 4x4 bicubic kernel gives the to q vectors
    vecPTx[0] = -1.0 * vecTx[1] + 2.0 * vecTx[2] - 1.0 * vecTx[3];
    vecPTx[1] = 2.0 * vecTx[0] - 5.0 * vecTx[2] + 3.0 * vecTx[3];
    vecPTx[2] = 1.0 * vecTx[1] + 4.0 * vecTx[2] - 3.0 * vecTx[3];
    vecPTx[3] = -1.0 * vecTx[2] + 1.0 * vecTx[3];
    vecPTy[0] = -1.0 * vecTy[1] + 2.0 * vecTy[2] - 1.0 * vecTy[3];
    vecPTy[1] = 2.0 * vecTy[0] - 5.0 * vecTy[2] + 3.0 * vecTy[3];
    vecPTy[2] = 1.0 * vecTy[1] + 4.0 * vecTy[2] - 3.0 * vecTy[3];
    vecPTy[3] = -1.0 * vecTy[2] + 1.0 * vecTy[3];

    // Determine 1D neighbour coordinates
    xn[0] = xBas0 - 1;
    xn[1] = xBas0;
    xn[2] = xBas0 + 1;
    xn[3] = xBas0 + 2;
    yn[0] = yBas0 - 1;
    yn[1] = yBas0;
    yn[2] = yBas0 + 1;
    yn[3] = yBas0 + 2;

    // First do interpolation in the x direction followed by interpolation in the y direction
    double res = 0.0;
    for (auto i = 0; i < 4; ++i) {
      res += vecPTy[i] *
             (vecPTx[0] * getImage2DPixelValue(img, width, height, xn[0], yn[i], m_padOption, TPixel(m_fillValue)) +
              vecPTx[1] * getImage2DPixelValue(img, width, height, xn[1], yn[i], m_padOption, TPixel(m_fillValue)) +
              vecPTx[2] * getImage2DPixelValue(img, width, height, xn[2], yn[i], m_padOption, TPixel(m_fillValue)) +
              vecPTx[3] * getImage2DPixelValue(img, width, height, xn[3], yn[i], m_padOption, TPixel(m_fillValue)));
    }
    return res;
  }
  return 0.0;
}

template<typename TPixel>
double
ZImageInterpolation::sample(const TPixel* img, size_t width, size_t height, size_t depth, double x, double y, double z)
  const
{
  if (m_boundInBorder && m_padOption == PadOption::Replicate && !inBound(width, height, depth, x, y, z)) {
    return m_fillValue;
  }

  if (m_interpolant == Interpolant::Nearest) {
    return getImage3DPixelValue(img,
                                width,
                                height,
                                depth,
                                roundTo<index_t>(x),
                                roundTo<index_t>(y),
                                roundTo<index_t>(z),
                                m_padOption,
                                TPixel(m_fillValue));
  } else if (m_interpolant == Interpolant::Linear) {
    double perc[8];
    index_t xBas0 = std::floor(x);
    index_t xBas1 = xBas0 + 1;
    index_t yBas0 = std::floor(y);
    index_t yBas1 = yBas0 + 1;
    index_t zBas0 = std::floor(z);
    index_t zBas1 = zBas0 + 1;

    // Linear interpolation constants (percentages)
    double xCom = x - xBas0;
    double yCom = y - yBas0;
    double zCom = z - zBas0;
    double xComi = 1 - xCom;
    double yComi = 1 - yCom;
    double zComi = 1 - zCom;
    perc[0] = xComi * yComi;
    perc[1] = perc[0] * zCom;
    perc[0] = perc[0] * zComi;
    perc[2] = xComi * yCom;
    perc[3] = perc[2] * zCom;
    perc[2] = perc[2] * zComi;
    perc[4] = xCom * yComi;
    perc[5] = perc[4] * zCom;
    perc[4] = perc[4] * zComi;
    perc[6] = xCom * yCom;
    perc[7] = perc[6] * zCom;
    perc[6] = perc[6] * zComi;

    return perc[0] *
             getImage3DPixelValue(img, width, height, depth, xBas0, yBas0, zBas0, m_padOption, TPixel(m_fillValue)) +
           perc[1] *
             getImage3DPixelValue(img, width, height, depth, xBas0, yBas0, zBas1, m_padOption, TPixel(m_fillValue)) +
           perc[2] *
             getImage3DPixelValue(img, width, height, depth, xBas0, yBas1, zBas0, m_padOption, TPixel(m_fillValue)) +
           perc[3] *
             getImage3DPixelValue(img, width, height, depth, xBas0, yBas1, zBas1, m_padOption, TPixel(m_fillValue)) +
           perc[4] *
             getImage3DPixelValue(img, width, height, depth, xBas1, yBas0, zBas0, m_padOption, TPixel(m_fillValue)) +
           perc[5] *
             getImage3DPixelValue(img, width, height, depth, xBas1, yBas0, zBas1, m_padOption, TPixel(m_fillValue)) +
           perc[6] *
             getImage3DPixelValue(img, width, height, depth, xBas1, yBas1, zBas0, m_padOption, TPixel(m_fillValue)) +
           perc[7] *
             getImage3DPixelValue(img, width, height, depth, xBas1, yBas1, zBas1, m_padOption, TPixel(m_fillValue));
  } else if (m_interpolant == Interpolant::Cubic) {
    // http://en.wikipedia.org/wiki/Bicubic_interpolation
    index_t xn[4], yn[4], zn[4];
    double vecTx[4], vecTy[4], vecTz[4];
    double vecPTx[4], vecPTy[4], vecPTz[4];

    // Determine of the zero neighbor
    index_t xBas0 = std::floor(x);
    index_t yBas0 = std::floor(y);
    index_t zBas0 = std::floor(z);

    // Determine the location in between the pixels 0..1
    double tx = x - xBas0;
    double ty = y - yBas0;
    double tz = z - zBas0;

    // Determine the t vectors
    vecTx[0] = 0.5;
    vecTx[1] = 0.5 * tx;
    vecTx[2] = 0.5 * tx * tx;
    vecTx[3] = 0.5 * tx * tx * tx;
    vecTy[0] = 0.5;
    vecTy[1] = 0.5 * ty;
    vecTy[2] = 0.5 * ty * ty;
    vecTy[3] = 0.5 * ty * ty * ty;
    vecTz[0] = 0.5;
    vecTz[1] = 0.5 * tz;
    vecTz[2] = 0.5 * tz * tz;
    vecTz[3] = 0.5 * tz * tz * tz;

    // t vector multiplied with 4x4 bicubic kernel gives the to q vectors
    vecPTx[0] = -1.0 * vecTx[1] + 2.0 * vecTx[2] - 1.0 * vecTx[3];
    vecPTx[1] = 2.0 * vecTx[0] - 5.0 * vecTx[2] + 3.0 * vecTx[3];
    vecPTx[2] = 1.0 * vecTx[1] + 4.0 * vecTx[2] - 3.0 * vecTx[3];
    vecPTx[3] = -1.0 * vecTx[2] + 1.0 * vecTx[3];
    vecPTy[0] = -1.0 * vecTy[1] + 2.0 * vecTy[2] - 1.0 * vecTy[3];
    vecPTy[1] = 2.0 * vecTy[0] - 5.0 * vecTy[2] + 3.0 * vecTy[3];
    vecPTy[2] = 1.0 * vecTy[1] + 4.0 * vecTy[2] - 3.0 * vecTy[3];
    vecPTy[3] = -1.0 * vecTy[2] + 1.0 * vecTy[3];
    vecPTz[0] = -1.0 * vecTz[1] + 2.0 * vecTz[2] - 1.0 * vecTz[3];
    vecPTz[1] = 2.0 * vecTz[0] - 5.0 * vecTz[2] + 3.0 * vecTz[3];
    vecPTz[2] = 1.0 * vecTz[1] + 4.0 * vecTz[2] - 3.0 * vecTz[3];
    vecPTz[3] = -1.0 * vecTz[2] + 1.0 * vecTz[3];

    // Determine 1D neighbour coordinates
    xn[0] = xBas0 - 1;
    xn[1] = xBas0;
    xn[2] = xBas0 + 1;
    xn[3] = xBas0 + 2;
    yn[0] = yBas0 - 1;
    yn[1] = yBas0;
    yn[2] = yBas0 + 1;
    yn[3] = yBas0 + 2;
    zn[0] = zBas0 - 1;
    zn[1] = zBas0;
    zn[2] = zBas0 + 1;
    zn[3] = zBas0 + 2;

    // First do interpolation in the x direction followed by interpolation in the y and z direction
    double res = 0.0;
    for (auto j = 0; j < 4; ++j) {
      double Ipixelxy = 0.0;
      for (auto i = 0; i < 4; ++i) {
        Ipixelxy +=
          vecPTy[i] *
          (vecPTx[0] *
             getImage3DPixelValue(img, width, height, depth, xn[0], yn[i], zn[j], m_padOption, TPixel(m_fillValue)) +
           vecPTx[1] *
             getImage3DPixelValue(img, width, height, depth, xn[1], yn[i], zn[j], m_padOption, TPixel(m_fillValue)) +
           vecPTx[2] *
             getImage3DPixelValue(img, width, height, depth, xn[2], yn[i], zn[j], m_padOption, TPixel(m_fillValue)) +
           vecPTx[3] *
             getImage3DPixelValue(img, width, height, depth, xn[3], yn[i], zn[j], m_padOption, TPixel(m_fillValue)));
      }
      res += vecPTz[j] * Ipixelxy;
    }
    return res;
  }
  return 0.0;
}

template double
ZImageInterpolation::sample<uint8_t>(const uint8_t* img, size_t width, size_t height, double x, double y) const;

template double
ZImageInterpolation::sample<uint16_t>(const uint16_t* img, size_t width, size_t height, double x, double y) const;

template double
ZImageInterpolation::sample<uint32_t>(const uint32_t* img, size_t width, size_t height, double x, double y) const;

template double
ZImageInterpolation::sample<uint64_t>(const uint64_t* img, size_t width, size_t height, double x, double y) const;

template double
ZImageInterpolation::sample<int8_t>(const int8_t* img, size_t width, size_t height, double x, double y) const;

template double
ZImageInterpolation::sample<int16_t>(const int16_t* img, size_t width, size_t height, double x, double y) const;

template double
ZImageInterpolation::sample<int32_t>(const int32_t* img, size_t width, size_t height, double x, double y) const;

template double
ZImageInterpolation::sample<int64_t>(const int64_t* img, size_t width, size_t height, double x, double y) const;

template double
ZImageInterpolation::sample<float>(const float* img, size_t width, size_t height, double x, double y) const;

template double
ZImageInterpolation::sample<double>(const double* img, size_t width, size_t height, double x, double y) const;

template double ZImageInterpolation::sample<uint8_t>(const uint8_t* img,
                                                     size_t width,
                                                     size_t height,
                                                     size_t depth,
                                                     double x,
                                                     double y,
                                                     double z) const;

template double ZImageInterpolation::sample<uint16_t>(const uint16_t* img,
                                                      size_t width,
                                                      size_t height,
                                                      size_t depth,
                                                      double x,
                                                      double y,
                                                      double z) const;

template double ZImageInterpolation::sample<uint32_t>(const uint32_t* img,
                                                      size_t width,
                                                      size_t height,
                                                      size_t depth,
                                                      double x,
                                                      double y,
                                                      double z) const;

template double ZImageInterpolation::sample<uint64_t>(const uint64_t* img,
                                                      size_t width,
                                                      size_t height,
                                                      size_t depth,
                                                      double x,
                                                      double y,
                                                      double z) const;

template double ZImageInterpolation::sample<int8_t>(const int8_t* img,
                                                    size_t width,
                                                    size_t height,
                                                    size_t depth,
                                                    double x,
                                                    double y,
                                                    double z) const;

template double ZImageInterpolation::sample<int16_t>(const int16_t* img,
                                                     size_t width,
                                                     size_t height,
                                                     size_t depth,
                                                     double x,
                                                     double y,
                                                     double z) const;

template double ZImageInterpolation::sample<int32_t>(const int32_t* img,
                                                     size_t width,
                                                     size_t height,
                                                     size_t depth,
                                                     double x,
                                                     double y,
                                                     double z) const;

template double ZImageInterpolation::sample<int64_t>(const int64_t* img,
                                                     size_t width,
                                                     size_t height,
                                                     size_t depth,
                                                     double x,
                                                     double y,
                                                     double z) const;

template double ZImageInterpolation::sample<float>(const float* img,
                                                   size_t width,
                                                   size_t height,
                                                   size_t depth,
                                                   double x,
                                                   double y,
                                                   double z) const;

template double ZImageInterpolation::sample<double>(const double* img,
                                                    size_t width,
                                                    size_t height,
                                                    size_t depth,
                                                    double x,
                                                    double y,
                                                    double z) const;

} // namespace nim
