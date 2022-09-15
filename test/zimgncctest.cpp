#include "zimgncc.h"
#include "zrandom.h"
#include "ztest.h"

TEST(ZImgNCC, normXCorr_S)
{
  using namespace nim;

  try {
    ZImg fixedImg(ZImgInfo(512, 426, 20));
    fixedImg.fillRandom();
    ZImg movingImg(ZImgInfo(467, 580, 16));
    movingImg.fillRandom();
    ZImg fixedImgView = fixedImg.createView();
    ZImg movingImgView = movingImg.createView();

    ZImg nccImg;
    ZImg numberOfOverlapVoxelsImg;
    normXCorr_S(fixedImgView, movingImgView, nccImg, numberOfOverlapVoxelsImg);

    //ZVoxelCoordinate cd;
    //while (cd.advance(nccImg.info())) {
    //  LOG(INFO) << nccImg.info().toQString() << cd.toQString();
    //  LOG(INFO) << *nccImg.data<double>(cd);
    //}

    for (size_t i=0; i<1000; ++i) {
      // check a random location
      ZVoxelCoordinate coord = ZVoxelCoordinate::random(nccImg.info());
      ZVoxelCoordinate offset = coord - ZVoxelCoordinate::lastCoordinate(movingImg.info());
      double ncc = getNCCOfOffset(fixedImg, movingImg, offset);
      //LOG(INFO) << nccImg.info().toQString() << coord.toQString();
      //LOG(INFO) << *nccImg.data<double>(coord);
      EXPECT_NEAR(ncc, *nccImg.data<double>(coord), 1e-8);
      //LOG(INFO) << "1";
    }
  }
  catch (const ZException & e) {
    LOG(ERROR) << "caught Exception: " << e.what();
  }
}

TEST(ZImgNCC, normXCorr)
{
  using namespace nim;

  try {
    ZImg fixedImg(ZImgInfo(512, 426, 20));
    fixedImg.fillRandom();
    ZImg movingImg(ZImgInfo(467, 580, 16));
    movingImg.fillRandom();
    ZImg fixedImgView = fixedImg.createView();
    ZImg movingImgView = movingImg.createView();

    ZImg nccImg;
    ZImg numberOfOverlapVoxelsImg;
    normXCorr(fixedImgView, movingImgView, nccImg, numberOfOverlapVoxelsImg);

    for (size_t i=0; i<1000; ++i) {
      // check a random location
      ZVoxelCoordinate coord = ZVoxelCoordinate::random(nccImg.info());
      ZVoxelCoordinate offset = coord - ZVoxelCoordinate::lastCoordinate(movingImg.info());
      double ncc = getNCCOfOffset(fixedImg, movingImg, offset);
      //LOG(INFO) << nccImg.info().toQString() << coord.toQString();
      //LOG(INFO) << *nccImg.data<double>(coord);
      EXPECT_NEAR(ncc, *nccImg.data<double>(coord), 1e-8);
      //LOG(INFO) << "1";
    }
  }
  catch (const ZException & e) {
    LOG(ERROR) << "caught Exception: " << e.what();
  }
}

TEST(ZImgNCC, normXCorrPart)
{
  using namespace nim;

  try {
    ZImg fixedImg(ZImgInfo(512, 426, 20));
    fixedImg.fillRandom();
    ZImg movingImg(ZImgInfo(467, 580, 16));
    movingImg.fillRandom();
    ZImg fixedImgView;
    ZImg movingImgView;

    ZImg nccImg;
    ZImg numberOfOverlapVoxelsImg;

    for (size_t i=0; i<10; ++i) {
      // create random
      size_t partWidth = 5;
      size_t partHeight = 7;
      size_t partDepth = 3;
      size_t xStart = ZRandom::instance().randInt<size_t>(fixedImg.width() + movingImg.width() - 1 - partWidth);
      size_t xEnd = xStart + partWidth;
      size_t yStart = ZRandom::instance().randInt<size_t>(fixedImg.height() + movingImg.height() - 1 - partHeight);
      size_t yEnd = yStart + partHeight;
      size_t zStart = ZRandom::instance().randInt<size_t>(fixedImg.depth() + movingImg.depth() - 1 - partDepth);
      size_t zEnd = zStart + partDepth;
      fixedImgView = fixedImg.createView();
      movingImgView = movingImg.createView();
      normXCorrPart(fixedImgView, movingImgView, xStart, xEnd, yStart, yEnd, zStart, zEnd, nccImg, numberOfOverlapVoxelsImg);
      EXPECT_EQ(partWidth, nccImg.width());
      EXPECT_EQ(partHeight, nccImg.height());
      EXPECT_EQ(partDepth, nccImg.depth());
      // check all
      for (size_t z=0; z<partDepth; ++z) {
        for (size_t y=0; y<partHeight; ++y) {
          for (size_t x=0; x<partWidth; ++x) {
            ZVoxelCoordinate coord(x,y,z);
            ZVoxelCoordinate offset = coord + ZVoxelCoordinate(xStart, yStart, zStart) - ZVoxelCoordinate::lastCoordinate(movingImg.info());;
            double ncc = getNCCOfOffset(fixedImg, movingImg, offset);
            EXPECT_NEAR(ncc, *nccImg.data<double>(coord), 1e-8) << qUtf8Printable(offset.toQString()) << qUtf8Printable(coord.toQString());
          }
        }
      }
    }
  }
  catch (const ZException & e) {
    LOG(ERROR) << "caught Exception: " << e.what();
  }
}
