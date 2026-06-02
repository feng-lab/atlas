#include "zimgopenimageio.h"

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imageio.h>

#include <QFile>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string_view>

namespace {

using namespace nim;

struct OiioOutputType
{
  VoxelFormat voxelFormat = VoxelFormat::Unsigned;
  size_t bytesPerVoxel = 1;
  OIIO::TypeDesc oiioType = OIIO::TypeUnknown;
};

struct OiioMemoryInput
{
  std::unique_ptr<OIIO::Filesystem::IOMemReader> memReader;
  OIIO::ImageInput::unique_ptr input;
};

struct OiioInputLayout
{
  std::vector<ZImgInfo> sceneInfos;
  std::vector<ZImgInfo> subimageInfos;
  bool gifFramesAsTime = false;
};

[[nodiscard]] std::string oiioError(const OIIO::ImageInput* input)
{
  std::string error = input ? input->geterror() : OIIO::geterror();
  if (error.empty()) {
    error = "unknown OpenImageIO error";
  }
  return error;
}

[[nodiscard]] std::string oiioFilename(const QString& filename)
{
#if defined(_WIN32) || defined(_WIN64)
  return filename.toStdString();
#else
  return QFile::encodeName(filename).toStdString();
#endif
}

[[nodiscard]] OIIO::ImageInput::unique_ptr openOiioInput(const QString& filename)
{
#if defined(_WIN32) || defined(_WIN64)
  auto input = OIIO::ImageInput::open(filename.toStdWString());
#else
  auto input = OIIO::ImageInput::open(oiioFilename(filename));
#endif
  if (!input) {
    throw ZException(fmt::format("OpenImageIO failed to open '{}': {}", filename, oiioError(nullptr)));
  }
  return input;
}

[[nodiscard]] bool hasPrefix(const uint8_t* mem, size_t size, const char* bytes, size_t byteCount)
{
  return size >= byteCount && std::memcmp(mem, bytes, byteCount) == 0;
}

[[nodiscard]] std::string memoryFilenameHint(const void* mem, size_t size)
{
  const auto* bytes = static_cast<const uint8_t*>(mem);
  if (hasPrefix(bytes, size, "\x89PNG\r\n\x1a\n", 8)) {
    return "memory.png";
  }
  if (hasPrefix(bytes, size, "\xff\xd8\xff", 3)) {
    return "memory.jpg";
  }
  if (hasPrefix(bytes, size, "II*\0", 4) || hasPrefix(bytes, size, "MM\0*", 4) || hasPrefix(bytes, size, "II+\0", 4) ||
      hasPrefix(bytes, size, "MM\0+", 4)) {
    return "memory.tif";
  }
  if (hasPrefix(bytes, size, "GIF87a", 6) || hasPrefix(bytes, size, "GIF89a", 6)) {
    return "memory.gif";
  }
  if (size >= 12 && hasPrefix(bytes, size, "RIFF", 4) && std::memcmp(bytes + 8, "WEBP", 4) == 0) {
    return "memory.webp";
  }
  if (hasPrefix(bytes, size, "\x76\x2f\x31\x01", 4)) {
    return "memory.exr";
  }
  if (hasPrefix(bytes, size, "\xff\x4f\xff\x51", 4)) {
    return "memory.j2k";
  }
  if (hasPrefix(bytes, size, "\0\0\0\x0cjP  \r\n\x87\n", 12)) {
    return "memory.jp2";
  }
  if (hasPrefix(bytes, size, "\xff\x0a", 2) || hasPrefix(bytes, size, "\0\0\0\x0cJXL \r\n\x87\n", 12)) {
    return "memory.jxl";
  }
  if (hasPrefix(bytes, size, "BM", 2)) {
    return "memory.bmp";
  }
  return "memory";
}

[[nodiscard]] OiioMemoryInput openOiioMemoryInput(const void* mem, size_t size)
{
  OiioMemoryInput result;
  result.memReader = std::make_unique<OIIO::Filesystem::IOMemReader>(mem, size);
  const std::string filenameHint = memoryFilenameHint(mem, size);
  result.input = OIIO::ImageInput::open(filenameHint, nullptr, result.memReader.get());
  if (!result.input) {
    throw ZException(fmt::format("OpenImageIO failed to open memory image: {}", oiioError(nullptr)));
  }
  return result;
}

[[nodiscard]] OIIO::TypeDesc scalarTypeForSpec(const OIIO::ImageSpec& spec)
{
  if (spec.channelformats.empty()) {
    return spec.format.scalartype();
  }
  if (spec.channelformats.size() != static_cast<size_t>(spec.nchannels)) {
    throw ZException(fmt::format("OpenImageIO reported {} per-channel formats for {} channels",
                                 spec.channelformats.size(),
                                 spec.nchannels));
  }

  OIIO::TypeDesc merged = spec.channelformats.front().scalartype();
  for (size_t i = 1; i < spec.channelformats.size(); ++i) {
    merged = OIIO::TypeDesc(OIIO::TypeDesc::basetype_merge(merged, spec.channelformats[i].scalartype()));
  }
  return merged.scalartype();
}

[[nodiscard]] OiioOutputType outputTypeForSpec(const OIIO::ImageSpec& spec)
{
  const OIIO::TypeDesc type = scalarTypeForSpec(spec);
  switch (static_cast<OIIO::TypeDesc::BASETYPE>(type.basetype)) {
    case OIIO::TypeDesc::UINT8:
      return {VoxelFormat::Unsigned, 1, OIIO::TypeUInt8};
    case OIIO::TypeDesc::INT8:
      return {VoxelFormat::Signed, 1, OIIO::TypeInt8};
    case OIIO::TypeDesc::UINT16:
      return {VoxelFormat::Unsigned, 2, OIIO::TypeUInt16};
    case OIIO::TypeDesc::INT16:
      return {VoxelFormat::Signed, 2, OIIO::TypeInt16};
    case OIIO::TypeDesc::UINT32:
      return {VoxelFormat::Unsigned, 4, OIIO::TypeUInt32};
    case OIIO::TypeDesc::INT32:
      return {VoxelFormat::Signed, 4, OIIO::TypeInt32};
    case OIIO::TypeDesc::UINT64:
      return {VoxelFormat::Unsigned, 8, OIIO::TypeUInt64};
    case OIIO::TypeDesc::INT64:
      return {VoxelFormat::Signed, 8, OIIO::TypeInt64};
    case OIIO::TypeDesc::HALF:
      return {VoxelFormat::Float, 4, OIIO::TypeFloat};
    case OIIO::TypeDesc::FLOAT:
      return {VoxelFormat::Float, 4, OIIO::TypeFloat};
    case OIIO::TypeDesc::DOUBLE:
      return {VoxelFormat::Float, 8, OIIO::TypeDesc(OIIO::TypeDesc::DOUBLE)};
    default:
      throw ZException(fmt::format("OpenImageIO pixel type '{}' is not supported by ZImg", type.c_str()));
  }
}

[[nodiscard]] ZImgInfo readInfoFromSpec(const OIIO::ImageSpec& spec)
{
  if (!spec.format) {
    throw ZException("OpenImageIO reported an unknown pixel type");
  }
  if (spec.deep) {
    throw ZException("OpenImageIO deep images are not supported by ZImg");
  }
  if (spec.width <= 0 || spec.height <= 0 || spec.depth <= 0 || spec.nchannels <= 0) {
    throw ZException(fmt::format("OpenImageIO reported invalid dimensions {}x{}x{} with {} channels",
                                 spec.width,
                                 spec.height,
                                 spec.depth,
                                 spec.nchannels));
  }

  const OiioOutputType outputType = outputTypeForSpec(spec);
  ZImgInfo info(static_cast<size_t>(spec.width),
                static_cast<size_t>(spec.height),
                static_cast<size_t>(spec.depth),
                static_cast<size_t>(spec.nchannels),
                1,
                outputType.bytesPerVoxel,
                outputType.voxelFormat);
  info.lastChannelIsAlphaChannel = spec.alpha_channel == spec.nchannels - 1 || spec.alpha_channel == spec.nchannels;
  info.createDefaultDescriptions();
  for (size_t c = 0; c < info.channelNames.size() && c < spec.channelnames.size(); ++c) {
    info.channelNames[c] = spec.channelnames[c];
  }
  return info;
}

[[nodiscard]] bool isGifInput(const OIIO::ImageInput& input)
{
  return std::string_view(input.format_name()) == "gif";
}

[[nodiscard]] bool hasSameAtlasLayout(const ZImgInfo& lhs, const ZImgInfo& rhs)
{
  return lhs.width == rhs.width && lhs.height == rhs.height && lhs.depth == rhs.depth &&
         lhs.numChannels == rhs.numChannels && lhs.bytesPerVoxel == rhs.bytesPerVoxel &&
         lhs.voxelFormat == rhs.voxelFormat && lhs.lastChannelIsAlphaChannel == rhs.lastChannelIsAlphaChannel;
}

[[nodiscard]] std::vector<ZImgInfo> readSubimageInfosFromInput(OIIO::ImageInput& input)
{
  std::vector<ZImgInfo> infos;
  for (int subimage = 0;; ++subimage) {
    OIIO::ImageSpec spec = input.spec(subimage, 0);
    if (!spec.format) {
      break;
    }
    infos.push_back(readInfoFromSpec(spec));
  }
  if (infos.empty()) {
    throw ZException(fmt::format("OpenImageIO did not report any readable subimages: {}", oiioError(&input)));
  }
  return infos;
}

[[nodiscard]] OiioInputLayout readInputLayout(OIIO::ImageInput& input)
{
  OiioInputLayout layout;
  layout.subimageInfos = readSubimageInfosFromInput(input);
  if (!isGifInput(input)) {
    layout.sceneInfos = layout.subimageInfos;
    return layout;
  }

  const ZImgInfo& firstFrameInfo = layout.subimageInfos.front();
  for (size_t t = 1; t < layout.subimageInfos.size(); ++t) {
    if (!hasSameAtlasLayout(firstFrameInfo, layout.subimageInfos[t])) {
      throw ZException("OpenImageIO GIF frames have incompatible Atlas image layouts");
    }
  }

  ZImgInfo animationInfo = firstFrameInfo;
  animationInfo.numTimes = layout.subimageInfos.size();
  animationInfo.createDefaultTimeStamps();
  layout.sceneInfos.push_back(std::move(animationInfo));
  layout.gifFramesAsTime = true;
  return layout;
}

[[nodiscard]] ZImg convertInterleavedToPlanar(ZImg& packed)
{
  if (packed.numChannels() == 1) {
    return std::move(packed);
  }
  ZImg planar(packed.info());
  ZImgFormat::CXYZtoXYZC(packed, planar);
  return planar;
}

[[nodiscard]] ZImgRegion cropRegionAfterTileRead(const ZImgRegion& region,
                                                 const ZImgInfo& sourceInfo,
                                                 size_t readChannelCount,
                                                 int xBegin,
                                                 int yBegin,
                                                 int zBegin,
                                                 const OIIO::ImageSpec& spec)
{
  const index_t zEnd = region.end.z == -1 ? sourceInfo.sDepth() : region.end.z;
  const index_t yEnd = region.end.y == -1 ? sourceInfo.sHeight() : region.end.y;
  const index_t xEnd = region.end.x == -1 ? sourceInfo.sWidth() : region.end.x;
  const index_t localXBegin = static_cast<index_t>(xBegin - spec.x);
  const index_t localYBegin = static_cast<index_t>(yBegin - spec.y);
  const index_t localZBegin = static_cast<index_t>(zBegin - spec.z);
  return ZImgRegion(region.start.x - localXBegin,
                    xEnd - localXBegin,
                    region.start.y - localYBegin,
                    yEnd - localYBegin,
                    region.start.z - localZBegin,
                    zEnd - localZBegin,
                    0,
                    static_cast<index_t>(readChannelCount),
                    0,
                    1);
}

[[nodiscard]] ZImgRegion
cropRegionAfterScanlineRead(const ZImgRegion& region, const ZImgInfo& sourceInfo, size_t readChannelCount)
{
  const index_t xEnd = region.end.x == -1 ? sourceInfo.sWidth() : region.end.x;
  return ZImgRegion(region.start.x,
                    xEnd,
                    0,
                    region.end.y == -1 ? sourceInfo.sHeight() - region.start.y : region.end.y - region.start.y,
                    0,
                    region.end.z == -1 ? sourceInfo.sDepth() - region.start.z : region.end.z - region.start.z,
                    0,
                    static_cast<index_t>(readChannelCount),
                    0,
                    1);
}

[[nodiscard]] ZImg readScanlineRegion(OIIO::ImageInput& input,
                                      const QString& sourceName,
                                      const OIIO::ImageSpec& spec,
                                      const ZImgInfo& sourceInfo,
                                      const ZImgRegion& region,
                                      size_t scene,
                                      int chbegin,
                                      int chend,
                                      OIIO::TypeDesc outputType)
{
  const int ybegin = spec.y + static_cast<int>(region.start.y);
  const int yend = spec.y + (region.end.y == -1 ? static_cast<int>(sourceInfo.height) : static_cast<int>(region.end.y));
  const int zbegin = spec.z + static_cast<int>(region.start.z);
  const int zend = spec.z + (region.end.z == -1 ? static_cast<int>(sourceInfo.depth) : static_cast<int>(region.end.z));
  CHECK(yend > ybegin);
  CHECK(zend > zbegin);
  CHECK(chend > chbegin);

  ZImgInfo readInfo = sourceInfo;
  readInfo.height = static_cast<size_t>(yend - ybegin);
  readInfo.depth = static_cast<size_t>(zend - zbegin);
  readInfo.numChannels = static_cast<size_t>(chend - chbegin);
  readInfo.lastChannelIsAlphaChannel =
    sourceInfo.lastChannelIsAlphaChannel && chend == static_cast<int>(sourceInfo.numChannels);
  readInfo.createDefaultDescriptions();
  ZImg packed(readInfo);

  const OIIO::stride_t xStride = static_cast<OIIO::stride_t>(readInfo.bytesPerVoxel * readInfo.numChannels);
  const OIIO::stride_t yStride = static_cast<OIIO::stride_t>(readInfo.width) * xStride;
  const size_t zByteStride = readInfo.height * static_cast<size_t>(yStride);
  auto* packedBytes = packed.timeData<uint8_t>(0);
  for (int z = zbegin; z < zend; ++z) {
    const size_t localZ = static_cast<size_t>(z - zbegin);
    if (!input.read_scanlines(static_cast<int>(scene),
                              0,
                              ybegin,
                              yend,
                              z,
                              chbegin,
                              chend,
                              outputType,
                              packedBytes + localZ * zByteStride,
                              xStride,
                              yStride)) {
      throw ZException(
        fmt::format("OpenImageIO failed to read scanlines from '{}': {}", sourceName, oiioError(&input)));
    }
  }

  ZImg planar = convertInterleavedToPlanar(packed);
  const ZImgRegion cropRegion = cropRegionAfterScanlineRead(region, sourceInfo, readInfo.numChannels);
  if (cropRegion.containsWholeImg(readInfo)) {
    return planar;
  }
  return planar.crop(cropRegion);
}

[[nodiscard]] int roundDownToTileBoundary(int value, int origin, int tileSize)
{
  CHECK(tileSize > 0);
  const int offset = value - origin;
  return origin + (offset / tileSize) * tileSize;
}

[[nodiscard]] int roundUpToTileBoundary(int value, int origin, int tileSize, int maxValue)
{
  CHECK(tileSize > 0);
  const int offset = value - origin;
  const int rounded = origin + ((offset + tileSize - 1) / tileSize) * tileSize;
  return std::min(rounded, maxValue);
}

[[nodiscard]] ZImg readTileRegion(OIIO::ImageInput& input,
                                  const QString& sourceName,
                                  const OIIO::ImageSpec& spec,
                                  const ZImgInfo& sourceInfo,
                                  const ZImgRegion& region,
                                  size_t scene,
                                  int chbegin,
                                  int chend,
                                  OIIO::TypeDesc outputType)
{
  const int tileWidth = std::max(spec.tile_width, 1);
  const int tileHeight = std::max(spec.tile_height, 1);
  const int tileDepth = std::max(spec.tile_depth, 1);
  const int imageXBegin = spec.x;
  const int imageYBegin = spec.y;
  const int imageZBegin = spec.z;
  const int imageXEnd = spec.x + spec.width;
  const int imageYEnd = spec.y + spec.height;
  const int imageZEnd = spec.z + spec.depth;
  const int requestedXBegin = spec.x + static_cast<int>(region.start.x);
  const int requestedYBegin = spec.y + static_cast<int>(region.start.y);
  const int requestedZBegin = spec.z + static_cast<int>(region.start.z);
  const int requestedXEnd =
    spec.x + (region.end.x == -1 ? static_cast<int>(sourceInfo.width) : static_cast<int>(region.end.x));
  const int requestedYEnd =
    spec.y + (region.end.y == -1 ? static_cast<int>(sourceInfo.height) : static_cast<int>(region.end.y));
  const int requestedZEnd =
    spec.z + (region.end.z == -1 ? static_cast<int>(sourceInfo.depth) : static_cast<int>(region.end.z));

  const int xbegin = std::max(imageXBegin, roundDownToTileBoundary(requestedXBegin, spec.x, tileWidth));
  const int ybegin = std::max(imageYBegin, roundDownToTileBoundary(requestedYBegin, spec.y, tileHeight));
  const int zbegin = std::max(imageZBegin, roundDownToTileBoundary(requestedZBegin, spec.z, tileDepth));
  const int xend = roundUpToTileBoundary(requestedXEnd, spec.x, tileWidth, imageXEnd);
  const int yend = roundUpToTileBoundary(requestedYEnd, spec.y, tileHeight, imageYEnd);
  const int zend = roundUpToTileBoundary(requestedZEnd, spec.z, tileDepth, imageZEnd);
  CHECK(xend > xbegin);
  CHECK(yend > ybegin);
  CHECK(zend > zbegin);
  CHECK(chend > chbegin);

  ZImgInfo readInfo = sourceInfo;
  readInfo.width = static_cast<size_t>(xend - xbegin);
  readInfo.height = static_cast<size_t>(yend - ybegin);
  readInfo.depth = static_cast<size_t>(zend - zbegin);
  readInfo.numChannels = static_cast<size_t>(chend - chbegin);
  readInfo.lastChannelIsAlphaChannel =
    sourceInfo.lastChannelIsAlphaChannel && chend == static_cast<int>(sourceInfo.numChannels);
  readInfo.createDefaultDescriptions();
  ZImg packed(readInfo);

  const OIIO::stride_t xStride = static_cast<OIIO::stride_t>(readInfo.bytesPerVoxel * readInfo.numChannels);
  const OIIO::stride_t yStride = static_cast<OIIO::stride_t>(readInfo.width) * xStride;
  const OIIO::stride_t zStride = static_cast<OIIO::stride_t>(readInfo.height) * yStride;
  if (!input.read_tiles(static_cast<int>(scene),
                        0,
                        xbegin,
                        xend,
                        ybegin,
                        yend,
                        zbegin,
                        zend,
                        chbegin,
                        chend,
                        outputType,
                        packed.timeData<uint8_t>(0),
                        xStride,
                        yStride,
                        zStride)) {
    throw ZException(fmt::format("OpenImageIO failed to read tiles from '{}': {}", sourceName, oiioError(&input)));
  }

  ZImg planar = convertInterleavedToPlanar(packed);
  const ZImgRegion cropRegion =
    cropRegionAfterTileRead(region, sourceInfo, readInfo.numChannels, xbegin, ybegin, zbegin, spec);
  if (cropRegion.containsWholeImg(readInfo)) {
    return planar;
  }
  return planar.crop(cropRegion);
}

[[nodiscard]] ZImg readSubimageFromInput(OIIO::ImageInput& input,
                                         const QString& sourceName,
                                         const ZImgInfo& sourceInfo,
                                         const ZImgRegion& region,
                                         size_t subimage)
{
  if (region.isEmpty() || !region.isValid(sourceInfo)) {
    throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", sourceInfo, region));
  }

  const OIIO::ImageSpec spec = input.spec(static_cast<int>(subimage), 0);
  const OiioOutputType outputType = outputTypeForSpec(spec);
  const int chbegin = static_cast<int>(region.start.c);
  const int chend = region.end.c == -1 ? static_cast<int>(sourceInfo.numChannels) : static_cast<int>(region.end.c);
  return spec.tile_width > 0
           ? readTileRegion(input, sourceName, spec, sourceInfo, region, subimage, chbegin, chend, outputType.oiioType)
           : readScanlineRegion(input,
                                sourceName,
                                spec,
                                sourceInfo,
                                region,
                                subimage,
                                chbegin,
                                chend,
                                outputType.oiioType);
}

void attachOiioMetadata(ZImg& img, const OIIO::ImageInput& input)
{
  ZImgMetadata metadata;
  metadata.attachToTopLevel(ZImgMetatag("OpenImageIO Format", input.format_name()));
  img.metadataRef().swap(metadata);
}

[[nodiscard]] ZImg readGifAnimationFromInput(OIIO::ImageInput& input,
                                             const QString& sourceName,
                                             const OiioInputLayout& layout,
                                             const ZImgRegion& region,
                                             size_t scene,
                                             bool includeMetadata)
{
  CHECK(layout.gifFramesAsTime);
  CHECK(layout.sceneInfos.size() == 1);
  if (scene != 0) {
    throw ZException("Invalid OpenImageIO scene for GIF animation: GIF frames are exposed as the time dimension");
  }

  const ZImgInfo& sourceInfo = layout.sceneInfos.front();
  if (region.isEmpty() || !region.isValid(sourceInfo)) {
    throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", sourceInfo, region));
  }

  const index_t tBegin = region.start.t;
  const index_t tEnd = region.end.t == -1 ? sourceInfo.sNumTimes() : region.end.t;
  ZImg img(region.clip(sourceInfo));
  const ZImgRegion frameRegion(region.start.x,
                               region.end.x,
                               region.start.y,
                               region.end.y,
                               region.start.z,
                               region.end.z,
                               region.start.c,
                               region.end.c,
                               0,
                               1);
  for (index_t t = tBegin; t < tEnd; ++t) {
    const size_t sourceFrame = static_cast<size_t>(t);
    CHECK(sourceFrame < layout.subimageInfos.size());
    ZImg frame = readSubimageFromInput(input, sourceName, layout.subimageInfos[sourceFrame], frameRegion, sourceFrame);
    CHECK(frame.numTimes() == 1);
    CHECK(frame.byteNumber() == img.timeByteNumber());
    std::copy_n(frame.timeData<uint8_t>(0), frame.byteNumber(), img.timeData<uint8_t>(static_cast<size_t>(t - tBegin)));
  }

  if (includeMetadata) {
    attachOiioMetadata(img, input);
  }
  return img;
}

[[nodiscard]] ZImg readImageFromLayout(OIIO::ImageInput& input,
                                       const QString& sourceName,
                                       const OiioInputLayout& layout,
                                       const ZImgRegion& region,
                                       size_t scene,
                                       bool includeMetadata)
{
  if (scene >= layout.sceneInfos.size()) {
    throw ZException(fmt::format("Invalid OpenImageIO scene {} for {} scenes", scene, layout.sceneInfos.size()));
  }

  if (layout.gifFramesAsTime) {
    return readGifAnimationFromInput(input, sourceName, layout, region, scene, includeMetadata);
  }

  ZImg img = readSubimageFromInput(input, sourceName, layout.sceneInfos[scene], region, scene);
  if (includeMetadata) {
    attachOiioMetadata(img, input);
  }
  return img;
}

[[nodiscard]] ZImg readImageFromInput(OIIO::ImageInput& input,
                                      const QString& sourceName,
                                      const ZImgRegion& region,
                                      size_t scene,
                                      bool includeMetadata)
{
  const OiioInputLayout layout = readInputLayout(input);
  return readImageFromLayout(input, sourceName, layout, region, scene, includeMetadata);
}

} // namespace

namespace nim {

bool ZImgOpenImageIO::supportRead() const
{
  return true;
}

bool ZImgOpenImageIO::supportWrite() const
{
  return false;
}

QString ZImgOpenImageIO::shortName() const
{
  return "OIIO";
}

QString ZImgOpenImageIO::fullName() const
{
  return "OpenImageIO";
}

QStringList ZImgOpenImageIO::extensions() const
{
  QStringList result;
  const auto extensionMap = OIIO::get_extension_map();
  for (const auto& [formatName, extensions] : extensionMap) {
    (void)formatName;
    for (const std::string& extension : extensions) {
      QString normalized = QString::fromStdString(extension).trimmed();
      if (normalized.startsWith('.')) {
        normalized.remove(0, 1);
      }
      if (normalized.isEmpty()) {
        continue;
      }
      const bool alreadyPresent = std::ranges::any_of(result, [&](const QString& existing) {
        return existing.compare(normalized, Qt::CaseInsensitive) == 0;
      });
      if (!alreadyPresent) {
        result.push_back(normalized);
      }
    }
  }
  return result;
}

void ZImgOpenImageIO::readInfo(const QString& filename,
                               std::vector<ZImgInfo>& infos,
                               std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  auto input = openOiioInput(filename);
  OiioInputLayout layout = readInputLayout(*input);
  infos = std::move(layout.sceneInfos);
  createDefaultSubBlocks(filename, infos, subBlocks, FileFormat::OpenImageIO);
}

void ZImgOpenImageIO::readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene)
{
  auto input = openOiioInput(filename);
  const OiioInputLayout layout = readInputLayout(*input);
  if (scene >= layout.sceneInfos.size()) {
    throw ZException(fmt::format("Invalid OpenImageIO scene {} for {} scenes", scene, layout.sceneInfos.size()));
  }
  meta.attachToTopLevel(ZImgMetatag("OpenImageIO Format", input->format_name()));
}

void ZImgOpenImageIO::readThumbnail(const QString& /*filename*/,
                                    ZImgThumbernail& /*thumbnail*/,
                                    const ZImgRegion& /*region*/,
                                    size_t /*scene*/)
{}

void ZImgOpenImageIO::readImg(const QString& filename,
                              ZImg& img,
                              const ZImgRegion& region,
                              size_t scene,
                              const ZImgReadOptions& readOptions)
{
  auto input = openOiioInput(filename);
  img = readImageFromInput(*input, filename, region, scene, readOptions.includeMetadata);
}

void ZImgOpenImageIO::readMemInfo(const uint8_t* mem, size_t size, ZImgInfo& info)
{
  CHECK(mem);
  auto memoryInput = openOiioMemoryInput(mem, size);
  const OiioInputLayout layout = readInputLayout(*memoryInput.input);
  CHECK(!layout.sceneInfos.empty());
  info = layout.sceneInfos.front();
}

void ZImgOpenImageIO::readMemImg(const uint8_t* mem, size_t size, uint8_t* des, size_t desSize)
{
  CHECK(mem);
  CHECK(des);
  auto layoutInput = openOiioMemoryInput(mem, size);
  const OiioInputLayout layout = readInputLayout(*layoutInput.input);
  CHECK(!layout.sceneInfos.empty());
  if (desSize < layout.sceneInfos.front().byteNumber()) {
    throw ZException(fmt::format("Output buffer is too small for OpenImageIO memory image. Need {}, got {}",
                                 layout.sceneInfos.front().byteNumber(),
                                 desSize));
  }
  auto decodeInput = openOiioMemoryInput(mem, size);
  ZImg img = readImageFromLayout(*decodeInput.input, QStringLiteral("<memory>"), layout, ZImgRegion(), 0, false);
  CHECK(img.byteNumber() == layout.sceneInfos.front().byteNumber());
  size_t offset = 0;
  for (size_t t = 0; t < img.numTimes(); ++t) {
    std::copy_n(img.timeData<uint8_t>(t), img.timeByteNumber(), des + offset);
    offset += img.timeByteNumber();
  }
  CHECK(offset == img.byteNumber());
}

} // namespace nim
