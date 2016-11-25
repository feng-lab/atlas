#pragma once

#include "gtest/gtest.h"

#include "zbenchtimer.h"
#include "zimgregioniterator.h"

namespace {

inline uint8_t add5(uint8_t current)
{
  return current + 5;
}

inline uint8_t addOther(uint8_t current, uint8_t other)
{
  return current + other;
}

}  // namespace

TEST(Img, UnaryOperator)
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

  ZImg img1 = img;
  img1.unaryOperation([](auto voxel) { return voxel + 5; });

  ZImgRegionIterator<uint8_t> it = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it1 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it.isAtEnd(); ++it, ++it1) {
    ASSERT_EQ(*it+5, *it1);
  }
}

TEST(Img, TypedUnaryOperator)
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

  ZImg img1 = img;
  img1.typedUnaryOperation<uint8_t>(add5);

  ZImgRegionIterator<uint8_t> it = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it1 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it.isAtEnd(); ++it, ++it1) {
    ASSERT_EQ(*it+5, *it1);
  }
}

TEST(Img, BinaryOperator)
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

  ZImg img1 = img;
  img1.binaryOperation(img, [](auto current, auto other) { return current + other; });

  ZImgRegionIterator<uint8_t> it = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it1 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it.isAtEnd(); ++it, ++it1) {
    ASSERT_EQ((*it) * 2, *it1);
  }
}

TEST(Img, TypedBinaryOperator)
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

  ZImg img1 = img;
  img1.typedBinaryOperation<uint8_t, uint8_t>(img, addOther);

  ZImgRegionIterator<uint8_t> it = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it1 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it.isAtEnd(); ++it, ++it1) {
    ASSERT_EQ((*it) * 2, *it1);
  }
}

TEST(Img, fill)
{
  using namespace nim;

  ZImgInfo info(30, 30, 2, 2, 1);
  ZImg img(info);
  img.fill(3);
  ZImgRegionConstIterator<uint8_t> lit = ZImgRegionIterator<uint8_t>(img);
  for (;!lit.isAtEnd(); ++lit) {
    ASSERT_EQ(3, *lit);
  }
  ASSERT_EQ(img.voxelNumber(), static_cast<size_t>(lit - ZImgRegionIterator<uint8_t>(img)));

  info.bytesPerVoxel = 4;
  img = ZImg(info);
  img.fill(68984535);
  for (ZImgRegionConstIterator<uint32_t> it = ZImgRegionIterator<uint32_t>(img);
       !it.isAtEnd(); ++it) {
    ASSERT_EQ(68984535_u32, *it);
  }

  info.voxelFormat = VoxelFormat::Signed;
  img = ZImg(info);
  img.fill(-368984535);
  for (ZImgRegionConstIterator<int32_t> it = ZImgRegionIterator<int32_t>(img);
       !it.isAtEnd(); ++it) {
    ASSERT_EQ(-368984535, *it);
  }

  info.bytesPerVoxel = 1;
  img = ZImg(info);
  img.fill(-100);
  for (ZImgRegionConstIterator<int8_t> it = ZImgRegionIterator<int8_t>(img);
       !it.isAtEnd(); ++it) {
    ASSERT_EQ(-100, *it);
  }

  info.voxelFormat = VoxelFormat::Float;
  info.bytesPerVoxel = 4;
  img = ZImg(info);
  img.fill(-1.02);
  for (ZImgRegionConstIterator<float> it = ZImgRegionIterator<float>(img);
       !it.isAtEnd(); ++it) {
    ASSERT_NEAR(-1.02, *it, std::numeric_limits<float>::epsilon());
  }

  info.bytesPerVoxel = 8;
  img = ZImg(info);
  img.fill(349.84);
  for (ZImgRegionConstIterator<double> it = ZImgRegionIterator<double>(img);
       !it.isAtEnd(); ++it) {
    ASSERT_NEAR(349.84, *it, std::numeric_limits<double>::epsilon());
  }
}

TEST(Img, OperatorAdd)
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

  ZImg imgOld = img;

  ZImg img1 = img + 5;

  ZImgRegionIterator<uint8_t> it = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it1 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it.isAtEnd(); ++it, ++it1) {
    ASSERT_EQ(*it+5, *it1);
  }

  img += 5;
  ASSERT_TRUE(img1 == img);

  img1 = img + 220;  // will saturate

  ZImgRegionIterator<uint8_t> it3 = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it4 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it3.isAtEnd(); ++it3, ++it4) {
    ASSERT_EQ(static_cast<uint8_t>(std::min(255,int(*it3)+220)), *it4);
  }

  // test SSE2 version
  img = imgOld;
  img1 = img + 5_u8;

  ZImgRegionIterator<uint8_t> it5 = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it6 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it5.isAtEnd(); ++it5, ++it6) {
    ASSERT_EQ(*it5+5, *it6);
  }

  img += 5_u8;
  ASSERT_TRUE(img1 == img);

  img1 = img + 220_u8;  // will saturate

  ZImgRegionIterator<uint8_t> it7 = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it8 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it7.isAtEnd(); ++it7, ++it8) {
    ASSERT_EQ(static_cast<uint8_t>(std::min(255,int(*it7)+220)), *it8);
  }
}

TEST(Img, OperatorSub)
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

  ZImg imgOld = img;

  ZImg img1 = img - 5;

  ZImgRegionIterator<uint8_t> it = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it1 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it.isAtEnd(); ++it, ++it1) {
    ASSERT_EQ(*it-5, *it1);
  }

  img -= 5;
  ASSERT_TRUE(img1 == img);

  img1 = img - 10;  // will saturate

  ZImgRegionIterator<uint8_t> it3 = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it4 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it3.isAtEnd(); ++it3, ++it4) {
    ASSERT_EQ(static_cast<uint8_t>(std::max(0,int(*it3)-10)), *it4);
  }

  // test SSE2 version
  img = imgOld;
  img1 = img - 5_u8;

  ZImgRegionIterator<uint8_t> it5 = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it6 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it5.isAtEnd(); ++it5, ++it6) {
    ASSERT_EQ(*it5-5, *it6);
  }

  img -= 5_u8;
  ASSERT_TRUE(img1 == img);

  img1 = img - 10_u8;  // will saturate

  ZImgRegionIterator<uint8_t> it7 = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it8 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it7.isAtEnd(); ++it7, ++it8) {
    ASSERT_EQ(static_cast<uint8_t>(std::max(0,int(*it7)-10)), *it8);
  }
}

TEST(Img, OperatorMul)
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

  ZImg img1 = img * 5;

  ZImgRegionIterator<uint8_t> it = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it1 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it.isAtEnd(); ++it, ++it1) {
    ASSERT_EQ(*it * 5, *it1);
  }

  img *= 5;
  ASSERT_TRUE(img1 == img);

  img1 = img * 2;  // will saturate

  ZImgRegionIterator<uint8_t> it3 = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it4 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it3.isAtEnd(); ++it3, ++it4) {
    ASSERT_EQ(static_cast<uint8_t>(std::min(255,int(*it3)*2)), *it4);
  }

  img1 = img * (-1);  // will be all zero
  img.fill(0);
  ASSERT_TRUE(img1 == img);
}

TEST(Img, OperatorDiv)
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

  ZImg img1 = img / 2;

  ZImgRegionIterator<uint8_t> it = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it1 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it.isAtEnd(); ++it, ++it1) {
    ASSERT_EQ(*it / 2, *it1);
  }

  img /= 2;
  ASSERT_TRUE(img1 == img);

  img1 = img / .01;  // will saturate

  ZImgRegionIterator<uint8_t> it3 = ZImgRegionIterator<uint8_t>(img);
  ZImgRegionIterator<uint8_t> it4 = ZImgRegionIterator<uint8_t>(img1);
  for (; !it3.isAtEnd(); ++it3, ++it4) {
    ASSERT_EQ(static_cast<uint8_t>(std::min(255.,(*it3)/.01)), *it4);
  }

  img1 = img / (-1);  // will be all zero
  img.fill(0);
  ASSERT_TRUE(img1 == img);
}

