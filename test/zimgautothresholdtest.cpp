#include "zimgautothreshold.h"
#include "ztest.h"

TEST(ZImgAutoThreshold, img0515)
{
  using namespace nim;

  try {
    ZImg img(getTestDataDir().filePath("img/im3d1.tif"));

    ZBenchTimer bt;
    ZImgAutoThreshold<> autothre;
    int thre = autothre.triangleThre<int>(img, 0);
    ASSERT_EQ(32, thre);
    STOP_AND_LOG(bt)
  }
  catch (const ZIOException & e) {
    LOG(WARNING) << e.what();
  }
}

