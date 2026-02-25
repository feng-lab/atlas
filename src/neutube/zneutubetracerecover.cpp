#include "zneutubetracerecover.h"

#include "zneutubeobjlabel.h"
#include "zneutubetraceallseeds.h"
#include "zneutubetraceseed.h"
#include "zneutubetraceseeder.h"
#include "zneutubetracescorethresholds.h"

#include "zlog.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace nim {

namespace {

[[nodiscard]] ZImg makeBinaryU8Like(const ZImg& like)
{
  ZImgInfo info(like.width(), like.height(), like.depth(), 1, 1, 1, VoxelFormat::Unsigned);
  ZImg out(info);
  out.fill(0);
  return out;
}

[[nodiscard]] bool stackIsDarkLegacyLike(const ZImg& stack)
{
  if (stack.isEmpty()) {
    return true;
  }
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  const size_t n = stack.voxelNumber();
  const auto* a = stack.timeData<uint8_t>(0);
  for (size_t i = 0; i < n; ++i) {
    if (a[i] > 0) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] ZImg stackBsubLegacyLike(const ZImg& stack1, const ZImg& stack2)
{
  CHECK(!stack1.isEmpty());
  CHECK(!stack2.isEmpty());
  CHECK(stack1.isSameSize(stack2));
  CHECK(stack1.numChannels() == 1);
  CHECK(stack1.numTimes() == 1);
  CHECK(stack2.numChannels() == 1);
  CHECK(stack2.numTimes() == 1);
  CHECK(stack1.isType<uint8_t>()) << stack1.info();
  CHECK(stack2.isType<uint8_t>()) << stack2.info();

  ZImg out = makeBinaryU8Like(stack1);
  const size_t n = out.voxelNumber();
  const auto* a = stack1.timeData<uint8_t>(0);
  const auto* b = stack2.timeData<uint8_t>(0);
  auto* o = out.timeData<uint8_t>(0);
  for (size_t i = 0; i < n; ++i) {
    o[i] = static_cast<uint8_t>(a[i] > b[i]);
  }
  return out;
}

// Port of `Stack_Z_Dilate` including its legacy signal-type bug:
// it indexes the signal's raw byte array by voxel index regardless of actual voxel size.
[[nodiscard]] ZImg stackZDilateLegacyLike(const ZImg& stack, int size, const ZImg& signal)
{
  CHECK(!stack.isEmpty());
  CHECK(stack.isSameSize(signal));
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);
  CHECK(signal.numChannels() == 1);
  CHECK(signal.numTimes() == 1);
  CHECK(stack.isType<uint8_t>()) << stack.info();
  CHECK(size > 0);

  ZImg out = stack;

  const int supportSize = 3;
  const int width = static_cast<int>(stack.width());
  const int height = static_cast<int>(stack.height());
  const int depth = static_cast<int>(stack.depth());
  const int area = width * height;

  const auto* inData = stack.timeData<uint8_t>(0);
  auto* outData = out.timeData<uint8_t>(0);

  // Legacy bug: use raw bytes for signal regardless of voxel type.
  const auto* signalBytes = signal.timeData<uint8_t>(0);

  double mean = 0.0;
  int offset = supportSize * area;

  // downward
  for (int k = supportSize - 1; k < depth - size - 1; ++k) {
    for (int j = 0; j < height; ++j) {
      for (int i = 0; i < width; ++i) {
        if ((inData[offset] == 1) && (inData[offset + area] == 0)) {
          mean = static_cast<double>(signalBytes[offset]);
          for (int z = 1; z < supportSize; ++z) {
            mean += static_cast<double>(signalBytes[offset - area * z]);
          }
          mean /= static_cast<double>(supportSize);
          for (int z = 1; z <= size; ++z) {
            if (mean >= static_cast<double>(signalBytes[offset + area * z])) {
              outData[offset + area * z] = 1;
            }
          }
        }
        ++offset;
      }
    }
  }

  offset = size * area;

  // upward
  for (int k = size; k < depth - supportSize; ++k) {
    for (int j = 0; j < height; ++j) {
      for (int i = 0; i < width; ++i) {
        if ((inData[offset] == 1) && (inData[offset - area] == 0)) {
          mean = static_cast<double>(signalBytes[offset]);
          for (int z = 1; z < supportSize; ++z) {
            mean += static_cast<double>(signalBytes[offset + area * z]);
          }
          mean /= static_cast<double>(supportSize);
          for (int z = 1; z <= size; ++z) {
            if (mean >= static_cast<double>(signalBytes[offset - area * z])) {
              outData[offset - area * z] = 1;
            }
          }
        }
        ++offset;
      }
    }
  }

  return out;
}

[[nodiscard]] ZImg removeSmallObjectsBinaryU8LegacyLike(const ZImg& in, int minSize, int connectivity)
{
  if (in.isEmpty()) {
    return {};
  }
  CHECK(in.numChannels() == 1);
  CHECK(in.numTimes() == 1);
  CHECK(in.isType<uint8_t>()) << in.info();

  if (minSize <= 0) {
    return in;
  }

  LabelLargeObjectsParams params;
  params.flag = 1;
  params.smallLabel = 2;
  params.minSize = minSize;
  params.connectivity = connectivity;

  const LabelLargeObjectsResult labeled = labelLargeObjectsLegacy(in, params);
  CHECK(labeled.labels.voxelNumber() == in.voxelNumber());

  ZImg out = makeBinaryU8Like(in);
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

} // namespace

RecoverResultLegacyLike recoverLegacyLike(const ZImg& signal,
                                          const TraceConfig& cfg,
                                          const ZImg& mask,
                                          std::optional<ZImg> baseMask,
                                          TraceWorkspace& tw)
{
  RecoverResultLegacyLike out;

  if (mask.isEmpty()) {
    out.baseMask = std::move(baseMask);
    return out;
  }

  CHECK(mask.numChannels() == 1);
  CHECK(mask.numTimes() == 1);
  CHECK(mask.isType<uint8_t>()) << mask.info();

  if (!baseMask || baseMask->isEmpty()) {
    out.baseMask = std::move(baseMask);
    return out;
  }
  CHECK(baseMask->isSameSize(mask));
  CHECK(baseMask->isType<uint8_t>()) << baseMask->info();

  // leftover = translate(m_mask, GREY, 0)
  ZImg leftover = mask;

  // traceMask binary = (traceMask>0) OR (baseMask==1)
  ZImg traceMaskBinary = makeBinaryU8Like(mask);
  {
    const size_t n = traceMaskBinary.voxelNumber();
    auto* traceBin = traceMaskBinary.timeData<uint8_t>(0);
    const auto* base = baseMask->timeData<uint8_t>(0);

    const bool haveTraceMask = static_cast<bool>(tw.traceMask);
    const auto* traceMask16 = haveTraceMask ? tw.traceMask->timeData<uint16_t>(0) : nullptr;

    for (size_t i = 0; i < n; ++i) {
      const bool traced = haveTraceMask && (traceMask16[i] > 0);
      const bool baseOne = (base[i] == 1);
      traceBin[i] = static_cast<uint8_t>(traced || baseOne);
    }
  }

  // Legacy frees baseMask here.
  baseMask.reset();

  const ZImg submask = stackZDilateLegacyLike(traceMaskBinary, /*size*/ 5, signal);
  traceMaskBinary = stackBsubLegacyLike(leftover, submask);

  leftover = removeSmallObjectsBinaryU8LegacyLike(traceMaskBinary, /*minSize*/ 27, /*connectivity*/ 26);

  // Legacy translate(leftover, GREY, 1) effectively normalizes to 0/1.
  {
    const size_t n = leftover.voxelNumber();
    auto* a = leftover.timeData<uint8_t>(0);
    for (size_t i = 0; i < n; ++i) {
      a[i] = static_cast<uint8_t>(a[i] > 0);
    }
  }

  if (stackIsDarkLegacyLike(leftover)) {
    out.baseMask = std::move(baseMask);
    return out;
  }

  const double originalMinLength = tw.minChainLength;
  if (!tw.refit) {
    tw.minChainLength = (NeurosegDefaultHLegacyLike - 1.0) * 2.0 - 1.0;
  } else {
    tw.minChainLength = (NeurosegDefaultHLegacyLike - 1.0) * 1.5 - 1.0;
  }

  // Extract seeds from leftover and trace them.
  Geo3dScalarField seeds;
  switch (cfg.seedMethod) {
    case 1:
      seeds = extractSeedOriginalLegacyLike(leftover);
      break;
    case 2:
      LOG(ERROR) << "recoverLegacyLike: seedMethod=2 (skeleton seeding) is not supported yet.";
      tw.minChainLength = originalMinLength;
      out.baseMask = std::move(baseMask);
      return out;
    default:
      LOG(ERROR) << "recoverLegacyLike: unsupported seedMethod=" << cfg.seedMethod;
      tw.minChainLength = originalMinLength;
      out.baseMask = std::move(baseMask);
      return out;
  }
  prepareTraceScoreThresholdLegacyLike(signal, cfg, TracingModeLegacyLike::Seed, tw);
  SeedSortResultLegacyLike sorted = sortSeedsLegacyLike(seeds, signal, tw);

  // Keep the updated base mask for parity (legacy stores it into m_baseMask).
  out.baseMask = sorted.baseMask;

  // Legacy traces with TRACING_AUTO thresholds after sorting seeds.
  prepareTraceScoreThresholdLegacyLike(signal, cfg, TracingModeLegacyLike::Auto, tw);
  out.chains = traceAllSeedsLegacyLike(signal, /*zScale*/ 1.0, sorted.locsegArray, sorted.scoreArray, tw);
  tw.minChainLength = originalMinLength;

  return out;
}

} // namespace nim
