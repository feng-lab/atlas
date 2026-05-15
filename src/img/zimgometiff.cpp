#include "zimgometiff.h"

#include "ztiff.h"
#include "zlog.h"
#include "zstringutils.h"
#include "zimgsliceprovider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>

namespace nim {

namespace {

constexpr auto kOmeNamespace = "http://www.openmicroscopy.org/Schemas/OME/2016-06";
constexpr auto kRoiNamespace = "http://www.openmicroscopy.org/Schemas/ROI/2016-06";
constexpr auto kBinaryNamespace = "http://www.openmicroscopy.org/Schemas/BinaryFile/2016-06";
constexpr auto kOmeSchemaLocation =
  "http://www.openmicroscopy.org/Schemas/OME/2016-06 http://www.openmicroscopy.org/Schemas/OME/2016-06/ome.xsd";
constexpr size_t kOmePyramidStopPixels = 512;
constexpr long double kClassicTiffSafeBytes = 1024.0L * 1024.0L * 3600.0L;

size_t checkedSize(std::string_view field, uint64_t value)
{
  if (value > std::numeric_limits<size_t>::max()) {
    throw ZException(fmt::format("OME-TIFF {} value is too large for this Atlas build: {}", field, value));
  }
  return static_cast<size_t>(value);
}

size_t checkedMul(std::string_view field, size_t lhs, size_t rhs)
{
  if (rhs != 0 && lhs > std::numeric_limits<size_t>::max() / rhs) {
    throw ZException(fmt::format("OME-TIFF {} value overflows Atlas size_t", field));
  }
  return lhs * rhs;
}

index_t checkedIndex(std::string_view field, size_t value)
{
  if (value > static_cast<size_t>(std::numeric_limits<index_t>::max())) {
    throw ZException(fmt::format("OME-TIFF {} value is too large for Atlas index_t: {}", field, value));
  }
  return static_cast<index_t>(value);
}

size_t parseSizeAttribute(const QXmlStreamAttributes& attributes, const QString& name, size_t defaultValue)
{
  if (!attributes.hasAttribute(name)) {
    return defaultValue;
  }
  bool ok = false;
  const auto value = attributes.value(name).toString().toULongLong(&ok);
  if (!ok) {
    throw ZException(fmt::format("Can not parse OME-TIFF {}", name));
  }
  return checkedSize(name.toStdString(), value);
}

std::optional<double> parseDoubleAttribute(const QXmlStreamAttributes& attributes, const QString& name)
{
  if (!attributes.hasAttribute(name)) {
    return std::nullopt;
  }
  bool ok = false;
  const double value = attributes.value(name).toString().toDouble(&ok);
  if (!ok) {
    throw ZException(fmt::format("Can not parse OME-TIFF {}", name));
  }
  return value;
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
  return ceilDiv(fullSize, ratio) == resolutionSize || floorDiv(fullSize, ratio) == resolutionSize;
}

size_t scaledResolutionCoordToBase(size_t coord, size_t resolutionSize, size_t fullSize, size_t ratio)
{
  CHECK(ratio > 0);
  CHECK(coord <= resolutionSize);
  return std::min(fullSize, checkedMul("scaled pyramid coordinate", coord, ratio));
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

VoxelSizeUnit omeUnitToAtlas(const QString& unit)
{
  QString normalized = unit.trimmed().toLower();
  normalized.replace(QString::fromUtf8("\xC2\xB5"), "u");
  normalized.replace(QString::fromUtf8("\xCE\xBC"), "u");
  normalized.remove(' ');
  normalized.remove('_');
  normalized.remove('-');

  if (normalized.isEmpty() || normalized == "um" || normalized == "micrometer" || normalized == "micrometre") {
    return VoxelSizeUnit::um;
  }
  if (normalized == "nm" || normalized == "nanometer" || normalized == "nanometre") {
    return VoxelSizeUnit::nm;
  }
  if (normalized == "mm" || normalized == "millimeter" || normalized == "millimetre") {
    return VoxelSizeUnit::mm;
  }
  if (normalized == "cm" || normalized == "centimeter" || normalized == "centimetre") {
    return VoxelSizeUnit::cm;
  }
  if (normalized == "m" || normalized == "meter" || normalized == "metre") {
    return VoxelSizeUnit::m;
  }
  VLOG(1) << "Unsupported OME physical size unit " << unit << "; interpreting value as micrometers";
  return VoxelSizeUnit::um;
}

double physicalSizeToUm(double value, const QXmlStreamAttributes& attributes, const QString& unitAttribute)
{
  const VoxelSizeUnit unit = attributes.hasAttribute(unitAttribute)
                               ? omeUnitToAtlas(attributes.value(unitAttribute).toString())
                               : VoxelSizeUnit::um;
  return value * unitSizeInMeter(unit) / unitSizeInMeter(VoxelSizeUnit::um);
}

void setVoxelFormatFromOmeType(const QString& type, ZImgInfo& info)
{
  if (type.compare("int8", Qt::CaseInsensitive) == 0) {
    info.setVoxelFormat<int8_t>();
  } else if (type.compare("int16", Qt::CaseInsensitive) == 0) {
    info.setVoxelFormat<int16_t>();
  } else if (type.compare("int32", Qt::CaseInsensitive) == 0) {
    info.setVoxelFormat<int32_t>();
  } else if (type.compare("int64", Qt::CaseInsensitive) == 0) {
    info.setVoxelFormat<int64_t>();
  } else if (type.compare("uint8", Qt::CaseInsensitive) == 0) {
    info.setVoxelFormat<uint8_t>();
  } else if (type.compare("uint16", Qt::CaseInsensitive) == 0) {
    info.setVoxelFormat<uint16_t>();
  } else if (type.compare("uint32", Qt::CaseInsensitive) == 0) {
    info.setVoxelFormat<uint32_t>();
  } else if (type.compare("uint64", Qt::CaseInsensitive) == 0) {
    info.setVoxelFormat<uint64_t>();
  } else if (type.compare("float", Qt::CaseInsensitive) == 0) {
    info.setVoxelFormat<float>();
  } else if (type.compare("double", Qt::CaseInsensitive) == 0) {
    info.setVoxelFormat<double>();
  } else {
    throw ZException(fmt::format("Not supported OME-TIFF pixel type: {}", type));
  }
}

QString absoluteCleanPath(const QString& filename)
{
  return QFileInfo(filename).absoluteFilePath();
}

bool sameFilePath(const QString& lhs, const QString& rhs)
{
  return absoluteCleanPath(lhs) == absoluteCleanPath(rhs);
}

QString resolveOmeFilename(const QString& metadataFilename, const QString& filename)
{
  if (filename.isEmpty()) {
    return absoluteCleanPath(metadataFilename);
  }

  QFileInfo info(filename);
  if (info.isAbsolute()) {
    return info.absoluteFilePath();
  }
  return QFileInfo(QFileInfo(metadataFilename).dir(), filename).absoluteFilePath();
}

std::string omeXmlPayloadFromImageDescription(const std::string& imageDescription)
{
  const size_t xmlDeclaration = imageDescription.find("<?xml");
  const size_t omeElement = imageDescription.find("<OME");
  size_t xmlStart = std::string::npos;
  if (xmlDeclaration != std::string::npos && omeElement != std::string::npos) {
    xmlStart = std::min(xmlDeclaration, omeElement);
  } else if (xmlDeclaration != std::string::npos) {
    xmlStart = xmlDeclaration;
  } else {
    xmlStart = omeElement;
  }

  if (xmlStart == std::string::npos) {
    return imageDescription;
  }
  return imageDescription.substr(xmlStart);
}

uint32_t parseOmeChannelColor(const QString& value)
{
  const QString trimmed = value.trimmed();
  bool ok = false;
  const qlonglong signedValue = trimmed.toLongLong(&ok, 10);
  if (ok && signedValue < 0) {
    if (signedValue < std::numeric_limits<int32_t>::min()) {
      throw ZException("Can not parse OME-TIFF channel Color");
    }
    return static_cast<uint32_t>(static_cast<int32_t>(signedValue));
  }

  const qulonglong unsignedValue = trimmed.toULongLong(&ok, 10);
  if (!ok || unsignedValue > std::numeric_limits<uint32_t>::max()) {
    throw ZException("Can not parse OME-TIFF channel Color");
  }
  return static_cast<uint32_t>(unsignedValue);
}

std::optional<QString> binaryOnlyMetadataFilename(const std::string& omeXml, const QString& binaryFilename)
{
  const std::string xmlPayload = omeXmlPayloadFromImageDescription(omeXml);
  QXmlStreamReader xml(QByteArray::fromRawData(xmlPayload.data(), static_cast<int>(xmlPayload.size())));
  while (!xml.atEnd() && !xml.hasError()) {
    if (xml.readNext() != QXmlStreamReader::StartElement) {
      continue;
    }
    if (xml.name() != QString("BinaryOnly")) {
      continue;
    }

    const QXmlStreamAttributes attributes = xml.attributes();
    if (attributes.hasAttribute("MetadataFile")) {
      return resolveOmeFilename(binaryFilename, attributes.value("MetadataFile").toString());
    }
    return std::nullopt;
  }
  if (xml.hasError()) {
    throw ZException(fmt::format("error parsing OME-TIFF BinaryOnly XML: {}", xml.errorString()));
  }
  return std::nullopt;
}

std::string readUtf8TextFile(const QString& filename)
{
  QFile file(filename);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    throw ZException(
      fmt::format("Can not open OME-TIFF companion metadata file '{}': {}", filename, file.errorString()));
  }
  return file.readAll().toStdString();
}

bool isTiffLikeFilename(const QString& filename)
{
  const QString suffix = QFileInfo(filename).suffix().toLower();
  return suffix == QStringLiteral("tif") || suffix == QStringLiteral("tiff") || suffix == QStringLiteral("tf2") ||
         suffix == QStringLiteral("tf8") || suffix == QStringLiteral("btf");
}

std::string readOmeMetadataFile(const QString& filename)
{
  if (isTiffLikeFilename(filename)) {
    ZTiff metadataTiff;
    metadataTiff.load(filename);
    if (metadataTiff.isValid()) {
      const std::string imageDescription = metadataTiff.ifds().front().imageDescription();
      if (!imageDescription.empty()) {
        return imageDescription;
      }
    }
  }
  return readUtf8TextFile(filename);
}

ZTiff& tiffForFile(std::map<QString, std::unique_ptr<ZTiff>>& cache, const QString& filename)
{
  const QString key = absoluteCleanPath(filename);
  auto it = cache.find(key);
  if (it == cache.end()) {
    auto tiff = std::make_unique<ZTiff>();
    tiff->load(key);
    it = cache.emplace(key, std::move(tiff)).first;
  }
  return *it->second;
}

bool omeExtensionRequiresBigTiff(const QString& filename)
{
  return filename.endsWith(".ome.tf2", Qt::CaseInsensitive) || filename.endsWith(".ome.tf8", Qt::CaseInsensitive) ||
         filename.endsWith(".ome.btf", Qt::CaseInsensitive);
}

std::vector<size_t> omePyramidRatios(const ZImgInfo& info)
{
  std::vector<size_t> ratios;
  size_t ratio = 1;
  size_t width = info.width;
  size_t height = info.height;
  while (width > kOmePyramidStopPixels || height > kOmePyramidStopPixels) {
    ratio *= 2;
    ratios.push_back(ratio);
    width = ceilDiv(width, 2);
    height = ceilDiv(height, 2);
  }
  return ratios;
}

long double estimatedOmeTiffPayloadBytes(const ZImgInfo& info)
{
  long double result = 0.0L;
  std::vector<size_t> ratios{1};
  const std::vector<size_t> pyramidRatios = omePyramidRatios(info);
  ratios.insert(ratios.end(), pyramidRatios.begin(), pyramidRatios.end());
  for (size_t ratio : ratios) {
    const long double width = static_cast<long double>(ceilDiv(info.width, ratio));
    const long double height = static_cast<long double>(ceilDiv(info.height, ratio));
    result += width * height * static_cast<long double>(info.depth) * static_cast<long double>(info.numChannels) *
              static_cast<long double>(info.numTimes) * static_cast<long double>(info.bytesPerVoxel);
  }
  const long double ifdCount = static_cast<long double>(info.depth) * static_cast<long double>(info.numChannels) *
                               static_cast<long double>(info.numTimes) * static_cast<long double>(ratios.size());
  return result + ifdCount * 4096.0L;
}

bool shouldWriteOmeBigTiff(const QString& filename, const ZImgInfo& info)
{
  return omeExtensionRequiresBigTiff(filename) || estimatedOmeTiffPayloadBytes(info) > kClassicTiffSafeBytes;
}

std::vector<ZImg> createOmePyramidSubIFDs(ZImg basePlane)
{
  CHECK(basePlane.depth() == 1);
  CHECK(basePlane.numTimes() == 1);
  std::vector<ZImg> subIFDs;
  while (basePlane.width() > kOmePyramidStopPixels || basePlane.height() > kOmePyramidStopPixels) {
    basePlane.zoom(0.5, 0.5);
    subIFDs.push_back(basePlane);
  }
  return subIFDs;
}

size_t tiffTileWidthOrDefault(const ZTiffIFD& ifd)
{
  return ifd.isTiledImage() ? checkedSize("tile width", ifd.tileWidth()) : 512;
}

size_t tiffTileHeightOrDefault(const ZTiffIFD& ifd)
{
  return ifd.isTiledImage() ? checkedSize("tile height", ifd.tileHeight()) : 512;
}

struct OmeSubIFDPlaneSource
{
  QString filename;
  size_t ifdIndex = 0;
  size_t subIFDIndex = 0;
  size_t c = 0;
  size_t channelCount = 1;
};

class ZImgOmeTiffResolutionSubBlock final : public ZImgSubBlock
{
public:
  ZImgOmeTiffResolutionSubBlock(ZImgRegion baseRegion,
                                ZImgRegion resolutionRegion,
                                ZImgInfo resolutionInfo,
                                std::vector<OmeSubIFDPlaneSource> planes,
                                size_t xRatio,
                                size_t yRatio,
                                size_t zRatio)
    : ZImgSubBlock(baseRegion.start.t,
                   baseRegion.start.x,
                   baseRegion.start.y,
                   baseRegion.start.z,
                   checkedSize("pyramid tile base width", baseRegion.end.x - baseRegion.start.x),
                   checkedSize("pyramid tile base height", baseRegion.end.y - baseRegion.start.y),
                   checkedSize("pyramid tile base depth", baseRegion.end.z - baseRegion.start.z),
                   xRatio,
                   yRatio,
                   zRatio)
    , m_resolutionRegion(std::move(resolutionRegion))
    , m_resolutionInfo(std::move(resolutionInfo))
    , m_planes(std::move(planes))
  {
    CHECK(m_resolutionRegion.isValid(m_resolutionInfo));
    CHECK(xRatio > 0);
    CHECK(yRatio > 0);
    CHECK(zRatio > 0);
  }

  [[nodiscard]] std::shared_ptr<ZImg> read() const override
  {
    ZImgRegion resolvedRegion = m_resolutionRegion;
    resolvedRegion.resolveRegionEnd(m_resolutionInfo);

    auto img = std::make_shared<ZImg>(resolvedRegion.clip(m_resolutionInfo));
    std::vector<uint8_t> channelFilled(img->numChannels(), 0);
    std::map<QString, std::unique_ptr<ZTiff>> tiffCache;
    for (const OmeSubIFDPlaneSource& plane : m_planes) {
      const index_t planeCStart = checkedIndex("pyramid channel start", plane.c);
      const index_t planeCEnd = checkedIndex("pyramid channel end", plane.c + plane.channelCount);
      const index_t overlapCStart = std::max(resolvedRegion.start.c, planeCStart);
      const index_t overlapCEnd = std::min(resolvedRegion.end.c, planeCEnd);
      if (overlapCStart >= overlapCEnd) {
        continue;
      }

      ZTiff& tiff = tiffForFile(tiffCache, plane.filename);
      if (plane.ifdIndex >= tiff.ifds().size()) {
        throw ZException(fmt::format("OME-TIFF pyramid plane references IFD {} in '{}', but the file has {} IFDs",
                                     plane.ifdIndex,
                                     plane.filename,
                                     tiff.ifds().size()));
      }
      const ZTiffIFD& ifd = tiff.ifds()[plane.ifdIndex];
      if (plane.subIFDIndex >= ifd.subIFDs().size()) {
        throw ZException(fmt::format("OME-TIFF pyramid plane references SubIFD {} for IFD {} in '{}', but only {} "
                                     "SubIFDs are present",
                                     plane.subIFDIndex,
                                     plane.ifdIndex,
                                     plane.filename,
                                     ifd.subIFDs().size()));
      }

      ZImg planeImg;
      ZImgRegion ifdRegion(resolvedRegion.start.x,
                           resolvedRegion.end.x,
                           resolvedRegion.start.y,
                           resolvedRegion.end.y,
                           0,
                           1,
                           overlapCStart - planeCStart,
                           overlapCEnd - planeCStart,
                           0,
                           1);
      tiff.readRegionFromIFD(ifd.subIFDs()[plane.subIFDIndex], planeImg, ifdRegion);
      img->pasteImg(planeImg, ZVoxelCoordinate(0, 0, 0, overlapCStart - resolvedRegion.start.c, 0), false);

      for (index_t c = overlapCStart; c < overlapCEnd; ++c) {
        channelFilled[checkedSize("pyramid channel fill", c - resolvedRegion.start.c)] = 1;
      }
    }

    for (size_t c = 0; c < channelFilled.size(); ++c) {
      if (channelFilled[c] == 0) {
        throw ZException(fmt::format("OME-TIFF pyramid tile is missing requested channel {}", c));
      }
    }
    return img;
  }

  [[nodiscard]] ZImgInfo readInfo() const override
  {
    return m_resolutionRegion.clip(m_resolutionInfo);
  }

private:
  ZImgRegion m_resolutionRegion;
  ZImgInfo m_resolutionInfo;
  std::vector<OmeSubIFDPlaneSource> m_planes;
};

void addOmeBaseTiledSubBlocks(const QString& filename,
                              const ZImgInfo& info,
                              size_t scene,
                              size_t tileWidth,
                              size_t tileHeight,
                              std::vector<std::shared_ptr<ZImgSubBlock>>& subBlocks)
{
  const size_t tw = std::max<size_t>(1, tileWidth);
  const size_t th = std::max<size_t>(1, tileHeight);
  for (size_t t = 0; t < info.numTimes; ++t) {
    for (size_t z = 0; z < info.depth; ++z) {
      for (size_t y = 0; y < info.height; y += th) {
        const size_t yEnd = std::min(info.height, y + th);
        for (size_t x = 0; x < info.width; x += tw) {
          const size_t xEnd = std::min(info.width, x + tw);
          subBlocks.emplace_back(std::make_shared<ZImgTileSubBlock>(
            ZImgSource(filename,
                       ZImgRegion(ZVoxelCoordinate(checkedIndex("OME-TIFF tile x", x),
                                                   checkedIndex("OME-TIFF tile y", y),
                                                   checkedIndex("OME-TIFF tile z", z),
                                                   0,
                                                   checkedIndex("OME-TIFF tile t", t)),
                                  ZVoxelCoordinate(checkedIndex("OME-TIFF tile x end", xEnd),
                                                   checkedIndex("OME-TIFF tile y end", yEnd),
                                                   checkedIndex("OME-TIFF tile z end", z + 1),
                                                   checkedIndex("OME-TIFF tile channel end", info.numChannels),
                                                   checkedIndex("OME-TIFF tile t end", t + 1))),
                       scene,
                       FileFormat::OmeTiff)));
        }
      }
    }
  }
}

void addOmePyramidTiledSubBlocks(const ZImgInfo& baseInfo,
                                 size_t resolutionZ,
                                 size_t resolutionT,
                                 const ZImgInfo& resolutionInfo,
                                 std::vector<OmeSubIFDPlaneSource> planes,
                                 size_t xRatio,
                                 size_t yRatio,
                                 size_t zRatio,
                                 size_t tileWidth,
                                 size_t tileHeight,
                                 std::vector<std::shared_ptr<ZImgSubBlock>>& subBlocks)
{
  const size_t tw = std::max<size_t>(1, tileWidth);
  const size_t th = std::max<size_t>(1, tileHeight);
  for (size_t y = 0; y < resolutionInfo.height; y += th) {
    const size_t yEnd = std::min(resolutionInfo.height, y + th);
    for (size_t x = 0; x < resolutionInfo.width; x += tw) {
      const size_t xEnd = std::min(resolutionInfo.width, x + tw);
      const size_t baseX = scaledResolutionCoordToBase(x, resolutionInfo.width, baseInfo.width, xRatio);
      const size_t baseY = scaledResolutionCoordToBase(y, resolutionInfo.height, baseInfo.height, yRatio);
      const size_t baseXEnd = scaledResolutionCoordToBase(xEnd, resolutionInfo.width, baseInfo.width, xRatio);
      const size_t baseYEnd = scaledResolutionCoordToBase(yEnd, resolutionInfo.height, baseInfo.height, yRatio);
      const size_t baseZ = scaledResolutionCoordToBase(resolutionZ, resolutionInfo.depth, baseInfo.depth, zRatio);
      const size_t baseZEnd =
        scaledResolutionCoordToBase(resolutionZ + 1, resolutionInfo.depth, baseInfo.depth, zRatio);

      ZImgRegion baseRegion(ZVoxelCoordinate(checkedIndex("OME-TIFF pyramid base x", baseX),
                                             checkedIndex("OME-TIFF pyramid base y", baseY),
                                             checkedIndex("OME-TIFF pyramid base z", baseZ),
                                             0,
                                             checkedIndex("OME-TIFF pyramid base t", resolutionT)),
                            ZVoxelCoordinate(checkedIndex("OME-TIFF pyramid base x end", baseXEnd),
                                             checkedIndex("OME-TIFF pyramid base y end", baseYEnd),
                                             checkedIndex("OME-TIFF pyramid base z end", baseZEnd),
                                             checkedIndex("OME-TIFF pyramid base channel end", baseInfo.numChannels),
                                             checkedIndex("OME-TIFF pyramid base t end", resolutionT + 1)));
      ZImgRegion resolutionRegion(
        ZVoxelCoordinate(checkedIndex("OME-TIFF pyramid resolution x", x),
                         checkedIndex("OME-TIFF pyramid resolution y", y),
                         checkedIndex("OME-TIFF pyramid resolution z", resolutionZ),
                         0,
                         checkedIndex("OME-TIFF pyramid resolution t", resolutionT)),
        ZVoxelCoordinate(checkedIndex("OME-TIFF pyramid resolution x end", xEnd),
                         checkedIndex("OME-TIFF pyramid resolution y end", yEnd),
                         checkedIndex("OME-TIFF pyramid resolution z end", resolutionZ + 1),
                         checkedIndex("OME-TIFF pyramid resolution channel end", resolutionInfo.numChannels),
                         checkedIndex("OME-TIFF pyramid resolution t end", resolutionT + 1)));

      subBlocks.emplace_back(std::make_shared<ZImgOmeTiffResolutionSubBlock>(std::move(baseRegion),
                                                                             std::move(resolutionRegion),
                                                                             resolutionInfo,
                                                                             planes,
                                                                             xRatio,
                                                                             yRatio,
                                                                             zRatio));
    }
  }
}

} // namespace

void ZImgOmeTiff::readIntoInternalStructure(const QString& filename, ZTiff& tiff)
{
  ZImgTiff::readIntoInternalStructure(filename, tiff);
  m_currentFilename = absoluteCleanPath(filename);
  m_omeXmlBaseFilename = m_currentFilename;
  if (m_imageDescription.empty() || !absl::StrContains(m_imageDescription, "<OME"sv)) {
    throw ZException("Not OME Tiff file");
  }

  for (size_t binaryOnlyDepth = 0;
       !absl::StrContains(m_imageDescription, "<Pixels"sv) && absl::StrContains(m_imageDescription, "<BinaryOnly"sv);
       ++binaryOnlyDepth) {
    if (binaryOnlyDepth >= 8) {
      throw ZException("OME-TIFF BinaryOnly metadata indirection is too deep");
    }
    const std::optional<QString> metadataFile = binaryOnlyMetadataFilename(m_imageDescription, m_currentFilename);
    if (!metadataFile) {
      throw ZException("OME-TIFF BinaryOnly block does not specify a companion MetadataFile");
    }
    m_omeXmlBaseFilename = absoluteCleanPath(*metadataFile);
    m_imageDescription = readOmeMetadataFile(*metadataFile);
    m_imageDescription = omeXmlPayloadFromImageDescription(m_imageDescription);
  }
  m_imageDescription = omeXmlPayloadFromImageDescription(m_imageDescription);

  if (absl::StrContains(m_imageDescription, "<Pixels"sv)) {
    readOmeInfo(tiff);
  } else {
    throw ZException("OME-TIFF XML does not contain Pixels metadata");
  }
}

void ZImgOmeTiff::clearInternalState()
{
  ZImgTiff::clearInternalState();
  m_omeSeries.clear();
  m_ifdIdxPosMap.clear();
  m_currentFilename.clear();
  m_omeXmlBaseFilename.clear();
}

void ZImgOmeTiff::detectImgInfo(ZTiff& tiff)
{
  if (m_omeSeries.empty()) {
    throw ZException("OME-TIFF metadata does not contain any Image/Pixels series");
  }

  std::map<QString, std::unique_ptr<ZTiff>> externalTiffs;
  auto sourceTiff = [&](const QString& filename) -> ZTiff& {
    if (sameFilePath(filename, m_currentFilename)) {
      return tiff;
    }
    return tiffForFile(externalTiffs, filename);
  };

  m_ifdIdxPosMap.clear();
  m_imgInfo.clear();
  m_imgInfo.reserve(m_omeSeries.size());
  for (size_t seriesIndex = 0; seriesIndex < m_omeSeries.size(); ++seriesIndex) {
    SeriesInfo& series = m_omeSeries[seriesIndex];
    if (series.info.isEmpty()) {
      throw ZException(fmt::format("OME-TIFF series {} is empty", seriesIndex));
    }

    std::map<QString, std::pair<size_t, size_t>> validIfdRangesByFile;
    for (const PlaneSource& plane : series.planes) {
      if (!plane.valid) {
        continue;
      }
      auto [it, inserted] =
        validIfdRangesByFile.emplace(plane.filename, std::make_pair(plane.ifdIndex, plane.ifdIndex));
      if (!inserted) {
        it->second.first = std::min(it->second.first, plane.ifdIndex);
        it->second.second = std::max(it->second.second, plane.ifdIndex);
      }
    }
    for (const auto& [planeFilename, ifdRange] : validIfdRangesByFile) {
      ZTiff& planeTiff = sourceTiff(planeFilename);
      const size_t ifdCount = planeTiff.ifds().size();
      if (ifdRange.first > 0 && ifdRange.second == ifdCount) {
        VLOG(1) << fmt::format("Interpreting OME-TIFF series {} TiffData IFD values in '{}' as 1-based because "
                               "the explicit mapping reaches past the last zero-based IFD",
                               seriesIndex,
                               planeFilename.toStdString());
        for (PlaneSource& plane : series.planes) {
          if (plane.valid && sameFilePath(plane.filename, planeFilename)) {
            --plane.ifdIndex;
          }
        }
      }
    }

    const PlaneSource* firstPlane = nullptr;
    for (const PlaneSource& plane : series.planes) {
      if (plane.valid) {
        firstPlane = &plane;
        break;
      }
    }
    if (!firstPlane) {
      throw ZException(fmt::format("OME-TIFF series {} has no mapped TiffData planes", seriesIndex));
    }

    ZImgInfo firstIFDInfo;
    bool firstIFDInfoSet = false;
    for (const PlaneSource& plane : series.planes) {
      if (!plane.valid) {
        continue;
      }
      ZTiff& planeTiff = sourceTiff(plane.filename);
      if (plane.ifdIndex >= planeTiff.ifds().size()) {
        throw ZException(fmt::format("OME-TIFF series {} references IFD {} in '{}', but the file has {} IFDs",
                                     seriesIndex,
                                     plane.ifdIndex,
                                     plane.filename,
                                     planeTiff.ifds().size()));
      }

      ZImgInfo ifdInfo;
      planeTiff.readInfoFromIFD(planeTiff.ifds()[plane.ifdIndex], ifdInfo);
      if (series.info.width != ifdInfo.width || series.info.height != ifdInfo.height ||
          series.info.bytesPerVoxel != ifdInfo.bytesPerVoxel) {
        throw ZException(fmt::format("OME-TIFF metadata for series {} <{}> doesn't match image data <{}>",
                                     seriesIndex,
                                     series.info,
                                     ifdInfo));
      }
      if (ifdInfo.numChannels != plane.channelCount) {
        if (ifdInfo.numChannels < plane.channelCount) {
          throw ZException(fmt::format("OME-TIFF series {} maps {} channel(s) to IFD {}, but that IFD contains {} "
                                       "sample(s)",
                                       seriesIndex,
                                       plane.channelCount,
                                       plane.ifdIndex,
                                       ifdInfo.numChannels));
        }
        VLOG(1) << fmt::format("OME-TIFF series {} maps {} channel(s) to IFD {}, which contains {} sample(s); "
                               "reading the mapped leading sample(s)",
                               seriesIndex,
                               plane.channelCount,
                               plane.ifdIndex,
                               ifdInfo.numChannels);
      }
      if (!firstIFDInfoSet) {
        firstIFDInfo = ifdInfo;
        firstIFDInfoSet = true;
        series.info.lastChannelIsAlphaChannel = ifdInfo.lastChannelIsAlphaChannel && series.info.numChannels > 0 &&
                                                plane.c + plane.channelCount == series.info.numChannels;
      }
    }

    series.info.voxelFormat = firstIFDInfo.voxelFormat;
    series.info.validBitCount = series.significantBits.value_or(firstIFDInfo.validBitCount);
    series.info.createDefaultDescriptions();
    m_imgInfo.push_back(series.info);

    for (const PlaneSource& plane : series.planes) {
      if (plane.valid && sameFilePath(plane.filename, m_currentFilename)) {
        m_ifdIdxPosMap[plane.ifdIndex] = IFDPos(plane.z, plane.c, plane.t, seriesIndex);
      }
    }
  }
}

bool ZImgOmeTiff::mapIFDToImgLocation(size_t ifdIdx, index_t& z, index_t& c, index_t& t, index_t& l)
{
  if (!m_ifdIdxPosMap.contains(ifdIdx)) {
    return false;
  }
  const IFDPos pos = m_ifdIdxPosMap[ifdIdx];
  z = checkedIndex("OME-TIFF IFD z", pos.z);
  c = checkedIndex("OME-TIFF IFD channel", pos.c);
  t = checkedIndex("OME-TIFF IFD time", pos.t);
  l = checkedIndex("OME-TIFF IFD scene", pos.l);
  return true;
}

QString ZImgOmeTiff::shortName() const
{
  return "OME Tiff";
}

QString ZImgOmeTiff::fullName() const
{
  return "OME Tiff";
}

QStringList ZImgOmeTiff::extensions() const
{
  QStringList res;
  res << "ome.tif"
      << "ome.tiff"
      << "ome.tf2"
      << "ome.tf8"
      << "ome.btf";
  return res;
}

void ZImgOmeTiff::writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, img.info(), paras);

  ZTiffWriter tiffWriter;
  int32_t extraSample = img.info().lastChannelIsAlphaChannel ? 2 : -1; // EXTRASAMPLE_UNASSALPHA or none
  tiffWriter.startWriting(filename, paras.compression, extraSample, shouldWriteOmeBigTiff(filename, img.info()));
  std::vector<ZImgMetatag> tags(1);
  makeImageDescriptionTag(img.info(), "XYZCT", tags[0]);
  for (size_t t = 0; t < img.numTimes(); ++t) {
    for (size_t c = 0; c < img.numChannels(); ++c) {
      for (size_t z = 0; z < img.depth(); ++z) {
        std::vector<ZImg> subIFDs = createOmePyramidSubIFDs(img.createView(z, c, t));
        if (t == 0 && z == 0 && c == 0) {
          tiffWriter.writeIFD(img, z, t, c, false, tags, &subIFDs, kOmePyramidStopPixels, kOmePyramidStopPixels);
        } else {
          tiffWriter.writeIFD(img, z, t, c, false, {}, &subIFDs, kOmePyramidStopPixels, kOmePyramidStopPixels);
        }
      }
    }
  }
}

void ZImgOmeTiff::writeImg(const QString& filename,
                           const ZImgSliceProvider& imgSliceProvider,
                           const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, imgSliceProvider.imgInfo(), paras);

  ZTiffWriter tiffWriter;
  int32_t extraSample = imgSliceProvider.imgInfo().lastChannelIsAlphaChannel ? 2 : -1; // EXTRASAMPLE_UNASSALPHA or none
  tiffWriter.startWriting(filename,
                          paras.compression,
                          extraSample,
                          shouldWriteOmeBigTiff(filename, imgSliceProvider.imgInfo()));
  std::vector<ZImgMetatag> tags(1);
  makeImageDescriptionTag(imgSliceProvider.imgInfo(), "XYCZT", tags[0]);
  for (size_t t = 0; t < imgSliceProvider.imgInfo().numTimes; ++t) {
    for (size_t z = 0; z < imgSliceProvider.imgInfo().depth; ++z) {
      ZImg img = imgSliceProvider.slice(z, t);
      for (size_t c = 0; c < imgSliceProvider.imgInfo().numChannels; ++c) {
        std::vector<ZImg> subIFDs = createOmePyramidSubIFDs(img.createView(c, 0));
        if (t == 0 && z == 0 && c == 0) {
          tiffWriter.writeIFD(img, 0, 0, c, false, tags, &subIFDs, kOmePyramidStopPixels, kOmePyramidStopPixels);
        } else {
          tiffWriter.writeIFD(img, 0, 0, c, false, {}, &subIFDs, kOmePyramidStopPixels, kOmePyramidStopPixels);
        }
      }
    }
  }
}

void ZImgOmeTiff::readInfo(const QString& filename,
                           std::vector<ZImgInfo>& infos,
                           std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  clearInternalState();
  ZTiff tiff;
  readIntoInternalStructure(filename, tiff);
  detectImgInfo(tiff);
  infos = m_imgInfo;
  createOmeSubBlocks(filename, tiff, subBlocks);

  if (!m_imageDescription.empty()) {
    VLOG(1) << m_imageDescription;
  }
}

void ZImgOmeTiff::readImg(const QString& filename,
                          ZImg& img,
                          const ZImgRegion& region,
                          size_t scene,
                          const ZImgReadOptions& readOptions)
{
  clearInternalState();
  ZTiff tiff;
  readIntoInternalStructure(filename, tiff);
  detectImgInfo(tiff);
  if (scene >= m_imgInfo.size()) {
    throw ZException(fmt::format("OME-TIFF scene {} is out of range for {}", scene, filename));
  }
  if (region.isEmpty() || !region.isValid(m_imgInfo[scene])) {
    throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", m_imgInfo[scene], region));
  }

  ZImgRegion resolvedRegion = region;
  resolvedRegion.resolveRegionEnd(m_imgInfo[scene]);
  const ZImgInfo partialImgInfo = resolvedRegion.clip(m_imgInfo[scene]);
  ZImg imgTmp(partialImgInfo);

  const SeriesInfo& series = m_omeSeries[scene];
  std::vector<uint8_t> planeChannelFilled(
    checkedMul("requested plane/channel flags",
               checkedMul("requested plane/channel flags", partialImgInfo.depth, partialImgInfo.numChannels),
               partialImgInfo.numTimes),
    0);
  auto markIndex = [&](size_t z, size_t c, size_t t) {
    const size_t localZ = z - static_cast<size_t>(resolvedRegion.start.z);
    const size_t localC = c - static_cast<size_t>(resolvedRegion.start.c);
    const size_t localT = t - static_cast<size_t>(resolvedRegion.start.t);
    return checkedMul("requested plane/channel index",
                      checkedMul("requested plane/channel index", localT, partialImgInfo.numChannels) + localC,
                      partialImgInfo.depth) +
           localZ;
  };

  std::map<QString, std::unique_ptr<ZTiff>> externalTiffs;
  auto sourceTiff = [&](const QString& sourceFilename) -> ZTiff& {
    if (sameFilePath(sourceFilename, m_currentFilename)) {
      return tiff;
    }
    return tiffForFile(externalTiffs, sourceFilename);
  };

  for (const PlaneSource& plane : series.planes) {
    if (!plane.valid || !resolvedRegion.zInRegion(checkedIndex("OME-TIFF plane z", plane.z)) ||
        !resolvedRegion.tInRegion(checkedIndex("OME-TIFF plane t", plane.t))) {
      continue;
    }

    const index_t planeCStart = checkedIndex("OME-TIFF plane channel start", plane.c);
    const index_t planeCEnd = checkedIndex("OME-TIFF plane channel end", plane.c + plane.channelCount);
    const index_t overlapCStart = std::max(resolvedRegion.start.c, planeCStart);
    const index_t overlapCEnd = std::min(resolvedRegion.end.c, planeCEnd);
    if (overlapCStart >= overlapCEnd) {
      continue;
    }

    ZTiff& planeTiff = sourceTiff(plane.filename);
    if (plane.ifdIndex >= planeTiff.ifds().size()) {
      throw ZException(fmt::format("OME-TIFF plane references IFD {} in '{}', but the file has {} IFDs",
                                   plane.ifdIndex,
                                   plane.filename,
                                   planeTiff.ifds().size()));
    }

    ZImg planeImg;
    ZImgRegion ifdRegion(resolvedRegion.start.x,
                         resolvedRegion.end.x,
                         resolvedRegion.start.y,
                         resolvedRegion.end.y,
                         0,
                         1,
                         overlapCStart - planeCStart,
                         overlapCEnd - planeCStart,
                         0,
                         1);
    planeTiff.readRegionFromIFD(planeTiff.ifds()[plane.ifdIndex], planeImg, ifdRegion);
    imgTmp.pasteImg(
      planeImg,
      ZVoxelCoordinate(0,
                       0,
                       checkedIndex("OME-TIFF paste z", plane.z - static_cast<size_t>(resolvedRegion.start.z)),
                       overlapCStart - resolvedRegion.start.c,
                       checkedIndex("OME-TIFF paste t", plane.t - static_cast<size_t>(resolvedRegion.start.t))),
      false);

    for (index_t c = overlapCStart; c < overlapCEnd; ++c) {
      planeChannelFilled[markIndex(plane.z, checkedSize("OME-TIFF requested channel", c), plane.t)] = 1;
    }
  }

  for (size_t t = 0; t < partialImgInfo.numTimes; ++t) {
    for (size_t c = 0; c < partialImgInfo.numChannels; ++c) {
      for (size_t z = 0; z < partialImgInfo.depth; ++z) {
        const size_t index = checkedMul("requested plane/channel index",
                                        checkedMul("requested plane/channel index", t, partialImgInfo.numChannels) + c,
                                        partialImgInfo.depth) +
                             z;
        if (planeChannelFilled[index] == 0) {
          throw ZException(fmt::format("OME-TIFF metadata does not map requested plane z={}, c={}, t={} in scene {}",
                                       z + static_cast<size_t>(resolvedRegion.start.z),
                                       c + static_cast<size_t>(resolvedRegion.start.c),
                                       t + static_cast<size_t>(resolvedRegion.start.t),
                                       scene));
        }
      }
    }
  }

  if (readOptions.includeMetadata && !m_imageDescription.empty()) {
    imgTmp.metadataRef().attachToTopLevel(ZImgMetatag("metadata", m_imageDescription));
  }
  imgTmp.swap(img);
}

bool ZImgOmeTiff::supportRead() const
{
  return true;
}

bool ZImgOmeTiff::supportWrite() const
{
  return true;
}

void ZImgOmeTiff::readOmeInfo(ZTiff& tiff)
{
  m_omeSeries.clear();
  m_ifdIdxPosMap.clear();

  QXmlStreamReader xml(QByteArray::fromRawData(m_imageDescription.data(), static_cast<int>(m_imageDescription.size())));
  while (!xml.atEnd() && !xml.hasError()) {
    if (xml.readNext() != QXmlStreamReader::StartElement) {
      continue;
    }
    if (xml.name() != QString("OME")) {
      continue;
    }
    parseOME(xml, tiff);
    break;
  }
  if (xml.hasError()) {
    throw ZException(fmt::format("error parsing OME-TIFF XML: {}", xml.errorString()));
  }
  xml.clear();
}

void ZImgOmeTiff::makeImageDescriptionTag(const ZImgInfo& info, const QString& dimensionOrder, ZImgMetatag& tag)
{
  ZImgMetatag("ImageDescription", createOmeXml(info, dimensionOrder), 270).swap(tag);
}

void ZImgOmeTiff::parseOME(QXmlStreamReader& xml, ZTiff& tiff)
{
  CHECK(xml.isStartElement() && xml.name() == QString("OME"));

  size_t seriesIndex = 0;
  while (xml.readNextStartElement()) {
    if (xml.name() == QString("Image")) {
      bool parsedPixels = false;
      while (xml.readNextStartElement()) {
        if (xml.name() == QString("Pixels")) {
          parsedPixels = parsePixels(xml, tiff, seriesIndex) || parsedPixels;
        } else {
          xml.skipCurrentElement();
        }
      }
      if (parsedPixels) {
        ++seriesIndex;
      } else if (seriesIndex < m_omeSeries.size() && m_omeSeries[seriesIndex].metadataOnly) {
        m_omeSeries.pop_back();
      } else {
        VLOG(1) << fmt::format("Skipping OME-TIFF Image {} because it has no Pixels element", seriesIndex);
      }
    } else {
      xml.skipCurrentElement();
    }
  }
}

bool ZImgOmeTiff::parsePixels(QXmlStreamReader& xml, ZTiff& tiff, size_t seriesIndex)
{
  CHECK(xml.isStartElement() && xml.name() == QString("Pixels"));
  if (seriesIndex >= m_omeSeries.size()) {
    m_omeSeries.resize(seriesIndex + 1);
  }
  SeriesInfo& series = m_omeSeries[seriesIndex];
  series.info.clear();
  series.dimensionOrder = "ZCTL";
  series.samplesPerPlane = 1;
  series.significantBits.reset();
  series.planes.clear();
  series.metadataOnly = false;

  const QXmlStreamAttributes attributes = xml.attributes();
  if (attributes.hasAttribute("DimensionOrder")) {
    QString dimOrder = attributes.value("DimensionOrder").toString();
    if (dimOrder.size() != 5 || !dimOrder.startsWith("XY")) {
      throw ZException(fmt::format("Wrong OME-TIFF DimensionOrder: {}", dimOrder));
    }
    dimOrder.remove(0, 2);
    dimOrder += "L";
    if (!isDimensionOrderValid(dimOrder)) {
      throw ZException(fmt::format("Wrong OME-TIFF DimensionOrder: {}", attributes.value("DimensionOrder")));
    }
    series.dimensionOrder = dimOrder;
  }

  series.info.width = parseSizeAttribute(attributes, "SizeX", 1);
  series.info.height = parseSizeAttribute(attributes, "SizeY", 1);
  series.info.depth = parseSizeAttribute(attributes, "SizeZ", 1);
  series.info.numChannels = parseSizeAttribute(attributes, "SizeC", 1);
  series.info.numTimes = parseSizeAttribute(attributes, "SizeT", 1);

  const QString type = attributes.hasAttribute("Type")        ? attributes.value("Type").toString()
                       : attributes.hasAttribute("PixelType") ? attributes.value("PixelType").toString()
                                                              : QString();
  if (type.isEmpty()) {
    throw ZException("Can not find OME-TIFF PixelType or Type attribute");
  }
  setVoxelFormatFromOmeType(type, series.info);
  if (attributes.hasAttribute("SignificantBits")) {
    series.significantBits = parseSizeAttribute(attributes, "SignificantBits", 0);
  }
  if (attributes.hasAttribute("BigEndian")) {
    const QString value = attributes.value("BigEndian").toString().trimmed().toLower();
    if (value != "true" && value != "false" && value != "1" && value != "0") {
      throw ZException(fmt::format("Can not parse OME-TIFF BigEndian: {}", value));
    }
  }
  if (attributes.hasAttribute("Interleaved")) {
    const QString value = attributes.value("Interleaved").toString().trimmed().toLower();
    if (value != "true" && value != "false" && value != "1" && value != "0") {
      throw ZException(fmt::format("Can not parse OME-TIFF Interleaved: {}", value));
    }
  }

  if (const std::optional<double> physicalSizeX = parseDoubleAttribute(attributes, "PhysicalSizeX")) {
    series.info.voxelSizeUnit = VoxelSizeUnit::um;
    series.info.voxelSizeX = physicalSizeToUm(*physicalSizeX, attributes, "PhysicalSizeXUnit");
  }
  if (const std::optional<double> physicalSizeY = parseDoubleAttribute(attributes, "PhysicalSizeY")) {
    series.info.voxelSizeUnit = VoxelSizeUnit::um;
    series.info.voxelSizeY = physicalSizeToUm(*physicalSizeY, attributes, "PhysicalSizeYUnit");
  }
  if (const std::optional<double> physicalSizeZ = parseDoubleAttribute(attributes, "PhysicalSizeZ")) {
    series.info.voxelSizeUnit = VoxelSizeUnit::um;
    series.info.voxelSizeZ = physicalSizeToUm(*physicalSizeZ, attributes, "PhysicalSizeZUnit");
  }

  if (const std::optional<double> timeIncrement = parseDoubleAttribute(attributes, "TimeIncrement")) {
    series.info.timeStamps.resize(series.info.numTimes);
    series.info.timeStamps[0] = 0.;
    for (size_t tIndex = 1; tIndex < series.info.numTimes; ++tIndex) {
      series.info.timeStamps[tIndex] = series.info.timeStamps[tIndex - 1] + *timeIncrement;
    }
  }

  const size_t initialPlaneCount = checkedMul("plane count",
                                              checkedMul("plane count", series.info.depth, series.info.numChannels),
                                              series.info.numTimes);
  series.planes.resize(initialPlaneCount);

  while (xml.readNextStartElement()) {
    if (xml.name() == QString("TiffData")) {
      parseTiffData(xml, tiff, seriesIndex);
    } else if (xml.name() == QString("Channel")) {
      parseChannel(xml, seriesIndex);
    } else if (xml.name() == QString("MetadataOnly")) {
      series.metadataOnly = true;
      xml.skipCurrentElement();
    } else if (xml.name() == QString("Plane")) {
      const QXmlStreamAttributes planeAttributes = xml.attributes();
      if (planeAttributes.hasAttribute("TheT") && planeAttributes.hasAttribute("DeltaT")) {
        const size_t theT = parseSizeAttribute(planeAttributes, "TheT", 0);
        const std::optional<double> deltaT = parseDoubleAttribute(planeAttributes, "DeltaT");
        if (deltaT && theT < series.info.numTimes) {
          if (series.info.timeStamps.size() != series.info.numTimes) {
            series.info.timeStamps.resize(series.info.numTimes, 0.);
          }
          series.info.timeStamps[theT] = *deltaT;
        }
      }
      xml.skipCurrentElement();
    } else {
      xml.skipCurrentElement();
    }
  }

  if (series.metadataOnly) {
    for (auto it = m_ifdIdxPosMap.begin(); it != m_ifdIdxPosMap.end();) {
      if (it->second.l == seriesIndex) {
        it = m_ifdIdxPosMap.erase(it);
      } else {
        ++it;
      }
    }
    series.info.clear();
    series.planes.clear();
    VLOG(1) << fmt::format("Skipping OME-TIFF Image {} because its Pixels element is MetadataOnly", seriesIndex);
    return false;
  }

  const bool hasMappedPlane = std::any_of(series.planes.begin(), series.planes.end(), [](const PlaneSource& plane) {
    return plane.valid;
  });
  if (!hasMappedPlane) {
    if (series.samplesPerPlane == 0 || series.info.numChannels % series.samplesPerPlane != 0) {
      throw ZException(fmt::format("OME-TIFF SamplesPerPixel {} is incompatible with SizeC {}",
                                   series.samplesPerPlane,
                                   series.info.numChannels));
    }
    const size_t effectiveChannels = series.info.numChannels / series.samplesPerPlane;
    ZImgInfo planeIndexInfo = series.info;
    planeIndexInfo.numChannels = effectiveChannels;
    for (size_t ifdIndex = 0; ifdIndex < series.planes.size(); ++ifdIndex) {
      index_t z = 0;
      index_t effectiveC = 0;
      index_t tIndex = 0;
      index_t l = 0;
      if (!IFDToLoc(ifdIndex, z, effectiveC, tIndex, l, 0, series.dimensionOrder, planeIndexInfo, 1)) {
        break;
      }
      const size_t c = checkedSize("channel", effectiveC) * series.samplesPerPlane;
      const size_t planeIndex =
        checkedMul("default OME-TIFF plane index",
                   checkedMul("default OME-TIFF plane index", checkedSize("time", tIndex), effectiveChannels) +
                     checkedSize("effective channel", effectiveC),
                   series.info.depth) +
        checkedSize("z", z);
      PlaneSource& plane = series.planes[planeIndex];
      plane.filename = m_currentFilename;
      plane.ifdIndex = ifdIndex;
      plane.z = checkedSize("z", z);
      plane.c = c;
      plane.t = checkedSize("time", tIndex);
      plane.channelCount = series.samplesPerPlane;
      plane.valid = true;
      m_ifdIdxPosMap[ifdIndex] = IFDPos(plane.z, plane.c, plane.t, seriesIndex);
    }
  }

  series.info.createDefaultDescriptions();
  return true;
}

void ZImgOmeTiff::parseTiffData(QXmlStreamReader& xml, ZTiff& tiff, size_t seriesIndex)
{
  (void)tiff;
  CHECK(seriesIndex < m_omeSeries.size());
  SeriesInfo& series = m_omeSeries[seriesIndex];
  const QXmlStreamAttributes attributes = xml.attributes();

  size_t ifd = parseSizeAttribute(attributes, "IFD", 0);
  size_t planeCount = attributes.hasAttribute("IFD") ? 1 : series.planes.size();
  if (attributes.hasAttribute("PlaneCount")) {
    planeCount = parseSizeAttribute(attributes, "PlaneCount", planeCount);
  }
  const size_t firstZ = parseSizeAttribute(attributes, "FirstZ", 0);
  const size_t firstT = parseSizeAttribute(attributes, "FirstT", 0);
  const size_t firstC = parseSizeAttribute(attributes, "FirstC", 0);

  QString planeFilename = m_currentFilename;
  while (xml.readNextStartElement()) {
    if (xml.name() == QString("UUID")) {
      const QXmlStreamAttributes uuidAttributes = xml.attributes();
      if (uuidAttributes.hasAttribute("FileName")) {
        planeFilename = resolveOmeFilename(m_omeXmlBaseFilename, uuidAttributes.value("FileName").toString());
      }
    }
    xml.skipCurrentElement();
  }

  if (series.samplesPerPlane == 0 || series.info.numChannels % series.samplesPerPlane != 0) {
    throw ZException(fmt::format("OME-TIFF SamplesPerPixel {} is incompatible with SizeC {}",
                                 series.samplesPerPlane,
                                 series.info.numChannels));
  }
  if (firstC % series.samplesPerPlane != 0) {
    throw ZException(
      fmt::format("OME-TIFF FirstC {} is not aligned to SamplesPerPixel {}", firstC, series.samplesPerPlane));
  }
  const size_t firstEffectiveC = firstC / series.samplesPerPlane;
  const size_t effectiveChannels = series.info.numChannels / series.samplesPerPlane;
  ZImgInfo planeIndexInfo = series.info;
  planeIndexInfo.numChannels = effectiveChannels;

  for (size_t planeOffset = 0; planeOffset < planeCount; ++planeOffset) {
    const size_t ifdIndex = ifd + planeOffset;
    index_t z = 0;
    index_t effectiveC = 0;
    index_t tIndex = 0;
    index_t l = 0;
    if (!IFDToLoc(ifdIndex,
                  z,
                  effectiveC,
                  tIndex,
                  l,
                  ifd,
                  series.dimensionOrder,
                  planeIndexInfo,
                  1,
                  checkedIndex("OME-TIFF FirstZ", firstZ),
                  checkedIndex("OME-TIFF FirstC", firstEffectiveC),
                  checkedIndex("OME-TIFF FirstT", firstT),
                  0)) {
      break;
    }

    const size_t c = checkedSize("OME-TIFF channel", effectiveC) * series.samplesPerPlane;
    const size_t planeIndex =
      checkedMul("OME-TIFF plane index",
                 checkedMul("OME-TIFF plane index", checkedSize("time", tIndex), effectiveChannels) +
                   checkedSize("effective channel", effectiveC),
                 series.info.depth) +
      checkedSize("z", z);
    if (planeIndex >= series.planes.size()) {
      throw ZException(fmt::format("OME-TIFF TiffData maps outside series {} plane table", seriesIndex));
    }

    PlaneSource& plane = series.planes[planeIndex];
    if (plane.valid) {
      throw ZException(fmt::format("OME-TIFF TiffData maps duplicate ownership for series {}, z={}, c={}, t={}",
                                   seriesIndex,
                                   checkedSize("OME-TIFF z", z),
                                   c,
                                   checkedSize("OME-TIFF time", tIndex)));
    }
    plane.filename = planeFilename;
    plane.ifdIndex = ifdIndex;
    plane.z = checkedSize("OME-TIFF z", z);
    plane.c = c;
    plane.t = checkedSize("OME-TIFF time", tIndex);
    plane.channelCount = series.samplesPerPlane;
    plane.valid = true;

    if (sameFilePath(planeFilename, m_currentFilename)) {
      m_ifdIdxPosMap[ifdIndex] = IFDPos(plane.z, plane.c, plane.t, seriesIndex);
    }
  }
}

void ZImgOmeTiff::parseChannel(QXmlStreamReader& xml, size_t seriesIndex)
{
  CHECK(seriesIndex < m_omeSeries.size());
  SeriesInfo& series = m_omeSeries[seriesIndex];
  const QXmlStreamAttributes attributes = xml.attributes();

  if (attributes.hasAttribute("SamplesPerPixel")) {
    const size_t samplesPerPixel = parseSizeAttribute(attributes, "SamplesPerPixel", 1);
    if (samplesPerPixel == 0) {
      throw ZException("OME-TIFF Channel SamplesPerPixel must be positive");
    }
    if (samplesPerPixel != series.samplesPerPlane) {
      const bool hasMappedPlane = std::any_of(series.planes.begin(), series.planes.end(), [](const PlaneSource& plane) {
        return plane.valid;
      });
      if (hasMappedPlane) {
        throw ZException("OME-TIFF SamplesPerPixel changed after TiffData planes were mapped");
      }
      if (series.info.numChannels % samplesPerPixel != 0) {
        throw ZException(fmt::format("OME-TIFF SamplesPerPixel {} is incompatible with SizeC {}",
                                     samplesPerPixel,
                                     series.info.numChannels));
      }
      series.samplesPerPlane = samplesPerPixel;
      const size_t effectiveChannels = series.info.numChannels / series.samplesPerPlane;
      series.planes.clear();
      series.planes.resize(checkedMul("plane count",
                                      checkedMul("plane count", series.info.depth, effectiveChannels),
                                      series.info.numTimes));
    }
  }

  if (attributes.hasAttribute("Name")) {
    series.info.channelNames.push_back(attributes.value("Name").toString().toStdString());
  }
  if (attributes.hasAttribute("Color")) {
    const uint32_t color = parseOmeChannelColor(attributes.value("Color").toString());
    col4 col = std::bit_cast<col4>(color);
    col.a = 255;
    std::swap(col.r, col.b);
    series.info.channelColors.push_back(col);
  }

  xml.skipCurrentElement();
}

void ZImgOmeTiff::createOmeSubBlocks(const QString& filename,
                                     ZTiff& tiff,
                                     std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks) const
{
  if (!subBlocks) {
    return;
  }

  subBlocks->clear();
  subBlocks->resize(m_omeSeries.size());

  std::map<QString, std::unique_ptr<ZTiff>> externalTiffs;
  auto sourceTiff = [&](const QString& sourceFilename) -> ZTiff& {
    if (sameFilePath(sourceFilename, m_currentFilename)) {
      return tiff;
    }
    return tiffForFile(externalTiffs, sourceFilename);
  };

  for (size_t seriesIndex = 0; seriesIndex < m_omeSeries.size(); ++seriesIndex) {
    const SeriesInfo& series = m_omeSeries[seriesIndex];
    const PlaneSource* firstPlane = nullptr;
    for (const PlaneSource& plane : series.planes) {
      if (plane.valid) {
        firstPlane = &plane;
        break;
      }
    }
    if (!firstPlane) {
      continue;
    }

    ZTiff& firstTiff = sourceTiff(firstPlane->filename);
    if (firstPlane->ifdIndex >= firstTiff.ifds().size()) {
      throw ZException(fmt::format("OME-TIFF series {} references IFD {} in '{}', but the file has {} IFDs",
                                   seriesIndex,
                                   firstPlane->ifdIndex,
                                   firstPlane->filename,
                                   firstTiff.ifds().size()));
    }
    const ZTiffIFD& firstIFD = firstTiff.ifds()[firstPlane->ifdIndex];
    addOmeBaseTiledSubBlocks(filename,
                             series.info,
                             seriesIndex,
                             tiffTileWidthOrDefault(firstIFD),
                             tiffTileHeightOrDefault(firstIFD),
                             (*subBlocks)[seriesIndex]);

    size_t previousResolutionRatio = 1;
    for (size_t subIFDIndex = 0; subIFDIndex < firstIFD.subIFDs().size(); ++subIFDIndex) {
      if (!firstIFD.subIFDs()[subIFDIndex].isReducedResolutionImage()) {
        VLOG(1) << fmt::format("Skipping OME-TIFF SubIFD {} for series {} because NewSubFileType does not mark "
                               "it as a reduced-resolution image",
                               subIFDIndex,
                               seriesIndex);
        continue;
      }
      ZImgInfo firstSubIFDInfo;
      firstTiff.readInfoFromIFD(firstIFD.subIFDs()[subIFDIndex], firstSubIFDInfo);
      const std::optional<size_t> xRatio = integerRatioForResolution(series.info.width, firstSubIFDInfo.width);
      const std::optional<size_t> yRatio = integerRatioForResolution(series.info.height, firstSubIFDInfo.height);
      if (!xRatio || !yRatio || *xRatio != *yRatio || *xRatio == 1) {
        VLOG(1) << fmt::format("Skipping OME-TIFF SubIFD {} for series {} because Atlas requires integer XY "
                               "downsample ratios with equal X/Y scaling (full {}x{}, resolution {}x{})",
                               subIFDIndex,
                               seriesIndex,
                               series.info.width,
                               series.info.height,
                               firstSubIFDInfo.width,
                               firstSubIFDInfo.height);
        continue;
      }
      if (*xRatio <= previousResolutionRatio) {
        VLOG(1) << fmt::format("Skipping OME-TIFF SubIFD {} for series {} because SubIFD levels must be ordered "
                               "largest-to-smallest",
                               subIFDIndex,
                               seriesIndex);
        continue;
      }
      previousResolutionRatio = *xRatio;

      ZImgInfo resolutionInfo = series.info;
      resolutionInfo.width = firstSubIFDInfo.width;
      resolutionInfo.height = firstSubIFDInfo.height;
      if (resolutionInfo.voxelSizeUnit != VoxelSizeUnit::none) {
        resolutionInfo.voxelSizeX *= static_cast<double>(*xRatio);
        resolutionInfo.voxelSizeY *= static_cast<double>(*yRatio);
      }
      resolutionInfo.createDefaultDescriptions();

      for (size_t tIndex = 0; tIndex < series.info.numTimes; ++tIndex) {
        for (size_t z = 0; z < series.info.depth; ++z) {
          std::vector<OmeSubIFDPlaneSource> subIFDPlanes;
          subIFDPlanes.reserve(series.info.numChannels);
          std::vector<uint8_t> channelsCovered(series.info.numChannels, 0);
          bool complete = true;
          size_t tileWidth = tiffTileWidthOrDefault(firstIFD.subIFDs()[subIFDIndex]);
          size_t tileHeight = tiffTileHeightOrDefault(firstIFD.subIFDs()[subIFDIndex]);

          for (const PlaneSource& plane : series.planes) {
            if (!plane.valid || plane.z != z || plane.t != tIndex) {
              continue;
            }
            ZTiff& planeTiff = sourceTiff(plane.filename);
            if (plane.ifdIndex >= planeTiff.ifds().size()) {
              complete = false;
              break;
            }
            const ZTiffIFD& planeIFD = planeTiff.ifds()[plane.ifdIndex];
            if (subIFDIndex >= planeIFD.subIFDs().size()) {
              complete = false;
              break;
            }
            if (!planeIFD.subIFDs()[subIFDIndex].isReducedResolutionImage()) {
              complete = false;
              break;
            }

            ZImgInfo subInfo;
            planeTiff.readInfoFromIFD(planeIFD.subIFDs()[subIFDIndex], subInfo);
            if (subInfo.width != resolutionInfo.width || subInfo.height != resolutionInfo.height ||
                subInfo.bytesPerVoxel != resolutionInfo.bytesPerVoxel ||
                subInfo.voxelFormat != resolutionInfo.voxelFormat || subInfo.numChannels != plane.channelCount) {
              complete = false;
              break;
            }

            tileWidth = std::min(tileWidth, tiffTileWidthOrDefault(planeIFD.subIFDs()[subIFDIndex]));
            tileHeight = std::min(tileHeight, tiffTileHeightOrDefault(planeIFD.subIFDs()[subIFDIndex]));
            subIFDPlanes.push_back({plane.filename, plane.ifdIndex, subIFDIndex, plane.c, plane.channelCount});
            for (size_t c = plane.c; c < plane.c + plane.channelCount && c < channelsCovered.size(); ++c) {
              channelsCovered[c] = 1;
            }
          }

          if (!complete || !std::all_of(channelsCovered.begin(), channelsCovered.end(), [](uint8_t covered) {
                return covered != 0;
              })) {
            VLOG(1) << fmt::format("Skipping OME-TIFF SubIFD {} for series {}, z={}, t={} because not every "
                                   "requested channel has a matching SubIFD",
                                   subIFDIndex,
                                   seriesIndex,
                                   z,
                                   tIndex);
            continue;
          }

          addOmePyramidTiledSubBlocks(series.info,
                                      z,
                                      tIndex,
                                      resolutionInfo,
                                      std::move(subIFDPlanes),
                                      *xRatio,
                                      *yRatio,
                                      1,
                                      tileWidth,
                                      tileHeight,
                                      (*subBlocks)[seriesIndex]);
        }
      }
    }
  }
}

QString ZImgOmeTiff::createOmeXml(const ZImgInfo& info, const QString& dimensionOrder)
{
  ZImgInfo normalizedInfo = info;
  normalizedInfo.createDefaultDescriptions();

  QString res;
  QXmlStreamWriter xml(&res);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
  xml.setCodec("UTF-8");
#endif
  xml.writeStartDocument();
  xml.writeComment("Warning: this comment is an OME-XML metadata block generated by Atlas. Do not edit unless the "
                   "OME-TIFF plane mapping is updated consistently.");

  xml.writeStartElement("OME");
  xml.writeAttribute("xmlns:xsd", "http://www.w3.org/2001/XMLSchema");
  xml.writeAttribute("xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance");
  xml.writeAttribute("xmlns", kOmeNamespace);
  xml.writeAttribute("xmlns:ROI", kRoiNamespace);
  xml.writeAttribute("xmlns:OME", kOmeNamespace);
  xml.writeAttribute("xmlns:BIN", kBinaryNamespace);
  xml.writeAttribute("xsi:schemaLocation", kOmeSchemaLocation);
  xml.writeAttribute("Creator", "Atlas");
  xml.writeAttribute("UUID", QString("urn:uuid:%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

  xml.writeStartElement("Image");
  xml.writeAttribute("ID", "Image:0");
  xml.writeAttribute("Name", "");
  xml.writeEmptyElement("Description");
  xml.writeStartElement("Pixels");
  xml.writeAttribute("ID", "Pixels:0");
  xml.writeAttribute("DimensionOrder", dimensionOrder);
  if (normalizedInfo.voxelFormat == VoxelFormat::Float) {
    if (normalizedInfo.bytesPerVoxel == 4) {
      xml.writeAttribute("Type", "float");
    } else if (normalizedInfo.bytesPerVoxel == 8) {
      xml.writeAttribute("Type", "double");
    } else {
      throw ZException(fmt::format("{} bytes float pixel?", normalizedInfo.bytesPerVoxel));
    }
  } else if (normalizedInfo.voxelFormat == VoxelFormat::Signed) {
    xml.writeAttribute("Type", QString("int%1").arg(normalizedInfo.bytesPerVoxel * 8));
  } else {
    xml.writeAttribute("Type", QString("uint%1").arg(normalizedInfo.bytesPerVoxel * 8));
  }
  xml.writeAttribute("SizeX", QString("%1").arg(normalizedInfo.width));
  xml.writeAttribute("SizeY", QString("%1").arg(normalizedInfo.height));
  xml.writeAttribute("SizeZ", QString("%1").arg(normalizedInfo.depth));
  xml.writeAttribute("SizeC", QString("%1").arg(normalizedInfo.numChannels));
  xml.writeAttribute("SizeT", QString("%1").arg(normalizedInfo.numTimes));
  xml.writeAttribute("BigEndian", std::endian::native == std::endian::big ? "true" : "false");
  xml.writeAttribute("Interleaved", "false");
  if (normalizedInfo.validBitCount > 0 && normalizedInfo.validBitCount <= normalizedInfo.bytesPerVoxel * 8) {
    xml.writeAttribute("SignificantBits", QString("%1").arg(normalizedInfo.validBitCount));
  }
  if (normalizedInfo.voxelSizeUnit != VoxelSizeUnit::none) {
    xml.writeAttribute("PhysicalSizeX", QString("%1").arg(normalizedInfo.voxelSizeXInUm()));
    xml.writeAttribute("PhysicalSizeY", QString("%1").arg(normalizedInfo.voxelSizeYInUm()));
    xml.writeAttribute("PhysicalSizeZ", QString("%1").arg(normalizedInfo.voxelSizeZInUm()));
    xml.writeAttribute("PhysicalSizeXUnit", QString::fromUtf8("\xC2\xB5m"));
    xml.writeAttribute("PhysicalSizeYUnit", QString::fromUtf8("\xC2\xB5m"));
    xml.writeAttribute("PhysicalSizeZUnit", QString::fromUtf8("\xC2\xB5m"));
  }

  for (size_t i = 0; i < normalizedInfo.numChannels; ++i) {
    xml.writeStartElement("Channel");
    xml.writeAttribute("ID", QString("Channel:0:%1").arg(i));
    xml.writeAttribute("Name", QString::fromStdString(normalizedInfo.channelNames[i]));
    col4 col = normalizedInfo.channelColors[i];
    col.a = 0;
    std::swap(col.r, col.b);
    const auto color = std::bit_cast<int32_t>(col);
    xml.writeAttribute("Color", QString("%1").arg(color));
    xml.writeEndElement();
  }

  QString internalDimensionOrder = dimensionOrder;
  if (internalDimensionOrder.size() != 5 || !internalDimensionOrder.startsWith("XY")) {
    throw ZException(fmt::format("Wrong OME-TIFF DimensionOrder for writing: {}", dimensionOrder));
  }
  internalDimensionOrder.remove(0, 2);
  internalDimensionOrder += "L";
  if (!isDimensionOrderValid(internalDimensionOrder)) {
    throw ZException(fmt::format("Wrong OME-TIFF DimensionOrder for writing: {}", dimensionOrder));
  }

  const size_t planeCount =
    checkedMul("OME-TIFF writer plane count",
               checkedMul("OME-TIFF writer plane count", normalizedInfo.depth, normalizedInfo.numChannels),
               normalizedInfo.numTimes);
  for (size_t ifdIndex = 0; ifdIndex < planeCount; ++ifdIndex) {
    index_t z = 0;
    index_t c = 0;
    index_t t = 0;
    index_t l = 0;
    if (!IFDToLoc(ifdIndex, z, c, t, l, 0, internalDimensionOrder, normalizedInfo, 1)) {
      throw ZException(fmt::format("Can not map OME-TIFF writer IFD {}", ifdIndex));
    }
    xml.writeEmptyElement("TiffData");
    xml.writeAttribute("IFD", QString("%1").arg(ifdIndex));
    xml.writeAttribute("FirstZ", QString("%1").arg(z));
    xml.writeAttribute("FirstC", QString("%1").arg(c));
    xml.writeAttribute("FirstT", QString("%1").arg(t));
    xml.writeAttribute("PlaneCount", "1");
  }

  xml.writeEndElement();
  xml.writeEndElement();
  xml.writeEndElement();

  xml.writeEndDocument();

  return res;
}

} // namespace nim
