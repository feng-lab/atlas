#include "zimgconnectedcomponents.h"
#include "ztest.h"

TEST(ZImgConnectedComponents, text)
{
  using namespace nim;

  try {
    ZImg img(getTestDataDir().filePath("img/text.tif"));

    ZBenchTimer bt;
    ZImgConnectedComponents conncomp;
    ConnComp CC = conncomp.run(img);
    STOP_AND_LOG(bt)

    ASSERT_EQ(size_t(8), CC.connectivity);
    ASSERT_EQ(size_t(88), CC.voxelIdxList.size());

    ASSERT_EQ(size_t(66), CC.voxelIdxList[0].size());
    ASSERT_EQ(size_t(80), CC.voxelIdxList[1].size());
    ASSERT_EQ(size_t(48), CC.voxelIdxList[2].size());
    ASSERT_EQ(size_t(48), CC.voxelIdxList[3].size());
    ASSERT_EQ(size_t(80), CC.voxelIdxList[4].size());
    ASSERT_EQ(size_t(85), CC.voxelIdxList[5].size());
    ASSERT_EQ(size_t(63), CC.voxelIdxList[6].size());
    ASSERT_EQ(size_t(9), CC.voxelIdxList[85].size());
    ASSERT_EQ(size_t(41), CC.voxelIdxList[86].size());

    ASSERT_EQ(size_t(60582), CC.voxelIdxList[85][0]);
    ASSERT_EQ(size_t(60583), CC.voxelIdxList[85][1]);
    ASSERT_EQ(size_t(60584), CC.voxelIdxList[85][2]);
    ASSERT_EQ(size_t(60838), CC.voxelIdxList[85][3]);
    ASSERT_EQ(size_t(60839), CC.voxelIdxList[85][4]);
    ASSERT_EQ(size_t(60840), CC.voxelIdxList[85][5]);
    ASSERT_EQ(size_t(61094), CC.voxelIdxList[85][6]);
    ASSERT_EQ(size_t(61095), CC.voxelIdxList[85][7]);
    ASSERT_EQ(size_t(61096), CC.voxelIdxList[85][8]);

    bt.resetAndStart();
    ZImgConnectedComponents conncompWithProgress;
    double lastProgress = 0.0;
    size_t progressCalls = 0;
    conncompWithProgress.setProgressCallback([&lastProgress, &progressCalls](double p) {
      lastProgress = p;
      ++progressCalls;
    });
    CC = conncompWithProgress.run(img);
    STOP_AND_LOG(bt)

    EXPECT_DOUBLE_EQ(lastProgress, 1.0);
    EXPECT_GT(progressCalls, 0_uz);

    ASSERT_EQ(size_t(8), CC.connectivity);
    ASSERT_EQ(size_t(88), CC.voxelIdxList.size());

    ASSERT_EQ(size_t(66), CC.voxelIdxList[0].size());
    ASSERT_EQ(size_t(80), CC.voxelIdxList[1].size());
    ASSERT_EQ(size_t(48), CC.voxelIdxList[2].size());
    ASSERT_EQ(size_t(48), CC.voxelIdxList[3].size());
    ASSERT_EQ(size_t(80), CC.voxelIdxList[4].size());
    ASSERT_EQ(size_t(85), CC.voxelIdxList[5].size());
    ASSERT_EQ(size_t(63), CC.voxelIdxList[6].size());
    ASSERT_EQ(size_t(9), CC.voxelIdxList[85].size());
    ASSERT_EQ(size_t(41), CC.voxelIdxList[86].size());

    ASSERT_EQ(size_t(60582), CC.voxelIdxList[85][0]);
    ASSERT_EQ(size_t(60583), CC.voxelIdxList[85][1]);
    ASSERT_EQ(size_t(60584), CC.voxelIdxList[85][2]);
    ASSERT_EQ(size_t(60838), CC.voxelIdxList[85][3]);
    ASSERT_EQ(size_t(60839), CC.voxelIdxList[85][4]);
    ASSERT_EQ(size_t(60840), CC.voxelIdxList[85][5]);
    ASSERT_EQ(size_t(61094), CC.voxelIdxList[85][6]);
    ASSERT_EQ(size_t(61095), CC.voxelIdxList[85][7]);
    ASSERT_EQ(size_t(61096), CC.voxelIdxList[85][8]);
  }
  catch (const ZException& e) {
    LOG(WARNING) << e.what();
  }
}
