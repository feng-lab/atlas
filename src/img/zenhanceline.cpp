#include "zenhanceline.h"

#include "zcancellation.h"
#include "zexception.h"
#include "zimg.h"
#include "zimgitkinterface.h"
#include "zlog.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <itkCastImageFilter.h>
#include <itkHessianRecursiveGaussianImageFilter.h>
#include <itkImageRegionConstIterator.h>

namespace nim {

namespace {

int solveCubicLegacyLike(double a, double b, double c, double d, double* sol)
{
  // Port of neuTube `Solve_Cubic` (tz_math.c).
  constexpr double kSolveCubicMind = 1e-5;

  const double u = b / 3.0 / a;
  const double q = c / 3.0 / a - u * u;
  const double r = (9.0 * a * b * c - 27.0 * a * a * d - 2.0 * b * b * b) / 54.0 / a / a / a;
  double delta = q * q * q + r * r;

  if (delta > 0.0) {
    if (delta < kSolveCubicMind) {
      delta = 0.0;
    } else {
      const double delta2 = std::sqrt(delta);
      const double s = std::cbrt(r + delta2);
      const double t = std::cbrt(r - delta2);
      sol[0] = s + t - u;
      sol[1] = 0.0;
      sol[2] = 0.0;
      return 1;
    }
  }

  if (delta < 0.0) {
    double rho = std::sqrt(r * r - delta);
    const double theta = std::acos(r / rho) / 3.0;
    rho = std::cbrt(rho);
    const double st = rho * std::cos(theta);
    const double s_t = std::sqrt(3.0) * rho * std::sin(theta);
    sol[0] = 2.0 * st - u;
    sol[1] = -st - u - s_t;
    sol[2] = -st - u + s_t;
    return 3;
  }

  if (delta == 0.0) {
    sol[0] = std::cbrt(r) * 2.0 - u;
    sol[1] = -std::cbrt(r) - u;
    sol[2] = sol[1];
    return 2;
  }

  return 0;
}

double eigen3SolutionScoreLegacyLike(double a, double b, double c, double d, double e, double f)
{
  // Port of FMatrix_Eigen3_Solution_Score (tz_matrix.c.t), using the same Solve_Cubic and swapping logic.
  if (a + b + c >= 0.0) {
    return 0.0;
  }

  double roots[3];
  double coeff[3];
  coeff[0] = -a - b - c;
  coeff[1] = a * b + b * c + a * c - e * e - d * d - f * f;
  coeff[2] = -a * b * c - 2.0 * d * e * f + a * f * f + c * d * d + b * e * e;

  if (solveCubicLegacyLike(1.0, coeff[0], coeff[1], coeff[2], roots) <= 0) {
    return 0.0;
  }

  // NOTE: Keep the same partial ordering semantics as legacy:
  // only ensure roots[0] is not smaller than roots[1]/roots[2] in a specific pattern.
  if (roots[0] < roots[1]) {
    std::swap(roots[0], roots[1]);
    if (roots[0] < roots[2]) {
      std::swap(roots[0], roots[2]);
    }
  }

  double score = 0.0;
  if (roots[1] >= 0.0 || roots[2] >= 0.0) {
    score = 0.0;
  } else {
    score = std::sqrt(roots[1] * roots[2]);
    if (roots[0] > 0.0) {
      roots[0] = -roots[0] / 2.0;
    }
  }

  score += roots[0];
  if (score < 0.0) {
    score = 0.0;
  }
  return score;
}

template<typename TVoxel>
ZImg enhanceLineOnSingleChannelLegacyLike(const ZImg& in, double sigma, folly::CancellationToken token)
{
  CHECK(in.numChannels() == 1);
  CHECK(in.numTimes() == 1);
  CHECK(in.isType<TVoxel>());

  if (sigma <= 0.0) {
    throw ZException("Sigma must be positive.");
  }

  using InputImageType = itk::Image<TVoxel, 3>;
  using FloatImageType = itk::Image<float, 3>;
  using CastFilterType = itk::CastImageFilter<InputImageType, FloatImageType>;
  using HessianFilterType = itk::HessianRecursiveGaussianImageFilter<FloatImageType>;

  maybeCancel(token);

  const auto itkIn = wrapZImgChannelAsITKImg<TVoxel>(in, /*c*/ 0, /*t*/ 0);

  auto cast = CastFilterType::New();
  cast->SetInput(itkIn);

  auto hessian = HessianFilterType::New();
  hessian->SetInput(cast->GetOutput());
  hessian->SetSigma(sigma);
  hessian->SetNormalizeAcrossScale(false);

  hessian->Update();
  maybeCancel(token);

  const auto* hessianImg = hessian->GetOutput();
  CHECK(hessianImg != nullptr);

  const size_t voxelNumber = in.voxelNumber();
  std::vector<double> score(voxelNumber, 0.0);

  double minScore = std::numeric_limits<double>::infinity();
  double maxScore = -std::numeric_limits<double>::infinity();

  itk::ImageRegionConstIterator<typename HessianFilterType::OutputImageType> it(hessianImg,
                                                                                hessianImg->GetLargestPossibleRegion());
  size_t idx = 0;
  for (it.GoToBegin(); !it.IsAtEnd(); ++it, ++idx) {
    const auto& tensor = it.Get();
    const double a = tensor(0, 0);
    const double b = tensor(1, 1);
    const double c = tensor(2, 2);
    const double d = tensor(0, 1);
    const double e = tensor(0, 2);
    const double f = tensor(1, 2);

    const double s = eigen3SolutionScoreLegacyLike(a, b, c, d, e, f);
    score[idx] = s;
    minScore = std::min(minScore, s);
    maxScore = std::max(maxScore, s);
  }

  if (idx != voxelNumber) {
    throw ZException("Enhance Line: internal size mismatch.");
  }

  const double maxDiff = maxScore - minScore;
  ZImg out(in.info());
  TVoxel* outData = out.timeData<TVoxel>(0);

  if (!(maxDiff > 0.0)) {
    std::fill_n(outData, voxelNumber, TVoxel{0});
    return out;
  }

  const double destMax = static_cast<double>(std::numeric_limits<TVoxel>::max());
  for (size_t i = 0; i < voxelNumber; ++i) {
    const double scaled = destMax * (score[i] - minScore) / maxDiff;
    const double clamped = std::clamp(scaled, 0.0, destMax);
    outData[i] = static_cast<TVoxel>(clamped);
  }
  return out;
}

} // namespace

void ZEnhanceLine::doWork()
{
  if (m_inputImagePath.trimmed().isEmpty()) {
    throw ZException("Input image path can not be empty.");
  }
  if (m_outputImagePath.trimmed().isEmpty()) {
    throw ZException("Output image path can not be empty.");
  }
  if (m_channel < 0) {
    throw ZException("Channel can not be negative.");
  }
  if (!(m_sigma > 0.0)) {
    throw ZException("Sigma must be positive.");
  }

  maybeCancel(m_cancellationToken);

  ZImg img(m_inputImagePath);
  if (img.isEmpty()) {
    throw ZException("Failed to read input image (empty image).");
  }
  if (img.numTimes() != 1) {
    throw ZException("Enhance Line only supports single-time images.");
  }
  if (m_channel >= static_cast<int>(img.numChannels())) {
    throw ZException(fmt::format("Channel {} is out of range for this image.", m_channel + 1));
  }

  LOG(INFO) << "Enhance Line: input=" << m_inputImagePath << " channel=" << (m_channel + 1) << " sigma=" << m_sigma
            << " output=" << m_outputImagePath;

  const ZImg view = img.createView(/*c*/ m_channel, /*t*/ 0);

  maybeCancel(m_cancellationToken);

  ZImg out;
  if (view.isType<uint8_t>()) {
    out = enhanceLineOnSingleChannelLegacyLike<uint8_t>(view, m_sigma, m_cancellationToken);
  } else if (view.isType<uint16_t>()) {
    out = enhanceLineOnSingleChannelLegacyLike<uint16_t>(view, m_sigma, m_cancellationToken);
  } else {
    throw ZException(fmt::format("Enhance Line only supports 8-bit and 16-bit unsigned images. Got {}", view.info()));
  }

  maybeCancel(m_cancellationToken);

  out.save(m_outputImagePath);

  LOG(INFO) << "Enhance Line: wrote " << QFileInfo(m_outputImagePath).fileName();
  reportProgress(1.0);
}

void ZEnhanceLine::read(const json::object& jo)
{
  m_inputImagePath = json::value_to<QString>(jo.at("input_image"));
  m_outputImagePath = json::value_to<QString>(jo.at("output_image"));
  m_channel = static_cast<int>(json::value_to<int64_t>(jo.at("channel")));
  m_sigma = jo.at("sigma").to_number<double>();
}

void ZEnhanceLine::write(json::object& jo) const
{
  jo["input_image"] = json::value_from(m_inputImagePath);
  jo["output_image"] = json::value_from(m_outputImagePath);
  jo["channel"] = json::value_from(static_cast<int64_t>(m_channel));
  jo["sigma"] = json::value_from(m_sigma);
}

} // namespace nim
