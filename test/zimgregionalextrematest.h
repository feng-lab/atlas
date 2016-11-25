#pragma once

#include "zimgregionalextrema.h"
#include "zbenchtimer.h"
#include "gtest/gtest.h"

TEST(ZImgRegionalExtrema, max)
{
  using namespace nim;

  ZImgInfo info(10, 10, 1);
  ZImg img(info);
  img.fill(10);

  ZImgRegion rgn1(1,4,1,4);
  for (ZImgRegionIterator<uint8_t> it = ZImgRegionIterator<uint8_t>(img, rgn1);
       !it.isAtEnd(); ++it) {
    *it = 22;
  }
  ZImgRegion rgn2(5,8,5,8);
  for (ZImgRegionIterator<uint8_t> it = ZImgRegionIterator<uint8_t>(img, rgn2);
       !it.isAtEnd(); ++it) {
    *it = 33;
  }
  img.setValue(44, ZVoxelCoordinate(1, 6));
  img.setValue(45, ZVoxelCoordinate(2, 7));
  img.setValue(44, ZVoxelCoordinate(3, 8));

  ZImgRegionalExtrema<> regionalExtrema;
  ZImg mask = regionalExtrema.regionalMax(img);
  for (ZImgRegionConstIterator<uint8_t> it = ZImgRegionConstIterator<uint8_t>(mask);
       !it.isAtEnd(); ++it) {
    ZVoxelCoordinate coord = it.coord();
    if (rgn1.containsCoord(coord, info) || rgn2.containsCoord(coord, info) ||
        (coord.x == 2 && coord.y == 7)) {
      ASSERT_EQ(1, *it) << qUtf8Printable(coord.toQString());
    } else {
      ASSERT_EQ(0, *it) << qUtf8Printable(coord.toQString());
    }
  }
}

TEST(ZImgRegionalExtrema, min)
{
  using namespace nim;

  ZImgInfo info(10, 10, 1);
  ZImg img(info);
  img.fill(10);

  ZImgRegion rgn1(1,4,1,4);
  for (ZImgRegionIterator<uint8_t> it = ZImgRegionIterator<uint8_t>(img, rgn1);
       !it.isAtEnd(); ++it) {
    *it = 2;
  }
  ZImgRegion rgn2(5,8,5,8);
  for (ZImgRegionIterator<uint8_t> it = ZImgRegionIterator<uint8_t>(img, rgn2);
       !it.isAtEnd(); ++it) {
    *it = 7;
  }

  ZImgRegionalExtrema<> regionalExtrema;
  ZImg mask = regionalExtrema.regionalMin(img);
  for (ZImgRegionConstIterator<uint8_t> it = ZImgRegionConstIterator<uint8_t>(mask);
       !it.isAtEnd(); ++it) {
    ZVoxelCoordinate coord = it.coord();
    if (rgn1.containsCoord(coord, info) || rgn2.containsCoord(coord, info)) {
      ASSERT_EQ(1, *it) << qUtf8Printable(coord.toQString());
    } else {
      ASSERT_EQ(0, *it) << qUtf8Printable(coord.toQString());
    }
  }
}

TEST(ZImgRegionalExtrema, max2d1)
{
  using namespace nim;

  try {
    ZImg img(GET_TEST_DATA_DIR.filePath("img/im2d1.tif"));
    ZImg res(GET_TEST_DATA_DIR.filePath("img/im2d1maxres.tif"));

    ZBenchTimer bt;
    bt.start();
    ZImgRegionalExtrema<> regionalExtrema;
    ZImg mask = regionalExtrema.regionalMax(img);
    STOP_AND_LOG(bt);

    ASSERT_TRUE(res == mask);

    bt.reset();
    bt.start();
    ZImgRegionalExtrema<true> regionalExtrema1;
    mask = regionalExtrema1.regionalMax(img);
    STOP_AND_LOG(bt);

    ASSERT_TRUE(res == mask);
  }
  catch (const ZIOException & e) {
    LOG(WARNING) << e.what();
  }
}

TEST(ZImgRegionalExtrema, min2d1)
{
  using namespace nim;

  try {
    ZImg img(GET_TEST_DATA_DIR.filePath("img/im2d1.tif"));
    ZImg res(GET_TEST_DATA_DIR.filePath("img/im2d1minres.tif"));

    ZBenchTimer bt;
    bt.start();
    ZImgRegionalExtrema<> regionalExtrema;
    ZImg mask = regionalExtrema.regionalMin(img);
    STOP_AND_LOG(bt);

    ASSERT_TRUE(res == mask);

    bt.reset();
    bt.start();
    ZImgRegionalExtrema<true> regionalExtrema1;
    mask = regionalExtrema1.regionalMin(img);
    STOP_AND_LOG(bt);

    ASSERT_TRUE(res == mask);
  }
  catch (const ZIOException & e) {
    LOG(WARNING) << e.what();
  }
}

TEST(ZImgRegionalExtrema, max3d1)
{
  using namespace nim;

  try {
    ZImg img(GET_TEST_DATA_DIR.filePath("img/im3d1.tif"));
    ZImg res(GET_TEST_DATA_DIR.filePath("img/im3d1maxres.tif"));

    ZBenchTimer bt;
    bt.start();
    ZImgRegionalExtrema<> regionalExtrema;
    ZImg mask = regionalExtrema.regionalMax(img);
    STOP_AND_LOG(bt);

    ASSERT_TRUE(res == mask);

    bt.reset();
    bt.start();
    ZImgRegionalExtrema<true> regionalExtrema1;
    mask = regionalExtrema1.regionalMax(img);
    STOP_AND_LOG(bt);

    ASSERT_TRUE(res == mask);
  }
  catch (const ZIOException & e) {
    LOG(WARNING) << e.what();
  }
}

TEST(ZImgRegionalExtrema, min3d1)
{
  using namespace nim;

  try {
    ZImg img(GET_TEST_DATA_DIR.filePath("img/im3d1.tif"));
    ZImg res(GET_TEST_DATA_DIR.filePath("img/im3d1minres.tif"));

    ZBenchTimer bt;
    bt.start();
    ZImgRegionalExtrema<> regionalExtrema;
    ZImg mask = regionalExtrema.regionalMin(img);
    STOP_AND_LOG(bt);

    ASSERT_TRUE(res == mask);

    bt.reset();
    bt.start();
    ZImgRegionalExtrema<true> regionalExtrema1;
    mask = regionalExtrema1.regionalMin(img);
    STOP_AND_LOG(bt);

    ASSERT_TRUE(res == mask);
  }
  catch (const ZIOException & e) {
    LOG(WARNING) << e.what();
  }
}

