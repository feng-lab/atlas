#include "zbinarizeimage.h"

#include "zcancellation.h"
#include "zexception.h"
#include "zimg.h"
#include "zimgautothreshold.h"
#include "zlog.h"

#include <QFileInfo>

#include <string>
#include <string_view>

namespace nim {

namespace {

[[nodiscard]] const char* thresholdModeToString(ZBinarizeImage::ThresholdMode mode)
{
  switch (mode) {
    case ZBinarizeImage::ThresholdMode::Manual:
      return "manual";
    case ZBinarizeImage::ThresholdMode::AutoLocmax:
      return "auto_locmax";
  }
  return "unknown";
}

[[nodiscard]] ZBinarizeImage::ThresholdMode thresholdModeFromString(std::string_view s)
{
  if (s == "manual") {
    return ZBinarizeImage::ThresholdMode::Manual;
  }
  if (s == "auto_locmax") {
    return ZBinarizeImage::ThresholdMode::AutoLocmax;
  }
  throw ZException(fmt::format("Unknown threshold_mode value: {}", s));
}

} // namespace

void ZBinarizeImage::doWork()
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
  if (m_threshold < 0) {
    throw ZException("Threshold can not be negative.");
  }
  if (m_autoThresholdRetryCount <= 0) {
    throw ZException("Auto-threshold retry count must be positive.");
  }

  maybeCancel(m_cancellationToken);

  ZImg img(m_inputImagePath);
  if (img.isEmpty()) {
    throw ZException("Failed to read input image (empty image).");
  }
  if (img.numTimes() != 1) {
    throw ZException("Binarize only supports single-time images.");
  }
  if (m_channel >= static_cast<int>(img.numChannels())) {
    throw ZException(fmt::format("Channel {} is out of range for this image.", m_channel + 1));
  }

  LOG(INFO) << "Binarize: input=" << m_inputImagePath << " channel=" << (m_channel + 1)
            << " threshold_mode=" << thresholdModeToString(m_thresholdMode) << " threshold=" << m_threshold
            << " output=" << m_outputImagePath;

  const ZImg view = img.createView(/*c*/ m_channel, /*t*/ 0);
  maybeCancel(m_cancellationToken);

  int actualThreshold = m_threshold;
  if (m_thresholdMode == ThresholdMode::AutoLocmax) {
    ZImgAutoThreshold autoThre;
    autoThre.setCancellationToken(m_cancellationToken);
    const auto thresholdOpt =
      autoThre.locmaxThreNeuTube(img, static_cast<size_t>(m_channel), 0, m_autoThresholdRetryCount);
    if (!thresholdOpt) {
      throw ZException("Auto threshold failed on this image/channel. Please try Manual threshold.");
    }
    actualThreshold = *thresholdOpt;
    LOG(INFO) << "Binarize: auto threshold=" << actualThreshold;
  }

  // Legacy neuTube `Stack_Threshold_Binarize`: foreground is strictly greater than threshold.
  const ZImg binary = view.binarized(actualThreshold, ZImg::ThresholdMode::ExcludeThreshold);
  maybeCancel(m_cancellationToken);

  binary.save(m_outputImagePath);

  LOG(INFO) << "Binarize: wrote " << QFileInfo(m_outputImagePath).fileName();
  reportProgress(1.0);
}

void ZBinarizeImage::read(const json::object& jo)
{
  m_inputImagePath = json::value_to<QString>(jo.at("input_image"));
  m_outputImagePath = json::value_to<QString>(jo.at("output_image"));
  m_channel = static_cast<int>(json::value_to<int64_t>(jo.at("channel")));
  m_threshold = static_cast<int>(json::value_to<int64_t>(jo.at("threshold")));

  if (auto it = jo.find("threshold_mode"); it != jo.end()) {
    const std::string s = json::value_to<std::string>(it->value());
    m_thresholdMode = thresholdModeFromString(s);
  }

  if (auto it = jo.find("auto_threshold_retry_count"); it != jo.end()) {
    m_autoThresholdRetryCount = static_cast<int>(json::value_to<int64_t>(it->value()));
  }
}

void ZBinarizeImage::write(json::object& jo) const
{
  jo["input_image"] = json::value_from(m_inputImagePath);
  jo["output_image"] = json::value_from(m_outputImagePath);
  jo["channel"] = json::value_from(static_cast<int64_t>(m_channel));
  jo["threshold"] = json::value_from(static_cast<int64_t>(m_threshold));
  jo["threshold_mode"] = json::value_from(std::string(thresholdModeToString(m_thresholdMode)));
  jo["auto_threshold_retry_count"] = json::value_from(static_cast<int64_t>(m_autoThresholdRetryCount));
}

} // namespace nim
