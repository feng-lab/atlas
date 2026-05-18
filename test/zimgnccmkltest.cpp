#include "zimgncc.h"
#include "zcommandlineflags.h"
#include "zfft.h"
#include "ztest.h"
#include <folly/ScopeGuard.h>

ABSL_DECLARE_FLAG(bool, zimg_use_mkl_for_fft_if_available);
ABSL_DECLARE_FLAG(uint32_t, zimg_global_fft_number_of_threads);

#if defined(ZIMG_USE_MKL) && (defined(__x86_64__) || defined(_M_X64) || defined(__amd64__))

TEST(ZImgNCC, normXCorr_S_mkl)
{
  using namespace nim;

  auto oldValue = absl::GetFlag(FLAGS_zimg_use_mkl_for_fft_if_available);
  auto guard = folly::makeGuard([=]() {
    absl::SetFlag(&FLAGS_zimg_use_mkl_for_fft_if_available, oldValue);
  });

  auto oldValue1 = absl::GetFlag(FLAGS_zimg_global_fft_number_of_threads);
  auto guard1 = folly::makeGuard([=]() {
    absl::SetFlag(&FLAGS_zimg_global_fft_number_of_threads, oldValue1);
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

    absl::SetFlag(&FLAGS_zimg_global_fft_number_of_threads, 1);
    absl::SetFlag(&FLAGS_zimg_use_mkl_for_fft_if_available, false);
    ZBenchTimer bt("normXCorr_S_pocketfft_one_thread");
    normXCorr_S(fixedImgView, movingImgView, nccImg, numberOfOverlapVoxelsImg);
    STOP_AND_LOG(bt)

    absl::SetFlag(&FLAGS_zimg_use_mkl_for_fft_if_available, true);
    fixedImgView = fixedImg.createView();
    movingImgView = movingImg.createView();
    bt.resetAndStart("normXCorr_S_mkl_one_thread");
    normXCorr_S(fixedImgView, movingImgView, nccImg, numberOfOverlapVoxelsImg);
    STOP_AND_LOG(bt)

    absl::SetFlag(&FLAGS_zimg_global_fft_number_of_threads, 0);
    absl::SetFlag(&FLAGS_zimg_use_mkl_for_fft_if_available, false);
    fixedImgView = fixedImg.createView();
    movingImgView = movingImg.createView();
    bt.resetAndStart("normXCorr_S_pocketfft");
    normXCorr_S(fixedImgView, movingImgView, nccImg, numberOfOverlapVoxelsImg);
    STOP_AND_LOG(bt)

    absl::SetFlag(&FLAGS_zimg_use_mkl_for_fft_if_available, true);
    fixedImgView = fixedImg.createView();
    movingImgView = movingImg.createView();
    bt.resetAndStart("normXCorr_S_mkl");
    normXCorr_S(fixedImgView, movingImgView, nccImg, numberOfOverlapVoxelsImg);
    STOP_AND_LOG(bt)

    // ZVoxelCoordinate cd;
    // while (cd.advance(nccImg.info())) {
    //   LOG(INFO) << nccImg.info().toString() << cd.toString();
    //   LOG(INFO) << *nccImg.data<double>(cd);
    // }

    for (size_t i = 0; i < 1000; ++i) {
      // check a random location
      ZVoxelCoordinate coord = ZVoxelCoordinate::random(nccImg.info());
      ZVoxelCoordinate offset = coord - ZVoxelCoordinate::lastCoordinate(movingImg.info());
      double ncc = getNCCOfOffset(fixedImg, movingImg, offset);
      // LOG(INFO) << nccImg.info().toString() << coord.toString();
      // LOG(INFO) << *nccImg.data<double>(coord);
      EXPECT_NEAR(ncc, *nccImg.data<double>(coord), 1e-8);
      // LOG(INFO) << "1";
    }
  }
  catch (const ZException& e) {
    LOG(ERROR) << "caught Exception: " << e.what();
  }
}

TEST(ZImgNCC, normXCorr_mkl)
{
  using namespace nim;

  auto oldValue = absl::GetFlag(FLAGS_zimg_use_mkl_for_fft_if_available);
  auto guard = folly::makeGuard([=]() {
    absl::SetFlag(&FLAGS_zimg_use_mkl_for_fft_if_available, oldValue);
  });

  auto oldValue1 = absl::GetFlag(FLAGS_zimg_global_fft_number_of_threads);
  auto guard1 = folly::makeGuard([=]() {
    absl::SetFlag(&FLAGS_zimg_global_fft_number_of_threads, oldValue1);
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

    absl::SetFlag(&FLAGS_zimg_global_fft_number_of_threads, 1);
    absl::SetFlag(&FLAGS_zimg_use_mkl_for_fft_if_available, false);
    ZBenchTimer bt("normXCorr_pocketfft_one_thread");
    normXCorr(fixedImgView, movingImgView, nccImg, numberOfOverlapVoxelsImg);
    STOP_AND_LOG(bt)

    absl::SetFlag(&FLAGS_zimg_use_mkl_for_fft_if_available, true);
    fixedImgView = fixedImg.createView();
    movingImgView = movingImg.createView();
    bt.resetAndStart("normXCorr_mkl_one_thread");
    normXCorr(fixedImgView, movingImgView, nccImg, numberOfOverlapVoxelsImg);
    STOP_AND_LOG(bt)

    absl::SetFlag(&FLAGS_zimg_global_fft_number_of_threads, 0);
    absl::SetFlag(&FLAGS_zimg_use_mkl_for_fft_if_available, false);
    fixedImgView = fixedImg.createView();
    movingImgView = movingImg.createView();
    bt.resetAndStart("normXCorr_pocketfft");
    normXCorr(fixedImgView, movingImgView, nccImg, numberOfOverlapVoxelsImg);
    STOP_AND_LOG(bt)

    absl::SetFlag(&FLAGS_zimg_use_mkl_for_fft_if_available, true);
    fixedImgView = fixedImg.createView();
    movingImgView = movingImg.createView();
    bt.resetAndStart("normXCorr_mkl");
    normXCorr(fixedImgView, movingImgView, nccImg, numberOfOverlapVoxelsImg);
    STOP_AND_LOG(bt)

    for (size_t i = 0; i < 1000; ++i) {
      // check a random location
      ZVoxelCoordinate coord = ZVoxelCoordinate::random(nccImg.info());
      ZVoxelCoordinate offset = coord - ZVoxelCoordinate::lastCoordinate(movingImg.info());
      double ncc = getNCCOfOffset(fixedImg, movingImg, offset);
      // LOG(INFO) << nccImg.info().toString() << coord.toString();
      // LOG(INFO) << *nccImg.data<double>(coord);
      EXPECT_NEAR(ncc, *nccImg.data<double>(coord), 1e-8);
      // LOG(INFO) << "1";
    }
  }
  catch (const ZException& e) {
    LOG(ERROR) << "caught Exception: " << e.what();
  }
}

TEST(ZImgNCC, fft_mkl_pocketfft)
{
  using namespace nim;

  auto oldValue = absl::GetFlag(FLAGS_zimg_use_mkl_for_fft_if_available);
  auto guard = folly::makeGuard([=]() {
    absl::SetFlag(&FLAGS_zimg_use_mkl_for_fft_if_available, oldValue);
  });

  try {
    ZImg fixedImg(ZImgInfo(512, 427, 20));
    fixedImg.fillRandom();

    absl::SetFlag(&FLAGS_zimg_use_mkl_for_fft_if_available, true);
    auto cimg_mkl = fft(fixedImg, 512, 427, 20);
    absl::SetFlag(&FLAGS_zimg_use_mkl_for_fft_if_available, false);
    auto cimg_pocketfft = fft(fixedImg, 512, 427, 20);

    for (size_t i = 0; i < 1000; ++i) {
      // check a random location
      ZVoxelCoordinate coord =
        ZVoxelCoordinate::random(std::array<size_t, 5>({cimg_mkl.width(), cimg_mkl.height(), cimg_mkl.depth(), 1, 1}));
      auto mkl = cimg_mkl.rawData()[coord.x + coord.y * cimg_mkl.width() + coord.z * cimg_mkl.width() * cimg_mkl.height()];
      auto pfft = cimg_pocketfft.rawData()[coord.x + coord.y * cimg_mkl.width() + coord.z * cimg_mkl.width() * cimg_mkl.height()];
      EXPECT_NEAR(mkl.real(), pfft.real(), 1e-7);
      EXPECT_NEAR(mkl.imag(), pfft.imag(), 1e-7);
      // LOG(INFO) << "1";
    }
  }
  catch (const ZException& e) {
    LOG(ERROR) << "caught Exception: " << e.what();
  }
}

#endif // ZIMG_MKL_ENABLED
