#include "zchromaticshiftcorrection.h"

#include "zimg.h"
#include "zstatisticsutils.h"
#include "zregistrationnumericdiffcostfunction.h"
#include "zimage2dutils.h"
#include "zimgregistration.h"
#include "zimagematrix3dtransform.h"
#include "zimagematrix2dtransform.h"
#include "zimagetransformresolve.h"
#include "zlogqttypesupport.h"
#include <algorithm>
#include <memory>
#include <numeric>
#include <utility>

namespace nim {

ZChromaticShiftCorrection::ZChromaticShiftCorrection(const ZImg& img)
  : m_img(img)
{
}

void ZChromaticShiftCorrection::doWork()
{
  LOG(INFO) << "";
  // todo : add back
  //  if (!m_stack.sourcePath()) {
  //    LOG(INFO) << "Start Registering Sections";
  //  } else {
  //    LOG(INFO) << "Start Registering Sections for Image " << m_stack.sourcePath();
  //  }
  //  LOG(INFO) << "";

  LOG(INFO) << "Method: " << m_method;

  if (m_method == "Registration") {
    if (m_referenceChannel >= 0 && static_cast<size_t>(m_referenceChannel) < m_img.numChannels()) {
      LOG(INFO) << "Reference Channel: " << m_referenceChannel + 1 << " (start from 1)";
    } else {
      throw ZImgException(QString("Wrong reference channel: %1. Abort").arg(m_referenceChannel));
    }
  }

  if (m_targetChannel >= 0 && static_cast<size_t>(m_targetChannel) < m_img.numChannels() &&
    (m_method != "Registration" || m_targetChannel != m_referenceChannel)) {
    LOG(INFO) << "Target Channel: " << m_targetChannel + 1 << " (start from 1)";
  } else {
    throw ZImgException(QString("Wrong target channel: %1. Abort").arg(m_targetChannel));
  }

  if (m_method == "Registration") {
    LOG(INFO) << "Remove Background: " << m_removeBackground;
    LOG(INFO) << "Remove High Foreground: " << m_removeHighForeground;
    LOG(INFO) << "Multithreading: " << m_useMultithreading;
    LOG(INFO) << "Metirc: " << m_metric;
    LOG(INFO) << "Transform: " << m_transform;
    LOG(INFO) << "Optimizer: " << m_optimizer;

    IMG_TYPED_CALL(calcChannelInfs, m_img);

    reportProgress(0.5);
    IMG_TYPED_CALL(alignChannel, m_img, m_referenceChannel, m_targetChannel);
    emit resultReady(m_resultFilename);
    reportProgress(1.0);
  } else {
    IMG_TYPED_CALL(calcChannelInfs, m_img);

    reportProgress(0.5);
    IMG_TYPED_CALL(alignChannelWithPresetTransform, m_img, m_targetChannel, m_method);
    emit resultReady(m_resultFilename);
    reportProgress(1.0);
  }
}

template<typename ImagePixelType>
void ZChromaticShiftCorrection::alignChannelWithPresetTransform(int movingChannel, const QString& presetName)
{
  std::map<QString, std::vector<double>> presetNameToParameters = {
    {"40x_1z", {-0.481418, 0.702386, 0.}},
    {"40x_2z", {-1.29442,  1.44593,  0.}},
    {"40x_4z", {-2.56525,  2.57374,  0.}},
    {"60x_1z", {-0.440559, 2.27874,  0.}},
    {"60x_2z", {-0.891503, 4.75318,  0.}},
    {"60x_4z", {-2.137,    10.1926,  0.}}
  };

  std::unique_ptr<ZImageTransform> transform;
  if (m_img.depth() > 1) {
    auto tfm = new ZImageTranslation3DTransform();
    transform.reset(tfm);
  } else {
    auto tfm = new ZImageTranslation2DTransform();
    transform.reset(tfm);
  }

  auto it = presetNameToParameters.find(presetName);
  if (it == presetNameToParameters.end()) {
    throw ZImgException(QString("Unknown preset name: %1. Abort").arg(presetName));
  } else {
    LOG(INFO) << "Use " << it->first << " preset: " << it->second;
    transform->setParameters(it->second);
  }

  // get output image from registered parameters
  transform->setImageInterpolation(ZImageInterpolation(Interpolant::Cubic, PadOption::Constant,
                                                       m_brightBackground ? m_channelInfos[movingChannel].max
                                                                          : m_channelInfos[movingChannel].min));

  ZImg correctedImg = m_img;
  if (m_img.depth() > 1) {
    transform->transformImage(m_img.channelData<ImagePixelType>(movingChannel),
                              m_img.width(), m_img.height(), m_img.depth(),
                              correctedImg.channelData<ImagePixelType>(movingChannel));
  } else {
    transform->transformImage(m_img.channelData<ImagePixelType>(movingChannel),
                              m_img.width(), m_img.height(),
                              correctedImg.channelData<ImagePixelType>(movingChannel));
  }

  correctedImg.save(m_resultFilename);
}

template<typename ImagePixelType>
void ZChromaticShiftCorrection::alignChannel(int fixedChannel, int movingChannel)
{
  LOG(INFO) << "";
  LOG(INFO) << "Registering Channel " << (movingChannel) << " to Channel " << (fixedChannel);

  size_t length = m_img.channelVoxelNumber();
  std::vector<double> fixedImageData(length);
  std::vector<double> movingImageData(length);

  const ImagePixelType* fixedImageDataSrc = m_img.channelData<ImagePixelType>(fixedChannel);
  const ImagePixelType* movingImageDataSrc = m_img.channelData<ImagePixelType>(movingChannel);
  double fixedMin = std::numeric_limits<double>::max();
  double fixedMax = std::numeric_limits<double>::lowest();
  double movingMin = fixedMin;
  double movingMax = fixedMax;
  for (size_t i = 0; i < length; ++i) {
    fixedImageData[i] = fixedImageDataSrc[i];
    fixedMin = std::min(fixedMin, fixedImageData[i]);
    fixedMax = std::max(fixedMax, fixedImageData[i]);
  }
  for (size_t i = 0; i < length; ++i) {
    movingImageData[i] = movingImageDataSrc[i];
    movingMin = std::min(movingMin, movingImageData[i]);
    movingMax = std::max(movingMax, movingImageData[i]);
  }

  if (fixedMin == fixedMax || movingMin == movingMax) {
    LOG(INFO) << "At least one image is empty, skip aligning.";
    // we already copied the img, so do nothing
    return;
  }

  double thre1 = m_channelInfos[fixedChannel].median + 0 * m_channelInfos[fixedChannel].std;
  double thre2 = m_channelInfos[movingChannel].median + 0 * m_channelInfos[movingChannel].std;

  if (m_removeBackground) {
    if (m_brightBackground) {
      double subtval = std::max(fixedMax, movingMax);
      for (size_t i = 0; i < length; ++i) {
        fixedImageData[i] = fixedImageData[i] > thre1 ? subtval : fixedImageData[i];
      }
      for (size_t i = 0; i < length; ++i) {
        movingImageData[i] = movingImageData[i] > thre2 ? subtval : movingImageData[i];
      }
      fixedMax = subtval;
      movingMax = subtval;
    } else {
      double subtval = std::min(fixedMin, movingMin);
      for (size_t i = 0; i < length; ++i) {
        fixedImageData[i] = fixedImageData[i] < thre1 ? subtval : fixedImageData[i];
      }
      for (size_t i = 0; i < length; ++i) {
        movingImageData[i] = movingImageData[i] < thre2 ? subtval : movingImageData[i];
      }
      fixedMin = subtval;
      movingMin = subtval;
    }
  }

  if (m_removeHighForeground) {
    if (m_brightBackground) {
      double thre1up = thre1 - 3 * m_channelInfos[fixedChannel].std;
      double thre2up = thre2 - 3 * m_channelInfos[movingChannel].std;
      for (size_t i = 0; i < length; ++i) {
        fixedImageData[i] = fixedImageData[i] < thre1up ? thre1up : fixedImageData[i];
      }
      fixedMin = std::max(thre1up, fixedMin);
      for (size_t i = 0; i < length; ++i) {
        movingImageData[i] = movingImageData[i] < thre2up ? thre2up : movingImageData[i];
      }
      movingMin = std::max(thre2up, movingMin);
    } else {
      double thre1up = thre1 + 3 * m_channelInfos[fixedChannel].std;
      double thre2up = thre2 + 3 * m_channelInfos[movingChannel].std;
      for (size_t i = 0; i < length; ++i) {
        fixedImageData[i] = fixedImageData[i] > thre1up ? thre1up : fixedImageData[i];
      }
      fixedMax = std::min(thre1up, fixedMax);
      for (size_t i = 0; i < length; ++i) {
        movingImageData[i] = movingImageData[i] > thre2up ? thre2up : movingImageData[i];
      }
      movingMax = std::min(thre2up, movingMax);
    }
  }

  if (fixedMin == fixedMax || movingMin == movingMax) {
    LOG(INFO) << "At least one image is empty, skip aligning.";
    // we already copied the img, so do nothing
    return;
  }

  double Imin = std::min(fixedMin, movingMin);
  double Imax = std::max(fixedMax, movingMax);
  double scale = 1.0 / (Imax - Imin);

  for (size_t i = 0; i < length; ++i) {
    fixedImageData[i] = (fixedImageData[i] - Imin) * scale;
  }
  for (size_t i = 0; i < length; ++i) {
    movingImageData[i] = (movingImageData[i] - Imin) * scale;
  }

  std::vector<double> filteredFixedImageData(length);
  std::vector<double> filteredMovingImageData(length);

  for (size_t i = 0; i < m_img.depth(); ++i) {
    image2DGaussianFilter(fixedImageData.data() + i * m_img.planeByteNumber(), m_img.width(), m_img.height(),
                          2.5, 2.5, filteredFixedImageData.data() + i * m_img.planeByteNumber(), 11, 11, PadOption::Constant, 0.0,
                          m_useMultithreading);
    image2DGaussianFilter(movingImageData.data() + i * m_img.planeByteNumber(), m_img.width(), m_img.height(),
                          2.5, 2.5, filteredMovingImageData.data() + i * m_img.planeByteNumber(), 11, 11, PadOption::Constant, 0.0,
                          m_useMultithreading);
  }

  //  image2DWrite(fixedImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/fim.tif");
  //  image2DWrite(movingImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/mim.tif");
  //  image2DWrite(filteredFixedImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/ffim.tif");
  //  image2DWrite(filteredMovingImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/fmim.tif");

  ZImageToImageMetric metric;
  if (m_metric == "Normalized Cross-Correlation") {
    metric.setType(ZImageToImageMetric::Type::NormalizedCrossCorrelation);
  } else if (m_metric == "Normalized Mutual Information") {
    metric.setType(ZImageToImageMetric::Type::NormalizedMutualInformation);
  } else {
    LOG(FATAL) << "impossible transform type selection";
  }

  std::unique_ptr<ZImageTransform> transform;
  if (m_transform == "Translation") {
    if (m_img.depth() > 1) {
      auto tfm = new ZImageTranslation3DTransform();
      transform.reset(tfm);
    } else {
      auto tfm = new ZImageTranslation2DTransform();
      transform.reset(tfm);
    }
  } else if (m_transform == "Rigid") {
    if (m_img.depth() > 1) {
      auto tfm = new ZImageRigid3DTransform();
      tfm->setRotationCenter(m_img.width() / 2.0, m_img.height() / 2.0, m_img.depth() / 2.0);
      transform.reset(tfm);
    } else {
      auto tfm = new ZImageRigid2DTransform();
      tfm->setRotationCenter(m_img.width() / 2.0, m_img.height() / 2.0);
      transform.reset(tfm);
    }
  } else {
    LOG(FATAL) << "impossible transform type selection";
  }
  transform->setImageInterpolation(ZImageInterpolation(Interpolant::Linear, PadOption::Replicate));

  ZRegistrationNumericDiffCostFunction costFunction(1e-6);
  costFunction.setMetric(metric);

  ZImgRegistration registration;
  registration.setUseMultithreading(m_useMultithreading);
  registration.setCostFunction(costFunction);
  registration.setOptimizer(m_optimizer);
  registration.setNumScales(m_numScales);
  registration.setInitialTransform(*transform.get());

  ZImg fixedImg;
  fixedImg.wrapData(filteredFixedImageData.data(), m_img.width(), m_img.height(), m_img.depth());
  ZImg movingImg;
  movingImg.wrapData(filteredMovingImageData.data(), m_img.width(), m_img.height(), m_img.depth());
  registration.setFixedImg(fixedImg);
  registration.setMovingImg(movingImg);
  registration.run();

  // get output image from registered parameters
  transform->setImageInterpolation(ZImageInterpolation(Interpolant::Cubic, PadOption::Constant,
                                                       m_brightBackground ? m_channelInfos[movingChannel].max
                                                                          : m_channelInfos[movingChannel].min));

  ZImg correctedImg = m_img;
  if (m_img.depth() > 1) {
    transform->transformImage(m_img.channelData<ImagePixelType>(movingChannel),
                              m_img.width(), m_img.height(), m_img.depth(),
                              correctedImg.channelData<ImagePixelType>(movingChannel));
  } else {
    transform->transformImage(m_img.channelData<ImagePixelType>(movingChannel),
                              m_img.width(), m_img.height(),
                              correctedImg.channelData<ImagePixelType>(movingChannel));
  }

  correctedImg.save(m_resultFilename);
}

template<typename ImagePixelType>
void ZChromaticShiftCorrection::calcChannelInfs()
{
  m_channelInfos.resize(m_img.numChannels());
  m_minValue = std::numeric_limits<double>::max();
  m_maxValue = std::numeric_limits<double>::lowest();
  size_t length = m_img.channelVoxelNumber();
  for (size_t i = 0; i < m_img.numChannels(); ++i) {
    const ImagePixelType* data = m_img.channelData<ImagePixelType>(i);
    std::pair<const ImagePixelType*, const ImagePixelType*> minmax =
      minMaxElement(data, data + length);
    m_channelInfos[i].min = *minmax.first;
    m_channelInfos[i].max = *minmax.second;
    m_minValue = std::min(m_minValue, m_channelInfos[i].min);
    m_maxValue = std::max(m_maxValue, m_channelInfos[i].max);
    std::vector<ImagePixelType> dataWithoutZero;
    for (size_t j = 0; j < length; ++j) {
      if (data[j] > 0)
        dataWithoutZero.push_back(data[j]);
    }
    if (dataWithoutZero.empty()) {
      m_channelInfos[i].mean = 0;
      m_channelInfos[i].std = 0;
      m_channelInfos[i].median = 0;
    } else {
      meanAndStandardDeviation(dataWithoutZero.begin(), dataWithoutZero.end(), m_channelInfos[i].mean,
                               m_channelInfos[i].std);
      m_channelInfos[i].median = medianInPlace(dataWithoutZero.begin(), dataWithoutZero.end());
    }
  }
}

} // namespace nim


