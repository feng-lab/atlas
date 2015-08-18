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

}

namespace nim {

ZImgNCCMatch::ZImgNCCMatch(const ZImg& fixedImg, const ZImg& movingImg,
                           size_t fixedT, size_t movingT)
  : m_fixedImg(fixedImg), m_movingImg(movingImg)
  , m_fixedT(fixedT), m_movingT(movingT)
  , m_movingImgPosHint(None)
{
  init();
}

void ZImgNCCMatch::setMovingImgPositionHint(PositionHint hint)
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

QString ZImgNCCMatch::positionHintToQString(PositionHint hint)
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
  if (hintList.empty()) {
    return "None";
  } else {
    return hintList.join(" | ");
  }
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

ZVoxelCoordinate ZImgNCCMatch::computeMovingImgOffset(double *maxNCC, double *maxWeightedNCC, double *numOverlapVoxels)
{
  if (m_fixedImg.isEmpty() || m_movingImg.isEmpty()) {
    throw ZImgException("computeMovingImgOffset: Can not match empty imgs");
  }
  ZVoxelCoordinate res;

  ZImg fixedImg;
  ZImg movingImg;

  constructSingleChannelFixedImg(fixedImg);
  constructSingleChannelMovingImg(movingImg);

  res = maxNormXCorrLoc_S(fixedImg, movingImg, maxNCC, maxWeightedNCC, numOverlapVoxels);

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

  ZImg fixedImg;
  ZImg movingImg;

  constructSingleChannelFixedImg(fixedImg);
  constructSingleChannelMovingImg(movingImg);

  intvX = std::min(intvX, std::min(fixedImg.width()-1, movingImg.width()-1));
  intvY = std::min(intvY, std::min(fixedImg.height()-1, movingImg.height()-1));
  intvZ = std::min(intvZ, std::min(fixedImg.depth()-1, movingImg.depth()-1));
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

  ZVoxelCoordinate offset = maxNormXCorrLoc_S(dsFixedImg, dsMovingImg, lowResMaxNCC, lowResMaxWeightedNCC, lowResNumOverlapVoxels);

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
  LINFO() << "max NCC of full scale img:" << maxNCC << "offset:" << res;
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
  LINFO() << "final offset:" << res;
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

  constructSingleChannelFixedImg(fixedImg);
  constructSingleChannelMovingImg(movingImg);

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

void ZImgNCCMatch::constructSingleChannelFixedImg(ZImg &fixedImg)
{
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
}

void ZImgNCCMatch::constructSingleChannelMovingImg(ZImg &movingImg)
{
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
}

void ZImgNCCMatch::removeBackground(ZImg &img)
{
  IMG_TYPED_CALL(removeBG, img, img);
}

// reference: matlab code normxcorr2_general.m
ZVoxelCoordinate ZImgNCCMatch::maxNormXCorrLoc(ZImg &fixedImg, ZImg &movingImg,
                                               double *maxNCC, double *maxWeightedNCC,
                                               double *numOverlapVoxels) const
{
  size_t movingImgWidth = movingImg.width();
  size_t movingImgHeight = movingImg.height();
  size_t movingImgDepth = movingImg.depth();

  size_t fixedImgWidth = fixedImg.width();
  size_t fixedImgHeight = fixedImg.height();
  size_t fixedImgDepth = fixedImg.depth();

  ZImg nccImg;
  ZImg numberOfOverlapVoxelsImg;
  normXCorr(fixedImg, movingImg, nccImg, numberOfOverlapVoxelsImg);

  ZImgRegion rgn;
  if (m_movingImgPosHint.testFlag(Left)) {
    rgn.end.x = fixedImgWidth / 2 + fixedImgWidth % 2;
  } else if (m_movingImgPosHint.testFlag(Right)) {
    rgn.start.x = fixedImgWidth / 2 + movingImgWidth - 1;
  }
  if (m_movingImgPosHint.testFlag(Up)) {
    rgn.end.y = fixedImgHeight / 2 + fixedImgHeight % 2;
  } else if (m_movingImgPosHint.testFlag(Down)) {
    rgn.start.y = fixedImgHeight / 2 + movingImgHeight - 1;
  }
  if (m_movingImgPosHint.testFlag(Front)) {
    rgn.end.z = fixedImgDepth / 2 + fixedImgDepth % 2;
  } else if (m_movingImgPosHint.testFlag(Back)) {
    rgn.start.z = fixedImgDepth / 2 + movingImgDepth - 1;
  }
  if (m_movingImgPosHint != None) {
    nccImg = nccImg.crop(rgn);
    numberOfOverlapVoxelsImg = numberOfOverlapVoxelsImg.crop(rgn);
  }
  size_t maxNCCIdx = getMaxWeightedNCCIdx(nccImg.channelData<double>(0), numberOfOverlapVoxelsImg.channelData<double>(0),
                                          getRequiredNumberOfOverlapPixels(), nccImg.channelVoxelNumber(),
                                          maxNCC, maxWeightedNCC, numOverlapVoxels);
  ZVoxelCoordinate maxNCCCoord = nccImg.indexToCoord(maxNCCIdx);
  ZVoxelCoordinate offset = maxNCCCoord + rgn.start - ZVoxelCoordinate(movingImgWidth-1, movingImgHeight-1, movingImgDepth-1);
  LINFO() << "max NCC coord:" << maxNCCCoord;
  LINFO() << "moving image offset:" << offset;

  return offset;
}

ZVoxelCoordinate ZImgNCCMatch::maxNormXCorrLoc_S(ZImg &fixedImg, ZImg &movingImg,
                                                 double *maxNCC, double *maxWeightedNCC, double *numOverlapVoxels) const
{
  size_t movingImgWidth = movingImg.width();
  size_t movingImgHeight = movingImg.height();
  size_t movingImgDepth = movingImg.depth();

  size_t fixedImgWidth = fixedImg.width();
  size_t fixedImgHeight = fixedImg.height();
  size_t fixedImgDepth = fixedImg.depth();

  ZImg nccImg;
  ZImg numberOfOverlapVoxelsImg;
  normXCorr(fixedImg, movingImg, nccImg, numberOfOverlapVoxelsImg);

  ZImgRegion rgn;
  if (m_movingImgPosHint.testFlag(Left)) {
    rgn.end.x = fixedImgWidth / 2 + fixedImgWidth % 2;
  } else if (m_movingImgPosHint.testFlag(Right)) {
    rgn.start.x = fixedImgWidth / 2 + movingImgWidth - 1;
  }
  if (m_movingImgPosHint.testFlag(Up)) {
    rgn.end.y = fixedImgHeight / 2 + fixedImgHeight % 2;
  } else if (m_movingImgPosHint.testFlag(Down)) {
    rgn.start.y = fixedImgHeight / 2 + movingImgHeight - 1;
  }
  if (m_movingImgPosHint.testFlag(Front)) {
    rgn.end.z = fixedImgDepth / 2 + fixedImgDepth % 2;
  } else if (m_movingImgPosHint.testFlag(Back)) {
    rgn.start.z = fixedImgDepth / 2 + movingImgDepth - 1;
  }
  if (m_movingImgPosHint != None) {
    nccImg = nccImg.crop(rgn);
    numberOfOverlapVoxelsImg = numberOfOverlapVoxelsImg.crop(rgn);
  }
  size_t maxNCCIdx = getMaxWeightedNCCIdx(nccImg.channelData<double>(0), numberOfOverlapVoxelsImg.channelData<double>(0),
                                          getRequiredNumberOfOverlapPixels(), nccImg.channelVoxelNumber(),
                                          maxNCC, maxWeightedNCC, numOverlapVoxels);
  ZVoxelCoordinate maxNCCCoord = nccImg.indexToCoord(maxNCCIdx);
  ZVoxelCoordinate offset = maxNCCCoord + rgn.start - ZVoxelCoordinate(movingImgWidth-1, movingImgHeight-1, movingImgDepth-1);
  LINFO() << "max NCC coord:" << maxNCCCoord;
  LINFO() << "moving image offset:" << offset;

  return offset;
}

ZVoxelCoordinate ZImgNCCMatch::maxNormXCorrLocPart(ZImg &fixedImg, ZImg &movingImg, size_t xStart, size_t xEnd,
                                                   size_t yStart, size_t yEnd, size_t zStart, size_t zEnd,
                                                   double *maxNCC, double *maxWeightedNCC, double *numOverlapVoxels) const
{
  size_t movingImgWidth = movingImg.width();
  size_t movingImgHeight = movingImg.height();
  size_t movingImgDepth = movingImg.depth();

  ZImg nccImg;
  ZImg numberOfOverlapVoxelsImg;
  normXCorrPart(fixedImg, movingImg, xStart, xEnd, yStart, yEnd, zStart, zEnd, nccImg, numberOfOverlapVoxelsImg);


  size_t maxNCCIdx = getMaxWeightedNCCIdx(nccImg.channelData<double>(0), numberOfOverlapVoxelsImg.channelData<double>(0),
                                          getRequiredNumberOfOverlapPixels(), nccImg.channelVoxelNumber(),
                                          maxNCC, maxWeightedNCC, numOverlapVoxels);
  ZVoxelCoordinate maxNCCCoord = nccImg.indexToCoord(maxNCCIdx);
  ZVoxelCoordinate offset = maxNCCCoord + ZVoxelCoordinate(xStart, yStart, zStart)
      - ZVoxelCoordinate(movingImgWidth-1, movingImgHeight-1, movingImgDepth-1);
  LINFO() << "max NCC coord:" << maxNCCCoord << "region" << xStart << xEnd << yStart << yEnd << zStart << zEnd;
  LINFO() << "moving image offset:" << offset;

  return offset;
}

double ZImgNCCMatch::getRequiredNumberOfOverlapPixels() const
{
  // ting's method, partial
  double res = std::min(std::min(m_fixedImg.width(), m_fixedImg.height()) * m_fixedImg.depth(),
                        std::min(m_movingImg.width(), m_movingImg.height()) * m_movingImg.depth());
  return res;
}

namespace {

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

size_t ZImgNCCMatch::getMaxWeightedNCCIdx(const double *NCCs, const double *overlapVoxels, double overlapVoxelThre, size_t dataLength,
                                          double *maxNCC, double *maxWeightedNCC, double *numOverlapVoxels) const
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

} // namespace nim
