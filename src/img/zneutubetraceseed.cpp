#include "zneutubetraceseed.h"

#include "zneutubeedt3d.h"
#include "zneutubeimglocmax.h"
#include "zneutubeobjlabel.h"

#include "zlog.h"

#include <cmath>

namespace nim {

namespace {

[[nodiscard]] ZImg removeSmallObjectsLegacyLike(const ZImg& mask, int minSize, int connectivity)
{
  if (mask.isEmpty()) {
    return {};
  }
  CHECK(mask.numChannels() == 1);
  CHECK(mask.numTimes() == 1);
  CHECK(mask.isType<uint8_t>()) << mask.info();

  if (minSize <= 0) {
    return mask;
  }

  LabelLargeObjectsParams params;
  params.flag = 1;
  params.smallLabel = 2;
  params.minSize = minSize;
  params.connectivity = connectivity;

  const LabelLargeObjectsResult labeled = labelLargeObjectsLegacy(mask, params);
  CHECK(labeled.labels.voxelNumber() == mask.voxelNumber());

  ZImg out(mask.info());
  auto* outData = out.timeData<uint8_t>(0);

  const size_t n = out.voxelNumber();

  if (labeled.labels.isType<uint8_t>()) {
    const auto* a = labeled.labels.timeData<uint8_t>(0);
    for (size_t i = 0; i < n; ++i) {
      outData[i] = static_cast<uint8_t>(a[i] > static_cast<uint8_t>(params.smallLabel));
    }
    return out;
  }

  CHECK(labeled.labels.isType<uint16_t>()) << labeled.labels.info();
  const auto* a = labeled.labels.timeData<uint16_t>(0);
  for (size_t i = 0; i < n; ++i) {
    outData[i] = static_cast<uint8_t>(a[i] > static_cast<uint16_t>(params.smallLabel));
  }
  return out;
}

[[nodiscard]] int minSeedObjSizeLegacyLike(double seedDensity, bool screeningSeed)
{
  if (!screeningSeed) {
    return 0;
  }
  return static_cast<int>(seedDensity * 16000.0);
}

} // namespace

Geo3dScalarField extractSeedOriginalLegacyLike(const ZImg& mask)
{
  Geo3dScalarField field;

  if (mask.isEmpty()) {
    return field;
  }

  CHECK(mask.numChannels() == 1);
  CHECK(mask.numTimes() == 1);
  CHECK(mask.isType<uint8_t>()) << "Expected uint8 mask, got " << mask.info();

  const ZImg dist = bwdistSquaredU16LegacyLike(mask, /*pad*/ 0);
  CHECK(dist.isType<uint16_t>()) << dist.info();

  const ZImg seeds = stackLocalMaxMaskLegacyLike(dist, StackLocmaxOptionLegacyLike::Center);
  CHECK(seeds.isType<uint8_t>()) << seeds.info();
  CHECK(seeds.voxelNumber() == mask.voxelNumber());

  const size_t width = seeds.width();
  const size_t height = seeds.height();
  const size_t depth = seeds.depth();
  const size_t plane = width * height;

  const size_t voxelNumber = seeds.voxelNumber();
  const auto* seedsData = seeds.timeData<uint8_t>(0);
  const auto* distData = dist.timeData<uint16_t>(0);

  // Legacy `Stack_To_Voxel_List` builds a linked list by pushing each voxel to the head,
  // so the final order is the reverse of the stack-array scan order.
  for (size_t idx = voxelNumber; idx-- > 0;) {
    if (seedsData[idx] == 0) {
      continue;
    }

    const size_t z = idx / plane;
    const size_t rem = idx - z * plane;
    const size_t y = rem / width;
    const size_t x = rem - y * width;

    const bool inOpenRange = x > 0 && y > 0 && z > 0 && x + 1 < width && y + 1 < height && z + 1 < depth;
    if (!inOpenRange) {
      continue;
    }

    field.points.push_back({static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)});
    field.values.push_back(std::sqrt(static_cast<double>(distData[idx])));
  }

  CHECK(field.points.size() == field.values.size());
  return field;
}

Geo3dScalarField removeTracedSeedLegacyLike(const Geo3dScalarField& seeds, const TraceWorkspace& tw)
{
  if (seeds.points.empty()) {
    return seeds;
  }

  CHECK(seeds.points.size() == seeds.values.size());

  if (!tw.traceMask) {
    return seeds;
  }

  Geo3dScalarField out;
  out.points.reserve(seeds.points.size());
  out.values.reserve(seeds.values.size());

  for (size_t i = 0; i < seeds.points.size(); ++i) {
    const int x = static_cast<int>(seeds.points[i][0]);
    const int y = static_cast<int>(seeds.points[i][1]);
    const int z = static_cast<int>(seeds.points[i][2]);

    const std::array<double, 3> pos = {static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)};
    if (traceWorkspaceMaskValueLegacyLike(tw, pos) == 0) {
      out.points.push_back(pos);
      out.values.push_back(seeds.values[i]);
    }
  }

  return out;
}

Geo3dScalarField removeNoisySeedLegacyLike(Geo3dScalarField seeds,
                                           ZImg& mask,
                                           int seedMethod,
                                           bool screeningSeed,
                                           RemoveNoisySeedDiagnosticsLegacyLike* diag)
{
  if (mask.isEmpty()) {
    seeds.points.clear();
    seeds.values.clear();
    return seeds;
  }

  const double seedDensity =
    (mask.voxelNumber() == 0) ? 0.0 : static_cast<double>(seeds.size()) / static_cast<double>(mask.voxelNumber());
  const int minSeedSize = minSeedObjSizeLegacyLike(seedDensity, screeningSeed);

  if (diag != nullptr) {
    diag->seedDensity = seedDensity;
    diag->minSeedSize = minSeedSize;
  }

  if (minSeedSize <= 0) {
    return seeds;
  }

  mask = removeSmallObjectsLegacyLike(mask, minSeedSize, /*connectivity*/ 26);

  switch (seedMethod) {
    case 1:
      return extractSeedOriginalLegacyLike(mask);
    case 2:
      LOG(ERROR) << "removeNoisySeedLegacyLike: seedMethod=2 (skeleton seeding) not supported yet.";
      seeds.points.clear();
      seeds.values.clear();
      return seeds;
    default:
      LOG(ERROR) << "removeNoisySeedLegacyLike: unsupported seed method: " << seedMethod;
      seeds.points.clear();
      seeds.values.clear();
      return seeds;
  }
}

} // namespace nim
