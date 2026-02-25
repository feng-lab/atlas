#include "zneutubeswcregionsampling.h"

#include "zneutubedarrayqsort.h"

#include "zlog.h"

#include <vector>

namespace nim {

std::unique_ptr<ZSwc> createSwcByRegionSampling(const ZNeutubeVoxelArray& voxelArray, double radiusAdjustment)
{
  if (voxelArray.isEmpty()) {
    return {};
  }

  const auto& voxels = voxelArray.data();

  std::vector<double> voxelSizeArray;
  voxelSizeArray.resize(voxels.size());
  for (size_t i = 0; i < voxels.size(); ++i) {
    voxelSizeArray[i] = -voxels[i].value;
  }

  std::vector<int> indexArray;
  darrayQsortLegacy(voxelSizeArray, &indexArray);

  std::vector<bool> sampled(voxels.size(), true);
  for (size_t i = 1; i < voxels.size(); ++i) {
    const size_t currentVoxelIndex = static_cast<size_t>(indexArray[i]);
    const ZNeutubeVoxel& currentVoxel = voxels[currentVoxelIndex];
    for (size_t j = 0; j < i; ++j) {
      const size_t prevVoxelIndex = static_cast<size_t>(indexArray[j]);
      if (sampled[prevVoxelIndex]) {
        const ZNeutubeVoxel& prevVoxel = voxels[prevVoxelIndex];
        const double dist = currentVoxel.distanceTo(prevVoxel);
        if (dist < prevVoxel.value) {
          sampled[currentVoxelIndex] = false;
          break;
        }
      }
    }
  }

  auto swc = std::make_unique<ZSwc>();
  ZSwc::SwcTreeNode prevNode;
  for (size_t i = 0; i < voxels.size(); ++i) {
    if (!sampled[i]) {
      continue;
    }

    const ZNeutubeVoxel& v = voxels[i];
    SwcNode node;
    node.id = 1;
    node.type = 0;
    node.x = static_cast<double>(v.x);
    node.y = static_cast<double>(v.y);
    node.z = static_cast<double>(v.z);
    node.radius = v.value + radiusAdjustment;
    node.parentID = -1;

    if (ZSwc::isNull(prevNode)) {
      prevNode = swc->appendRoot(node);
    } else {
      prevNode = swc->appendChild(prevNode, node);
    }
  }

  return swc;
}

} // namespace nim
