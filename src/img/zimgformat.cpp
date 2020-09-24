#include "zimgformat.h"

#include "zioutils.h"
#include "zimgsliceprovider.h"
#include "zimgblockprovider.h"
#include "zlog.h"
#include "zimgio.h"

namespace nim {

ZImgFormat::~ZImgFormat() = default;

bool ZImgFormat::canRead(const QString& filename) const
{
  if (!supportRead())
    return false;
  QStringList exts = extensions();
  for (int i = 0; i < exts.size(); ++i) {
    if (filename.endsWith(QString(".") + exts[i], Qt::CaseInsensitive))
      return true;
  }
  return false;
}

bool ZImgFormat::canWrite(const QString& filename) const
{
  if (!supportWrite())
    return false;
  QStringList exts = extensions();
  for (int i = 0; i < exts.size(); ++i) {
    if (filename.endsWith(QString(".") + exts[i], Qt::CaseInsensitive))
      return true;
  }
  return false;
}

void ZImgFormat::checkImgBeforeWriting(const QString& filename, const ZImgInfo& /*info*/,
                                       const ZImgWriteParameters& /*paras*/)
{
  if (!canWrite(filename)) {
    throw ZIOException(QString("filename %1 is not supported for writing").arg(filename));
  }
}

void ZImgFormat::writeImg(const QString& /*filename*/, const ZImg& /*img*/, const ZImgWriteParameters& /*paras*/)
{
}

void ZImgFormat::writeImg(const QString& filename,
                          const ZImgSliceProvider& imgSliceProvider,
                          const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, imgSliceProvider.imgInfo(), paras);
  writeImg(filename, imgSliceProvider.wholeImg(), paras);
}

void ZImgFormat::writeImg(const QString& filename,
                          const ZImgBlockProvider& imgBlockProvider,
                          const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, imgBlockProvider.imgInfo(), paras);
  writeImg(filename, imgBlockProvider.wholeImg(), paras);
}

ZImg ZImgFormat::readRawImg(const QString& filename, const ZImgInfo& imgInfo, const QString& dimensionOrderIn,
                            uint64_t dataOffset, const ZImgRegion& region, uint64_t timeStride)
{
  if (region.isEmpty() || !region.isValid(imgInfo)) {
    throw ZIOException(
      QString("Invalid image region. Image info: '%1', region: '%2'").arg(imgInfo.toQString()).arg(region.toQString()));
  }
  if (dimensionOrderIn != "XYZCT" && dimensionOrderIn != "XYCZT" && dimensionOrderIn != "CXYZT") {
    throw ZIOException(QString("Not supported dimension order: %1").arg(dimensionOrderIn));
  }
  auto dimensionOrder = dimensionOrderIn;
  if (dimensionOrder == "CXYZT" && imgInfo.numChannels == 1) {
    dimensionOrder = "XYZCT";
  }

  ZImgInfo partialImgInfo = region.clip(imgInfo);
  ZImg res(partialImgInfo);

  if (dimensionOrder == "CXYZT") {
    ZImgInfo tmpInfo = imgInfo;
    tmpInfo.bytesPerVoxel *= tmpInfo.numChannels;
    tmpInfo.numChannels = 1;
    ZImgRegion rgn = region;
    rgn.start.c = 0;
    rgn.end.c = -1;
    ZImg tmpImg = readRawImg(filename, tmpInfo, "XYZCT", dataOffset, rgn, timeStride);
    if (region.start.c == 0 &&
        (region.end.c == -1 || size_t(region.end.c) == imgInfo.numChannels)) { // read all channel
      tmpImg.infoRef() = res.info();
      CXYZtoXYZC(tmpImg, res);
    } else {
      ZImgInfo tmpPartialImgInfo = rgn.clip(imgInfo);
      ZImg tmpRes(tmpPartialImgInfo);   // tmpImg contains all channel so res is not big enough, need a tmp res
      tmpImg.infoRef() = tmpRes.info();
      CXYZtoXYZC(tmpImg, tmpRes);
      ZImgRegion tmpRgn;
      tmpRgn.start.c = region.start.c;
      tmpRgn.end.c = region.end.c;
      res = tmpRes.crop(tmpRgn);
    }
  } else {
    std::ifstream inputFileStream;
    openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

    if (timeStride == 0)
      timeStride = imgInfo.timeByteNumber();
    CHECK(timeStride >= imgInfo.timeByteNumber());

    int tEnd = region.end.t == -1 ? imgInfo.numTimes : region.end.t;
    for (int t = region.start.t; t < tEnd; ++t) {
      if (region.containsWholeTime(imgInfo) && (dimensionOrder == "XYZCT")) {
        size_t offset = dataOffset + t * timeStride;
        inputFileStream.seekg(offset, std::ios_base::beg);
        readStream(inputFileStream, res.timeData<char>(t - region.start.t), res.timeByteNumber());
      } else if (region.containsWholeChannel(imgInfo) && (dimensionOrder == "XYZCT")) {
        size_t offset = dataOffset + t * timeStride + region.start.c * imgInfo.channelByteNumber();
        inputFileStream.seekg(offset, std::ios_base::beg);
        readStream(inputFileStream, res.timeData<char>(t - region.start.t), res.timeByteNumber());
      } else if (region.containsWholePlane(imgInfo) && (dimensionOrder == "XYZCT")) {
        int cEnd = region.end.c == -1 ? imgInfo.numChannels : region.end.c;
        // channel by channel
        for (int c = region.start.c; c < cEnd; ++c) {
          size_t offset = dataOffset + t * timeStride + c * imgInfo.channelByteNumber() +
                          region.start.z * imgInfo.planeByteNumber();
          inputFileStream.seekg(offset, std::ios_base::beg);
          readStream(inputFileStream, res.channelData<char>(c - region.start.c, t - region.start.t),
                     res.channelByteNumber());
        }
      } else if (region.containsWholeRow(imgInfo)) {
        int cEnd = region.end.c == -1 ? imgInfo.numChannels : region.end.c;
        int zEnd = region.end.z == -1 ? imgInfo.depth : region.end.z;
        // plane by plane
        for (int c = region.start.c; c < cEnd; ++c) {
          for (int z = region.start.z; z < zEnd; ++z) {
            size_t offset = 0;
            if ((dimensionOrder == "XYZCT")) {
              offset = dataOffset + t * timeStride +
                       c * imgInfo.channelByteNumber() + z * imgInfo.planeByteNumber() +
                       region.start.y * imgInfo.rowByteNumber();
            } else { // "XYCZT"
              offset = dataOffset + t * timeStride +
                       (c + imgInfo.numChannels * z) * imgInfo.planeByteNumber() +
                       region.start.y * imgInfo.rowByteNumber();
            }
            inputFileStream.seekg(offset, std::ios_base::beg);
            readStream(inputFileStream, res.planeData<char>(z - region.start.z, c - region.start.c, t - region.start.t),
                       res.planeByteNumber());
          }
        }
      } else {
        int cEnd = region.end.c == -1 ? imgInfo.numChannels : region.end.c;
        int zEnd = region.end.z == -1 ? imgInfo.depth : region.end.z;
        int yEnd = region.end.y == -1 ? imgInfo.height : region.end.y;
        // row by row
        for (int c = region.start.c; c < cEnd; ++c) {
          for (int z = region.start.z; z < zEnd; ++z) {
            for (int y = region.start.y; y < yEnd; ++y) {
              size_t offset = 0;
              if ((dimensionOrder == "XYZCT")) {
                offset = dataOffset + t * timeStride +
                         c * imgInfo.channelByteNumber() + z * imgInfo.planeByteNumber() +
                         y * imgInfo.rowByteNumber() + region.start.x * imgInfo.voxelByteNumber();
              } else { // "XYCZT"
                offset = dataOffset + t * timeStride +
                         (c + imgInfo.numChannels * z) * imgInfo.planeByteNumber() +
                         y * imgInfo.rowByteNumber() + region.start.x * imgInfo.voxelByteNumber();
              }
              inputFileStream.seekg(offset, std::ios_base::beg);
              readStream(inputFileStream,
                         res.rowData<char>(y - region.start.y, z - region.start.z, c - region.start.c,
                                           t - region.start.t),
                         res.rowByteNumber());
            }
          }
        }
      }
    }
  }

  return res;
}

ZImg ZImgFormat::readRawImg(const QString& filename, const ZImgInfo& imgInfo,
                            const std::vector<size_t>& dimensionStrides,
                            uint64_t dataOffset, const ZImgRegion& region)
{
  ZImg res;

  if (region.isEmpty() || !region.isValid(imgInfo)) {
    throw ZIOException(
      QString("Invalid image region. Image info: '%1', region: '%2'").arg(imgInfo.toQString()).arg(region.toQString()));
  }

  CHECK(dimensionStrides.size() == 5);
  std::vector<size_t> sortedIndexes = argSort(dimensionStrides.begin(), dimensionStrides.end());
  QString dimensionOrderIn("XYZCT");
  QString dimensionOrder("XYZCT");
  std::vector<size_t> packedStrides(5, 0);
  size_t prev = imgInfo.bytesPerVoxel;
  size_t packedPrev = imgInfo.bytesPerVoxel;
  for (size_t i = 0; i < sortedIndexes.size(); ++i) {
    auto idx = sortedIndexes[i];
    packedStrides[idx] = packedPrev;
    if (dimensionStrides[idx] < prev) {
      throw ZIOException(QString("invalid dimensionStrides %1 for image %2").arg(qtTypeToQString(dimensionStrides)).arg(
        imgInfo.toQString()));
    }
    packedPrev = packedStrides[idx] * imgInfo.size(idx);
    prev = dimensionStrides[idx] * imgInfo.size(idx);
    dimensionOrder[uint(i)] = dimensionOrderIn[uint(idx)];
  }
  packedStrides[4] = dimensionStrides[4]; // time dimenstion does not need to be packed
  bool packed = packedStrides == dimensionStrides;
  //LOG(INFO) << dimensionStrides << " " << dimensionOrder << " Packed: " << packed << " " << imgInfo.toQString();
  if (packed && (dimensionOrder == "XYZCT" || dimensionOrder == "XYCZT" || dimensionOrder == "CXYZT")) {
    res = readRawImg(filename, imgInfo, dimensionOrder, dataOffset, region, dimensionStrides[4]);
  } else {
    ZImgInfo partialImgInfo = region.clip(imgInfo);
    res = ZImg(partialImgInfo);

    std::ifstream inputFileStream;
    openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

    int tEnd = region.end.t == -1 ? imgInfo.numTimes : region.end.t;
    int cEnd = region.end.c == -1 ? imgInfo.numChannels : region.end.c;
    int zEnd = region.end.z == -1 ? imgInfo.depth : region.end.z;
    int yEnd = region.end.y == -1 ? imgInfo.height : region.end.y;
    int xEnd = region.end.x == -1 ? imgInfo.width : region.end.x;
    // pixel by pixel
    for (int t = region.start.t; t < tEnd; ++t) {
      for (int c = region.start.c; c < cEnd; ++c) {
        for (int z = region.start.z; z < zEnd; ++z) {
          for (int y = region.start.y; y < yEnd; ++y) {
            for (int x = region.start.x; x < xEnd; ++x) {
              size_t offset = dataOffset + t * dimensionStrides[4] +
                              c * dimensionStrides[3] + z * dimensionStrides[2] +
                              y * dimensionStrides[1] + x * dimensionStrides[0];
              inputFileStream.seekg(offset, std::ios_base::beg);
              readStream(inputFileStream,
                         res.data<char>(x - region.start.x, y - region.start.y, z - region.start.z, c - region.start.c,
                                        t - region.start.t),
                         res.voxelByteNumber());
            }
          }
        }
      }
    }
  }

  return res;
}

void ZImgFormat::CXYZtoXYZC(const ZImg& bufImg, ZImg& img, bool BGRtoRGB, bool ARGBtoRGBA)
{
  CHECK(
    bufImg.isSameSize(img) && bufImg.isSameType(img) && img.channelData<uint8_t>(0) != bufImg.channelData<uint8_t>(0));

  if (bufImg.numChannels() == 1) {
    CHECK(false);
    for (size_t t = 0; t < img.numTimes(); ++t)
      std::memcpy(img.timeData<uint8_t>(t),
                  bufImg.timeData<uint8_t>(t),
                  bufImg.timeByteNumber());
    return;
  }

  for (size_t t = 0; t < img.numTimes(); ++t) {
    for (size_t c = 0; c < img.numChannels(); ++c) {
      size_t srcC = c;
      if (BGRtoRGB) {
        if (srcC == 0) {
          srcC = 2;
        } else if (srcC == 2) {
          srcC = 0;
        }
      } else if (ARGBtoRGBA) {
        if (srcC == 0) {
          srcC = 3;
        } else {
          srcC -= 1;
        }
      }

      switch (img.voxelByteNumber()) {
        case 1: {
          uint8_t* des = img.channelData<uint8_t>(c, t);
          const uint8_t* src = bufImg.channelData<uint8_t>(0, t) + srcC;
          size_t numCh = img.numChannels();
          size_t i = 0;
          while (i++ < img.channelVoxelNumber()) {
            *des = *src;
            ++des;
            src += numCh;
          }
        }
          break;
        case 2: {
          uint16_t* des = img.channelData<uint16_t>(c, t);
          const uint16_t* src = bufImg.channelData<uint16_t>(0, t) + srcC;
          size_t numCh = img.numChannels();
          size_t i = 0;
          while (i++ < img.channelVoxelNumber()) {
            *des = *src;
            ++des;
            src += numCh;
          }
        }
          break;
        default: {
          uint8_t* des = img.channelData<uint8_t>(c, t);
          const uint8_t* src = bufImg.channelData<uint8_t>(0, t) + srcC * img.voxelByteNumber();
          size_t voxelByte = img.voxelByteNumber();
          size_t srcStride = img.numChannels() * voxelByte;
          size_t i = 0;
          while (i++ < img.channelVoxelNumber()) {
            std::memcpy(des, src, voxelByte);
            des += voxelByte;
            src += srcStride;
          }
        }
      }
    }
  }
}

void ZImgFormat::XYZCtoCXYZ(const ZImg& bufImg, ZImg& img)
{
  CHECK(
    bufImg.isSameSize(img) && bufImg.isSameType(img) && img.channelData<uint8_t>(0) != bufImg.channelData<uint8_t>(0));

  if (bufImg.numChannels() == 1) {
    CHECK(false);
    for (size_t t = 0; t < img.numTimes(); ++t)
      std::memcpy(img.timeData<uint8_t>(t),
                  bufImg.timeData<uint8_t>(t),
                  bufImg.timeByteNumber());
    return;
  }

  for (size_t t = 0; t < img.numTimes(); ++t) {
    for (size_t c = 0; c < img.numChannels(); ++c) {
      size_t srcC = c;
      switch (img.voxelByteNumber()) {
        case 1: {
          uint8_t* des = img.channelData<uint8_t>(0, t) + srcC;
          const uint8_t* src = bufImg.channelData<uint8_t>(c, t);
          size_t numCh = img.numChannels();
          size_t i = 0;
          while (i++ < img.channelVoxelNumber()) {
            *des = *src;
            des += numCh;
            ++src;
          }
        }
          break;
        case 2: {
          uint16_t* des = img.channelData<uint16_t>(0, t) + srcC;
          const uint16_t* src = bufImg.channelData<uint16_t>(c, t);
          size_t numCh = img.numChannels();
          size_t i = 0;
          while (i++ < img.channelVoxelNumber()) {
            *des = *src;
            des += numCh;
            ++src;
          }
        }
          break;
        default: {
          uint8_t* des = img.channelData<uint8_t>(0, t) + srcC * img.voxelByteNumber();
          const uint8_t* src = bufImg.channelData<uint8_t>(c, t);
          size_t voxelByte = img.voxelByteNumber();
          size_t desStride = img.numChannels() * voxelByte;
          size_t i = 0;
          while (i++ < img.channelVoxelNumber()) {
            std::memcpy(des, src, voxelByte);
            des += desStride;
            src += voxelByte;
          }
        }
      }
    }
  }
}

void ZImgFormat::fixDimensionOrder(const uint8_t* buf, const QString& dimensionOrder, ZImg& img, bool BGRtoRGB)
{
  CHECK(dimensionOrder.size() == 5);
  QString desOrder("XYZCT");
  QString srcOrder = dimensionOrder;
  if (img.numTimes() == 1) {
    srcOrder.remove(QChar('T'));
    desOrder.remove(QChar('T'));
  }
  if (img.numChannels() == 1) {
    srcOrder.remove(QChar('C'));
    desOrder.remove(QChar('C'));
  }
  if (img.depth() == 1) {
    srcOrder.remove(QChar('Z'));
    desOrder.remove(QChar('Z'));
  }
  if (img.height() == 1) {
    srcOrder.remove(QChar('Y'));
    desOrder.remove(QChar('Y'));
  }
  if (img.width() == 1) {
    srcOrder.remove(QChar('X'));
    desOrder.remove(QChar('X'));
  }
  if (srcOrder == desOrder) {
    if (BGRtoRGB) {
      for (size_t t = 0; t < img.numTimes(); ++t) {
        for (size_t c = 0; c < img.numChannels(); ++c) {
          size_t srcC = c;
          if (c == 0) {
            srcC = 2;
          } else if (c == 2) {
            srcC = 0;
          }
          std::memcpy(img.channelData<uint8_t>(c, t),
                      buf + t * img.timeByteNumber() + srcC * img.channelByteNumber(),
                      img.channelByteNumber());
        }
      }
    } else {
      for (size_t t = 0; t < img.numTimes(); ++t) {
        std::memcpy(img.timeData<uint8_t>(t),
                    buf + t * img.timeByteNumber(),
                    img.timeByteNumber());
      }
    }
    return;
  }

  std::vector<size_t> srcDimSizes(5);
  std::vector<size_t> srcDimIdxToDesDimIdx(5);
  for (size_t i = 0; i < srcDimSizes.size(); ++i) {
    switch (dimensionOrder.at(i).toLatin1()) {
      case 'X':
        srcDimSizes[i] = img.width();
        srcDimIdxToDesDimIdx[i] = 0;
        break;
      case 'Y':
        srcDimSizes[i] = img.height();
        srcDimIdxToDesDimIdx[i] = 1;
        break;
      case 'Z':
        srcDimSizes[i] = img.depth();
        srcDimIdxToDesDimIdx[i] = 2;
        break;
      case 'C':
        srcDimSizes[i] = img.numChannels();
        srcDimIdxToDesDimIdx[i] = 3;
        break;
      case 'T':
        srcDimSizes[i] = img.numTimes();
        srcDimIdxToDesDimIdx[i] = 4;
        break;
      default:
        CHECK(false);
        break;
    }
  }

  std::vector<size_t> desLocs(5, 0);
  const uint8_t* srcLoc = buf;
  size_t srcStride = img.voxelByteNumber();
  if ('X' == dimensionOrder[0]) {
    srcStride *= img.width();
    if ('Y' == dimensionOrder[1]) {
      srcStride *= img.height();
      if ('Z' == dimensionOrder[2]) {
        srcStride *= img.depth();
        for (size_t i4 = 0; i4 < srcDimSizes[4]; ++i4) {
          desLocs[srcDimIdxToDesDimIdx[4]] = i4;
          for (size_t i3 = 0; i3 < srcDimSizes[3]; ++i3) {
            desLocs[srcDimIdxToDesDimIdx[3]] = i3;
            size_t desC = desLocs[3];
            if (BGRtoRGB) {
              if (desC == 0) {
                desC = 2;
              } else if (desC == 2) {
                desC = 0;
              }
            }
            uint8_t* desLoc = img.channelData<uint8_t>(desC, desLocs[4]);
            std::memcpy(desLoc, srcLoc, srcStride);
            srcLoc += srcStride;
          }
        }
      } else {
        for (size_t i4 = 0; i4 < srcDimSizes[4]; ++i4) {
          desLocs[srcDimIdxToDesDimIdx[4]] = i4;
          for (size_t i3 = 0; i3 < srcDimSizes[3]; ++i3) {
            desLocs[srcDimIdxToDesDimIdx[3]] = i3;
            for (size_t i2 = 0; i2 < srcDimSizes[2]; ++i2) {
              desLocs[srcDimIdxToDesDimIdx[2]] = i2;
              size_t desC = desLocs[3];
              if (BGRtoRGB) {
                if (desC == 0) {
                  desC = 2;
                } else if (desC == 2) {
                  desC = 0;
                }
              }
              uint8_t* desLoc = img.planeData<uint8_t>(desLocs[2], desC, desLocs[4]);
              std::memcpy(desLoc, srcLoc, srcStride);
              srcLoc += srcStride;
            }
          }
        }
      }
    } else {
      for (size_t i4 = 0; i4 < srcDimSizes[4]; ++i4) {
        desLocs[srcDimIdxToDesDimIdx[4]] = i4;
        for (size_t i3 = 0; i3 < srcDimSizes[3]; ++i3) {
          desLocs[srcDimIdxToDesDimIdx[3]] = i3;
          for (size_t i2 = 0; i2 < srcDimSizes[2]; ++i2) {
            desLocs[srcDimIdxToDesDimIdx[2]] = i2;
            for (size_t i1 = 0; i1 < srcDimSizes[1]; ++i1) {
              desLocs[srcDimIdxToDesDimIdx[1]] = i1;
              size_t desC = desLocs[3];
              if (BGRtoRGB) {
                if (desC == 0) {
                  desC = 2;
                } else if (desC == 2) {
                  desC = 0;
                }
              }
              uint8_t* desLoc = img.rowData<uint8_t>(desLocs[1], desLocs[2], desC, desLocs[4]);
              std::memcpy(desLoc, srcLoc, srcStride);
              srcLoc += srcStride;
            }
          }
        }
      }
    }
  } else {
    for (size_t i4 = 0; i4 < srcDimSizes[4]; ++i4) {
      desLocs[srcDimIdxToDesDimIdx[4]] = i4;
      for (size_t i3 = 0; i3 < srcDimSizes[3]; ++i3) {
        desLocs[srcDimIdxToDesDimIdx[3]] = i3;
        for (size_t i2 = 0; i2 < srcDimSizes[2]; ++i2) {
          desLocs[srcDimIdxToDesDimIdx[2]] = i2;
          for (size_t i1 = 0; i1 < srcDimSizes[1]; ++i1) {
            desLocs[srcDimIdxToDesDimIdx[1]] = i1;
            for (size_t i0 = 0; i0 < srcDimSizes[0]; ++i0) {
              desLocs[srcDimIdxToDesDimIdx[0]] = i0;
              size_t desC = desLocs[3];
              if (BGRtoRGB) {
                if (desC == 0) {
                  desC = 2;
                } else if (desC == 2) {
                  desC = 0;
                }
              }
              uint8_t* desLoc = img.data<uint8_t>(desLocs[0], desLocs[1], desLocs[2], desC, desLocs[4]);
              std::memcpy(desLoc, srcLoc, srcStride);
              srcLoc += srcStride;
            }
          }
        }
      }
    }
  }
}

void ZImgFormat::createDefaultSubBlocks(const QString& filename,
                                        const std::vector<ZImgInfo>& infos,
                                        std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  if (!subBlocks)
    return;
  subBlocks->resize(infos.size());
  for (size_t s = 0; s < infos.size(); ++s) {
    for (size_t t = 0; t < infos[s].numTimes; ++t) {
      for (size_t z = 0; z < infos[s].depth; ++z) {
        (*subBlocks)[s].emplace_back(std::make_shared<ZImgTileSubBlock>(
          ZImgSource(filename,
                     ZImgRegion(ZVoxelCoordinate(0, 0, z, 0, t),
                                          ZVoxelCoordinate(infos[s].width, infos[s].height, z + 1, infos[s].numChannels, t + 1)),
                     s)));
      }
    }
  }
}

void ZImgFormat::createEmptySubBlocks(const std::vector<ZImgInfo>& infos,
                                      std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  if (!subBlocks)
    return;
  subBlocks->resize(infos.size());
}

} // namespace nim
