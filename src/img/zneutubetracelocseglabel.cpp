#include "zneutubetracelocseglabel.h"

#include "zneutubelocsegchain.h"
#include "zneutubemathutils.h"
#include "zneutubeneighborhood.h"
#include "zneutubestackfitoptions.h"
#include "zneutubetraceswclabelstack.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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

[[nodiscard]] double stackNeighborMeanLegacyLike(const ZImg& stack, const ZNeighborhood& nb, int x, int y, int z)
{
  const size_t width = stack.width();
  const size_t height = stack.height();
  const size_t depth = stack.depth();
  const size_t plane = width * height;

  const size_t centerIdx = static_cast<size_t>(x) + static_cast<size_t>(y) * width + static_cast<size_t>(z) * plane;

  return imgTypeDispatcher(stack.info(), [&]<typename TVoxel>() -> double {
    const TVoxel* data = stack.timeData<TVoxel>(0);
    double sum = static_cast<double>(data[centerIdx]);
    int nInBound = 0;

    for (size_t i = 0; i < nb.size(); ++i) {
      const auto& o = nb.offset(i);
      const int nx = x + o.x;
      const int ny = y + o.y;
      const int nz = z + o.z;
      if (nx < 0 || ny < 0 || nz < 0) {
        continue;
      }
      if (static_cast<size_t>(nx) >= width || static_cast<size_t>(ny) >= height || static_cast<size_t>(nz) >= depth) {
        continue;
      }

      const size_t idx = static_cast<size_t>(nx) + static_cast<size_t>(ny) * width + static_cast<size_t>(nz) * plane;
      sum += static_cast<double>(data[idx]);
      ++nInBound;
    }

    return sum / static_cast<double>(nInBound + 1);
  });
}

[[nodiscard]] double stackNeighborMinLegacyLike(const ZImg& stack, const ZNeighborhood& nb, int x, int y, int z)
{
  const size_t width = stack.width();
  const size_t height = stack.height();
  const size_t depth = stack.depth();
  const size_t plane = width * height;

  const size_t centerIdx = static_cast<size_t>(x) + static_cast<size_t>(y) * width + static_cast<size_t>(z) * plane;

  return imgTypeDispatcher(stack.info(), [&]<typename TVoxel>() -> double {
    const TVoxel* data = stack.timeData<TVoxel>(0);
    double minV = static_cast<double>(data[centerIdx]);

    for (size_t i = 0; i < nb.size(); ++i) {
      const auto& o = nb.offset(i);
      const int nx = x + o.x;
      const int ny = y + o.y;
      const int nz = z + o.z;
      if (nx < 0 || ny < 0 || nz < 0) {
        continue;
      }
      if (static_cast<size_t>(nx) >= width || static_cast<size_t>(ny) >= height || static_cast<size_t>(nz) >= depth) {
        continue;
      }

      const size_t idx = static_cast<size_t>(nx) + static_cast<size_t>(ny) * width + static_cast<size_t>(nz) * plane;
      const double v = static_cast<double>(data[idx]);
      if (v < minV) {
        minV = v;
      }
    }

    return minV;
  });
}

} // namespace

void localNeurosegLabelGLegacyLike(const LocalNeuroseg& seg, ZImg& stack, int flag, int value, double zToXYRatio)
{
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  if (stack.isEmpty()) {
    return;
  }

  if ((seg.seg.r1 == 0.0) || (seg.seg.scale == 0.0)) {
    return;
  }

  const size_t width = stack.width();
  const size_t height = stack.height();
  const size_t depth = stack.depth();

  const std::array<double, 3> bottom = localNeurosegBottomLegacyLike(seg);

  std::array<int, 3> c = {0, 0, 0};
  std::array<double, 3> offpos = {0.0, 0.0, 0.0};

  c[0] = iroundLegacyLike(bottom[0]);
  c[1] = iroundLegacyLike(bottom[1]);
  offpos[0] = bottom[0] - static_cast<double>(c[0]);
  offpos[1] = bottom[1] - static_cast<double>(c[1]);

  const bool needZScale = (testZToXYRatioLegacyLike(zToXYRatio) != 0);
  if (needZScale) {
    c[2] = iroundLegacyLike(bottom[2] / zToXYRatio);
    offpos[2] = bottom[2] / zToXYRatio - static_cast<double>(c[2]);
  } else {
    c[2] = iroundLegacyLike(bottom[2]);
    offpos[2] = bottom[2] - static_cast<double>(c[2]);
  }

  const FieldRangeLegacyLike range = neurosegFieldRangeLegacyLike(seg.seg, zToXYRatio);

  const std::array<int, 3> regionCorner = {range.firstCorner[0] + c[0],
                                           range.firstCorner[1] + c[1],
                                           range.firstCorner[2] + c[2]};

  const double coef = seg.seg.c;

  // Avoid per-voxel trigonometric function calls by hoisting cos/sin outside the
  // inner loops, and dispatch voxel type once per call.
  const size_t plane = width * height;

  const int x0 = range.firstCorner[0];
  const int y0 = range.firstCorner[1];
  const int z0 = range.firstCorner[2];

  const double theta = seg.seg.theta;
  const double psi = seg.seg.psi;
  const double cosTheta = std::cos(theta);
  const double sinTheta = std::sin(theta);
  const double cosPsi = std::cos(psi);
  const double sinPsi = std::sin(psi);

  const double alpha = seg.seg.alpha;
  const double cosAlpha = std::cos(alpha);
  const double sinAlpha = std::sin(alpha);
  const double scale = seg.seg.scale;

  const double zMin = -0.5;
  const double zMax = seg.seg.h - 0.5;

  const bool clampUint8 = stack.isType<uint8_t>() && flag < 0;

  imgTypeDispatcher(stack.info(), [&]<typename TVoxel>() {
    auto* stackData = stack.timeData<TVoxel>(0);

    TVoxel writeValue = static_cast<TVoxel>(value);
    if (clampUint8) {
      writeValue = static_cast<TVoxel>(std::clamp(value, 0, 255));
    }

    for (int k = 0; k < range.size[2]; ++k) {
      const int pz = regionCorner[2] + k;
      for (int j = 0; j < range.size[1]; ++j) {
        const int py = regionCorner[1] + j;
        for (int i = 0; i < range.size[0]; ++i) {
          const int px = regionCorner[0] + i;

          if ((px < 0) || (py < 0) || (pz < 0) || (static_cast<size_t>(px) >= width) ||
              (static_cast<size_t>(py) >= height) || (static_cast<size_t>(pz) >= depth)) {
            continue;
          }

          double coord0 = static_cast<double>(i + x0);
          double coord1 = static_cast<double>(j + y0);
          double coord2 = static_cast<double>(k + z0);

          if (needZScale) {
            coord2 *= zToXYRatio;
          }

          coord0 -= offpos[0];
          coord1 -= offpos[1];
          coord2 -= offpos[2];

          // rotateXZLegacyLike(coord, theta, psi, inverse=1)
          const double inx = coord0;
          const double iny = coord1;
          const double inz = coord2;
          double result0 = cosPsi * inx + sinPsi * iny;
          double result1 = iny * cosPsi - inx * sinPsi;
          double result2 = inz * cosTheta - result1 * sinTheta;
          result1 = inz * sinTheta + result1 * cosTheta;

          // scaleXRotateZLegacyLike(coord, scale, alpha, inverse=1)
          const double preX = result0;
          const double preY = result1;
          const double tmp = (preX * cosAlpha + preY * sinAlpha) / scale;
          result1 = -preX * sinAlpha + preY * cosAlpha;
          result0 = tmp;

          const double f = neurofield7LegacyLike(coef, seg.seg.r1, result0, result1, result2, zMin, zMax);
          if (f <= 0.0) {
            continue;
          }

          const size_t idx =
            static_cast<size_t>(px) + static_cast<size_t>(py) * width + static_cast<size_t>(pz) * plane;

          if (flag >= 0) {
            const int current = static_cast<int>(stackData[idx]);
            if (current != flag) {
              continue;
            }
          }

          stackData[idx] = writeValue;
        }
      }
    }
  });
}

void localNeurosegLabelWLegacyLike(const LocalNeuroseg& seg,
                                   ZImg& stack,
                                   double zToXYRatio,
                                   LocsegLabelWorkspaceLegacyLike& ws)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_Label_W().
  if (stack.isEmpty()) {
    return;
  }

  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  const std::array<double, 3> bottom = localNeurosegBottomLegacyLike(seg);

  std::array<int, 3> c = {0, 0, 0};
  std::array<double, 3> offpos = {0.0, 0.0, 0.0};
  localNeurosegStackPositionLegacyLike(bottom, c, offpos, zToXYRatio);

  LocalNeuroseg labelLocseg = seg;
  if (ws.option == 10) {
    labelLocseg.seg.r1 = 0.75;
    labelLocseg.seg.scale = 1.0;
    labelLocseg.seg.c = 0.0;
  } else {
    neurosegSwellLegacyLike(labelLocseg.seg, ws.sratio, ws.sdiff, ws.slimit);
  }

  const FieldRangeLegacyLike range = neurosegFieldRangeLegacyLike(labelLocseg.seg, zToXYRatio);

  CHECK(range.size[0] >= 0);
  CHECK(range.size[1] >= 0);
  CHECK(range.size[2] >= 0);
  const size_t filterSize =
    static_cast<size_t>(range.size[0]) * static_cast<size_t>(range.size[1]) * static_cast<size_t>(range.size[2]);

  auto& filter = ws.filterScratch;
  if (ws.option > 10) {
    CHECK(false) << "localNeurosegLabelWLegacyLike: option > 10 is not supported yet: " << ws.option;
  } else {
    neurosegDistFilterLegacyLikeInto(labelLocseg.seg, range, &offpos, zToXYRatio, filter);
  }
  CHECK(filter.size() >= filterSize);

  auto& filter2 = ws.filter2Scratch;
  double threshold = 0.0;
  if (ws.option >= 2 && ws.option <= 4) {
    CHECK(ws.signal != nullptr) << "localNeurosegLabelWLegacyLike: option " << ws.option
                                << " requires ws.signal (input stack)";
    CHECK(ws.signal->numChannels() == 1);
    CHECK(ws.signal->numTimes() == 1);
    CHECK(ws.signal->width() == stack.width());
    CHECK(ws.signal->height() == stack.height());
    CHECK(ws.signal->depth() == stack.depth());

    StackFitScore fs{};
    fs.n = 1;
    fs.options[0] = static_cast<int>(StackFitOption::LowMeanSignal);
    (void)localNeurosegScorePLegacyLike(seg, *ws.signal, zToXYRatio, &fs);
    threshold = fs.scores[0];

    neurosegDistFilterLegacyLikeInto(seg.seg, range, &offpos, zToXYRatio, filter2);
    CHECK(filter2.size() >= filterSize);
  }

  const std::array<int, 3> regionCorner = {range.firstCorner[0] + c[0],
                                           range.firstCorner[1] + c[1],
                                           range.firstCorner[2] + c[2]};

  for (int i = 0; i < 3; ++i) {
    if (ws.range[static_cast<size_t>(i)] < 0) {
      ws.range[static_cast<size_t>(i)] = regionCorner[static_cast<size_t>(i)];
    } else if (ws.range[static_cast<size_t>(i)] > regionCorner[static_cast<size_t>(i)]) {
      ws.range[static_cast<size_t>(i)] = regionCorner[static_cast<size_t>(i)];
    }
  }

  for (int i = 0; i < 3; ++i) {
    const int end = regionCorner[static_cast<size_t>(i)] + range.size[static_cast<size_t>(i)] - 1;
    if (ws.range[static_cast<size_t>(i) + 3] < 0) {
      ws.range[static_cast<size_t>(i) + 3] = end;
    } else if (ws.range[static_cast<size_t>(i) + 3] < end) {
      ws.range[static_cast<size_t>(i) + 3] = end;
    }
  }

  for (size_t i = 0; i < ws.range.size(); ++i) {
    if (ws.range[i] < 0) {
      ws.range[i] = 0;
    }
  }

  // Legacy clamping (note the "== width/height/depth" behavior).
  const int width = static_cast<int>(stack.width());
  const int height = static_cast<int>(stack.height());
  const int depth = static_cast<int>(stack.depth());

  if (ws.range[0] >= width) {
    ws.range[0] = width;
  }
  if (ws.range[3] >= width) {
    ws.range[3] = width;
  }

  if (ws.range[1] >= height) {
    ws.range[1] = height;
  }
  if (ws.range[4] >= height) {
    ws.range[4] = height;
  }

  if (ws.range[2] >= depth) {
    ws.range[2] = depth;
  }
  if (ws.range[5] >= depth) {
    ws.range[5] = depth;
  }

  const ZNeighborhood& conn18 = neighborhoodLegacyOrder(18);
  const bool clampUint8 = stack.isType<uint8_t>();
  const size_t widthS = stack.width();
  const size_t plane = widthS * stack.height();

  size_t offset = 0;
  imgTypeDispatcher(stack.info(), [&]<typename TVoxel>() {
    auto* stackData = stack.timeData<TVoxel>(0);
    TVoxel writeValue = static_cast<TVoxel>(ws.value);
    if (clampUint8) {
      writeValue = static_cast<TVoxel>(std::clamp(ws.value, 0, 255));
    }

    for (int k = 0; k < range.size[2]; ++k) {
      const int pz = regionCorner[2] + k;
      for (int j = 0; j < range.size[1]; ++j) {
        const int py = regionCorner[1] + j;
        for (int i = 0; i < range.size[0]; ++i) {
          const int px = regionCorner[0] + i;

          if (px >= 0 && py >= 0 && pz >= 0 && px < width && py < height && pz < depth) {
            const size_t idx =
              static_cast<size_t>(px) + static_cast<size_t>(py) * widthS + static_cast<size_t>(pz) * plane;

            bool label = true;
            if (ws.flag >= 0) {
              const int current = static_cast<int>(stackData[idx]);
              if (current != ws.flag) {
                label = false;
              }
            }

            if (label) {
              switch (ws.option) {
                case 1:
                case 10:
                  if (filter[offset] <= 1.0) {
                    stackData[idx] = writeValue;
                  }
                  break;
                case 2:
                  if (filter2[offset] <= 1.0) {
                    stackData[idx] = writeValue;
                  } else if (filter[offset] <= 1.0) {
                    const double v = imgTypeDispatcher(ws.signal->info(), [&]<typename TSignalVoxel>() -> double {
                      return static_cast<double>(*ws.signal->data<TSignalVoxel>(static_cast<size_t>(px),
                                                                                static_cast<size_t>(py),
                                                                                static_cast<size_t>(pz)));
                    });
                    if (v < threshold) {
                      stackData[idx] = writeValue;
                    }
                  }
                  break;
                case 3:
                  if (filter2[offset] <= 1.0) {
                    stackData[idx] = writeValue;
                  } else if (filter[offset] <= 1.0) {
                    if (stackNeighborMeanLegacyLike(*ws.signal, conn18, px, py, pz) < threshold) {
                      stackData[idx] = writeValue;
                    }
                  }
                  break;
                case 4:
                  if (filter2[offset] <= 1.0) {
                    stackData[idx] = writeValue;
                  } else if (filter[offset] <= 1.0) {
                    if (stackNeighborMinLegacyLike(*ws.signal, conn18, px, py, pz) < threshold) {
                      stackData[idx] = writeValue;
                    }
                  }
                  break;
                default:
                  CHECK(false) << "localNeurosegLabelWLegacyLike: unsupported option: " << ws.option;
                  break;
              }
            }
          }

          ++offset;
        }
      }
    }
  });

  CHECK(offset == filterSize);
}

void locsegChainLabelWLegacyLike(const LocsegChain& chain,
                                 ZImg& stack,
                                 double zToXYRatio,
                                 int begin,
                                 int end,
                                 LocsegLabelWorkspaceLegacyLike& ws)
{
  if (end < 0) {
    end += chain.length();
  }

  if (ws.option == 6 || ws.option == 7) {
    // Port of tz_locseg_chain.c::Locseg_Chain_Label_W() option 6/7 path.
    if (ws.bufferMask) {
      if (ws.bufferMask->voxelNumber() != stack.voxelNumber() || ws.bufferMask->width() != stack.width() ||
          ws.bufferMask->height() != stack.height() || ws.bufferMask->depth() != stack.depth()) {
        ws.bufferMask.reset();
      }
    }

    if (!ws.bufferMask) {
      ZImgInfo info = stack.info();
      info.setVoxelFormat<uint8_t>();
      info.createDefaultDescriptions();
      ws.bufferMask = std::make_unique<ZImg>(info);
    }

    ws.bufferMask->fill(0);

    LocsegLabelWorkspaceLegacyLike tmpWs;
    tmpWs.signal = ws.signal;
    tmpWs.option = 1;
    tmpWs.value = 1;
    tmpWs.flag = 0;
    tmpWs.sratio = ws.sratio;
    tmpWs.sdiff = ws.sdiff;
    tmpWs.slimit = ws.slimit;

    locsegChainLabelWLegacyLike(chain, *ws.bufferMask, zToXYRatio, begin, end, tmpWs);

    if (ws.option == 6) {
      stack += *ws.bufferMask;
    } else {
      stack -= *ws.bufferMask;
    }
    return;
  }

  int i = 0;
  for (const auto& node : chain) {
    if (i >= begin && i <= end) {
      localNeurosegLabelWLegacyLike(node.locseg, stack, zToXYRatio, ws);
    }
    ++i;
  }
}

} // namespace nim
