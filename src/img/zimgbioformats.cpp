#include "zimgbioformats.h"

#include "zbioformatsbridgeclient.h"
#include "zexception.h"
#include "zimgmetatag.h"
#include "zlog.h"

#include <QFileInfo>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <set>

namespace nim {

namespace {

size_t checkedSize(std::string_view field, uint64_t value)
{
  if (value > std::numeric_limits<size_t>::max()) {
    throw ZException(fmt::format("Bio-Formats {} value is too large for this Atlas build: {}", field, value));
  }
  return static_cast<size_t>(value);
}

size_t checkedMul(std::string_view field, size_t lhs, size_t rhs)
{
  if (rhs != 0 && lhs > std::numeric_limits<size_t>::max() / rhs) {
    throw ZException(fmt::format("Bio-Formats {} value overflows Atlas size_t", field));
  }
  return lhs * rhs;
}

index_t checkedIndex(std::string_view field, size_t value)
{
  if (value > static_cast<size_t>(std::numeric_limits<index_t>::max())) {
    throw ZException(fmt::format("Bio-Formats {} value is too large for Atlas index_t: {}", field, value));
  }
  return static_cast<index_t>(value);
}

size_t ceilDiv(size_t numerator, size_t denominator)
{
  CHECK(denominator > 0);
  return (numerator + denominator - 1) / denominator;
}

size_t floorDiv(size_t numerator, size_t denominator)
{
  CHECK(denominator > 0);
  return numerator / denominator;
}

bool resolutionDimensionMatchesRatio(size_t fullSize, size_t resolutionSize, size_t ratio)
{
  CHECK(ratio > 0);
  // BDV and some Bio-Formats readers report pyramid dimensions rounded down for odd full sizes.
  return ceilDiv(fullSize, ratio) == resolutionSize || floorDiv(fullSize, ratio) == resolutionSize;
}

size_t scaledResolutionCoordToBase(size_t coord, size_t resolutionSize, size_t fullSize, size_t ratio)
{
  CHECK(ratio > 0);
  CHECK(coord <= resolutionSize);
  return std::min(fullSize, checkedMul("scaled pyramid coordinate", coord, ratio));
}

void setVoxelFormatFromBioFormats(const ZBioFormatsSeriesInfo& series, ZImgInfo& info)
{
  const QString pixelType = series.pixelType.toLower();
  const size_t bytesPerPixel = series.bytesPerPixel;
  if (bytesPerPixel == 0) {
    throw ZException(fmt::format("Bio-Formats series {} reports zero bytes per pixel", series.series));
  }

  if (pixelType == "bit") {
    info.setVoxelFormat<uint8_t>(1);
  } else if (pixelType == "uint8") {
    info.setVoxelFormat<uint8_t>();
  } else if (pixelType == "int8") {
    info.setVoxelFormat<int8_t>();
  } else if (pixelType == "uint16") {
    info.setVoxelFormat<uint16_t>();
  } else if (pixelType == "int16") {
    info.setVoxelFormat<int16_t>();
  } else if (pixelType == "uint32") {
    info.setVoxelFormat<uint32_t>();
  } else if (pixelType == "int32") {
    info.setVoxelFormat<int32_t>();
  } else if (pixelType == "float") {
    info.setVoxelFormat<float>();
  } else if (pixelType == "double") {
    info.setVoxelFormat<double>();
  } else {
    throw ZException(fmt::format("Bio-Formats pixel type '{}' is not supported by Atlas", series.pixelType));
  }

  if (info.bytesPerVoxel != bytesPerPixel) {
    throw ZException(fmt::format("Bio-Formats pixel type '{}' reports {} bytes, Atlas mapped it to {} bytes",
                                 series.pixelType,
                                 bytesPerPixel,
                                 info.bytesPerVoxel));
  }
}

ZImgInfo toImgInfo(const ZBioFormatsSeriesInfo& series)
{
  ZImgInfo info;
  info.width = checkedSize("size_x", series.sizeX);
  info.height = checkedSize("size_y", series.sizeY);
  info.depth = checkedSize("size_z", series.sizeZ);
  info.numChannels = checkedMul("channel count",
                                checkedSize("effective_size_c", series.effectiveSizeC),
                                std::max<size_t>(1, series.rgbChannelCount));
  info.numTimes = checkedSize("size_t", series.sizeT);
  setVoxelFormatFromBioFormats(series, info);

  if (series.hasPhysicalSizeX || series.hasPhysicalSizeY || series.hasPhysicalSizeZ) {
    info.voxelSizeUnit = VoxelSizeUnit::um;
    info.voxelSizeX = series.hasPhysicalSizeX ? series.physicalSizeXUm : 1.;
    info.voxelSizeY = series.hasPhysicalSizeY ? series.physicalSizeYUm : 1.;
    info.voxelSizeZ = series.hasPhysicalSizeZ ? series.physicalSizeZUm : 1.;
  }

  const size_t rgbChannelCount = std::max<size_t>(1, series.rgbChannelCount);
  if (!series.channelNames.empty()) {
    info.channelNames.clear();
    info.channelNames.reserve(info.numChannels);
    for (size_t c = 0; c < checkedSize("effective_size_c", series.effectiveSizeC); ++c) {
      const QString baseName =
        c < static_cast<size_t>(series.channelNames.size()) ? series.channelNames[int(c)] : QString();
      for (size_t rgb = 0; rgb < rgbChannelCount; ++rgb) {
        if (rgbChannelCount == 1 || baseName.isEmpty()) {
          info.channelNames.push_back(baseName.toStdString());
        } else {
          static constexpr std::array<std::string_view, 4> suffixes{
            {"R", "G", "B", "A"}
          };
          const std::string suffix = rgb < suffixes.size() ? std::string(suffixes[rgb]) : fmt::format("{}", rgb + 1);
          info.channelNames.push_back(fmt::format("{} {}", baseName.toStdString(), suffix));
        }
      }
    }
  }

  if (series.channelColorsRgba.size() == info.numChannels) {
    info.channelColors.clear();
    info.channelColors.reserve(info.numChannels);
    for (uint32_t rgba : series.channelColorsRgba) {
      col4 color;
      color.r = static_cast<col4::value_type>((rgba >> 24_u32) & 0xff_u32);
      color.g = static_cast<col4::value_type>((rgba >> 16_u32) & 0xff_u32);
      color.b = static_cast<col4::value_type>((rgba >> 8_u32) & 0xff_u32);
      color.a = static_cast<col4::value_type>(rgba & 0xff_u32);
      info.channelColors.push_back(color);
    }
  }

  info.createDefaultDescriptions();
  if (info.isEmpty()) {
    throw ZException(fmt::format("Bio-Formats series {} is empty", series.series));
  }
  return info;
}

QStringList uniqueSortedExtensions(const std::vector<ZBioFormatsReaderFormat>& formats)
{
  QStringList extensions;
  for (const auto& format : formats) {
    for (const QString& suffix : format.suffixes) {
      const QString trimmed = suffix.trimmed();
      if (trimmed.isEmpty()) {
        continue;
      }
      bool exists = false;
      for (const auto& existing : extensions) {
        if (existing.compare(trimmed, Qt::CaseInsensitive) == 0) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        extensions.push_back(trimmed);
      }
    }
  }
  std::sort(extensions.begin(), extensions.end(), [](const QString& lhs, const QString& rhs) {
    return lhs.compare(rhs, Qt::CaseInsensitive) < 0;
  });
  return extensions;
}

size_t tileSizeOrDefault(uint32_t value)
{
  return value == 0 ? 512 : static_cast<size_t>(value);
}

std::optional<size_t> integerRatioForResolution(size_t fullSize, size_t resolutionSize)
{
  if (fullSize == 0 || resolutionSize == 0) {
    return std::nullopt;
  }
  if (resolutionSize >= fullSize) {
    return size_t(1);
  }

  const double exactRatio = static_cast<double>(fullSize) / static_cast<double>(resolutionSize);
  std::set<size_t> candidates;
  candidates.insert(std::max<size_t>(1, static_cast<size_t>(std::floor(exactRatio))));
  candidates.insert(std::max<size_t>(1, static_cast<size_t>(std::ceil(exactRatio))));
  if (fullSize % resolutionSize == 0) {
    candidates.insert(fullSize / resolutionSize);
  }

  for (size_t ratio : candidates) {
    if (ratio > 0 && resolutionDimensionMatchesRatio(fullSize, resolutionSize, ratio)) {
      return ratio;
    }
  }
  return std::nullopt;
}

ZImgInfo toResolutionImgInfo(const ZBioFormatsSeriesInfo& series, const ZBioFormatsResolutionInfo& resolution)
{
  ZBioFormatsSeriesInfo adjusted = series;
  adjusted.sizeX = resolution.sizeX;
  adjusted.sizeY = resolution.sizeY;
  adjusted.sizeZ = resolution.sizeZ;
  adjusted.effectiveSizeC = resolution.effectiveSizeC;
  adjusted.sizeT = resolution.sizeT;
  return toImgInfo(adjusted);
}

class ZImgBioFormatsPyramidSubBlock final : public ZImgSubBlock
{
public:
  ZImgBioFormatsPyramidSubBlock(QString filename,
                                size_t scene,
                                uint32_t resolution,
                                ZImgRegion baseRegion,
                                ZImgRegion resolutionRegion,
                                ZImgInfo resolutionInfo,
                                size_t xRatio,
                                size_t yRatio,
                                size_t zRatio)
    : ZImgSubBlock(baseRegion.start.t,
                   baseRegion.start.x,
                   baseRegion.start.y,
                   baseRegion.start.z,
                   checkedSize("pyramid tile width", baseRegion.end.x - baseRegion.start.x),
                   checkedSize("pyramid tile height", baseRegion.end.y - baseRegion.start.y),
                   checkedSize("pyramid tile depth", baseRegion.end.z - baseRegion.start.z),
                   xRatio,
                   yRatio,
                   zRatio)
    , m_filename(std::move(filename))
    , m_scene(scene)
    , m_resolution(resolution)
    , m_resolutionRegion(std::move(resolutionRegion))
    , m_resolutionInfo(std::move(resolutionInfo))
  {
    CHECK(m_resolution > 0);
    CHECK(m_resolutionRegion.isValid(m_resolutionInfo));
    CHECK(xRatio > 0);
    CHECK(yRatio > 0);
    CHECK(zRatio > 0);
  }

  [[nodiscard]] std::shared_ptr<ZImg> read() const override
  {
    ZImgInfo info = readInfo();
    auto img = std::make_shared<ZImg>(info);
    std::vector<uint8_t> pixels =
      ZBioFormatsBridgeClient::instance().readRegion(m_filename, m_scene, m_resolution, m_resolutionRegion);
    if (pixels.size() != img->byteNumber()) {
      throw ZException(fmt::format("Bio-Formats bridge returned {} pyramid tile bytes, expected {} for image '{}'",
                                   pixels.size(),
                                   img->byteNumber(),
                                   info));
    }

    size_t offset = 0;
    for (size_t t = 0; t < img->numTimes(); ++t) {
      std::copy_n(pixels.data() + offset, img->timeByteNumber(), img->timeData<uint8_t>(t));
      offset += img->timeByteNumber();
    }
    return img;
  }

  [[nodiscard]] ZImgInfo readInfo() const override
  {
    return m_resolutionRegion.clip(m_resolutionInfo);
  }

private:
  QString m_filename;
  size_t m_scene = 0;
  uint32_t m_resolution = 0;
  ZImgRegion m_resolutionRegion;
  ZImgInfo m_resolutionInfo;
};

void addBioFormatsTiledSubBlocks(const QString& filename,
                                 const ZImgInfo& info,
                                 size_t scene,
                                 uint32_t resolution,
                                 const ZImgInfo& resolutionInfo,
                                 size_t xRatio,
                                 size_t yRatio,
                                 size_t zRatio,
                                 size_t tileWidth,
                                 size_t tileHeight,
                                 std::vector<std::shared_ptr<ZImgSubBlock>>& subBlocks)
{
  const size_t tw = std::max<size_t>(1, tileWidth);
  const size_t th = std::max<size_t>(1, tileHeight);
  for (size_t t = 0; t < resolutionInfo.numTimes; ++t) {
    for (size_t z = 0; z < resolutionInfo.depth; ++z) {
      for (size_t y = 0; y < resolutionInfo.height; y += th) {
        const size_t yEnd = std::min(resolutionInfo.height, y + th);
        for (size_t x = 0; x < resolutionInfo.width; x += tw) {
          const size_t xEnd = std::min(resolutionInfo.width, x + tw);
          const size_t baseX = scaledResolutionCoordToBase(x, resolutionInfo.width, info.width, xRatio);
          const size_t baseY = scaledResolutionCoordToBase(y, resolutionInfo.height, info.height, yRatio);
          const size_t baseXEnd = scaledResolutionCoordToBase(xEnd, resolutionInfo.width, info.width, xRatio);
          const size_t baseYEnd = scaledResolutionCoordToBase(yEnd, resolutionInfo.height, info.height, yRatio);
          const size_t baseZ = scaledResolutionCoordToBase(z, resolutionInfo.depth, info.depth, zRatio);
          const size_t baseZEnd = scaledResolutionCoordToBase(z + 1, resolutionInfo.depth, info.depth, zRatio);
          ZImgRegion baseRegion(ZVoxelCoordinate(checkedIndex("pyramid base x", baseX),
                                                 checkedIndex("pyramid base y", baseY),
                                                 checkedIndex("pyramid base z", baseZ),
                                                 0,
                                                 checkedIndex("pyramid base t", t)),
                                ZVoxelCoordinate(checkedIndex("pyramid base x end", baseXEnd),
                                                 checkedIndex("pyramid base y end", baseYEnd),
                                                 checkedIndex("pyramid base z end", baseZEnd),
                                                 checkedIndex("pyramid base channel end", info.numChannels),
                                                 checkedIndex("pyramid base t end", t + 1)));
          ZImgRegion resolutionRegion(
            ZVoxelCoordinate(checkedIndex("pyramid resolution x", x),
                             checkedIndex("pyramid resolution y", y),
                             checkedIndex("pyramid resolution z", z),
                             0,
                             checkedIndex("pyramid resolution t", t)),
            ZVoxelCoordinate(checkedIndex("pyramid resolution x end", xEnd),
                             checkedIndex("pyramid resolution y end", yEnd),
                             checkedIndex("pyramid resolution z end", z + 1),
                             checkedIndex("pyramid resolution channel end", resolutionInfo.numChannels),
                             checkedIndex("pyramid resolution t end", t + 1)));
          if (resolution == 0) {
            subBlocks.emplace_back(
              std::make_shared<ZImgTileSubBlock>(ZImgSource(filename, baseRegion, scene, FileFormat::BioFormats)));
          } else {
            subBlocks.emplace_back(std::make_shared<ZImgBioFormatsPyramidSubBlock>(filename,
                                                                                   scene,
                                                                                   resolution,
                                                                                   std::move(baseRegion),
                                                                                   std::move(resolutionRegion),
                                                                                   resolutionInfo,
                                                                                   xRatio,
                                                                                   yRatio,
                                                                                   zRatio));
          }
        }
      }
    }
  }
}

void createBioFormatsSubBlocks(const QString& filename,
                               const ZBioFormatsDatasetInfo& dataset,
                               const std::vector<ZImgInfo>& infos,
                               std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  if (!subBlocks) {
    return;
  }
  subBlocks->clear();
  subBlocks->resize(infos.size());
  for (size_t s = 0; s < infos.size(); ++s) {
    const ZBioFormatsSeriesInfo& series = dataset.series[s];
    const ZImgInfo& info = infos[s];
    addBioFormatsTiledSubBlocks(filename,
                                info,
                                s,
                                0,
                                info,
                                1,
                                1,
                                1,
                                tileSizeOrDefault(series.optimalTileWidth),
                                tileSizeOrDefault(series.optimalTileHeight),
                                (*subBlocks)[s]);

    for (const ZBioFormatsResolutionInfo& resolution : series.resolutions) {
      if (resolution.resolution == 0) {
        continue;
      }
      const auto xRatio = integerRatioForResolution(info.width, checkedSize("resolution size_x", resolution.sizeX));
      const auto yRatio = integerRatioForResolution(info.height, checkedSize("resolution size_y", resolution.sizeY));
      const auto zRatio = integerRatioForResolution(info.depth, checkedSize("resolution size_z", resolution.sizeZ));
      if (!xRatio || !yRatio || !zRatio || (*xRatio == 1 && *yRatio == 1 && *zRatio == 1)) {
        VLOG(1) << fmt::format("Skipping Bio-Formats resolution {} for series {} because Atlas requires integer "
                               "downsample ratios (full {}x{}x{}, resolution {}x{}x{})",
                               resolution.resolution,
                               s,
                               info.width,
                               info.height,
                               info.depth,
                               resolution.sizeX,
                               resolution.sizeY,
                               resolution.sizeZ);
        continue;
      }

      const ZImgInfo resolutionInfo = toResolutionImgInfo(series, resolution);
      if (resolutionInfo.numChannels != info.numChannels || resolutionInfo.numTimes != info.numTimes) {
        VLOG(1) << fmt::format("Skipping Bio-Formats resolution {} for series {} because Atlas pyramid subblocks "
                               "require matching C/T dimensions (full {}x{}x{}, C={}, T={}; resolution "
                               "{}x{}x{}, C={}, T={})",
                               resolution.resolution,
                               s,
                               info.width,
                               info.height,
                               info.depth,
                               info.numChannels,
                               info.numTimes,
                               resolutionInfo.width,
                               resolutionInfo.height,
                               resolutionInfo.depth,
                               resolutionInfo.numChannels,
                               resolutionInfo.numTimes);
        continue;
      }
      addBioFormatsTiledSubBlocks(filename,
                                  info,
                                  s,
                                  resolution.resolution,
                                  resolutionInfo,
                                  *xRatio,
                                  *yRatio,
                                  *zRatio,
                                  tileSizeOrDefault(resolution.optimalTileWidth),
                                  tileSizeOrDefault(resolution.optimalTileHeight),
                                  (*subBlocks)[s]);
    }
  }
}

} // namespace

namespace bioformats_detail {

void createSubBlocksForTesting(const QString& filename,
                               const ZBioFormatsDatasetInfo& dataset,
                               const std::vector<ZImgInfo>& infos,
                               std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  createBioFormatsSubBlocks(filename, dataset, infos, subBlocks);
}

} // namespace bioformats_detail

bool ZImgBioFormats::supportRead() const
{
  return ZBioFormatsBridgeClient::hasRuntimeSupport();
}

bool ZImgBioFormats::supportWrite() const
{
  return false;
}

bool ZImgBioFormats::canRead(const QString& filename) const
{
  if (!supportRead()) {
    return false;
  }
  try {
    if (ZImgFormat::canRead(filename)) {
      return true;
    }
    if (!QFileInfo(filename).exists()) {
      return false;
    }
    return ZBioFormatsBridgeClient::instance().canRead(filename);
  }
  catch (const ZException& e) {
    VLOG(1) << "Bio-Formats reader probe failed for " << filename << ": " << e.what();
    return false;
  }
}

QString ZImgBioFormats::shortName() const
{
  return "BioFormats";
}

QString ZImgBioFormats::fullName() const
{
  return "Bio-Formats";
}

QStringList ZImgBioFormats::extensions() const
{
  if (!supportRead()) {
    return {};
  }
  return uniqueSortedExtensions(ZBioFormatsBridgeClient::instance().listFormats());
}

void ZImgBioFormats::readInfo(const QString& filename,
                              std::vector<ZImgInfo>& infos,
                              std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  const auto& dataset = ZBioFormatsBridgeClient::instance().openDataset(filename);
  infos.clear();
  infos.reserve(dataset.series.size());
  for (const auto& series : dataset.series) {
    infos.push_back(toImgInfo(series));
  }
  if (infos.empty()) {
    throw ZException(fmt::format("Bio-Formats found no readable series in {}", filename));
  }

  createBioFormatsSubBlocks(filename, dataset, infos, subBlocks);
}

void ZImgBioFormats::readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene)
{
  const auto& dataset = ZBioFormatsBridgeClient::instance().openDataset(filename);
  if (scene >= dataset.series.size()) {
    throw ZException(fmt::format("Bio-Formats scene {} is out of range for {}", scene, filename));
  }
  const auto& series = dataset.series[scene];
  meta.attachToTopLevel(ZImgMetatag("Bio-Formats Format", dataset.formatName.toStdString()));
  meta.attachToTopLevel(ZImgMetatag("Bio-Formats Reader", dataset.readerClass.toStdString()));
  for (const QString& usedFile : series.usedFiles) {
    meta.attachToTopLevel(ZImgMetatag("Bio-Formats Used File", usedFile.toStdString()));
  }
  for (const auto& entry : series.metadata) {
    meta.attachToTopLevel(ZImgMetatag(entry.key, entry.value));
  }
  for (const auto& resolution : series.resolutions) {
    meta.attachToTopLevel(ZImgMetatag("Bio-Formats Resolution",
                                      fmt::format("{}: {}x{}x{}, C={}, T={}, tile={}x{}",
                                                  resolution.resolution,
                                                  resolution.sizeX,
                                                  resolution.sizeY,
                                                  resolution.sizeZ,
                                                  resolution.effectiveSizeC,
                                                  resolution.sizeT,
                                                  resolution.optimalTileWidth,
                                                  resolution.optimalTileHeight)));
  }
}

void ZImgBioFormats::readThumbnail(const QString& filename,
                                   ZImgThumbernail& thumbnail,
                                   const ZImgRegion& region,
                                   size_t scene)
{
  thumbnail.clear();
  const auto& dataset = ZBioFormatsBridgeClient::instance().openDataset(filename);
  if (scene >= dataset.series.size()) {
    throw ZException(fmt::format("Bio-Formats scene {} is out of range for {}", scene, filename));
  }

  const ZImgInfo imgInfo = toImgInfo(dataset.series[scene]);
  if (region.isEmpty() || !region.isValid(imgInfo)) {
    throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", imgInfo, region));
  }

  ZImgRegion resolvedRegion = region;
  resolvedRegion.resolveRegionEnd(imgInfo);
  for (index_t t = resolvedRegion.start.t; t < resolvedRegion.end.t; ++t) {
    for (index_t z = resolvedRegion.start.z; z < resolvedRegion.end.z; ++z) {
      try {
        ZBioFormatsThumbnail thumbnailPixels =
          ZBioFormatsBridgeClient::instance().readThumbnail(filename,
                                                            scene,
                                                            static_cast<size_t>(z),
                                                            static_cast<size_t>(t));
        ZBioFormatsSeriesInfo thumbnailSeries = dataset.series[scene];
        thumbnailSeries.sizeX = thumbnailPixels.width;
        thumbnailSeries.sizeY = thumbnailPixels.height;
        thumbnailSeries.sizeZ = 1;
        thumbnailSeries.effectiveSizeC = thumbnailPixels.channelCount;
        thumbnailSeries.rgbChannelCount = 1;
        thumbnailSeries.sizeT = 1;
        thumbnailSeries.bytesPerPixel = thumbnailPixels.bytesPerPixel;
        thumbnailSeries.pixelType = thumbnailPixels.pixelType;
        ZImgInfo thumbnailInfo = toImgInfo(thumbnailSeries);
        ZImg thumbImg(thumbnailInfo);
        if (thumbnailPixels.pixels.size() != thumbImg.byteNumber()) {
          throw ZException(fmt::format("Bio-Formats bridge returned {} thumbnail bytes, expected {} for image '{}'",
                                       thumbnailPixels.pixels.size(),
                                       thumbImg.byteNumber(),
                                       thumbnailInfo));
        }
        std::copy_n(thumbnailPixels.pixels.data(), thumbImg.byteNumber(), thumbImg.timeData<uint8_t>(0));
        thumbnail.attachToPlane(thumbImg,
                                static_cast<size_t>(z - resolvedRegion.start.z),
                                static_cast<size_t>(t - resolvedRegion.start.t));
      }
      catch (const ZException& e) {
        VLOG(1) << "Bio-Formats thumbnail read failed for " << filename << ": " << e.what();
      }
    }
  }
}

void ZImgBioFormats::readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene)
{
  const auto& dataset = ZBioFormatsBridgeClient::instance().openDataset(filename);
  if (scene >= dataset.series.size()) {
    throw ZException(fmt::format("Bio-Formats scene {} is out of range for {}", scene, filename));
  }

  const ZImgInfo imgInfo = toImgInfo(dataset.series[scene]);
  if (region.isEmpty() || !region.isValid(imgInfo)) {
    throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", imgInfo, region));
  }

  ZImgRegion resolvedRegion = region;
  resolvedRegion.resolveRegionEnd(imgInfo);
  const ZImgInfo clipInfo = resolvedRegion.clip(imgInfo);
  img = ZImg(clipInfo);

  std::vector<uint8_t> pixels = ZBioFormatsBridgeClient::instance().readRegion(filename, scene, resolvedRegion);
  if (pixels.size() != img.byteNumber()) {
    throw ZException(fmt::format("Bio-Formats bridge returned {} pixel bytes, expected {} for image '{}'",
                                 pixels.size(),
                                 img.byteNumber(),
                                 clipInfo));
  }

  size_t offset = 0;
  for (size_t t = 0; t < img.numTimes(); ++t) {
    std::copy_n(pixels.data() + offset, img.timeByteNumber(), img.timeData<uint8_t>(t));
    offset += img.timeByteNumber();
  }

  readMetadata(filename, img.metadataRef(), scene);
}

} // namespace nim
