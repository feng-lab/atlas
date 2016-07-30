#include "zimgregion.h"
//#include <sstream>

namespace nim {

bool ZImgRegion::containsCoord(const nim::ZVoxelCoordinate& coord, const nim::ZImgInfo& info) const
{
  ZImgRegion rgn = *this;
  rgn.resolveRegionEnd(info);
  return coord.allGreaterEqual(rgn.start) && coord.allLessThan(rgn.end);
}

void ZImgRegion::fitInto(const ZImgInfo& info)
{
  ZVoxelCoordinate endCoord(info.width, info.height, info.depth,
                            info.numChannels, info.numTimes);
  start = min(endCoord, max(0, start));
  end = min(endCoord, end);
  if (end.x < 0) end.x = endCoord.x;
  if (end.y < 0) end.y = endCoord.y;
  if (end.z < 0) end.z = endCoord.z;
  if (end.c < 0) end.c = endCoord.c;
  if (end.t < 0) end.t = endCoord.t;
  //if (end.l < 0) end.l = endCoord.l;
}

void ZImgRegion::resolveRegionEnd(const ZImgInfo& info)
{
  if (end.x < 0) end.x = info.width;
  if (end.y < 0) end.y = info.height;
  if (end.z < 0) end.z = info.depth;
  if (end.c < 0) end.c = info.numChannels;
  if (end.t < 0) end.t = info.numTimes;
  //if (end.l < 0) end.l = info.numLocations;
}

//std::string ZImgRegion::toString() const
//{
//  std::ostringstream res;
//  res << "x:[" << xStart << ", " << xEnd << ")"
//      << ", y:[" << yStart << ", " << yEnd << ")"
//      << ", z:[" << zStart << ", " << zEnd << ")"
//      << ", c:[" << cStart << ", " << cEnd << ")"
//      << ", t:[" << tStart << ", " << tEnd << ")"
//      << ", l:[" << lStart << ", " << lEnd << ")";

//  return res.str();
//}

QString ZImgRegion::toQString() const
{
  return QString("x: [%1, %2)").arg(start.x).arg(end.x) %
         QString(", y: [%1, %2)").arg(start.y).arg(end.y) %
         QString(", z: [%1, %2)").arg(start.z).arg(end.z) %
         QString(", c: [%1, %2)").arg(start.c).arg(end.c) %
         QString(", t: [%1, %2)").arg(start.t).arg(end.t);
}

ZImgInfo ZImgRegion::clip(const ZImgInfo& info) const
{
  ZImgInfo res = info;
  //  value_type rlEnd = end.l == -1 ? info.numLocations : end.l;
  //  res.numLocations = rlEnd - start.l;
  //  res.locations = std::vector<Location>(info.locations.begin() + start.l,
  //                                        info.locations.begin() + rlEnd);
  value_type rtEnd = end.t == -1 ? info.numTimes : end.t;
  res.numTimes = rtEnd - start.t;
  res.timeStamps = std::vector<double>(info.timeStamps.begin() + start.t,
                                       info.timeStamps.begin() + rtEnd);
  value_type rcEnd = end.c == -1 ? info.numChannels : end.c;
  res.numChannels = rcEnd - start.c;
  res.channelColors = std::vector<col4>(info.channelColors.begin() + start.c,
                                        info.channelColors.begin() + rcEnd);
  res.channelNames = std::vector<QString>(info.channelNames.begin() + start.c,
                                          info.channelNames.begin() + rcEnd);
  res.depth = end.z >= 0 ? (end.z - start.z) : (info.depth - start.z);
  res.height = end.y >= 0 ? (end.y - start.y) : (info.height - start.y);
  res.width = end.x >= 0 ? (end.x - start.x) : (info.width - start.x);
  res.validBitCount = info.validBitCount;

  return res;
}

}  // namespace nim
