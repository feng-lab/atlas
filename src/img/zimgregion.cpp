#include "zimgregion.h"

#include "zlog.h"

namespace nim {

bool ZImgRegion::containsCoord(const ZVoxelCoordinate& coord, const ZImgInfo& info) const
{
  ZImgRegion rgn = *this;
  rgn.resolveRegionEnd(info);
  return coord.allGreaterEqual(rgn.start) && coord.allLessThan(rgn.end);
}

void ZImgRegion::fitInto(const ZImgInfo& info)
{
  ZVoxelCoordinate endCoord(info.width, info.height, info.depth, info.numChannels, info.numTimes);
  start = min(endCoord, max(0, start));
  end = min(endCoord, end);
  if (end.x < 0) {
    end.x = endCoord.x;
  }
  if (end.y < 0) {
    end.y = endCoord.y;
  }
  if (end.z < 0) {
    end.z = endCoord.z;
  }
  if (end.c < 0) {
    end.c = endCoord.c;
  }
  if (end.t < 0) {
    end.t = endCoord.t;
  }
  // if (end.l < 0) end.l = endCoord.l;
}

void ZImgRegion::resolveRegionEnd(const ZImgInfo& info)
{
  if (end.x < 0) {
    end.x = info.width;
  }
  if (end.y < 0) {
    end.y = info.height;
  }
  if (end.z < 0) {
    end.z = info.depth;
  }
  if (end.c < 0) {
    end.c = info.numChannels;
  }
  if (end.t < 0) {
    end.t = info.numTimes;
  }
  // if (end.l < 0) end.l = info.numLocations;
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
  res.timeStamps = std::vector<double>(info.timeStamps.begin() + start.t, info.timeStamps.begin() + rtEnd);
  value_type rcEnd = end.c == -1 ? info.numChannels : end.c;
  res.numChannels = rcEnd - start.c;
  res.channelColors = std::vector<col4>(info.channelColors.begin() + start.c, info.channelColors.begin() + rcEnd);
  res.channelNames = std::vector<QString>(info.channelNames.begin() + start.c, info.channelNames.begin() + rcEnd);
  res.depth = end.z >= 0 ? (end.z - start.z) : (info.depth - start.z);
  res.height = end.y >= 0 ? (end.y - start.y) : (info.height - start.y);
  res.width = end.x >= 0 ? (end.x - start.x) : (info.width - start.x);
  res.validBitCount = info.validBitCount;

  return res;
}

std::vector<ZImgRegion> ZImgRegion::splitBigImage(const ZImgInfo& info,
                                                  std::vector<ZImgRegion>& nonExpandRegions,
                                                  size_t tileSize,
                                                  size_t expand,
                                                  index_t ch,
                                                  index_t t)
{
  CHECK(expand < tileSize);
  std::vector<ZImgRegion> res;
  nonExpandRegions.clear();
  index_t chs = ch < 0 ? 0 : ch;
  index_t che = ch < 0 ? -1 : ch + 1;
  index_t ts = t < 0 ? 0 : t;
  index_t te = t < 0 ? -1 : t + 1;
  for (size_t y = 0; y < info.height; y += tileSize) {
    for (size_t x = 0; x < info.width; x += tileSize) {
      nonExpandRegions.emplace_back(x,
                                    std::min(x + tileSize, info.width),
                                    y,
                                    std::min(y + tileSize, info.height),
                                    0,
                                    -1,
                                    chs,
                                    che,
                                    ts,
                                    te);
      res.emplace_back(std::max(expand, x) - expand,
                       std::min(x + tileSize + expand, info.width),
                       std::max(expand, y) - expand,
                       std::min(y + tileSize + expand, info.height),
                       0,
                       -1,
                       chs,
                       che,
                       ts,
                       te);
    }
  }

  return res;
}

} // namespace nim
