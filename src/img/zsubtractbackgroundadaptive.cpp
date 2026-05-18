#include "zsubtractbackgroundadaptive.h"

#include "zcancellation.h"
#include "zexception.h"
#include "zimg.h"
#include "zlog.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace nim {

namespace {

template<typename TVoxel>
void subtractBackgroundAdaptiveLegacyLike(const std::vector<TVoxel>& src,
                                          int width,
                                          int height,
                                          int depth,
                                          int nsample,
                                          int stride,
                                          TVoxel* out)
{
  CHECK(width > 0);
  CHECK(height > 0);
  CHECK(depth > 0);
  CHECK(nsample > 0);
  CHECK(stride > 0);
  CHECK(out != nullptr);

  const size_t area = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t voxelCount = area * static_cast<size_t>(depth);
  CHECK(src.size() == voxelCount);

  size_t offset = 0;
  for (int z = 0; z < depth; ++z) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        double backgroundSum = 0.0;
        int count = 0;
        for (int step = 1; step <= nsample; ++step) {
          const int dist = stride * step;

          if (x >= dist) { // along -x
            backgroundSum += static_cast<double>(src[offset - static_cast<size_t>(dist)]);
            ++count;
          }
          if (x + dist < width) { // along +x
            backgroundSum += static_cast<double>(src[offset + static_cast<size_t>(dist)]);
            ++count;
          }
          if (y >= dist) { // along -y
            backgroundSum += static_cast<double>(src[offset - static_cast<size_t>(dist) * static_cast<size_t>(width)]);
            ++count;
          }
          if (y + dist < height) { // along +y
            backgroundSum += static_cast<double>(src[offset + static_cast<size_t>(dist) * static_cast<size_t>(width)]);
            ++count;
          }
          if (z >= dist) { // along -z
            backgroundSum += static_cast<double>(src[offset - static_cast<size_t>(dist) * area]);
            ++count;
          }
          if (z + dist < depth) { // along +z
            backgroundSum += static_cast<double>(src[offset + static_cast<size_t>(dist) * area]);
            ++count;
          }
        }

        if (count > 0) {
          const double diff = static_cast<double>(src[offset]) - backgroundSum / static_cast<double>(count);
          if (diff < 0.0) {
            out[offset] = TVoxel{0};
          } else {
            // Match legacy neutu `iround()` behavior (nearest int; ties away from zero).
            out[offset] = static_cast<TVoxel>(std::lround(diff));
          }
        } else {
          out[offset] = src[offset];
        }

        ++offset;
      }
    }
  }
}

} // namespace

void ZSubtractBackgroundAdaptive::doWork()
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
  if (m_numSamples <= 0) {
    throw ZException("Num samples must be positive.");
  }
  if (m_stride <= 0) {
    throw ZException("Stride must be positive.");
  }

  maybeCancel(m_cancellationToken);

  ZImg img(m_inputImagePath);
  if (img.isEmpty()) {
    throw ZException("Failed to read input image (empty image).");
  }
  if (img.numTimes() != 1) {
    throw ZException("Subtract Background (Adaptive) only supports single-time images.");
  }
  if (m_channel >= static_cast<int>(img.numChannels())) {
    throw ZException(fmt::format("Channel {} is out of range for this image.", m_channel + 1));
  }

  LOG(INFO) << "Subtract Background (Adaptive): input=" << m_inputImagePath << " channel=" << (m_channel + 1)
            << " nsample=" << m_numSamples << " stride=" << m_stride << " output=" << m_outputImagePath;

  const ZImg view = img.createView(/*c*/ m_channel, /*t*/ 0);
  ZImg out(view);

  maybeCancel(m_cancellationToken);

  const int width = static_cast<int>(out.width());
  const int height = static_cast<int>(out.height());
  const int depth = static_cast<int>(out.depth());

  if (out.isType<uint8_t>()) {
    const uint8_t* outData = out.timeData<uint8_t>(0);
    std::vector<uint8_t> src(out.voxelNumber());
    std::copy_n(outData, out.voxelNumber(), src.data());
    subtractBackgroundAdaptiveLegacyLike(src, width, height, depth, m_numSamples, m_stride, out.timeData<uint8_t>(0));
  } else if (out.isType<uint16_t>()) {
    const uint16_t* outData = out.timeData<uint16_t>(0);
    std::vector<uint16_t> src(out.voxelNumber());
    std::copy_n(outData, out.voxelNumber(), src.data());
    subtractBackgroundAdaptiveLegacyLike(src, width, height, depth, m_numSamples, m_stride, out.timeData<uint16_t>(0));
  } else {
    throw ZException(
      fmt::format("Subtract Background (Adaptive) only supports 8-bit and 16-bit unsigned images. Got {}", out.info()));
  }

  maybeCancel(m_cancellationToken);

  out.save(m_outputImagePath);

  LOG(INFO) << "Subtract Background (Adaptive): wrote " << QFileInfo(m_outputImagePath).fileName();
  reportProgress(1.0);
}

void ZSubtractBackgroundAdaptive::read(const json::object& jo)
{
  m_inputImagePath = json::value_to<QString>(jo.at("input_image"));
  m_outputImagePath = json::value_to<QString>(jo.at("output_image"));
  m_channel = static_cast<int>(json::value_to<int64_t>(jo.at("channel")));
  m_numSamples = static_cast<int>(json::value_to<int64_t>(jo.at("num_samples")));
  m_stride = static_cast<int>(json::value_to<int64_t>(jo.at("stride")));
}

void ZSubtractBackgroundAdaptive::write(json::object& jo) const
{
  jo["input_image"] = json::value_from(m_inputImagePath);
  jo["output_image"] = json::value_from(m_outputImagePath);
  jo["channel"] = json::value_from(static_cast<int64_t>(m_channel));
  jo["num_samples"] = json::value_from(static_cast<int64_t>(m_numSamples));
  jo["stride"] = json::value_from(static_cast<int64_t>(m_stride));
}

} // namespace nim
