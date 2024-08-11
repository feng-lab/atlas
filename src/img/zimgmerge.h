#pragma once

#include "zvoxelcoordinate.h"
#include "zimg.h"
#include "zimgblockprovider.h"
#include "zvoxelregion.h"
#include "zimgtile.h"
#include <map>

namespace nim {

// use provided absolute locations or relative locations to merge multiple imgs
// if one img has multiple relative locations, use minimum spanning tree to find the optimal one
//
class ZImgMerge : public ZImgBlockProvider
{
public:
  ~ZImgMerge() override = default;

  // img has absolute location, if img already exist, update its location
  void addImg(const ZImgSubBlock& img, const ZVoxelCoordinate& loc, const QString& imgName = "");

  // img2 has relative location to img1, if pair already exist, update its offset and cost
  void addImgPair(const ZImgSubBlock& img1,
                  const ZImgSubBlock& img2,
                  const ZVoxelCoordinate& img2Offset,
                  double connectionCost = 0.,
                  const QString& img1Name = "",
                  const QString& img2Name = "");

  // remove img
  void removeImg(const ZImgSubBlock& img);

  // remove connection between imgs, keep img
  void removeImgPair(const ZImgSubBlock& img1, const ZImgSubBlock& img2);

  // return summary info
  // throw ZException if error
  std::vector<std::string> resolveLocations();

  void setMergeMode(ImgMergeMode mode = ImgMergeMode::Max)
  {
    m_mergeMode = mode;
  }

  [[nodiscard]] ZImgInfo imgInfo() const override
  {
    return m_imgInfo;
  }

  //  ZImg slice(size_t z, size_t t, size_t ratio) const override;
  //
  //  ZImg allSlices(size_t t, size_t ratio) const override;
  //
  //  ZImg wholeImg(size_t ratio) const override;

  [[nodiscard]] size_t numBlocks() const override;

  [[nodiscard]] ZImg block(size_t blockIdx) const override;

  [[nodiscard]] ZVoxelCoordinate blockCoord(size_t blockIdx) const override;

  [[nodiscard]] ZImg wholeImg() const override;

  // resolveLocations() needs to be called before this
  void save(const QString& fileName,
            FileFormat format = FileFormat::Unknown,
            const ZImgWriteParameters& paras = ZImgWriteParameters());

protected:
  void resolveLocations_impl(std::map<const ZImgSubBlock*, ZVoxelCoordinate>& imgs,
                             const ZImgSubBlock& refImg,
                             double minCost,
                             std::vector<std::string>& summary) const;

  void mergeImgs(ZImg& res,
                 const std::map<const ZImgSubBlock*, ZVoxelCoordinate>& imgs,
                 ImgMergeMode mode,
                 std::string& summary) const;

private:
  std::map<const ZImgSubBlock*, ZVoxelCoordinate> m_imgCoords;
  std::map<std::pair<const ZImgSubBlock*, const ZImgSubBlock*>, std::pair<ZVoxelCoordinate, double>> m_imgPairs;
  std::map<const ZImgSubBlock*, QString> m_imgNames;
  std::map<const ZImgSubBlock*, ZImgInfo> m_imgInfos;
  ImgMergeMode m_mergeMode = ImgMergeMode::Max;

  std::map<const ZImgSubBlock*, ZVoxelCoordinate> m_imgFinalCoords;
  std::vector<ZImgTile> m_tiles;
  ZVoxelRegion m_overlapRegion;
  ZVoxelCoordinate m_minCoord;
  ZImgInfo m_imgInfo;
};

} // namespace nim
