#include "zimgformat.h"

#include "zioutils.h"
#include "zimgsliceprovider.h"
#include "zimgblockprovider.h"
#include "zlog.h"
#include "zimgio.h"

namespace nim {

ZImgCommonSubBlock::ZImgCommonSubBlock(const QString& fileName, FileFormat format, size_t scene, size_t ratio, size_t t,
                                       size_t z, size_t x, size_t y, size_t width, size_t height)
  : ZImgSubBlock(ratio, t, z, x, y, width, height)
{
  ZImgRegion rgn;
  rgn.start.t = t;
  rgn.end.t = t + 1;
  rgn.start.z = z;
  rgn.end.z = z + 1;
  rgn.start.x = x;
  rgn.end.x = x + width;
  rgn.start.y = y;
  rgn.end.y = y + height;
  m_imgSource = ZImgSource(fileName, rgn, scene, format);
}

ZImgCommonSubBlock::ZImgCommonSubBlock(const QStringList& fileList, Dimension catDim, FileFormat format, size_t scene,
                                       size_t ratio, size_t t, size_t z, size_t x, size_t y, size_t width,
                                       size_t height)
  : ZImgSubBlock(ratio, t, z, x, y, width, height)
{
  ZImgRegion rgn;
  rgn.start.t = t;
  rgn.end.t = t + 1;
  rgn.start.z = z;
  rgn.end.z = z + 1;
  rgn.start.x = x;
  rgn.end.x = x + width;
  rgn.start.y = y;
  rgn.end.y = y + height;
  m_imgSource = ZImgSource(fileList, catDim, rgn, scene, format);
}

std::shared_ptr<ZImg> ZImgCommonSubBlock::read() const
{
  return std::make_shared<ZImg>(m_imgSource);
}

ZImgInfo ZImgCommonSubBlock::readInfo() const
{
  ZImgInfo info;
  ZImgIO::instance().readInfo(m_imgSource, info);
  return info;
}

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

void ZImgFormat::writeImg(const QString& /*filename*/, const ZImg& /*img*/, Compression /*comp*/)
{
}

void ZImgFormat::writeImg(const QString& filename,
                          const ZImgSliceProvider& imgSliceProvider,
                          Compression comp)
{
  if (canWrite(filename)) {
    writeImg(filename, imgSliceProvider.wholeImg(1), comp);
  }
}

void ZImgFormat::writeImg(const QString& filename,
                          const ZImgBlockProvider& imgBlockProvider,
                          Compression comp)
{
  if (canWrite(filename)) {
    writeImg(filename, imgBlockProvider.wholeImg(), comp);
  }
}

ZImg ZImgFormat::readRawImg(const QString& filename, const ZImgInfo& imgInfo, const QString& dimensionOrder,
                            uint64_t dataOffset, const ZImgRegion& region)
{
  if (region.isEmpty() || !region.isValid(imgInfo)) {
    throw ZIOException(
      QString("Invalid image region. Image info: '%1', region: '%2'").arg(imgInfo.toQString()).arg(region.toQString()));
  }
  if (dimensionOrder != "ZCTL" && dimensionOrder != "CZTL" && dimensionOrder != "ZTL") {
    throw ZIOException(QString("Not supported dimension order: %1").arg(dimensionOrder));
  }

  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

  ZImgInfo partialImgInfo = region.clip(imgInfo);
  ZImg res(partialImgInfo);

  if (region.containsWholeTime(imgInfo) && (dimensionOrder == "ZCTL" || dimensionOrder == "ZTL")) {
    inputFileStream.seekg(dataOffset, std::ios_base::beg);
    readStream(inputFileStream, res.timeData<char>(0), res.timeByteNumber());
  } else if (region.containsWholeChannel(imgInfo) && (dimensionOrder == "ZCTL" || dimensionOrder == "ZTL")) {
    size_t offset = dataOffset + region.start.c * imgInfo.channelByteNumber();
    if (offset > 0)
      inputFileStream.seekg(offset, std::ios_base::beg);
    readStream(inputFileStream, res.timeData<char>(0), res.timeByteNumber());
  } else if (region.containsWholePlane(imgInfo) && (dimensionOrder == "ZCTL" || dimensionOrder == "ZTL")) {
    int cEnd = region.end.c == -1 ? imgInfo.numChannels : region.end.c;
    // channel by channel
    for (int c = region.start.c; c < cEnd; ++c) {
      size_t offset = dataOffset + c * imgInfo.channelByteNumber() + region.start.z * imgInfo.planeByteNumber();
      inputFileStream.seekg(offset, std::ios_base::beg);
      readStream(inputFileStream, res.channelData<char>(c - region.start.c, 0), res.channelByteNumber());
    }
  } else if (region.containsWholeRow(imgInfo)) {
    int cEnd = region.end.c == -1 ? imgInfo.numChannels : region.end.c;
    int zEnd = region.end.z == -1 ? imgInfo.depth : region.end.z;
    // plane by plane
    for (int c = region.start.c; c < cEnd; ++c) {
      for (int z = region.start.z; z < zEnd; ++z) {
        size_t offset = 0;
        if ((dimensionOrder == "ZCTL" || dimensionOrder == "ZTL")) {
          offset = dataOffset + c * imgInfo.channelByteNumber() + z * imgInfo.planeByteNumber() +
                   region.start.y * imgInfo.rowByteNumber();
        } else { // "CZTL"
          offset = dataOffset + (c + imgInfo.numChannels * z) * imgInfo.planeByteNumber() +
                   region.start.y * imgInfo.rowByteNumber();
        }
        inputFileStream.seekg(offset, std::ios_base::beg);
        readStream(inputFileStream, res.planeData<char>(z - region.start.z, c - region.start.c, 0),
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
          if ((dimensionOrder == "ZCTL" || dimensionOrder == "ZTL")) {
            offset = dataOffset + c * imgInfo.channelByteNumber() + z * imgInfo.planeByteNumber() +
                     y * imgInfo.rowByteNumber() + region.start.x * imgInfo.voxelByteNumber();
          } else { // "CZTL"
            offset = dataOffset + (c + imgInfo.numChannels * z) * imgInfo.planeByteNumber() +
                     y * imgInfo.rowByteNumber() + region.start.x * imgInfo.voxelByteNumber();
          }
          inputFileStream.seekg(offset, std::ios_base::beg);
          readStream(inputFileStream, res.rowData<char>(y - region.start.y, z - region.start.z, c - region.start.c, 0),
                     res.rowByteNumber());
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
                                        std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                                        std::vector<std::set<size_t>>* pyramidalRatios)
{
  if (pyramidalRatios) {
    pyramidalRatios->resize(1);
    (*pyramidalRatios)[0].insert(1);
  }
  if (!subBlocks)
    return;
  subBlocks->resize(infos.size());
  for (size_t s = 0; s < infos.size(); ++s) {
    for (size_t t = 0; t < infos[s].numTimes; ++t) {
      for (size_t z = 0; z < infos[s].depth; ++z) {
        (*subBlocks)[s].emplace_back(std::make_shared<ZImgCommonSubBlock>(filename, format(), s, 1, t, z,
                                                                          0, 0, infos[s].width, infos[s].height));
      }
    }
  }
}

void ZImgFormat::createEmptySubBlocks(const std::vector<ZImgInfo>& infos,
                                      std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                                      std::vector<std::set<size_t>>* pyramidalRatios)
{
  if (pyramidalRatios) {
    pyramidalRatios->resize(1);
    (*pyramidalRatios)[0].insert(1);
  }
  if (!subBlocks)
    return;
  subBlocks->resize(infos.size());
}

} // namespace nim
