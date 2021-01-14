#include "zimagefilterkernel.h"

#include <boost/math/constants/constants.hpp>
#include <cmath>

namespace nim {

template<typename TFloat>
std::vector<TFloat> create1DGaussianKernel(TFloat sigmaX, index_t width, size_t* kWidth)
{
  if (width < 0) {
    width = std::ceil(sigmaX * 6.0);
    if (width % 2 == 0)
      ++width;
  }

  if (kWidth)
    *kWidth = width;

  std::vector<TFloat> res(width);
  TFloat sum = 0.0;
  double sigmaX22 = 2.0 * sigmaX * sigmaX;
  double centerX = (width - 1.0) / 2.0;

  size_t idx = 0;

  for (index_t i = 0; i < width; ++i) {
    double distToCenterX2 = (i - centerX) * (i - centerX);
    res[idx++] = std::exp(-distToCenterX2 / sigmaX22);
    sum += res[idx - 1];
  }


  for (auto i = 0; i < width; ++i) {
    res[i] /= sum;
  }

  return res;
}

template std::vector<float> create1DGaussianKernel(float, index_t, size_t*);

template std::vector<double> create1DGaussianKernel(double, index_t, size_t*);


template<typename TFloat>
std::vector<TFloat> create2DGaussianKernel(TFloat sigmaX, TFloat sigmaY,
                                           index_t width, index_t height,
                                           size_t* kWidth, size_t* kHeight)
{
  if (width < 0) {
    width = std::ceil(sigmaX * 6.0);
    if (width % 2 == 0)
      ++width;
  }
  if (height < 0) {
    height = std::ceil(sigmaY * 6.0);
    if (height % 2 == 0)
      ++height;
  }
  if (kWidth)
    *kWidth = width;
  if (kHeight)
    *kHeight = height;
  std::vector<TFloat> res(width * height);
  TFloat sum = 0.0;
  double sigmaX22 = 2.0 * sigmaX * sigmaX;
  double sigmaY22 = 2.0 * sigmaY * sigmaY;
  double centerX = (width - 1.0) / 2.0;
  double centerY = (height - 1.0) / 2.0;

  size_t idx = 0;
  for (index_t j = 0; j < height; ++j) {
    double distToCenterY2 = (j - centerY) * (j - centerY);
    for (index_t i = 0; i < width; ++i) {
      double distToCenterX2 = (i - centerX) * (i - centerX);
      res[idx++] = std::exp(-distToCenterX2 / sigmaX22 - distToCenterY2 / sigmaY22);
      sum += res[idx - 1];
    }
  }

  for (auto i = 0_z; i < height * width; ++i) {
    res[i] /= sum;
  }

  return res;
}

template std::vector<float> create2DGaussianKernel(float, float, index_t, index_t, size_t*, size_t*);

template std::vector<double> create2DGaussianKernel(double, double, index_t, index_t, size_t*, size_t*);


template<typename TFloat>
std::vector<TFloat> create3DGaussianKernel(TFloat sigmaX, TFloat sigmaY, TFloat sigmaZ,
                                           index_t width, index_t height, index_t depth,
                                           size_t* kWidth, size_t* kHeight, size_t* kDepth)
{
  if (width < 0) {
    width = std::ceil(sigmaX * 6.0);
    if (width % 2 == 0)
      ++width;
  }
  if (height < 0) {
    height = std::ceil(sigmaY * 6.0);
    if (height % 2 == 0)
      ++height;
  }
  if (depth < 0) {
    depth = std::ceil(sigmaZ * 6.0);
    if (depth % 2 == 0)
      ++depth;
  }
  if (kWidth)
    *kWidth = width;
  if (kHeight)
    *kHeight = height;
  if (kDepth)
    *kDepth = depth;
  std::vector<TFloat> res(width * height * depth);
  TFloat sum = 0.0;
  double sigmaX22 = 2.0 * sigmaX * sigmaX;
  double sigmaY22 = 2.0 * sigmaY * sigmaY;
  double sigmaZ22 = 2.0 * sigmaZ * sigmaZ;
  double centerX = (width - 1.0) / 2.0;
  double centerY = (height - 1.0) / 2.0;
  double centerZ = (depth - 1.0) / 2.0;

  size_t idx = 0;
  for (index_t k = 0; k < depth; ++k) {
    double distToCenterZ2 = (k - centerZ) * (k - centerZ);
    for (index_t j = 0; j < height; ++j) {
      double distToCenterY2 = (j - centerY) * (j - centerY);
      for (index_t i = 0; i < width; ++i) {
        double distToCenterX2 = (i - centerX) * (i - centerX);
        res[idx++] = std::exp(-distToCenterX2 / sigmaX22 - distToCenterY2 / sigmaY22 - distToCenterZ2 / sigmaZ22);
        sum += res[idx - 1];
      }
    }
  }

  for (auto i = 0_z; i < height * width * depth; ++i) {
    res[i] /= sum;
  }

  return res;
}

template std::vector<float> create3DGaussianKernel(float, float, float, index_t, index_t, index_t, size_t*, size_t*, size_t*);

template std::vector<double> create3DGaussianKernel(double, double, double, index_t, index_t, index_t, size_t*, size_t*, size_t*);


template<typename TFloat>
std::vector<TFloat> create1DLoGKernel(TFloat sigmaX,
                                      index_t width,
                                      size_t* kWidth)
{
  if (width < 0) {
    width = std::ceil(sigmaX * 6.0);
    if (width % 2 == 0)
      ++width;
  }

  if (kWidth)
    *kWidth = width;

  std::vector<TFloat> res(width);
  TFloat sum = 0.0;
  double sigmaX2 = sigmaX * sigmaX;
  double sigmaX4 = sigmaX2 * sigmaX2;
  double sigmaX22 = 2.0 * sigmaX2;
  double centerX = (width - 1.0) / 2.0;

  size_t idx = 0;

  // (1/(sqrt(2*pi)*s)) * (x.^2/s^4 - 1/s^2 ) .* exp(-x.^2/(2*s^2));
  for (index_t i = 0; i < width; ++i) {
    double distToCenterX2 = (i - centerX) * (i - centerX);
    res[idx++] = 1.0 / (std::sqrt(boost::math::double_constants::two_pi) * sigmaX) *
                 (distToCenterX2 / sigmaX4 - 1.0 / sigmaX2) * std::exp(-distToCenterX2 / sigmaX22);
    sum += res[idx - 1];
  }
  double mean = sum / width;

  for (index_t i = 0; i < width; ++i) {
    res[i] -= mean;
  }

  return res;
}

template std::vector<float> create1DLoGKernel(float, index_t, size_t*);

template std::vector<double> create1DLoGKernel(double, index_t, size_t*);

}  // namespace nim
