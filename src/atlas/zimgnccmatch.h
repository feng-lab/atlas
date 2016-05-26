#ifndef ZIMGNCCMATCH_H
#define ZIMGNCCMATCH_H

#include "zimg.h"
#include "zvoxelcoordinate.h"
#include <set>
#include <QFlags>

namespace nim {

// use normalized cross-correlation to match two img
// for stitching
// if any img is empty, return zero offset
class ZImgNCCMatch
{
public:
  // Conflicting combinations of flags have undefined meanings.
  enum ___PositionHint
  {
    None = 0x00, // no hint, any position can be possible
    Left = 0x01,  // img is in left side, only overlap with left part of another img
    Right = 0x02, // img is in right side, only overlap with right part of another img
    Up = 0x04,
    Down = 0x08,
    Front = 0x10,
    Back = 0x20
  };
  Q_DECLARE_FLAGS(PositionHint, ___PositionHint)

  ZImgNCCMatch(const ZImg& fixedImg, const ZImg& movingImg, size_t fixedT = 0, size_t movingT = 0);

  void setMovingImgPositionHint(PositionHint hint, double overlapRate = 1.0);
  QString positionHintToQString() const;
  static void reversePositionHint(PositionHint& hint);
  static QString positionHintToQString(PositionHint hint, double overlapRate);

  // NCC works for single channel, if img contains many channels, these methods control
  // which channels to use. Default use average of all channels
  // don't need to call this for single channel img
  // use one channel
  void useFixedImgChannel(size_t ch);
  void useMovingImgChannel(size_t ch);
  // use two channels
  void useFixedImgChannel(size_t ch1, size_t ch2);
  void useMovingImgChannel(size_t ch1, size_t ch2);
  // use three channels
  void useFixedImgChannel(size_t ch1, size_t ch2, size_t ch3);
  void useMovingImgChannel(size_t ch1, size_t ch2, size_t ch3);
  // use many channels
  void useFixedImgChannel(const std::vector<size_t>& chs);
  void useMovingImgChannel(const std::vector<size_t>& chs);
  // use all channels, this is default
  void useAllFixedImgChannels();
  void useAllMovingImgChannels();

  // optional preprocess
  // remove background for channel ch
  void enableRemoveBackgroundForFixedImgChannel(size_t ch);
  void enableRemoveBackgroundForMovingImgChannel(size_t ch);
  // don't remove background for channel ch
  void disableRemoveBackgroundForFixedImgChannel(size_t ch);
  void disableRemoveBackgroundForMovingImgChannel(size_t ch);
  //
  void enableRemoveBackgroundForAllFixedImgChannels();
  void enableRemoveBackgroundForAllMovingImgChannels();
  //  this is default behavior
  void disableRemoveBackgroundForAllFixedImgChannels();
  void disableRemoveBackgroundForAllMovingImgChannels();

  // do correlation
  ZVoxelCoordinate computeMovingImgOffset(double *maxNCC = nullptr,
                                          double *maxWeightedNCC = nullptr,
                                          double *numOverlapVoxels = nullptr);

  // use coarse-to-fine method to reduce memory usage
  ZVoxelCoordinate computeMovingImgOffsetMR(size_t intvX, size_t intvY, size_t intvZ,
                                            double *maxNCC = nullptr,
                                            double *maxWeightedNCC = nullptr,
                                            double *numOverlapVoxels = nullptr,
                                            double *lowResMaxNCC = nullptr,
                                            double *lowResMaxWeightedNCC = nullptr,
                                            double *lowResNumOverlapVoxels = nullptr);

  // give a offset of moving img, get ncc of this offset
  double computeNCCOfOffset(const ZVoxelCoordinate& offset);

private:
  void init();
  // throw if ch don't exist
  void checkFixedImgChannel(size_t ch);
  void checkMovingImgChannel(size_t ch);

  void constructSingleChannelFixedImg(const ZImgRegion& rgn, ZImg &fixedImg);
  void constructSingleChannelMovingImg(const ZImgRegion& rgn, ZImg &movingImg);
  // only works for single channel img
  void removeBackground(ZImg& img);

  // will release input, need 4 double padded extra space
  // for two 200M 8bit imgs, this will need 4*200*8*8M = 51200M = 51.2G memory
  static ZVoxelCoordinate maxNormXCorrLoc(ZImg& fixedImg, ZImg& movingImg, const PositionHint& hint,
                                          double *maxNCC = nullptr,
                                          double *maxWeightedNCC = nullptr,
                                          double *numOverlapVoxels = nullptr);

  // slower but use less memory, need 3 double padded extra space
  // for two 200M 8bit imgs, this will need 3*200*8*8M = 38400M = 38.4G memory
  static ZVoxelCoordinate maxNormXCorrLoc_S(ZImg& fixedImg, ZImg& movingImg, const PositionHint& hint,
                                            double *maxNCC = nullptr,
                                            double *maxWeightedNCC = nullptr,
                                            double *numOverlapVoxels = nullptr);

  static ZVoxelCoordinate maxNormXCorrLocPart(ZImg& fixedImg, ZImg& movingImg, size_t xStart, size_t xEnd,
                                              size_t yStart, size_t yEnd, size_t zStart, size_t zEnd,
                                              double *maxNCC = nullptr,
                                              double *maxWeightedNCC = nullptr,
                                              double *numOverlapVoxels = nullptr);

  static ZImgRegion getNccImgValidRegion(const PositionHint& hint, const ZImgInfo &fixedImgInfo, const ZImgInfo &movingImgInfo);

  // ting's method, partial
  static double getRequiredNumberOfOverlapPixels(const ZImgInfo &fixedImgInfo, const ZImgInfo &movingImgInfo);

  // ting's method
  static size_t getMaxWeightedNCCIdx(const double* NCCs, const double* overlapVoxels, double overlapVoxelThre, size_t dataLength,
                                     double *maxNCC = nullptr, double *maxWeightedNCC = nullptr, double *numOverlapVoxels = nullptr);

  static std::pair<ZImgRegion, ZImgRegion> getRequiredSrcImgRegion(const PositionHint& hint,
                                                                   const ZImg &fixedImg, const ZImg &movingImg, double overlapRate);
  static ZVoxelCoordinate mapOffsetToSrcImg(ZVoxelCoordinate offset, const ZImgRegion &fixedRgn, const ZImgRegion &movingRgn);

private:
  const ZImg& m_fixedImg;
  const ZImg& m_movingImg;
  size_t m_fixedT;
  size_t m_movingT;
  PositionHint m_movingImgPosHint;
  double m_overlapRate;  // 0 to 1

  std::set<size_t> m_fixedImgChannelsToUse;
  std::set<size_t> m_movingImgChannelsToUse;
  std::set<size_t> m_fixedImgChannelsToRemoveBackground;
  std::set<size_t> m_movingImgChannelsToRemoveBackground;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(ZImgNCCMatch::PositionHint)

} // namespace nim

#endif // ZIMGNCCMATCH_H
