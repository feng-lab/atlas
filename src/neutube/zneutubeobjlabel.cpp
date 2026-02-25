#include "zneutubeobjlabel.h"

#include "zneutubeneighborhood.h"

#include "zexception.h"
#include "zlog.h"

#include "zimgneighborhooditerator.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace nim {

namespace {

[[nodiscard]] bool fitsInUint8(int v)
{
  return v >= 0 && v <= std::numeric_limits<uint8_t>::max();
}

[[nodiscard]] bool fitsInUint16(int v)
{
  return v >= 0 && v <= std::numeric_limits<uint16_t>::max();
}

[[nodiscard]] ZImg makeUint8Volume(size_t width, size_t height, size_t depth)
{
  ZImgInfo info(width, height, depth);
  info.setVoxelFormat<uint8_t>();
  info.createDefaultDescriptions();
  ZImg img(info);
  img.fill(0);
  return img;
}

[[nodiscard]] ZImg makeUint16Volume(size_t width, size_t height, size_t depth)
{
  ZImgInfo info(width, height, depth);
  info.setVoxelFormat<uint16_t>();
  info.createDefaultDescriptions();
  ZImg img(info);
  img.fill(0);
  return img;
}

void promoteLabelsToUint16(ZImg* labels)
{
  CHECK(labels != nullptr);
  CHECK(labels->isType<uint8_t>());

  ZImg promoted = makeUint16Volume(labels->width(), labels->height(), labels->depth());

  const size_t voxelNumber = labels->voxelNumber();
  const auto* src = labels->timeData<uint8_t>(0);
  auto* dst = promoted.timeData<uint16_t>(0);
  for (size_t i = 0; i < voxelNumber; ++i) {
    dst[i] = src[i];
  }

  *labels = std::move(promoted);
}

template<typename TInVoxel>
void fillMarkerEqFlag(const ZImg& img, int flag, ZImg* marker)
{
  CHECK(marker != nullptr);
  CHECK(marker->isType<uint8_t>());

  const size_t voxelNumber = img.voxelNumber();
  const auto* in = img.timeData<TInVoxel>(0);
  auto* out = marker->timeData<uint8_t>(0);

  const TInVoxel flagVoxel = static_cast<TInVoxel>(flag);
  for (size_t i = 0; i < voxelNumber; ++i) {
    out[i] = (in[i] == flagVoxel) ? uint8_t{1} : uint8_t{0};
  }
}

} // namespace

LabelLargeObjectsResult labelLargeObjectsLegacy(const ZImg& img, const LabelLargeObjectsParams& params)
{
  if (img.isEmpty()) {
    throw ZException("labelLargeObjectsLegacy: empty image.");
  }

  if (img.numChannels() != 1 || img.numTimes() != 1) {
    throw ZException(fmt::format("labelLargeObjectsLegacy: expected 1 channel/time, got channels={} times={}.",
                                 img.numChannels(),
                                 img.numTimes()));
  }

  if (params.smallLabel <= params.flag) {
    throw ZException(fmt::format("labelLargeObjectsLegacy: invalid labels (smallLabel={} must be > flag={}).",
                                 params.smallLabel,
                                 params.flag));
  }

  if (params.connectivity != 4 && params.connectivity != 8 && params.connectivity != 6 && params.connectivity != 10 &&
      params.connectivity != 18 && params.connectivity != 26) {
    throw ZException(fmt::format("labelLargeObjectsLegacy: unsupported connectivity {}.", params.connectivity));
  }

  if (params.maxLabel < params.smallLabel + 1) {
    throw ZException(fmt::format("labelLargeObjectsLegacy: invalid maxLabel {} (must be >= smallLabel+1={}).",
                                 params.maxLabel,
                                 params.smallLabel + 1));
  }

  if (params.flag < 0 || params.smallLabel < 0) {
    throw ZException("labelLargeObjectsLegacy: negative label values are not supported.");
  }

  const size_t width = img.width();
  const size_t height = img.height();
  const size_t depth = img.depth();
  const size_t voxelNumber = img.voxelNumber();

  ZImg marker = makeUint8Volume(width, height, depth);
  if (img.isType<uint8_t>()) {
    fillMarkerEqFlag<uint8_t>(img, params.flag, &marker);
  } else if (img.isType<uint16_t>()) {
    fillMarkerEqFlag<uint16_t>(img, params.flag, &marker);
  } else {
    throw ZException(fmt::format("labelLargeObjectsLegacy: unsupported voxel type '{}'.", img.info()));
  }

  ZImg labels;
  const bool startAsUint16 = img.isType<uint16_t>() || !fitsInUint8(params.smallLabel) || !fitsInUint8(params.flag);
  if (startAsUint16) {
    if (!fitsInUint16(params.smallLabel) || !fitsInUint16(params.flag)) {
      throw ZException("labelLargeObjectsLegacy: label values exceed uint16, unsupported by legacy semantics.");
    }
    labels = makeUint16Volume(width, height, depth);
  } else {
    labels = makeUint8Volume(width, height, depth);
  }

  const ZNeighborhood& nb = neighborhoodLegacyOrder(params.connectivity);
  ZImgNeighborhoodConstIterator<uint8_t> nit(nb, marker);

  auto* markerData = marker.timeData<uint8_t>(0);
  auto* labels8 = labels.isType<uint8_t>() ? labels.timeData<uint8_t>(0) : nullptr;
  auto* labels16 = labels.isType<uint16_t>() ? labels.timeData<uint16_t>(0) : nullptr;

  const int smallLabel = params.smallLabel;
  const int firstLargeLabel = smallLabel + 1;
  int currentLargeLabel = firstLargeLabel;

  size_t numLargeObjects = 0;

  std::vector<size_t> stack;
  std::vector<size_t> objectVoxels;
  stack.reserve(1024);
  objectVoxels.reserve(1024);

  for (size_t i = 0; i < voxelNumber; ++i) {
    if (markerData[i] != 1) {
      continue;
    }

    // Legacy promotion trigger: translate GREY->GREY16 when the *current* large label exceeds 255, even if this
    // particular component ends up being re-labeled to `smallLabel`.
    if (labels8 != nullptr && currentLargeLabel > std::numeric_limits<uint8_t>::max()) {
      promoteLabelsToUint16(&labels);
      labels8 = nullptr;
      labels16 = labels.timeData<uint16_t>(0);
    }

    stack.clear();
    objectVoxels.clear();

    stack.push_back(i);
    markerData[i] = 0;
    objectVoxels.push_back(i);

    while (!stack.empty()) {
      const size_t idx = stack.back();
      stack.pop_back();

      nit.goToIndex(static_cast<index_t>(idx));
      for (size_t n = 0; n < nb.size(); ++n) {
        if (!nit.isInBound(n)) {
          continue;
        }
        const size_t nidx = static_cast<size_t>(nit.index(n));
        if (markerData[nidx] == 1) {
          markerData[nidx] = 0;
          stack.push_back(nidx);
          objectVoxels.push_back(nidx);
        }
      }
    }

    const size_t objSize = objectVoxels.size();

    bool isLarge = true;
    if (params.minSize > 0) {
      isLarge = objSize >= static_cast<size_t>(params.minSize);
    }

    int finalLabel = smallLabel;
    if (isLarge) {
      finalLabel = currentLargeLabel;
      ++numLargeObjects;
      if (params.incrementLargeLabel) {
        ++currentLargeLabel;
        if (currentLargeLabel > params.maxLabel) {
          currentLargeLabel = firstLargeLabel;
        }
      }
    }

    if (labels8 != nullptr) {
      CHECK(fitsInUint8(finalLabel));
      const auto v = static_cast<uint8_t>(finalLabel);
      for (const size_t p : objectVoxels) {
        labels8[p] = v;
      }
    } else {
      CHECK(labels16 != nullptr);
      CHECK(fitsInUint16(finalLabel));
      const auto v = static_cast<uint16_t>(finalLabel);
      for (const size_t p : objectVoxels) {
        labels16[p] = v;
      }
    }
  }

  LabelLargeObjectsResult res;
  res.labels = std::move(labels);
  res.numLargeObjects = numLargeObjects;
  return res;
}

} // namespace nim
