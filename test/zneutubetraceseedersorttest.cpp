#include "zneutubetraceseeder.h"

#include "zneutubetracelocseglabel.h"
#include "zneutubetraceworkspace.h"
#include "zexception.h"
#include "zimg.h"
#include "ztest.h"

#include <gflags/gflags.h>
#include <folly/CancellationToken.h>

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

DECLARE_bool(atlas_autotrace_seed_sort_commit_by_score);

namespace {

class ScopedSeedSortCommitMode
{
public:
  explicit ScopedSeedSortCommitMode(bool commitByScore)
    : _previous(FLAGS_atlas_autotrace_seed_sort_commit_by_score)
  {
    FLAGS_atlas_autotrace_seed_sort_commit_by_score = commitByScore;
  }

  ~ScopedSeedSortCommitMode()
  {
    FLAGS_atlas_autotrace_seed_sort_commit_by_score = _previous;
  }

  ScopedSeedSortCommitMode(const ScopedSeedSortCommitMode&) = delete;
  ScopedSeedSortCommitMode& operator=(const ScopedSeedSortCommitMode&) = delete;

private:
  bool _previous = true;
};

[[nodiscard]] nim::LocalNeuroseg makeSeedLocseg(std::array<double, 3> center, double radius)
{
  nim::LocalNeuroseg seed;
  seed.seg.r1 = radius;
  seed.seg.c = 0.0;
  seed.seg.h = nim::NeurosegDefaultHLegacyLike;
  seed.seg.theta = 0.0;
  seed.seg.psi = 0.0;
  seed.seg.curvature = 0.0;
  seed.seg.alpha = 0.0;
  seed.seg.scale = 1.0;
  nim::setNeurosegPositionLegacyLike(seed, center, nim::NeuroposReferenceLegacyLike::Center);
  return seed;
}

[[nodiscard]] nim::ZImg makeSeedSignal()
{
  nim::ZImgInfo info(32, 32, 16, 1, 1, 1, nim::VoxelFormat::Unsigned);
  info.setVoxelFormat<uint8_t>();
  info.createDefaultDescriptions();

  nim::ZImg signal(info);
  signal.fill(0);

  const nim::LocalNeuroseg seed = makeSeedLocseg({16.0, 16.0, 8.0}, 4.5);
  nim::localNeurosegLabelGLegacyLike(seed, signal, /*flag*/ -1, /*value*/ 255, /*zScale*/ 1.0);
  return signal;
}

[[nodiscard]] nim::Geo3dScalarField makeSeeds(std::initializer_list<std::array<double, 4>> entries)
{
  nim::Geo3dScalarField seeds;
  seeds.points.reserve(entries.size());
  seeds.values.reserve(entries.size());
  for (const auto& entry : entries) {
    seeds.points.push_back({entry[0], entry[1], entry[2]});
    seeds.values.push_back(entry[3]);
  }
  return seeds;
}

[[nodiscard]] std::vector<uint8_t> copyMaskBytes(const nim::ZImg& mask)
{
  CHECK(mask.isType<uint8_t>()) << mask.info();
  const uint8_t* data = mask.timeData<uint8_t>(0);
  return {data, data + mask.voxelNumber()};
}

[[nodiscard]] nim::TraceWorkspace makeSeedSortWorkspace(const nim::ZImg& signal)
{
  nim::TraceWorkspace tw;
  nim::locsegChainDefaultTraceWorkspaceLegacyLike(tw, signal);
  tw.minScore = std::numeric_limits<double>::infinity();
  return tw;
}

[[nodiscard]] double findWeakerLegacyWinningSeedRadius(const nim::ZImg& signal)
{
  using namespace nim;

  TraceWorkspace referenceSeedWorkspace = makeSeedSortWorkspace(signal);
  const SeedSortResultLegacyLike referenceSeedResult = sortSeedsLegacyLike(makeSeeds({
                                                                             {16.0, 16.0, 8.0, 4.5}
  }),
                                                                           signal,
                                                                           referenceSeedWorkspace);
  const double referenceScore = referenceSeedResult.scoreArray[0];

  const std::array<double, 8> candidateRadii = {
    1.5,
    2.0,
    2.5,
    3.0,
    3.5,
    4.0,
    5.0,
    6.0,
  };

  for (double candidateRadius : candidateRadii) {
    TraceWorkspace candidateSeedWorkspace = makeSeedSortWorkspace(signal);
    const SeedSortResultLegacyLike candidateSeedResult = sortSeedsLegacyLike(makeSeeds({
                                                                               {16.0, 16.0, 8.0, candidateRadius}
    }),
                                                                             signal,
                                                                             candidateSeedWorkspace);
    if (!(candidateSeedResult.scoreArray[0] < referenceScore)) {
      continue;
    }

    return candidateRadius;
  }

  return std::numeric_limits<double>::quiet_NaN();
}

} // namespace

TEST(NeutubeTraceSeeder, UsesScoreOrderedBaseMaskCommitByDefault)
{
  using namespace nim;

  const ZImg signal = makeSeedSignal();
  const double weakerSeedRadius = findWeakerLegacyWinningSeedRadius(signal);
  ASSERT_FALSE(std::isnan(weakerSeedRadius));

  TraceWorkspace firstSingleSeedWorkspace = makeSeedSortWorkspace(signal);
  const SeedSortResultLegacyLike firstSingleSeedResult = sortSeedsLegacyLike(makeSeeds({
                                                                               {16.0, 16.0, 8.0, weakerSeedRadius}
  }),
                                                                             signal,
                                                                             firstSingleSeedWorkspace);
  const std::vector<uint8_t> firstSingleMask = copyMaskBytes(firstSingleSeedResult.baseMask);
  ASSERT_GT(std::count_if(firstSingleMask.begin(),
                          firstSingleMask.end(),
                          [](uint8_t v) {
                            return v > 0;
                          }),
            0);

  TraceWorkspace secondSingleSeedWorkspace = makeSeedSortWorkspace(signal);
  const SeedSortResultLegacyLike secondSingleSeedResult = sortSeedsLegacyLike(makeSeeds({
                                                                                {16.0, 16.0, 8.0, 4.5}
  }),
                                                                              signal,
                                                                              secondSingleSeedWorkspace);
  const std::vector<uint8_t> secondSingleMask = copyMaskBytes(secondSingleSeedResult.baseMask);
  ASSERT_GT(std::count_if(secondSingleMask.begin(),
                          secondSingleMask.end(),
                          [](uint8_t v) {
                            return v > 0;
                          }),
            0);
  ASSERT_LT(firstSingleSeedResult.scoreArray[0], secondSingleSeedResult.scoreArray[0]);

  TraceWorkspace overlappingSeedWorkspace = makeSeedSortWorkspace(signal);
  const SeedSortResultLegacyLike overlappingSeedResult = sortSeedsLegacyLike(makeSeeds({
                                                                               {16.0, 16.0, 8.0, weakerSeedRadius},
                                                                               {16.0, 16.0, 8.0, 4.5             }
  }),
                                                                             signal,
                                                                             overlappingSeedWorkspace);

  ASSERT_EQ(overlappingSeedResult.scoreArray.size(), 2u);
  EXPECT_EQ(overlappingSeedResult.scoreArray[0], 0.0);
  EXPECT_GT(overlappingSeedResult.scoreArray[1], 0.0);
  EXPECT_EQ(copyMaskBytes(overlappingSeedResult.baseMask), secondSingleMask);
}

TEST(NeutubeTraceSeeder, KeepsLegacyOrderedBaseMaskCommitWhenFlagDisabled)
{
  using namespace nim;

  ScopedSeedSortCommitMode legacyCommitMode(false);
  const ZImg signal = makeSeedSignal();
  const double weakerSeedRadius = findWeakerLegacyWinningSeedRadius(signal);
  ASSERT_FALSE(std::isnan(weakerSeedRadius));

  TraceWorkspace singleSeedWorkspace = makeSeedSortWorkspace(signal);
  const SeedSortResultLegacyLike singleSeedResult = sortSeedsLegacyLike(makeSeeds({
                                                                          {16.0, 16.0, 8.0, weakerSeedRadius}
  }),
                                                                        signal,
                                                                        singleSeedWorkspace);
  const std::vector<uint8_t> singleMask = copyMaskBytes(singleSeedResult.baseMask);
  ASSERT_GT(std::count_if(singleMask.begin(),
                          singleMask.end(),
                          [](uint8_t v) {
                            return v > 0;
                          }),
            0);

  TraceWorkspace overlappingSeedWorkspace = makeSeedSortWorkspace(signal);
  const SeedSortResultLegacyLike overlappingSeedResult = sortSeedsLegacyLike(makeSeeds({
                                                                               {16.0, 16.0, 8.0, weakerSeedRadius},
                                                                               {16.0, 16.0, 8.0, 4.5             }
  }),
                                                                             signal,
                                                                             overlappingSeedWorkspace);

  ASSERT_EQ(overlappingSeedResult.scoreArray.size(), 2u);
  EXPECT_GT(overlappingSeedResult.scoreArray[0], 0.0);
  EXPECT_EQ(overlappingSeedResult.scoreArray[1], 0.0);
  EXPECT_EQ(copyMaskBytes(overlappingSeedResult.baseMask), singleMask);
}

TEST(NeutubeTraceSeeder, HonorsPreCancelledToken)
{
  using namespace nim;

  const ZImg signal = makeSeedSignal();
  TraceWorkspace tw = makeSeedSortWorkspace(signal);

  folly::CancellationSource cancellationSource;
  cancellationSource.requestCancellation();
  tw.cancellationToken = cancellationSource.getToken();

  EXPECT_THROW((void)sortSeedsLegacyLike(makeSeeds({
                                           {16.0, 16.0, 8.0, 4.5}
  }),
                                         signal,
                                         tw),
               ZCancellationException);
}
