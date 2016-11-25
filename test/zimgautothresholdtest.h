#pragma once

#include "zimgautothreshold.h"
#include "gtest/gtest.h"

TEST(ZImgAutoThreshold, img0515)
{
  using namespace nim;

  try {
    ZImg img(GET_TEST_DATA_DIR.filePath("img/im3d1.tif"));

    ZBenchTimer bt;
    bt.start();
    ZImgAutoThreshold<> autothre;
    int thre = autothre.triangleThre<int>(img, 0);
    ASSERT_EQ(32, thre);
    STOP_AND_LOG(bt);
  }
  catch (const ZIOException & e) {
    LOG(WARNING) << e.what();
  }
}

