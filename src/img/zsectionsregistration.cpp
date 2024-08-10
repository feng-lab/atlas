#include "zsectionsregistration.h"

#include "zcachedimg.h"
#include "zstatisticsutils.h"
#include "zregistrationnumericdiffcostfunction.h"
#include "zimage2dutils.h"
#include "zimgregistration.h"
#include "zimagematrix2dtransform.h"
#include "zimagetransformresolve.h"
#include <algorithm>
#include <memory>
#include <numeric>
#include <utility>

namespace nim {

void ZSectionsRegistration::doWork()
{
  LOG(INFO) << "";
  LOG(INFO) << "Start Sections Registration";
  logLongString(toString());
  LOG(INFO) << "";

  LOG(INFO) << "Image Filenames: " << m_imgFilenames;
  LOG(INFO) << "Result Filename: " << m_resultFilename;

  ZImgSource
    imgSource(m_imgFilenames, Dimension::Z, true, ZImgRegion(), 0, FileFormat::Unknown, true, m_brightBackground);
  LOG(INFO) << "Image Info: " << imgSource.toString();
  ZCachedImg srcImg(imgSource);
  if (srcImg.depth() <= 1) {
    throw ZException("Only one slice. Do not need alignment.");
  }
  if (srcImg.numTimes() > 1) {
    throw ZException(fmt::format("Can not align time sequence image: {}", srcImg.info().toString()));
  }

  if (m_fixedSliceIndex >= 0 && static_cast<size_t>(m_fixedSliceIndex) < srcImg.depth()) {
    LOG(INFO) << "Fixed Image Index: " << m_fixedSliceIndex + 1 << " (start from 1)";
  } else {
    throw ZException(fmt::format("Wrong fixed image index: {}. Abort", m_fixedSliceIndex));
  }

  if (m_referenceChannel == -1) {
    if (srcImg.numChannels() == 1) {
      m_referenceChannel = 0;
    } else {
      IMG_TYPED_CALL(calcRefCh, srcImg.info(), srcImg)
    }
  }

  if (m_referenceChannel >= 0 && static_cast<size_t>(m_referenceChannel) < srcImg.numChannels()) {
    LOG(INFO) << "Reference Channel: " << m_referenceChannel + 1 << " (start from 1)";
  } else {
    throw ZException(fmt::format("Wrong reference channel: {}. Abort", m_referenceChannel));
  }

  LOG(INFO) << "Remove Background: " << m_removeBackground;
  LOG(INFO) << "Remove High Foreground: " << m_removeHighForeground;
  LOG(INFO) << "Allow Flip: " << m_allowFlip;
  LOG(INFO) << "Multithreading: " << m_useMultithreading;
  LOG(INFO) << "Metirc: " << m_metric;
  LOG(INFO) << "Transform: " << m_transform;
  LOG(INFO) << "Optimizer: " << m_optimizer;

  IMG_TYPED_CALL(calcSecInfs, srcImg.info(), srcImg)

  double totalNumPairs = srcImg.depth() * m_numNeighbors;

  std::map<std::pair<size_t, size_t>, std::pair<std::unique_ptr<ZImageTransform>, double>> idxPairs;
  double progress = 0;
  for (size_t i = 0; i < srcImg.depth(); ++i) {
    for (size_t j = i + 1; j < i + 1 + m_numNeighbors; ++j) {
      if (j >= srcImg.depth()) {
        break;
      }
      double cost;
      ZImageTransform* tfm = nullptr;
      IMG_TYPED_CALL(alignSection, srcImg.info(), srcImg, i, j, cost, tfm)
      idxPairs[std::make_pair(i, j)] = std::make_pair(std::unique_ptr<ZImageTransform>(tfm), cost);
      progress += 1;
      reportProgress(progress / totalNumPairs);
    }
  }

  ZImageTransformResolve tfmResolve;
  auto notfm = std::make_unique<ZImageAffine2DTransform>();
  tfmResolve.addFixedImage(m_fixedSliceIndex, notfm.get());
  for (const auto& fixedMovingTfmCost : idxPairs) {
    tfmResolve.addImagePair(fixedMovingTfmCost.first.first,
                            fixedMovingTfmCost.first.second,
                            fixedMovingTfmCost.second.first.get(),
                            fixedMovingTfmCost.second.second);
  }
  std::map<size_t, std::unique_ptr<ZImageCompositeTransform>> tfmmap = tfmResolve.resolve();

  for (size_t i = 0; i < srcImg.depth(); ++i) {
    tfmmap.at(i)->setImageInterpolation(
      ZImageInterpolation(Interpolant::Cubic,
                          PadOption::Constant,
                          m_brightBackground ? m_sectionInfos[i].max : m_sectionInfos[i].min));
  }
  srcImg.setSliceTransform(&tfmmap);
  srcImg.save(m_resultFilename);

  reportProgress(1.0);
}

void ZSectionsRegistration::read(const json::object& jo)
{
  setInputOutput(json::value_to<QStringList>(jo.at("input_files")),
                 json::value_to<QString>(jo.at("result_file")),
                 json::value_to<int>(jo.at("fixed_slice_index")));

  setReferenceChannel(json::value_to<int>(jo.at("reference_channel")));
  setRemoveBackground(jo.at("remove_background").as_bool());
  setRemoveHighForeground(jo.at("remove_high_foreground").as_bool());
  setAllowFlip(jo.at("allow_flip").as_bool());
  setBrightBackground(jo.at("bright_background").as_bool());
  setMetric(json::value_to<QString>(jo.at("metric")));
  setTransform(json::value_to<QString>(jo.at("transform")));
  setOptimizer(json::value_to<QString>(jo.at("optimizer")));
  setUseMultithreading(jo.at("use_multithreading").as_bool());
  setNumScales(json::value_to<int>(jo.at("num_scales")));
  setNumNeighbors(json::value_to<int>(jo.at("num_neighbors")));
}

void ZSectionsRegistration::write(json::object& jo) const
{
  jo["input_files"] = json::value_from(m_imgFilenames);
  jo["result_file"] = json::value_from(m_resultFilename);
  jo["fixed_slice_index"] = m_fixedSliceIndex;

  jo["reference_channel"] = m_referenceChannel;
  jo["remove_background"] = m_removeBackground;
  jo["remove_high_foreground"] = m_removeHighForeground;
  jo["allow_flip"] = m_allowFlip;
  jo["bright_background"] = m_brightBackground;
  jo["metric"] = json::value_from(m_metric);
  jo["transform"] = json::value_from(m_transform);
  jo["optimizer"] = json::value_from(m_optimizer);
  jo["use_multithreading"] = m_useMultithreading;
  jo["num_scales"] = m_numScales;
  jo["num_neighbors"] = m_numNeighbors;
}

template<typename ImagePixelType>
void ZSectionsRegistration::alignSection(const ZCachedImg& srcImg,
                                         size_t fixedImageIndex,
                                         size_t movingImageIndex,
                                         double& cost,
                                         ZImageTransform*& transform)
{
  LOG(INFO) << "";
  LOG(INFO) << "Registering Image " << (movingImageIndex) << " to Image " << (fixedImageIndex);

  size_t length = srcImg.info().planeVoxelNumber();
  std::vector<double> fixedImageData(length);
  std::vector<double> movingImageData(length);

  // const auto* fixedImageDataSrc = srcImg.planeData<ImagePixelType>(fixedImageIndex, m_referenceChannel);
  // const auto* movingImageDataSrc = srcImg.planeData<ImagePixelType>(movingImageIndex, m_referenceChannel);
  auto fixedImage = srcImg.slice(fixedImageIndex, m_referenceChannel, 0);
  auto movingImage = srcImg.slice(movingImageIndex, m_referenceChannel, 0);
  const auto* fixedImageDataSrc = fixedImage.planeData<ImagePixelType>(0);
  const auto* movingImageDataSrc = movingImage.planeData<ImagePixelType>(0);

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
    LOG(INFO) << "At least one image is empty, skip registratering.";
    cost = 1e10; // a large cost
    transform = new ZImageRigid2DTransform(); // no transform
    return;
  }

  double thre1 = m_sectionInfos[fixedImageIndex].median + 0 * m_sectionInfos[fixedImageIndex].std;
  double thre2 = m_sectionInfos[movingImageIndex].median + 0 * m_sectionInfos[movingImageIndex].std;

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
      double thre1up = thre1 - 3 * m_sectionInfos[fixedImageIndex].std;
      double thre2up = thre2 - 3 * m_sectionInfos[movingImageIndex].std;
      for (size_t i = 0; i < length; ++i) {
        fixedImageData[i] = fixedImageData[i] < thre1up ? thre1up : fixedImageData[i];
      }
      fixedMin = std::max(thre1up, fixedMin);
      for (size_t i = 0; i < length; ++i) {
        movingImageData[i] = movingImageData[i] < thre2up ? thre2up : movingImageData[i];
      }
      movingMin = std::max(thre2up, movingMin);
    } else {
      double thre1up = thre1 + 3 * m_sectionInfos[fixedImageIndex].std;
      double thre2up = thre2 + 3 * m_sectionInfos[movingImageIndex].std;
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
    LOG(INFO) << "At least one image is empty, skip registratering.";
    cost = 1e10; // a large cost
    transform = new ZImageRigid2DTransform(); // no transform
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

  image2DGaussianFilter(fixedImageData.data(),
                        srcImg.width(),
                        srcImg.height(),
                        2.5,
                        2.5,
                        filteredFixedImageData.data(),
                        11,
                        11,
                        PadOption::Constant,
                        0.0,
                        m_useMultithreading);
  image2DGaussianFilter(movingImageData.data(),
                        srcImg.width(),
                        srcImg.height(),
                        2.5,
                        2.5,
                        filteredMovingImageData.data(),
                        11,
                        11,
                        PadOption::Constant,
                        0.0,
                        m_useMultithreading);

  //  image2DWrite(fixedImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/fim.tif");
  //  image2DWrite(movingImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/mim.tif");
  //  image2DWrite(filteredFixedImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/ffim.tif");
  //  image2DWrite(filteredMovingImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/fmim.tif");

  ZImageToImageMetric metric;
  if (m_metric == "Mean Differences") {
    metric.setType(ZImageToImageMetric::Type::MeanDifferences);
  } else if (m_metric == "Mean Squared Differences") {
    metric.setType(ZImageToImageMetric::Type::MeanSquaredDifferences);
  } else if (m_metric == "Log Absolute Differences") {
    metric.setType(ZImageToImageMetric::Type::LogAbsoluteDifferences);
  } else if (m_metric == "Normalized Cross-Correlation") {
    metric.setType(ZImageToImageMetric::Type::NormalizedCrossCorrelation);
  } else if (m_metric == "Normalized Mutual Information") {
    metric.setType(ZImageToImageMetric::Type::NormalizedMutualInformation);
  } else {
    LOG(FATAL) << "impossible transform type selection";
  }

  if (m_transform == "YTranslation") {
    auto tfm = new ZImageYTranslation2DTransform();
    transform = tfm;
  } else if (m_transform == "Translation") {
    auto tfm = new ZImageTranslation2DTransform();
    transform = tfm;
  } else if (m_transform == "Rigid") {
    auto tfm = new ZImageRigid2DTransform();
    tfm->setRotationCenter(srcImg.width() / 2.0, srcImg.height() / 2.0);
    transform = tfm;
  } else if (m_transform == "Similarity") {
    auto tfm = new ZImageSimilarity2DTransform();
    tfm->setRotationCenter(srcImg.width() / 2.0, srcImg.height() / 2.0);
    transform = tfm;
  } else if (m_transform == "Affine") {
    auto tfm = new ZImageAffine2DTransform();
    tfm->setRotationCenter(srcImg.width() / 2.0, srcImg.height() / 2.0);
    transform = tfm;
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
  registration.setInitialTransform(*transform);

  ZImg fixedImg;
  fixedImg.wrapData(filteredFixedImageData.data(), srcImg.width(), srcImg.height());
  ZImg movingImg;
  movingImg.wrapData(filteredMovingImageData.data(), srcImg.width(), srcImg.height());
  registration.setFixedImg(fixedImg);
  registration.setMovingImg(movingImg);
  cost = registration.run();
}

template<typename ImagePixelType>
void ZSectionsRegistration::calcRefCh(const ZCachedImg& srcImg)
{
  size_t length = srcImg.info().planeVoxelNumber();
  // const auto* data = srcImg.planeData<ImagePixelType>(m_fixedSliceIndex, 0);
  auto fixedImage = srcImg.slice(m_fixedSliceIndex, 0);
  const auto* data = fixedImage.planeData<ImagePixelType>(0, 0);
  double maxsum = std::accumulate(data, data + length, 0.0);
  m_referenceChannel = 0;
  for (size_t i = 1; i < srcImg.numChannels(); ++i) {
    // data = srcImg.planeData<ImagePixelType>(m_fixedSliceIndex, i);
    data = fixedImage.planeData<ImagePixelType>(0, i);
    double sum = std::accumulate(data, data + length, 0.0);
    if ((!m_brightBackground && sum > maxsum) || (m_brightBackground && sum < maxsum)) {
      maxsum = sum;
      m_referenceChannel = i;
    }
  }
}

template<typename ImagePixelType>
void ZSectionsRegistration::calcSecInfs(const ZCachedImg& srcImg)
{
  m_sectionInfos.resize(srcImg.depth());
  m_minValue = std::numeric_limits<double>::max();
  m_maxValue = std::numeric_limits<double>::lowest();
  size_t length = srcImg.info().planeVoxelNumber();

  tbb::parallel_for(tbb::blocked_range<size_t>(0, srcImg.depth()), [&](const tbb::blocked_range<size_t>& range) {
    for (size_t i = range.begin(); i < range.end(); ++i) {
      auto image = srcImg.slice(i, m_referenceChannel, 0);
      const auto* data = image.planeData<ImagePixelType>(0);
      // std::pair<const ImagePixelType*, const ImagePixelType*> minmax = minMaxElement(data, data + length);
      // m_sectionInfos[i].min = *minmax.first;
      // m_sectionInfos[i].max = *minmax.second;
      std::tie(m_sectionInfos[i].min, m_sectionInfos[i].max) = parallel_minmax(data, data + length);
      m_minValue = std::min(m_minValue, m_sectionInfos[i].min);
      m_maxValue = std::max(m_maxValue, m_sectionInfos[i].max);
      std::vector<ImagePixelType> dataWithoutZero;
      for (size_t j = 0; j < length; ++j) {
        if (data[j] > 0) {
          dataWithoutZero.push_back(data[j]);
        }
      }
      if (dataWithoutZero.empty()) {
        m_sectionInfos[i].mean = 0;
        m_sectionInfos[i].std = 0;
        m_sectionInfos[i].median = 0;
      } else {
        //      meanAndStandardDeviation(dataWithoutZero.begin(),
        //                               dataWithoutZero.end(),
        //                               m_sectionInfos[i].mean,
        //                               m_sectionInfos[i].std);
        std::tie(m_sectionInfos[i].mean, m_sectionInfos[i].std) =
          parallel_mean_and_sample_standard_deviation(dataWithoutZero.begin(), dataWithoutZero.end());
        m_sectionInfos[i].median = median(dataWithoutZero.begin(), dataWithoutZero.end());
      }
    }
  });
  //  for (size_t i = 0; i < srcImg.depth(); ++i) {
  //    auto image = srcImg.slice(i, m_referenceChannel, 0);
  //    const auto* data = image.planeData<ImagePixelType>(0);
  //    // std::pair<const ImagePixelType*, const ImagePixelType*> minmax = minMaxElement(data, data + length);
  //    // m_sectionInfos[i].min = *minmax.first;
  //    // m_sectionInfos[i].max = *minmax.second;
  //    std::tie(m_sectionInfos[i].min, m_sectionInfos[i].max) = parallel_minmax(data, data + length);
  //    m_minValue = std::min(m_minValue, m_sectionInfos[i].min);
  //    m_maxValue = std::max(m_maxValue, m_sectionInfos[i].max);
  //    std::vector<ImagePixelType> dataWithoutZero;
  //    for (size_t j = 0; j < length; ++j) {
  //      if (data[j] > 0) {
  //        dataWithoutZero.push_back(data[j]);
  //      }
  //    }
  //    if (dataWithoutZero.empty()) {
  //      m_sectionInfos[i].mean = 0;
  //      m_sectionInfos[i].std = 0;
  //      m_sectionInfos[i].median = 0;
  //    } else {
  //      //      meanAndStandardDeviation(dataWithoutZero.begin(),
  //      //                               dataWithoutZero.end(),
  //      //                               m_sectionInfos[i].mean,
  //      //                               m_sectionInfos[i].std);
  //      std::tie(m_sectionInfos[i].mean, m_sectionInfos[i].std) =
  //        parallel_mean_and_sample_standard_deviation(dataWithoutZero.begin(), dataWithoutZero.end());
  //      m_sectionInfos[i].median = median(dataWithoutZero.begin(), dataWithoutZero.end());
  //    }
  //  }
}

} // namespace nim
