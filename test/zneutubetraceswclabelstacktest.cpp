#include <gtest/gtest.h>

#include "zneutubelocalneuroseg.h"
#include "zneutubetraceswclabelstack.h"
#include "zneutubetraceswclocseg.h"
#include "zimg.h"
#include "zswc.h"

namespace {

nim::ZSwc makeSimpleZSwc()
{
  nim::ZSwc swc;

  nim::SwcNode root;
  root.id = 1;
  root.type = 0;
  root.x = 5.0;
  root.y = 5.0;
  root.z = 1.0;
  root.radius = 0.4;
  root.parentID = -1;
  root.label = 0;
  const auto rootIt = swc.appendRoot(root);

  nim::SwcNode child;
  child.id = 2;
  child.type = 0;
  child.x = 5.0;
  child.y = 5.0;
  child.z = 3.0;
  child.radius = 0.4;
  child.parentID = -1;
  child.label = 0;
  (void)swc.appendChild(rootIt, child);

  return swc;
}

TEST(ZneutubeTraceSwcLabelStack, SwcNodeToLocsegConvertsImageZToTraceSpace)
{
  nim::ZSwc swc = makeSimpleZSwc();

  auto it = swc.begin();
  ASSERT_NE(it, swc.end());
  ++it;
  ASSERT_NE(it, swc.end());

  const double zToXYRatio = 4.0;
  const auto locsegOpt = nim::swcNodeToLocsegLegacyLike(it, zToXYRatio);
  ASSERT_TRUE(locsegOpt.has_value());

  const std::array<double, 3> bottom = nim::localNeurosegBottomLegacyLike(*locsegOpt);
  const std::array<double, 3> top = nim::localNeurosegTopLegacyLike(*locsegOpt);

  EXPECT_DOUBLE_EQ(bottom[0], 5.0);
  EXPECT_DOUBLE_EQ(bottom[1], 5.0);
  EXPECT_DOUBLE_EQ(bottom[2], 1.0 * zToXYRatio);

  EXPECT_DOUBLE_EQ(top[0], 5.0);
  EXPECT_DOUBLE_EQ(top[1], 5.0);
  EXPECT_DOUBLE_EQ(top[2], 3.0 * zToXYRatio);
}

TEST(ZneutubeTraceSwcLabelStack, LabelSwcIntoMaskUsesImageSpaceZAfterTraceConversion)
{
  nim::ZSwc swc = makeSimpleZSwc();

  nim::ZImgInfo info(11, 11, 6, 1, 1, 1, nim::VoxelFormat::Unsigned);
  info.setVoxelFormat<uint8_t>();
  info.createDefaultDescriptions();
  nim::ZImg mask(info);
  mask.fill(0);

  const double zToXYRatio = 4.0;
  nim::labelSwcIntoMaskLegacyLike(swc, mask, zToXYRatio, /*value*/ 255);

  const auto valueAt = [&](size_t x, size_t y, size_t z) -> uint8_t {
    return *mask.data<uint8_t>(x, y, z);
  };

  EXPECT_EQ(valueAt(5, 5, 1), 255);
  EXPECT_EQ(valueAt(5, 5, 2), 255);
  EXPECT_EQ(valueAt(5, 5, 3), 255);
}

} // namespace
