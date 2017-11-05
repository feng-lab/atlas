#pragma once

#include "zvoxelcoordinate.h"
#include "zimg.h"
#include "zimgblockprovider.h"
#include "zvoxelregion.h"
#include "zimgtile.h"
#include <QString>
#include <map>

namespace nim {

class ZImgTileSubBlock : public ZImgSubBlock
{
public:
  ZImgTileSubBlock(const ZImgSource& source,
                   size_t downsampleBlockWidth = 1,
                   size_t downsampleBlockHeight = 1,
                   size_t downsampleBlockDepth = 1,
                   ZImg::CombineMode downsampleCombineMode = ZImg::CombineMode::Mean);

  std::shared_ptr<ZImg> read() const override;
  ZImgInfo readInfo() const override;

private:
  const ZImgSource& m_source;
  size_t m_downsampleBlockWidth;
  size_t m_downsampleBlockHeight;
  size_t m_downsampleBlockDepth;
  ZImg::CombineMode m_downsampleCombineMode;
};

// use provided absolute locations or relative locations to merge multiple imgs
// if one img has multiple relative locations, use minimum spanning tree to find the optimal one
//
class ZImgMerge : public ZImgBlockProvider
{
public:
  enum class Mode
  {
    Max, Min, Mean, Median, First
  };

  // img has absolute location, if img already exist, update its location
  void addImg(const ZImgSubBlock& img, const ZVoxelCoordinate& loc, const QString& imgName = "");

  // img2 has relative location to img1, if pair already exist, update its offset and cost
  void addImgPair(const ZImgSubBlock& img1, const ZImgSubBlock& img2, const ZVoxelCoordinate& img2Offset,
                  double connectionCost = 0.,
                  const QString& img1Name = "", const QString& img2Name = "");

  // remove img
  void removeImg(const ZImgSubBlock& img);

  // remove connection between imgs, keep img
  void removeImgPair(const ZImgSubBlock& img1, const ZImgSubBlock& img2);

  // return summary info
  // throw ZImgException if error
  QStringList resolveLocations();

  void setMergeMode(Mode mode = Mode::Max)
  { m_mergeMode = mode; }

  const ZImgInfo& imgInfo() const override
  { return m_imgInfo; }

//  ZImg slice(size_t z, size_t t, size_t ratio) const override;
//
//  ZImg allSlices(size_t t, size_t ratio) const override;
//
//  ZImg wholeImg(size_t ratio) const override;

  size_t numBlocks() const override;

  ZImg block(size_t blockIdx) const override;

  ZVoxelCoordinate blockCoord(size_t blockIdx) const override;

  ZImg wholeImg() const override;

protected:
  void resolveLocations(std::map<const ZImgSubBlock*, ZVoxelCoordinate>& imgs,
                        const ZImgSubBlock* refImg, double minCost, QStringList& summary) const;

  void mergeImgs(ZImg& res, const std::map<const ZImgSubBlock*, ZVoxelCoordinate>& imgs,
                 Mode mode, QString& summary) const;

private:
  std::map<const ZImgSubBlock*, ZVoxelCoordinate> m_imgCoords;
  std::map<std::pair<const ZImgSubBlock*, const ZImgSubBlock*>, std::pair<ZVoxelCoordinate, double>> m_imgPairs;
  std::map<const ZImgSubBlock*, QString> m_imgNames;
  std::map<const ZImgSubBlock*, ZImgInfo> m_imgInfos;
  Mode m_mergeMode = Mode::Max;

  std::map<const ZImgSubBlock*, ZVoxelCoordinate> m_imgFinalCoords;
  std::vector<ZImgTile> m_tiles;
  ZVoxelRegion m_overlapRegion;
  ZVoxelCoordinate m_minCoord;
  ZImgInfo m_imgInfo;
};

} // namespace nim

