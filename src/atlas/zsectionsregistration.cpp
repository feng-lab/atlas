#include "zsectionsregistration.h"

#include "zimg.h"
#include <numeric>
#include <algorithm>
#include "zstatisticsutils.h"

#include "zregistrationnumericdiffcostfunction.h"
#include "zimage2dutils.h"
#include "zimgregistration.h"
#include "zimagematrix2dtransform.h"
#include <memory>
#include "zimagetransformresolve.h"
#include <utility>

namespace nim {

ZSectionsRegistration::ZSectionsRegistration(const ZImg &img, int fixedSliceIndex, ZImg &registeredImg)
  : ZImgProcess()
  , m_img(img)
  , m_fixedSliceIndex(fixedSliceIndex)
  , m_registeredImg(registeredImg)
  , m_referenceChannel(-1)
  , m_removeBackground(true)
  , m_removeHighForeground(true)
  , m_allowFlip(false)
  , m_brightBackground(false)
  , m_useMultithreading(true)
  , m_numScales(1)
  , m_numNeighbors(1)
  , m_metric("Log Absolute Differences")
  , m_transform("Rigid")
  , m_optimizer("LBFGS")
{
}

void ZSectionsRegistration::doWork()
{
  LINFO() << "";
  // todo : add back
  //  if (!m_stack.sourcePath()) {
  //    LINFO() << "Start Registering Sections";
  //  } else {
  //    LINFO() << "Start Registering Sections for Image" << m_stack.sourcePath();
  //  }
  //  LINFO() << "";

  if (m_fixedSliceIndex >= 0 && static_cast<size_t>(m_fixedSliceIndex) < m_img.depth()) {
    LINFO() << "Fixed Image Index:" << m_fixedSliceIndex+1 << "(start from 1)";
  } else {
    throw ZImgException(QString("Wrong fixed image index: %1. Abort.").arg(m_fixedSliceIndex));
  }

  if (m_referenceChannel == -1) {
    if (m_img.numChannels() == 1) {
      m_referenceChannel = 0;
    } else {
      IMG_TYPED_CALL(calcRefCh, m_img);
    }
  }

  if (m_referenceChannel >= 0 && static_cast<size_t>(m_referenceChannel) < m_img.numChannels()) {
    LINFO() << "Reference Channel:" << m_referenceChannel+1 << "(start from 1)";
  } else {
    throw ZImgException(QString("Wrong reference channel: %1. Abort").arg(m_referenceChannel));
  }

  LINFO() << "Remove Background:" << m_removeBackground;
  LINFO() << "Remove High Foreground:" << m_removeHighForeground;
  LINFO() << "Allow Flip:" << m_allowFlip;
  LINFO() << "Multithreading:" << m_useMultithreading;
  LINFO() << "Metirc:" << m_metric;
  LINFO() << "Transform:" << m_transform;
  LINFO() << "Optimizer:" << m_optimizer;

  IMG_TYPED_CALL(calcSecInfs, m_img);

#if 0
  m_registeredImg = m_img;
  double progress = 1.0;
  reportProgress(progress/m_img.depth());

  for (size_t i=m_fixedSliceIndex+1; i<m_img.depth(); ++i) {
    IMG_TYPED_CALL(alignSection, m_img, i-1, i);
    progress += 1.0;
    reportProgress(progress/m_img.depth());
  }

  for (int i=m_fixedSliceIndex-1; i>=0; --i) {
    IMG_TYPED_CALL(alignSection, m_img, i+1, i);
    progress += 1.0;
    reportProgress(progress/m_img.depth());
  }
#else
  double totalNumPairs = m_img.depth() * m_numNeighbors;

  std::map<std::pair<size_t, size_t>, std::pair<std::unique_ptr<ZImageTransform>, double>> idxPairs;
  double progress = 0;
  for (size_t i=0; i<m_img.depth(); ++i) {
    for (size_t j=i+1; j<i+1+m_numNeighbors; ++j) {
      if (j >= m_img.depth())
        break;
      double cost;
      ZImageTransform *tfm = nullptr;
      IMG_TYPED_CALL(alignSection, m_img, i, j, cost, tfm);
      idxPairs[std::make_pair(i,j)] = std::make_pair(std::unique_ptr<ZImageTransform>(tfm), cost);
      progress += 1;
      reportProgress(progress / totalNumPairs);
    }
  }

  ZImageTransformResolve tfmResolve;
  auto notfm = std::make_unique<ZImageAffine2DTransform>();
  tfmResolve.addFixedImage(m_fixedSliceIndex, notfm.get());
  for (auto it = idxPairs.cbegin(); it != idxPairs.cend(); ++it) {
    tfmResolve.addImagePair(it->first.first, it->first.second, it->second.first.get(), it->second.second);
  }
  std::map<size_t, std::unique_ptr<ZImageCompositeTransform>> tfmmap = tfmResolve.resolve();

  IMG_TYPED_CALL(transformSections, m_img, tfmmap, m_img, m_registeredImg);

  reportProgress(1.0);
#endif
}

template <typename ImagePixelType>
void ZSectionsRegistration::alignSection(int fixedImageIndex, int movingImageIndex, double &cost, ZImageTransform*& transform)
{
  LINFO() << "";
  LINFO() << "Registering Image" << (movingImageIndex) << "to Image " << (fixedImageIndex);

  size_t length = m_img.planeVoxelNumber();
  std::vector<double> fixedImageData(length);
  std::vector<double> movingImageData(length);

  const ImagePixelType* fixedImageDataSrc = m_img.planeData<ImagePixelType>(fixedImageIndex, m_referenceChannel);
  const ImagePixelType* movingImageDataSrc = m_img.planeData<ImagePixelType>(movingImageIndex, m_referenceChannel);
  double fixedMin = std::numeric_limits<double>::max();
  double fixedMax = -std::numeric_limits<double>::max();
  double movingMin = fixedMin;
  double movingMax = fixedMax;
  for (size_t i=0; i<length; ++i) {
    fixedImageData[i] = fixedImageDataSrc[i];
    fixedMin = std::min(fixedMin, fixedImageData[i]);
    fixedMax = std::max(fixedMax, fixedImageData[i]);
  }
  for (size_t i=0; i<length; ++i) {
    movingImageData[i] = movingImageDataSrc[i];
    movingMin = std::min(movingMin, movingImageData[i]);
    movingMax = std::max(movingMax, movingImageData[i]);
  }

  if (fixedMin == fixedMax || movingMin == movingMax) {
    LINFO() << "At least one image is empty, skip registratering.";
    cost = 1e10;  // a large cost
    transform = new ZImageRigid2DTransform();  // no transform
    return;
  }

  double thre1 = m_sectionInfos[fixedImageIndex].median + 0 * m_sectionInfos[fixedImageIndex].std;
  double thre2 = m_sectionInfos[movingImageIndex].median + 0 * m_sectionInfos[movingImageIndex].std;

  if (m_removeBackground) {
    if (m_brightBackground) {
      double subtval = std::max(fixedMax, movingMax);
      for (size_t i=0; i<length; ++i) {
        fixedImageData[i] = fixedImageData[i] > thre1 ? subtval : fixedImageData[i];
      }
      for (size_t i=0; i<length; ++i) {
        movingImageData[i] = movingImageData[i] > thre2 ? subtval : movingImageData[i];
      }
      fixedMax = subtval;
      movingMax = subtval;
    } else {
      double subtval = std::min(fixedMin, movingMin);
      for (size_t i=0; i<length; ++i) {
        fixedImageData[i] = fixedImageData[i] < thre1 ? subtval : fixedImageData[i];
      }
      for (size_t i=0; i<length; ++i) {
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
      for (size_t i=0; i<length; ++i) {
        fixedImageData[i] = fixedImageData[i] < thre1up ? thre1up : fixedImageData[i];
      }
      fixedMin = std::max(thre1up, fixedMin);
      for (size_t i=0; i<length; ++i) {
        movingImageData[i] = movingImageData[i] < thre2up ? thre2up : movingImageData[i];
      }
      movingMin = std::max(thre2up, movingMin);
    } else {
      double thre1up = thre1 + 3 * m_sectionInfos[fixedImageIndex].std;
      double thre2up = thre2 + 3 * m_sectionInfos[movingImageIndex].std;
      for (size_t i=0; i<length; ++i) {
        fixedImageData[i] = fixedImageData[i] > thre1up ? thre1up : fixedImageData[i];
      }
      fixedMax = std::min(thre1up, fixedMax);
      for (size_t i=0; i<length; ++i) {
        movingImageData[i] = movingImageData[i] > thre2up ? thre2up : movingImageData[i];
      }
      movingMax = std::min(thre2up, movingMax);
    }
  }

  if (fixedMin == fixedMax || movingMin == movingMax) {
    LINFO() << "At least one image is empty, skip registratering.";
    cost = 1e10;  // a large cost
    transform = new ZImageRigid2DTransform();  // no transform
    return;
  }

  double Imin = std::min(fixedMin, movingMin);
  double Imax = std::max(fixedMax, movingMax);
  double scale = 1.0 / (Imax - Imin);

  for (size_t i=0; i<length; ++i) {
    fixedImageData[i] = (fixedImageData[i]-Imin) * scale;
  }
  for (size_t i=0; i<length; ++i) {
    movingImageData[i] = (movingImageData[i]-Imin) * scale;
  }

  std::vector<double> filteredFixedImageData(length);
  std::vector<double> filteredMovingImageData(length);

  image2DGaussianFilter(fixedImageData.data(), m_img.width(), m_img.height(),
      2.5, 2.5, filteredFixedImageData.data(), 11, 11, PadOption::Constant, 0.0, m_useMultithreading);
  image2DGaussianFilter(movingImageData.data(), m_img.width(), m_img.height(),
      2.5, 2.5, filteredMovingImageData.data(), 11, 11, PadOption::Constant, 0.0, m_useMultithreading);

  //  image2DWrite(fixedImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/fim.tif");
  //  image2DWrite(movingImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/mim.tif");
  //  image2DWrite(filteredFixedImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/ffim.tif");
  //  image2DWrite(filteredMovingImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/fmim.tif");

  ZImageToImageMetric metric;
  if (m_metric == "Mean Differences")
    metric.setType(ZImageToImageMetric::Type::MeanDifferences);
  else if (m_metric == "Mean Squared Differences")
    metric.setType(ZImageToImageMetric::Type::MeanSquaredDifferences);
  else if (m_metric == "Log Absolute Differences")
    metric.setType(ZImageToImageMetric::Type::LogAbsoluteDifferences);
  else if (m_metric == "Normalized Cross-Correlation")
    metric.setType(ZImageToImageMetric::Type::NormalizedCrossCorrelation);
  else if (m_metric == "Normalized Mutual Information")
    metric.setType(ZImageToImageMetric::Type::NormalizedMutualInformation);
  else
    LFATAL() << "impossible transform type selection";

  if (m_transform == "YTranslation") {
    ZImageYTranslation2DTransform *tfm = new ZImageYTranslation2DTransform();
    transform = tfm;
  } else if (m_transform == "Translation") {
    ZImageTranslation2DTransform *tfm = new ZImageTranslation2DTransform();
    transform = tfm;
  } else if (m_transform == "Rigid") {
    ZImageRigid2DTransform *tfm = new ZImageRigid2DTransform();
    tfm->setRotationCenter(m_img.width() / 2.0, m_img.height() / 2.0);
    transform = tfm;
  } else if (m_transform == "Similarity") {
    ZImageSimilarity2DTransform *tfm = new ZImageSimilarity2DTransform();
    tfm->setRotationCenter(m_img.width() / 2.0, m_img.height() / 2.0);
    transform = tfm;
  } else if (m_transform == "Affine") {
    ZImageAffine2DTransform *tfm = new ZImageAffine2DTransform();
    tfm->setRotationCenter(m_img.width() / 2.0, m_img.height() / 2.0);
    transform = tfm;
  } else {
    LFATAL() << "impossible transform type selection";
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
  fixedImg.wrapData(filteredFixedImageData.data(), m_img.width(), m_img.height());
  ZImg movingImg;
  movingImg.wrapData(filteredMovingImageData.data(), m_img.width(), m_img.height());
  registration.setFixedImg(fixedImg);
  registration.setMovingImg(movingImg);
  cost = registration.run();
}

template <typename ImagePixelType>
void ZSectionsRegistration::transformSections(const std::map<size_t, std::unique_ptr<ZImageCompositeTransform> > &tfmmap, const ZImg &inImg, ZImg &outImg) const
{
  outImg = inImg;
  for (size_t i=0; i<inImg.depth(); ++i) {
    auto& tfm = tfmmap.at(i);
    tfm->setImageInterpolation(ZImageInterpolation(Interpolant::Cubic, PadOption::Constant,
                                                   m_brightBackground ? m_sectionInfos[i].max : m_sectionInfos[i].min));
    for (size_t c=0; c<inImg.numChannels(); ++c) {
      tfm->transformImage(inImg.planeData<ImagePixelType>(i, c),
                          inImg.width(), inImg.height(),
                          outImg.planeData<ImagePixelType>(i, c));
    }
  }
}

template <typename ImagePixelType>
void ZSectionsRegistration::alignSection(int fixedImageIndex, int movingImageIndex)
{
  LINFO() << "";
  LINFO() << "Registering Image" << (movingImageIndex) << "to Image " << (fixedImageIndex);

  size_t length = m_img.planeVoxelNumber();
  std::vector<double> fixedImageData(length);
  std::vector<double> movingImageData(length);

  const ImagePixelType* fixedImageDataSrc = m_registeredImg.planeData<ImagePixelType>(fixedImageIndex, m_referenceChannel);
  const ImagePixelType* movingImageDataSrc = m_img.planeData<ImagePixelType>(movingImageIndex, m_referenceChannel);
  double fixedMin = std::numeric_limits<double>::max();
  double fixedMax = -std::numeric_limits<double>::max();
  double movingMin = fixedMin;
  double movingMax = fixedMax;
  for (size_t i=0; i<length; ++i) {
    fixedImageData[i] = fixedImageDataSrc[i];
    fixedMin = std::min(fixedMin, fixedImageData[i]);
    fixedMax = std::max(fixedMax, fixedImageData[i]);
  }
  for (size_t i=0; i<length; ++i) {
    movingImageData[i] = movingImageDataSrc[i];
    movingMin = std::min(movingMin, movingImageData[i]);
    movingMax = std::max(movingMax, movingImageData[i]);
  }

  if (fixedMin == fixedMax || movingMin == movingMax) {
    LINFO() << "At least one image is empty, skip registratering.";
    // we already copied the img, so do nothing
    return;
  }

  double thre1 = m_sectionInfos[fixedImageIndex].median + 0 * m_sectionInfos[fixedImageIndex].std;
  double thre2 = m_sectionInfos[movingImageIndex].median + 0 * m_sectionInfos[movingImageIndex].std;

  if (m_removeBackground) {
    if (m_brightBackground) {
      double subtval = std::max(fixedMax, movingMax);
      for (size_t i=0; i<length; ++i) {
        fixedImageData[i] = fixedImageData[i] > thre1 ? subtval : fixedImageData[i];
      }
      for (size_t i=0; i<length; ++i) {
        movingImageData[i] = movingImageData[i] > thre2 ? subtval : movingImageData[i];
      }
      fixedMax = subtval;
      movingMax = subtval;
    } else {
      double subtval = std::min(fixedMin, movingMin);
      for (size_t i=0; i<length; ++i) {
        fixedImageData[i] = fixedImageData[i] < thre1 ? subtval : fixedImageData[i];
      }
      for (size_t i=0; i<length; ++i) {
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
      for (size_t i=0; i<length; ++i) {
        fixedImageData[i] = fixedImageData[i] < thre1up ? thre1up : fixedImageData[i];
      }
      fixedMin = std::max(thre1up, fixedMin);
      for (size_t i=0; i<length; ++i) {
        movingImageData[i] = movingImageData[i] < thre2up ? thre2up : movingImageData[i];
      }
      movingMin = std::max(thre2up, movingMin);
    } else {
      double thre1up = thre1 + 3 * m_sectionInfos[fixedImageIndex].std;
      double thre2up = thre2 + 3 * m_sectionInfos[movingImageIndex].std;
      for (size_t i=0; i<length; ++i) {
        fixedImageData[i] = fixedImageData[i] > thre1up ? thre1up : fixedImageData[i];
      }
      fixedMax = std::min(thre1up, fixedMax);
      for (size_t i=0; i<length; ++i) {
        movingImageData[i] = movingImageData[i] > thre2up ? thre2up : movingImageData[i];
      }
      movingMax = std::min(thre2up, movingMax);
    }
  }

  if (fixedMin == fixedMax || movingMin == movingMax) {
    LINFO() << "At least one image is empty, skip registratering.";
    // we already copied the img, so do nothing
    return;
  }

  double Imin = std::min(fixedMin, movingMin);
  double Imax = std::max(fixedMax, movingMax);
  double scale = 1.0 / (Imax - Imin);

  for (size_t i=0; i<length; ++i) {
    fixedImageData[i] = (fixedImageData[i]-Imin) * scale;
  }
  for (size_t i=0; i<length; ++i) {
    movingImageData[i] = (movingImageData[i]-Imin) * scale;
  }

  std::vector<double> filteredFixedImageData(length);
  std::vector<double> filteredMovingImageData(length);

  image2DGaussianFilter(fixedImageData.data(), m_img.width(), m_img.height(),
      2.5, 2.5, filteredFixedImageData.data(), 11, 11, PadOption::Constant, 0.0, m_useMultithreading);
  image2DGaussianFilter(movingImageData.data(), m_img.width(), m_img.height(),
      2.5, 2.5, filteredMovingImageData.data(), 11, 11, PadOption::Constant, 0.0, m_useMultithreading);

  //  image2DWrite(fixedImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/fim.tif");
  //  image2DWrite(movingImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/mim.tif");
  //  image2DWrite(filteredFixedImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/ffim.tif");
  //  image2DWrite(filteredMovingImageData.data(), m_stack.width(), m_stack.height(), "/Users/feng/Downloads/fmim.tif");

  ZImageToImageMetric metric;
  if (m_metric == "Mean Differences")
    metric.setType(ZImageToImageMetric::Type::MeanDifferences);
  else if (m_metric == "Mean Squared Differences")
    metric.setType(ZImageToImageMetric::Type::MeanSquaredDifferences);
  else if (m_metric == "Log Absolute Differences")
    metric.setType(ZImageToImageMetric::Type::LogAbsoluteDifferences);
  else if (m_metric == "Normalized Cross-Correlation")
    metric.setType(ZImageToImageMetric::Type::NormalizedCrossCorrelation);
  else if (m_metric == "Normalized Mutual Information")
    metric.setType(ZImageToImageMetric::Type::NormalizedMutualInformation);
  else
    LFATAL() << "impossible transform type selection";

  std::unique_ptr<ZImageTransform> transform;
  if (m_transform == "YTranslation") {
    ZImageYTranslation2DTransform *tfm = new ZImageYTranslation2DTransform();
    transform.reset(tfm);
  } else if (m_transform == "Translation") {
    ZImageTranslation2DTransform *tfm = new ZImageTranslation2DTransform();
    transform.reset(tfm);
  } else if (m_transform == "Rigid") {
    ZImageRigid2DTransform *tfm = new ZImageRigid2DTransform();
    tfm->setRotationCenter(m_img.width() / 2.0, m_img.height() / 2.0);
    transform.reset(tfm);
  } else if (m_transform == "Similarity") {
    ZImageSimilarity2DTransform *tfm = new ZImageSimilarity2DTransform();
    tfm->setRotationCenter(m_img.width() / 2.0, m_img.height() / 2.0);
    transform.reset(tfm);
  } else if (m_transform == "Affine") {
    ZImageAffine2DTransform *tfm = new ZImageAffine2DTransform();
    tfm->setRotationCenter(m_img.width() / 2.0, m_img.height() / 2.0);
    transform.reset(tfm);
  } else {
    LFATAL() << "impossible transform type selection";
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

  bool flip = false;
  ZImg fixedImg;
  fixedImg.wrapData(filteredFixedImageData.data(), m_img.width(), m_img.height());
  ZImg movingImg;
  movingImg.wrapData(filteredMovingImageData.data(), m_img.width(), m_img.height());
  registration.setFixedImg(fixedImg);
  registration.setMovingImg(movingImg);
  double cost = registration.run();
  if (m_allowFlip) {
    image2DFlip(filteredMovingImageData.data(), m_img.width(), m_img.height(), Dimension::X);
    std::unique_ptr<ZImageTransform> flipTransform;
    if (m_transform == "YTranslation") {
      ZImageYTranslation2DTransform *tfm = new ZImageYTranslation2DTransform();
      flipTransform.reset(tfm);
    } else if (m_transform == "Translation") {
      ZImageTranslation2DTransform *tfm = new ZImageTranslation2DTransform();
      flipTransform.reset(tfm);
    } else if (m_transform == "Rigid") {
      ZImageRigid2DTransform *tfm = new ZImageRigid2DTransform();
      tfm->setRotationCenter(m_img.width() / 2.0, m_img.height() / 2.0);
      flipTransform.reset(tfm);
    } else if (m_transform == "Similarity") {
      ZImageSimilarity2DTransform *tfm = new ZImageSimilarity2DTransform();
      tfm->setRotationCenter(m_img.width() / 2.0, m_img.height() / 2.0);
      flipTransform.reset(tfm);
    } else if (m_transform == "Affine") {
      ZImageAffine2DTransform *tfm = new ZImageAffine2DTransform();
      tfm->setRotationCenter(m_img.width() / 2.0, m_img.height() / 2.0);
      flipTransform.reset(tfm);
    } else {
      LFATAL() << "impossible transform type selection";
    }
    flipTransform->setImageInterpolation(ZImageInterpolation(Interpolant::Linear, PadOption::Replicate));
    registration.setInitialTransform(*flipTransform.get());

    LINFO() << "";
    LINFO() << "Align fixed image with flipped moving image: ";
    movingImg.wrapData(filteredMovingImageData.data(), m_img.width(), m_img.height());
    registration.setMovingImg(movingImg);
    double flipCost = registration.run();
    if (flipCost < cost) {
      flip = true;
      LINFO() << "** Flip slice" << movingImageIndex;
      transform = std::move(flipTransform);
    }
  }

  // get output image from registered parameters
  transform->setImageInterpolation(ZImageInterpolation(Interpolant::Cubic, PadOption::Constant,
                                                       m_brightBackground ? m_sectionInfos[movingImageIndex].max : m_sectionInfos[movingImageIndex].min));
  if (flip) {
    std::vector<ImagePixelType> buffer(m_img.planeVoxelNumber());
    for (size_t i=0; i<m_registeredImg.numChannels(); ++i) {
      memcpy(buffer.data(), m_img.planeData<ImagePixelType>(movingImageIndex, i), m_img.planeByteNumber());
      image2DFlip(buffer.data(), m_img.width(), m_img.height(), Dimension::X);
      transform->transformImage(buffer.data(),
          m_img.width(), m_img.height(),
          m_registeredImg.planeData<ImagePixelType>(movingImageIndex, i));
    }
  } else {
    for (size_t i=0; i<m_registeredImg.numChannels(); ++i) {
      transform->transformImage(m_img.planeData<ImagePixelType>(movingImageIndex, i),
                                m_img.width(), m_img.height(),
                                m_registeredImg.planeData<ImagePixelType>(movingImageIndex, i));
    }
  }
}

template <typename ImagePixelType>
void ZSectionsRegistration::calcRefCh()
{
  size_t length = m_img.planeVoxelNumber();
  const ImagePixelType* data = m_img.planeData<ImagePixelType>(m_fixedSliceIndex, 0);
  double maxsum = std::accumulate(data, data+length, 0.0);
  m_referenceChannel = 0;
  for (size_t i=1; i<m_img.numChannels(); ++i) {
    data = m_img.planeData<ImagePixelType>(m_fixedSliceIndex, i);
    double sum = std::accumulate(data, data+length, 0.0);
    if ((!m_brightBackground && sum > maxsum) ||
        (m_brightBackground && sum < maxsum)) {
      maxsum = sum;
      m_referenceChannel = i;
    }
  }
}

template <typename ImagePixelType>
void ZSectionsRegistration::calcSecInfs()
{
  m_sectionInfos.resize(m_img.depth());
  m_minValue = std::numeric_limits<double>::max();
  m_maxValue = -std::numeric_limits<double>::max();
  size_t length = m_img.planeVoxelNumber();
  for (size_t i=0; i<m_img.depth(); ++i) {
    const ImagePixelType* data = m_img.planeData<ImagePixelType>(i, m_referenceChannel);
    std::pair<const ImagePixelType*,const ImagePixelType*> minmax =
        minMaxElement(data, data+length);
    m_sectionInfos[i].min = *minmax.first;
    m_sectionInfos[i].max = *minmax.second;
    m_minValue = std::min(m_minValue, m_sectionInfos[i].min);
    m_maxValue = std::max(m_maxValue, m_sectionInfos[i].max);
    std::vector<ImagePixelType> dataWithoutZero;
    for (size_t j=0; j<length; ++j) {
      if (data[j] > 0)
        dataWithoutZero.push_back(data[j]);
    }
    if (dataWithoutZero.empty()) {
      m_sectionInfos[i].mean = 0;
      m_sectionInfos[i].std = 0;
      m_sectionInfos[i].median = 0;
    } else {
      meanAndStandardDeviation(dataWithoutZero.begin(), dataWithoutZero.end(), m_sectionInfos[i].mean, m_sectionInfos[i].std);
      m_sectionInfos[i].median = medianInPlace(dataWithoutZero.begin(), dataWithoutZero.end());
    }
  }
}

} // namespace nim
