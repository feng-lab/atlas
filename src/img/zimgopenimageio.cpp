#include "zimgopenimageio.h"
#include "zlog.h"

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imageio.h>
#include <gif_lib.h>

#include <QFile>
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
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

struct GifInputFile
{
  GifFileType* file = nullptr;

  GifInputFile() = default;
  GifInputFile(const GifInputFile&) = delete;
  GifInputFile& operator=(const GifInputFile&) = delete;

  GifInputFile(GifInputFile&& other) noexcept
    : file(other.file)
  {
    other.file = nullptr;
  }

  GifInputFile& operator=(GifInputFile&& other) noexcept
  {
    if (this != &other) {
      close();
      file = other.file;
      other.file = nullptr;
    }
    return *this;
  }

  ~GifInputFile()
  {
    close();
  }

  void close()
  {
    if (file) {
      int error = 0;
      DGifCloseFile(file, &error);
      file = nullptr;
    }
  }
};

struct GifFrameControl
{
  int disposal = DISPOSAL_UNSPECIFIED;
  int transparentColor = NO_TRANSPARENT_COLOR;
};

struct GifFrameRect
{
  int left = 0;
  int top = 0;
  int width = 0;
  int height = 0;
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

[[nodiscard]] std::string gifErrorString(int error)
{
  const char* errorText = GifErrorString(error);
  return errorText ? errorText : fmt::format("giflib error {}", error);
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

[[nodiscard]] index_t resolveRegionEnd(index_t regionEnd, size_t sourceSize)
{
  return regionEnd == -1 ? static_cast<index_t>(sourceSize) : regionEnd;
}

void validateBoundedGifRegion(const ZImgRegion& region, const ZImgInfo& sourceInfo, index_t tEnd)
{
  if (region.isEmpty() || region.start.x < 0 || region.start.y < 0 || region.start.z < 0 || region.start.c < 0 ||
      region.start.t < 0 || tEnd <= region.start.t || region.start.x >= sourceInfo.sWidth() ||
      region.start.y >= sourceInfo.sHeight() || region.start.z >= sourceInfo.sDepth() ||
      region.start.c >= sourceInfo.sNumChannels() ||
      resolveRegionEnd(region.end.x, sourceInfo.width) > sourceInfo.sWidth() ||
      resolveRegionEnd(region.end.y, sourceInfo.height) > sourceInfo.sHeight() ||
      resolveRegionEnd(region.end.z, sourceInfo.depth) > sourceInfo.sDepth() ||
      resolveRegionEnd(region.end.c, sourceInfo.numChannels) > sourceInfo.sNumChannels()) {
    throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", sourceInfo, region));
  }
}

[[nodiscard]] ZImgInfo clipBoundedGifRegion(const ZImgRegion& region, const ZImgInfo& sourceInfo, index_t tEnd)
{
  ZImgInfo result = sourceInfo;
  const index_t xEnd = resolveRegionEnd(region.end.x, sourceInfo.width);
  const index_t yEnd = resolveRegionEnd(region.end.y, sourceInfo.height);
  const index_t zEnd = resolveRegionEnd(region.end.z, sourceInfo.depth);
  const index_t cEnd = resolveRegionEnd(region.end.c, sourceInfo.numChannels);
  result.width = static_cast<size_t>(xEnd - region.start.x);
  result.height = static_cast<size_t>(yEnd - region.start.y);
  result.depth = static_cast<size_t>(zEnd - region.start.z);
  result.numChannels = static_cast<size_t>(cEnd - region.start.c);
  result.channelColors =
    std::vector(sourceInfo.channelColors.begin() + region.start.c, sourceInfo.channelColors.begin() + cEnd);
  result.channelNames =
    std::vector(sourceInfo.channelNames.begin() + region.start.c, sourceInfo.channelNames.begin() + cEnd);
  result.lastChannelIsAlphaChannel =
    sourceInfo.lastChannelIsAlphaChannel && cEnd == static_cast<index_t>(sourceInfo.numChannels);
  result.numTimes = static_cast<size_t>(tEnd - region.start.t);
  result.timeStamps.resize(result.numTimes);
  for (size_t t = 0; t < result.numTimes; ++t) {
    result.timeStamps[t] = static_cast<double>(region.start.t + static_cast<index_t>(t));
  }
  return result;
}

[[nodiscard]] int gifInterlacedRow(int row, int height)
{
  if (height > 1 && row >= (height + 1) / 2) {
    return 2 * (row - (height + 1) / 2) + 1;
  }
  if (height > 2 && row >= (height + 3) / 4) {
    return 4 * (row - (height + 3) / 4) + 2;
  }
  if (height > 4 && row >= (height + 7) / 8) {
    return 8 * (row - (height + 7) / 8) + 4;
  }
  return row * 8;
}

[[nodiscard]] std::array<uint8_t, 4> gifBackgroundColor(const GifFileType& gif)
{
  if (!gif.SColorMap || gif.SBackGroundColor < 0 || gif.SBackGroundColor >= gif.SColorMap->ColorCount) {
    return {0, 0, 0, 0};
  }
  const GifColorType& color = gif.SColorMap->Colors[gif.SBackGroundColor];
  return {color.Red, color.Green, color.Blue, 255};
}

void fillGifCanvasRect(std::vector<uint8_t>& canvas,
                       int canvasWidth,
                       int canvasHeight,
                       const GifFrameRect& rect,
                       const std::array<uint8_t, 4>& color)
{
  const int xBegin = std::max(rect.left, 0);
  const int yBegin = std::max(rect.top, 0);
  const int xEnd = std::min(rect.left + rect.width, canvasWidth);
  const int yEnd = std::min(rect.top + rect.height, canvasHeight);
  for (int y = yBegin; y < yEnd; ++y) {
    for (int x = xBegin; x < xEnd; ++x) {
      std::copy_n(color.data(), color.size(), canvas.data() + static_cast<size_t>((y * canvasWidth + x) * 4));
    }
  }
}

void applyGifDisposal(std::vector<uint8_t>& canvas,
                      const std::vector<uint8_t>& previousCanvas,
                      const GifFrameControl& previousControl,
                      const GifFrameRect& previousRect,
                      const std::array<uint8_t, 4>& backgroundColor,
                      int canvasWidth,
                      int canvasHeight)
{
  if (previousControl.disposal == DISPOSE_BACKGROUND) {
    fillGifCanvasRect(canvas, canvasWidth, canvasHeight, previousRect, backgroundColor);
  } else if (previousControl.disposal == DISPOSE_PREVIOUS && previousCanvas.size() == canvas.size()) {
    canvas = previousCanvas;
  }
}

void readGifControlExtension(GifFrameControl& control, const GifByteType* extension)
{
  if (!extension || extension[0] < 4) {
    return;
  }
  control.disposal = (extension[1] & 0x1c) >> 2;
  control.transparentColor = (extension[1] & 0x01) ? static_cast<int>(extension[4]) : NO_TRANSPARENT_COLOR;
}

void drawGifFrame(GifFileType& gif, std::vector<uint8_t>& canvas, const GifFrameControl& control)
{
  const ColorMapObject* colorMap = gif.Image.ColorMap ? gif.Image.ColorMap : gif.SColorMap;
  if (!colorMap) {
    throw ZException("GIF frame does not have a local or global color map");
  }

  const int canvasWidth = gif.SWidth;
  const int canvasHeight = gif.SHeight;
  const int frameWidth = gif.Image.Width;
  const int frameHeight = gif.Image.Height;
  std::vector<GifPixelType> row(static_cast<size_t>(frameWidth));
  for (int rowIndex = 0; rowIndex < frameHeight; ++rowIndex) {
    if (DGifGetLine(&gif, row.data(), frameWidth) == GIF_ERROR) {
      throw ZException(fmt::format("giflib failed to read GIF frame row: {}", gifErrorString(gif.Error)));
    }
    const int y = gif.Image.Top + (gif.Image.Interlace ? gifInterlacedRow(rowIndex, frameHeight) : rowIndex);
    if (y < 0 || y >= canvasHeight) {
      continue;
    }
    for (int xOffset = 0; xOffset < frameWidth; ++xOffset) {
      const int paletteIndex = row[static_cast<size_t>(xOffset)];
      if (paletteIndex == control.transparentColor) {
        continue;
      }
      if (paletteIndex < 0 || paletteIndex >= colorMap->ColorCount) {
        throw ZException(
          fmt::format("GIF frame palette index {} exceeds color map size {}", paletteIndex, colorMap->ColorCount));
      }
      const int x = gif.Image.Left + xOffset;
      if (x < 0 || x >= canvasWidth) {
        continue;
      }
      const GifColorType& color = colorMap->Colors[paletteIndex];
      auto* pixel = canvas.data() + static_cast<size_t>((y * canvasWidth + x) * 4);
      pixel[0] = color.Red;
      pixel[1] = color.Green;
      pixel[2] = color.Blue;
      pixel[3] = 255;
    }
  }
}

void copyGifCanvasRegionToImg(const std::vector<uint8_t>& canvas,
                              int canvasWidth,
                              const ZImgRegion& region,
                              const ZImgInfo& sourceInfo,
                              index_t sourceTime,
                              ZImg& img)
{
  const index_t xEnd = resolveRegionEnd(region.end.x, sourceInfo.width);
  const index_t yEnd = resolveRegionEnd(region.end.y, sourceInfo.height);
  const index_t cEnd = resolveRegionEnd(region.end.c, sourceInfo.numChannels);
  const size_t localTime = static_cast<size_t>(sourceTime - region.start.t);
  for (index_t c = region.start.c; c < cEnd; ++c) {
    for (index_t y = region.start.y; y < yEnd; ++y) {
      for (index_t x = region.start.x; x < xEnd; ++x) {
        const auto* pixel = canvas.data() + static_cast<size_t>((y * canvasWidth + x) * 4);
        *img.data<uint8_t>(static_cast<size_t>(x - region.start.x),
                           static_cast<size_t>(y - region.start.y),
                           0,
                           static_cast<size_t>(c - region.start.c),
                           localTime) = pixel[c];
      }
    }
  }
}

[[nodiscard]] GifInputFile openGifInputFile(const QString& filename)
{
  int error = 0;
  GifInputFile result;
  result.file = DGifOpenFileName(oiioFilename(filename).c_str(), &error);
  if (!result.file) {
    throw ZException(fmt::format("giflib failed to open '{}': {}", filename, gifErrorString(error)));
  }
  return result;
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

[[nodiscard]] ZImg readGifAnimationFromLayout(OIIO::ImageInput& input,
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

[[nodiscard]] ZImg readGifAnimationFromInput(OIIO::ImageInput& input,
                                             const QString& sourceName,
                                             const ZImgRegion& region,
                                             size_t scene,
                                             bool includeMetadata)
{
  if (scene != 0) {
    throw ZException("Invalid OpenImageIO scene for GIF animation: GIF frames are exposed as the time dimension");
  }
  if (region.end.t == -1) {
    const OiioInputLayout layout = readInputLayout(input);
    return readGifAnimationFromLayout(input, sourceName, layout, region, scene, includeMetadata);
  }

  OIIO::ImageSpec firstSpec = input.spec(static_cast<int>(region.start.t), 0);
  if (!firstSpec.format) {
    throw ZException(fmt::format("Invalid GIF frame {} for '{}'", region.start.t, sourceName));
  }
  const ZImgInfo firstFrameInfo = readInfoFromSpec(firstSpec);
  const index_t tEnd = region.end.t;
  validateBoundedGifRegion(region, firstFrameInfo, tEnd);

  ZImg img(clipBoundedGifRegion(region, firstFrameInfo, tEnd));
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
  for (index_t t = region.start.t; t < tEnd; ++t) {
    OIIO::ImageSpec spec = t == region.start.t ? firstSpec : input.spec(static_cast<int>(t), 0);
    if (!spec.format) {
      throw ZException(fmt::format("Invalid GIF frame {} for '{}'", t, sourceName));
    }
    ZImgInfo frameInfo = readInfoFromSpec(spec);
    if (!hasSameAtlasLayout(firstFrameInfo, frameInfo)) {
      throw ZException("OpenImageIO GIF frames have incompatible Atlas image layouts");
    }
    ZImg frame = readSubimageFromInput(input, sourceName, frameInfo, frameRegion, static_cast<size_t>(t));
    CHECK(frame.numTimes() == 1);
    CHECK(frame.byteNumber() == img.timeByteNumber());
    std::copy_n(frame.timeData<uint8_t>(0),
                frame.byteNumber(),
                img.timeData<uint8_t>(static_cast<size_t>(t - region.start.t)));
  }

  if (includeMetadata) {
    attachOiioMetadata(img, input);
  }
  return img;
}

[[nodiscard]] ZImg readGifAnimationFromFile(const QString& filename,
                                            const ZImgRegion& region,
                                            size_t scene,
                                            std::optional<size_t> knownFrameCount,
                                            bool includeMetadata)
{
  // OIIO reports GIF frames well, but giflib gives Atlas direct control over
  // stateless canvas composition for transparency and disposal methods.
  if (scene != 0) {
    throw ZException("Invalid OpenImageIO scene for GIF animation: GIF frames are exposed as the time dimension");
  }

  GifInputFile input = openGifInputFile(filename);
  CHECK(input.file);
  GifFileType& gif = *input.file;
  if (gif.SWidth <= 0 || gif.SHeight <= 0) {
    throw ZException(fmt::format("GIF reported invalid logical screen size {}x{}", gif.SWidth, gif.SHeight));
  }

  const index_t tEnd = region.end.t == -1 ? static_cast<index_t>(knownFrameCount.value_or(0)) : region.end.t;
  if (tEnd <= 0) {
    throw ZException("GIF frame count is unknown for an unbounded read");
  }
  ZImgInfo sourceInfo(static_cast<size_t>(gif.SWidth),
                      static_cast<size_t>(gif.SHeight),
                      1,
                      4,
                      static_cast<size_t>(tEnd),
                      1,
                      VoxelFormat::Unsigned);
  sourceInfo.lastChannelIsAlphaChannel = true;
  sourceInfo.createDefaultDescriptions();
  validateBoundedGifRegion(region, sourceInfo, tEnd);

  ZImg img(clipBoundedGifRegion(region, sourceInfo, tEnd));
  std::vector<uint8_t> canvas(static_cast<size_t>(gif.SWidth) * static_cast<size_t>(gif.SHeight) * 4, 0);
  const std::array<uint8_t, 4> backgroundColor = gifBackgroundColor(gif);
  GifFrameControl currentControl;
  GifFrameControl previousControl;
  GifFrameRect previousRect;
  std::vector<uint8_t> restoreCanvas;
  index_t frameIndex = 0;

  for (;;) {
    GifRecordType recordType = UNDEFINED_RECORD_TYPE;
    if (DGifGetRecordType(&gif, &recordType) == GIF_ERROR) {
      throw ZException(fmt::format("giflib failed to read GIF record: {}", gifErrorString(gif.Error)));
    }

    if (recordType == TERMINATE_RECORD_TYPE) {
      break;
    }

    if (recordType == EXTENSION_RECORD_TYPE) {
      int extensionCode = 0;
      GifByteType* extension = nullptr;
      if (DGifGetExtension(&gif, &extensionCode, &extension) == GIF_ERROR) {
        throw ZException(fmt::format("giflib failed to read GIF extension: {}", gifErrorString(gif.Error)));
      }
      if (extensionCode == GRAPHICS_EXT_FUNC_CODE) {
        readGifControlExtension(currentControl, extension);
      }
      while (extension) {
        if (DGifGetExtensionNext(&gif, &extension) == GIF_ERROR) {
          throw ZException(fmt::format("giflib failed to read GIF extension block: {}", gifErrorString(gif.Error)));
        }
      }
      continue;
    }

    if (recordType != IMAGE_DESC_RECORD_TYPE) {
      continue;
    }

    if (DGifGetImageDesc(&gif) == GIF_ERROR) {
      throw ZException(fmt::format("giflib failed to read GIF image descriptor: {}", gifErrorString(gif.Error)));
    }

    applyGifDisposal(canvas, restoreCanvas, previousControl, previousRect, backgroundColor, gif.SWidth, gif.SHeight);
    if (currentControl.disposal == DISPOSE_PREVIOUS) {
      restoreCanvas = canvas;
    } else {
      restoreCanvas.clear();
    }
    GifFrameRect currentRect{gif.Image.Left, gif.Image.Top, gif.Image.Width, gif.Image.Height};
    drawGifFrame(gif, canvas, currentControl);
    if (frameIndex >= region.start.t && frameIndex < tEnd) {
      copyGifCanvasRegionToImg(canvas, gif.SWidth, region, sourceInfo, frameIndex, img);
    }

    previousControl = currentControl;
    previousRect = currentRect;
    currentControl = GifFrameControl{};
    ++frameIndex;
    if (frameIndex >= tEnd) {
      break;
    }
  }

  if (frameIndex < tEnd) {
    throw ZException(
      fmt::format("Invalid GIF frame range [{}, {}) for {} decoded frames", region.start.t, tEnd, frameIndex));
  }

  if (includeMetadata) {
    ZImgMetadata metadata;
    metadata.attachToTopLevel(ZImgMetatag("OpenImageIO Format", "gif"));
    img.metadataRef().swap(metadata);
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
    return readGifAnimationFromLayout(input, sourceName, layout, region, scene, includeMetadata);
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
  if (isGifInput(input)) {
    if (sourceName != QStringLiteral("<memory>")) {
      if (region.end.t == -1) {
        const OiioInputLayout layout = readInputLayout(input);
        CHECK(!layout.sceneInfos.empty());
        return readGifAnimationFromFile(sourceName, region, scene, layout.sceneInfos.front().numTimes, includeMetadata);
      }
      return readGifAnimationFromFile(sourceName, region, scene, std::nullopt, includeMetadata);
    }
    return readGifAnimationFromInput(input, sourceName, region, scene, includeMetadata);
  }
  const OiioInputLayout layout = readInputLayout(input);
  return readImageFromLayout(input, sourceName, layout, region, scene, includeMetadata);
}

} // namespace

namespace nim {

bool ZImgOpenImageIO::supportRead() const
{
  return true;
}

void ZImgOpenImageIO::initializeRuntime()
{
  if (!OIIO::attribute("use_tbb", 1)) {
    LOG(WARNING) << "Failed to configure OpenImageIO to use TBB";
  }
}

void ZImgOpenImageIO::shutdownRuntime() noexcept
{
  try {
    OIIO::shutdown();
  }
  catch (const std::exception& e) {
    LOG(WARNING) << "OpenImageIO shutdown failed: " << e.what();
  }
  catch (...) {
    LOG(WARNING) << "OpenImageIO shutdown failed with unknown exception";
  }
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

ZImgInfo ZImgOpenImageIO::readMemInfo(std::span<const uint8_t> bytes)
{
  if (bytes.empty()) {
    throw ZException("Invalid OpenImageIO memory buffer: empty payload");
  }
  auto memoryInput = openOiioMemoryInput(bytes.data(), bytes.size());
  const OiioInputLayout layout = readInputLayout(*memoryInput.input);
  CHECK(!layout.sceneInfos.empty());
  return layout.sceneInfos.front();
}

void ZImgOpenImageIO::readMemImg(std::span<const uint8_t> bytes, std::span<uint8_t> des)
{
  if (bytes.empty()) {
    throw ZException("Invalid OpenImageIO memory buffer: empty payload");
  }
  auto layoutInput = openOiioMemoryInput(bytes.data(), bytes.size());
  const OiioInputLayout layout = readInputLayout(*layoutInput.input);
  CHECK(!layout.sceneInfos.empty());
  if (des.size() < layout.sceneInfos.front().byteNumber()) {
    throw ZException(fmt::format("Output buffer is too small for OpenImageIO memory image. Need {}, got {}",
                                 layout.sceneInfos.front().byteNumber(),
                                 des.size()));
  }
  auto decodeInput = openOiioMemoryInput(bytes.data(), bytes.size());
  ZImg img = readImageFromLayout(*decodeInput.input, QStringLiteral("<memory>"), layout, ZImgRegion(), 0, false);
  CHECK(img.byteNumber() == layout.sceneInfos.front().byteNumber());
  size_t offset = 0;
  for (size_t t = 0; t < img.numTimes(); ++t) {
    std::copy_n(img.timeData<uint8_t>(t), img.timeByteNumber(), des.data() + offset);
    offset += img.timeByteNumber();
  }
  CHECK(offset == img.byteNumber());
}

} // namespace nim
