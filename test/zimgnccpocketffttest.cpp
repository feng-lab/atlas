#include "zimgncc.h"
#include "ztest.h"
#include <folly/ScopeGuard.h>

DECLARE_bool(zimg_use_mkl_for_fft_if_available);

TEST(ZImgNCC, normXCorr_S_pocketfft)
{
  using namespace nim;

  auto oldValue = FLAGS_zimg_use_mkl_for_fft_if_available;
  FLAGS_zimg_use_mkl_for_fft_if_available = false;
  auto guard = folly::makeGuard([=]() {
    FLAGS_zimg_use_mkl_for_fft_if_available = oldValue;
  });

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

TEST(ZImgNCC, normXCorr_pocketfft)
{
  using namespace nim;

  auto oldValue = FLAGS_zimg_use_mkl_for_fft_if_available;
  FLAGS_zimg_use_mkl_for_fft_if_available = false;
  auto guard = folly::makeGuard([=]() {
    FLAGS_zimg_use_mkl_for_fft_if_available = oldValue;
  });

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
