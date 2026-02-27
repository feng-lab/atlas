#include "zimgsigneddistancemap.h"
#include "zimgregioniterator.h"
#include "ztest.h"

TEST(ZImgSignedDistanceMap, test1)
{
  using namespace nim;

  ZImgInfo info(10, 10, 1);
  ZImg img(info);

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
  //LOG(INFO) << img.showContentAsQString();

  alignas(32) int res[] = {2, 1, 1, 1, 2, 5, 10, 17, 26, 29,
                           1, 0, 0, 0, 1, 4, 9, 16, 17, 20,
                           1, 0, -1, 0, 1, 4, 9, 9, 10, 13,
                           1, 0, 0, 0, 1, 4, 4, 4, 5, 8,
                           2, 1, 1, 1, 2, 1, 1, 1, 2, 5,
                           2, 1, 2, 4, 1, 0, 0, 0, 1, 4,
                           1, 0, 1, 2, 1, 0, -1, 0, 1, 4,
                           2, 1, 0, 1, 1, 0, 0, 0, 1, 4,
                           5, 2, 1, 0, 1, 1, 1, 1, 2, 5,
                           8, 5, 2, 1, 2, 4, 4, 4, 5, 8};
  ZImg resSquaredImg;
  resSquaredImg.wrapData(res, img.width(), img.height(), img.depth());

  ZImg resSquaredImgDouble = resSquaredImg.castTo<double>();
  ZImg resImgDouble = resSquaredImgDouble;
  resImgDouble.typedUnaryOperation<double>([](double v) { return std::sqrt(v); });
  *resImgDouble.data<double>(2,2) = -1.0;
  *resImgDouble.data<double>(6,6) = -1.0;

  ZImgSignedDistanceMap signedDM;
  ZImg dm = signedDM.run<double>(img);
  double* expDData = resImgDouble.timeData<double>(0);
  double* dData = dm.timeData<double>(0);
  for (size_t i=0; i<dm.voxelNumber(); ++i) {
    ASSERT_NEAR(expDData[i], dData[i], 1e-13);
  }
  //LOG(INFO) << dm.showContentAsQString();

  signedDM.setUseSquaredDistance(true);
  dm = signedDM.run<double>(img);
  expDData = resSquaredImgDouble.timeData<double>(0);
  dData = dm.timeData<double>(0);
  for (size_t i=0; i<dm.voxelNumber(); ++i) {
    ASSERT_NEAR(expDData[i], dData[i], 1e-13);
  }
  //LOG(INFO) << dm.showContentAsQString();

//  dm = signedDM.run<int>(img);
//  int* expData = resSquaredImg.timeData<int>(0);
//  int *data = dm.timeData<int>(0);
//  for (size_t i=0; i<dm.voxelNumber(); ++i) {
//    ASSERT_EQ(expData[i], data[i]);
//  }
  //LOG(INFO) << dm.showContentAsQString();

  signedDM.setInsideIsPositive(true);
//  dm = signedDM.run<int>(img);
//  resSquaredImg.typedUnaryOperation<int>(std::negate<int>());
//  expData = resSquaredImg.timeData<int>(0);
//  data = dm.timeData<int>(0);
//  for (size_t i=0; i<dm.voxelNumber(); ++i) {
//    ASSERT_EQ(expData[i], data[i]);
//  }
  //LOG(INFO) << dm.showContentAsQString();

  signedDM.setUseSquaredDistance(false);
  dm = signedDM.run<double>(img);
  resImgDouble.typedUnaryOperation<double>(std::negate<double>());
  expDData = resImgDouble.timeData<double>(0);
  dData = dm.timeData<double>(0);
  for (size_t i=0; i<dm.voxelNumber(); ++i) {
    ASSERT_NEAR(expDData[i], dData[i], 1e-13);
  }
  //LOG(INFO) << dm.showContentAsQString();
}
