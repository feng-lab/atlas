#include "zneutubetraceswclocseg.h"

#include "zneutubegeo3dutils.h"

#include "zlog.h"

#include <cmath>

namespace nim {

void localNeurosegChangeTopLegacyLike(LocalNeuroseg& locseg, const std::array<double, 3>& newTop)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Change_Top().
  const std::array<double, 3> bottom = localNeurosegBottomLegacyLike(locseg);
  std::array<double, 3> axisVector = {newTop[0] - bottom[0], newTop[1] - bottom[1], newTop[2] - bottom[2]};

  const double h =
    std::sqrt(axisVector[0] * axisVector[0] + axisVector[1] * axisVector[1] + axisVector[2] * axisVector[2]);
  if (h < 0.1) {
    locseg.seg.h = 1.0;
    return;
  }

  axisVector[0] /= h;
  axisVector[1] /= h;
  axisVector[2] /= h;
  locseg.seg.h = h + 1.0;

  geo3dNormalOrientationLegacyLike(axisVector[0], axisVector[1], axisVector[2], locseg.seg.theta, locseg.seg.psi);

  setNeurosegPositionLegacyLike(locseg, newTop, NeuroposReferenceLegacyLike::Top);
}

std::optional<LocalNeuroseg> swcNodeToLocsegLegacyLike(const ZSwc::ConstSwcTreeNode& node, double zToXYRatio)
{
  // Port of tz_swc_tree.c::Swc_Tree_Node_To_Locseg().
  CHECK(std::isfinite(zToXYRatio));
  CHECK(zToXYRatio > 0.0);
  if (ZSwc::isNull(node)) {
    return std::nullopt;
  }
  if (ZSwc::isRoot(node)) {
    return std::nullopt;
  }

  const auto parent = ZSwc::parent(node);
  if (ZSwc::isNull(parent)) {
    return std::nullopt;
  }

  LocalNeuroseg locseg;
  locseg.pos = {parent->x, parent->y, parent->z * zToXYRatio};

  const std::array<double, 3> top = {node->x, node->y, node->z * zToXYRatio};
  localNeurosegChangeTopLegacyLike(locseg, top);

  locseg.seg.r1 = parent->radius;
  locseg.seg.scale = 1.0;

  const double adjustedHeight = locseg.seg.h - 1.0;
  if (adjustedHeight < 0.001) {
    locseg.seg.c = 1.0;
  } else {
    locseg.seg.c = (node->radius - locseg.seg.r1) / adjustedHeight;
  }

  locseg.seg.alpha = 0.0;
  locseg.seg.curvature = 0.0;

  return locseg;
}

} // namespace nim
