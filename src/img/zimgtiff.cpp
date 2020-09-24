#include "zimgtiff.h"

#include "zlog.h"
#include "zimgsliceprovider.h"
#include "ztiff.h"
#include <cmath>
#include <set>

namespace nim {

QString ZImgTiff::shortName() const
{
  return "Tiff";
}

QString ZImgTiff::fullName() const
{
  return "Tiff";
}

QStringList ZImgTiff::extensions() const
{
  QStringList res;
  res << "tif" << "tiff";
  return res;
}

void ZImgTiff::readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                        std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  clearInternalState();
  ZTiff tiff;
  readIntoInternalStructure(filename, tiff);
  detectImgInfo(tiff);
  infos = m_imgInfo;

  createDefaultSubBlocks(filename, infos, subBlocks);

  if (!m_imageDescription.isEmpty()) {
    LOG(INFO) << m_imageDescription;
  }
}

void ZImgTiff::readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene)
{
  clearInternalState();
  ZTiff tiff;
  readIntoInternalStructure(filename, tiff);
  detectImgInfo(tiff);
  if (scene >= m_imgInfo.size()) {
    throw ZIOException("invalid scene");
  }
  readMetadataInternal(meta, scene, tiff);
}

void
ZImgTiff::readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene)
{
  clearInternalState();
  ZTiff tiff;
  readIntoInternalStructure(filename, tiff);
  detectImgInfo(tiff);
  if (scene >= m_imgInfo.size()) {
    throw ZIOException("invalid scene");
  }
  readThumbnailInternal(thumbnail, region, scene, tiff);
}

void ZImgTiff::readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene)
{
  clearInternalState();
  ZTiff tiff;
  readIntoInternalStructure(filename, tiff);
  detectImgInfo(tiff);
  if (scene >= m_imgInfo.size()) {
    throw ZIOException("invalid scene");
  }
  const std::vector<ZTiffIFD>& ifds = tiff.ifds();

  if (m_isImageJTiff && m_onlyOneIFDInImageJTiff) {
    if (!ifds[0].isNormalImage() || ifds[0].compression() != enumToUnderlyingType(Compression::NONE) ||
        ifds[0].isTiledImage() || ifds[0].stripsPerImage() != 1) {
      throw ZIOException("Wrong ImageJ Tiff file");
    }
    auto dimensionOrder = m_dimensionOrder;
    dimensionOrder.remove('L');
    dimensionOrder.prepend("XY");
    if (dimensionOrder == "XYZT")
      dimensionOrder = "XYZCT";
    CHECK(dimensionOrder.size() == 5);
    img = readRawImg(filename, m_imgInfo[scene], dimensionOrder, ifds[0].stripOffsets(0), region);
    if (!tiff.isNativeEndianness()) {
      img.reverseEndianness();
    }
    return;
  }

  if (region.isEmpty() || !region.isValid(m_imgInfo[scene])) {
    throw ZIOException(
      QString("Invalid image region. Image info: '%1', region: '%2'").arg(m_imgInfo[scene].toQString()).arg(
        region.toQString()));
  }

  ZImgInfo imgInfo2D(m_imgInfo[scene]);
  imgInfo2D.depth = 1;
  imgInfo2D.numTimes = 1;
  if (m_dimensionOrder.contains(QChar('C')))
    imgInfo2D.numChannels = 1;
  ZImg buf2DImg(imgInfo2D);
  //LOG(INFO) << imgInfo2D.toQString();

  ZImgInfo partialImgInfo = region.clip(m_imgInfo[scene]);
  ZImg imgTmp(partialImgInfo);
  //LOG(INFO) << partialImgInfo.toQString();

  int z = 0;
  int c = 0;
  int t = 0;
  int l = 0;
  size_t ifdIdx = 0;

  for (size_t i = 0; i < ifds.size(); ++i) {
    if (ifds[i].isNormalImage()) {
      mapIFDToImgLocation(ifdIdx, z, c, t, l);
      if ((region.zInRegion(z)) &&
          (region.cInRegion(c) || c == -1) &&
          (region.tInRegion(t)) &&
          (static_cast<int>(scene) == l)) {
        tiff.readImgFromIFD(i, buf2DImg);
        //LOG(INFO) << ifdIdx << " " << z << " " << c << " " << t << " " << l;
        cpyImg(buf2DImg, region, imgTmp, z - region.start.z, c, t - region.start.t);
      }
      ifdIdx++;
    }
  }

  readMetadataInternal(imgTmp.metadataRef(), scene, tiff);
  readThumbnailInternal(imgTmp.thumbnailRef(), region, scene, tiff);
  imgTmp.swap(img);
}

void ZImgTiff::writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, img.info(), paras);

  ZTiffWriter tiffWriter;
  int extraSample = img.info().lastChannelIsAlphaChannel ? 2 : -1;  //EXTRASAMPLE_UNASSALPHA or none
  if (img.byteNumber() > 1024_usize * 1024 * 3600) {
    tiffWriter.startWriting(filename, paras.compression, extraSample, true);
  } else {
    tiffWriter.startWriting(filename, paras.compression, extraSample, false);
  }
  for (size_t t = 0; t < img.numTimes(); ++t) {
    for (size_t z = 0; z < img.depth(); ++z) {
      tiffWriter.writeIFD(img, z, t, -1, true);
    }
  }
}

void ZImgTiff::writeImg(const QString& filename, const ZImgSliceProvider& imgSliceProvider,
                        const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, imgSliceProvider.imgInfo(), paras);

  ZTiffWriter tiffWriter;
  int extraSample = imgSliceProvider.imgInfo().lastChannelIsAlphaChannel ? 2 : -1;  //EXTRASAMPLE_UNASSALPHA or none
  if (imgSliceProvider.imgInfo().byteNumber() > 1024_usize * 1024 * 3600) {
    tiffWriter.startWriting(filename, paras.compression, extraSample, true);
  } else {
    tiffWriter.startWriting(filename, paras.compression, extraSample, false);
  }
  for (size_t t = 0; t < imgSliceProvider.imgInfo().numTimes; ++t) {
    for (size_t z = 0; z < imgSliceProvider.imgInfo().depth; ++z) {
      tiffWriter.writeIFD(imgSliceProvider.slice(z, t), 0, 0, -1, true);
    }
  }
}

bool ZImgTiff::supportRead() const
{
  return true;
}

bool ZImgTiff::supportWrite() const
{
  return true;
}

void ZImgTiff::readIntoInternalStructure(const QString& filename, ZTiff& tiff)
{
  tiff.load(filename);
  if (!tiff.isValid()) {
    throw ZIOException("No Image in Tiff");
  }
  m_imageDescription = tiff.ifds()[0].imageDescriptionAsQString();
}

void ZImgTiff::clearInternalState()
{
  m_imgInfo.clear();
  m_dimensionOrder = "ZTL";
  m_startIFDIndex = 0;
  m_imageDescription.clear();
  m_isImageJTiff = false;
  m_onlyOneIFDInImageJTiff = false;
}

void ZImgTiff::detectImgInfo(ZTiff& tiff)
{
  const std::vector<ZTiffIFD>& ifds = tiff.ifds();
  std::set<size_t> widths;
  std::set<size_t> heights;
  std::set<size_t> numChannels;
  std::set<size_t> bytesPerVoxels;
  std::set<VoxelFormat> voxelFormats;
  std::set<bool> alphaChannel;
  std::set<size_t> validBitCounts;
  m_imgInfo.resize(1);
  m_imgInfo[0].depth = 0;
  m_imgInfo[0].numTimes = 1;
  for (size_t i = 0; i < ifds.size(); ++i) {
    if (ifds[i].isNormalImage()) {
      ZImgInfo tmpInfo;
      tiff.readInfoFromIFD(ifds[i], tmpInfo);

      widths.insert(tmpInfo.width);
      heights.insert(tmpInfo.height);
      numChannels.insert(tmpInfo.numChannels);
      bytesPerVoxels.insert(tmpInfo.bytesPerVoxel);
      voxelFormats.insert(tmpInfo.voxelFormat);
      alphaChannel.insert(tmpInfo.lastChannelIsAlphaChannel);
      validBitCounts.insert(tmpInfo.validBitCount);
      if (widths.size() != 1 || heights.size() != 1 || numChannels.size() != 1 || bytesPerVoxels.size() != 1 ||
          voxelFormats.size() != 1 ||
          alphaChannel.size() != 1 || validBitCounts.size() != 1)
        throw ZIOException("Different image dimensions or formats is not supported.");

      m_imgInfo[0].depth++;
    }
  }
  if (m_imgInfo[0].depth > 0) {
    m_imgInfo[0].width = *widths.begin();
    m_imgInfo[0].height = *heights.begin();
    m_imgInfo[0].numChannels = *numChannels.begin();
    m_imgInfo[0].bytesPerVoxel = *bytesPerVoxels.begin();
    m_imgInfo[0].voxelFormat = *voxelFormats.begin();
    m_imgInfo[0].lastChannelIsAlphaChannel = *alphaChannel.begin();
    m_imgInfo[0].validBitCount = *validBitCounts.begin();

    if (m_imageDescription.startsWith("ImageJ=") && m_imageDescription.contains("images=")) {
      m_isImageJTiff = true;
      size_t images = 0;
      size_t channels = 1;
      size_t slices = 1;
      size_t frames = 1;
      bool hyperstack = false;
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
      QStringList fields = m_imageDescription.split("\n", Qt::SkipEmptyParts);
#else
      QStringList fields = m_imageDescription.split("\n", QString::SkipEmptyParts);
#endif
      for (int i = 0; i < fields.size(); ++i) {
        if (fields[i].startsWith("images=")) {
          images = fields[i].remove(0, 7).toInt();
        } else if (fields[i].startsWith("channels=")) {
          channels = fields[i].remove(0, 9).toInt();
        } else if (fields[i].startsWith("slices=")) {
          slices = fields[i].remove(0, 7).toInt();
        } else if (fields[i].startsWith("frames=")) {
          frames = fields[i].remove(0, 7).toInt();
        } else if (fields[i].startsWith("hyperstack=true")) {
          hyperstack = true;
        }
      }

      if (images != channels * slices * frames) {
        throw ZIOException(QString("Not supported ImageJ tiff: %1").arg(m_imageDescription));
      }

      if (images > 1 && ifds.size() == 1) {
        m_onlyOneIFDInImageJTiff = true;
      }
      m_imgInfo[0].numChannels = std::max(m_imgInfo[0].numChannels, channels);
      m_imgInfo[0].depth = slices;
      m_imgInfo[0].numTimes = frames;

      if (channels > 1) {
        if (hyperstack) {
          m_dimensionOrder = "CZTL";
        } else {
          m_dimensionOrder = "ZCTL";
        }
      } else {
        m_dimensionOrder = "ZTL";
      }
    }

    m_imgInfo[0].createDefaultDescriptions();
  } else {
    throw ZIOException("No Image in Tiff");
  }
}

void ZImgTiff::readMetadataInternal(ZImgMetadata& meta, size_t scene, ZTiff& tiff)
{
  const std::vector<ZTiffIFD>& ifds = tiff.ifds();
  int z = 0;
  int c = 0;
  int t = 0;
  int l = 0;
  size_t ifdIdx = 0;
  if (!m_imageDescription.isEmpty()) {
    ZImgMetatag tag("metadata", m_imageDescription);
    meta.attachToTopLevel(tag);
  }
  for (size_t i = 0; i < ifds.size(); ++i) {
    if (ifds[i].isNormalImage()) {
      mapIFDToImgLocation(ifdIdx, z, c, t, l);
      if (l != static_cast<int>(scene))
        continue;
      std::vector<ZImgMetatag> tags = ifds[i].extractMetadata();
      if (!tags.empty()) {
        if (c == -1)
          meta.attachToPlane(tags, z, t);
        else
          meta.attachToSingleChannelPlane(tags, z, c, t);
      }
      ifdIdx++;
    }
  }
}

void ZImgTiff::readThumbnailInternal(ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene, ZTiff& tiff)
{
  const std::vector<ZTiffIFD>& ifds = tiff.ifds();

  if (region.isEmpty() || !region.isValid(m_imgInfo[scene])) {
    throw ZIOException(
      QString("Invalid image region. Image info: '%1', region: '%2'").arg(m_imgInfo[scene].toQString()).arg(
        region.toQString()));
  }

  // thumbnail follows image or in subifd
  int z = 0;
  int c = 0;
  int t = 0;
  int l = 0;
  size_t ifdIdx = 0;
  for (size_t i = 0; i < ifds.size(); ++i) {
    if (ifds[i].isNormalImage()) {
      mapIFDToImgLocation(ifdIdx, z, c, t, l);
      if ((region.zInRegion(z)) &&
          (region.cInRegion(c) || c == -1) &&
          (region.tInRegion(t)) &&
          (static_cast<int>(scene) == l)) {
        const std::vector<ZTiffIFD>& subifds = ifds[i].subIFDs();
        for (size_t sub = 0; sub < subifds.size(); ++sub) {
          ZImg thumb = tiff.readThumbnailFromIFD(subifds[sub]);
          if (!thumb.isEmpty())
            thumbnail.attachToPlane(thumb, z - region.start.z, t - region.start.t);
        }
      }
      ifdIdx++;
    } else if (ifds[i].isReducedResolutionImage() && ifdIdx > 0) {
      mapIFDToImgLocation(ifdIdx - 1, z, c, t, l);
      if ((region.zInRegion(z)) &&
          (region.cInRegion(c) || c == -1) &&
          (region.tInRegion(t)) &&
          (static_cast<int>(scene) == l)) {
        ZImg thumb = tiff.readThumbnailFromIFD(ifds[i]);
        if (!thumb.isEmpty()) {
          thumbnail.attachToPlane(thumb, z - region.start.z, t - region.start.t);
        }
      }
    }
  }
}

void ZImgTiff::setDimensionOrder(const QString& order)
{
  if (isDimensionOrderValid(order)) {
    m_dimensionOrder = order;
  } else {
    throw ZIOException(QString("Wrong dimension order: %1").arg(order));
  }
}

bool ZImgTiff::mapIFDToImgLocation(size_t ifdIdx, int& z, int& c, int& t, int& l)
{
  return IFDToLoc(ifdIdx, z, c, t, l, m_startIFDIndex, m_dimensionOrder, m_imgInfo[0], m_imgInfo.size());
}

bool ZImgTiff::isDimensionOrderValid(const QString& order)
{
  return (order.size() == 3 && order.contains(QChar('Z')) && order.contains(QChar('T')) &&
          order.contains(QChar('L'))) ||
         (order.size() == 4 && order.contains(QChar('Z')) && order.contains(QChar('C')) &&
          order.contains(QChar('T')) && order.contains(QChar('L')));
}

bool ZImgTiff::IFDToLoc(size_t ifdIdx, int& z, int& c, int& t, int& l,
                        size_t startIFDIndex, const QString& dimensionOrder, const ZImgInfo& imgInfo,
                        size_t numScenes,
                        int startZ, int startC, int startT, int startL)
{
  size_t ndims = dimensionOrder.size();
  std::vector<size_t> dimSizes;
  std::vector<int> dimStarts;
  for (size_t i = 0; i < ndims; ++i) {
    if (dimensionOrder.at(i) == QChar('Z')) {
      dimSizes.push_back(imgInfo.depth);
      dimStarts.push_back(startZ);
    } else if (dimensionOrder.at(i) == QChar('C')) {
      dimSizes.push_back(imgInfo.numChannels);
      dimStarts.push_back(startC);
    } else if (dimensionOrder.at(i) == QChar('T')) {
      dimSizes.push_back(imgInfo.numTimes);
      dimStarts.push_back(startT);
    } else if (dimensionOrder.at(i) == QChar('L')) {
      dimSizes.push_back(numScenes);
      dimStarts.push_back(startL);
    }
  }

  std::vector<size_t> dimStrides(ndims, 1);
  for (size_t i = 1; i < ndims; ++i)
    dimStrides[i] = dimStrides[i - 1] * dimSizes[i - 1];

  size_t maxIFDNum = dimStrides[ndims - 1] * dimSizes[ndims - 1];
  if (ifdIdx < startIFDIndex || ifdIdx >= startIFDIndex + maxIFDNum)
    return false;
  ifdIdx -= startIFDIndex;

  std::vector<size_t> locs(ndims);
  locs[0] = ifdIdx % dimSizes[0];
  for (size_t i = 1; i < ndims; ++i) {
    locs[i] = ifdIdx % (dimStrides[i] * dimSizes[i]);
    locs[i] /= dimStrides[i];
  }

  // add start location to result location
  for (size_t i = 0; i < ndims; ++i) {
    locs[i] += dimStarts[i];
    if (locs[i] >= dimSizes[i]) {
      if (i < ndims - 1) {
        locs[i] %= dimSizes[i];
        locs[i + 1] += 1;
      } else {
        return false;
      }
    }
  }

  for (size_t i = 0; i < ndims; ++i) {
    if (dimensionOrder.at(i) == QChar('Z'))
      z = locs[i];
    else if (dimensionOrder.at(i) == QChar('C'))
      c = locs[i];
    else if (dimensionOrder.at(i) == QChar('T'))
      t = locs[i];
    else if (dimensionOrder.at(i) == QChar('L'))
      l = locs[i];
  }

  if (ndims == 3) {
    c = -1;
  }

  //LOG(INFO) << ifdIdx << " " << z << " " << c << " " << t << " " << l << " " << startIFDIndex << " " << dimensionOrder << " " << imgInfo.toQString() << " "
  //         << startZ << " " << startC << " " << startT << " " << startL;
  return true;
}

void ZImgTiff::cpyImg(const ZImg& img2D, const ZImgRegion& region, ZImg& img, int z, int c, int t)
{
  if (c < 0) {
    size_t cEnd = region.end.c == -1 ? img2D.numChannels() : region.end.c;
    for (size_t lc = region.start.c; lc < cEnd; ++lc) {
      if (region.containsWholeRow(img2D.info())) {
        std::memcpy(img.planeData<uint8_t>(z, lc - region.start.c, t), img2D.rowData<uint8_t>(region.start.y, 0, lc, 0),
                    img.planeByteNumber());
      } else {
        size_t yEnd = region.end.y == -1 ? img2D.height() : region.end.y;
        for (size_t y = region.start.y; y < yEnd; ++y) {
          std::memcpy(img.rowData<uint8_t>(y - region.start.y, z, lc - region.start.c, t),
                      img2D.data<uint8_t>(region.start.x, y, 0, lc, 0), img.rowByteNumber());
        }
      }
    }
  } else if (region.cInRegion(c)) {
    if (region.containsWholeRow(img2D.info())) {
      std::memcpy(img.planeData<uint8_t>(z, c - region.start.c, t), img2D.rowData<uint8_t>(region.start.y, 0, 0, 0),
                  img.planeByteNumber());
    } else {
      size_t yEnd = region.end.y == -1 ? img2D.height() : region.end.y;
      for (size_t y = region.start.y; y < yEnd; ++y) {
        std::memcpy(img.rowData<uint8_t>(y - region.start.y, z, c - region.start.c, t),
                    img2D.data<uint8_t>(region.start.x, y, 0, 0, 0), img.rowByteNumber());
      }
    }
  }
}

} // namespace nim
