#include "zimgnccmatch.h"

#include "zlog.h"
#include "zimgncc.h"
#include "zimgautothreshold.h"
#include <QStringList>
#include <array>

namespace {

using namespace nim;

template<typename TVoxel>
void removeBG(ZImg& img)
{
  ZImgAutoThreshold<> autoThre;
  img -= autoThre.typedMaxHistThre<TVoxel>(img);
}

// ting's method
struct WeightNCCTing
{
  explicit WeightNCCTing(double overlapThre)
    : m_overlapThre(overlapThre)
  {}

  inline double operator()(double v, double numOverlap)
  {
    return numOverlap < m_overlapThre ? 0 : v <= 0 ? v : v *
                                                         std::log(v * std::sqrt((numOverlap - 2) / (1.0001 - v * v)));
  }

private:
  double m_overlapThre;
};

}  // namespace

namespace nim {

ZImgNCCMatch::ZImgNCCMatch(const ZImg& fixedImg, const ZImg& movingImg,
                           size_t fixedT, size_t movingT)
  : m_fixedImg(fixedImg), m_movingImg(movingImg)
  , m_fixedT(fixedT), m_movingT(movingT)
{
  init();
}

void ZImgNCCMatch::setMovingImgPositionHint(PositionHint hint, double maxOverlapRate)
{
  // make it correct
  if (is_flag_set(hint, PositionHint::Left) && is_flag_set(hint, PositionHint::Right)) {
    flip_flag(hint, PositionHint::Left);
    flip_flag(hint, PositionHint::Right);
  }
  if (is_flag_set(hint, PositionHint::Up) && is_flag_set(hint, PositionHint::Down)) {
    flip_flag(hint, PositionHint::Up);
    flip_flag(hint, PositionHint::Down);
  }
  if (is_flag_set(hint, PositionHint::Front) && is_flag_set(hint, PositionHint::Back)) {
    flip_flag(hint, PositionHint::Front);
    flip_flag(hint, PositionHint::Back);
  }
  m_movingImgPosHint = hint;
  CHECK(maxOverlapRate >= 0.01 && maxOverlapRate <= 1.0);
  m_maxOverlapRate = maxOverlapRate;
}

QString ZImgNCCMatch::positionHintToQString() const
{
  return positionHintToQString(m_movingImgPosHint, m_maxOverlapRate);
}

void ZImgNCCMatch::reversePositionHint(PositionHint& hint)
{
  // make it correct
  if (is_flag_set(hint, PositionHint::Left) && is_flag_set(hint, PositionHint::Right)) {
    flip_flag(hint, PositionHint::Left);
    flip_flag(hint, PositionHint::Right);
  }
  if (is_flag_set(hint, PositionHint::Up) && is_flag_set(hint, PositionHint::Down)) {
    flip_flag(hint, PositionHint::Up);
    flip_flag(hint, PositionHint::Down);
  }
  if (is_flag_set(hint, PositionHint::Front) && is_flag_set(hint, PositionHint::Back)) {
    flip_flag(hint, PositionHint::Front);
    flip_flag(hint, PositionHint::Back);
  }
  // reverse
  if (is_flag_set(hint, PositionHint::Left)) {
    flip_flag(hint, PositionHint::Left);
    set_flag(hint, PositionHint::Right);
  } else if (is_flag_set(hint, PositionHint::Right)) {
    flip_flag(hint, PositionHint::Right);
    set_flag(hint, PositionHint::Left);
  }
  if (is_flag_set(hint, PositionHint::Up)) {
    flip_flag(hint, PositionHint::Up);
    set_flag(hint, PositionHint::Down);
  } else if (is_flag_set(hint, PositionHint::Down)) {
    flip_flag(hint, PositionHint::Down);
    set_flag(hint, PositionHint::Up);
  }
  if (is_flag_set(hint, PositionHint::Front)) {
    flip_flag(hint, PositionHint::Front);
    set_flag(hint, PositionHint::Back);
  } else if (is_flag_set(hint, PositionHint::Back)) {
    flip_flag(hint, PositionHint::Back);
    set_flag(hint, PositionHint::Front);
  }
}

QString ZImgNCCMatch::positionHintToQString(PositionHint hint, double maxOverlapRate)
{
  QStringList hintList;
  if (is_flag_set(hint, PositionHint::Left)) {
    hintList << "Left";
  } else if (is_flag_set(hint, PositionHint::Right)) {
    hintList << "Right";
  }
  if (is_flag_set(hint, PositionHint::Up)) {
    hintList << "Up";
  } else if (is_flag_set(hint, PositionHint::Down)) {
    hintList << "Down";
  }
  if (is_flag_set(hint, PositionHint::Front)) {
    hintList << "Front";
  } else if (is_flag_set(hint, PositionHint::Back)) {
    hintList << "Back";
  }
  QString res;
  if (hintList.empty()) {
    res = "None";
  } else {
    res = hintList.join(" | ");
  }
  if (maxOverlapRate < 1.0) {
    res += QString(", Max Overlap Rate %1").arg(maxOverlapRate);
  }
  return res;
}

void ZImgNCCMatch::useFixedImgChannel(size_t ch)
{
  std::vector<size_t> chs;
  chs.push_back(ch);
  useFixedImgChannel(chs);
}

void ZImgNCCMatch::useMovingImgChannel(size_t ch)
{
  std::vector<size_t> chs;
  chs.push_back(ch);
  useMovingImgChannel(chs);
}

void ZImgNCCMatch::useFixedImgChannel(size_t ch1, size_t ch2)
{
  std::vector<size_t> chs;
  chs.push_back(ch1);
  chs.push_back(ch2);
  useFixedImgChannel(chs);
}

void ZImgNCCMatch::useMovingImgChannel(size_t ch1, size_t ch2)
{
  std::vector<size_t> chs;
  chs.push_back(ch1);
  chs.push_back(ch2);
  useMovingImgChannel(chs);
}

void ZImgNCCMatch::useFixedImgChannel(size_t ch1, size_t ch2, size_t ch3)
{
  std::vector<size_t> chs;
  chs.push_back(ch1);
  chs.push_back(ch2);
  chs.push_back(ch3);
  useFixedImgChannel(chs);
}

void ZImgNCCMatch::useMovingImgChannel(size_t ch1, size_t ch2, size_t ch3)
{
  std::vector<size_t> chs;
  chs.push_back(ch1);
  chs.push_back(ch2);
  chs.push_back(ch3);
  useMovingImgChannel(chs);
}

void ZImgNCCMatch::useFixedImgChannel(const std::vector<size_t>& chs)
{
  for (size_t i = 0; i < chs.size(); ++i) {
    checkFixedImgChannel(chs[i]);
  }

  m_fixedImgChannelsToUse.clear();
  for (size_t i = 0; i < chs.size(); ++i) {
    m_fixedImgChannelsToUse.insert(chs[i]);
  }
}

void ZImgNCCMatch::useMovingImgChannel(const std::vector<size_t>& chs)
{
  for (size_t i = 0; i < chs.size(); ++i) {
    checkMovingImgChannel(chs[i]);
  }

  m_movingImgChannelsToUse.clear();
  for (size_t i = 0; i < chs.size(); ++i) {
    m_movingImgChannelsToUse.insert(chs[i]);
  }
}

void ZImgNCCMatch::useAllFixedImgChannels()
{
  m_fixedImgChannelsToUse.clear();
  for (size_t c = 0; c < m_fixedImg.numChannels(); ++c)
    m_fixedImgChannelsToUse.insert(c);
}

void ZImgNCCMatch::useAllMovingImgChannels()
{
  m_movingImgChannelsToUse.clear();
  for (size_t c = 0; c < m_movingImg.numChannels(); ++c)
    m_movingImgChannelsToUse.insert(c);
}

void ZImgNCCMatch::enableRemoveBackgroundForFixedImgChannel(size_t ch)
{
  checkFixedImgChannel(ch);
  m_fixedImgChannelsToRemoveBackground.insert(ch);
}

void ZImgNCCMatch::enableRemoveBackgroundForMovingImgChannel(size_t ch)
{
  checkMovingImgChannel(ch);
  m_movingImgChannelsToRemoveBackground.insert(ch);
}

void ZImgNCCMatch::disableRemoveBackgroundForFixedImgChannel(size_t ch)
{
  checkFixedImgChannel(ch);
  m_fixedImgChannelsToRemoveBackground.erase(ch);
}

void ZImgNCCMatch::disableRemoveBackgroundForMovingImgChannel(size_t ch)
{
  checkMovingImgChannel(ch);
  m_movingImgChannelsToRemoveBackground.erase(ch);
}

void ZImgNCCMatch::enableRemoveBackgroundForAllFixedImgChannels()
{
  for (size_t c = 0; c < m_fixedImg.numChannels(); ++c)
    m_fixedImgChannelsToRemoveBackground.insert(c);
}

void ZImgNCCMatch::enableRemoveBackgroundForAllMovingImgChannels()
{
  for (size_t c = 0; c < m_movingImg.numChannels(); ++c)
    m_movingImgChannelsToRemoveBackground.insert(c);
}

void ZImgNCCMatch::disableRemoveBackgroundForAllFixedImgChannels()
{
  m_fixedImgChannelsToRemoveBackground.clear();
}

void ZImgNCCMatch::disableRemoveBackgroundForAllMovingImgChannels()
{
  m_movingImgChannelsToRemoveBackground.clear();
}

ZVoxelCoordinate ZImgNCCMatch::computeMovingImgOffset(double* maxNCC, double* maxWeightedNCC, double* numOverlapVoxels)
{
  if (m_fixedImg.isEmpty() || m_movingImg.isEmpty()) {
    throw ZImgException("computeMovingImgOffset: Can not match empty imgs");
  }
  ZVoxelCoordinate res;
  if (m_movingImgPosHint == PositionHint::None && m_maxOverlapRate < 1) {
    std::array<PositionHint, 4> hints{{PositionHint::Left, PositionHint::Right, PositionHint::Up, PositionHint::Down}};
    double wncc = std::numeric_limits<double>::lowest();
    for (PositionHint hint : hints) {
      double tmpMaxNCC;
      double tmpMaxWeightedNCC;
      double tmpNumOverlapVoxels;
      ZVoxelCoordinate tmpRes = computeMovingImgOffset(hint, m_maxOverlapRate,
                                                       tmpMaxNCC, tmpMaxWeightedNCC, tmpNumOverlapVoxels);
      if (tmpMaxWeightedNCC > wncc) {
        wncc = tmpMaxWeightedNCC;
        res = tmpRes;
        if (maxNCC) *maxNCC = tmpMaxNCC;
        if (maxWeightedNCC) *maxWeightedNCC = tmpMaxWeightedNCC;
        if (numOverlapVoxels) *numOverlapVoxels = tmpNumOverlapVoxels;
      }
    }
  } else {
    double tmpMaxNCC;
    double tmpMaxWeightedNCC;
    double tmpNumOverlapVoxels;
    res = computeMovingImgOffset(m_movingImgPosHint, m_maxOverlapRate,
                                 tmpMaxNCC, tmpMaxWeightedNCC, tmpNumOverlapVoxels);
    if (maxNCC) *maxNCC = tmpMaxNCC;
    if (maxWeightedNCC) *maxWeightedNCC = tmpMaxWeightedNCC;
    if (numOverlapVoxels) *numOverlapVoxels = tmpNumOverlapVoxels;
  }
  return res;
}

ZVoxelCoordinate ZImgNCCMatch::computeMovingImgOffsetMR(size_t intvX, size_t intvY, size_t intvZ,
                                                        double* maxNCC, double* maxWeightedNCC,
                                                        double* numOverlapVoxels,
                                                        double* lowResMaxNCC, double* lowResMaxWeightedNCC,
                                                        double* lowResNumOverlapVoxels)
{
  if (m_fixedImg.isEmpty() || m_movingImg.isEmpty()) {
    throw ZImgException("computeMovingImgOffset: Can not match empty imgs");
  }
  ZVoxelCoordinate res;
  if (m_movingImgPosHint == PositionHint::None && m_maxOverlapRate < 1) {
    std::array<PositionHint, 4> hints{{PositionHint::Left, PositionHint::Right, PositionHint::Up, PositionHint::Down}};
    double wncc = std::numeric_limits<double>::lowest();
    for (PositionHint hint : hints) {
      double tmpMaxNCC;
      double tmpMaxWeightedNCC;
      double tmpNumOverlapVoxels;
      double tmpLowResMaxNCC;
      double tmpLowResMaxWeightedNCC;
      double tmpLowResNumOverlapVoxels;
      ZVoxelCoordinate tmpRes = computeMovingImgOffsetMR(hint, m_maxOverlapRate,
                                                         intvX, intvY, intvZ,
                                                         tmpMaxNCC, tmpMaxWeightedNCC, tmpNumOverlapVoxels,
                                                         tmpLowResMaxNCC, tmpLowResMaxWeightedNCC,
                                                         tmpLowResNumOverlapVoxels);
      if (tmpMaxWeightedNCC > wncc) {
        wncc = tmpMaxWeightedNCC;
        res = tmpRes;
        if (maxNCC) *maxNCC = tmpMaxNCC;
        if (maxWeightedNCC) *maxWeightedNCC = tmpMaxWeightedNCC;
        if (numOverlapVoxels) *numOverlapVoxels = tmpNumOverlapVoxels;
        if (lowResMaxNCC) *lowResMaxNCC = tmpLowResMaxNCC;
        if (lowResMaxWeightedNCC) *lowResMaxWeightedNCC = tmpLowResMaxWeightedNCC;
        if (lowResNumOverlapVoxels) *lowResNumOverlapVoxels = tmpLowResNumOverlapVoxels;
      }
    }
  } else {
    double tmpMaxNCC;
    double tmpMaxWeightedNCC;
    double tmpNumOverlapVoxels;
    double tmpLowResMaxNCC;
    double tmpLowResMaxWeightedNCC;
    double tmpLowResNumOverlapVoxels;
    res = computeMovingImgOffsetMR(m_movingImgPosHint, m_maxOverlapRate,
                                   intvX, intvY, intvZ,
                                   tmpMaxNCC, tmpMaxWeightedNCC, tmpNumOverlapVoxels,
                                   tmpLowResMaxNCC, tmpLowResMaxWeightedNCC, tmpLowResNumOverlapVoxels);
    if (maxNCC) *maxNCC = tmpMaxNCC;
    if (maxWeightedNCC) *maxWeightedNCC = tmpMaxWeightedNCC;
    if (numOverlapVoxels) *numOverlapVoxels = tmpNumOverlapVoxels;
    if (lowResMaxNCC) *lowResMaxNCC = tmpLowResMaxNCC;
    if (lowResMaxWeightedNCC) *lowResMaxWeightedNCC = tmpLowResMaxWeightedNCC;
    if (lowResNumOverlapVoxels) *lowResNumOverlapVoxels = tmpLowResNumOverlapVoxels;
  }
  return res;
}

ZVoxelCoordinate ZImgNCCMatch::refineMovingImgOffset(const ZVoxelCoordinate& offset,
                                                     size_t intvX, size_t intvY, size_t intvZ,
                                                     double* maxNCC, double* maxWeightedNCC, double* numOverlapVoxels)
{
  if (m_fixedImg.isEmpty() || m_movingImg.isEmpty()) {
    throw ZImgException("computeMovingImgOffset: Can not match empty imgs");
  }
  ZVoxelCoordinate res;

  ZImg fixedImg;
  ZImg movingImg;
  constructSingleChannelFixedImg(ZImgRegion(), fixedImg);
  constructSingleChannelMovingImg(ZImgRegion(), movingImg);
  //LOG(INFO) << fixedImg.info().toQString();
  //LOG(INFO) << movingImg.info().toQString();

  double tmpMaxNCC;
  double tmpMaxWeightedNCC;
  double tmpNumOverlapVoxels;
  ZImg subFixedImg;
  ZImg subMovingImg;
  cropOverlapSubImg(fixedImg, movingImg, offset, subFixedImg, subMovingImg);
  size_t xEnd = subMovingImg.width() + subFixedImg.width() - 1;
  size_t yEnd = subMovingImg.height() + subFixedImg.height() - 1;
  size_t zEnd = subMovingImg.depth() + subFixedImg.depth() - 1;
  size_t xStart = std::max(0, static_cast<int>(subMovingImg.width()) - 1 - static_cast<int>(intvX));
  xEnd = std::min(xEnd, subMovingImg.width() + intvX);
  size_t yStart = std::max(0, static_cast<int>(subMovingImg.height()) - 1 - static_cast<int>(intvY));
  yEnd = std::min(yEnd, subMovingImg.height() + intvY);
  size_t zStart = std::max(0, static_cast<int>(subMovingImg.depth()) - 1 - static_cast<int>(intvZ));
  zEnd = std::min(zEnd, subMovingImg.depth() + intvZ);
  res = maxNormXCorrLocPart(subFixedImg, subMovingImg, xStart, xEnd, yStart, yEnd, zStart, zEnd,
                            tmpMaxNCC, tmpMaxWeightedNCC, tmpNumOverlapVoxels);
  res += offset;
  //LOG(INFO) << "final offset: " << res;

  if (maxNCC) *maxNCC = tmpMaxNCC;
  if (maxWeightedNCC) *maxWeightedNCC = tmpMaxWeightedNCC;
  if (numOverlapVoxels) *numOverlapVoxels = tmpNumOverlapVoxels;
  return res;
}

double ZImgNCCMatch::computeNCCOfOffset(const ZVoxelCoordinate& offset)
{
  if (m_fixedImg.isEmpty() || m_movingImg.isEmpty()) {
    throw ZImgException("computeNCCOfOffset: Can not match empty imgs");
  }

  ZImg fixedImg;
  ZImg movingImg;

  constructSingleChannelFixedImg(ZImgRegion(), fixedImg);
  constructSingleChannelMovingImg(ZImgRegion(), movingImg);

  return getNCCOfOffset(fixedImg, movingImg, offset);
}

void ZImgNCCMatch::init()
{
  useAllFixedImgChannels();
  useAllMovingImgChannels();
  disableRemoveBackgroundForAllFixedImgChannels();
  disableRemoveBackgroundForAllMovingImgChannels();
}

void ZImgNCCMatch::checkFixedImgChannel(size_t ch)
{
  if (ch >= m_fixedImg.numChannels()) {
    throw ZImgException(QString("Wrong channel %1 for fixed img <%2>").arg(ch).arg(m_fixedImg.info().toQString()));
  }
}

void ZImgNCCMatch::checkMovingImgChannel(size_t ch)
{
  if (ch >= m_movingImg.numChannels()) {
    throw ZImgException(QString("Wrong channel %1 for moving img <%2>").arg(ch).arg(m_movingImg.info().toQString()));
  }
}

void ZImgNCCMatch::constructSingleChannelFixedImg(const ZImgRegion& rgn, ZImg& fixedImg)
{
  ZImgInfo info = m_fixedImg.info();
  info.numChannels = 1;
  info.numTimes = 1;
  if (rgn.containsWholeImg(info)) {
    if (m_fixedImgChannelsToUse.size() == 1) {
      size_t ch = *(m_fixedImgChannelsToUse.begin());
      if (m_fixedImgChannelsToRemoveBackground.find(ch) == m_fixedImgChannelsToRemoveBackground.end()) {
        fixedImg = m_fixedImg.createView(ch, m_fixedT); // virtual img
      } else {
        fixedImg = m_fixedImg.extractChannel(ch, m_fixedT);
        removeBackground(fixedImg);
      }
    } else if (m_fixedImgChannelsToUse.size() > 1) {
      std::vector<ZImg> imgs(m_fixedImgChannelsToUse.size());
      size_t idx = 0;
      for (auto ch : m_fixedImgChannelsToUse) {
        if (m_fixedImgChannelsToRemoveBackground.find(ch) == m_fixedImgChannelsToRemoveBackground.end()) {
          imgs[idx] = m_fixedImg.createView(ch, m_fixedT); // virtual img
        } else {
          imgs[idx] = m_fixedImg.extractChannel(ch, m_fixedT);
          removeBackground(imgs[idx]);
        }
        idx++;
      }
      fixedImg = ZImg::combine(imgs, ImgMergeMode::Mean);
    }
  } else {
    if (m_fixedImgChannelsToUse.size() == 1) {
      size_t ch = *(m_fixedImgChannelsToUse.begin());
      ZImgRegion tmpRgn = rgn;
      tmpRgn.start.c = ch;
      tmpRgn.end.c = ch + 1;
      tmpRgn.start.t = m_fixedT;
      tmpRgn.end.t = m_fixedT + 1;
      fixedImg = m_fixedImg.crop(tmpRgn);
      if (m_fixedImgChannelsToRemoveBackground.find(ch) != m_fixedImgChannelsToRemoveBackground.end()) {
        removeBackground(fixedImg);
      }
    } else if (m_fixedImgChannelsToUse.size() > 1) {
      std::vector<ZImg> imgs(m_fixedImgChannelsToUse.size());
      size_t idx = 0;
      for (auto ch : m_fixedImgChannelsToUse) {
        ZImgRegion tmpRgn = rgn;
        tmpRgn.start.c = ch;
        tmpRgn.end.c = ch + 1;
        tmpRgn.start.t = m_fixedT;
        tmpRgn.end.t = m_fixedT + 1;
        imgs[idx] = m_fixedImg.crop(tmpRgn);
        if (m_fixedImgChannelsToRemoveBackground.find(ch) != m_fixedImgChannelsToRemoveBackground.end()) {
          removeBackground(imgs[idx]);
        }
        idx++;
      }
      fixedImg = ZImg::combine(imgs, ImgMergeMode::Mean);
    }
  }
}

void ZImgNCCMatch::constructSingleChannelMovingImg(const ZImgRegion& rgn, ZImg& movingImg)
{
  ZImgInfo info = m_movingImg.info();
  info.numChannels = 1;
  info.numTimes = 1;
  if (rgn.containsWholeImg(info)) {
    if (m_movingImgChannelsToUse.size() == 1) {
      size_t ch = *(m_movingImgChannelsToUse.begin());
      if (m_movingImgChannelsToRemoveBackground.find(ch) == m_movingImgChannelsToRemoveBackground.end()) {
        movingImg = m_movingImg.createView(ch, m_movingT); // virtual img
      } else {
        movingImg = m_movingImg.extractChannel(ch, m_movingT);
        removeBackground(movingImg);
      }
    } else if (m_movingImgChannelsToUse.size() > 1) {
      std::vector<ZImg> imgs(m_movingImgChannelsToUse.size());
      size_t idx = 0;
      for (auto ch : m_movingImgChannelsToUse) {
        if (m_movingImgChannelsToRemoveBackground.find(ch) == m_movingImgChannelsToRemoveBackground.end()) {
          imgs[idx] = m_movingImg.createView(ch, m_movingT); // virtual img
        } else {
          imgs[idx] = m_movingImg.extractChannel(ch, m_movingT);
          removeBackground(imgs[idx]);
        }
        idx++;
      }
      movingImg = ZImg::combine(imgs, ImgMergeMode::Mean);
    }
  } else {
    if (m_movingImgChannelsToUse.size() == 1) {
      size_t ch = *(m_movingImgChannelsToUse.begin());
      ZImgRegion tmpRgn = rgn;
      tmpRgn.start.c = ch;
      tmpRgn.end.c = ch + 1;
      tmpRgn.start.t = m_movingT;
      tmpRgn.end.t = m_movingT + 1;
      movingImg = m_movingImg.crop(tmpRgn);
      if (m_movingImgChannelsToRemoveBackground.find(ch) != m_movingImgChannelsToRemoveBackground.end()) {
        removeBackground(movingImg);
      }
    } else if (m_movingImgChannelsToUse.size() > 1) {
      std::vector<ZImg> imgs(m_movingImgChannelsToUse.size());
      size_t idx = 0;
      for (auto ch : m_movingImgChannelsToUse) {
        ZImgRegion tmpRgn = rgn;
        tmpRgn.start.c = ch;
        tmpRgn.end.c = ch + 1;
        tmpRgn.start.t = m_movingT;
        tmpRgn.end.t = m_movingT + 1;
        imgs[idx] = m_movingImg.crop(tmpRgn);
        if (m_movingImgChannelsToRemoveBackground.find(ch) != m_movingImgChannelsToRemoveBackground.end()) {
          removeBackground(imgs[idx]);
        }
        idx++;
      }
      movingImg = ZImg::combine(imgs, ImgMergeMode::Mean);
    }
  }
}

void ZImgNCCMatch::removeBackground(ZImg& img)
{
  IMG_TYPED_CALL(removeBG, img, img);
}

// reference: matlab code normxcorr2_general.m
ZVoxelCoordinate ZImgNCCMatch::maxNormXCorrLoc(ZImg& fixedImg, ZImg& movingImg,
                                               const ZImgRegion& nccImgValidRegion,
                                               double requiredNumberOfOverlapPixels,
                                               double& maxNCC, double& maxWeightedNCC, double& numOverlapVoxels)
{
  ZImgInfo movingImgInfo = movingImg.info();

  ZImg nccImg;
  ZImg numberOfOverlapVoxelsImg;
  normXCorr(fixedImg, movingImg, nccImg, numberOfOverlapVoxelsImg);

  CHECK(!nccImg.isTimeSeries() && !nccImg.isMultiChannelsImg());
  if (!nccImgValidRegion.containsWholeImg(nccImg.info())) {
    nccImg = nccImg.crop(nccImgValidRegion);
    numberOfOverlapVoxelsImg = numberOfOverlapVoxelsImg.crop(nccImgValidRegion);
  }
  size_t maxNCCIdx = getMaxWeightedNCCIdx(nccImg.channelData<double>(0),
                                          numberOfOverlapVoxelsImg.channelData<double>(0),
                                          requiredNumberOfOverlapPixels, nccImg.channelVoxelNumber(),
                                          maxNCC, maxWeightedNCC, numOverlapVoxels);
  ZVoxelCoordinate maxNCCCoord = nccImg.indexToCoord(maxNCCIdx);
  ZVoxelCoordinate offset = maxNCCCoord + nccImgValidRegion.start -
                            ZVoxelCoordinate(movingImgInfo.width - 1, movingImgInfo.height - 1,
                                             movingImgInfo.depth - 1);
  LOG(INFO) << "max NCC coord: " << maxNCCCoord;
  LOG(INFO) << "moving image offset: " << offset;

  return offset;
}

ZVoxelCoordinate ZImgNCCMatch::maxNormXCorrLoc_S(ZImg& fixedImg, ZImg& movingImg,
                                                 const ZImgRegion& nccImgValidRegion,
                                                 double requiredNumberOfOverlapPixels,
                                                 double& maxNCC, double& maxWeightedNCC, double& numOverlapVoxels)
{
  ZImgInfo fixedImgInfo = fixedImg.info();
  ZImgInfo movingImgInfo = movingImg.info();
  LOG(INFO) << fixedImgInfo.toQString();
  LOG(INFO) << movingImgInfo.toQString();

  ZImg nccImg;
  ZImg numberOfOverlapVoxelsImg;
  normXCorr(fixedImg, movingImg, nccImg, numberOfOverlapVoxelsImg);

  CHECK(!nccImg.isTimeSeries() && !nccImg.isMultiChannelsImg());
  if (!nccImgValidRegion.containsWholeImg(nccImg.info())) {
    nccImg = nccImg.crop(nccImgValidRegion);
    numberOfOverlapVoxelsImg = numberOfOverlapVoxelsImg.crop(nccImgValidRegion);
  }
  size_t maxNCCIdx = getMaxWeightedNCCIdx(nccImg.channelData<double>(0),
                                          numberOfOverlapVoxelsImg.channelData<double>(0),
                                          requiredNumberOfOverlapPixels, nccImg.channelVoxelNumber(),
                                          maxNCC, maxWeightedNCC, numOverlapVoxels);
  ZVoxelCoordinate maxNCCCoord = nccImg.indexToCoord(maxNCCIdx);
  ZVoxelCoordinate offset = maxNCCCoord + nccImgValidRegion.start -
                            ZVoxelCoordinate(movingImgInfo.width - 1, movingImgInfo.height - 1,
                                             movingImgInfo.depth - 1);
  LOG(INFO) << "max NCC coord: " << maxNCCCoord;
  LOG(INFO) << nccImgValidRegion.toQString();
  LOG(INFO) << "moving image offset: " << offset;

  return offset;
}

ZVoxelCoordinate ZImgNCCMatch::maxNormXCorrLocPart(ZImg& fixedImg, ZImg& movingImg, size_t xStart, size_t xEnd,
                                                   size_t yStart, size_t yEnd, size_t zStart, size_t zEnd,
                                                   double& maxNCC, double& maxWeightedNCC, double& numOverlapVoxels)
{
  ZImgInfo fixedImgInfo = fixedImg.info();
  ZImgInfo movingImgInfo = movingImg.info();

  ZImg nccImg;
  ZImg numberOfOverlapVoxelsImg;
  normXCorrPart(fixedImg, movingImg, xStart, xEnd, yStart, yEnd, zStart, zEnd, nccImg, numberOfOverlapVoxelsImg);

  size_t maxNCCIdx = getMaxWeightedNCCIdx(nccImg.channelData<double>(0),
                                          numberOfOverlapVoxelsImg.channelData<double>(0),
                                          1, nccImg.channelVoxelNumber(),
                                          maxNCC, maxWeightedNCC, numOverlapVoxels);
  ZVoxelCoordinate maxNCCCoord = nccImg.indexToCoord(maxNCCIdx);
  ZVoxelCoordinate offset = maxNCCCoord + ZVoxelCoordinate(xStart, yStart, zStart)
                            - ZVoxelCoordinate(movingImgInfo.width - 1, movingImgInfo.height - 1,
                                               movingImgInfo.depth - 1);
  LOG(INFO) << "max NCC coord: " << maxNCCCoord << " region " << xStart << xEnd << yStart << yEnd << zStart << zEnd;
  LOG(INFO) << "moving image offset: " << offset;

  return offset;
}

ZImgRegion ZImgNCCMatch::getNccImgValidRegion(const PositionHint& hint, const ZImgInfo& fixedImgInfo,
                                              const ZImgInfo& movingImgInfo)
{
  ZImgRegion rgn;
  if (is_flag_set(hint, PositionHint::Left)) {
    rgn.end.x = fixedImgInfo.width;
  } else if (is_flag_set(hint, PositionHint::Right)) {
    rgn.start.x = movingImgInfo.width - 1;
  }
  if (is_flag_set(hint, PositionHint::Up)) {
    rgn.end.y = fixedImgInfo.height;
  } else if (is_flag_set(hint, PositionHint::Down)) {
    rgn.start.y = movingImgInfo.height - 1;
  }
  if (is_flag_set(hint, PositionHint::Front)) {
    rgn.end.z = fixedImgInfo.depth;
  } else if (is_flag_set(hint, PositionHint::Back)) {
    rgn.start.z = movingImgInfo.depth - 1;
  }
  return rgn;
}

double ZImgNCCMatch::getRequiredNumberOfOverlapPixels(const PositionHint& hint, double minOverlapRate,
                                                      const ZImgInfo& fixedImgInfo, const ZImgInfo& movingImgInfo)
{
  double res = std::min(fixedImgInfo.channelVoxelNumber(), movingImgInfo.channelVoxelNumber()) * minOverlapRate;
  double l = 0;
  if (is_flag_set(hint, PositionHint::Left) || is_flag_set(hint, PositionHint::Right)) {
    l += 1;
  }
  if (is_flag_set(hint, PositionHint::Up) || is_flag_set(hint, PositionHint::Down)) {
    l += 1;
  }
  if (is_flag_set(hint, PositionHint::Front) || is_flag_set(hint, PositionHint::Back)) {
    l += 1;
  }
  if (l == 2) {
    res *= minOverlapRate;
  } else if (l == 3) {
    res *= minOverlapRate * minOverlapRate;
  }
  return std::max(res, 1e4);
}

size_t ZImgNCCMatch::getMaxWeightedNCCIdx(const double* NCCs, const double* overlapVoxels, double overlapVoxelThre,
                                          size_t dataLength,
                                          double& maxNCC, double& maxWeightedNCC, double& numOverlapVoxels)
{
  WeightNCCTing weightNCC(overlapVoxelThre);
  size_t i = 0;
  size_t maxNCCIdx = i;
  maxNCC = NCCs[i];
  numOverlapVoxels = overlapVoxels[i];
  double maxWeightedNCCTmp = weightNCC(NCCs[i], overlapVoxels[i]);
  for (i = 1; i < dataLength; ++i) {
    double weightedNCC = weightNCC(NCCs[i], overlapVoxels[i]);
    if (weightedNCC > maxWeightedNCCTmp) {
      maxWeightedNCCTmp = weightedNCC;
      maxNCCIdx = i;
      maxNCC = NCCs[i];
      numOverlapVoxels = overlapVoxels[i];
    }
  }
  LOG(INFO) << "max NCC: " << NCCs[maxNCCIdx] << " max weighted NCC: " << maxWeightedNCCTmp
            << " number of overlap voxels: " << overlapVoxels[maxNCCIdx];
  maxWeightedNCC = maxWeightedNCCTmp;
  return maxNCCIdx;
}

std::pair<ZImgRegion, ZImgRegion>
ZImgNCCMatch::getRequiredSrcImgRegion(const PositionHint& hint, const ZImg& fixedImg, const ZImg& movingImg,
                                      double overlapRate)
{
  ZImgRegion fixedRgn;
  ZImgRegion movingRgn;

  if (is_flag_set(hint, PositionHint::Left)) {
    size_t maxNumOverlapPixelsX = std::ceil(std::min(fixedImg.width(), movingImg.width()) * overlapRate);
    fixedRgn.end.x = std::min(maxNumOverlapPixelsX, fixedImg.width());
    CHECK(movingImg.width() >= maxNumOverlapPixelsX);
    movingRgn.start.x = movingImg.width() - maxNumOverlapPixelsX;
  } else if (is_flag_set(hint, PositionHint::Right)) {
    size_t maxNumOverlapPixelsX = std::ceil(std::min(fixedImg.width(), movingImg.width()) * overlapRate);
    movingRgn.end.x = std::min(maxNumOverlapPixelsX, movingImg.width());
    CHECK(fixedImg.width() >= maxNumOverlapPixelsX);
    fixedRgn.start.x = fixedImg.width() - maxNumOverlapPixelsX;
  }
  if (is_flag_set(hint, PositionHint::Up)) {
    size_t maxNumOverlapPixelsY = std::ceil(std::min(fixedImg.height(), movingImg.height()) * overlapRate);
    fixedRgn.end.y = std::min(maxNumOverlapPixelsY, fixedImg.height());
    CHECK(movingImg.height() >= maxNumOverlapPixelsY);
    movingRgn.start.y = movingImg.height() - maxNumOverlapPixelsY;
  } else if (is_flag_set(hint, PositionHint::Down)) {
    size_t maxNumOverlapPixelsY = std::ceil(std::min(fixedImg.height(), movingImg.height()) * overlapRate);
    movingRgn.end.y = std::min(maxNumOverlapPixelsY, movingImg.height());
    CHECK(fixedImg.height() >= maxNumOverlapPixelsY);
    fixedRgn.start.y = fixedImg.height() - maxNumOverlapPixelsY;
  }
  if (is_flag_set(hint, PositionHint::Front)) {
    size_t maxNumOverlapPixelsZ = std::ceil(std::min(fixedImg.depth(), movingImg.depth()) * overlapRate);
    fixedRgn.end.z = std::min(maxNumOverlapPixelsZ, fixedImg.depth());
    CHECK(movingImg.depth() >= maxNumOverlapPixelsZ);
    movingRgn.start.z = movingImg.depth() - maxNumOverlapPixelsZ;
  } else if (is_flag_set(hint, PositionHint::Back)) {
    size_t maxNumOverlapPixelsZ = std::ceil(std::min(fixedImg.depth(), movingImg.depth()) * overlapRate);
    movingRgn.end.z = std::min(maxNumOverlapPixelsZ, movingImg.depth());
    CHECK(fixedImg.depth() >= maxNumOverlapPixelsZ);
    fixedRgn.start.z = fixedImg.depth() - maxNumOverlapPixelsZ;
  }

  return std::make_pair(fixedRgn, movingRgn);
}

ZVoxelCoordinate ZImgNCCMatch::mapOffsetToSrcImg(ZVoxelCoordinate offset,
                                                 const ZImgRegion& fixedRgn, const ZImgRegion& movingRgn)
{
  return offset - movingRgn.start + fixedRgn.start;
}

ZVoxelCoordinate ZImgNCCMatch::computeMovingImgOffset(const PositionHint& movingImgPosHint, double maxOverlapRate,
                                                      double& maxNCC, double& maxWeightedNCC, double& numOverlapVoxels)
{
  ZVoxelCoordinate res;

  ZImgRegion fixedRgn;
  ZImgRegion movingRgn;
  std::tie(fixedRgn, movingRgn) = getRequiredSrcImgRegion(movingImgPosHint, m_fixedImg, m_movingImg, maxOverlapRate);

  ZImg fixedImg;
  ZImg movingImg;

  constructSingleChannelFixedImg(fixedRgn, fixedImg);
  constructSingleChannelMovingImg(movingRgn, movingImg);
  //LOG(INFO) << fixedImg.info().toQString();
  //LOG(INFO) << movingImg.info().toQString();

  res = maxNormXCorrLoc_S(fixedImg, movingImg,
                          getNccImgValidRegion(movingImgPosHint, fixedImg.info(), movingImg.info()),
                          getRequiredNumberOfOverlapPixels(movingImgPosHint, std::min(0.01, maxOverlapRate / 10),
                                                           fixedImg.info(), movingImg.info()),
                          maxNCC, maxWeightedNCC, numOverlapVoxels);
  res = mapOffsetToSrcImg(res, fixedRgn, movingRgn);

  return res;
}

ZVoxelCoordinate ZImgNCCMatch::computeMovingImgOffsetMR(const PositionHint& movingImgPosHint, double maxOverlapRate,
                                                        size_t intvX, size_t intvY, size_t intvZ,
                                                        double& maxNCC, double& maxWeightedNCC,
                                                        double& numOverlapVoxels,
                                                        double& lowResMaxNCC, double& lowResMaxWeightedNCC,
                                                        double& lowResNumOverlapVoxels)
{
  ZVoxelCoordinate res;

  intvX = std::min(intvX, std::min(m_fixedImg.width() - 1, m_movingImg.width() - 1));
  intvY = std::min(intvY, std::min(m_fixedImg.height() - 1, m_movingImg.height() - 1));
  intvZ = std::min(intvZ, std::min(m_fixedImg.depth() - 1, m_movingImg.depth() - 1));
  if (intvX == 0 && intvY == 0 && intvZ == 0) {
    double ncc;
    double wncc;
    double nop;
    res = computeMovingImgOffset(movingImgPosHint, maxOverlapRate, ncc, wncc, nop);
    maxNCC = ncc;
    lowResMaxNCC = ncc;
    maxWeightedNCC = wncc;
    lowResMaxWeightedNCC = wncc;
    numOverlapVoxels = nop;
    lowResNumOverlapVoxels = nop;
    return res;
  }

  ZImgRegion fixedRgn;
  ZImgRegion movingRgn;
  std::tie(fixedRgn, movingRgn) = getRequiredSrcImgRegion(movingImgPosHint, m_fixedImg, m_movingImg, maxOverlapRate);

  ZImg fixedImg;
  ZImg movingImg;

  constructSingleChannelFixedImg(fixedRgn, fixedImg);
  constructSingleChannelMovingImg(movingRgn, movingImg);
  //LOG(INFO) << fixedImg.info().toQString();
  //LOG(INFO) << movingImg.info().toQString();

#if 0
  double scaleX = 1. / (intvX + 1.);
  double scaleY = 1. / (intvY + 1.);
  double scaleZ = 1. / (intvZ + 1.);
  ZImg dsFixedImg = fixedImg.zoomed(scaleX, scaleY, scaleZ, Interpolant::Cubic);
  ZImg dsMovingImg = movingImg.zoomed(scaleX, scaleY, scaleZ, Interpolant::Cubic);
#else
  ZImg dsFixedImg = fixedImg.blockDownsampled(intvX + 1, intvY + 1, intvZ + 1, ImgMergeMode::Mean);
  ZImg dsMovingImg = movingImg.blockDownsampled(intvX + 1, intvY + 1, intvZ + 1, ImgMergeMode::Mean);
#endif

  ZImgInfo fixedInfo = m_fixedImg.info();
  ZImgInfo movingInfo = m_movingImg.info();
  fixedInfo.width /= double(intvX + 1);
  fixedInfo.height /= double(intvY + 1);
  fixedInfo.depth /= double(intvZ + 1);
  movingInfo.width /= double(intvX + 1);
  movingInfo.height /= double(intvY + 1);
  movingInfo.depth /= double(intvZ + 1);
  ZVoxelCoordinate offset = maxNormXCorrLoc_S(dsFixedImg, dsMovingImg,
                                              getNccImgValidRegion(movingImgPosHint, dsFixedImg.info(),
                                                                   dsMovingImg.info()),
                                              getRequiredNumberOfOverlapPixels(movingImgPosHint,
                                                                               std::min(0.01, maxOverlapRate / 10),
                                                                               fixedInfo, movingInfo),
                                              lowResMaxNCC, lowResMaxWeightedNCC, lowResNumOverlapVoxels);

  offset *= ZVoxelCoordinate(intvX + 1, intvY + 1, intvZ + 1, 1, 1);
  offset.x = std::max(1 - static_cast<int>(movingImg.width()),
                      std::min(static_cast<int>(fixedImg.width()) - 1, offset.x));
  offset.y = std::max(1 - static_cast<int>(movingImg.height()),
                      std::min(static_cast<int>(fixedImg.height()) - 1, offset.y));
  offset.z = std::max(1 - static_cast<int>(movingImg.depth()),
                      std::min(static_cast<int>(fixedImg.depth()) - 1, offset.z));

#if 0
  double maxNCC = std::numeric_limits<double>::min();
  res = offset;
  for (int mx = -(int)intvX-1; mx <= (int)intvX+1; ++mx) {
    for (int my = -(int)intvY-1; my <= (int)intvY+1; ++my) {
      for (int mz = -(int)intvZ-1; mz <= (int)intvZ+1; ++mz) {
        ZVoxelCoordinate newOffset = offset + ZVoxelCoordinate(mx, my, mz);
        double ncc = getNCCOfOffset(fixedImg, movingImg, newOffset);
        if (ncc > maxNCC) {
          maxNCC = ncc;
          res = newOffset;
        }
      }
    }
  }
  //LOG(INFO) << "max NCC of full scale img: " << maxNCC << " offset: " << res;
#else
  ZImg subFixedImg;
  ZImg subMovingImg;
  cropOverlapSubImg(fixedImg, movingImg, offset, subFixedImg, subMovingImg);
  size_t xEnd = subMovingImg.width() + subFixedImg.width() - 1;
  size_t yEnd = subMovingImg.height() + subFixedImg.height() - 1;
  size_t zEnd = subMovingImg.depth() + subFixedImg.depth() - 1;
  size_t xStart = std::max(0, static_cast<int>(subMovingImg.width()) - 1 - static_cast<int>(intvX) - 1);
  xEnd = std::min(xEnd, subMovingImg.width() + intvX + 1);
  size_t yStart = std::max(0, static_cast<int>(subMovingImg.height()) - 1 - static_cast<int>(intvY) - 1);
  yEnd = std::min(yEnd, subMovingImg.height() + intvY + 1);
  size_t zStart = std::max(0, static_cast<int>(subMovingImg.depth()) - 1 - static_cast<int>(intvZ) - 1);
  zEnd = std::min(zEnd, subMovingImg.depth() + intvZ + 1);
  res = maxNormXCorrLocPart(subFixedImg, subMovingImg, xStart, xEnd, yStart, yEnd, zStart, zEnd, maxNCC, maxWeightedNCC,
                            numOverlapVoxels);
  res += offset;
  res = mapOffsetToSrcImg(res, fixedRgn, movingRgn);
  //LOG(INFO) << "final offset: " << res;
#endif

  return res;
}

} // namespace nim
