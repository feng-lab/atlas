#include "zneutubetraceconnectiontest.h"

#include "zneutubelocsegchaininterpolate.h"
#include "zneutubelocsegchainshortestpath.h"
#include "zneutubegeo3dutils.h"
#include "zneutubelocalneuroseg.h"
#include "zneutubeneuroseg.h"
#include "zneutubestackgraph.h"
#include "zswcgeom.h"

#include "zlog.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace nim {

namespace {

[[nodiscard]] double logisticLegacyLike(double x)
{
  return 1.0 / (1.0 + std::exp(-x));
}

void stackUtilCoordLegacyLike(int64_t offset, int width, int height, int* x, int* y, int* z)
{
  CHECK(x != nullptr);
  CHECK(y != nullptr);
  CHECK(z != nullptr);
  CHECK(width > 0);
  CHECK(height > 0);
  const int64_t area = static_cast<int64_t>(width) * static_cast<int64_t>(height);
  const int64_t zz = offset / area;
  const int64_t rem = offset - zz * area;
  const int64_t yy = rem / static_cast<int64_t>(width);
  const int64_t xx = rem - yy * static_cast<int64_t>(width);
  *x = static_cast<int>(xx);
  *y = static_cast<int>(yy);
  *z = static_cast<int>(zz);
}

[[nodiscard]] double stackArrayValueLegacyLike(const ZImg& stack, int64_t offset)
{
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);
  CHECK(offset >= 0);
  CHECK(static_cast<size_t>(offset) < stack.voxelNumber());

  return imgTypeDispatcher(stack.info(), [&]<typename TVoxel>() -> double {
    return static_cast<double>(stack.timeData<TVoxel>(0)[static_cast<size_t>(offset)]);
  });
}

void coordinate3dUnitizeLegacyLike(std::array<double, 3>* v)
{
  CHECK(v != nullptr);
  const double norm = std::sqrt((*v)[0] * (*v)[0] + (*v)[1] * (*v)[1] + (*v)[2] * (*v)[2]);
  if (norm != 0.0) {
    (*v)[0] /= norm;
    (*v)[1] /= norm;
    (*v)[2] /= norm;
  }
}

[[nodiscard]] double
locsegChainDistUpperBoundLegacyLike(const LocsegChain& chain, double zScale, const LocalNeuroseg& testSeg)
{
  // Port of tz_locseg_chain.c::locseg_chain_dist_upper_bound().
  const std::array<double, 3> source = localNeurosegCenterLegacyLike(testSeg);

  double minDist = std::numeric_limits<double>::infinity();
  for (const auto& node : chain) {
    LocalNeuroseg locseg2 = node.locseg;
    localNeurosegScaleZLegacyLike(locseg2, zScale);
    const std::array<double, 3> target = localNeurosegCenterLegacyLike(locseg2);
    const double dist = geo3dDist(source[0], source[1], source[2], target[0], target[1], target[2]);
    if (dist < minDist) {
      minDist = dist;
    }
  }

  return minDist;
}

} // namespace

bool locsegChainConnectionTestLegacyLike(const LocsegChain& chain1,
                                         LocsegChain& chain2,
                                         const ZImg* signal,
                                         double zScale,
                                         NeurocompConnLegacyLike& conn,
                                         const ConnectionTestWorkspaceLegacyLike& ctw)
{
  // No connection if either of the chains is empty.
  if (chain1.empty() || chain2.empty()) {
    conn.mode = NeurocompConnModeLegacyLike::None;
    return false;
  }

  // Get head and tail of the hook.
  const LocalNeuroseg* shead = chain1.headSeg();
  const LocalNeuroseg* stail = chain1.tailSeg();
  CHECK(shead != nullptr);
  CHECK(stail != nullptr);

  LocalNeuroseg head = *shead;
  LocalNeuroseg tail = *stail;

  // Adjust head and tail.
  flipLocalNeurosegLegacyLike(tail);

  if (chain1.length() >= 1) {
    head.seg.h = 2.0;
    tail.seg.h = 2.0;
  }

  localNeurosegScaleZLegacyLike(head, zScale);
  localNeurosegScaleZLegacyLike(tail, zScale);

  double mindist = 0.0;
  if (ctw.hookSpot == 0) {
    mindist = locsegChainDistUpperBoundLegacyLike(chain2, zScale, head);
  } else if (ctw.hookSpot == 1) {
    mindist = locsegChainDistUpperBoundLegacyLike(chain2, zScale, tail);
  } else {
    CHECK(ctw.hookSpot == -1);
    mindist = std::min(locsegChainDistUpperBoundLegacyLike(chain2, zScale, head),
                       locsegChainDistUpperBoundLegacyLike(chain2, zScale, tail));
  }

  conn.mode = NeurocompConnModeLegacyLike::HookLoop;
  conn.pdist = 0.0;

  int index = 0;
  Geo3dBallLegacyLike range1{};
  Geo3dBallLegacyLike range2{};
  std::array<double, 3> tmpPos{};

  // Calculate the distance from the hook end(s) to the loop chain surface.
  for (const auto& node : chain2) {
    LocalNeuroseg locseg2 = node.locseg;
    localNeurosegScaleZLegacyLike(locseg2, zScale);
    localNeurosegBallBoundLegacyLike(locseg2, range1);

    // head test
    if (ctw.hookSpot == 0 || ctw.hookSpot == -1) {
      localNeurosegBallBoundLegacyLike(head, range2);
      const double minPossible = geo3dDist(range1.center[0],
                                           range1.center[1],
                                           range1.center[2],
                                           range2.center[0],
                                           range2.center[1],
                                           range2.center[2]) -
                                 range1.radius - range2.radius;
      if (minPossible < mindist) {
        const double dist = localNeurosegDist2LegacyLike(head, locseg2, &tmpPos);
        bool update = false;
        if (dist < mindist) {
          mindist = dist;
          conn.pdist = localNeurosegPlanarDistLLegacyLike(head, locseg2);
          update = true;
        } else if (dist == mindist) {
          const double pdist = localNeurosegPlanarDistLLegacyLike(head, locseg2);
          if (conn.pdist > pdist) {
            conn.pdist = pdist;
            update = true;
          }
        }

        if (update) {
          conn.info[0] = 0;
          conn.info[1] = index;
          conn.pos = tmpPos;
        }
      }
    }

    // tail test
    if (ctw.hookSpot == 1 || ctw.hookSpot == -1) {
      localNeurosegBallBoundLegacyLike(tail, range2);
      const double minPossible = geo3dDist(range1.center[0],
                                           range1.center[1],
                                           range1.center[2],
                                           range2.center[0],
                                           range2.center[1],
                                           range2.center[2]) -
                                 range1.radius - range2.radius;
      if (minPossible < mindist) {
        const double dist = localNeurosegDist2LegacyLike(tail, locseg2, &tmpPos);
        if (dist < mindist) {
          mindist = dist;
          conn.info[0] = 1;
          conn.info[1] = index;
          conn.pos = tmpPos;
          conn.pdist = localNeurosegPlanarDistLLegacyLike(tail, locseg2);
        } else if (dist == mindist) {
          const double pdist = localNeurosegPlanarDistLLegacyLike(tail, locseg2);
          if (conn.pdist > pdist) {
            conn.info[0] = 1;
            conn.info[1] = index;
            conn.pos = tmpPos;
            conn.pdist = pdist;
          }
        }
      }
    }

    ++index;
  }

  // scale position back to the chain space
  conn.pos[2] *= zScale;

  conn.sdist = mindist;

  if (conn.sdist > ctw.distThre) {
    conn.mode = NeurocompConnModeLegacyLike::None;
    conn.cost = 10.0;
    return false;
  }

  // Compute default orientation from the hook end.
  {
    const LocalNeuroseg* locseg1 = nullptr;
    if (conn.info[0] == 0) {
      locseg1 = chain1.headSeg();
    } else {
      locseg1 = chain1.tailSeg();
    }
    CHECK(locseg1 != nullptr);

    double nx = 0.0;
    double ny = 0.0;
    double nz = 0.0;
    geo3dOrientationNormalLegacyLike(locseg1->seg.theta, locseg1->seg.psi, nx, ny, nz);
    conn.ort = {nx, ny, nz};
  }

  if (conn.sdist > 2.0 && ctw.spTest && signal != nullptr) {
    double gdist = 0.0;

    StackGraphWorkspaceLegacyLike sgw;
    defaultStackGraphWorkspaceLegacyLike(sgw);
    sgw.conn = 26;
    sgw.weightFunc = &stackVoxelWeightSLegacyLike;
    sgw.resolution = ctw.resolution;
    sgw.signalMask = ctw.mask;
    sgw.includingSignalBorder = true;

    const std::vector<int64_t> path = locsegChainShortestPathLegacyLike(chain1, chain2, *signal, zScale, sgw);
    sgw.signalMask = nullptr;

    if (!path.empty()) {
      gdist = sgw.value;

      const int width = static_cast<int>(signal->width());
      const int height = static_cast<int>(signal->height());

      int coord[3] = {0, 0, 0};
      int hitIndex = 0;
      int darkCount = 0;
      int brightCount = 0;

      for (size_t k = 0; k < path.size(); ++k) {
        const int64_t off = path[k];

        if (hitIndex < 3) {
          stackUtilCoordLegacyLike(off, width, height, &coord[0], &coord[1], &coord[2]);
          if (conn.info[0] == 0) {
            hitIndex = locsegChainHitTestLegacyLike(chain1, TraceDirection::Forward, coord[0], coord[1], coord[2]);
          } else {
            hitIndex = locsegChainHitTestLegacyLike(chain1, TraceDirection::Backward, coord[0], coord[1], coord[2]);
          }
        }

        bool count = true;
        if (ctw.mask != nullptr) {
          if (stackArrayValueLegacyLike(*ctw.mask, off) < 0.5) {
            count = false;
          }
        }

        if (count) {
          const double v = stackArrayValueLegacyLike(*signal, off);
          if ((v < sgw.argv[3] - sgw.argv[4]) || (v == 0.0)) {
            ++darkCount;
          } else {
            ++brightCount;
          }
        }
      }

      if (((darkCount >= 2) && (darkCount >= brightCount)) || (darkCount >= 5) || (hitIndex >= 3)) {
        conn.mode = NeurocompConnModeLegacyLike::None;
      } else {
        if (darkCount + brightCount >= 2 && path.size() >= 3) {
          int prevPos[3] = {0, 0, 0};
          int curPos[3] = {0, 0, 0};
          stackUtilCoordLegacyLike(path[path.size() - 2], width, height, &prevPos[0], &prevPos[1], &prevPos[2]);

          conn.ort = {0.0, 0.0, 0.0};
          int countSteps = 0;
          for (int i = static_cast<int>(path.size()) - 3; i >= 0; --i) {
            stackUtilCoordLegacyLike(path[static_cast<size_t>(i)], width, height, &curPos[0], &curPos[1], &curPos[2]);
            conn.ort[0] += static_cast<double>(prevPos[0] - curPos[0]);
            conn.ort[1] += static_cast<double>(prevPos[1] - curPos[1]);
            conn.ort[2] += static_cast<double>(prevPos[2] - curPos[2]);
            prevPos[0] = curPos[0];
            prevPos[1] = curPos[1];
            prevPos[2] = curPos[2];
            ++countSteps;
            if (countSteps >= 5) {
              break;
            }
          }

          coordinate3dUnitizeLegacyLike(&conn.ort);
        }
      }

      gdist /= sgw.resolution[0];
    } else {
      conn.mode = NeurocompConnModeLegacyLike::None;
      gdist = 0.0;
    }

    if (conn.mode != NeurocompConnModeLegacyLike::None) {
      conn.cost = logisticLegacyLike((conn.sdist + gdist) / 100.0);
    }
  } else {
    conn.cost = logisticLegacyLike(conn.sdist / 100.0);
  }

  if (conn.mode == NeurocompConnModeLegacyLike::None) {
    return false;
  }

  if (ctw.interpolate) {
    if (conn.mode == NeurocompConnModeLegacyLike::HookLoop) {
      std::array<double, 3> pos = conn.pos;
      const int insertedIndex = locsegChainInterpolateLLegacyLike(chain2, pos, &conn.ort, &pos);
      conn.pos = pos;

      if (insertedIndex >= 0) {
        conn.info[1] = insertedIndex;
      } else {
        if (conn.info[1] == 0) {
          conn.mode = NeurocompConnModeLegacyLike::Link;
          conn.info[1] = 0;
        } else if (conn.info[1] == chain2.length() - 1) {
          conn.mode = NeurocompConnModeLegacyLike::Link;
          conn.info[1] = 1;
        }
      }
    }
  }

  return true;
}

} // namespace nim
