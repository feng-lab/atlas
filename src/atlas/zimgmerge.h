#pragma once

#include "zvoxelcoordinate.h"
#include "zimg.h"
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

  virtual std::shared_ptr<ZImg> read() const override;
  virtual ZImgInfo readInfo() const override;

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
class ZImgMerge
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
  void removeImgConnection(const ZImgSubBlock& img1, const ZImgSubBlock& img2);

  // throw ZImgException if error
  ZImg merge(Mode mode = Mode::Max, QString* summary = nullptr) const;

protected:
  void resolveLocations(std::map<const ZImgSubBlock*, ZVoxelCoordinate>& imgs,
                        const ZImgSubBlock* refImg, double minCost, QString& summary) const;

  void mergeImgs(ZImg& res, const std::map<const ZImgSubBlock*, ZVoxelCoordinate>& imgs,
                 Mode mode, QString& summary) const;

private:
  std::map<const ZImgSubBlock*, ZVoxelCoordinate> m_imgCoords;
  std::map<std::pair<const ZImgSubBlock*, const ZImgSubBlock*>, std::pair<ZVoxelCoordinate, double>> m_imgPairs;
  std::map<const ZImgSubBlock*, QString> m_imgNames;
  std::map<const ZImgSubBlock*, ZImgInfo> m_imgInfos;
};

} // namespace nim

