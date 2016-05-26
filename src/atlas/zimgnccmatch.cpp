#include "zimgnccmatch.h"

#include "QsLog.h"
#include <algorithm>
#include "zimgncc.h"
#include "zimgautothreshold.h"
#include <QStringList>

namespace {

using namespace nim;

template<typename TVoxel>
void removeBG(ZImg &img)
{
  ZImgAutoThreshold<> autoThre;
  img -= autoThre.typedMaxHistThre<TVoxel>(img);
}

// ting's method
struct WeightNCCTing
{
  WeightNCCTing(double overlapThre)
    : m_overlapThre(overlapThre)
  {}
  inline double operator()(double v, double numOverlap)
  {
    return numOverlap < m_overlapThre ? 0 : v <= 0 ? v : v * std::log(v * std::sqrt((numOverlap-2)/(1.0001-v*v)));
  }
private:
  double m_overlapThre;
};

}

namespace nim {

ZImgNCCMatch::ZImgNCCMatch(const ZImg& fixedImg, const ZImg& movingImg,
                           size_t fixedT, size_t movingT)
  : m_fixedImg(fixedImg), m_movingImg(movingImg)
  , m_fixedT(fixedT), m_movingT(movingT)
  , m_movingImgPosHint(None)
  , m_overlapRate(1.)
{
  init();
}

void ZImgNCCMatch::setMovingImgPositionHint(PositionHint hint, double overlapRate)
{
  // make it correct
  if (hint.testFlag(Left) && hint.testFlag(Right)) {
    hint ^= Left;
    hint ^= Right;
  }
  if (hint.testFlag(Up) && hint.testFlag(Down)) {
    hint ^= Up;
    hint ^= Down;
  }
  if (hint.testFlag(Front) && hint.testFlag(Back)) {
    hint ^= Front;
    hint ^= Back;
  }
  m_movingImgPosHint = hint;
  assert(overlapRate >= 0.01 && overlapRate <= 1.0);
  m_overlapRate = overlapRate;
}

QString ZImgNCCMatch::positionHintToQString() const
{
  return positionHintToQString(m_movingImgPosHint, m_overlapRate);
}

void ZImgNCCMatch::reversePositionHint(PositionHint &hint)
{
  // make it correct
  if (hint.testFlag(Left) && hint.testFlag(Right)) {
    hint ^= Left;
    hint ^= Right;
  }
  if (hint.testFlag(Up) && hint.testFlag(Down)) {
    hint ^= Up;
    hint ^= Down;
  }
  if (hint.testFlag(Front) && hint.testFlag(Back)) {
    hint ^= Front;
    hint ^= Back;
  }
  // reverse
  if (hint.testFlag(Left)) {
    hint |= Right;
    hint ^= Left;
  } else if (hint.testFlag(Right)) {
    hint |= Left;
    hint ^= Right;
  }
  if (hint.testFlag(Up)) {
    hint |= Down;
    hint ^= Up;
  } else if (hint.testFlag(Down)) {
    hint |= Up;
    hint ^= Down;
  }
  if (hint.testFlag(Front)) {
    hint |= Back;
    hint ^= Front;
  } else if (hint.testFlag(Back)) {
    hint |= Front;
    hint ^= Back;
  }
}

QString ZImgNCCMatch::positionHintToQString(PositionHint hint, double overlapRate)
{
  QStringList hintList;
  if (hint.testFlag(Left)) {
    hintList << "Left";
  } else if (hint.testFlag(Right)) {
    hintList << "Right";
  }
  if (hint.testFlag(Up)) {
    hintList << "Up";
  } else if (hint.testFlag(Down)) {
    hintList << "Down";
  }
  if (hint.testFlag(Front)) {
    hintList << "Front";
  } else if (hint.testFlag(Back)) {
    hintList << "Back";
  }
  QString res;
  if (hintList.empty()) {
    res = "None";
  } else {
    res = hintList.join(" | ");
  }
  if (overlapRate < 1.0) {
    res += QString(", Max Overlap %1").arg(overlapRate);
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

void ZImgNCCMatch::useFixedImgChannel(const std::vector<size_t> &chs)
{
  for (size_t i=0; i<chs.size(); ++i) {
    checkFixedImgChannel(chs[i]);
  }

  m_fixedImgChannelsToUse.clear();
  for (size_t i=0; i<chs.size(); ++i) {
    m_fixedImgChannelsToUse.insert(chs[i]);
  }
}

void ZImgNCCMatch::useMovingImgChannel(const std::vector<size_t> &chs)
{
  for (size_t i=0; i<chs.size(); ++i) {
    checkMovingImgChannel(chs[i]);
  }

  m_movingImgChannelsToUse.clear();
  for (size_t i=0; i<chs.size(); ++i) {
    m_movingImgChannelsToUse.insert(chs[i]);
  }
}

void ZImgNCCMatch::useAllFixedImgChannels()
{
  m_fixedImgChannelsToUse.clear();
  for (size_t c=0; c<m_fixedImg.numChannels(); ++c)
    m_fixedImgChannelsToUse.insert(c);
}

void ZImgNCCMatch::useAllMovingImgChannels()
{
  m_movingImgChannelsToUse.clear();
  for (size_t c=0; c<m_movingImg.numChannels(); ++c)
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
  for (size_t c=0; c<m_fixedImg.numChannels(); ++c)
    m_fixedImgChannelsToRemoveBackground.insert(c);
}

void ZImgNCCMatch::enableRemoveBackgroundForAllMovingImgChannels()
{
  for (size_t c=0; c<m_movingImg.numChannels(); ++c)
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

// todo: Position hint none and overlap < 1 not handled
ZVoxelCoordinate ZImgNCCMatch::computeMovingImgOffset(double *maxNCC, double *maxWeightedNCC, double *numOverlapVoxels)
{
  if (m_fixedImg.isEmpty() || m_movingImg.isEmpty()) {
    throw ZImgException("computeMovingImgOffset: Can not match empty imgs");
  }
  ZVoxelCoordinate res;

  ZImgRegion fixedRgn;
  ZImgRegion movingRgn;
  std::tie(fixedRgn, movingRgn) = getRequiredSrcImgRegion(m_movingImgPosHint, m_fixedImg, m_movingImg, m_overlapRate);

  ZImg fixedImg;
  ZImg movingImg;

  constructSingleChannelFixedImg(fixedRgn, fixedImg);
  constructSingleChannelMovingImg(movingRgn, movingImg);
  //LINFO() << fixedImg.info().toQString();
  //LINFO() << movingImg.info().toQString();

  res = maxNormXCorrLoc_S(fixedImg, movingImg, m_movingImgPosHint, maxNCC, maxWeightedNCC, numOverlapVoxels);
  res = mapOffsetToSrcImg(res, fixedRgn, movingRgn);

  return res;
}

ZVoxelCoordinate ZImgNCCMatch::computeMovingImgOffsetMR(size_t intvX, size_t intvY, size_t intvZ,
                                                        double *maxNCC, double *maxWeightedNCC, double *numOverlapVoxels,
                                                        double *lowResMaxNCC, double *lowResMaxWeightedNCC, double *lowResNumOverlapVoxels)
{
  if (m_fixedImg.isEmpty() || m_movingImg.isEmpty()) {
    throw ZImgException("computeMovingImgOffsetMR: Can not match empty imgs");
  }
  ZVoxelCoordinate res;

  intvX = std::min(intvX, std::min(m_fixedImg.width()-1, m_movingImg.width()-1));
  intvY = std::min(intvY, std::min(m_fixedImg.height()-1, m_movingImg.height()-1));
  intvZ = std::min(intvZ, std::min(m_fixedImg.depth()-1, m_movingImg.depth()-1));
  if (intvX == 0 && intvY == 0 && intvZ == 0) {
    double ncc;
    double wncc;
    double nop;
    res = computeMovingImgOffset(&ncc, &wncc, &nop);
    if (maxNCC) *maxNCC = ncc;
    if (lowResMaxNCC) *lowResMaxNCC = ncc;
    if (maxWeightedNCC) *maxWeightedNCC = wncc;
    if (lowResMaxWeightedNCC) *lowResMaxWeightedNCC = wncc;
    if (numOverlapVoxels) *numOverlapVoxels = nop;
    if (lowResNumOverlapVoxels) *lowResNumOverlapVoxels = nop;
    return res;
  }

  ZImgRegion fixedRgn;
  ZImgRegion movingRgn;
  std::tie(fixedRgn, movingRgn) = getRequiredSrcImgRegion(m_movingImgPosHint, m_fixedImg, m_movingImg, m_overlapRate);

  ZImg fixedImg;
  ZImg movingImg;

  constructSingleChannelFixedImg(fixedRgn, fixedImg);
  constructSingleChannelMovingImg(movingRgn, movingImg);
  //LINFO() << fixedImg.info().toQString();
  //LINFO() << movingImg.info().toQString();

#if 0
  double scaleX = 1. / (intvX + 1.);
  double scaleY = 1. / (intvY + 1.);
  double scaleZ = 1. / (intvZ + 1.);
  ZImg dsFixedImg = fixedImg.zoomed(scaleX, scaleY, scaleZ, Interpolant::Cubic);
  ZImg dsMovingImg = movingImg.zoomed(scaleX, scaleY, scaleZ, Interpolant::Cubic);
#else
  ZImg dsFixedImg = fixedImg.blockDownsampled(intvX+1, intvY+1, intvZ+1, ZImg::CombineMode::Mean);
  ZImg dsMovingImg = movingImg.blockDownsampled(intvX+1, intvY+1, intvZ+1, ZImg::CombineMode::Mean);
#endif

  ZVoxelCoordinate offset = maxNormXCorrLoc_S(dsFixedImg, dsMovingImg, m_movingImgPosHint, lowResMaxNCC, lowResMaxWeightedNCC, lowResNumOverlapVoxels);

  offset *= ZVoxelCoordinate(intvX+1, intvY+1, intvZ+1, 1, 1);
  offset.x = std::max(1 - static_cast<int>(movingImg.width()), std::min(static_cast<int>(fixedImg.width()) - 1, offset.x));
  offset.y = std::max(1 - static_cast<int>(movingImg.height()), std::min(static_cast<int>(fixedImg.height()) - 1, offset.y));
  offset.z = std::max(1 - static_cast<int>(movingImg.depth()), std::min(static_cast<int>(fixedImg.depth()) - 1, offset.z));

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
  //LINFO() << "max NCC of full scale img:" << maxNCC << "offset:" << res;
#else
  ZImg subFixedImg;
  ZImg subMovingImg;
  cropOverlapSubImg(fixedImg, movingImg, offset, subFixedImg, subMovingImg);
  size_t xEnd = subMovingImg.width() + subFixedImg.width() - 1;
  size_t yEnd = subMovingImg.height() + subFixedImg.height() - 1;
  size_t zEnd = subMovingImg.depth() + subFixedImg.depth() - 1;
  size_t xStart = std::max(0, static_cast<int>(subMovingImg.width())-1-static_cast<int>(intvX)-1);
  xEnd = std::min(xEnd, subMovingImg.width()-1+intvX+1+1);
  size_t yStart = std::max(0, static_cast<int>(subMovingImg.height())-1-static_cast<int>(intvY)-1);
  yEnd = std::min(yEnd, subMovingImg.height()-1+intvY+1+1);
  size_t zStart = std::max(0, static_cast<int>(subMovingImg.depth())-1-static_cast<int>(intvZ)-1);
  zEnd = std::min(zEnd, subMovingImg.depth()-1+intvZ+1+1);
  res = maxNormXCorrLocPart(subFixedImg, subMovingImg, xStart, xEnd, yStart, yEnd, zStart, zEnd, maxNCC, maxWeightedNCC, numOverlapVoxels);
  res += offset;
  res = mapOffsetToSrcImg(res, fixedRgn, movingRgn);
  //LINFO() << "final offset:" << res;
#endif

  return res;
}

double ZImgNCCMatch::computeNCCOfOffset(const ZVoxelCoordinate &offset)
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

void ZImgNCCMatch::constructSingleChannelFixedImg(const ZImgRegion &rgn, ZImg &fixedImg)
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
      for (std::set<size_t>::iterator it = m_fixedImgChannelsToUse.begin();
           it != m_fixedImgChannelsToUse.end(); ++it) {
        size_t ch = *it;
        if (m_fixedImgChannelsToRemoveBackground.find(ch) == m_fixedImgChannelsToRemoveBackground.end()) {
          imgs[idx] = m_fixedImg.createView(ch, m_fixedT); // virtual img
        } else {
          imgs[idx] = m_fixedImg.extractChannel(ch, m_fixedT);
          removeBackground(imgs[idx]);
        }
        idx++;
      }
      fixedImg = ZImg::combine(imgs, ZImg::CombineMode::Mean);
    }
  } else {
    if (m_fixedImgChannelsToUse.size() == 1) {
      size_t ch = *(m_fixedImgChannelsToUse.begin());
      ZImgRegion tmpRgn = rgn;
      tmpRgn.start.c = ch;
      tmpRgn.end.c = ch+1;
      tmpRgn.start.t = m_fixedT;
      tmpRgn.end.t = m_fixedT+1;
      fixedImg = m_fixedImg.crop(tmpRgn);
      if (m_fixedImgChannelsToRemoveBackground.find(ch) != m_fixedImgChannelsToRemoveBackground.end()) {
        removeBackground(fixedImg);
      }
    } else if (m_fixedImgChannelsToUse.size() > 1) {
      std::vector<ZImg> imgs(m_fixedImgChannelsToUse.size());
      size_t idx = 0;
      for (std::set<size_t>::iterator it = m_fixedImgChannelsToUse.begin();
           it != m_fixedImgChannelsToUse.end(); ++it) {
        size_t ch = *it;
        ZImgRegion tmpRgn = rgn;
        tmpRgn.start.c = ch;
        tmpRgn.end.c = ch+1;
        tmpRgn.start.t = m_fixedT;
        tmpRgn.end.t = m_fixedT+1;
        imgs[idx] = m_fixedImg.crop(tmpRgn);
        if (m_fixedImgChannelsToRemoveBackground.find(ch) != m_fixedImgChannelsToRemoveBackground.end()) {
          removeBackground(imgs[idx]);
        }
        idx++;
      }
      fixedImg = ZImg::combine(imgs, ZImg::CombineMode::Mean);
    }
  }
}

void ZImgNCCMatch::constructSingleChannelMovingImg(const ZImgRegion &rgn, ZImg &movingImg)
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
      for (std::set<size_t>::iterator it = m_movingImgChannelsToUse.begin();
           it != m_movingImgChannelsToUse.end(); ++it) {
        size_t ch = *it;
        if (m_movingImgChannelsToRemoveBackground.find(ch) == m_movingImgChannelsToRemoveBackground.end()) {
          imgs[idx] = m_movingImg.createView(ch, m_movingT); // virtual img
        } else {
          imgs[idx] = m_movingImg.extractChannel(ch, m_movingT);
          removeBackground(imgs[idx]);
        }
        idx++;
      }
      movingImg = ZImg::combine(imgs, ZImg::CombineMode::Mean);
    }
  } else {
    if (m_movingImgChannelsToUse.size() == 1) {
      size_t ch = *(m_movingImgChannelsToUse.begin());
      ZImgRegion tmpRgn = rgn;
      tmpRgn.start.c = ch;
      tmpRgn.end.c = ch+1;
      tmpRgn.start.t = m_movingT;
      tmpRgn.end.t = m_movingT+1;
      movingImg = m_movingImg.crop(tmpRgn);
      if (m_movingImgChannelsToRemoveBackground.find(ch) != m_movingImgChannelsToRemoveBackground.end()) {
        removeBackground(movingImg);
      }
    } else if (m_movingImgChannelsToUse.size() > 1) {
      std::vector<ZImg> imgs(m_movingImgChannelsToUse.size());
      size_t idx = 0;
      for (std::set<size_t>::iterator it = m_movingImgChannelsToUse.begin();
           it != m_movingImgChannelsToUse.end(); ++it) {
        size_t ch = *it;
        ZImgRegion tmpRgn = rgn;
        tmpRgn.start.c = ch;
        tmpRgn.end.c = ch+1;
        tmpRgn.start.t = m_movingT;
        tmpRgn.end.t = m_movingT+1;
        imgs[idx] = m_movingImg.crop(tmpRgn);
        if (m_movingImgChannelsToRemoveBackground.find(ch) != m_movingImgChannelsToRemoveBackground.end()) {
          removeBackground(imgs[idx]);
        }
        idx++;
      }
      movingImg = ZImg::combine(imgs, ZImg::CombineMode::Mean);
    }
  }
}

void ZImgNCCMatch::removeBackground(ZImg &img)
{
  IMG_TYPED_CALL(removeBG, img, img);
}

// reference: matlab code normxcorr2_general.m
ZVoxelCoordinate ZImgNCCMatch::maxNormXCorrLoc(ZImg &fixedImg, ZImg &movingImg, const PositionHint &hint,
                                               double *maxNCC, double *maxWeightedNCC,
                                               double *numOverlapVoxels)
{
  ZImgInfo fixedImgInfo = fixedImg.info();
  ZImgInfo movingImgInfo = movingImg.info();

  ZImg nccImg;
  ZImg numberOfOverlapVoxelsImg;
  normXCorr(fixedImg, movingImg, nccImg, numberOfOverlapVoxelsImg);

  ZImgRegion rgn = getNccImgValidRegion(hint, fixedImgInfo, movingImgInfo);
  assert(!nccImg.isTimeSeries() && !nccImg.isMultiChannelsImg());
  if (!rgn.containsWholeImg(nccImg.info())) {
    nccImg = nccImg.crop(rgn);
    numberOfOverlapVoxelsImg = numberOfOverlapVoxelsImg.crop(rgn);
  }
  size_t maxNCCIdx = getMaxWeightedNCCIdx(nccImg.channelData<double>(0), numberOfOverlapVoxelsImg.channelData<double>(0),
                                          getRequiredNumberOfOverlapPixels(fixedImgInfo, movingImgInfo), nccImg.channelVoxelNumber(),
                                          maxNCC, maxWeightedNCC, numOverlapVoxels);
  ZVoxelCoordinate maxNCCCoord = nccImg.indexToCoord(maxNCCIdx);
  ZVoxelCoordinate offset = maxNCCCoord + rgn.start - ZVoxelCoordinate(movingImgInfo.width-1, movingImgInfo.height-1, movingImgInfo.depth-1);
  LINFO() << "max NCC coord:" << maxNCCCoord;
  LINFO() << "moving image offset:" << offset;

  return offset;
}

ZVoxelCoordinate ZImgNCCMatch::maxNormXCorrLoc_S(ZImg &fixedImg, ZImg &movingImg, const PositionHint &hint,
                                                 double *maxNCC, double *maxWeightedNCC,
                                                 double *numOverlapVoxels)
{
  ZImgInfo fixedImgInfo = fixedImg.info();
  ZImgInfo movingImgInfo = movingImg.info();
  LINFO() << fixedImgInfo.toQString();
  LINFO() << movingImgInfo.toQString();

  ZImg nccImg;
  ZImg numberOfOverlapVoxelsImg;
  normXCorr(fixedImg, movingImg, nccImg, numberOfOverlapVoxelsImg);

  ZImgRegion rgn = getNccImgValidRegion(hint, fixedImgInfo, movingImgInfo);
  assert(!nccImg.isTimeSeries() && !nccImg.isMultiChannelsImg());
  if (!rgn.containsWholeImg(nccImg.info())) {
    nccImg = nccImg.crop(rgn);
    numberOfOverlapVoxelsImg = numberOfOverlapVoxelsImg.crop(rgn);
  }
  size_t maxNCCIdx = getMaxWeightedNCCIdx(nccImg.channelData<double>(0), numberOfOverlapVoxelsImg.channelData<double>(0),
                                          getRequiredNumberOfOverlapPixels(fixedImgInfo, movingImgInfo), nccImg.channelVoxelNumber(),
                                          maxNCC, maxWeightedNCC, numOverlapVoxels);
  ZVoxelCoordinate maxNCCCoord = nccImg.indexToCoord(maxNCCIdx);
  ZVoxelCoordinate offset = maxNCCCoord + rgn.start - ZVoxelCoordinate(movingImgInfo.width-1, movingImgInfo.height-1, movingImgInfo.depth-1);
  LINFO() << "max NCC coord:" << maxNCCCoord;
  LINFO() << rgn.toQString();
  LINFO() << "moving image offset:" << offset;

  return offset;
}

ZVoxelCoordinate ZImgNCCMatch::maxNormXCorrLocPart(ZImg &fixedImg, ZImg &movingImg, size_t xStart, size_t xEnd,
                                                   size_t yStart, size_t yEnd, size_t zStart, size_t zEnd,
                                                   double *maxNCC, double *maxWeightedNCC, double *numOverlapVoxels)
{
  ZImgInfo fixedImgInfo = fixedImg.info();
  ZImgInfo movingImgInfo = movingImg.info();

  ZImg nccImg;
  ZImg numberOfOverlapVoxelsImg;
  normXCorrPart(fixedImg, movingImg, xStart, xEnd, yStart, yEnd, zStart, zEnd, nccImg, numberOfOverlapVoxelsImg);

  size_t maxNCCIdx = getMaxWeightedNCCIdx(nccImg.channelData<double>(0), numberOfOverlapVoxelsImg.channelData<double>(0),
                                          getRequiredNumberOfOverlapPixels(fixedImgInfo, movingImgInfo), nccImg.channelVoxelNumber(),
                                          maxNCC, maxWeightedNCC, numOverlapVoxels);
  ZVoxelCoordinate maxNCCCoord = nccImg.indexToCoord(maxNCCIdx);
  ZVoxelCoordinate offset = maxNCCCoord + ZVoxelCoordinate(xStart, yStart, zStart)
      - ZVoxelCoordinate(movingImgInfo.width-1, movingImgInfo.height-1, movingImgInfo.depth-1);
  LINFO() << "max NCC coord:" << maxNCCCoord << "region" << xStart << xEnd << yStart << yEnd << zStart << zEnd;
  LINFO() << "moving image offset:" << offset;

  return offset;
}

ZImgRegion ZImgNCCMatch::getNccImgValidRegion(const PositionHint &hint, const ZImgInfo &fixedImgInfo, const ZImgInfo &movingImgInfo)
{
  ZImgRegion rgn;
  if (hint.testFlag(Left)) {
    rgn.end.x = fixedImgInfo.width;
  } else if (hint.testFlag(Right)) {
    rgn.start.x = movingImgInfo.width - 1;
  }
  if (hint.testFlag(Up)) {
    rgn.end.y = fixedImgInfo.height;
  } else if (hint.testFlag(Down)) {
    rgn.start.y = movingImgInfo.height - 1;
  }
  if (hint.testFlag(Front)) {
    rgn.end.z = fixedImgInfo.depth;
  } else if (hint.testFlag(Back)) {
    rgn.start.z = movingImgInfo.depth - 1;
  }
  return rgn;
}

double ZImgNCCMatch::getRequiredNumberOfOverlapPixels(const ZImgInfo &fixedImgInfo, const ZImgInfo &movingImgInfo)
{
  // ting's method, partial
  double res = std::min(std::min(fixedImgInfo.width, fixedImgInfo.height) * fixedImgInfo.depth,
                        std::min(movingImgInfo.width, movingImgInfo.height) * movingImgInfo.depth);
  return res;
}

size_t ZImgNCCMatch::getMaxWeightedNCCIdx(const double *NCCs, const double *overlapVoxels, double overlapVoxelThre, size_t dataLength,
                                          double *maxNCC, double *maxWeightedNCC, double *numOverlapVoxels)
{
  WeightNCCTing weightNCC(overlapVoxelThre);
  size_t i=0;
  size_t maxNCCIdx = i;
  if (maxNCC) *maxNCC = NCCs[i];
  if (numOverlapVoxels) *numOverlapVoxels = overlapVoxels[i];
  double maxWeightedNCCTmp = weightNCC(NCCs[i], overlapVoxels[i]);
  for (i=1; i<dataLength; ++i) {
    double weightedNCC = weightNCC(NCCs[i], overlapVoxels[i]);
    if (weightedNCC > maxWeightedNCCTmp) {
      maxWeightedNCCTmp = weightedNCC;
      maxNCCIdx = i;
      if (maxNCC) *maxNCC = NCCs[i];
      if (numOverlapVoxels) *numOverlapVoxels = overlapVoxels[i];
    }
  }
  LINFO() << "max NCC:" << NCCs[maxNCCIdx] << "max weighted NCC:" << maxWeightedNCCTmp << "number of overlap voxels:" << overlapVoxels[maxNCCIdx];
  if (maxWeightedNCC) *maxWeightedNCC = maxWeightedNCCTmp;
  return maxNCCIdx;
}

std::pair<ZImgRegion, ZImgRegion> ZImgNCCMatch::getRequiredSrcImgRegion(const PositionHint& hint, const ZImg &fixedImg, const ZImg &movingImg,
                                                                        double overlapRate)
{
  ZImgRegion fixedRgn;
  ZImgRegion movingRgn;

  if (hint.testFlag(Left)) {
    size_t maxNumOverlapPixelsX = std::ceil(std::min(fixedImg.width(), movingImg.width()) * overlapRate);
    fixedRgn.end.x = std::min(maxNumOverlapPixelsX, fixedImg.width());
    assert(movingImg.width() >= maxNumOverlapPixelsX);
    movingRgn.start.x = movingImg.width() - maxNumOverlapPixelsX;
  } else if (hint.testFlag(Right)) {
    size_t maxNumOverlapPixelsX = std::ceil(std::min(fixedImg.width(), movingImg.width()) * overlapRate);
    movingRgn.end.x = std::min(maxNumOverlapPixelsX, movingImg.width());
    assert(fixedImg.width() >= maxNumOverlapPixelsX);
    fixedRgn.start.x = fixedImg.width() - maxNumOverlapPixelsX;
  }
  if (hint.testFlag(Up)) {
    size_t maxNumOverlapPixelsY = std::ceil(std::min(fixedImg.height(), movingImg.height()) * overlapRate);
    fixedRgn.end.y = std::min(maxNumOverlapPixelsY, fixedImg.height());
    assert(movingImg.height() >= maxNumOverlapPixelsY);
    movingRgn.start.y = movingImg.height() - maxNumOverlapPixelsY;
  } else if (hint.testFlag(Down)) {
    size_t maxNumOverlapPixelsY = std::ceil(std::min(fixedImg.height(), movingImg.height()) * overlapRate);
    movingRgn.end.y = std::min(maxNumOverlapPixelsY, movingImg.height());
    assert(fixedImg.height() >= maxNumOverlapPixelsY);
    fixedRgn.start.y = fixedImg.height() - maxNumOverlapPixelsY;
  }
  if (hint.testFlag(Front)) {
    size_t maxNumOverlapPixelsZ = std::ceil(std::min(fixedImg.depth(), movingImg.depth()) * overlapRate);
    fixedRgn.end.z = std::min(maxNumOverlapPixelsZ, fixedImg.depth());
    assert(movingImg.depth() >= maxNumOverlapPixelsZ);
    movingRgn.start.z = movingImg.depth() - maxNumOverlapPixelsZ;
  } else if (hint.testFlag(Back)) {
    size_t maxNumOverlapPixelsZ = std::ceil(std::min(fixedImg.depth(), movingImg.depth()) * overlapRate);
    movingRgn.end.z = std::min(maxNumOverlapPixelsZ, movingImg.depth());
    assert(fixedImg.depth() >= maxNumOverlapPixelsZ);
    fixedRgn.start.z = fixedImg.depth() - maxNumOverlapPixelsZ;
  }

  return std::make_pair(fixedRgn, movingRgn);
}

ZVoxelCoordinate ZImgNCCMatch::mapOffsetToSrcImg(ZVoxelCoordinate offset,
                                                 const ZImgRegion &fixedRgn, const ZImgRegion &movingRgn)
{
  return offset - movingRgn.start + fixedRgn.start;
}

} // namespace nim
