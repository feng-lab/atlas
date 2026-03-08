#include <gtest/gtest.h>

#include "zneutubelocsegchaincircle.h"

namespace {

nim::LocsegChain makeSingleNodeChain()
{
  nim::LocsegChain chain;

  nim::LocsegNode node;
  node.locseg.seg.r1 = 2.0;
  node.locseg.seg.c = 0.1;
  node.locseg.seg.h = 11.0;
  node.locseg.seg.theta = 0.0;
  node.locseg.seg.psi = 0.0;
  node.locseg.seg.scale = 1.0;
  node.locseg.pos = {10.0, 20.0, 30.0};

  (void)chain.addNode(std::move(node), nim::LocsegChainEndLegacyLike::Tail);
  return chain;
}

TEST(ZneutubeLocsegChainCircle, SwelledConversionExpandsCircleRadii)
{
  const nim::LocsegChain chain = makeSingleNodeChain();

  const std::vector<nim::Geo3dCircle> raw = nim::locsegChainToGeo3dCircleArrayLegacyLike(chain);
  const std::vector<nim::Geo3dCircle> swelled =
    nim::locsegChainToGeo3dCircleArraySwelledZLegacyLike(chain,
                                                         /*zScale*/ 1.0,
                                                         /*swellRatio*/ 1.5,
                                                         /*swellDiff*/ 0.0,
                                                         /*swellLimit*/ 3.0);

  ASSERT_EQ(raw.size(), 2u);
  ASSERT_EQ(swelled.size(), raw.size());

  EXPECT_DOUBLE_EQ(raw[0].radius, 2.0);
  EXPECT_DOUBLE_EQ(raw[1].radius, 3.0);

  // Legacy Neuroseg_Swell preserves taper when c>0, so the bottom radius grows less than the top radius.
  EXPECT_DOUBLE_EQ(swelled[0].radius, 3.5);
  EXPECT_DOUBLE_EQ(swelled[1].radius, 4.5);
}

} // namespace
