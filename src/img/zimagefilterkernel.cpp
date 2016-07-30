#include "zimagefilterkernel.h"

#include <QtMath>  //for M_PI
#include <cmath>

namespace nim {

template<typename TFloat>
std::vector<TFloat> create1DGaussianKernel(TFloat sigmaX, int width, size_t* kWidth)
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

  for (int i = 0; i < width; ++i) {
    double distToCenterX2 = (i - centerX) * (i - centerX);
    res[idx++] = std::exp(-distToCenterX2 / sigmaX22);
    sum += res[idx - 1];
  }


  for (size_t i = 0; i < static_cast<size_t>(width); ++i) {
    res[i] /= sum;
  }

  return res;
}

template std::vector<float> create1DGaussianKernel(float, int, size_t*);

template std::vector<double> create1DGaussianKernel(double, int, size_t*);


template<typename TFloat>
std::vector<TFloat> create2DGaussianKernel(TFloat sigmaX, TFloat sigmaY,
                                           int width, int height,
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
  std::vector<TFloat> res(static_cast<size_t>(width) * height);
  TFloat sum = 0.0;
  double sigmaX22 = 2.0 * sigmaX * sigmaX;
  double sigmaY22 = 2.0 * sigmaY * sigmaY;
  double centerX = (width - 1.0) / 2.0;
  double centerY = (height - 1.0) / 2.0;

  size_t idx = 0;
  for (int j = 0; j < height; ++j) {
    double distToCenterY2 = (j - centerY) * (j - centerY);
    for (int i = 0; i < width; ++i) {
      double distToCenterX2 = (i - centerX) * (i - centerX);
      res[idx++] = std::exp(-distToCenterX2 / sigmaX22 - distToCenterY2 / sigmaY22);
      sum += res[idx - 1];
    }
  }

  for (size_t i = 0; i < static_cast<size_t>(height) * width; ++i) {
    res[i] /= sum;
  }

  return res;
}

template std::vector<float> create2DGaussianKernel(float, float, int, int, size_t*, size_t*);

template std::vector<double> create2DGaussianKernel(double, double, int, int, size_t*, size_t*);


template<typename TFloat>
std::vector<TFloat> create3DGaussianKernel(TFloat sigmaX, TFloat sigmaY, TFloat sigmaZ,
                                           int width, int height, int depth,
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
  std::vector<TFloat> res(static_cast<size_t>(width) * height * depth);
  TFloat sum = 0.0;
  double sigmaX22 = 2.0 * sigmaX * sigmaX;
  double sigmaY22 = 2.0 * sigmaY * sigmaY;
  double sigmaZ22 = 2.0 * sigmaZ * sigmaZ;
  double centerX = (width - 1.0) / 2.0;
  double centerY = (height - 1.0) / 2.0;
  double centerZ = (depth - 1.0) / 2.0;

  size_t idx = 0;
  for (int k = 0; k < depth; ++k) {
    double distToCenterZ2 = (k - centerZ) * (k - centerZ);
    for (int j = 0; j < height; ++j) {
      double distToCenterY2 = (j - centerY) * (j - centerY);
      for (int i = 0; i < width; ++i) {
        double distToCenterX2 = (i - centerX) * (i - centerX);
        res[idx++] = std::exp(-distToCenterX2 / sigmaX22 - distToCenterY2 / sigmaY22 - distToCenterZ2 / sigmaZ22);
        sum += res[idx - 1];
      }
    }
  }

  for (size_t i = 0; i < static_cast<size_t>(height) * width * depth; ++i) {
    res[i] /= sum;
  }

  return res;
}

template std::vector<float> create3DGaussianKernel(float, float, float, int, int, int, size_t*, size_t*, size_t*);

template std::vector<double> create3DGaussianKernel(double, double, double, int, int, int, size_t*, size_t*, size_t*);


template<typename TFloat>
std::vector<TFloat> create1DLoGKernel(TFloat sigmaX,
                                      int width,
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
  for (int i = 0; i < width; ++i) {
    double distToCenterX2 = (i - centerX) * (i - centerX);
    res[idx++] = 1.0 / (std::sqrt(2 * M_PI) * sigmaX) *
                 (distToCenterX2 / sigmaX4 - 1.0 / sigmaX2) * std::exp(-distToCenterX2 / sigmaX22);
    sum += res[idx - 1];
  }
  double mean = sum / width;

  for (size_t i = 0; i < static_cast<size_t>(width); ++i) {
    res[i] -= mean;
  }

  return res;
}

template std::vector<float> create1DLoGKernel(float, int, size_t*);

template std::vector<double> create1DLoGKernel(double, int, size_t*);

}
