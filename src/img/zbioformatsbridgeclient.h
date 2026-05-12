#pragma once

#include "zimgregion.h"
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
#include <vector>

class QProcess;

namespace nim {

struct ZBioFormatsReaderFormat
{
  QString formatName;
  QString readerClass;
  QStringList suffixes;
  QStringList domains;
  bool hasCompanionFiles = false;
};

struct ZBioFormatsMetadataEntry
{
  std::string key;
  std::string value;
};

struct ZBioFormatsResolutionInfo
{
  uint32_t resolution = 0;
  uint64_t sizeX = 0;
  uint64_t sizeY = 0;
  uint64_t sizeZ = 0;
  uint64_t effectiveSizeC = 0;
  uint64_t sizeT = 0;
  uint64_t imageCount = 0;
  uint32_t optimalTileWidth = 512;
  uint32_t optimalTileHeight = 512;
};

struct ZBioFormatsSeriesInfo
{
  uint32_t series = 0;
  uint64_t sizeX = 0;
  uint64_t sizeY = 0;
  uint64_t sizeZ = 0;
  uint64_t effectiveSizeC = 0;
  uint64_t sizeT = 0;
  uint32_t rgbChannelCount = 1;
  uint32_t bytesPerPixel = 1;
  QString pixelType;
  bool littleEndian = true;
  QString dimensionOrder;
  uint32_t resolutionCount = 1;
  uint32_t optimalTileWidth = 512;
  uint32_t optimalTileHeight = 512;
  bool hasPhysicalSizeX = false;
  bool hasPhysicalSizeY = false;
  bool hasPhysicalSizeZ = false;
  double physicalSizeXUm = 1.;
  double physicalSizeYUm = 1.;
  double physicalSizeZUm = 1.;
  QStringList channelNames;
  std::vector<uint32_t> channelColorsRgba;
  QStringList usedFiles;
  std::vector<ZBioFormatsMetadataEntry> metadata;
  std::vector<ZBioFormatsResolutionInfo> resolutions;
};

struct ZBioFormatsDatasetInfo
{
  QString path;
  QString formatName;
  QString readerClass;
  QStringList usedFiles;
  std::vector<ZBioFormatsSeriesInfo> series;
};

struct ZBioFormatsThumbnail
{
  uint64_t width = 0;
  uint64_t height = 0;
  uint64_t channelCount = 0;
  uint32_t bytesPerPixel = 1;
  QString pixelType;
  std::vector<uint8_t> pixels;
};

class ZBioFormatsBridgeClient
{
public:
  static ZBioFormatsBridgeClient& instance();

  static bool hasRuntimeSupport();

  static QStringList missingRuntimeFiles();

  static void configureJavaExecutablePath(const QString& javaExecutablePath);
  static void configureBridgeJarPath(const QString& bridgeJarPath);
  static void configureBioFormatsJarPath(const QString& bioFormatsJarPath);
  static QString javaExecutablePath();
  static QString bridgeJarPath();
  static QString bioFormatsJarPath();

  // Tests reconfigure runtime paths in-process; reset before the next
  // instance() call so the singleton captures the new paths.
  static void resetInstanceForTesting();

  ZBioFormatsBridgeClient();

  ~ZBioFormatsBridgeClient();

  ZBioFormatsBridgeClient(const ZBioFormatsBridgeClient&) = delete;

  ZBioFormatsBridgeClient& operator=(const ZBioFormatsBridgeClient&) = delete;

  [[nodiscard]] std::vector<ZBioFormatsReaderFormat> listFormats();

  [[nodiscard]] bool canRead(const QString& filename);

  [[nodiscard]] ZBioFormatsDatasetInfo readDatasetInfo(const QString& filename);

  [[nodiscard]] std::vector<uint8_t> readRegion(const QString& filename, size_t scene, const ZImgRegion& region);

  [[nodiscard]] std::vector<uint8_t>
  readRegion(const QString& filename, size_t scene, uint32_t resolution, const ZImgRegion& region);

  [[nodiscard]] ZBioFormatsThumbnail readThumbnail(const QString& filename, size_t scene, size_t z, size_t t);

  void warmUp();

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace nim
