#include "zneutubetraceswclabelstack.h"

#include "zneutube3dgeom.h"
#include "zneutubegeo3dscalarfield.h"
#include "zneutubemathutils.h"
#include "zneutubeneuroseg.h"
#include "zneutubetraceswclocseg.h"

#include "zlog.h"
#include "zvoxelvolume.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nim {

namespace {

[[nodiscard]] int compareFloatLegacyLike(double a, double b, double eps)
{
  if (a < b - eps) {
    return -1;
  }
  if (a > b + eps) {
    return 1;
  }
  return 0;
}

[[nodiscard]] int testZToXYRatioLegacyLike(double zToXYRatio)
{
  return compareFloatLegacyLike(zToXYRatio, 1.0, 1e-5);
}

void scaleXRotateZLegacyLike(std::array<double, 3>* p, double s, double alpha, int inverse)
{
  CHECK(p != nullptr);

  const double cosA = std::cos(alpha);
  const double sinA = std::sin(alpha);

  if (inverse == 0) {
    const double tmp = (*p)[0] * s * cosA - (*p)[1] * sinA;
    (*p)[1] = (*p)[0] * s * sinA + (*p)[1] * cosA;
    (*p)[0] = tmp;
  } else {
    const double tmp = ((*p)[0] * cosA + (*p)[1] * sinA) / s;
    (*p)[1] = -(*p)[0] * sinA + (*p)[1] * cosA;
    (*p)[0] = tmp;
  }
}

[[nodiscard]] bool writeMaskVoxel(ZImg& mask, size_t x, size_t y, size_t z, int value)
{
  CHECK(mask.numChannels() == 1);
  CHECK(mask.numTimes() == 1);

  imgTypeDispatcher(mask.info(), [&]<typename TVoxel>() {
    *mask.data<TVoxel>(x, y, z) = static_cast<TVoxel>(value);
  });
  return true;
}

void writeMaskVoxel(ZVoxelMaskMutable& mask, int x, int y, int z, int value)
{
  CHECK(value >= 0);
  CHECK(value <= static_cast<int>(std::numeric_limits<std::uint8_t>::max()));
  mask.setValueU8(x, y, z, static_cast<std::uint8_t>(value));
}

} // namespace

double neurofield7LegacyLike(double coef, double base, double x, double y, double z, double zMin, double zMax)
{
  // Port of tz_neurofield.c::Neurofield7().
  if (z < zMin) {
    return 0.0;
  }
  if (z > zMax) {
    return 0.0;
  }

  double sigma2 = coef * z + base;
  sigma2 = sigma2 * sigma2;

  const double t = (x * x + y * y) / sigma2;

  // 0.01 is for relaxation (legacy comment).
  if (t > 1.01) {
    return 0.0;
  }

  return (1.0 - t) * std::exp(-t) / sigma2 + 0.1;
}

FieldRangeLegacyLike neurosegFieldRangeLegacyLike(const Neuroseg& seg, double zToXYRatio)
{
  // Port of tz_neuroseg.c::Neuroseg_Field_Range().
  FieldRangeLegacyLike out;

  static thread_local Geo3dScalarField fieldScratch;
  neurosegFieldSFastLegacyLikeInto(seg, nullptr, fieldScratch);
  if (fieldScratch.points.empty()) {
    return out;
  }

  std::array<double, 3> boundMin = fieldScratch.points.front();
  std::array<double, 3> boundMax = fieldScratch.points.front();

  for (size_t i = 1; i < fieldScratch.points.size(); ++i) {
    const auto& p = fieldScratch.points[i];
    for (size_t j = 0; j < 3; ++j) {
      if (p[j] <= boundMin[j]) {
        boundMin[j] = p[j];
      } else if (p[j] > boundMax[j]) {
        boundMax[j] = p[j];
      }
    }
  }

  out.firstCorner[0] = static_cast<int>(boundMin[0] - 1.5);
  out.firstCorner[1] = static_cast<int>(boundMin[1] - 1.5);
  out.firstCorner[2] = static_cast<int>(boundMin[2] / zToXYRatio - 1.5);

  out.size[0] = static_cast<int>(boundMax[0] - static_cast<double>(out.firstCorner[0]) + 1.5);
  out.size[1] = static_cast<int>(boundMax[1] - static_cast<double>(out.firstCorner[1]) + 1.5);
  out.size[2] = static_cast<int>(boundMax[2] / zToXYRatio - static_cast<double>(out.firstCorner[2]) + 1.5);

  return out;
}

void localNeurosegLabelCLegacyLike(const LocalNeuroseg& locseg, ZImg& mask, double zToXYRatio, int value)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Label_C().
  CHECK(mask.numChannels() == 1);
  CHECK(mask.numTimes() == 1);

  if (mask.isEmpty()) {
    return;
  }

  const size_t width = mask.width();
  const size_t height = mask.height();
  const size_t depth = mask.depth();

  const std::array<double, 3> bottom = localNeurosegBottomLegacyLike(locseg);

  std::array<int, 3> c = {0, 0, 0};
  std::array<double, 3> offpos = {0.0, 0.0, 0.0};

  c[0] = iroundLegacyLike(bottom[0]);
  c[1] = iroundLegacyLike(bottom[1]);
  offpos[0] = bottom[0] - static_cast<double>(c[0]);
  offpos[1] = bottom[1] - static_cast<double>(c[1]);

  if (testZToXYRatioLegacyLike(zToXYRatio) != 0) {
    c[2] = iroundLegacyLike(bottom[2] / zToXYRatio);
    offpos[2] = bottom[2] / zToXYRatio - static_cast<double>(c[2]);
  } else {
    c[2] = iroundLegacyLike(bottom[2]);
    offpos[2] = bottom[2] - static_cast<double>(c[2]);
  }

  const FieldRangeLegacyLike range = neurosegFieldRangeLegacyLike(locseg.seg, zToXYRatio);

  const std::array<int, 3> regionCorner = {range.firstCorner[0] + c[0],
                                           range.firstCorner[1] + c[1],
                                           range.firstCorner[2] + c[2]};

  const double coef = locseg.seg.c;

  for (int k = 0; k < range.size[2]; ++k) {
    const int pz = regionCorner[2] + k;
    for (int j = 0; j < range.size[1]; ++j) {
      const int py = regionCorner[1] + j;
      for (int i = 0; i < range.size[0]; ++i) {
        const int px = regionCorner[0] + i;

        std::array<double, 3> coord = {static_cast<double>(i + range.firstCorner[0]),
                                       static_cast<double>(j + range.firstCorner[1]),
                                       static_cast<double>(k + range.firstCorner[2])};

        if (testZToXYRatioLegacyLike(zToXYRatio) != 0) {
          coord[2] *= zToXYRatio;
        }

        coord[0] -= offpos[0];
        coord[1] -= offpos[1];
        coord[2] -= offpos[2];

        rotateXZLegacyLike(coord, locseg.seg.theta, locseg.seg.psi, 1);
        scaleXRotateZLegacyLike(&coord, locseg.seg.scale, locseg.seg.alpha, 1);

        const double f = neurofield7LegacyLike(coef,
                                               locseg.seg.r1,
                                               coord[0],
                                               coord[1],
                                               coord[2],
                                               /*zMin*/ -0.5,
                                               /*zMax*/ locseg.seg.h - 0.5);

        if ((px >= 0) && (py >= 0) && (pz >= 0) && (static_cast<size_t>(px) < width) &&
            (static_cast<size_t>(py) < height) && (static_cast<size_t>(pz) < depth) && (f > 0.0)) {
          (void)writeMaskVoxel(mask, static_cast<size_t>(px), static_cast<size_t>(py), static_cast<size_t>(pz), value);
        }
      }
    }
  }
}

void localNeurosegLabelCLegacyLike(const LocalNeuroseg& locseg, ZVoxelMaskMutable& mask, double zToXYRatio, int value)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Label_C().
  if (mask.isEmpty()) {
    return;
  }

  const size_t width = mask.width();
  const size_t height = mask.height();
  const size_t depth = mask.depth();

  const std::array<double, 3> bottom = localNeurosegBottomLegacyLike(locseg);

  std::array<int, 3> c = {0, 0, 0};
  std::array<double, 3> offpos = {0.0, 0.0, 0.0};

  c[0] = iroundLegacyLike(bottom[0]);
  c[1] = iroundLegacyLike(bottom[1]);
  offpos[0] = bottom[0] - static_cast<double>(c[0]);
  offpos[1] = bottom[1] - static_cast<double>(c[1]);

  if (testZToXYRatioLegacyLike(zToXYRatio) != 0) {
    c[2] = iroundLegacyLike(bottom[2] / zToXYRatio);
    offpos[2] = bottom[2] / zToXYRatio - static_cast<double>(c[2]);
  } else {
    c[2] = iroundLegacyLike(bottom[2]);
    offpos[2] = bottom[2] - static_cast<double>(c[2]);
  }

  const FieldRangeLegacyLike range = neurosegFieldRangeLegacyLike(locseg.seg, zToXYRatio);

  const std::array<int, 3> regionCorner = {range.firstCorner[0] + c[0],
                                           range.firstCorner[1] + c[1],
                                           range.firstCorner[2] + c[2]};

  const double coef = locseg.seg.c;

  for (int k = 0; k < range.size[2]; ++k) {
    const int pz = regionCorner[2] + k;
    for (int j = 0; j < range.size[1]; ++j) {
      const int py = regionCorner[1] + j;
      for (int i = 0; i < range.size[0]; ++i) {
        const int px = regionCorner[0] + i;

        std::array<double, 3> coord = {static_cast<double>(i + range.firstCorner[0]),
                                       static_cast<double>(j + range.firstCorner[1]),
                                       static_cast<double>(k + range.firstCorner[2])};

        if (testZToXYRatioLegacyLike(zToXYRatio) != 0) {
          coord[2] *= zToXYRatio;
        }

        coord[0] -= offpos[0];
        coord[1] -= offpos[1];
        coord[2] -= offpos[2];

        rotateXZLegacyLike(coord, locseg.seg.theta, locseg.seg.psi, 1);
        scaleXRotateZLegacyLike(&coord, locseg.seg.scale, locseg.seg.alpha, 1);

        const double f = neurofield7LegacyLike(coef,
                                               locseg.seg.r1,
                                               coord[0],
                                               coord[1],
                                               coord[2],
                                               /*zMin*/ -0.5,
                                               /*zMax*/ locseg.seg.h - 0.5);

        if ((px >= 0) && (py >= 0) && (pz >= 0) && (static_cast<size_t>(px) < width) &&
            (static_cast<size_t>(py) < height) && (static_cast<size_t>(pz) < depth) && (f > 0.0)) {
          writeMaskVoxel(mask, px, py, pz, value);
        }
      }
    }
  }
}

void geo3dBallLabelStackLegacyLike(const std::array<double, 3>& center, double radius, ZImg& mask, int value)
{
  // Port of tz_geo3d_ball.c::Geo3d_Ball_Label_Stack().
  CHECK(mask.numChannels() == 1);
  CHECK(mask.numTimes() == 1);

  if (mask.isEmpty()) {
    return;
  }

  const int width = static_cast<int>(mask.width());
  const int height = static_cast<int>(mask.height());
  const int depth = static_cast<int>(mask.depth());

  std::array<int, 3> cb{};
  std::array<int, 3> ce{};
  for (size_t i = 0; i < 3; ++i) {
    cb[i] = std::max(0, static_cast<int>(std::ceil(center[i] - radius)));
    ce[i] = static_cast<int>(std::floor(center[i] + radius));
  }

  ce[0] = std::min(ce[0], width - 1);
  ce[1] = std::min(ce[1], height - 1);
  ce[2] = std::min(ce[2], depth - 1);

  const double r2 = radius * radius;
  for (int k = cb[2]; k <= ce[2]; ++k) {
    for (int j = cb[1]; j <= ce[1]; ++j) {
      for (int i = cb[0]; i <= ce[0]; ++i) {
        const double dx = static_cast<double>(i) - center[0];
        const double dy = static_cast<double>(j) - center[1];
        const double dz = static_cast<double>(k) - center[2];
        if (dx * dx + dy * dy + dz * dz <= r2) {
          (void)writeMaskVoxel(mask, static_cast<size_t>(i), static_cast<size_t>(j), static_cast<size_t>(k), value);
        }
      }
    }
  }
}

void geo3dBallLabelStackLegacyLike(const std::array<double, 3>& center,
                                   double radius,
                                   ZVoxelMaskMutable& mask,
                                   int value)
{
  // Port of tz_geo3d_ball.c::Geo3d_Ball_Label_Stack().
  if (mask.isEmpty()) {
    return;
  }

  const int width = static_cast<int>(mask.width());
  const int height = static_cast<int>(mask.height());
  const int depth = static_cast<int>(mask.depth());

  std::array<int, 3> cb{};
  std::array<int, 3> ce{};
  for (size_t i = 0; i < 3; ++i) {
    cb[i] = std::max(0, static_cast<int>(std::ceil(center[i] - radius)));
    ce[i] = static_cast<int>(std::floor(center[i] + radius));
  }

  ce[0] = std::min(ce[0], width - 1);
  ce[1] = std::min(ce[1], height - 1);
  ce[2] = std::min(ce[2], depth - 1);

  const double r2 = radius * radius;
  for (int k = cb[2]; k <= ce[2]; ++k) {
    for (int j = cb[1]; j <= ce[1]; ++j) {
      for (int i = cb[0]; i <= ce[0]; ++i) {
        const double dx = static_cast<double>(i) - center[0];
        const double dy = static_cast<double>(j) - center[1];
        const double dz = static_cast<double>(k) - center[2];
        if (dx * dx + dy * dy + dz * dz <= r2) {
          writeMaskVoxel(mask, i, j, k, value);
        }
      }
    }
  }
}

void labelSwcIntoMaskLegacyLike(const ZSwc& swc, ZImg& mask, double zToXYRatio, int value)
{
  // Port of ZSwcTree::labelStack(Stack*) -> Swc_Tree_Node_Label_Stack().
  for (auto it = swc.cbeginBreadthFirst(); it != swc.cendBreadthFirst(); ++it) {
    if (ZSwc::isNull(it)) {
      continue;
    }

    if (!ZSwc::isRoot(it)) {
      const std::optional<LocalNeuroseg> seg = swcNodeToLocsegLegacyLike(it, zToXYRatio);
      if (seg.has_value()) {
        localNeurosegLabelCLegacyLike(*seg, mask, zToXYRatio, value);
      }
    }

    geo3dBallLabelStackLegacyLike({it->x, it->y, it->z}, it->radius, mask, value);
  }
}

void labelSwcIntoMaskLegacyLike(const ZSwc& swc, ZVoxelMaskMutable& mask, double zToXYRatio, int value)
{
  for (auto it = swc.cbeginBreadthFirst(); it != swc.cendBreadthFirst(); ++it) {
    if (ZSwc::isNull(it)) {
      continue;
    }

    if (!ZSwc::isRoot(it)) {
      const std::optional<LocalNeuroseg> seg = swcNodeToLocsegLegacyLike(it, zToXYRatio);
      if (seg.has_value()) {
        localNeurosegLabelCLegacyLike(*seg, mask, zToXYRatio, value);
      }
    }

    geo3dBallLabelStackLegacyLike({it->x, it->y, it->z}, it->radius, mask, value);
  }
}

} // namespace nim
