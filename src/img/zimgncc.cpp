#include "zimgncc.h"

#include "zlog.h"
#include "zfft.h"
#include "zimagetoimagemetric.h"
#include <algorithm>

namespace {

size_t factorizeNumber(size_t n)
{
  size_t ifac[3] = {2, 3, 5};
  for (auto i : ifac) {
    while (n % i == 0) {
      n = n / i;
    }
  }
  return n;
}

size_t findClosestValidDimension(size_t n)
{
  while (factorizeNumber(n) != 1) {
    ++n;
  }
  return n;
}

double removeNegative(double v)
{
  return std::max(0.0, v);
}

double secureDivideSqrt(double v1, double v2)
{
  v2 = std::sqrt(v2);
  if (v2 > 1e-8) {
    return v1 / v2;
  }
  return 0.0;
}

double secureDivideSqrt2(double v1, double v2)
{
  v2 = std::sqrt(v2);
  if (v2 > 1e-8) {
    return std::max(-1.0, std::min(1.0, v1 / v2));
  } else {
    return 0.0;
  }
}

using namespace nim;

void checkInputImgs(const ZImg& fixedImg, const ZImg& movingImg, const QString& name = "")
{
  if (fixedImg.isEmpty() || movingImg.isEmpty() || fixedImg.numChannels() != 1 || fixedImg.numTimes() != 1 ||
      movingImg.numChannels() != 1 || movingImg.numTimes() != 1) {
    throw ZException(fmt::format("{} input img dimension is not supported: fixed img <{}>, moving img <{}>",
                                 name,
                                 fixedImg.info(),
                                 movingImg.info()));
  }
}

} // namespace

namespace nim {

double getNCCOfOffset(const ZImg& fixedImgIn, const ZImg& movingImgIn, const ZVoxelCoordinate& offset)
{
  checkInputImgs(fixedImgIn, movingImgIn, "getNCCOfOffset");
  ZImg fixedImg;
  ZImg movingImg;
  cropOverlapSubImg(fixedImgIn, movingImgIn, offset, fixedImg, movingImg);

  if (!fixedImg.isSameType(movingImg)) {
    if (!fixedImg.isType<double>()) {
      fixedImg = fixedImg.castTo<double>();
    }
    if (!movingImg.isType<double>()) {
      movingImg = movingImg.castTo<double>();
    }
  }
  return imgTypeDispatcher(fixedImg.info(), [&]<typename TVoxel>() {
    ZImageToImageMetric metric;
    metric.setType(ZImageToImageMetric::Type::NormalizedCrossCorrelation);
    return -metric.value(fixedImg.channelData<TVoxel>(0),
                         movingImg.channelData<TVoxel>(0),
                         fixedImg.width(),
                         fixedImg.height(),
                         fixedImg.depth());
  });
}

// reference: matlab code normxcorr2_general.m
void normXCorr(ZImg& fixedImg, ZImg& movingImg, ZImg& nccImg, ZImg& numberOfOverlapVoxelsImg)
{
  checkInputImgs(fixedImg, movingImg, "normXCorr");
  if (!fixedImg.isType<double>()) {
    fixedImg = fixedImg.castTo<double>();
  }
  if (!movingImg.isType<double>()) {
    movingImg = movingImg.castTo<double>();
  }

  movingImg.reflect();

  // VLOG(1) << movingImg.info() << " " << fixedImg.info();

  ZImgInfo info = fixedImg.info();
  numberOfOverlapVoxelsImg = ZImg(info); // 1
  numberOfOverlapVoxelsImg.fill(1);
  numberOfOverlapVoxelsImg =
    numberOfOverlapVoxelsImg.blockSum(movingImg.width(), movingImg.height(), movingImg.depth());

  // fixedImg.write("~/Downloads/test_xcorr_fixedImg.tif");
  // movingImg.write("~/Downloads/test_xcorr_movingImg.tif");
  // numberOfOverlapPixels.write("~/Downloads/test_xcorr_overlap.tif");

  ZImg localSumFixed = fixedImg.blockSum(movingImg.width(), movingImg.height(), movingImg.depth()); // 2
  ZImg localSumMoving = movingImg.blockSum(fixedImg.width(), fixedImg.height(), fixedImg.depth()); // 3
  ZImg numeratorPart = localSumFixed * localSumMoving; // 4
  numeratorPart /= numberOfOverlapVoxelsImg;

  localSumFixed *= localSumFixed;
  localSumFixed /= numberOfOverlapVoxelsImg;
  localSumMoving *= localSumMoving;
  localSumMoving /= numberOfOverlapVoxelsImg;

  numberOfOverlapVoxelsImg.clear(); // 3

  nccImg = xCorrFFT(fixedImg, movingImg, false); // 4
  // numerator.write("~/Downloads/test_xcorr_afterfftcorr.tif");
  nccImg -= numeratorPart;
  numeratorPart.clear(); // 3

  movingImg *= movingImg;
  ZImg denom = movingImg.blockSum(fixedImg.width(), fixedImg.height(), fixedImg.depth()); // 4
  denom -= localSumMoving;
  localSumMoving.clear(); // 3
  denom.typedUnaryOperation<double>(removeNegative);

  if (movingImg.isImgView()) { // movingImg is original img
    movingImg.reflect();
  }
  size_t movingImgWidth = movingImg.width();
  size_t movingImgHeight = movingImg.height();
  size_t movingImgDepth = movingImg.depth();
  movingImg.clear();

  fixedImg *= fixedImg;
  ZImg denomFixed = fixedImg.blockSum(movingImgWidth, movingImgHeight, movingImgDepth); // 4
  fixedImg.clear();

  denomFixed -= localSumFixed;
  localSumFixed.clear(); // 3
  denomFixed.typedUnaryOperation<double>(removeNegative);
  denom *= denomFixed;
  denomFixed.clear(); // 2

  // denom.write("~/Downloads/test_xcorr_denom.tif");

  // numerator.write("~/Downloads/test_xcorr_numerator.tif");
  nccImg.typedBinaryOperation<double, double>(denom, secureDivideSqrt2);
  denom.clear(); // 1

  numberOfOverlapVoxelsImg = ZImg(info); // 2
  numberOfOverlapVoxelsImg.fill(1);
  numberOfOverlapVoxelsImg = numberOfOverlapVoxelsImg.blockSum(movingImgWidth, movingImgHeight, movingImgDepth);
}

void normXCorr_S(ZImg& fixedImg, ZImg& movingImg, ZImg& nccImg, ZImg& numberOfOverlapVoxelsImg)
{
  checkInputImgs(fixedImg, movingImg, "normXCorr_S");
  if (!fixedImg.isType<double>()) {
    fixedImg = fixedImg.castTo<double>();
  }
  if (!movingImg.isType<double>()) {
    movingImg = movingImg.castTo<double>();
  }

  //  ZVoxelCoordinate offset1(0,5,0);
  //  VLOG(1) << offset1 << " " << getNCCOfOffset(fixedImg, movingImg, offset1);
  //  offset1 = ZVoxelCoordinate(0, 255, 0);
  //  VLOG(1) << offset1 << " " << getNCCOfOffset(fixedImg, movingImg, offset1);

  movingImg.reflect();

  // VLOG(1) << movingImg.info() << " " << fixedImg.info();

  // VLOG(1) << "1";
  nccImg = xCorrFFT(fixedImg, movingImg, false); // 1, I think peak is 3
  // numerator.write(QString("~/Downloads/test_xcorr_fftxcorr%1.tif").arg(m_fixedImg.width()));
  // VLOG(1) << "2";
  ZImg numeratorPart = fixedImg.blockSum(movingImg.width(), movingImg.height(), movingImg.depth()); // 2
  // VLOG(1) << "3";
  ZImg localSumMoving = movingImg.blockSum(fixedImg.width(), fixedImg.height(), fixedImg.depth()); // 3
  numeratorPart *= localSumMoving;
  // VLOG(1)) << "2";
  localSumMoving.clear(); // 2

  ZImgInfo info = fixedImg.info();
  // VLOG(1) << "3";
  numberOfOverlapVoxelsImg = ZImg(info); // 3
  numberOfOverlapVoxelsImg.fill(1);
  numberOfOverlapVoxelsImg =
    numberOfOverlapVoxelsImg.blockSum(movingImg.width(), movingImg.height(), movingImg.depth());

  numeratorPart /= numberOfOverlapVoxelsImg;
  nccImg -= numeratorPart;
  // VLOG(1) << "2";
  numeratorPart.clear(); // 2
  // numerator.write(QString("~/Downloads/test_xcorr_numerator%1.tif").arg(m_fixedImg.width()));

  // VLOG(1) << "3";
  ZImg localSumFixed = fixedImg.blockSum(movingImg.width(), movingImg.height(), movingImg.depth()); // 3
  localSumFixed *= localSumFixed;
  localSumFixed /= numberOfOverlapVoxelsImg;
  // VLOG(1) << "2";
  numberOfOverlapVoxelsImg.clear(); // 2

  fixedImg *= fixedImg;
  // VLOG(1) << "3";
  ZImg denomFixed = fixedImg.blockSum(movingImg.width(), movingImg.height(), movingImg.depth()); // 3
  size_t fixedImgWidth = fixedImg.width();
  size_t fixedImgHeight = fixedImg.height();
  size_t fixedImgDepth = fixedImg.depth();
  fixedImg.clear();
  denomFixed -= localSumFixed;
  // VLOG(1) << "2";
  localSumFixed.clear(); // 2
  denomFixed.typedUnaryOperation<double>(removeNegative);

  // denomFixed.write(QString("~/Downloads/test_xcorr_df%1.tif").arg(m_fixedImg.width()));
  nccImg.typedBinaryOperation<double, double>(denomFixed, secureDivideSqrt);
  // VLOG(1) << "1";
  denomFixed.clear(); // 1

  // VLOG(1) << "2";
  numberOfOverlapVoxelsImg = ZImg(info); // 2
  numberOfOverlapVoxelsImg.fill(1);
  numberOfOverlapVoxelsImg =
    numberOfOverlapVoxelsImg.blockSum(movingImg.width(), movingImg.height(), movingImg.depth());

  // VLOG(1) << "3";
  localSumMoving = movingImg.blockSum(fixedImgWidth, fixedImgHeight, fixedImgDepth); // 3
  localSumMoving *= localSumMoving;
  localSumMoving /= numberOfOverlapVoxelsImg;
  // VLOG(1) << "2";
  numberOfOverlapVoxelsImg.clear(); // 2

  movingImg *= movingImg;
  // VLOG(1) << "3";
  ZImg denom = movingImg.blockSum(fixedImgWidth, fixedImgHeight, fixedImgDepth); // 3
  denom -= localSumMoving;
  // VLOG(1) << "2";
  localSumMoving.clear(); // 2
  denom.typedUnaryOperation<double>(removeNegative);

  if (movingImg.isImgView()) { // movingImg is original img
    movingImg.reflect();
  }
  size_t movingImgWidth = movingImg.width();
  size_t movingImgHeight = movingImg.height();
  size_t movingImgDepth = movingImg.depth();
  movingImg.clear();

  // denom.write(QString("~/Downloads/test_xcorr_dm%1.tif").arg(m_fixedImg.width()));
  nccImg.typedBinaryOperation<double, double>(denom, secureDivideSqrt2);
  // VLOG(1) << "1";
  denom.clear(); // 1

  // numerator.write("~/Downloads/test_xcorr_res.tif");

  // VLOG(1) << "2";
  numberOfOverlapVoxelsImg = ZImg(info); // 2
  numberOfOverlapVoxelsImg.fill(1);
  numberOfOverlapVoxelsImg = numberOfOverlapVoxelsImg.blockSum(movingImgWidth, movingImgHeight, movingImgDepth);
}

void normXCorrPart(ZImg& fixedImg,
                   ZImg& movingImg,
                   size_t xStart,
                   size_t xEnd,
                   size_t yStart,
                   size_t yEnd,
                   size_t zStart,
                   size_t zEnd,
                   ZImg& nccImg,
                   ZImg& numberOfOverlapVoxelsImg)
{
  checkInputImgs(fixedImg, movingImg, "normXCorrPart");
  if (xStart >= xEnd || yStart >= yEnd || zStart >= zEnd || xEnd > fixedImg.width() + movingImg.width() - 1 ||
      yEnd > fixedImg.height() + movingImg.height() - 1 || zEnd > fixedImg.depth() + movingImg.depth() - 1) {
    throw ZException(fmt::format("normXCorrPart: invalid part ({}:{},{}:{},{}:{}) of fixedImg <{}> and movingImg <{}>",
                                 xStart,
                                 xEnd,
                                 yStart,
                                 yEnd,
                                 zStart,
                                 zEnd,
                                 fixedImg.info(),
                                 movingImg.info()));
  }

  if (!fixedImg.isType<double>()) {
    fixedImg = fixedImg.castTo<double>();
  }
  if (!movingImg.isType<double>()) {
    movingImg = movingImg.castTo<double>();
  }

  // ZVoxelCoordinate offset1(0,0,0);
  // VLOG(1) << offset1 << " " << getNCCOfOffset(fixedImg, movingImg, offset1);
  // offset1 = ZVoxelCoordinate(0, 255, 0);
  // VLOG(1) << offset1 << " " << getNCCOfOffset(fixedImg, movingImg, offset1);

  nccImg = xCorrPart(fixedImg, movingImg, xStart, xEnd, yStart, yEnd, zStart, zEnd); //

  movingImg.reflect();

  // VLOG(1) << movingImg.info() << " " << fixedImg.info();

  ZImgInfo info = fixedImg.info();
  numberOfOverlapVoxelsImg = ZImg(info); //
  numberOfOverlapVoxelsImg.fill(1);
  numberOfOverlapVoxelsImg =
    numberOfOverlapVoxelsImg
      .blockSumPart(movingImg.width(), movingImg.height(), movingImg.depth(), xStart, xEnd, yStart, yEnd, zStart, zEnd);

  //  ZImg nop(info);
  //  nop.fill(1);
  //  nop = nop.blockSum(movingImg.width(), movingImg.height(), movingImg.depth());
  //  ZImgRegion rgn(xStart, xEnd, yStart, yEnd, zStart, zEnd);
  //  nop = nop.crop(rgn);

  // fixedImg.write("~/Downloads/test_xcorr_fixedImg.tif");
  // movingImg.write("~/Downloads/test_xcorr_movingImg.tif");
  // numberOfOverlapPixels.write("~/Downloads/test_xcorr_overlap.tif");
  // nop.write("~/Downloads/test_xcorr_overlap1.tif");

  ZImg localSumFixed = fixedImg.blockSumPart(movingImg.width(),
                                             movingImg.height(),
                                             movingImg.depth(),
                                             xStart,
                                             xEnd,
                                             yStart,
                                             yEnd,
                                             zStart,
                                             zEnd); //
  ZImg localSumMoving =
    movingImg
      .blockSumPart(fixedImg.width(), fixedImg.height(), fixedImg.depth(), xStart, xEnd, yStart, yEnd, zStart, zEnd); //
  ZImg numeratorPart = localSumFixed * localSumMoving; //
  numeratorPart /= numberOfOverlapVoxelsImg;

  localSumFixed *= localSumFixed;
  localSumFixed /= numberOfOverlapVoxelsImg;
  localSumMoving *= localSumMoving;
  localSumMoving /= numberOfOverlapVoxelsImg;

  // numerator.write("~/Downloads/test_xcorr_afterfftcorr.tif");
  nccImg -= numeratorPart;
  numeratorPart.clear(); //

  movingImg *= movingImg;
  ZImg denom = movingImg.blockSumPart(fixedImg.width(),
                                      fixedImg.height(),
                                      fixedImg.depth(),
                                      xStart,
                                      xEnd,
                                      yStart,
                                      yEnd,
                                      zStart,
                                      zEnd); // 4
  denom -= localSumMoving;
  localSumMoving.clear(); //
  denom.typedUnaryOperation<double>(removeNegative);

  if (movingImg.isImgView()) { // movingImg is original img
    movingImg.reflect();
  }
  size_t movingImgWidth = movingImg.width();
  size_t movingImgHeight = movingImg.height();
  size_t movingImgDepth = movingImg.depth();
  movingImg.clear();

  fixedImg *= fixedImg;
  ZImg denomFixed =
    fixedImg
      .blockSumPart(movingImgWidth, movingImgHeight, movingImgDepth, xStart, xEnd, yStart, yEnd, zStart, zEnd); // 4
  fixedImg.clear();

  denomFixed -= localSumFixed;
  localSumFixed.clear(); //
  denomFixed.typedUnaryOperation<double>(removeNegative);
  denom *= denomFixed;
  denomFixed.clear(); //

  // denom.write("~/Downloads/test_xcorr_denom.tif");

  // numerator.write("~/Downloads/test_xcorr_numerator.tif");
  nccImg.typedBinaryOperation<double, double>(denom, secureDivideSqrt2);
}

ZImg xCorrFFT(const ZImg& fixedImg, ZImg& movingImg, bool reflectMovingImg)
{
  checkInputImgs(fixedImg, movingImg, "xCorrFFT");
  size_t outWidth = fixedImg.width() + movingImg.width() - 1;
  size_t outHeight = fixedImg.height() + movingImg.height() - 1;
  size_t outDepth = fixedImg.depth() + movingImg.depth() - 1;
  size_t optimalWidth = findClosestValidDimension(outWidth);
  size_t optimalHeight = findClosestValidDimension(outHeight);
  size_t optimalDepth = findClosestValidDimension(outDepth);
  // VLOG(1) << optimalWidth << " " << optimalHeight << " " << optimalDepth;

  ZComplexImg cfixed = fft(fixedImg, optimalWidth, optimalHeight, optimalDepth);
  // ZImg img;
  // img.mapData(reinterpret_cast<double*>(cfixed.rawData()), cfixed.width()*2, cfixed.height(), cfixed.depth());
  // img.write("~/Downloads/test_xcorr_fixedfft.tif");
  if (reflectMovingImg) {
    cfixed *= fft(movingImg.reflect(), optimalWidth, optimalHeight, optimalDepth);
    movingImg.reflect();
  } else {
    //    ZComplexImg tmp = fft(movingImg, optimalWidth, optimalHeight, optimalDepth);
    //    img.mapData(reinterpret_cast<double*>(tmp.rawData()), tmp.width()*2, tmp.height(), tmp.depth());
    //    img.write("~/Downloads/test_xcorr_movingfft.tif");
    cfixed *= fft(movingImg, optimalWidth, optimalHeight, optimalDepth);
    // img.mapData(reinterpret_cast<double*>(cfixed.rawData()), cfixed.width()*2, cfixed.height(), cfixed.depth());
    // img.write("~/Downloads/test_xcorr_fixedmovingfft.tif");
  }
  return ifft(cfixed, optimalWidth, outWidth, outHeight, outDepth);
}

ZImg xCorrPart(const ZImg& fixedImg,
               const ZImg& movingImg,
               size_t xStart,
               size_t xEnd,
               size_t yStart,
               size_t yEnd,
               size_t zStart,
               size_t zEnd)
{
  checkInputImgs(fixedImg, movingImg, "xCorrPart");
  if (xStart >= xEnd || yStart >= yEnd || zStart >= zEnd || xEnd > fixedImg.width() + movingImg.width() - 1 ||
      yEnd > fixedImg.height() + movingImg.height() - 1 || zEnd > fixedImg.depth() + movingImg.depth() - 1) {
    throw ZException(fmt::format("xCorrPart: invalid part ({}:{},{}:{},{}:{}) of fixedImg <{}> and movingImg <{}>",
                                 xStart,
                                 xEnd,
                                 yStart,
                                 yEnd,
                                 zStart,
                                 zEnd,
                                 fixedImg.info(),
                                 movingImg.info()));
  }

  ZImgInfo info = fixedImg.info();
  info.width = xEnd - xStart;
  info.height = yEnd - yStart;
  info.depth = zEnd - zStart;
  info.voxelFormat = VoxelFormat::Float;
  info.bytesPerVoxel = 8;
  ZImg res(info);

  imgTypeDispatcher(fixedImg.info(), [&]<typename TVoxel1>() {
    imgTypeDispatcher(movingImg.info(), [&]<typename TVoxel2>() {
      const auto fixedData = fixedImg.channelData<TVoxel1>(0, 0);
      const auto movingData = movingImg.channelData<TVoxel2>(0, 0);
      auto desData = res.channelData<double>(0, 0);
      size_t fixedPlaneNum = fixedImg.planeVoxelNumber();
      size_t fixedRowNum = fixedImg.rowVoxelNumber();
      size_t movingPlaneNum = movingImg.planeVoxelNumber();
      size_t movingRowNum = movingImg.rowVoxelNumber();
      size_t desOffset = 0;
      for (auto z = static_cast<index_t>(zStart); z < static_cast<index_t>(zEnd); ++z) {
        size_t movingStartZ = std::max(0_z, movingImg.sDepth() - 1 - z);
        size_t movingEndZ = std::min(fixedImg.sDepth() + movingImg.sDepth() - 1 - z, movingImg.sDepth());
        size_t fixedStartZ = std::max(0_z, z - movingImg.sDepth() + 1);
        for (auto y = static_cast<index_t>(yStart); y < static_cast<index_t>(yEnd); ++y) {
          size_t movingStartY = std::max(0_z, movingImg.sHeight() - 1 - y);
          size_t movingEndY = std::min(fixedImg.sHeight() + movingImg.sHeight() - 1 - y, movingImg.sHeight());
          size_t fixedStartY = std::max(0_z, y - movingImg.sHeight() + 1);
          for (auto x = static_cast<index_t>(xStart); x < static_cast<index_t>(xEnd); ++x) {
            size_t movingStartX = std::max(0_z, movingImg.sWidth() - 1 - x);
            size_t movingEndX = std::min(fixedImg.sWidth() + movingImg.sWidth() - 1 - x, movingImg.sWidth());
            size_t fixedStartX = std::max(0_z, x - movingImg.sWidth() + 1);
            size_t fixedOffset = fixedStartZ * fixedPlaneNum + fixedStartY * fixedRowNum + fixedStartX;
            size_t movingOffset = movingStartZ * movingPlaneNum + movingStartY * movingRowNum + movingStartX;
            for (size_t mz = movingStartZ; mz < movingEndZ; ++mz) {
              for (size_t my = movingStartY; my < movingEndY; ++my) {
                for (size_t mx = movingStartX; mx < movingEndX; ++mx) {
                  desData[desOffset] += static_cast<double>(fixedData[fixedOffset]) * movingData[movingOffset];
                  ++fixedOffset;
                  ++movingOffset;
                }
                fixedOffset += fixedRowNum - (movingEndX - movingStartX);
                movingOffset += movingRowNum - (movingEndX - movingStartX);
              }
              fixedOffset += fixedPlaneNum - (movingEndY - movingStartY) * fixedRowNum;
              movingOffset += movingPlaneNum - (movingEndY - movingStartY) * movingRowNum;
            }

            ++desOffset;
          }
        }
      }
    });
  });

  return res;
}

void cropOverlapSubImg(const ZImg& fixedImgIn,
                       const ZImg& movingImgIn,
                       const ZVoxelCoordinate& offset,
                       ZImg& subFixedImg,
                       ZImg& subMovingImg)
{
  // find overlap region
  ZVoxelCoordinate fixedStart = max(offset, 0); // max of zero and offset
  ZVoxelCoordinate fixedEnd = min(offset + movingImgIn.endCoord(), fixedImgIn.endCoord());
  if (fixedEnd.anyLessEqual(fixedStart)) {
    throw ZException(fmt::format("Trying to crop overlap region of non-overlap img1 <{}> and img2 <{}> with offset: {}",
                                 fixedImgIn.info(),
                                 movingImgIn.info(),
                                 offset));
  }
  ZImgRegion fixedRegion(fixedStart, fixedEnd);
  subFixedImg = fixedImgIn.crop(fixedRegion);

  ZImgRegion movingRegion(fixedStart - offset, fixedEnd - offset);
  subMovingImg = movingImgIn.crop(movingRegion);
}

} // namespace nim
