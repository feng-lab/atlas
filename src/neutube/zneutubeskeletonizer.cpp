#include "zneutubeskeletonizer.h"

#include "zneutubeimgbwmorph.h"
#include "zneutubeobjlabel.h"
#include "zneutubeplanaredt.h"
#include "zneutubespgrow.h"
#include "zneutubespgrowparser.h"
#include "zneutubeswcreconnect.h"
#include "zneutubeswcregionsampling.h"
#include "zneutubeswcops.h"
#include "zneutubeswcresampler.h"

#include "zexception.h"
#include "zlog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace nim::neutube {

namespace {

[[nodiscard]] bool hasAnyForegroundVoxel(const ZImg& mask)
{
  CHECK(mask.numChannels() == 1);
  CHECK(mask.numTimes() == 1);
  CHECK(mask.isType<uint8_t>());

  const size_t n = mask.voxelNumber();
  const auto* data = mask.timeData<uint8_t>(0);
  for (size_t i = 0; i < n; ++i) {
    if (data[i] > 0) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] double maxMaskIntensity(const ZImg& img)
{
  CHECK(img.numChannels() == 1);
  CHECK(img.numTimes() == 1);

  const size_t n = img.voxelNumber();
  if (n == 0) {
    return 0.0;
  }

  double maxV = 0.0;

  imgTypeDispatcher(img.info(), [&]<typename TVoxel>() {
    const auto* data = img.timeData<TVoxel>(0);
    TVoxel m = data[0];
    for (size_t i = 1; i < n; ++i) {
      if (data[i] > m) {
        m = data[i];
      }
    }
    maxV = static_cast<double>(m);
  });

  return maxV;
}

[[nodiscard]] size_t maxIndexU16First(const ZImg& img)
{
  CHECK(img.numChannels() == 1);
  CHECK(img.numTimes() == 1);
  CHECK(img.isType<uint16_t>()) << img.info();

  const auto* data = img.timeData<uint16_t>(0);
  const size_t n = img.voxelNumber();
  CHECK(n > 0);

  size_t maxIndex = 0;
  uint16_t maxValue = data[0];
  for (size_t i = 1; i < n; ++i) {
    if (data[i] > maxValue) {
      maxValue = data[i];
      maxIndex = i;
    }
  }
  return maxIndex;
}

struct ObjectStat
{
  bool present = false;
  size_t size = 0;

  int minX = std::numeric_limits<int>::max();
  int minY = std::numeric_limits<int>::max();
  int minZ = std::numeric_limits<int>::max();

  int maxX = std::numeric_limits<int>::min();
  int maxY = std::numeric_limits<int>::min();
  int maxZ = std::numeric_limits<int>::min();
};

[[nodiscard]] int labelAtIndex(const ZImg& labels, size_t idx)
{
  CHECK(labels.numChannels() == 1);
  CHECK(labels.numTimes() == 1);

  if (labels.isType<uint8_t>()) {
    return static_cast<int>(labels.timeData<uint8_t>(0)[idx]);
  }
  if (labels.isType<uint16_t>()) {
    return static_cast<int>(labels.timeData<uint16_t>(0)[idx]);
  }

  CHECK(false) << "Unsupported label type: " << labels.info();
  return 0;
}

[[nodiscard]] std::vector<ObjectStat> computeObjectStatsLegacy(const ZImg& labels, size_t nobj)
{
  CHECK(labels.numChannels() == 1);
  CHECK(labels.numTimes() == 1);

  std::vector<ObjectStat> stats(nobj);

  const int width = static_cast<int>(labels.width());
  const int height = static_cast<int>(labels.height());
  const int depth = static_cast<int>(labels.depth());
  const int64_t area = static_cast<int64_t>(width) * static_cast<int64_t>(height);
  const int64_t voxelNumber = static_cast<int64_t>(labels.voxelNumber());

  for (int64_t idx = 0; idx < voxelNumber; ++idx) {
    const int label = labelAtIndex(labels, static_cast<size_t>(idx));
    if (label < 3) {
      continue;
    }

    const size_t objIndex = static_cast<size_t>(label - 3);
    CHECK(objIndex < nobj);

    ObjectStat& st = stats[objIndex];
    st.present = true;
    ++st.size;

    const int z = static_cast<int>(idx / area);
    const int64_t rem = idx - static_cast<int64_t>(z) * area;
    const int y = static_cast<int>(rem / width);
    const int x = static_cast<int>(rem - static_cast<int64_t>(y) * width);

    st.minX = std::min(st.minX, x);
    st.minY = std::min(st.minY, y);
    st.minZ = std::min(st.minZ, z);
    st.maxX = std::max(st.maxX, x);
    st.maxY = std::max(st.maxY, y);
    st.maxZ = std::max(st.maxZ, z);
  }

  for (size_t i = 0; i < nobj; ++i) {
    CHECK(stats[i].present) << "Missing stats for object " << i;
    CHECK(stats[i].size > 0);
    CHECK(stats[i].minX <= stats[i].maxX);
    CHECK(stats[i].minY <= stats[i].maxY);
    CHECK(stats[i].minZ <= stats[i].maxZ);
    CHECK(stats[i].minX >= 0 && stats[i].minX < width);
    CHECK(stats[i].minY >= 0 && stats[i].minY < height);
    CHECK(stats[i].minZ >= 0 && stats[i].minZ < depth);
  }

  return stats;
}

[[nodiscard]] bool hasOverlapCenters(const ZSwc::ConstSwcTreeNode& a, const ZSwc::ConstSwcTreeNode& b)
{
  const double dx = a->x - b->x;
  const double dy = a->y - b->y;
  const double dz = a->z - b->z;
  const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
  return d < (a->radius + b->radius);
}

[[nodiscard]] double adjustedDistanceWeight(double v)
{
  // Port of legacy `AdjustedDistanceWeight` in zstackskeletonizer.cpp:
  // return dmax2(0.1, sqrt(v) - 0.5);
  const double d = std::sqrt(v) - 0.5;
  return std::max(0.1, d);
}

void rescaleSwc(ZSwc* tree, double scaleX, double scaleY, double scaleZ, bool changingRadius)
{
  CHECK(tree != nullptr);

  const double dScale = std::sqrt(scaleX * scaleY);
  for (auto it = tree->begin(); it != tree->end(); ++it) {
    it->x *= scaleX;
    it->y *= scaleY;
    it->z *= scaleZ;
    if (changingRadius) {
      it->radius *= dScale;
    }
  }
}

} // namespace

ZNeutubeSkeletonizer::ZNeutubeSkeletonizer() = default;

void ZNeutubeSkeletonizer::setResolution(double xyRes, double zRes)
{
  m_resolution[0] = xyRes;
  m_resolution[1] = xyRes;
  m_resolution[2] = zRes;
}

void ZNeutubeSkeletonizer::setDownsampleInterval(int xintv, int yintv, int zintv)
{
  m_downsampleInterval[0] = std::max(0, xintv);
  m_downsampleInterval[1] = std::max(0, yintv);
  m_downsampleInterval[2] = std::max(0, zintv);
}

double ZNeutubeSkeletonizer::getLengthThreshold(double linScale) const
{
  return std::max(m_finalLengthThreshold, m_lengthThreshold / linScale);
}

std::unique_ptr<ZSwc> ZNeutubeSkeletonizer::makeSkeleton(const ZImg& img) const
{
  if (img.isEmpty()) {
    return {};
  }

  if (img.numChannels() != 1 || img.numTimes() != 1) {
    throw ZException(fmt::format("Skeletonize expects 1C1T image, got {}", img.info()));
  }

  const std::array<int, 3> dsIntv = m_downsampleInterval;

  const size_t dsX = static_cast<size_t>(dsIntv[0]) + 1;
  const size_t dsY = static_cast<size_t>(dsIntv[1]) + 1;
  const size_t dsZ = static_cast<size_t>(dsIntv[2]) + 1;

  // Legacy uses Downsample_Stack_Max() before skeletonization (even for binary input),
  // and then rescales the resulting SWC back to the original voxel scale.
  ZImg imgDs = img.blockDownsampled(dsX, dsY, dsZ, ImgMergeMode::Max);
  if (imgDs.isEmpty()) {
    return {};
  }

  return makeSkeletonWithoutDs(std::move(imgDs), dsIntv);
}

std::unique_ptr<ZSwc> ZNeutubeSkeletonizer::makeSkeletonWithoutDs(ZImg img, const std::array<int, 3>& dsIntv) const
{
  if (img.isEmpty()) {
    return {};
  }
  CHECK(img.numChannels() == 1);
  CHECK(img.numTimes() == 1);

  const double maxIntensity = maxMaskIntensity(img);
  if (maxIntensity == 0.0) {
    LOG(INFO) << "Not a binary image. No skeleton generated.";
    return {};
  }

  // Legacy code expects a GREY binary stack at this stage (0/1 uint8).
  ZImg stackData = img.binarized();
  if (!hasAnyForegroundVoxel(stackData)) {
    return {};
  }

  if (m_removingBorder) {
    LOG(INFO) << "Remove 1-pixel gaps ...";
    stackData = stackNotBinaryU8(stackData);
    stackData = majorityFilterBinaryU8(stackData, 8);
    stackData = stackNotBinaryU8(stackData);
  }

  if (m_interpolating) {
    throw ZException("Skeletonize: interpolating=true is not ported yet.");
  }

  if (m_fillingHole) {
    LOG(INFO) << "Filling hole ...";
    stackData = fillHolesBinaryU8(stackData, 26);
  }

  const int dsVol = (dsIntv[0] + 1) * (dsIntv[1] + 1) * (dsIntv[2] + 1);
  CHECK(dsVol > 0);

  int minObjSize = m_minObjSize;
  minObjSize /= dsVol;

  LabelLargeObjectsParams labelParams;
  labelParams.flag = 1;
  labelParams.smallLabel = 2;
  labelParams.minSize = minObjSize;
  labelParams.connectivity = 26;

  const LabelLargeObjectsResult labeled = labelLargeObjectsLegacy(stackData, labelParams);
  const size_t nobj = labeled.numLargeObjects;

  if (nobj == 0) {
    LOG(INFO) << "No object found in the image. No skeleton generated.";
    return {};
  }

  if (nobj > 65533) {
    LOG(INFO) << "Too many objects ( > 65533). No skeleton generated.";
    return {};
  }

  const std::vector<ObjectStat> stats = computeObjectStatsLegacy(labeled.labels, nobj);

  ZSwc wholeTree;

  for (size_t objIndex = 0; objIndex < nobj; ++objIndex) {
    const ObjectStat& st = stats[objIndex];

    const int objectOffsetX = st.minX;
    const int objectOffsetY = st.minY;
    const int objectOffsetZ = st.minZ;

    const int boxW = st.maxX - st.minX + 1;
    const int boxH = st.maxY - st.minY + 1;
    const int boxD = st.maxZ - st.minZ + 1;

    CHECK(boxW > 0 && boxH > 0 && boxD > 0);

    const int dsX = dsIntv[0] + 1;
    const int dsY = dsIntv[1] + 1;
    const int dsZ = dsIntv[2] + 1;
    const int dsVolCheck = dsX * dsY * dsZ;
    CHECK(dsVolCheck == dsVol);

    double linScale = 1.0;
    if (dsIntv[0] == dsIntv[1] && dsIntv[1] == dsIntv[2]) {
      linScale = dsIntv[0] + 1;
    } else {
      linScale = std::cbrt(static_cast<double>(dsVol));
    }

    const double lengthThreshold = getLengthThreshold(linScale);

    ZSwc subtree;

    if (st.size == 1) {
      if (m_keepingSingleObject || lengthThreshold <= 1.0) {
        SwcNode node;
        node.id = 1;
        node.type = 0;
        node.x = objectOffsetX;
        node.y = objectOffsetY;
        node.z = objectOffsetZ;
        node.radius = 1.0;
        node.parentID = -1;
        subtree.appendRoot(node);
      }
    } else {
      ZImgInfo croppedInfo(static_cast<size_t>(boxW), static_cast<size_t>(boxH), static_cast<size_t>(boxD));
      croppedInfo.setVoxelFormat<uint8_t>();
      croppedInfo.createDefaultDescriptions();
      ZImg croppedObjMask(croppedInfo);
      croppedObjMask.fill<uint8_t>(0);

      const int width = static_cast<int>(labeled.labels.width());
      const int height = static_cast<int>(labeled.labels.height());
      const int64_t area = static_cast<int64_t>(width) * static_cast<int64_t>(height);
      const int objectLabel = static_cast<int>(3 + objIndex);

      uint8_t* outMask = croppedObjMask.timeData<uint8_t>(0);
      for (int z = st.minZ; z <= st.maxZ; ++z) {
        for (int y = st.minY; y <= st.maxY; ++y) {
          const int64_t base = static_cast<int64_t>(z) * area + static_cast<int64_t>(y) * width;
          for (int x = st.minX; x <= st.maxX; ++x) {
            const size_t idx = static_cast<size_t>(base + x);
            const int lbl = labelAtIndex(labeled.labels, idx);
            if (lbl == objectLabel) {
              const int cx = x - st.minX;
              const int cy = y - st.minY;
              const int cz = z - st.minZ;
              const size_t cidx = static_cast<size_t>(cx) + static_cast<size_t>(cy) * static_cast<size_t>(boxW) +
                                  static_cast<size_t>(cz) * static_cast<size_t>(boxW) * static_cast<size_t>(boxH);
              outMask[cidx] = 1;
            }
          }
        }
      }

      ZImg tmpdist = planarBwdistSquaredU16P(croppedObjMask);
      const size_t maxIndex = maxIndexU16First(tmpdist);

      SpGrowWorkspace sgw;
      sgw.lengthBufferEnabled = true;
      sgw.resolution = m_resolution;
      sgw.conn = 26;

      const size_t nvoxel = croppedObjMask.voxelNumber();
      sgw.mask.assign(nvoxel, 0);
      const uint8_t* maskData = croppedObjMask.timeData<uint8_t>(0);
      for (size_t i = 0; i < nvoxel; ++i) {
        if (maskData[i] == 0) {
          sgw.mask[i] = SP_GROW_BARRIER;
        }
      }
      sgw.mask[maxIndex] = SP_GROW_SOURCE;

      stackSpGrow(tmpdist, &sgw);

      ZNeutubeSpGrowParser parser(&sgw);

      if (m_rebase) {
        LOG(INFO) << "Replacing start point ...";
        ZNeutubeVoxelArray path = parser.extractLongestPath(nullptr, false);

        for (size_t i = 0; i < nvoxel; ++i) {
          if (sgw.mask[i] != SP_GROW_BARRIER) {
            sgw.mask[i] = 0;
          }
        }

        if (!path.isEmpty()) {
          const ZNeutubeVoxel& seed = path.data().front();
          const int64_t seedIndex = seed.toIndex(static_cast<int>(croppedObjMask.width()),
                                                 static_cast<int>(croppedObjMask.height()),
                                                 static_cast<int>(croppedObjMask.depth()));
          if (seedIndex >= 0) {
            sgw.mask[static_cast<size_t>(seedIndex)] = SP_GROW_SOURCE;
          }
        }

        stackSpGrow(tmpdist, &sgw);
      }

      std::vector<ZNeutubeVoxelArray> pathArray = parser.extractAllPath(lengthThreshold, tmpdist);
      if (pathArray.empty() && m_keepingSingleObject) {
        pathArray.push_back(parser.extractLongestPath(nullptr, false));
      }

      for (auto& path : pathArray) {
        if (path.isEmpty()) {
          continue;
        }

        path.sample(tmpdist, adjustedDistanceWeight);
        for (auto& v : path.data()) {
          v.translate(objectOffsetX, objectOffsetY, objectOffsetZ);
        }

        std::unique_ptr<ZSwc> branch = createSwcByRegionSampling(path);
        if (!branch || branch->empty()) {
          continue;
        }

        // Remove small termini (legacy heuristic).
        auto branchRoot = branch->begin();
        if (branchRoot != branch->end()) {
          const auto firstChild = ZSwc::firstChild(branchRoot);
          if (!ZSwc::isNull(firstChild)) {
            if (branchRoot->radius * 2.0 < firstChild->radius) {
              const auto oldRoot = branchRoot;
              branch->setAsRoot(firstChild);
              branch->erase(oldRoot);
              branchRoot = firstChild;
            }
          }

          auto leaf = branchRoot;
          while (true) {
            const auto child = ZSwc::firstChild(leaf);
            if (ZSwc::isNull(child)) {
              break;
            }
            leaf = child;
          }

          const auto parent = ZSwc::parent(leaf);
          if (!ZSwc::isNull(parent)) {
            if (leaf->radius * 2.0 < parent->radius) {
              branch->erase(leaf);
            }
          }
        }

        // Merge the branch into the subtree forest.
        auto branchRootInSubtree = subtree.appendRoot(*branch->beginRoot());
        subtree.copy(branchRootInSubtree, *branch, branch->beginRoot());

        const auto tn = connectBranch(&subtree, branchRootInSubtree);
        const auto parent = ZSwc::parent(tn);
        if (!ZSwc::isNull(parent) && hasOverlapCenters(tn, parent)) {
          mergeToParent(&subtree, tn);
        }
      }
    }

    // Merge this object's forest into the whole-tree forest (preserving root order).
    for (auto root = subtree.beginRoot(); root != subtree.endRoot(); ++root) {
      auto newRoot = wholeTree.appendRoot(*root);
      wholeTree.copy(newRoot, subtree, root);
    }
  }

  if (wholeTree.empty()) {
    return {};
  }

  if (dsIntv[0] > 0 || dsIntv[1] > 0 || dsIntv[2] > 0) {
    rescaleSwc(&wholeTree, dsIntv[0] + 1, dsIntv[1] + 1, dsIntv[2] + 1, true);
  }

  if (m_connectingBranch) {
    LOG(INFO) << "Reconnecting ...";
    double zScale = m_resolution[2];
    if (m_resolution[0] != 1.0) {
      zScale /= m_resolution[0];
    }
    reconnectSwc(&wholeTree, zScale, m_distanceThreshold / m_resolution[0]);
  }

  if (m_resampleSwc) {
    ZNeutubeSwcResampler resampler;
    resampler.optimalDownsample(&wholeTree);
  }

  wholeTree.resortID();

  return std::make_unique<ZSwc>(std::move(wholeTree));
}

void ZNeutubeSkeletonizer::print() const
{
  LOG(INFO) << "Minimal length: " << m_lengthThreshold;
  LOG(INFO) << "Final minimal length: " << m_finalLengthThreshold;
  LOG(INFO) << "Maximal distance: " << m_distanceThreshold;
  LOG(INFO) << "Rebase: " << m_rebase;
  LOG(INFO) << "Intepolate: " << m_interpolating;
  LOG(INFO) << "Remove border: " << m_removingBorder;
  LOG(INFO) << "Filling hole: " << m_fillingHole;
  LOG(INFO) << "Minimal object size: " << m_minObjSize;
  LOG(INFO) << "Keep short object: " << m_keepingSingleObject;
  LOG(INFO) << "Level: " << m_level;
  LOG(INFO) << "Connect branch: " << m_connectingBranch;
  LOG(INFO) << "Resolution: (" << m_resolution[0] << ", " << m_resolution[1] << ", " << m_resolution[2] << ")";
  LOG(INFO) << "Downsample interval: (" << m_downsampleInterval[0] << ", " << m_downsampleInterval[1] << ", "
            << m_downsampleInterval[2] << ")";
}

} // namespace nim::neutube
