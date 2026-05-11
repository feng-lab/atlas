#include "ztest.h"

#include "zbioformatsbridgeclient.h"
#include "zimgbioformats.h"
#include "zimginit.h"
#include "zimgio.h"
#include "zcpuinfo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <future>
#include <gflags/gflags.h>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

DECLARE_int32(atlas_bioformats_bridge_io_timeout_ms);
DECLARE_bool(atlas_bioformats_bridge_use_grpc);

namespace nim {

namespace bioformats_detail {

void createSubBlocksForTesting(const QString& filename,
                               const ZBioFormatsDatasetInfo& dataset,
                               const std::vector<ZImgInfo>& infos,
                               std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks);

} // namespace bioformats_detail

namespace {

constexpr int kBioFormatsBridgeTestIoTimeoutMs = 10 * 60 * 1000;

void configureBioFormatsTestBridge()
{
  // Production defaults wait indefinitely for bridge I/O because local file
  // reads may legitimately be long-running. Tests should fail before CTest's
  // broad per-case timeout so the error path includes bridge diagnostics. Keep
  // enough headroom for slower CI runners that may start the JVM cold.
  if (::FLAGS_atlas_bioformats_bridge_io_timeout_ms <= 0) {
    ::FLAGS_atlas_bioformats_bridge_io_timeout_ms = kBioFormatsBridgeTestIoTimeoutMs;
  }
}

QString platformJavaExecutableName()
{
#ifdef _WIN32
  return QStringLiteral("java.exe");
#else
  return QStringLiteral("java");
#endif
}

QString javaExecutableInJreDir(const QString& jreDir)
{
  return QDir(jreDir).filePath(QStringLiteral("bin/") + platformJavaExecutableName());
}

std::optional<QString> normalizeBundledJreDir(const QString& path)
{
  QDir dir(path);
  if (!dir.exists()) {
    return std::nullopt;
  }

  if (dir.exists(QStringLiteral("bin/") + platformJavaExecutableName())) {
    return dir.absolutePath();
  }

  const QString macHome = dir.filePath(QStringLiteral("Contents/Home"));
  QDir macHomeDir(macHome);
  if (macHomeDir.exists(QStringLiteral("bin/") + platformJavaExecutableName())) {
    return macHomeDir.absolutePath();
  }

  return std::nullopt;
}

std::optional<QString> bundledJreDir()
{
  const QDir thirdPartyBuild(QStringLiteral(ATLAS_THIRDPARTY_BUILD_DIR));
  const QString jreName = ZCpuInfo::instance().isX86_64 ? QStringLiteral("jre") : QStringLiteral("jre-arm");
  return normalizeBundledJreDir(thirdPartyBuild.filePath(jreName));
}

std::optional<std::string> bioFormatsRuntimeSkipReason()
{
  configureBioFormatsTestBridge();

  const QDir thirdPartyBuild(QStringLiteral(ATLAS_THIRDPARTY_BUILD_DIR));
  const QDir jarsDir(thirdPartyBuild.filePath(QStringLiteral("jars")));
  if (!jarsDir.exists("bioformats_package.jar")) {
    return fmt::format("missing {}", jarsDir.filePath("bioformats_package.jar"));
  }
  if (!jarsDir.exists("atlas-bioformats-bridge.jar")) {
    return fmt::format("missing {}", jarsDir.filePath("atlas-bioformats-bridge.jar"));
  }
  const std::optional<QString> jreDir = bundledJreDir();
  if (!jreDir) {
    return fmt::format("missing bundled JRE under {}", QStringLiteral(ATLAS_THIRDPARTY_BUILD_DIR).toStdString());
  }

  ZLogInit::instance("zbioformatstest");
  LOG(INFO) << "Bio-Formats test runtime: jreDIR=" << *jreDir << ", java=" << javaExecutableInJreDir(*jreDir)
            << ", jarsDIR=" << jarsDir.absolutePath() << ", useGrpc=" << ::FLAGS_atlas_bioformats_bridge_use_grpc;

  try {
    ZImgInit::instance("", *jreDir, jarsDir.absolutePath(), false);
    if (!ZImgBioFormats().supportRead()) {
      return "Bio-Formats runtime support is not available";
    }
  }
  catch (const std::exception& e) {
    return fmt::format("Bio-Formats runtime initialization failed: {}", e.what());
  }

  return std::nullopt;
}

struct BioFormatsBridgeTestTransport
{
  const char* name;
  bool useGrpc;
};

constexpr std::array<BioFormatsBridgeTestTransport, 2> kBioFormatsBridgeTestTransports = {
  BioFormatsBridgeTestTransport{"stdio", false},
  BioFormatsBridgeTestTransport{"grpc",  true },
};

class ScopedBioFormatsBridgeTestTransport
{
public:
  explicit ScopedBioFormatsBridgeTestTransport(const BioFormatsBridgeTestTransport& transport)
  {
    ::FLAGS_atlas_bioformats_bridge_use_grpc = transport.useGrpc;
    ZBioFormatsBridgeClient::resetInstanceForTesting();
  }

  ~ScopedBioFormatsBridgeTestTransport()
  {
    ZBioFormatsBridgeClient::resetInstanceForTesting();
  }
};

class ScopedBioFormatsBridgeDefaultTransport
{
public:
  ScopedBioFormatsBridgeDefaultTransport()
  {
    ::FLAGS_atlas_bioformats_bridge_use_grpc = true;
    ZBioFormatsBridgeClient::resetInstanceForTesting();
  }

  ~ScopedBioFormatsBridgeDefaultTransport()
  {
    ZBioFormatsBridgeClient::resetInstanceForTesting();
  }
};

template<typename Fn>
void runForEachBioFormatsBridgeTransport(Fn&& fn)
{
  for (const BioFormatsBridgeTestTransport& transport : kBioFormatsBridgeTestTransports) {
    SCOPED_TRACE(transport.name);
    const ScopedBioFormatsBridgeTestTransport scopedTransport(transport);
    if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
      GTEST_SKIP() << *reason;
    }
    fn();
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
  }
}

QString createFakeReaderFile(QTemporaryDir& dir, const QString& basename)
{
  const QString filename = basename.endsWith(QStringLiteral(".fake")) ? basename : basename + QStringLiteral(".fake");
  const QString path = dir.filePath(filename);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    throw std::runtime_error(
      fmt::format("failed to create FakeReader file '{}': {}", path.toStdString(), file.errorString().toStdString()));
  }
  return path;
}

size_t nonNegativeIndex(index_t value)
{
  CHECK(value >= 0);
  return static_cast<size_t>(value);
}

QString canonicalExistingPath(const QString& path)
{
  QFileInfo fi(path);
  CHECK(fi.exists());
  const QString canonicalPath = fi.canonicalFilePath();
  return canonicalPath.isEmpty() ? fi.absoluteFilePath() : canonicalPath;
}

class WarningLogCapture final : public google::LogSink
{
public:
  void clear()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_messages.clear();
  }

  [[nodiscard]] std::vector<std::string> messages() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_messages;
  }

  void send(google::LogSeverity severity,
            const char*,
            const char* baseFilename,
            int line,
            const google::LogMessageTime& time,
            const char* message,
            size_t messageLen) override
  {
    if (severity < google::GLOG_WARNING) {
      return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_messages.push_back(formatLogMessage(severity, baseFilename, line, time, message, messageLen));
  }

private:
  mutable std::mutex m_mutex;
  std::vector<std::string> m_messages;
};

class ScopedWarningLogCapture
{
public:
  ScopedWarningLogCapture()
  {
    addLogSink(&m_sink);
  }

  ~ScopedWarningLogCapture()
  {
    removeLogSink(&m_sink);
  }

  void clear()
  {
    m_sink.clear();
  }

  [[nodiscard]] std::vector<std::string> messages() const
  {
    return m_sink.messages();
  }

private:
  WarningLogCapture m_sink;
};

struct CorpusFailure
{
  QString canonicalPath;
  std::string message;
};

struct CorpusManifestFile
{
  QString relativePath;
  QString absolutePath;
};

struct CorpusDriverRule
{
  QString topLevelFormat;
  QStringList driverPathSuffixes;
  bool includeExtensionlessNonDocs = false;
};

struct CorpusFormatStats
{
  size_t manifestFiles = 0;
  size_t selectedDriverFiles = 0;
  size_t missingFiles = 0;
  size_t missingDriverFiles = 0;
  size_t openedDriverFiles = 0;
  size_t coveredDriverFiles = 0;
};

const std::vector<CorpusDriverRule>& corpusDriverRules()
{
  static const std::vector<CorpusDriverRule> kRules = {
    {QStringLiteral("AmiraMesh"), {QStringLiteral(".am")}},
    {QStringLiteral("BDV"), {QStringLiteral(".xml")}},
    {QStringLiteral("CV7000"), {QStringLiteral(".wpi")}},
    {QStringLiteral("CellH5"), {QStringLiteral(".ch5")}},
    {QStringLiteral("CellSens"), {QStringLiteral(".vsi")}},
    {QStringLiteral("Cellomics"), {QStringLiteral(".dib")}},
    {QStringLiteral("DCIMG"), {QStringLiteral(".dcimg")}},
    {QStringLiteral("DICOM"), {QStringLiteral(".dcm")}, true},
    {QStringLiteral("DV"), {QStringLiteral(".dv"), QStringLiteral(".r3d"), QStringLiteral(".r3d_d3d")}},
    {QStringLiteral("ECAT7"), {QStringLiteral(".v")}},
    {QStringLiteral("Flex"), {QStringLiteral(".flex")}},
    {QStringLiteral("Gatan"), {QStringLiteral(".dm3"), QStringLiteral(".dm4")}},
    {QStringLiteral("HCS"),
     {QStringLiteral(".dib"),
      QStringLiteral(".xdce"),
      QStringLiteral("/index.idx.xml"),
      QStringLiteral("/index.ref.xml"),
      QStringLiteral("/index.xml")}},
    {QStringLiteral("Hamamatsu-NDPI"), {QStringLiteral(".ndpi")}},
    {QStringLiteral("Hamamatsu-VMS"), {QStringLiteral(".vms")}},
    {QStringLiteral("ICS"), {QStringLiteral(".ics")}},
    {QStringLiteral("Imaris-IMS"), {QStringLiteral(".ims")}},
    {QStringLiteral("Imspector"), {QStringLiteral(".msr")}},
    {QStringLiteral("InCell2000"), {QStringLiteral(".xdce")}},
    {QStringLiteral("InCell3000"), {QStringLiteral(".frm")}},
    {QStringLiteral("JDCE"), {QStringLiteral(".jdce")}},
    {QStringLiteral("KLB"), {QStringLiteral(".klb")}},
    {QStringLiteral("LEO"), {QStringLiteral(".tif"), QStringLiteral(".tiff")}},
    {QStringLiteral("Leica-LIF"), {QStringLiteral(".lif")}},
    {QStringLiteral("Leica-SCN"), {QStringLiteral(".scn")}},
    {QStringLiteral("Leica-XLEF"), {QStringLiteral(".xlef")}},
    {QStringLiteral("MRC"),
     {QStringLiteral(".map"), QStringLiteral(".mrc"), QStringLiteral(".rec"), QStringLiteral(".st")}},
    {QStringLiteral("MetaXpress"), {QStringLiteral(".htd")}},
    {QStringLiteral("Metamorph"), {QStringLiteral(".nd")}},
    {QStringLiteral("Micro-Manager"),
     {QStringLiteral("/metadata.txt"), QStringLiteral("_metadata.txt"), QStringLiteral("/acqusition.xml")}},
    {QStringLiteral("ND2"), {QStringLiteral(".nd2")}},
    {QStringLiteral("NIfTI"), {QStringLiteral(".nii"), QStringLiteral(".nii.gz"), QStringLiteral(".hdr")}},
    {QStringLiteral("NRRD"), {QStringLiteral(".nhdr"), QStringLiteral(".nrrd")}},
    {QStringLiteral("OBF"), {QStringLiteral(".obf")}},
    {QStringLiteral("OME-TIFF"),
     {QStringLiteral(".ome.tif"),
      QStringLiteral(".ome.tiff"),
      QStringLiteral(".tif"),
      QStringLiteral(".tiff"),
      QStringLiteral(".btf"),
      QStringLiteral(".tf2"),
      QStringLiteral(".tf8")}},
    {QStringLiteral("OME-XML"), {QStringLiteral(".ome.xml")}},
    {QStringLiteral("Olympus-FluoView"), {QStringLiteral(".oib"), QStringLiteral(".oif")}},
    {QStringLiteral("Olympus-OIR"), {QStringLiteral(".oir")}},
    {QStringLiteral("PNG"), {QStringLiteral(".png")}},
    {QStringLiteral("PerkinElmer-Columbus"), {QStringLiteral("/measurementindex.columbusidx.xml")}},
    {QStringLiteral("PerkinElmer-Operetta"),
     {QStringLiteral("/index.idx.xml"), QStringLiteral("/index.ref.xml"), QStringLiteral("/index.xml")}},
    {QStringLiteral("SDT"), {QStringLiteral(".sdt")}},
    {QStringLiteral("SPC-FIFO"), {QStringLiteral(".set")}},
    {QStringLiteral("SVS"), {QStringLiteral(".svs")}},
    {QStringLiteral("ScanR"), {QStringLiteral("/experiment_descriptor.xml")}},
    {QStringLiteral("TIFF"), {QStringLiteral(".tif"), QStringLiteral(".tiff"), QStringLiteral(".g3")}},
    {QStringLiteral("Trestle"), {QStringLiteral(".tif"), QStringLiteral(".tiff")}},
    {QStringLiteral("Vectra-QPTIFF"), {QStringLiteral(".qptiff")}},
    {QStringLiteral("Ventana"), {QStringLiteral(".bif")}},
    {QStringLiteral("Zeiss-CZI"), {QStringLiteral(".czi")}},
    {QStringLiteral("gateway_tests"), {QStringLiteral(".dv"), QStringLiteral(".tiff"), QStringLiteral(".tif")}},
    {QStringLiteral("u-track"), {QStringLiteral(".zip")}},
  };
  return kRules;
}

QString topLevelFormat(const QString& relativePath)
{
  const int separator = relativePath.indexOf(QLatin1Char('/'));
  return separator < 0 ? relativePath : relativePath.left(separator);
}

bool isKnownNonImageDocument(const QString& relativePath)
{
  const QString name = QFileInfo(relativePath).fileName().toLower();
  return name == QStringLiteral("copying") || name == QStringLiteral("license") ||
         name == QStringLiteral("license.txt") || name == QStringLiteral("readme") ||
         name == QStringLiteral("readme.txt");
}

bool hasNoFileExtension(const QString& relativePath)
{
  return !QFileInfo(relativePath).fileName().contains(QLatin1Char('.'));
}

std::optional<bool> isCorpusDriverFile(const QString& relativePath)
{
  const QString topLevel = topLevelFormat(relativePath);
  const auto& rules = corpusDriverRules();
  const auto it = std::ranges::find_if(rules, [&topLevel](const CorpusDriverRule& rule) {
    return rule.topLevelFormat == topLevel;
  });
  if (it == rules.end()) {
    return std::nullopt;
  }

  const QString lowerRelativePath = relativePath.toLower();
  for (const QString& suffix : it->driverPathSuffixes) {
    if (lowerRelativePath.endsWith(suffix)) {
      return true;
    }
  }
  if (it->includeExtensionlessNonDocs && hasNoFileExtension(relativePath) && !isKnownNonImageDocument(relativePath)) {
    return true;
  }
  return false;
}

void expectSameRegionPixels(const ZImg& full, const ZImg& region, const ZImgRegion& sourceRegion)
{
  ASSERT_EQ(sourceRegion.end.x - sourceRegion.start.x, region.sWidth());
  ASSERT_EQ(sourceRegion.end.y - sourceRegion.start.y, region.sHeight());
  ASSERT_EQ(sourceRegion.end.z - sourceRegion.start.z, region.sDepth());
  ASSERT_EQ(sourceRegion.end.c - sourceRegion.start.c, region.sNumChannels());
  ASSERT_EQ(sourceRegion.end.t - sourceRegion.start.t, region.sNumTimes());

  for (index_t t = 0; t < region.sNumTimes(); ++t) {
    for (index_t c = 0; c < region.sNumChannels(); ++c) {
      for (index_t z = 0; z < region.sDepth(); ++z) {
        for (index_t y = 0; y < region.sHeight(); ++y) {
          for (index_t x = 0; x < region.sWidth(); ++x) {
            EXPECT_EQ(*full.data<uint16_t>(nonNegativeIndex(sourceRegion.start.x + x),
                                           nonNegativeIndex(sourceRegion.start.y + y),
                                           nonNegativeIndex(sourceRegion.start.z + z),
                                           nonNegativeIndex(sourceRegion.start.c + c),
                                           nonNegativeIndex(sourceRegion.start.t + t)),
                      *region.data<uint16_t>(nonNegativeIndex(x),
                                             nonNegativeIndex(y),
                                             nonNegativeIndex(z),
                                             nonNegativeIndex(c),
                                             nonNegativeIndex(t)))
              << "x=" << x << ", y=" << y << ", z=" << z << ", c=" << c << ", t=" << t;
          }
        }
      }
    }
  }
}

std::vector<CorpusManifestFile> corpusFilesFromManifest(const QString& rootPath)
{
  const QString manifestPath = QDir(rootPath).filePath(QStringLiteral("manifest.json"));
  QFile manifestFile(manifestPath);
  if (!manifestFile.exists()) {
    throw std::runtime_error(fmt::format("Bio-Formats corpus manifest is missing: {}", manifestPath.toStdString()));
  }
  if (!manifestFile.open(QIODevice::ReadOnly)) {
    throw std::runtime_error(fmt::format("failed to open {}", manifestPath.toStdString()));
  }

  QJsonParseError error;
  const QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject()) {
    throw std::runtime_error(
      fmt::format("failed to parse {}: {}", manifestPath.toStdString(), error.errorString().toStdString()));
  }

  const QJsonValue samplesValue = doc.object().value(QStringLiteral("samples"));
  if (!samplesValue.isArray()) {
    throw std::runtime_error(
      fmt::format("Bio-Formats corpus manifest samples must be an array: {}", manifestPath.toStdString()));
  }

  std::vector<CorpusManifestFile> files;
  const QJsonArray samples = samplesValue.toArray();
  if (samples.isEmpty()) {
    throw std::runtime_error(fmt::format("Bio-Formats corpus manifest has no samples: {}", manifestPath.toStdString()));
  }
  files.reserve(static_cast<size_t>(samples.size()));
  for (qsizetype index = 0; index < samples.size(); ++index) {
    const QJsonValue& value = samples[index];
    if (!value.isObject()) {
      throw std::runtime_error(fmt::format("Bio-Formats corpus manifest sample {} must be an object in {}",
                                           index + 1,
                                           manifestPath.toStdString()));
    }
    const QJsonObject object = value.toObject();
    const QString relativePath = object.value(QStringLiteral("relative_path")).toString();
    if (relativePath.isEmpty()) {
      throw std::runtime_error(fmt::format("Bio-Formats corpus manifest sample {} is missing relative_path in {}",
                                           index + 1,
                                           manifestPath.toStdString()));
    }
    files.push_back({relativePath, QDir(rootPath).filePath(relativePath)});
  }
  return files;
}

std::vector<CorpusManifestFile> publicCorpusFiles()
{
  const QByteArray root = qgetenv("ATLAS_BIOFORMATS_BREADTH_DIR");
  if (root.isEmpty()) {
    return {};
  }
  const QString rootPath = QString::fromUtf8(root);
  if (!QDir(rootPath).exists()) {
    throw std::runtime_error(fmt::format("ATLAS_BIOFORMATS_BREADTH_DIR does not exist: {}", rootPath.toStdString()));
  }
  std::vector<CorpusManifestFile> files = corpusFilesFromManifest(rootPath);
  std::sort(files.begin(), files.end(), [](const CorpusManifestFile& lhs, const CorpusManifestFile& rhs) {
    return lhs.relativePath < rhs.relativePath;
  });
  return files;
}

std::vector<ZImgRegion> smokeRegions(const ZImgInfo& info)
{
  static constexpr index_t kSmokeTileSize = 16;
  std::vector<ZImgRegion> regions;
  auto addRegion = [&](index_t x0, index_t y0, index_t z0, index_t t0, index_t width, index_t height) {
    regions.emplace_back(ZVoxelCoordinate(x0, y0, z0, 0, t0),
                         ZVoxelCoordinate(std::min<index_t>(info.sWidth(), x0 + width),
                                          std::min<index_t>(info.sHeight(), y0 + height),
                                          z0 + 1,
                                          info.sNumChannels(),
                                          t0 + 1));
  };

  addRegion(0,
            0,
            0,
            0,
            std::min<index_t>(kSmokeTileSize, info.sWidth()),
            std::min<index_t>(kSmokeTileSize, info.sHeight()));
  if (info.sWidth() > kSmokeTileSize || info.sHeight() > kSmokeTileSize) {
    addRegion(std::max<index_t>(0, info.sWidth() / 2 - kSmokeTileSize / 2),
              std::max<index_t>(0, info.sHeight() / 2 - kSmokeTileSize / 2),
              0,
              0,
              std::min<index_t>(kSmokeTileSize, info.sWidth()),
              std::min<index_t>(kSmokeTileSize, info.sHeight()));
  }
  if (info.sDepth() > 1 || info.sNumTimes() > 1) {
    addRegion(0, 0, info.sDepth() - 1, info.sNumTimes() - 1, 1, 1);
  }
  return regions;
}

} // namespace

TEST(ZBioFormatsTest, FormatExtensionListDoesNotRequireAnOpenFile)
{
  runForEachBioFormatsBridgeTransport([]() {
    const QStringList extensions = ZImgBioFormats().extensions();
    ASSERT_FALSE(extensions.empty());
    EXPECT_TRUE(extensions.contains(QStringLiteral("fake"), Qt::CaseInsensitive));
  });
}

TEST(ZBioFormatsTest, FakeReaderMetadataCreatesDeterministicInfoAndSubBlocks)
{
  runForEachBioFormatsBridgeTransport([]() {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path =
      createFakeReaderFile(dir, QStringLiteral("metadata&sizeX=7&sizeY=5&sizeZ=3&sizeC=2&sizeT=4&pixelType=uint8"));

    std::vector<ZImgInfo> infos;
    std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
    ZImgIO::instance().readInfos(path, infos, &subBlocks, FileFormat::BioFormats);

    ASSERT_EQ(1u, infos.size());
    const ZImgInfo& info = infos.front();
    EXPECT_EQ(7u, info.width);
    EXPECT_EQ(5u, info.height);
    EXPECT_EQ(3u, info.depth);
    EXPECT_EQ(2u, info.numChannels);
    EXPECT_EQ(4u, info.numTimes);
    EXPECT_TRUE(info.isType<uint8_t>());

    ASSERT_EQ(infos.size(), subBlocks.size());
    ASSERT_FALSE(subBlocks.front().empty());
    const std::shared_ptr<ZImgSubBlock>& firstBlock = subBlocks.front().front();
    ASSERT_NE(nullptr, firstBlock);
    EXPECT_EQ(0, firstBlock->x);
    EXPECT_EQ(0, firstBlock->y);
    EXPECT_EQ(0, firstBlock->z);
    EXPECT_EQ(0, firstBlock->t);
    EXPECT_EQ(7, firstBlock->width);
    EXPECT_EQ(5, firstBlock->height);
    EXPECT_EQ(1, firstBlock->depth);
  });
}

TEST(ZBioFormatsTest, IndexedFalseColorDataStaysSingleChannel)
{
  runForEachBioFormatsBridgeTransport([]() {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = createFakeReaderFile(
      dir,
      QStringLiteral(
        "indexed&sizeX=9&sizeY=7&sizeZ=1&sizeC=1&sizeT=1&pixelType=uint8&indexed=true&falseColor=true&lutLength=256"));

    std::vector<ZImgInfo> infos;
    ZImgIO::instance().readInfos(path, infos, nullptr, FileFormat::BioFormats);

    ASSERT_EQ(1u, infos.size());
    EXPECT_EQ(1u, infos.front().numChannels);
    EXPECT_TRUE(infos.front().isType<uint8_t>());

    const ZImg img(path, ZImgRegion(), 0, 1, 1, 1, FileFormat::BioFormats);
    EXPECT_EQ(1u, img.numChannels());
    EXPECT_TRUE(img.info().isType<uint8_t>());
  });
}

TEST(ZBioFormatsTest, ChannelColorMetadataIsPreserved)
{
  runForEachBioFormatsBridgeTransport([]() {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = createFakeReaderFile(
      dir,
      QStringLiteral("color&sizeX=9&sizeY=7&sizeZ=1&sizeC=1&sizeT=1&pixelType=uint8&color=0x00ff00ff"));

    std::vector<ZImgInfo> infos;
    ZImgIO::instance().readInfos(path, infos, nullptr, FileFormat::BioFormats);

    ASSERT_EQ(1u, infos.size());
    ASSERT_EQ(1u, infos.front().channelColors.size());
    EXPECT_EQ(0, infos.front().channelColors[0].r);
    EXPECT_EQ(255, infos.front().channelColors[0].g);
    EXPECT_EQ(0, infos.front().channelColors[0].b);
    EXPECT_EQ(255, infos.front().channelColors[0].a);
  });
}

TEST(ZBioFormatsTest, FakeReaderRegionMatchesTheSameCoordinatesFromFullRead)
{
  runForEachBioFormatsBridgeTransport([]() {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path =
      createFakeReaderFile(dir, QStringLiteral("region&sizeX=13&sizeY=11&sizeZ=3&sizeC=2&sizeT=2&pixelType=uint16"));

    ZImg full(path, ZImgRegion(), 0, 1, 1, 1, FileFormat::BioFormats);
    ASSERT_TRUE(full.info().isType<uint16_t>());
    ASSERT_EQ(13u, full.width());
    ASSERT_EQ(11u, full.height());
    ASSERT_EQ(3u, full.depth());
    ASSERT_EQ(2u, full.numChannels());
    ASSERT_EQ(2u, full.numTimes());

    const ZImgRegion sourceRegion(2, 9, 3, 8, 1, 3, 0, 2, 1, 2);
    ZImg region;
    ZImgIO::instance().readImg(path, region, sourceRegion, 0, 1, 1, 1, FileFormat::BioFormats);

    ASSERT_TRUE(region.info().isType<uint16_t>());
    expectSameRegionPixels(full, region, sourceRegion);
  });
}

TEST(ZBioFormatsTest, FakeReaderReadReassemblesMultiplePixelChunks)
{
  runForEachBioFormatsBridgeTransport([]() {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path =
      createFakeReaderFile(dir, QStringLiteral("chunks&sizeX=4097&sizeY=2048&sizeZ=1&sizeC=1&sizeT=1&pixelType=uint8"));

    ZImg img(path, ZImgRegion(), 0, 1, 1, 1, FileFormat::BioFormats);

    ASSERT_TRUE(img.info().isType<uint8_t>());
    EXPECT_EQ(4097u, img.width());
    EXPECT_EQ(2048u, img.height());
    EXPECT_EQ(4097u * 2048u, img.byteNumber());
    EXPECT_GT(img.byteNumber(), 8u * 1024u * 1024u);

    const auto boundary = *img.data<uint8_t>(4096, 2047);
    const auto reread = ZImg(path, ZImgRegion(4096, 4097, 2047, 2048), 0, 1, 1, 1, FileFormat::BioFormats);
    ASSERT_TRUE(reread.info().isType<uint8_t>());
    ASSERT_EQ(1u, reread.width());
    ASSERT_EQ(1u, reread.height());
    EXPECT_EQ(boundary, *reread.data<uint8_t>(0, 0));
  });
}

TEST(ZBioFormatsTest, FakeReaderGrpcConcurrentRegionReads)
{
  const BioFormatsBridgeTestTransport grpcTransport{"grpc", true};
  const ScopedBioFormatsBridgeTestTransport scopedTransport(grpcTransport);
  if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
    GTEST_SKIP() << *reason;
  }

  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path =
    createFakeReaderFile(dir, QStringLiteral("concurrent&sizeX=64&sizeY=48&sizeZ=4&sizeC=2&sizeT=2&pixelType=uint16"));

  const ZImgRegion regionA(0, 32, 0, 24, 0, 1, 0, 2, 0, 1);
  const ZImgRegion regionB(16, 48, 8, 40, 1, 3, 0, 1, 0, 2);
  const ZImgRegion regionC(32, 64, 16, 48, 3, 4, 1, 2, 1, 2);

  ZBioFormatsBridgeClient& client = ZBioFormatsBridgeClient::instance();
  const std::vector<uint8_t> expectedA = client.readRegion(path, 0, regionA);
  const std::vector<uint8_t> expectedB = client.readRegion(path, 0, regionB);
  const std::vector<uint8_t> expectedC = client.readRegion(path, 0, regionC);

  auto readRegionAsync = [path](ZImgRegion region) {
    return std::async(std::launch::async, [path, region]() {
      return ZBioFormatsBridgeClient::instance().readRegion(path, 0, region);
    });
  };

  std::future<std::vector<uint8_t>> actualA = readRegionAsync(regionA);
  std::future<std::vector<uint8_t>> actualB = readRegionAsync(regionB);
  std::future<std::vector<uint8_t>> actualC = readRegionAsync(regionC);

  EXPECT_EQ(expectedA, actualA.get());
  EXPECT_EQ(expectedB, actualB.get());
  EXPECT_EQ(expectedC, actualC.get());
}

TEST(ZBioFormatsTest, FakeReaderResolutionMetadataCreatesPyramidSubBlocks)
{
  runForEachBioFormatsBridgeTransport([]() {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = createFakeReaderFile(
      dir,
      QStringLiteral(
        "pyramid&sizeX=64&sizeY=48&sizeZ=1&sizeC=1&sizeT=1&pixelType=uint8&resolutions=3&resolutionScale=2"));

    const ZBioFormatsDatasetInfo dataset = ZBioFormatsBridgeClient::instance().readDatasetInfo(path);
    ASSERT_EQ(1u, dataset.series.size());
    ASSERT_GE(dataset.series.front().resolutions.size(), 3u);
    EXPECT_EQ(64u, dataset.series.front().resolutions[0].sizeX);
    EXPECT_EQ(48u, dataset.series.front().resolutions[0].sizeY);
    EXPECT_EQ(32u, dataset.series.front().resolutions[1].sizeX);
    EXPECT_EQ(24u, dataset.series.front().resolutions[1].sizeY);
    EXPECT_EQ(16u, dataset.series.front().resolutions[2].sizeX);
    EXPECT_EQ(12u, dataset.series.front().resolutions[2].sizeY);

    std::vector<ZImgInfo> infos;
    std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
    ZImgIO::instance().readInfos(path, infos, &subBlocks, FileFormat::BioFormats);
    ASSERT_EQ(1u, subBlocks.size());
    EXPECT_TRUE(std::ranges::any_of(subBlocks.front(), [](const std::shared_ptr<ZImgSubBlock>& block) {
      return block && block->xRatio > 1 && block->yRatio > 1;
    }));
  });
}

TEST(ZBioFormatsTest, ZDownsampledResolutionCreatesPyramidSubBlocks)
{
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = createFakeReaderFile(dir, QStringLiteral("synthetic-z-downsample"));

  ZImgInfo info;
  info.width = 5;
  info.height = 5;
  info.depth = 5;
  info.numChannels = 1;
  info.numTimes = 1;
  info.setVoxelFormat<uint8_t>();
  info.createDefaultDescriptions();

  ZBioFormatsSeriesInfo series;
  series.sizeX = info.width;
  series.sizeY = info.height;
  series.sizeZ = info.depth;
  series.effectiveSizeC = info.numChannels;
  series.sizeT = info.numTimes;
  series.rgbChannelCount = 1;
  series.bytesPerPixel = 1;
  series.pixelType = QStringLiteral("uint8");
  series.resolutionCount = 2;
  series.optimalTileWidth = 512;
  series.optimalTileHeight = 512;

  ZBioFormatsResolutionInfo baseResolution;
  baseResolution.resolution = 0;
  baseResolution.sizeX = info.width;
  baseResolution.sizeY = info.height;
  baseResolution.sizeZ = info.depth;
  baseResolution.effectiveSizeC = info.numChannels;
  baseResolution.sizeT = info.numTimes;
  baseResolution.imageCount = info.depth;
  series.resolutions.push_back(baseResolution);

  ZBioFormatsResolutionInfo downsampledResolution;
  downsampledResolution.resolution = 1;
  downsampledResolution.sizeX = 2;
  downsampledResolution.sizeY = 2;
  downsampledResolution.sizeZ = 2;
  downsampledResolution.effectiveSizeC = info.numChannels;
  downsampledResolution.sizeT = info.numTimes;
  downsampledResolution.imageCount = 2;
  downsampledResolution.optimalTileWidth = 1;
  downsampledResolution.optimalTileHeight = 1;
  series.resolutions.push_back(downsampledResolution);

  ZBioFormatsDatasetInfo dataset;
  dataset.series.push_back(series);

  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  bioformats_detail::createSubBlocksForTesting(path, dataset, {info}, &subBlocks);

  ASSERT_EQ(1u, subBlocks.size());

  std::vector<const ZImgSubBlock*> zDownsampledBlocks;
  for (const auto& block : subBlocks.front()) {
    if (block && block->xRatio == 2 && block->yRatio == 2 && block->zRatio == 2) {
      zDownsampledBlocks.push_back(block.get());
    }
  }

  ASSERT_EQ(8u, zDownsampledBlocks.size());
  EXPECT_TRUE(std::ranges::any_of(zDownsampledBlocks, [](const ZImgSubBlock* block) {
    return block->x == 0 && block->y == 0 && block->z == 0 && block->width == 2 && block->height == 2 &&
           block->depth == 2;
  }));
  EXPECT_TRUE(std::ranges::any_of(zDownsampledBlocks, [](const ZImgSubBlock* block) {
    return block->x == 2 && block->y == 2 && block->z == 2 && block->width == 2 && block->height == 2 &&
           block->depth == 2;
  }));
  EXPECT_FALSE(std::ranges::any_of(zDownsampledBlocks, [](const ZImgSubBlock* block) {
    return block->z >= 4;
  }));
}

TEST(ZBioFormatsTest, FakeReaderThumbnailIsReadWhenAvailable)
{
  runForEachBioFormatsBridgeTransport([]() {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = createFakeReaderFile(
      dir,
      QStringLiteral("thumbnail&sizeX=64&sizeY=48&sizeZ=1&sizeC=1&sizeT=1&pixelType=uint8&thumbSizeX=8&thumbSizeY=6"));

    ZImgThumbernail thumbnail = ZImg::readImgThumbnail(path, ZImgRegion(), 0, FileFormat::BioFormats);
    ASSERT_TRUE(thumbnail.hasPlaneAttachment(0, 0));
    const std::vector<ZImg>& thumbnails = thumbnail.planeAttachments(0, 0);
    ASSERT_FALSE(thumbnails.empty());
    EXPECT_EQ(8u, thumbnails.front().width());
    EXPECT_EQ(6u, thumbnails.front().height());
    EXPECT_EQ(1u, thumbnails.front().numChannels());
    EXPECT_TRUE(thumbnails.front().info().isType<uint8_t>());
  });
}

TEST(ZBioFormatsTest, PublicCorpusReadsMetadataAndSmallRegions)
{
  const std::vector<CorpusManifestFile> files = publicCorpusFiles();
  if (files.empty()) {
    GTEST_SKIP() << "set ATLAS_BIOFORMATS_BREADTH_DIR to run public Bio-Formats corpus smoke tests";
  }

  const ScopedBioFormatsBridgeDefaultTransport scopedTransport;
  if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
    GTEST_SKIP() << *reason;
  }

  ScopedWarningLogCapture warningCapture;
  std::set<QString> coveredCompanionFiles;
  std::set<QString> formatsWithoutDriverRules;
  std::set<QString> formatsWithoutSelectedDrivers;
  std::set<QString> formatsWithoutCheckedDrivers;
  std::map<QString, CorpusFormatStats> formatStats;
  size_t openedFiles = 0;
  size_t missingFiles = 0;
  size_t selectedDriverFiles = 0;
  size_t skippedNonDriverFiles = 0;
  size_t skippedCoveredCompanionFiles = 0;
  std::vector<CorpusFailure> openFailureRecords;
  std::vector<CorpusFailure> regionFailureRecords;
  std::vector<CorpusFailure> warningFailureRecords;
  for (const CorpusManifestFile& file : files) {
    const QString& path = file.absolutePath;
    const QString format = topLevelFormat(file.relativePath);
    CorpusFormatStats& stats = formatStats[format];
    ++stats.manifestFiles;
    const std::optional<bool> isDriver = isCorpusDriverFile(file.relativePath);
    if (isDriver.has_value() && *isDriver) {
      ++selectedDriverFiles;
      ++stats.selectedDriverFiles;
    }

    SCOPED_TRACE(file.relativePath.toStdString());
    if (!QFileInfo(path).exists()) {
      ++missingFiles;
      ++stats.missingFiles;
      if (isDriver.has_value() && *isDriver) {
        ++stats.missingDriverFiles;
      }
      ADD_FAILURE() << "Bio-Formats corpus manifest entry is missing on disk: " << file.relativePath.toStdString()
                    << " (" << path.toStdString() << ")";
      continue;
    }

    if (!isDriver.has_value()) {
      formatsWithoutDriverRules.insert(format);
      ++skippedNonDriverFiles;
      continue;
    }
    if (!*isDriver) {
      ++skippedNonDriverFiles;
      continue;
    }

    const QString canonicalPath = canonicalExistingPath(path);
    if (coveredCompanionFiles.contains(canonicalPath)) {
      ++skippedCoveredCompanionFiles;
      ++stats.coveredDriverFiles;
      continue;
    }

    warningCapture.clear();
    ZBioFormatsDatasetInfo dataset;
    std::vector<ZImgInfo> infos;
    try {
      dataset = ZBioFormatsBridgeClient::instance().readDatasetInfo(path);
      ZImgIO::instance().readInfos(path, infos, nullptr, FileFormat::BioFormats);
    }
    catch (const std::exception& e) {
      openFailureRecords.push_back(
        {canonicalPath, fmt::format("failed to open Bio-Formats corpus file {}: {}", path.toStdString(), e.what())});
      continue;
    }
    if (infos.empty()) {
      openFailureRecords.push_back(
        {canonicalPath, fmt::format("Bio-Formats corpus file has no readable series: {}", path.toStdString())});
      continue;
    }
    ++openedFiles;
    ++stats.openedDriverFiles;

    for (size_t scene = 0; scene < infos.size(); ++scene) {
      SCOPED_TRACE(fmt::format("scene {}", scene));
      for (const ZImgRegion& region : smokeRegions(infos[scene])) {
        ZImg img;
        try {
          ZImgIO::instance().readImg(path, img, region, scene, 1, 1, 1, FileFormat::BioFormats);
        }
        catch (const std::exception& e) {
          regionFailureRecords.push_back(
            {canonicalPath,
             fmt::format("failed to read Bio-Formats corpus region from {}: {}", path.toStdString(), e.what())});
          continue;
        }
        EXPECT_FALSE(img.isEmpty());
      }
    }

    const std::vector<std::string> warningMessages = warningCapture.messages();
    if (!warningMessages.empty()) {
      warningFailureRecords.push_back({canonicalPath,
                                       fmt::format("warnings/errors while reading Bio-Formats corpus file {}:\n{}",
                                                   path.toStdString(),
                                                   fmt::join(warningMessages, "\n"))});
    }

    for (const QString& usedFile : dataset.usedFiles) {
      if (!QFileInfo(usedFile).exists()) {
        continue;
      }
      const QString usedPath = canonicalExistingPath(usedFile);
      if (usedPath != canonicalPath) {
        coveredCompanionFiles.insert(usedPath);
      }
    }
  }

  size_t suppressedCoveredCompanionFailures = 0;
  auto reportFailures = [&](const std::vector<CorpusFailure>& failures) {
    size_t reportedFailures = 0;
    for (const CorpusFailure& failure : failures) {
      if (coveredCompanionFiles.contains(failure.canonicalPath)) {
        ++suppressedCoveredCompanionFailures;
        continue;
      }
      ++reportedFailures;
      ADD_FAILURE() << failure.message;
    }
    return reportedFailures;
  };
  const size_t openFailures = reportFailures(openFailureRecords);
  const size_t regionFailures = reportFailures(regionFailureRecords);
  const size_t warningFailures = reportFailures(warningFailureRecords);

  for (const QString& format : formatsWithoutDriverRules) {
    ADD_FAILURE() << "Bio-Formats corpus top-level format has no driver rule: " << format.toStdString();
  }
  for (const auto& [format, stats] : formatStats) {
    if (formatsWithoutDriverRules.contains(format)) {
      continue;
    }
    if (stats.selectedDriverFiles == 0) {
      formatsWithoutSelectedDrivers.insert(format);
      ADD_FAILURE() << "Bio-Formats corpus top-level format selected no driver files: " << format.toStdString()
                    << " (manifest entries: " << stats.manifestFiles
                    << "). Fix the driver rule or include at least one real driver in manifest.json.";
      continue;
    }
    if (stats.missingDriverFiles == 0 && stats.openedDriverFiles + stats.coveredDriverFiles == 0) {
      formatsWithoutCheckedDrivers.insert(format);
      ADD_FAILURE() << "Bio-Formats corpus top-level format checked no driver files: " << format.toStdString()
                    << " (selected drivers: " << stats.selectedDriverFiles
                    << "). Selected drivers must either open or be covered by another opened driver.";
    }
  }

  RecordProperty("files", static_cast<int>(files.size()));
  RecordProperty("missing_files", static_cast<int>(missingFiles));
  RecordProperty("selected_driver_files", static_cast<int>(selectedDriverFiles));
  RecordProperty("skipped_non_driver_files", static_cast<int>(skippedNonDriverFiles));
  RecordProperty("opened_files", static_cast<int>(openedFiles));
  RecordProperty("skipped_covered_companion_files", static_cast<int>(skippedCoveredCompanionFiles));
  RecordProperty("suppressed_covered_companion_failures", static_cast<int>(suppressedCoveredCompanionFailures));
  RecordProperty("formats_without_driver_rules", static_cast<int>(formatsWithoutDriverRules.size()));
  RecordProperty("formats_without_selected_drivers", static_cast<int>(formatsWithoutSelectedDrivers.size()));
  RecordProperty("formats_without_checked_drivers", static_cast<int>(formatsWithoutCheckedDrivers.size()));
  RecordProperty("open_failures", static_cast<int>(openFailures));
  RecordProperty("region_failures", static_cast<int>(regionFailures));
  RecordProperty("warning_failures", static_cast<int>(warningFailures));
  EXPECT_GT(openedFiles, 0u);
  EXPECT_EQ(0u, missingFiles);
  EXPECT_TRUE(formatsWithoutDriverRules.empty());
  EXPECT_TRUE(formatsWithoutSelectedDrivers.empty());
  EXPECT_TRUE(formatsWithoutCheckedDrivers.empty());
  EXPECT_EQ(0u, openFailures);
  EXPECT_EQ(0u, regionFailures);
  EXPECT_EQ(0u, warningFailures);
}

} // namespace nim
