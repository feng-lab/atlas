#pragma once

#include "zimg.h"
#include "zswc.h"

#include <array>
#include <memory>

namespace nim::neutube {

// ZImg/ZSwc-native skeletonization algorithm (neuTube legacy port).
//
// Goal: preserve byte-identical behavior vs the legacy Stack/C implementation, but without
// depending on Stack / tz_* C libraries / Swc_Tree adapters.
class ZNeutubeSkeletonizer
{
public:
  ZNeutubeSkeletonizer();

  void setLengthThreshold(double threshold)
  {
    m_lengthThreshold = threshold;
  }

  void setFinalLengthThreshold(double t)
  {
    m_finalLengthThreshold = t;
  }

  void setDistanceThreshold(double threshold)
  {
    m_distanceThreshold = threshold;
  }

  void setRebase(bool rebase)
  {
    m_rebase = rebase;
  }

  void setInterpolating(bool inter)
  {
    m_interpolating = inter;
  }

  void setRemovingBorder(bool removing)
  {
    m_removingBorder = removing;
  }

  void setFillingHole(bool filling)
  {
    m_fillingHole = filling;
  }

  void setMinObjSize(int s)
  {
    m_minObjSize = s;
  }

  void setKeepingSingleObject(bool keeping)
  {
    m_keepingSingleObject = keeping;
  }

  void setLevel(int level)
  {
    m_level = level;
  }

  // 0: >=, 1: <=, 2: ==
  void setLevelOp(int op)
  {
    m_grayOp = op;
  }

  void setResolution(double xyRes, double zRes);

  void setDownsampleInterval(int xintv, int yintv, int zintv);

  void setConnectingBranch(bool connecting)
  {
    m_connectingBranch = connecting;
  }

  void useOriginalSignal(bool state)
  {
    m_usingOriginalSignal = state;
  }

  void setResampleSwc(bool v)
  {
    m_resampleSwc = v;
  }

  void setAutoGrayThreshold(bool v)
  {
    m_autoGrayThreshold = v;
  }

  // Returns an empty pointer when no skeleton is generated (matching legacy behavior).
  [[nodiscard]] std::unique_ptr<ZSwc> makeSkeleton(const ZImg& img) const;

  void print() const;

private:
  [[nodiscard]] std::unique_ptr<ZSwc> makeSkeletonWithoutDs(ZImg img, const std::array<int, 3>& dsIntv) const;

  [[nodiscard]] double getLengthThreshold(double linScale) const;

private:
  double m_lengthThreshold = 15.0;
  double m_finalLengthThreshold = 0.0;
  double m_distanceThreshold = -1.0;
  bool m_rebase = false;
  bool m_interpolating = false;
  bool m_removingBorder = false;
  bool m_fillingHole = false;
  int m_minObjSize = 0;
  bool m_keepingSingleObject = false;
  int m_level = -1;
  bool m_connectingBranch = true;
  std::array<double, 3> m_resolution = {1.0, 1.0, 1.0};
  std::array<int, 3> m_downsampleInterval = {0, 0, 0};
  bool m_usingOriginalSignal = false;
  bool m_resampleSwc = true;
  bool m_autoGrayThreshold = true;
  int m_grayOp = 0;
};

} // namespace nim::neutube
