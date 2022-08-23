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

inline double removeNegative(double v)
{
  return std::max(0.0, v);
}

inline double secureDivideSqrt(double v1, double v2)
{
  v2 = std::sqrt(v2);
  if (v2 > 1e-8) {
    return v1 / v2;
  }
  return 0.0;
}

inline double secureDivideSqrt2(double v1, double v2)
{
  v2 = std::sqrt(v2);
  if (v2 > 1e-8) {
    return std::max(-1.0, std::min(1.0, v1 / v2));
  } else {
    return 0.0;
  }
}

using namespace nim;

template<typename TVoxel1, typename TVoxel2>
void xCorrPart_Impl(const ZImg& fixedImg,
                    const ZImg& movingImg,
                    size_t xStart,
                    size_t xEnd,
                    size_t yStart,
                    size_t yEnd,
                    size_t zStart,
                    size_t zEnd,
                    ZImg& res)
{
  const TVoxel1* fixedData = fixedImg.channelData<TVoxel1>(0, 0);
  const TVoxel2* movingData = movingImg.channelData<TVoxel2>(0, 0);
  double* desData = res.channelData<double>(0, 0);
  size_t fixedPlaneNum = fixedImg.planeVoxelNumber();
  size_t fixedRowNum = fixedImg.rowVoxelNumber();
  size_t movingPlaneNum = movingImg.planeVoxelNumber();
  size_t movingRowNum = movingImg.rowVoxelNumber();
  size_t desOffset = 0;
  for (size_t z = zStart; z < zEnd; ++z) {
    size_t movingStartZ = std::max(0, static_cast<int>(movingImg.depth()) - 1 - static_cast<int>(z));
    size_t movingEndZ =
      std::min(static_cast<int>(fixedImg.depth()) + static_cast<int>(movingImg.depth()) - 1 - static_cast<int>(z),
               static_cast<int>(movingImg.depth()));
    size_t fixedStartZ = std::max(0, static_cast<int>(z) - static_cast<int>(movingImg.depth()) + 1);
    for (size_t y = yStart; y < yEnd; ++y) {
      size_t movingStartY = std::max(0, static_cast<int>(movingImg.height()) - 1 - static_cast<int>(y));
      size_t movingEndY =
        std::min(static_cast<int>(fixedImg.height()) + static_cast<int>(movingImg.height()) - 1 - static_cast<int>(y),
                 static_cast<int>(movingImg.height()));
      size_t fixedStartY = std::max(0, static_cast<int>(y) - static_cast<int>(movingImg.height()) + 1);
      for (size_t x = xStart; x < xEnd; ++x) {
        size_t movingStartX = std::max(0, static_cast<int>(movingImg.width()) - 1 - static_cast<int>(x));
        size_t movingEndX =
          std::min(static_cast<int>(fixedImg.width()) + static_cast<int>(movingImg.width()) - 1 - static_cast<int>(x),
                   static_cast<int>(movingImg.width()));
        size_t fixedStartX = std::max(0, static_cast<int>(x) - static_cast<int>(movingImg.width()) + 1);
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
}

template<typename TVoxel>
double getNCCOfOffset_Impl(const ZImg& fixedImg, const ZImg& movingImg)
{
  ZImageToImageMetric metric;
  metric.setType(ZImageToImageMetric::Type::NormalizedCrossCorrelation);
  return -metric.value(fixedImg.channelData<TVoxel>(0),
                       movingImg.channelData<TVoxel>(0),
                       fixedImg.width(),
                       fixedImg.height(),
                       fixedImg.depth());
}

void checkInputImgs(const ZImg& fixedImg, const ZImg& movingImg, const QString& name = "")
{
  if (fixedImg.isEmpty() || movingImg.isEmpty() || fixedImg.numChannels() != 1 || fixedImg.numTimes() != 1 ||
      movingImg.numChannels() != 1 || movingImg.numTimes() != 1) {
    throw ZImgException(QString("%1 input img dimension is not supported: fixed img <%1>, moving img <%2>")
                          .arg(name)
                          .arg(fixedImg.info().toQString())
                          .arg(movingImg.info().toQString()));
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
  IMG_RETURN_TYPED_CALL(getNCCOfOffset_Impl, fixedImg.info(), fixedImg, movingImg)
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

  // LOG(INFO) << movingImg.info().toQString() << " " << fixedImg.info().toQString();

  ZImgInfo info = fixedImg.info();
  numberOfOverlapVoxelsImg = ZImg(info); // 1
  numberOfOverlapVoxelsImg.fill(1);
  numberOfOverlapVoxelsImg =
    numberOfOverlapVoxelsImg.blockSum(movingImg.width(), movingImg.height(), movingImg.depth());

  // fixedImg.write("/Users/feng/Downloads/test_xcorr_fixedImg.tif");
  // movingImg.write("/Users/feng/Downloads/test_xcorr_movingImg.tif");
  // numberOfOverlapPixels.write("/Users/feng/Downloads/test_xcorr_overlap.tif");

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
  // numerator.write("/Users/feng/Downloads/test_xcorr_afterfftcorr.tif");
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

  // denom.write("/Users/feng/Downloads/test_xcorr_denom.tif");

  // numerator.write("/Users/feng/Downloads/test_xcorr_numerator.tif");
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
  //  LOG(INFO) << offset1 << " " << getNCCOfOffset(fixedImg, movingImg, offset1);
  //  offset1 = ZVoxelCoordinate(0, 255, 0);
  //  LOG(INFO) << offset1 << " " << getNCCOfOffset(fixedImg, movingImg, offset1);

  movingImg.reflect();

  // LOG(INFO) << movingImg.info().toQString() << " " << fixedImg.info().toQString();

  // LOG(WARNING) << "1";
  nccImg = xCorrFFT(fixedImg, movingImg, false); // 1, I think peak is 3
  // numerator.write(QString("/Users/feng/Downloads/test_xcorr_fftxcorr%1.tif").arg(m_fixedImg.width()));
  // LOG(WARNING) << "2";
  ZImg numeratorPart = fixedImg.blockSum(movingImg.width(), movingImg.height(), movingImg.depth()); // 2
  // LOG(WARNING) << "3";
  ZImg localSumMoving = movingImg.blockSum(fixedImg.width(), fixedImg.height(), fixedImg.depth()); // 3
  numeratorPart *= localSumMoving;
  // LOG(WARNING) << "2";
  localSumMoving.clear(); // 2

  ZImgInfo info = fixedImg.info();
  // LOG(WARNING) << "3";
  numberOfOverlapVoxelsImg = ZImg(info); // 3
  numberOfOverlapVoxelsImg.fill(1);
  numberOfOverlapVoxelsImg =
    numberOfOverlapVoxelsImg.blockSum(movingImg.width(), movingImg.height(), movingImg.depth());

  numeratorPart /= numberOfOverlapVoxelsImg;
  nccImg -= numeratorPart;
  // LOG(WARNING) << "2";
  numeratorPart.clear(); // 2
  // numerator.write(QString("/Users/feng/Downloads/test_xcorr_numerator%1.tif").arg(m_fixedImg.width()));

  // LOG(WARNING) << "3";
  ZImg localSumFixed = fixedImg.blockSum(movingImg.width(), movingImg.height(), movingImg.depth()); // 3
  localSumFixed *= localSumFixed;
  localSumFixed /= numberOfOverlapVoxelsImg;
  // LOG(WARNING) << "2";
  numberOfOverlapVoxelsImg.clear(); // 2

  fixedImg *= fixedImg;
  // LOG(WARNING) << "3";
  ZImg denomFixed = fixedImg.blockSum(movingImg.width(), movingImg.height(), movingImg.depth()); // 3
  size_t fixedImgWidth = fixedImg.width();
  size_t fixedImgHeight = fixedImg.height();
  size_t fixedImgDepth = fixedImg.depth();
  fixedImg.clear();
  denomFixed -= localSumFixed;
  // LOG(WARNING) << "2";
  localSumFixed.clear(); // 2
  denomFixed.typedUnaryOperation<double>(removeNegative);

  // denomFixed.write(QString("/Users/feng/Downloads/test_xcorr_df%1.tif").arg(m_fixedImg.width()));
  nccImg.typedBinaryOperation<double, double>(denomFixed, secureDivideSqrt);
  // LOG(WARNING) << "1";
  denomFixed.clear(); // 1

  // LOG(WARNING) << "2";
  numberOfOverlapVoxelsImg = ZImg(info); // 2
  numberOfOverlapVoxelsImg.fill(1);
  numberOfOverlapVoxelsImg =
    numberOfOverlapVoxelsImg.blockSum(movingImg.width(), movingImg.height(), movingImg.depth());

  // LOG(WARNING) << "3";
  localSumMoving = movingImg.blockSum(fixedImgWidth, fixedImgHeight, fixedImgDepth); // 3
  localSumMoving *= localSumMoving;
  localSumMoving /= numberOfOverlapVoxelsImg;
  // LOG(WARNING) << "2";
  numberOfOverlapVoxelsImg.clear(); // 2

  movingImg *= movingImg;
  // LOG(WARNING) << "3";
  ZImg denom = movingImg.blockSum(fixedImgWidth, fixedImgHeight, fixedImgDepth); // 3
  denom -= localSumMoving;
  // LOG(WARNING) << "2";
  localSumMoving.clear(); // 2
  denom.typedUnaryOperation<double>(removeNegative);

  if (movingImg.isImgView()) { // movingImg is original img
    movingImg.reflect();
  }
  size_t movingImgWidth = movingImg.width();
  size_t movingImgHeight = movingImg.height();
  size_t movingImgDepth = movingImg.depth();
  movingImg.clear();

  // denom.write(QString("/Users/feng/Downloads/test_xcorr_dm%1.tif").arg(m_fixedImg.width()));
  nccImg.typedBinaryOperation<double, double>(denom, secureDivideSqrt2);
  // LOG(WARNING) << "1";
  denom.clear(); // 1

  // numerator.write("/Users/feng/Downloads/test_xcorr_res.tif");

  // LOG(WARNING) << "2";
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
    throw ZImgException(QString("normXCorrPart: invalid part (%1:%2,%3:%4,%5:%6) of fixedImg <%7> and movingImg <%8>")
                          .arg(xStart)
                          .arg(xEnd)
                          .arg(yStart)
                          .arg(yEnd)
                          .arg(zStart)
                          .arg(zEnd)
                          .arg(fixedImg.info().toQString())
                          .arg(movingImg.info().toQString()));
  }

  if (!fixedImg.isType<double>()) {
    fixedImg = fixedImg.castTo<double>();
  }
  if (!movingImg.isType<double>()) {
    movingImg = movingImg.castTo<double>();
  }

  // ZVoxelCoordinate offset1(0,0,0);
  // LOG(INFO) << offset1 << " " << getNCCOfOffset(fixedImg, movingImg, offset1);
  // offset1 = ZVoxelCoordinate(0, 255, 0);
  // LOG(INFO) << offset1 << " " << getNCCOfOffset(fixedImg, movingImg, offset1);

  nccImg = xCorrPart(fixedImg, movingImg, xStart, xEnd, yStart, yEnd, zStart, zEnd); //

  movingImg.reflect();

  // LOG(INFO) << movingImg.info().toQString() << " " << fixedImg.info().toQString();

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

  // fixedImg.write("/Users/feng/Downloads/test_xcorr_fixedImg.tif");
  // movingImg.write("/Users/feng/Downloads/test_xcorr_movingImg.tif");
  // numberOfOverlapPixels.write("/Users/feng/Downloads/test_xcorr_overlap.tif");
  // nop.write("/Users/feng/Downloads/test_xcorr_overlap1.tif");

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

  // numerator.write("/Users/feng/Downloads/test_xcorr_afterfftcorr.tif");
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

  // denom.write("/Users/feng/Downloads/test_xcorr_denom.tif");

  // numerator.write("/Users/feng/Downloads/test_xcorr_numerator.tif");
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
  // LOG(INFO) << optimalWidth << " " << optimalHeight << " " << optimalDepth;

  ZComplexImg cfixed = fft(fixedImg, optimalWidth, optimalHeight, optimalDepth);
  // ZImg img;
  // img.mapData(reinterpret_cast<double*>(cfixed.rawData()), cfixed.width()*2, cfixed.height(), cfixed.depth());
  // img.write("/Users/feng/Downloads/test_xcorr_fixedfft.tif");
  if (reflectMovingImg) {
    cfixed *= fft(movingImg.reflect(), optimalWidth, optimalHeight, optimalDepth);
    movingImg.reflect();
  } else {
    //    ZComplexImg tmp = fft(movingImg, optimalWidth, optimalHeight, optimalDepth);
    //    img.mapData(reinterpret_cast<double*>(tmp.rawData()), tmp.width()*2, tmp.height(), tmp.depth());
    //    img.write("/Users/feng/Downloads/test_xcorr_movingfft.tif");
    cfixed *= fft(movingImg, optimalWidth, optimalHeight, optimalDepth);
    // img.mapData(reinterpret_cast<double*>(cfixed.rawData()), cfixed.width()*2, cfixed.height(), cfixed.depth());
    // img.write("/Users/feng/Downloads/test_xcorr_fixedmovingfft.tif");
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
    throw ZImgException(QString("xCorrPart: invalid part (%1:%2,%3:%4,%5:%6) of fixedImg <%7> and movingImg <%8>")
                          .arg(xStart)
                          .arg(xEnd)
                          .arg(yStart)
                          .arg(yEnd)
                          .arg(zStart)
                          .arg(zEnd)
                          .arg(fixedImg.info().toQString())
                          .arg(movingImg.info().toQString()));
  }

  ZImgInfo info = fixedImg.info();
  info.width = xEnd - xStart;
  info.height = yEnd - yStart;
  info.depth = zEnd - zStart;
  info.voxelFormat = VoxelFormat::Float;
  info.bytesPerVoxel = 8;
  ZImg res(info);

  IMG_TYPED_CALL_2TYPE(xCorrPart_Impl,
                       fixedImg.info(),
                       movingImg.info(),
                       fixedImg,
                       movingImg,
                       xStart,
                       xEnd,
                       yStart,
                       yEnd,
                       zStart,
                       zEnd,
                       res)

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
    throw ZImgException(QString("Trying to crop overlap region of non-overlap img1 <%1> and img2 <%2> with offset: %3")
                          .arg(fixedImgIn.info().toQString())
                          .arg(movingImgIn.info().toQString())
                          .arg(offset.toQString()));
  }
  ZImgRegion fixedRegion(fixedStart, fixedEnd);
  subFixedImg = fixedImgIn.crop(fixedRegion);

  ZImgRegion movingRegion(fixedStart - offset, fixedEnd - offset);
  subMovingImg = movingImgIn.crop(movingRegion);
}

} // namespace nim
