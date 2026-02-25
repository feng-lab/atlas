#pragma once

#include "zswc.h"

namespace nim {

template<typename Iter>
[[nodiscard]] bool isRegular(const Iter& pos)
{
  return !ZSwc::isNull(pos);
}

template<typename Iter>
[[nodiscard]] bool isRoot(const Iter& pos)
{
  return isRegular(pos) && ZSwc::isRoot(pos);
}

template<typename Iter>
[[nodiscard]] bool isLeaf(const Iter& pos)
{
  return isRegular(pos) && ZSwc::isLeaf(pos);
}

template<typename Iter>
[[nodiscard]] bool isBranchPoint(const Iter& pos)
{
  return isRegular(pos) && ZSwc::isBranchNode(pos);
}

template<typename Iter>
[[nodiscard]] bool isContinuation(const Iter& pos)
{
  if (!isRegular(pos)) {
    return false;
  }
  return !ZSwc::isRoot(pos) && !ZSwc::isLeaf(pos) && !ZSwc::isBranchNode(pos);
}

} // namespace nim
