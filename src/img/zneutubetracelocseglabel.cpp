#include "zneutubetracelocseglabel.h"

#include "zneutube3dgeom.h"
#include "zneutubelocsegchain.h"
#include "zneutubeneighborhood.h"
#include "zneutubestackfitoptions.h"
#include "zneutubetraceswclabelstack.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nim {

namespace {

[[nodiscard]] int iroundLegacyLike(double x)
{
  return static_cast<int>(std::lround(x));
}

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

[[nodiscard]] int testZScaleLegacyLike(double zScale)
{
  return compareFloatLegacyLike(zScale, 1.0, 1e-5);
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

[[nodiscard]] int readVoxelAsIntLegacyLike(const ZImg& stack, size_t x, size_t y, size_t z)
{
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  if (stack.isType<uint8_t>()) {
    return static_cast<int>(*stack.data<uint8_t>(x, y, z));
  }
  if (stack.isType<uint16_t>()) {
    return static_cast<int>(*stack.data<uint16_t>(x, y, z));
  }

  CHECK(false) << "Unsupported stack voxel type: " << stack.info();
  return 0;
}

void writeVoxelFromIntLegacyLike(ZImg& stack, size_t x, size_t y, size_t z, int value)
{
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  if (stack.isType<uint8_t>()) {
    *stack.data<uint8_t>(x, y, z) = static_cast<uint8_t>(value);
    return;
  }
  if (stack.isType<uint16_t>()) {
    *stack.data<uint16_t>(x, y, z) = static_cast<uint16_t>(value);
    return;
  }

  CHECK(false) << "Unsupported stack voxel type: " << stack.info();
}

[[nodiscard]] int clampToStackTypeLegacyLike(const ZImg& stack, int value)
{
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  if (stack.isType<uint8_t>()) {
    return std::clamp(value, 0, 255);
  }
  if (stack.isType<uint16_t>()) {
    return std::clamp(value, 0, 65535);
  }

  CHECK(false) << "Unsupported stack voxel type: " << stack.info();
  return 0;
}

void stackAddRLegacyLike(const ZImg& stack1, const ZImg& stack2, const std::array<int, 6>& range, ZImg& out)
{
  CHECK(stack1.numChannels() == 1 && stack1.numTimes() == 1);
  CHECK(stack2.numChannels() == 1 && stack2.numTimes() == 1);
  CHECK(out.numChannels() == 1 && out.numTimes() == 1);
  CHECK(stack1.width() == stack2.width() && stack1.height() == stack2.height() && stack1.depth() == stack2.depth());
  CHECK(stack1.width() == out.width() && stack1.height() == out.height() && stack1.depth() == out.depth());

  for (int k = range[2]; k <= range[5]; ++k) {
    for (int j = range[1]; j <= range[4]; ++j) {
      for (int i = range[0]; i <= range[3]; ++i) {
        const int v1 =
          readVoxelAsIntLegacyLike(stack1, static_cast<size_t>(i), static_cast<size_t>(j), static_cast<size_t>(k));
        const int v2 =
          readVoxelAsIntLegacyLike(stack2, static_cast<size_t>(i), static_cast<size_t>(j), static_cast<size_t>(k));
        const int res = clampToStackTypeLegacyLike(out, v1 + v2);
        writeVoxelFromIntLegacyLike(out, static_cast<size_t>(i), static_cast<size_t>(j), static_cast<size_t>(k), res);
      }
    }
  }
}

void stackSubRLegacyLike(const ZImg& stack1, const ZImg& stack2, const std::array<int, 6>& range, ZImg& out)
{
  CHECK(stack1.numChannels() == 1 && stack1.numTimes() == 1);
  CHECK(stack2.numChannels() == 1 && stack2.numTimes() == 1);
  CHECK(out.numChannels() == 1 && out.numTimes() == 1);
  CHECK(stack1.width() == stack2.width() && stack1.height() == stack2.height() && stack1.depth() == stack2.depth());
  CHECK(stack1.width() == out.width() && stack1.height() == out.height() && stack1.depth() == out.depth());

  for (int k = range[2]; k <= range[5]; ++k) {
    for (int j = range[1]; j <= range[4]; ++j) {
      for (int i = range[0]; i <= range[3]; ++i) {
        const int v1 =
          readVoxelAsIntLegacyLike(stack1, static_cast<size_t>(i), static_cast<size_t>(j), static_cast<size_t>(k));
        const int v2 =
          readVoxelAsIntLegacyLike(stack2, static_cast<size_t>(i), static_cast<size_t>(j), static_cast<size_t>(k));
        const int res = clampToStackTypeLegacyLike(out, v1 - v2);
        writeVoxelFromIntLegacyLike(out, static_cast<size_t>(i), static_cast<size_t>(j), static_cast<size_t>(k), res);
      }
    }
  }
}

[[nodiscard]] double readVoxelAsDoubleLegacyLike(const ZImg& stack, size_t x, size_t y, size_t z)
{
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  if (stack.isType<uint8_t>()) {
    return static_cast<double>(*stack.data<uint8_t>(x, y, z));
  }
  if (stack.isType<uint16_t>()) {
    return static_cast<double>(*stack.data<uint16_t>(x, y, z));
  }

  CHECK(false) << "Unsupported stack voxel type: " << stack.info();
  return 0.0;
}

[[nodiscard]] double stackNeighborMeanLegacyLike(const ZImg& stack, int connectivity, int x, int y, int z)
{
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  if (x < 0 || y < 0 || z < 0) {
    return 0.0;
  }

  const size_t width = stack.width();
  const size_t height = stack.height();
  const size_t depth = stack.depth();

  if (static_cast<size_t>(x) >= width || static_cast<size_t>(y) >= height || static_cast<size_t>(z) >= depth) {
    return 0.0;
  }

  const ZNeighborhood& nb = neighborhoodLegacyOrder(connectivity);

  double sum =
    readVoxelAsDoubleLegacyLike(stack, static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z));
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

    sum +=
      readVoxelAsDoubleLegacyLike(stack, static_cast<size_t>(nx), static_cast<size_t>(ny), static_cast<size_t>(nz));
    ++nInBound;
  }

  return sum / static_cast<double>(nInBound + 1);
}

[[nodiscard]] double stackNeighborMinLegacyLike(const ZImg& stack, int connectivity, int x, int y, int z)
{
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  if (x < 0 || y < 0 || z < 0) {
    return 0.0;
  }

  const size_t width = stack.width();
  const size_t height = stack.height();
  const size_t depth = stack.depth();

  if (static_cast<size_t>(x) >= width || static_cast<size_t>(y) >= height || static_cast<size_t>(z) >= depth) {
    return 0.0;
  }

  const ZNeighborhood& nb = neighborhoodLegacyOrder(connectivity);

  double minV =
    readVoxelAsDoubleLegacyLike(stack, static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z));
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

    const double v =
      readVoxelAsDoubleLegacyLike(stack, static_cast<size_t>(nx), static_cast<size_t>(ny), static_cast<size_t>(nz));
    if (v < minV) {
      minV = v;
    }
  }

  return minV;
}

} // namespace

void localNeurosegLabelGLegacyLike(const LocalNeuroseg& seg, ZImg& stack, int flag, int value, double zScale)
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

  c[0] = static_cast<int>(std::lrint(bottom[0]));
  c[1] = static_cast<int>(std::lrint(bottom[1]));
  offpos[0] = bottom[0] - static_cast<double>(c[0]);
  offpos[1] = bottom[1] - static_cast<double>(c[1]);

  if (testZScaleLegacyLike(zScale) != 0) {
    c[2] = static_cast<int>(std::lrint(bottom[2] * zScale));
    offpos[2] = bottom[2] * zScale - static_cast<double>(c[2]);
  } else {
    c[2] = static_cast<int>(std::lrint(bottom[2]));
    offpos[2] = bottom[2] - static_cast<double>(c[2]);
  }

  const FieldRangeLegacyLike range = neurosegFieldRangeLegacyLike(seg.seg, zScale);

  const std::array<int, 3> regionCorner = {range.firstCorner[0] + c[0],
                                           range.firstCorner[1] + c[1],
                                           range.firstCorner[2] + c[2]};

  const double coef = seg.seg.c;

  for (int k = 0; k < range.size[2]; ++k) {
    const int pz = regionCorner[2] + k;
    for (int j = 0; j < range.size[1]; ++j) {
      const int py = regionCorner[1] + j;
      for (int i = 0; i < range.size[0]; ++i) {
        const int px = regionCorner[0] + i;

        std::array<double, 3> coord = {static_cast<double>(i + range.firstCorner[0]),
                                       static_cast<double>(j + range.firstCorner[1]),
                                       static_cast<double>(k + range.firstCorner[2])};

        if (testZScaleLegacyLike(zScale) != 0) {
          coord[2] /= zScale;
        }

        coord[0] -= offpos[0];
        coord[1] -= offpos[1];
        coord[2] -= offpos[2];

        rotateXZLegacyLike(&coord, 1, seg.seg.theta, seg.seg.psi, 1);
        scaleXRotateZLegacyLike(&coord, seg.seg.scale, seg.seg.alpha, 1);

        const double f = neurofield7LegacyLike(coef,
                                               seg.seg.r1,
                                               coord[0],
                                               coord[1],
                                               coord[2],
                                               /*zMin*/ -0.5,
                                               /*zMax*/ seg.seg.h - 0.5);
        if (f <= 0.0) {
          continue;
        }

        if ((px < 0) || (py < 0) || (pz < 0) || (static_cast<size_t>(px) >= width) ||
            (static_cast<size_t>(py) >= height) || (static_cast<size_t>(pz) >= depth)) {
          continue;
        }

        if (flag >= 0) {
          if (readVoxelAsIntLegacyLike(stack,
                                       static_cast<size_t>(px),
                                       static_cast<size_t>(py),
                                       static_cast<size_t>(pz)) != flag) {
            continue;
          }
        }

        writeVoxelFromIntLegacyLike(stack,
                                    static_cast<size_t>(px),
                                    static_cast<size_t>(py),
                                    static_cast<size_t>(pz),
                                    value);
      }
    }
  }
}

void localNeurosegLabelWLegacyLike(const LocalNeuroseg& seg,
                                   ZImg& stack,
                                   double zScale,
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
  localNeurosegStackPositionLegacyLike(bottom, c, offpos, zScale);

  LocalNeuroseg labelLocseg = seg;
  if (ws.option == 10) {
    labelLocseg.seg.r1 = 0.75;
    labelLocseg.seg.scale = 1.0;
    labelLocseg.seg.c = 0.0;
  } else {
    neurosegSwellLegacyLike(labelLocseg.seg, ws.sratio, ws.sdiff, ws.slimit);
  }

  const FieldRangeLegacyLike range = neurosegFieldRangeLegacyLike(labelLocseg.seg, zScale);

  std::vector<double> filter;
  if (ws.option > 10) {
    CHECK(false) << "localNeurosegLabelWLegacyLike: option > 10 is not supported yet: " << ws.option;
  } else {
    filter = neurosegDistFilterLegacyLike(labelLocseg.seg, range, &offpos, zScale);
  }

  std::vector<double> filter2;
  double threshold = 0.0;
  if (ws.option >= 2 && ws.option <= 4) {
    CHECK(ws.signal != nullptr) << "localNeurosegLabelWLegacyLike: option " << ws.option
                                << " requires ws.signal (input stack)";

    StackFitScore fs{};
    fs.n = 1;
    fs.options[0] = static_cast<int>(StackFitOption::LowMeanSignal);
    (void)localNeurosegScorePLegacyLike(seg, *ws.signal, zScale, &fs);
    threshold = fs.scores[0];

    filter2 = neurosegDistFilterLegacyLike(seg.seg, range, &offpos, zScale);
    CHECK(filter2.size() == filter.size());
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

  size_t offset = 0;
  for (int k = 0; k < range.size[2]; ++k) {
    const int pz = regionCorner[2] + k;
    for (int j = 0; j < range.size[1]; ++j) {
      const int py = regionCorner[1] + j;
      for (int i = 0; i < range.size[0]; ++i) {
        const int px = regionCorner[0] + i;

        if (px >= 0 && py >= 0 && pz >= 0 && px < width && py < height && pz < depth) {
          bool label = true;
          if (ws.flag >= 0) {
            const int current = readVoxelAsIntLegacyLike(stack,
                                                         static_cast<size_t>(px),
                                                         static_cast<size_t>(py),
                                                         static_cast<size_t>(pz));
            if (iroundLegacyLike(static_cast<double>(current)) != ws.flag) {
              label = false;
            }
          }

          if (label) {
            switch (ws.option) {
              case 1:
              case 10:
                if (filter[offset] <= 1.0) {
                  writeVoxelFromIntLegacyLike(stack,
                                              static_cast<size_t>(px),
                                              static_cast<size_t>(py),
                                              static_cast<size_t>(pz),
                                              ws.value);
                }
                break;
              case 2:
                if (filter2[offset] <= 1.0) {
                  writeVoxelFromIntLegacyLike(stack,
                                              static_cast<size_t>(px),
                                              static_cast<size_t>(py),
                                              static_cast<size_t>(pz),
                                              ws.value);
                } else if (filter[offset] <= 1.0) {
                  if (readVoxelAsDoubleLegacyLike(*ws.signal,
                                                  static_cast<size_t>(px),
                                                  static_cast<size_t>(py),
                                                  static_cast<size_t>(pz)) < threshold) {
                    writeVoxelFromIntLegacyLike(stack,
                                                static_cast<size_t>(px),
                                                static_cast<size_t>(py),
                                                static_cast<size_t>(pz),
                                                ws.value);
                  }
                }
                break;
              case 3:
                if (filter2[offset] <= 1.0) {
                  writeVoxelFromIntLegacyLike(stack,
                                              static_cast<size_t>(px),
                                              static_cast<size_t>(py),
                                              static_cast<size_t>(pz),
                                              ws.value);
                } else if (filter[offset] <= 1.0) {
                  if (stackNeighborMeanLegacyLike(*ws.signal, /*connectivity*/ 18, px, py, pz) < threshold) {
                    writeVoxelFromIntLegacyLike(stack,
                                                static_cast<size_t>(px),
                                                static_cast<size_t>(py),
                                                static_cast<size_t>(pz),
                                                ws.value);
                  }
                }
                break;
              case 4:
                if (filter2[offset] <= 1.0) {
                  writeVoxelFromIntLegacyLike(stack,
                                              static_cast<size_t>(px),
                                              static_cast<size_t>(py),
                                              static_cast<size_t>(pz),
                                              ws.value);
                } else if (filter[offset] <= 1.0) {
                  if (stackNeighborMinLegacyLike(*ws.signal, /*connectivity*/ 18, px, py, pz) < threshold) {
                    writeVoxelFromIntLegacyLike(stack,
                                                static_cast<size_t>(px),
                                                static_cast<size_t>(py),
                                                static_cast<size_t>(pz),
                                                ws.value);
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

  CHECK(offset == filter.size());
}

void locsegChainLabelWLegacyLike(const LocsegChain& chain,
                                 ZImg& stack,
                                 double zScale,
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
    tmpWs.range = {-1, -1, -1, -1, -1, -1};

    locsegChainLabelWLegacyLike(chain, *ws.bufferMask, zScale, begin, end, tmpWs);

    for (int i = 0; i < 3; ++i) {
      if (tmpWs.range[static_cast<size_t>(i)] < 0) {
        tmpWs.range[static_cast<size_t>(i)] = 0;
      }
    }

    const int width = static_cast<int>(stack.width());
    const int height = static_cast<int>(stack.height());
    const int depth = static_cast<int>(stack.depth());

    if (tmpWs.range[3] < 0 || tmpWs.range[3] >= width) {
      tmpWs.range[3] = width - 1;
    }
    if (tmpWs.range[4] < 0 || tmpWs.range[4] >= height) {
      tmpWs.range[4] = height - 1;
    }
    if (tmpWs.range[5] < 0 || tmpWs.range[5] >= depth) {
      tmpWs.range[5] = depth - 1;
    }

    if (ws.option == 6) {
      stackAddRLegacyLike(stack, *ws.bufferMask, tmpWs.range, stack);
    } else {
      stackSubRLegacyLike(stack, *ws.bufferMask, tmpWs.range, stack);
    }

    // Clean the buffer mask.
    tmpWs.option = 1;
    tmpWs.value = 0;
    tmpWs.flag = 1;
    locsegChainLabelWLegacyLike(chain, *ws.bufferMask, zScale, begin, end, tmpWs);
    return;
  }

  int i = 0;
  for (const auto& node : chain) {
    if (i >= begin && i <= end) {
      localNeurosegLabelWLegacyLike(node.locseg, stack, zScale, ws);
    }
    ++i;
  }
}

} // namespace nim
