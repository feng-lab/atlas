#pragma once

#include "zimginfo.h"
#include "zvoxelcoordinate.h"
#include "zjson.h"

namespace nim {

// a box region defined by [start, end)
// start should always >= 0, end should always >= -1
// end == -1 means to the end of that dimension
struct ZImgRegion
{
  using value_type = ZVoxelCoordinate::value_type;

  ZVoxelCoordinate start;
  ZVoxelCoordinate end;

  ZImgRegion()
    : start(0, 0, 0, 0, 0)
    , end(-1, -1, -1, -1, -1)
  {}

  ZImgRegion(value_type xStart,
             value_type xEnd,
             value_type yStart,
             value_type yEnd,
             value_type zStart = 0,
             value_type zEnd = -1,
             value_type cStart = 0,
             value_type cEnd = -1,
             value_type tStart = 0,
             value_type tEnd = -1)
    : start(xStart, yStart, zStart, cStart, tStart)
    , end(xEnd, yEnd, zEnd, cEnd, tEnd)
  {}

  ZImgRegion(const ZVoxelCoordinate& startCoord, const ZVoxelCoordinate& endCoord)
    : start(startCoord)
    , end(endCoord)
  {}

  bool operator==(const ZImgRegion& other) const
  {
    return start == other.start && end == other.end;
  }

  // return true if contained by img, can be an empty region
  // always return false for empty img
  [[nodiscard]] bool isValid(const ZImgInfo& info) const
  {
    return start.allGreaterEqual(0) &&
           start.allLessThan(ZVoxelCoordinate(info.width, info.height, info.depth, info.numChannels, info.numTimes)) &&
           (end.x == -1 || static_cast<size_t>(end.x) <= info.width) &&
           (end.y == -1 || static_cast<size_t>(end.y) <= info.height) &&
           (end.z == -1 || static_cast<size_t>(end.z) <= info.depth) &&
           (end.c == -1 || static_cast<size_t>(end.c) <= info.numChannels) &&
           (end.t == -1 || static_cast<size_t>(end.t) <= info.numTimes);
  }

  [[nodiscard]] bool isDefault() const
  {
    return start.x == 0 && start.y == 0 && start.z == 0 && start.c == 0 && start.t == 0 && end.x == -1 && end.y == -1 &&
           end.z == -1 && end.c == -1 && end.t == -1;
  }

  // check if this is an empty region
  [[nodiscard]] bool isEmpty() const
  {
    return start.anyLessThan(0) || (end.x != -1 && start.x >= end.x) || (end.y != -1 && start.y >= end.y) ||
           (end.z != -1 && start.z >= end.z) || (end.c != -1 && start.c >= end.c) || (end.t != -1 && start.t >= end.t);
  }

  [[nodiscard]] bool containsCoord(const ZVoxelCoordinate& coord, const ZImgInfo& info) const;

  // clamp region into img, might result in a empty region
  void fitInto(const ZImgInfo& info);

  // if *End is -1, change it to actual positive value based on img info
  void resolveRegionEnd(const ZImgInfo& info);

  [[nodiscard]] bool containsWholeRow(const ZImgInfo& info) const
  {
    return start.x == 0 && (end.x == -1 || end.x == static_cast<value_type>(info.width));
  }

  [[nodiscard]] bool containsWholePlane(const ZImgInfo& info) const
  {
    return containsWholeRow(info) && start.y == 0 && (end.y == -1 || end.y == static_cast<value_type>(info.height));
  }

  [[nodiscard]] bool containsWholeChannel(const ZImgInfo& info) const
  {
    return containsWholePlane(info) && start.z == 0 && (end.z == -1 || end.z == static_cast<value_type>(info.depth));
  }

  [[nodiscard]] bool containsWholeTime(const ZImgInfo& info) const
  {
    return containsWholeChannel(info) && start.c == 0 &&
           (end.c == -1 || end.c == static_cast<value_type>(info.numChannels));
  }

  [[nodiscard]] bool containsWholeImg(const ZImgInfo& info) const
  {
    return containsWholeTime(info) && start.t == 0 && (end.t == -1 || end.t == static_cast<value_type>(info.numTimes));
  }

  [[nodiscard]] std::string toString() const
  {
    return jsonToString(*this);
  }

  [[nodiscard]] ZImgInfo clip(const ZImgInfo& info) const;

  [[nodiscard]] bool xInRegion(value_type x) const
  {
    return x >= start.x && (end.x == -1 || x < end.x);
  }

  [[nodiscard]] bool yInRegion(value_type y) const
  {
    return y >= start.y && (end.y == -1 || y < end.y);
  }

  [[nodiscard]] bool zInRegion(value_type z) const
  {
    return z >= start.z && (end.z == -1 || z < end.z);
  }

  [[nodiscard]] bool cInRegion(value_type c) const
  {
    return c >= start.c && (end.c == -1 || c < end.c);
  }

  [[nodiscard]] bool tInRegion(value_type t) const
  {
    return t >= start.t && (end.t == -1 || t < end.t);
  }
  // inline bool lInRegion(value_type l) const { return l >= start.l && (end.l == -1 || l < end.l); }

  [[nodiscard]] value_type xStart() const
  {
    return start.x;
  }

  [[nodiscard]] value_type xEnd() const
  {
    return end.x;
  }

  [[nodiscard]] value_type yStart() const
  {
    return start.y;
  }

  [[nodiscard]] value_type yEnd() const
  {
    return end.y;
  }

  [[nodiscard]] value_type zStart() const
  {
    return start.z;
  }

  [[nodiscard]] value_type zEnd() const
  {
    return end.z;
  }

  [[nodiscard]] value_type cStart() const
  {
    return start.c;
  }

  [[nodiscard]] value_type cEnd() const
  {
    return end.c;
  }

  [[nodiscard]] value_type tStart() const
  {
    return start.t;
  }

  [[nodiscard]] value_type tEnd() const
  {
    return end.t;
  }
  //  value_type lStart() const { return start.l; }
  //  value_type lEnd() const { return end.l; }

  static std::vector<ZImgRegion> splitBigImage(const ZImgInfo& info,
                                               std::vector<ZImgRegion>& nonExpandRegions,
                                               size_t tileSize = 1024,
                                               size_t expand = 0,
                                               index_t ch = -1,
                                               index_t t = -1);
};

inline void tag_invoke(const json::value_from_tag&, json::value& jv, const ZImgRegion& rgn)
{
  auto& jo = jv.emplace_object();
  jo["start"] = json::value_from(rgn.start);
  jo["end"] = json::value_from(rgn.end);
}

inline ZImgRegion tag_invoke(const json::value_to_tag<ZImgRegion>&, const json::value& jv)
{
  ZImgRegion res;
  res.start = json::value_to<ZVoxelCoordinate>(jv.at("start"));
  res.end = json::value_to<ZVoxelCoordinate>(jv.at("end"));
  if (res.isEmpty()) {
    throw ZException(QString("Invalid json creates empty ZImgRegion"));
  }
  return res;
}

} // namespace nim
