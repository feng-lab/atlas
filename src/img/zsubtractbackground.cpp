#include "zsubtractbackground.h"

#include "zcancellation.h"
#include "zexception.h"
#include "zimg.h"
#include "zlog.h"

#include <QFileInfo>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace nim {

namespace {

template<typename TVoxel>
struct HistogramRange
{
  std::vector<size_t> counts;
  int minValue = 0;
  int maxValue = 0;
};

template<typename TVoxel>
HistogramRange<TVoxel> buildHistogramFullRange(const ZImg& img)
{
  CHECK(img.isType<TVoxel>());
  CHECK(img.numChannels() == 1);
  CHECK(img.numTimes() == 1);

  const size_t voxelNumber = img.voxelNumber();
  CHECK(voxelNumber > 0);

  constexpr int kMin = static_cast<int>(std::numeric_limits<TVoxel>::min());
  constexpr int kMax = static_cast<int>(std::numeric_limits<TVoxel>::max());
  static_assert(kMin == 0, "Expected unsigned voxel type.");

  HistogramRange<TVoxel> out;
  out.counts.assign(static_cast<size_t>(kMax) + 1, size_t{0});

  const TVoxel* data = img.timeData<TVoxel>(0);
  for (size_t i = 0; i < voxelNumber; ++i) {
    ++out.counts[static_cast<size_t>(data[i])];
  }

  int minV = 0;
  while (minV < kMax && out.counts[static_cast<size_t>(minV)] == 0) {
    ++minV;
  }
  int maxV = kMax;
  while (maxV > 0 && out.counts[static_cast<size_t>(maxV)] == 0) {
    --maxV;
  }

  out.minValue = minV;
  out.maxValue = maxV;
  return out;
}

int histogramModeInRange(const std::vector<size_t>& counts, int minV, int maxV)
{
  CHECK(minV <= maxV);
  CHECK(minV >= 0);
  CHECK(maxV < static_cast<int>(counts.size()));

  int m = minV;
  size_t maxCount = counts[static_cast<size_t>(m)];
  for (int v = minV; v <= maxV; ++v) {
    const size_t c = counts[static_cast<size_t>(v)];
    if (c > maxCount) {
      maxCount = c;
      m = v;
    }
  }
  return m;
}

template<typename TVoxel>
int subtractBackgroundInPlaceLegacyLike(ZImg& img, double minFr, int maxIter)
{
  if (img.isEmpty()) {
    return 0;
  }
  CHECK(img.numChannels() == 1);
  CHECK(img.numTimes() == 1);

  if (minFr <= 0.0) {
    minFr = 0.0;
  }
  if (minFr > 1.0) {
    minFr = 1.0;
  }
  if (maxIter < 0) {
    maxIter = 0;
  }

  HistogramRange<TVoxel> hist = buildHistogramFullRange<TVoxel>(img);
  const int maxV = hist.maxValue;
  const size_t totalCount = img.voxelNumber();
  CHECK(totalCount > 0);

  int commonIntensity = 0;

  const size_t darkCount = hist.counts[0];
  if (static_cast<double>(darkCount) / static_cast<double>(totalCount) > 0.9) {
    commonIntensity = hist.minValue;
  } else {
    // Port of legacy ZStackProcessor::SubtractBackground(Stack*, minFr, maxIter).
    std::vector<size_t> upper(hist.counts.size() + 1, size_t{0});
    for (int v = static_cast<int>(hist.counts.size()) - 1; v >= 0; --v) {
      upper[static_cast<size_t>(v)] = upper[static_cast<size_t>(v) + 1] + hist.counts[static_cast<size_t>(v)];
    }

    for (int iter = 0; iter < maxIter; ++iter) {
      const int mode = histogramModeInRange(hist.counts, commonIntensity + 1, maxV);
      if (mode == maxV) {
        break;
      }
      commonIntensity = mode;

      const size_t upperCount = upper[static_cast<size_t>(std::clamp(commonIntensity + 1, 0, maxV + 1))];
      const double fgRatio = static_cast<double>(upperCount) / static_cast<double>(totalCount);
      if (fgRatio < minFr) {
        break;
      }
    }
  }

  if (commonIntensity > 0) {
    // Saturating subtract matches legacy Stack_Subc for unsigned types.
    img -= commonIntensity;
  }

  return commonIntensity;
}

} // namespace

void ZSubtractBackground::doWork()
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
  if (m_maxIterations < 0) {
    throw ZException("Max iterations can not be negative.");
  }

  maybeCancel(m_cancellationToken);

  ZImg img(m_inputImagePath);
  if (img.isEmpty()) {
    throw ZException("Failed to read input image (empty image).");
  }
  if (img.numTimes() != 1) {
    throw ZException("Subtract Background only supports single-time images.");
  }
  if (m_channel >= static_cast<int>(img.numChannels())) {
    throw ZException(fmt::format("Channel {} is out of range for this image.", m_channel + 1));
  }

  LOG(INFO) << "Subtract Background: input=" << m_inputImagePath << " channel=" << (m_channel + 1)
            << " minFr=" << m_minForegroundRatio << " maxIter=" << m_maxIterations << " output=" << m_outputImagePath;

  // Work on a single-channel copy.
  const ZImg view = img.createView(/*c*/ m_channel, /*t*/ 0);
  ZImg out(view);

  maybeCancel(m_cancellationToken);

  int commonIntensity = 0;
  if (out.isType<uint8_t>()) {
    commonIntensity = subtractBackgroundInPlaceLegacyLike<uint8_t>(out, m_minForegroundRatio, m_maxIterations);
  } else if (out.isType<uint16_t>()) {
    commonIntensity = subtractBackgroundInPlaceLegacyLike<uint16_t>(out, m_minForegroundRatio, m_maxIterations);
  } else {
    throw ZException(
      fmt::format("Subtract Background only supports 8-bit and 16-bit unsigned images. Got {}", out.info()));
  }

  maybeCancel(m_cancellationToken);

  LOG(INFO) << "Subtract Background: commonIntensity=" << commonIntensity;
  out.save(m_outputImagePath);

  LOG(INFO) << "Subtract Background: wrote " << QFileInfo(m_outputImagePath).fileName();
  reportProgress(1.0);
}

void ZSubtractBackground::read(const json::object& jo)
{
  m_inputImagePath = json::value_to<QString>(jo.at("input_image"));
  m_outputImagePath = json::value_to<QString>(jo.at("output_image"));
  m_channel = static_cast<int>(json::value_to<int64_t>(jo.at("channel")));
  m_minForegroundRatio = json::value_to<double>(jo.at("min_foreground_ratio"));
  m_maxIterations = static_cast<int>(json::value_to<int64_t>(jo.at("max_iterations")));
}

void ZSubtractBackground::write(json::object& jo) const
{
  jo["input_image"] = json::value_from(m_inputImagePath);
  jo["output_image"] = json::value_from(m_outputImagePath);
  jo["channel"] = json::value_from(static_cast<int64_t>(m_channel));
  jo["min_foreground_ratio"] = json::value_from(m_minForegroundRatio);
  jo["max_iterations"] = json::value_from(static_cast<int64_t>(m_maxIterations));
}

} // namespace nim
