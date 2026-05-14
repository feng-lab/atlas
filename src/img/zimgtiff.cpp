#include "zimgtiff.h"

#include "zlog.h"
#include "zimgsliceprovider.h"
#include "ztiff.h"
#include "zstringutils.h"
#include <algorithm>
#include <cmath>

namespace nim {

namespace {

constexpr size_t kTiffDefaultTileSize = 512;

bool firstNormalTiffTileSize(const ZTiff& tiff, size_t& tileWidth, size_t& tileHeight)
{
  for (const ZTiffIFD& ifd : tiff.ifds()) {
    if (!ifd.isNormalImage()) {
      continue;
    }
    if (!ifd.isTiledImage()) {
      return false;
    }
    tileWidth = static_cast<size_t>(ifd.tileWidth());
    tileHeight = static_cast<size_t>(ifd.tileHeight());
    return tileWidth > 0 && tileHeight > 0;
  }
  return false;
}

} // namespace

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
  res << "tif"
      << "tiff";
  return res;
}

void ZImgTiff::readInfo(const QString& filename,
                        std::vector<ZImgInfo>& infos,
                        std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  clearInternalState();
  ZTiff tiff;
  readIntoInternalStructure(filename, tiff);
  detectImgInfo(tiff);
  infos = m_imgInfo;

  size_t tileWidth = kTiffDefaultTileSize;
  size_t tileHeight = kTiffDefaultTileSize;
  firstNormalTiffTileSize(tiff, tileWidth, tileHeight);
  createTiledSubBlocks(filename, infos, subBlocks, tileWidth, tileHeight, FileFormat::Tiff);

  if (!m_imageDescription.empty()) {
    VLOG(1) << m_imageDescription;
  }
}

void ZImgTiff::readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene)
{
  clearInternalState();
  ZTiff tiff;
  readIntoInternalStructure(filename, tiff);
  detectImgInfo(tiff);
  if (scene >= m_imgInfo.size()) {
    throw ZException("invalid scene");
  }
  readMetadataInternal(meta, scene, tiff);
}

void ZImgTiff::readThumbnail(const QString& filename,
                             ZImgThumbernail& thumbnail,
                             const ZImgRegion& region,
                             size_t scene)
{
  clearInternalState();
  ZTiff tiff;
  readIntoInternalStructure(filename, tiff);
  detectImgInfo(tiff);
  if (scene >= m_imgInfo.size()) {
    throw ZException("invalid scene");
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
    throw ZException("invalid scene");
  }
  const std::vector<ZTiffIFD>& ifds = tiff.ifds();

  if (m_isImageJTiff && m_onlyOneIFDInImageJTiff) {
    if (!ifds[0].isNormalImage() || ifds[0].compression() != getTiffCompressionTag(Compression::NONE) ||
        ifds[0].isTiledImage() || ifds[0].stripsPerImage() != 1) {
      throw ZException("Wrong ImageJ Tiff file");
    }
    auto dimensionOrder = m_dimensionOrder;
    dimensionOrder.remove('L');
    dimensionOrder.prepend("XY");
    if (dimensionOrder == "XYZT") {
      dimensionOrder = "XYZCT";
    }
    CHECK(dimensionOrder.size() == 5);
    img = readRawImg(filename, m_imgInfo[scene], dimensionOrder, ifds[0].stripOffsets(0), region);
    if (!tiff.isNativeEndianness()) {
      img.reverseEndianness();
    }
    return;
  }

  if (region.isEmpty() || !region.isValid(m_imgInfo[scene])) {
    throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", m_imgInfo[scene], region));
  }

  ZImgInfo partialImgInfo = region.clip(m_imgInfo[scene]);
  ZImg imgTmp(partialImgInfo);
  ZImgRegion resolvedRegion = region;
  resolvedRegion.resolveRegionEnd(m_imgInfo[scene]);
  // VLOG(1) << partialImgInfo;

  index_t z = 0;
  index_t c = 0;
  index_t t = 0;
  index_t l = 0;
  size_t ifdIdx = 0;

  for (size_t i = 0; i < ifds.size(); ++i) {
    if (ifds[i].isNormalImage()) {
      if (!mapIFDToImgLocation(ifdIdx, z, c, t, l)) {
        break;
      }
      if ((resolvedRegion.zInRegion(z)) && (c == -1 || resolvedRegion.cInRegion(c)) && (resolvedRegion.tInRegion(t)) &&
          (scene == static_cast<size_t>(l))) {
        // VLOG(1) << ifdIdx << " " << z << " " << c << " " << t << " " << l;
        ZImg planeImg;
        if (c < 0) {
          tiff.readRegionFromIFD(ifds[i],
                                 planeImg,
                                 ZImgRegion(resolvedRegion.start.x,
                                            resolvedRegion.end.x,
                                            resolvedRegion.start.y,
                                            resolvedRegion.end.y,
                                            0,
                                            1,
                                            resolvedRegion.start.c,
                                            resolvedRegion.end.c,
                                            0,
                                            1));
          imgTmp.pasteImg(planeImg,
                          ZVoxelCoordinate(0, 0, z - resolvedRegion.start.z, 0, t - resolvedRegion.start.t),
                          false);
        } else {
          tiff.readRegionFromIFD(ifds[i],
                                 planeImg,
                                 ZImgRegion(resolvedRegion.start.x,
                                            resolvedRegion.end.x,
                                            resolvedRegion.start.y,
                                            resolvedRegion.end.y,
                                            0,
                                            1,
                                            0,
                                            1,
                                            0,
                                            1));
          imgTmp.pasteImg(
            planeImg,
            ZVoxelCoordinate(0, 0, z - resolvedRegion.start.z, c - resolvedRegion.start.c, t - resolvedRegion.start.t),
            false);
        }
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
  int32_t extraSample = img.info().lastChannelIsAlphaChannel ? 2 : -1; // EXTRASAMPLE_UNASSALPHA or none
  if (img.byteNumber() > 1024_uz * 1024 * 3600) {
    tiffWriter.startWriting(filename, paras.compression, extraSample, true);
  } else {
    tiffWriter.startWriting(filename, paras.compression, extraSample, false);
  }
  for (size_t t = 0; t < img.numTimes(); ++t) {
    for (size_t z = 0; z < img.depth(); ++z) {
      tiffWriter.writeIFD(img, z, t, -1, true, {}, nullptr, kTiffDefaultTileSize, kTiffDefaultTileSize);
    }
  }
}

void ZImgTiff::writeImg(const QString& filename,
                        const ZImgSliceProvider& imgSliceProvider,
                        const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, imgSliceProvider.imgInfo(), paras);

  ZTiffWriter tiffWriter;
  int32_t extraSample = imgSliceProvider.imgInfo().lastChannelIsAlphaChannel ? 2 : -1; // EXTRASAMPLE_UNASSALPHA or none
  if (imgSliceProvider.imgInfo().byteNumber() > 1024_uz * 1024 * 3600) {
    tiffWriter.startWriting(filename, paras.compression, extraSample, true);
  } else {
    tiffWriter.startWriting(filename, paras.compression, extraSample, false);
  }
  for (size_t t = 0; t < imgSliceProvider.imgInfo().numTimes; ++t) {
    for (size_t z = 0; z < imgSliceProvider.imgInfo().depth; ++z) {
      tiffWriter.writeIFD(imgSliceProvider.slice(z, t),
                          0,
                          0,
                          -1,
                          true,
                          {},
                          nullptr,
                          kTiffDefaultTileSize,
                          kTiffDefaultTileSize);
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
    throw ZException("No Image in Tiff", ZException::Option::CheckErrno);
  }
  m_imageDescription = tiff.ifds()[0].imageDescription();
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
  m_imgInfo.resize(1);
  m_imgInfo[0].depth = 0;
  m_imgInfo[0].numTimes = 1;
  for (const auto& ifd : ifds) {
    if (ifd.isNormalImage()) {
      ZImgInfo tmpInfo;
      tiff.readInfoFromIFD(ifd, tmpInfo);

      if (m_imgInfo[0].depth == 0) {
        m_imgInfo[0].width = tmpInfo.width;
        m_imgInfo[0].height = tmpInfo.height;
        m_imgInfo[0].numChannels = tmpInfo.numChannels;
        m_imgInfo[0].bytesPerVoxel = tmpInfo.bytesPerVoxel;
        m_imgInfo[0].voxelFormat = tmpInfo.voxelFormat;
        m_imgInfo[0].lastChannelIsAlphaChannel = tmpInfo.lastChannelIsAlphaChannel;
        m_imgInfo[0].validBitCount = tmpInfo.validBitCount;
      } else {
        if (m_imgInfo[0].width != tmpInfo.width || m_imgInfo[0].height != tmpInfo.height ||
            m_imgInfo[0].numChannels != tmpInfo.numChannels || m_imgInfo[0].bytesPerVoxel != tmpInfo.bytesPerVoxel ||
            m_imgInfo[0].voxelFormat != tmpInfo.voxelFormat ||
            m_imgInfo[0].lastChannelIsAlphaChannel != tmpInfo.lastChannelIsAlphaChannel ||
            m_imgInfo[0].validBitCount != tmpInfo.validBitCount) {
          LOG(WARNING)
            << "Different image dimensions or formats is not supported, might be Thumbnail but not marked properly";
          break;
        }
      }

      m_imgInfo[0].depth++;
    }
  }
  if (m_imgInfo[0].depth > 0) {
    static constexpr auto imagej_prefix = "ImageJ="sv;
    static constexpr auto images_prefix = "images="sv;
    static constexpr auto channels_prefix = "channels="sv;
    static constexpr auto slices_prefix = "slices="sv;
    static constexpr auto frames_prefix = "frames="sv;
    static constexpr auto hyperstack_prefix = "hyperstack=true"sv;
    if (m_imageDescription.starts_with(imagej_prefix) && absl::StrContains(m_imageDescription, images_prefix)) {
      m_isImageJTiff = true;
      size_t images = 0;
      size_t channels = 1;
      size_t slices = 1;
      size_t frames = 1;
      bool hyperstack = false;
      std::vector<std::string_view> fields = absl::StrSplit(m_imageDescription, '\n', absl::SkipEmpty());
      for (auto& field : fields) {
        if (field.starts_with(images_prefix)) {
          field.remove_prefix(images_prefix.size());
          stringToValue(field, images);
        } else if (field.starts_with(channels_prefix)) {
          field.remove_prefix(channels_prefix.size());
          stringToValue(field, channels);
        } else if (field.starts_with(slices_prefix)) {
          field.remove_prefix(slices_prefix.size());
          stringToValue(field, slices);
        } else if (field.starts_with(frames_prefix)) {
          field.remove_prefix(frames_prefix.size());
          stringToValue(field, frames);
        } else if (field.starts_with(hyperstack_prefix)) {
          hyperstack = true;
        }
      }

      if (images != channels * slices * frames) {
        throw ZException(fmt::format("Not supported ImageJ tiff: {}", m_imageDescription));
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
    throw ZException("No Image in Tiff", ZException::Option::CheckErrno);
  }
}

void ZImgTiff::readMetadataInternal(ZImgMetadata& meta, size_t scene, ZTiff& tiff)
{
  const std::vector<ZTiffIFD>& ifds = tiff.ifds();
  index_t z = 0;
  index_t c = 0;
  index_t t = 0;
  index_t l = 0;
  size_t ifdIdx = 0;
  if (!m_imageDescription.empty()) {
    ZImgMetatag tag("metadata", m_imageDescription);
    meta.attachToTopLevel(tag);
  }
  for (const auto& ifd : ifds) {
    if (ifd.isNormalImage()) {
      if (!mapIFDToImgLocation(ifdIdx, z, c, t, l)) {
        break;
      }
      if (static_cast<size_t>(l) != scene) {
        continue;
      }
      std::vector<ZImgMetatag> tags = ifd.extractMetadata();
      if (!tags.empty()) {
        if (c == -1) {
          meta.attachToPlane(tags, z, t);
        } else {
          meta.attachToSingleChannelPlane(tags, z, c, t);
        }
      }
      ifdIdx++;
    }
  }
}

void ZImgTiff::readThumbnailInternal(ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene, ZTiff& tiff)
{
  const std::vector<ZTiffIFD>& ifds = tiff.ifds();

  if (region.isEmpty() || !region.isValid(m_imgInfo[scene])) {
    throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", m_imgInfo[scene], region));
  }

  // thumbnail follows image or in subifd
  index_t z = 0;
  index_t c = 0;
  index_t t = 0;
  index_t l = 0;
  size_t ifdIdx = 0;
  for (const auto& ifd : ifds) {
    if (ifd.isNormalImage()) {
      if (!mapIFDToImgLocation(ifdIdx, z, c, t, l)) {
        break;
      }
      if ((region.zInRegion(z)) && (region.cInRegion(c) || c == -1) && (region.tInRegion(t)) &&
          (scene == static_cast<size_t>(l))) {
        const std::vector<ZTiffIFD>& subifds = ifd.subIFDs();
        for (const auto& subifd : subifds) {
          ZImg thumb = tiff.readThumbnailFromIFD(subifd);
          if (!thumb.isEmpty()) {
            thumbnail.attachToPlane(thumb, z - region.start.z, t - region.start.t);
          }
        }
      }
      ifdIdx++;
    } else if (ifd.isReducedResolutionImage() && ifdIdx > 0) {
      if (!mapIFDToImgLocation(ifdIdx - 1, z, c, t, l)) {
        break;
      }
      if ((region.zInRegion(z)) && (region.cInRegion(c) || c == -1) && (region.tInRegion(t)) &&
          (scene == static_cast<size_t>(l))) {
        ZImg thumb = tiff.readThumbnailFromIFD(ifd);
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
    throw ZException(fmt::format("Wrong dimension order: {}", order));
  }
}

bool ZImgTiff::mapIFDToImgLocation(size_t ifdIdx, index_t& z, index_t& c, index_t& t, index_t& l)
{
  return IFDToLoc(ifdIdx, z, c, t, l, m_startIFDIndex, m_dimensionOrder, m_imgInfo[0], m_imgInfo.size());
}

bool ZImgTiff::isDimensionOrderValid(const QString& order)
{
  return (order.size() == 3 && order.contains(QChar('Z')) && order.contains(QChar('T')) &&
          order.contains(QChar('L'))) ||
         (order.size() == 4 && order.contains(QChar('Z')) && order.contains(QChar('C')) && order.contains(QChar('T')) &&
          order.contains(QChar('L')));
}

bool ZImgTiff::IFDToLoc(size_t ifdIdx,
                        index_t& z,
                        index_t& c,
                        index_t& t,
                        index_t& l,
                        size_t startIFDIndex,
                        const QString& dimensionOrder,
                        const ZImgInfo& imgInfo,
                        size_t numScenes,
                        index_t startZ,
                        index_t startC,
                        index_t startT,
                        index_t startL)
{
  size_t ndims = dimensionOrder.size();
  std::vector<size_t> dimSizes;
  std::vector<index_t> dimStarts;
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
  for (size_t i = 1; i < ndims; ++i) {
    dimStrides[i] = dimStrides[i - 1] * dimSizes[i - 1];
  }

  size_t maxIFDNum = dimStrides[ndims - 1] * dimSizes[ndims - 1];
  if (ifdIdx < startIFDIndex || ifdIdx >= startIFDIndex + maxIFDNum) {
    return false;
  }
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
    if (dimensionOrder.at(i) == QChar('Z')) {
      z = locs[i];
    } else if (dimensionOrder.at(i) == QChar('C')) {
      c = locs[i];
    } else if (dimensionOrder.at(i) == QChar('T')) {
      t = locs[i];
    } else if (dimensionOrder.at(i) == QChar('L')) {
      l = locs[i];
    }
  }

  if (ndims == 3) {
    c = -1;
  }

  //  VLOG(1) << ifdIdx << " " << z << " " << c << " " << t << " " << l << " " << startIFDIndex << " " <<
  //  dimensionOrder
  //            << " " << imgInfo << " " << startZ << " " << startC << " " << startT << " " << startL;
  return true;
}

} // namespace nim
