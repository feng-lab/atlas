#pragma once

#include "zvoxelcoordinate.h"
#include <QString>
#include <map>

namespace nim {

class ZImg;

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

  // don't add empty img
  // img1 has absolute location, if img already exist, update its location
  void addImg(const ZImg& img, const ZVoxelCoordinate& loc, const QString& imgName = "");

  // img2 has relative location to img1, if pair already exist, update its offset and cost
  void addImgPair(const ZImg& img1, const ZImg& img2, const ZVoxelCoordinate& img2Offset, double connectionCost = 0.,
                  const QString& img1Name = "", const QString& img2Name = "");

  // remove img
  void removeImg(const ZImg& img);

  // remove connection between imgs, keep img
  void removeImgConnection(const ZImg& img1, const ZImg& img2);

  // throw ZImgException if error
  ZImg merge(Mode mode = Mode::Max, QString* summary = nullptr) const;

protected:
  void resolveLocations(std::map<const ZImg*, ZVoxelCoordinate>& imgs,
                        const ZImg* refImg, double minCost, QString& summary) const;

  void mergeImgs(ZImg& res, const std::map<const ZImg*, ZVoxelCoordinate>& imgs,
                 Mode mode, QString& summary) const;

private:
  std::map<const ZImg*, ZVoxelCoordinate> m_imgCoords;
  std::map<const ZImg*, QString> m_imgNames;
  std::map<std::pair<const ZImg*, const ZImg*>, std::pair<ZVoxelCoordinate, double>> m_imgPairs;
};

} // namespace nim

