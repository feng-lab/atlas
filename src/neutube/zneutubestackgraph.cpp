#include "zneutubestackgraph.h"

#include "zlog.h"

#include <algorithm>
#include <limits>

namespace nim {

void defaultStackGraphWorkspaceLegacyLike(StackGraphWorkspaceLegacyLike& sgw)
{
  sgw.conn = 26;
  sgw.range.reset();
  sgw.weightFunc = &stackVoxelWeightSLegacyLike;
  sgw.spOption = 0;
  sgw.resolution = {1.0, 1.0, 1.0};
  sgw.argv.fill(std::numeric_limits<double>::quiet_NaN());
  sgw.groupMask.reset();
  sgw.signalMask = nullptr;
  sgw.value = 0.0;
  sgw.virtualVertex = -1;
  sgw.includingSignalBorder = false;
  sgw.greyFactor = 1.0;
  sgw.greyOffset = 0.0;
}

void stackGraphWorkspaceSetRangeLegacyLike(StackGraphWorkspaceLegacyLike& sgw,
                                           int x0,
                                           int x1,
                                           int y0,
                                           int y1,
                                           int z0,
                                           int z1)
{
  std::array<int, 6> r = {x0, x1, y0, y1, z0, z1};
  if (r[0] > r[1]) {
    std::swap(r[0], r[1]);
  }
  if (r[2] > r[3]) {
    std::swap(r[2], r[3]);
  }
  if (r[4] > r[5]) {
    std::swap(r[4], r[5]);
  }

  sgw.range = r;
}

void stackGraphWorkspaceUpdateRangeLegacyLike(StackGraphWorkspaceLegacyLike& sgw, int x, int y, int z)
{
  CHECK(sgw.range.has_value());

  std::array<int, 6>& r = *sgw.range;

  if (x < r[0]) {
    r[0] = x;
  } else if (x > r[1]) {
    r[1] = x;
  }

  if (y < r[2]) {
    r[2] = y;
  } else if (y > r[3]) {
    r[3] = y;
  }

  if (z < r[4]) {
    r[4] = z;
  } else if (z > r[5]) {
    r[5] = z;
  }
}

void stackGraphWorkspaceExpandRangeLegacyLike(StackGraphWorkspaceLegacyLike& sgw,
                                              int mx0,
                                              int mx1,
                                              int my0,
                                              int my1,
                                              int mz0,
                                              int mz1)
{
  CHECK(sgw.range.has_value());

  std::array<int, 6>& r = *sgw.range;
  r[0] -= mx0;
  r[1] += mx1;
  r[2] -= my0;
  r[3] += my1;
  r[4] -= mz0;
  r[5] += mz1;
}

void stackGraphWorkspaceValidateRangeLegacyLike(StackGraphWorkspaceLegacyLike& sgw, int width, int height, int depth)
{
  CHECK(sgw.range.has_value());

  std::array<int, 6>& r = *sgw.range;

  if (r[0] < 0) {
    r[0] = 0;
  }
  if (r[1] > width - 1) {
    r[1] = width - 1;
  }

  if (r[2] < 0) {
    r[2] = 0;
  }
  if (r[3] > height - 1) {
    r[3] = height - 1;
  }

  if (r[4] < 0) {
    r[4] = 0;
  }
  if (r[5] > depth - 1) {
    r[5] = depth - 1;
  }
}

double stackVoxelWeightSLegacyLike(double* argv)
{
  CHECK(argv != nullptr);

  const double v1 = argv[1];
  const double v2 = argv[2];
  const double d = argv[0];

  double thre = argv[3];
  if (std::isnan(thre)) {
    thre = 60.0;
  }

  double scale = argv[4];
  if (std::isnan(scale)) {
    scale = 5.0;
  }

  const double w =
    d * (1.0 / (1.0 + std::exp((v1 - thre) / scale)) + 1.0 / (1.0 + std::exp((v2 - thre) / scale)) + 0.00001);
  return w;
}

double stackVoxelWeightSrLegacyLike(double* argv)
{
  CHECK(argv != nullptr);

  const double v1 = argv[1];
  const double v2 = argv[2];
  const double d = argv[0];

  double thre = argv[3];
  if (std::isnan(thre)) {
    thre = 60.0;
  }

  double scale = argv[4];
  if (std::isnan(scale)) {
    scale = 5.0;
  }

  // Legacy Stack_Voxel_Weight_Sr:
  // d * (1/(1+exp((thre-v1)/scale)) + 1/(1+exp((thre-v2)/scale)) + 0.00001)
  const double w =
    d * (1.0 / (1.0 + std::exp((thre - v1) / scale)) + 1.0 / (1.0 + std::exp((thre - v2) / scale)) + 0.00001);
  return w;
}

} // namespace nim
