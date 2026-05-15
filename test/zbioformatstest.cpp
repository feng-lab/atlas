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
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <gflags/gflags.h>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

DECLARE_int32(atlas_bioformats_bridge_io_timeout_ms);

namespace nim {

namespace bioformats_detail {

void createSubBlocksForTesting(const QString& filename,
                               const ZBioFormatsDatasetInfo& dataset,
                               const std::vector<ZImgInfo>& infos,
                               std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks);

} // namespace bioformats_detail

namespace {

constexpr int kBioFormatsBridgeTestIoTimeoutMs = 10 * 60 * 1000;
constexpr size_t kDefaultPublicCorpusProgressInterval = 250;
constexpr size_t kDefaultCorpusSceneSampleLimit = 20;

void configureBioFormatsTestBridge()
{
  const QByteArray timeoutOverride = qgetenv("ATLAS_BIOFORMATS_TEST_IO_TIMEOUT_MS");
  if (!timeoutOverride.isEmpty()) {
    bool ok = false;
    const qlonglong value = QString::fromUtf8(timeoutOverride).toLongLong(&ok);
    if (!ok || value < 0 || value > std::numeric_limits<int32_t>::max()) {
      throw std::runtime_error("ATLAS_BIOFORMATS_TEST_IO_TIMEOUT_MS must be an integer from 0 to INT32_MAX");
    }
    ::FLAGS_atlas_bioformats_bridge_io_timeout_ms = static_cast<int32_t>(value);
    return;
  }

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
            << ", jarsDIR=" << jarsDir.absolutePath();

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

class ScopedBioFormatsBridgeTestProcess
{
public:
  ScopedBioFormatsBridgeTestProcess()
  {
    ZBioFormatsBridgeClient::resetInstanceForTesting();
  }

  ~ScopedBioFormatsBridgeTestProcess()
  {
    ZBioFormatsBridgeClient::resetInstanceForTesting();
  }
};

template<typename Fn>
void runWithBioFormatsBridge(Fn&& fn)
{
  const ScopedBioFormatsBridgeTestProcess scopedBridge;
  if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
    GTEST_SKIP() << *reason;
  }
  fn();
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
  uint64_t sizeBytes = 0;
};

struct CorpusDriverRule
{
  QString topLevelFormat;
  QStringList driverPathSuffixes;
  bool includeExtensionlessNonDocs = false;
  QStringList excludedDriverPathSuffixes;
};

struct CorpusFormatStats
{
  size_t manifestFiles = 0;
  size_t selectedDriverFiles = 0;
  size_t missingFiles = 0;
  size_t missingDriverFiles = 0;
  size_t missingFilesOutsideFullManifest = 0;
  size_t skippedIncompleteCorpusFiles = 0;
  size_t skippedIncompleteCorpusDriverFiles = 0;
  size_t openedDriverFiles = 0;
  size_t coveredDriverFiles = 0;
  size_t skippedNonDriverFiles = 0;
  size_t skippedCoveredCompanionFiles = 0;
  size_t openFailures = 0;
  size_t regionFailures = 0;
  size_t warningFailures = 0;
  size_t metadataLevelComparisons = 0;
  size_t metadataLevelMismatchFailures = 0;
  size_t expectedBioFormatsOpenFailures = 0;
  size_t expectedBioFormatsRegionFailures = 0;
  size_t unavailableCorpusOpenFailures = 0;
  size_t unavailableCorpusRegionFailures = 0;
};

enum class CorpusScenePolicy
{
  All,
  Representative,
  Sampled,
};

enum class CorpusFailureStage
{
  Open,
  Region,
};

struct ExpectedBioFormatsFailureRule
{
  CorpusFailureStage stage;
  QString pathSubstring;
  QString errorSubstring;
  const char* reason;
};

struct KnownIncompleteCorpusRule
{
  QString pathSubstring;
  const char* reason;
};

struct KnownIncompleteCorpusDriverRule
{
  QString pathSubstring;
  QString requiredManifestPath;
  const char* reason;
};

struct NativeBioFormatsComparisonRule
{
  QString topLevelFormat;
  FileFormat nativeFormat;
  QStringList driverPathSuffixes;
};

struct NativeBioFormatsComparisonCandidate
{
  CorpusManifestFile file;
  const NativeBioFormatsComparisonRule* rule = nullptr;
};

struct NativeBioFormatsScenePair
{
  size_t nativeScene = 0;
  size_t bioFormatsScene = 0;
};

struct NativeBioFormatsSceneMapping
{
  std::vector<NativeBioFormatsScenePair> pairs;
  bool sceneCountsAreCompatible = false;
  std::optional<std::string> note;
};

const std::vector<CorpusDriverRule>& corpusDriverRules()
{
  static const std::vector<CorpusDriverRule> kRules = {
    {QStringLiteral("AmiraMesh"), {QStringLiteral(".am")}},
    {QStringLiteral("BDV"), {QStringLiteral(".xml")}},
    {QStringLiteral("CV7000"), {QStringLiteral(".wpi")}},
    // _all_positions.ch5 is a CellH5 aggregate with absolute external HDF5
    // links outside the corpus. The individual .ch5 files are the readable
    // dataset drivers.
    {QStringLiteral("CellH5"), {QStringLiteral(".ch5")}, false, {QStringLiteral("/_all_positions.ch5")}},
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

const std::vector<ExpectedBioFormatsFailureRule>& expectedBioFormatsFailureRules()
{
  static const std::vector<ExpectedBioFormatsFailureRule> kRules = {
    // Verified directly against bioformats_package.jar 8.5.0: this corpus
    // directory contains the .wpi and TIFF planes, but omits the
    // MeasurementData.mlf companion file that CV7000Reader requires during
    // setId().
    {CorpusFailureStage::Open,
     QStringLiteral("CV7000/cpg0016/Dest21053D1-15214/Dest210531-152149.wpi"),
     QStringLiteral("Missing MeasurementData.mlf file"),
     "Bio-Formats CV7000Reader.setId fails directly because this corpus group omits required MeasurementData.mlf"},
    // Verified directly against bioformats_package.jar 8.5.0 with loci.formats.ImageReader:
    // DicomReader.openBytes throws from DicomReader.getTile for these NEMA WG04
    // compressed samples before the Atlas bridge sees image bytes.
    {CorpusFailureStage::Region,
     QStringLiteral("DICOM/nema/WG04/IMAGES/JLSL/"),
     QStringLiteral("arraycopy:"),
     "Bio-Formats DicomReader.openBytes fails directly for these NEMA WG04 JLSL samples"                         },
    {CorpusFailureStage::Region,
     QStringLiteral("DICOM/nema/WG04/IMAGES/JLSN/"),
     QStringLiteral("arraycopy:"),
     "Bio-Formats DicomReader.openBytes fails directly for these NEMA WG04 JLSN samples"                         },
    {CorpusFailureStage::Region,
     QStringLiteral("DICOM/nema/WG04/IMAGES/JPLY/"),
     QStringLiteral("arraycopy:"),
     "Bio-Formats DicomReader.openBytes fails directly for these NEMA WG04 JPLY samples"                         },
    // Verified directly against bioformats_package.jar 8.5.0: DicomReader.setId
    // throws ArithmeticException("/ by zero") while initializing these WG16 files.
    {CorpusFailureStage::Open,
     QStringLiteral("DICOM/nema/WG16/Liver/DICOM/"),
     QStringLiteral("/ by zero"),
     "Bio-Formats DicomReader.setId fails directly for these NEMA WG16 Liver samples"                            },
    // Verified directly against bioformats_package.jar 8.5.0: NRRDReader.openBytes
    // throws from readGZIPPlane for this detached gzip sample when the requested
    // tile has y > 0.
    {CorpusFailureStage::Region,
     QStringLiteral("NRRD/midas/01011-dwi.nhdr"),
     QStringLiteral("out of bounds for length 512"),
     "Bio-Formats NRRDReader.openBytes fails directly for a non-origin tile in this gzip sample"                 },
    // Verified directly against bioformats_package.jar 8.5.0: OIRReader.setId
    // reaches MetadataTools.populatePixels with zero pixel dimensions for this
    // map-only OIR file.
    {CorpusFailureStage::Open,
     QStringLiteral("Olympus-OIR/gh-4205/zenodo-13680725/Map_A01.oir"),
     QStringLiteral("0 must be non-null and strictly positive"),
     "Bio-Formats OIRReader.setId fails directly for this map-only OIR file"                                     },
    // Verified directly against bioformats_package.jar 8.5.0: TiffReader.openBytes
    // throws from ImageTools.splitChannels for this 24-bit RGB contiguous strip
    // sample. The planar and tiled 24-bit variants read successfully.
    {CorpusFailureStage::Region,
     QStringLiteral("TIFF/libtiff/depth/flower-rgb-strip-contig-24.tif"),
     QStringLiteral("Index 3072 out of bounds for length 3072"),
     "Bio-Formats TiffReader.openBytes fails directly for this 24-bit RGB contiguous strip sample"               },
    // Verified directly against bioformats_package.jar 8.5.0: the native TIFF
    // path rejects CCITT T.4 Group 3 Fax compression, then TiffJAIReader
    // requires the optional JAI runtime that is not present in our bridge
    // classpath.
    {CorpusFailureStage::Region,
     QStringLiteral("TIFF/libtiff/fax2d.tif"),
     QStringLiteral("Java Advanced Imaging (JAI) is required"),
     "Bio-Formats TiffReader.openBytes requires optional JAI for this CCITT Group 3 Fax TIFF"                    },
    {CorpusFailureStage::Region,
     QStringLiteral("TIFF/libtiff/g3test.tif"),
     QStringLiteral("Java Advanced Imaging (JAI) is required"),
     "Bio-Formats TiffReader.openBytes requires optional JAI for this CCITT Group 3 Fax TIFF"                    },
    {CorpusFailureStage::Region,
     QStringLiteral("TIFF/libtiff/text.tif"),
     QStringLiteral("Java Advanced Imaging (JAI) is required"),
     "Bio-Formats TiffReader.openBytes requires optional JAI for this Thunderscan-compressed TIFF"               },
    // Verified directly against bioformats_package.jar 8.5.0: ImageReader does
    // not auto-detect this standalone .g3 fax stream as a supported format.
    {CorpusFailureStage::Open,
     QStringLiteral("TIFF/libtiff/g3test.g3"),
     QStringLiteral("Unknown file format"),
     "Bio-Formats ImageReader does not auto-detect this standalone .g3 fax stream"                               },
    // Verified directly against bioformats_package.jar 8.5.0: these libtiff
    // samples use SGILog/SGILog24 compression codes that Bio-Formats 8.5.0
    // does not enumerate, so TiffReader fails while reading IFD metadata.
    {CorpusFailureStage::Open,
     QStringLiteral("TIFF/libtiff/off_l16.tif"),
     QStringLiteral("Unable to find TiffCompresssion with code: 34676"),
     "Bio-Formats TiffReader.setId fails directly for this SGILog-compressed TIFF"                               },
    {CorpusFailureStage::Open,
     QStringLiteral("TIFF/libtiff/off_luv24.tif"),
     QStringLiteral("Unable to find TiffCompresssion with code: 34677"),
     "Bio-Formats TiffReader.setId fails directly for this SGILog24-compressed TIFF"                             },
    {CorpusFailureStage::Open,
     QStringLiteral("TIFF/libtiff/off_luv32.tif"),
     QStringLiteral("Unable to find TiffCompresssion with code: 34676"),
     "Bio-Formats TiffReader.setId fails directly for this SGILog-compressed TIFF"                               },
    // Verified directly against bioformats_package.jar 8.5.0: TiffReader
    // delegates JPEG decompression for this sample to ImageIO, which returns
    // null for the stream before Bio-Formats can produce image bytes.
    {CorpusFailureStage::Region,
     QStringLiteral("TIFF/libtiff/smallliz.tif"),
     QStringLiteral("ImageIO returned null when reading JPEG stream"),
     "Bio-Formats TiffReader.openBytes fails directly for this JPEG-compressed TIFF"                             },
    {CorpusFailureStage::Region,
     QStringLiteral("TIFF/libtiff/zackthecat.tif"),
     QStringLiteral("ImageIO returned null when reading JPEG stream"),
     "Bio-Formats TiffReader.openBytes fails directly for this JPEG-compressed TIFF"                             },
    // Verified directly against bioformats_package.jar 8.5.0: ZipReader maps
    // the two embedded OME-TIFF entries to ZipHandles, but OMETiffReader then
    // tries to open the bare entry name relative to the process working
    // directory before producing metadata.
    {CorpusFailureStage::Open,
     QStringLiteral("u-track/integrins.zip"),
     QStringLiteral("case1_higherSNR.ome.tiff"),
     "Bio-Formats ZipReader.setId fails directly for this ZIP of OME-TIFF entries"                               },
  };
  return kRules;
}

const std::vector<KnownIncompleteCorpusDriverRule>& knownIncompleteCorpusDriverRules()
{
  static const std::vector<KnownIncompleteCorpusDriverRule> kRules = {
    // The companion OME metadata names field_3.ome.tiff, field_4.ome.tiff,
    // and many other planes that are not present in the downloaded manifest.
    // Bio-Formats correctly rejects any field in this binary-only set before
    // Atlas can read metadata or pixels, so this partial corpus group cannot be
    // used as support evidence until the missing companions are available.
    {QStringLiteral("OME-TIFF/2016-06/BBBC017/multi-file/"),
     QStringLiteral("OME-TIFF/2016-06/BBBC017/multi-file/field_2303.ome.tiff"),
     "partial BBBC017 binary-only OME-TIFF set: companion metadata references omitted field_*.ome.tiff files"      },
    // These Operetta index files reference remote http://oprt1171/... TIFF
    // planes, and the downloaded corpus directories contain only XML metadata.
    // The complete idr0034 Operetta dataset in this corpus carries the local
    // format coverage.
    {QStringLiteral("PerkinElmer-Operetta/59548/"),
     QStringLiteral("PerkinElmer-Operetta/59548/96015bda-454a-4a19-929b-2212ce6b3a6b/"
                    "r01c01-0565849973.tiff.gz"),
     "partial Operetta metadata-only set: local Images directory omits the TIFF planes referenced by Index.ref.xml"},
    {QStringLiteral("PerkinElmer-Operetta/59549/"),
     QStringLiteral("PerkinElmer-Operetta/59549/b8a3e30d-258c-46f1-b8fe-a96fb51b9af6/"
                    "r01c01-0359943894.tiff.gz"),
     "partial Operetta metadata-only set: local Images directory omits the TIFF planes referenced by Index.ref.xml"},
  };
  return kRules;
}

const std::vector<KnownIncompleteCorpusRule>& knownIncompleteCorpusEntryRules()
{
  static const std::vector<KnownIncompleteCorpusRule> kRules = {
    // This download was still partial when the corpus breadth test was added:
    // the WPI driver is present, but Bio-Formats requires MeasurementData.mlf
    // and the manifest still names TIFF planes that are not on disk.
    {QStringLiteral("CV7000/cpg0016/Dest21053D1-15214/"),
     "partial CV7000 corpus group: local download omits required MeasurementData.mlf and some TIFF planes"},
  };
  return kRules;
}

std::optional<std::string>
expectedBioFormatsFailureReason(const QString& relativePath, CorpusFailureStage stage, const char* error)
{
  const QString errorText = QString::fromUtf8(error);
  for (const ExpectedBioFormatsFailureRule& rule : expectedBioFormatsFailureRules()) {
    if (rule.stage == stage && relativePath.contains(rule.pathSubstring) && errorText.contains(rule.errorSubstring)) {
      return std::string(rule.reason);
    }
  }
  return std::nullopt;
}

std::optional<std::string> knownIncompleteCorpusEntryReason(const QString& relativePath)
{
  for (const KnownIncompleteCorpusRule& rule : knownIncompleteCorpusEntryRules()) {
    if (relativePath.contains(rule.pathSubstring)) {
      return std::string(rule.reason);
    }
  }
  return std::nullopt;
}

std::optional<std::string> knownIncompleteCorpusDriverReason(const QString& relativePath,
                                                             const std::set<QString>& fullManifestRelativePaths)
{
  for (const KnownIncompleteCorpusDriverRule& rule : knownIncompleteCorpusDriverRules()) {
    if (relativePath.contains(rule.pathSubstring) && !fullManifestRelativePaths.contains(rule.requiredManifestPath)) {
      return std::string(rule.reason);
    }
  }
  return std::nullopt;
}

std::optional<QString> corpusRelativePathForLocalPath(const QString& rootPath, const QString& localPath)
{
  const QString cleanRoot = QDir(rootPath).absolutePath();
  const QString cleanPath = QDir::cleanPath(localPath);
  if (cleanPath == cleanRoot) {
    return QString();
  }
  const QString rootPrefix = cleanRoot + QLatin1Char('/');
  if (!cleanPath.startsWith(rootPrefix)) {
    return std::nullopt;
  }
  return cleanPath.mid(rootPrefix.size());
}

std::vector<QString> corpusLocalPathsMentionedInError(const QString& rootPath, const char* error)
{
  const QString text = QString::fromUtf8(error);
  const QString root = QDir(rootPath).absolutePath();
  std::vector<QString> paths;

  for (qsizetype rootIndex = text.indexOf(root); rootIndex >= 0;
       rootIndex = text.indexOf(root, rootIndex + root.size())) {
    qsizetype endIndex = text.size();
    const QStringList terminators = {
      QStringLiteral("\n"),
      QStringLiteral("\r"),
      QStringLiteral(" as '"),
      QStringLiteral(" for reading"),
      QStringLiteral(" <errno"),
      QStringLiteral(" (No such"),
      QStringLiteral(" does not exist"),
      QStringLiteral(" not found"),
      QStringLiteral(" is missing"),
      QStringLiteral("\""),
      QStringLiteral("'"),
      QStringLiteral(")"),
      QStringLiteral(";"),
    };
    for (const QString& terminator : terminators) {
      const qsizetype terminatorIndex = text.indexOf(terminator, rootIndex);
      if (terminatorIndex > rootIndex) {
        endIndex = std::min(endIndex, terminatorIndex);
      }
    }

    QString path = QDir::cleanPath(text.mid(rootIndex, endIndex - rootIndex).trimmed());
    while (path.endsWith(QLatin1Char(',')) || path.endsWith(QLatin1Char('.'))) {
      path.chop(1);
    }
    if (!path.isEmpty()) {
      paths.push_back(path);
    }
  }
  return paths;
}

std::optional<std::string>
missingReferencedFileOutsideFullManifestReason(const QString& rootPath,
                                               const std::set<QString>& fullManifestRelativePaths,
                                               const char* error)
{
  const QString errorText = QString::fromUtf8(error);
  const bool hasMissingFileSignal = errorText.contains(QStringLiteral("No such file"), Qt::CaseInsensitive) ||
                                    errorText.contains(QStringLiteral("does not exist"), Qt::CaseInsensitive) ||
                                    errorText.contains(QStringLiteral("not found"), Qt::CaseInsensitive) ||
                                    errorText.contains(QStringLiteral("for reading"), Qt::CaseInsensitive) ||
                                    errorText.contains(QStringLiteral("missing"), Qt::CaseInsensitive);
  if (!hasMissingFileSignal) {
    return std::nullopt;
  }

  for (const QString& localPath : corpusLocalPathsMentionedInError(rootPath, error)) {
    if (QFileInfo(localPath).exists()) {
      continue;
    }
    const std::optional<QString> relativePath = corpusRelativePathForLocalPath(rootPath, localPath);
    if (!relativePath.has_value()) {
      continue;
    }
    if (!fullManifestRelativePaths.contains(*relativePath)) {
      return fmt::format("referenced local file is missing and absent from full_manifest.json: {} ({})",
                         relativePath->toStdString(),
                         localPath.toStdString());
    }
  }
  return std::nullopt;
}

const std::vector<NativeBioFormatsComparisonRule>& nativeBioFormatsComparisonRules()
{
  static const std::vector<NativeBioFormatsComparisonRule> kRules = {
    {QStringLiteral("OME-TIFF"),
     FileFormat::OmeTiff,
     {QStringLiteral(".ome.tif"),
      QStringLiteral(".ome.tiff"),
      QStringLiteral(".ome.tf2"),
      QStringLiteral(".ome.tf8"),
      QStringLiteral(".ome.btf"),
      QStringLiteral(".tif"),
      QStringLiteral(".tiff"),
      QStringLiteral(".tf2"),
      QStringLiteral(".tf8"),
      QStringLiteral(".btf")}                                                     },
    {QStringLiteral("Zeiss-CZI"),  FileFormat::ZeissCZI, {QStringLiteral(".czi")} },
    {QStringLiteral("Leica-LIF"),  FileFormat::Leica,    {QStringLiteral(".lif")} },
    {QStringLiteral("Leica-XLEF"), FileFormat::Leica,    {QStringLiteral(".xlef")}},
    // No Zeiss-LSM corpus directory is present in the current public manifest;
    // keep the rule here so the comparison starts covering it automatically if
    // the corpus gains LSM samples.
    {QStringLiteral("Zeiss-LSM"),  FileFormat::ZeissLsm, {QStringLiteral(".lsm")} },
  };
  return kRules;
}

std::optional<QString> corpusDatasetGroupKey(const QString& relativePath)
{
  if (relativePath.startsWith(QStringLiteral("Cellomics/")) &&
      relativePath.endsWith(QStringLiteral(".DIB"), Qt::CaseInsensitive)) {
    const qsizetype lastSlash = relativePath.lastIndexOf(u'/');
    if (lastSlash > 0) {
      return relativePath.left(lastSlash);
    }
  }
  if (relativePath.startsWith(QStringLiteral("Flex/")) &&
      relativePath.endsWith(QStringLiteral(".flex"), Qt::CaseInsensitive)) {
    // FlexReader declares MUST_GROUP: each measurement directory is one
    // grouped dataset whose .flex files are companions, not independent
    // corpus drivers.
    const qsizetype lastSlash = relativePath.lastIndexOf(u'/');
    if (lastSlash > 0) {
      return relativePath.left(lastSlash);
    }
  }
  if (relativePath.startsWith(QStringLiteral("DICOM/wsi/"))) {
    const qsizetype lastSlash = relativePath.lastIndexOf(u'/');
    if (lastSlash > 0) {
      return relativePath.left(lastSlash);
    }
  }
  if (relativePath.startsWith(QStringLiteral("OME-TIFF/2016-06/BBBC017/multi-file/field_")) &&
      relativePath.endsWith(QStringLiteral(".ome.tiff"), Qt::CaseInsensitive)) {
    // Each field file opens the same 2304-series OME-TIFF group. Testing the
    // first driver covers the grouped dataset; reopening every field file only
    // repeats the same Bio-Formats setId/openBytes path for hours.
    const qsizetype lastSlash = relativePath.lastIndexOf(u'/');
    if (lastSlash > 0) {
      return relativePath.left(lastSlash);
    }
  }
  return std::nullopt;
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
  for (const QString& suffix : it->excludedDriverPathSuffixes) {
    if (lowerRelativePath.endsWith(suffix)) {
      return false;
    }
  }
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

std::vector<CorpusManifestFile> corpusFilesFromManifest(const QString& rootPath, const QString& manifestName)
{
  const QString manifestPath = QDir(rootPath).filePath(manifestName);
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
    uint64_t sizeBytes = 0;
    if (object.contains(QStringLiteral("size"))) {
      const double sizeValue = object.value(QStringLiteral("size")).toDouble(-1.);
      if (sizeValue < 0.) {
        throw std::runtime_error(fmt::format("Bio-Formats corpus manifest sample {} has invalid size in {}",
                                             index + 1,
                                             manifestPath.toStdString()));
      }
      sizeBytes = static_cast<uint64_t>(sizeValue);
    }
    files.push_back({relativePath, QDir(rootPath).filePath(relativePath), sizeBytes});
  }
  return files;
}

std::vector<CorpusManifestFile> corpusFilesFromManifest(const QString& rootPath)
{
  return corpusFilesFromManifest(rootPath, QStringLiteral("manifest.json"));
}

std::set<QString> corpusRelativePathSet(const std::vector<CorpusManifestFile>& files)
{
  std::set<QString> relativePaths;
  for (const CorpusManifestFile& file : files) {
    relativePaths.insert(file.relativePath);
  }
  return relativePaths;
}

std::set<QString> corpusRelativePathsFromManifest(const QString& rootPath, const QString& manifestName)
{
  return corpusRelativePathSet(corpusFilesFromManifest(rootPath, manifestName));
}

std::optional<QString> publicCorpusRootPath()
{
  const QByteArray root = qgetenv("ATLAS_BIOFORMATS_BREADTH_DIR");
  if (root.isEmpty()) {
    return std::nullopt;
  }
  const QString rootPath = QString::fromUtf8(root);
  if (!QDir(rootPath).exists()) {
    throw std::runtime_error(fmt::format("ATLAS_BIOFORMATS_BREADTH_DIR does not exist: {}", rootPath.toStdString()));
  }
  return rootPath;
}

std::vector<CorpusManifestFile> publicCorpusFiles()
{
  const std::optional<QString> rootPath = publicCorpusRootPath();
  if (!rootPath) {
    return {};
  }
  std::vector<CorpusManifestFile> files = corpusFilesFromManifest(*rootPath);
  std::sort(files.begin(), files.end(), [](const CorpusManifestFile& lhs, const CorpusManifestFile& rhs) {
    return lhs.relativePath < rhs.relativePath;
  });
  return files;
}

std::set<QString> excludedPublicCorpusFormats()
{
  const QByteArray raw = qgetenv("ATLAS_BIOFORMATS_BREADTH_EXCLUDE_FORMATS");
  std::set<QString> formats;
  if (raw.isEmpty()) {
    return formats;
  }

  const QStringList parts = QString::fromUtf8(raw).split(QLatin1Char(','), Qt::SkipEmptyParts);
  for (const QString& part : parts) {
    const QString format = part.trimmed();
    if (!format.isEmpty()) {
      formats.insert(format);
    }
  }
  return formats;
}

std::set<QString> includedPublicCorpusFormats()
{
  const QByteArray raw = qgetenv("ATLAS_BIOFORMATS_BREADTH_ONLY_FORMATS");
  std::set<QString> formats;
  if (raw.isEmpty()) {
    return formats;
  }

  const QStringList parts = QString::fromUtf8(raw).split(QLatin1Char(','), Qt::SkipEmptyParts);
  for (const QString& part : parts) {
    const QString format = part.trimmed();
    if (!format.isEmpty()) {
      formats.insert(format);
    }
  }
  return formats;
}

QStringList publicCorpusPathSubstrings()
{
  const QByteArray raw = qgetenv("ATLAS_BIOFORMATS_BREADTH_PATH_SUBSTRINGS");
  QStringList pathSubstrings;
  if (raw.isEmpty()) {
    return pathSubstrings;
  }

  const QStringList parts = QString::fromUtf8(raw).split(QLatin1Char(','), Qt::SkipEmptyParts);
  for (const QString& part : parts) {
    const QString pathSubstring = part.trimmed();
    if (!pathSubstring.isEmpty()) {
      pathSubstrings.push_back(pathSubstring);
    }
  }
  return pathSubstrings;
}

void initializePublicCorpusLogging()
{
  const QByteArray logPrefix = qgetenv("ATLAS_BIOFORMATS_BREADTH_LOG_PREFIX");
  if (logPrefix.isEmpty()) {
    ZLogInit::instance("zbioformatstest");
    return;
  }
  ZLogInit::instance("zbioformatstest", QString::fromUtf8(logPrefix));
}

bool publicCorpusBooleanEnv(const char* name, bool defaultValue)
{
  const QByteArray raw = qgetenv(name);
  if (raw.isEmpty()) {
    return defaultValue;
  }

  const QString value = QString::fromUtf8(raw).trimmed().toLower();
  if (value == QStringLiteral("0") || value == QStringLiteral("false") || value == QStringLiteral("no") ||
      value == QStringLiteral("off")) {
    return false;
  }
  if (value == QStringLiteral("1") || value == QStringLiteral("true") || value == QStringLiteral("yes") ||
      value == QStringLiteral("on")) {
    return true;
  }
  throw std::runtime_error(fmt::format("{} must be one of 0/1, false/true, no/yes, or off/on", name));
}

size_t publicCorpusProgressInterval()
{
  const QByteArray raw = qgetenv("ATLAS_BIOFORMATS_BREADTH_PROGRESS_INTERVAL");
  if (raw.isEmpty()) {
    return kDefaultPublicCorpusProgressInterval;
  }

  bool ok = false;
  const qulonglong value = QString::fromUtf8(raw).toULongLong(&ok);
  if (!ok) {
    throw std::runtime_error("ATLAS_BIOFORMATS_BREADTH_PROGRESS_INTERVAL must be a non-negative integer");
  }
  if (value > std::numeric_limits<size_t>::max()) {
    throw std::runtime_error("ATLAS_BIOFORMATS_BREADTH_PROGRESS_INTERVAL is too large for this platform");
  }
  return static_cast<size_t>(value);
}

bool publicCorpusLogDriverFiles()
{
  return publicCorpusBooleanEnv("ATLAS_BIOFORMATS_BREADTH_LOG_DRIVER_FILES", true);
}

bool publicCorpusLogRegionReads()
{
  return publicCorpusBooleanEnv("ATLAS_BIOFORMATS_BREADTH_LOG_REGION_READS", true);
}

bool publicCorpusFailFast()
{
  return publicCorpusBooleanEnv("ATLAS_BIOFORMATS_BREADTH_FAIL_FAST", false);
}

bool publicCorpusCompareMetadataLevels()
{
  return publicCorpusBooleanEnv("ATLAS_BIOFORMATS_BREADTH_COMPARE_METADATA_LEVELS", false);
}

size_t publicCorpusSizeEnv(const char* name, size_t defaultValue)
{
  const QByteArray raw = qgetenv(name);
  if (raw.isEmpty()) {
    return defaultValue;
  }

  bool ok = false;
  const qulonglong value = QString::fromUtf8(raw).trimmed().toULongLong(&ok);
  if (!ok || value > std::numeric_limits<size_t>::max()) {
    throw std::runtime_error(fmt::format("{} must be a non-negative integer", name));
  }
  return static_cast<size_t>(value);
}

size_t publicCorpusSceneSampleLimit()
{
  return publicCorpusSizeEnv("ATLAS_BIOFORMATS_BREADTH_SCENE_SAMPLE_LIMIT", kDefaultCorpusSceneSampleLimit);
}

size_t nativeComparisonSceneSampleLimit()
{
  const QByteArray raw = qgetenv("ATLAS_NATIVE_BIOFORMATS_COMPARE_SCENE_SAMPLE_LIMIT");
  if (raw.isEmpty()) {
    return publicCorpusSceneSampleLimit();
  }
  return publicCorpusSizeEnv("ATLAS_NATIVE_BIOFORMATS_COMPARE_SCENE_SAMPLE_LIMIT", kDefaultCorpusSceneSampleLimit);
}

CorpusScenePolicy publicCorpusScenePolicy()
{
  const QByteArray raw = qgetenv("ATLAS_BIOFORMATS_BREADTH_SCENE_POLICY");
  if (raw.isEmpty()) {
    return CorpusScenePolicy::Sampled;
  }

  const QString value = QString::fromUtf8(raw).trimmed().toLower();
  if (value == QStringLiteral("all")) {
    return CorpusScenePolicy::All;
  }
  if (value == QStringLiteral("representative")) {
    return CorpusScenePolicy::Representative;
  }
  if (value == QStringLiteral("sampled")) {
    return CorpusScenePolicy::Sampled;
  }
  throw std::runtime_error("ATLAS_BIOFORMATS_BREADTH_SCENE_POLICY must be 'all', 'representative', or 'sampled'");
}

const char* corpusScenePolicyName(CorpusScenePolicy policy)
{
  switch (policy) {
    case CorpusScenePolicy::All:
      return "all";
    case CorpusScenePolicy::Representative:
      return "representative";
    case CorpusScenePolicy::Sampled:
      return "sampled";
  }
  CHECK(false) << "unhandled Bio-Formats corpus scene policy";
  return "unknown";
}

uint64_t corpusStableSceneScore(const QString& stableKey, size_t scene)
{
  uint64_t hash = 1469598103934665603ull;
  auto appendByte = [&](uint8_t byte) {
    hash ^= byte;
    hash *= 1099511628211ull;
  };

  const QByteArray keyBytes = stableKey.toUtf8();
  for (char byte : keyBytes) {
    appendByte(static_cast<uint8_t>(byte));
  }
  appendByte(0xffu);
  uint64_t sceneValue = static_cast<uint64_t>(scene);
  for (size_t byteIndex = 0; byteIndex < sizeof(sceneValue); ++byteIndex) {
    appendByte(static_cast<uint8_t>((sceneValue >> (byteIndex * 8)) & 0xffu));
  }
  return hash;
}

std::vector<size_t>
corpusScenesToRead(size_t sceneCount, CorpusScenePolicy policy, const QString& stableKey, size_t sampledSceneLimit)
{
  CHECK(sceneCount > 0);
  std::vector<size_t> scenes;
  switch (policy) {
    case CorpusScenePolicy::All:
      scenes.reserve(sceneCount);
      for (size_t scene = 0; scene < sceneCount; ++scene) {
        scenes.push_back(scene);
      }
      return scenes;
    case CorpusScenePolicy::Representative:
      scenes.push_back(0);
      if (sceneCount > 2) {
        scenes.push_back(sceneCount / 2);
      }
      if (sceneCount > 1) {
        scenes.push_back(sceneCount - 1);
      }
      return scenes;
    case CorpusScenePolicy::Sampled:
      if (sampledSceneLimit == 0 || sceneCount <= sampledSceneLimit) {
        scenes.reserve(sceneCount);
        for (size_t scene = 0; scene < sceneCount; ++scene) {
          scenes.push_back(scene);
        }
        return scenes;
      }

      {
        std::vector<std::pair<uint64_t, size_t>> scoredScenes;
        scoredScenes.reserve(sceneCount);
        for (size_t scene = 0; scene < sceneCount; ++scene) {
          scoredScenes.push_back({corpusStableSceneScore(stableKey, scene), scene});
        }
        std::sort(scoredScenes.begin(), scoredScenes.end(), [](const auto& lhs, const auto& rhs) {
          if (lhs.first != rhs.first) {
            return lhs.first < rhs.first;
          }
          return lhs.second < rhs.second;
        });

        scenes.reserve(sampledSceneLimit);
        for (size_t i = 0; i < sampledSceneLimit; ++i) {
          scenes.push_back(scoredScenes[i].second);
        }
        std::sort(scenes.begin(), scenes.end());
        return scenes;
      }
  }
  CHECK(false) << "unhandled Bio-Formats corpus scene policy";
  return scenes;
}

std::string corpusSceneListToString(const std::vector<size_t>& scenes)
{
  std::vector<std::string> parts;
  parts.reserve(scenes.size());
  for (size_t scene : scenes) {
    parts.push_back(fmt::format("{}", scene));
  }
  return fmt::format("[{}]", fmt::join(parts, ","));
}

std::string corpusFormatSetToString(const std::set<QString>& formats)
{
  std::vector<std::string> names;
  names.reserve(formats.size());
  for (const QString& format : formats) {
    names.push_back(format.toStdString());
  }
  return fmt::format("[{}]", fmt::join(names, ", "));
}

std::string corpusPathSubstringListToString(const QStringList& pathSubstrings)
{
  std::vector<std::string> items;
  items.reserve(pathSubstrings.size());
  for (const QString& pathSubstring : pathSubstrings) {
    items.push_back(pathSubstring.toStdString());
  }
  return fmt::format("[{}]", fmt::join(items, ", "));
}

void emitCorpusProgress(const std::string& message)
{
  LOG(INFO) << "[bioformats-corpus] " << message;
}

void emitNativeComparisonProgress(const std::string& message)
{
  LOG(INFO) << "[native-bioformats-corpus] " << message;
}

bool relativePathEndsWithAnySuffix(const QString& relativePath, const QStringList& suffixes)
{
  const QString lowerRelativePath = relativePath.toLower();
  for (const QString& suffix : suffixes) {
    if (lowerRelativePath.endsWith(suffix)) {
      return true;
    }
  }
  return false;
}

std::vector<NativeBioFormatsComparisonCandidate>
nativeBioFormatsComparisonCandidates(const std::vector<CorpusManifestFile>& files)
{
  std::vector<NativeBioFormatsComparisonCandidate> selected;
  for (const NativeBioFormatsComparisonRule& rule : nativeBioFormatsComparisonRules()) {
    std::vector<NativeBioFormatsComparisonCandidate> candidates;
    for (const CorpusManifestFile& file : files) {
      if (topLevelFormat(file.relativePath) != rule.topLevelFormat) {
        continue;
      }
      if (!relativePathEndsWithAnySuffix(file.relativePath, rule.driverPathSuffixes)) {
        continue;
      }
      candidates.push_back({file, &rule});
    }

    std::sort(candidates.begin(),
              candidates.end(),
              [](const NativeBioFormatsComparisonCandidate& lhs, const NativeBioFormatsComparisonCandidate& rhs) {
                return lhs.file.relativePath < rhs.file.relativePath;
              });

    std::set<QString> coveredGroupKeys;
    std::vector<NativeBioFormatsComparisonCandidate> groupedCandidates;
    groupedCandidates.reserve(candidates.size());
    for (const NativeBioFormatsComparisonCandidate& candidate : candidates) {
      const std::optional<QString> groupKey = corpusDatasetGroupKey(candidate.file.relativePath);
      if (groupKey.has_value()) {
        const QString scopedGroupKey = rule.topLevelFormat + QLatin1Char('\n') + *groupKey;
        if (!coveredGroupKeys.insert(scopedGroupKey).second) {
          continue;
        }
      }
      groupedCandidates.push_back(candidate);
    }

    selected.insert(selected.end(), groupedCandidates.begin(), groupedCandidates.end());
  }
  std::sort(selected.begin(),
            selected.end(),
            [](const NativeBioFormatsComparisonCandidate& lhs, const NativeBioFormatsComparisonCandidate& rhs) {
              return lhs.file.relativePath < rhs.file.relativePath;
            });
  return selected;
}

std::string nativeComparisonCandidateCountsToString(const std::vector<NativeBioFormatsComparisonCandidate>& candidates)
{
  std::map<QString, size_t> counts;
  for (const NativeBioFormatsComparisonCandidate& candidate : candidates) {
    CHECK(candidate.rule != nullptr);
    ++counts[candidate.rule->topLevelFormat];
  }

  std::vector<std::string> parts;
  parts.reserve(counts.size());
  for (const auto& [format, count] : counts) {
    parts.push_back(fmt::format("{}={}", format.toStdString(), count));
  }
  return fmt::format("[{}]", fmt::join(parts, ","));
}

template<typename T>
void appendNativeBioFormatsInfoMismatch(std::vector<std::string>& mismatches,
                                        const char* field,
                                        const T& nativeValue,
                                        const T& bioFormatsValue)
{
  if (nativeValue == bioFormatsValue) {
    return;
  }
  mismatches.push_back(fmt::format("{} differs: native={} bioformats={}", field, nativeValue, bioFormatsValue));
}

std::vector<std::string> compareNativeBioFormatsCoreInfo(const ZImgInfo& nativeInfo, const ZImgInfo& bioFormatsInfo)
{
  std::vector<std::string> mismatches;
  appendNativeBioFormatsInfoMismatch(mismatches, "width", nativeInfo.width, bioFormatsInfo.width);
  appendNativeBioFormatsInfoMismatch(mismatches, "height", nativeInfo.height, bioFormatsInfo.height);
  appendNativeBioFormatsInfoMismatch(mismatches, "depth", nativeInfo.depth, bioFormatsInfo.depth);
  appendNativeBioFormatsInfoMismatch(mismatches, "numChannels", nativeInfo.numChannels, bioFormatsInfo.numChannels);
  appendNativeBioFormatsInfoMismatch(mismatches, "numTimes", nativeInfo.numTimes, bioFormatsInfo.numTimes);
  appendNativeBioFormatsInfoMismatch(mismatches,
                                     "bytesPerVoxel",
                                     nativeInfo.bytesPerVoxel,
                                     bioFormatsInfo.bytesPerVoxel);
  if (nativeInfo.voxelFormat != bioFormatsInfo.voxelFormat) {
    mismatches.push_back(fmt::format("voxelFormat differs: native={} bioformats={}",
                                     enumToString(nativeInfo.voxelFormat),
                                     enumToString(bioFormatsInfo.voxelFormat)));
  }
  return mismatches;
}

bool nativeBioFormatsCoreInfoMatches(const ZImgInfo& nativeInfo, const ZImgInfo& bioFormatsInfo)
{
  return compareNativeBioFormatsCoreInfo(nativeInfo, bioFormatsInfo).empty();
}

std::vector<NativeBioFormatsScenePair> indexMatchedNativeBioFormatsScenePairs(size_t sceneCount)
{
  std::vector<NativeBioFormatsScenePair> pairs;
  pairs.reserve(sceneCount);
  for (size_t scene = 0; scene < sceneCount; ++scene) {
    pairs.push_back({scene, scene});
  }
  return pairs;
}

std::vector<NativeBioFormatsScenePair> cziAcquisitionScenePairs(const std::vector<ZImgInfo>& nativeInfos,
                                                                const std::vector<ZImgInfo>& bioFormatsInfos)
{
  std::vector<NativeBioFormatsScenePair> pairs;
  pairs.reserve(nativeInfos.size());
  std::vector<bool> usedBioFormatsScenes(bioFormatsInfos.size(), false);

  for (size_t nativeScene = 0; nativeScene < nativeInfos.size(); ++nativeScene) {
    std::optional<size_t> matchingBioFormatsScene;
    if (nativeScene < bioFormatsInfos.size() && !usedBioFormatsScenes[nativeScene] &&
        nativeBioFormatsCoreInfoMatches(nativeInfos[nativeScene], bioFormatsInfos[nativeScene])) {
      matchingBioFormatsScene = nativeScene;
    } else {
      for (size_t bioFormatsScene = 0; bioFormatsScene < bioFormatsInfos.size(); ++bioFormatsScene) {
        if (usedBioFormatsScenes[bioFormatsScene]) {
          continue;
        }
        if (nativeBioFormatsCoreInfoMatches(nativeInfos[nativeScene], bioFormatsInfos[bioFormatsScene])) {
          matchingBioFormatsScene = bioFormatsScene;
          break;
        }
      }
    }

    if (!matchingBioFormatsScene.has_value()) {
      return {};
    }
    usedBioFormatsScenes[*matchingBioFormatsScene] = true;
    pairs.push_back({nativeScene, *matchingBioFormatsScene});
  }
  return pairs;
}

NativeBioFormatsSceneMapping buildNativeBioFormatsSceneMapping(const NativeBioFormatsComparisonRule& rule,
                                                               const std::vector<ZImgInfo>& nativeInfos,
                                                               const std::vector<ZImgInfo>& bioFormatsInfos)
{
  if (nativeInfos.size() == bioFormatsInfos.size()) {
    return {.pairs = indexMatchedNativeBioFormatsScenePairs(nativeInfos.size()), .sceneCountsAreCompatible = true};
  }

  if (rule.nativeFormat == FileFormat::ZeissCZI && nativeInfos.size() < bioFormatsInfos.size()) {
    std::vector<NativeBioFormatsScenePair> pairs = cziAcquisitionScenePairs(nativeInfos, bioFormatsInfos);
    if (pairs.size() == nativeInfos.size()) {
      return {
        .pairs = std::move(pairs),
        .sceneCountsAreCompatible = true,
        .note = fmt::format(
          "matched {} native CZI acquisition scenes against {} Bio-Formats series; unmatched Bio-Formats series are "
          "CZI pyramid/thumbnail representations that Atlas stores outside the scene list",
          nativeInfos.size(),
          bioFormatsInfos.size())};
    }
  }

  return {.pairs = indexMatchedNativeBioFormatsScenePairs(std::min(nativeInfos.size(), bioFormatsInfos.size())),
          .sceneCountsAreCompatible = false};
}

std::string nativeBioFormatsScenePairListToString(const std::vector<size_t>& pairIndices,
                                                  const std::vector<NativeBioFormatsScenePair>& pairs)
{
  std::vector<std::string> parts;
  parts.reserve(pairIndices.size());
  for (size_t pairIndex : pairIndices) {
    CHECK(pairIndex < pairs.size());
    const NativeBioFormatsScenePair& pair = pairs[pairIndex];
    if (pair.nativeScene == pair.bioFormatsScene) {
      parts.push_back(fmt::format("{}", pair.nativeScene));
    } else {
      parts.push_back(fmt::format("{}->{}", pair.nativeScene, pair.bioFormatsScene));
    }
  }
  return fmt::format("[{}]", fmt::join(parts, ","));
}

std::optional<std::string> firstDifferingByteDescription(const ZImg& nativeImg, const ZImg& bioFormatsImg)
{
  if (!nativeImg.isSameSize(bioFormatsImg) || !nativeImg.isSameType(bioFormatsImg)) {
    return std::nullopt;
  }
  for (size_t t = 0; t < nativeImg.numTimes(); ++t) {
    const uint8_t* nativeData = nativeImg.timeData(t);
    const uint8_t* bioFormatsData = bioFormatsImg.timeData(t);
    for (size_t byte = 0; byte < nativeImg.timeByteNumber(); ++byte) {
      if (nativeData[byte] != bioFormatsData[byte]) {
        return fmt::format("time={} byte={} native={} bioformats={}",
                           t,
                           byte,
                           static_cast<uint32_t>(nativeData[byte]),
                           static_cast<uint32_t>(bioFormatsData[byte]));
      }
    }
  }
  return std::nullopt;
}

bool sameRgbChannelColor(const col4& lhs, const col4& rhs)
{
  return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
}

std::string channelColorRgbToString(const col4& color)
{
  return fmt::format("rgb({},{},{})",
                     static_cast<uint32_t>(color.r),
                     static_cast<uint32_t>(color.g),
                     static_cast<uint32_t>(color.b));
}

std::string channelColorsRgbToString(const std::vector<col4>& colors)
{
  std::vector<std::string> parts;
  parts.reserve(colors.size());
  for (const col4& color : colors) {
    parts.push_back(channelColorRgbToString(color));
  }
  return fmt::format("[{}]", fmt::join(parts, ","));
}

bool channelMapIsIdentity(const std::vector<size_t>& channelMap)
{
  for (size_t c = 0; c < channelMap.size(); ++c) {
    if (channelMap[c] != c) {
      return false;
    }
  }
  return true;
}

std::string channelMapToString(const std::vector<size_t>& nativeToBioFormatsChannel)
{
  std::vector<std::string> parts;
  parts.reserve(nativeToBioFormatsChannel.size());
  for (size_t nativeChannel = 0; nativeChannel < nativeToBioFormatsChannel.size(); ++nativeChannel) {
    parts.push_back(fmt::format("{}->{}", nativeChannel, nativeToBioFormatsChannel[nativeChannel]));
  }
  return fmt::format("[{}]", fmt::join(parts, ","));
}

std::optional<std::vector<size_t>> leicaNativeToBioFormatsChannelMapByColor(const ZImgInfo& nativeInfo,
                                                                            const ZImgInfo& bioFormatsInfo)
{
  if (nativeInfo.numChannels <= 1 || nativeInfo.numChannels != bioFormatsInfo.numChannels ||
      nativeInfo.channelColors.size() != nativeInfo.numChannels ||
      bioFormatsInfo.channelColors.size() != bioFormatsInfo.numChannels) {
    return std::nullopt;
  }

  std::vector<size_t> nativeToBioFormatsChannel(nativeInfo.numChannels, 0);
  std::vector<bool> usedBioFormatsChannels(bioFormatsInfo.numChannels, false);
  for (size_t nativeChannel = 0; nativeChannel < nativeInfo.numChannels; ++nativeChannel) {
    std::optional<size_t> matchingBioFormatsChannel;
    for (size_t bioFormatsChannel = 0; bioFormatsChannel < bioFormatsInfo.numChannels; ++bioFormatsChannel) {
      if (usedBioFormatsChannels[bioFormatsChannel] ||
          !sameRgbChannelColor(nativeInfo.channelColors[nativeChannel],
                               bioFormatsInfo.channelColors[bioFormatsChannel])) {
        continue;
      }
      if (matchingBioFormatsChannel.has_value()) {
        return std::nullopt;
      }
      matchingBioFormatsChannel = bioFormatsChannel;
    }
    if (!matchingBioFormatsChannel.has_value()) {
      return std::nullopt;
    }
    usedBioFormatsChannels[*matchingBioFormatsChannel] = true;
    nativeToBioFormatsChannel[nativeChannel] = *matchingBioFormatsChannel;
  }

  if (channelMapIsIdentity(nativeToBioFormatsChannel)) {
    return std::nullopt;
  }
  return nativeToBioFormatsChannel;
}

std::optional<std::vector<size_t>> nativeToBioFormatsChannelMapByColor(const NativeBioFormatsComparisonRule& rule,
                                                                       const ZImgInfo& nativeInfo,
                                                                       const ZImgInfo& bioFormatsInfo)
{
  if (rule.nativeFormat != FileFormat::Leica) {
    return std::nullopt;
  }
  return leicaNativeToBioFormatsChannelMapByColor(nativeInfo, bioFormatsInfo);
}

std::optional<std::string>
firstDifferingByteDescriptionWithChannelMap(const ZImg& nativeImg,
                                            const ZImg& bioFormatsImg,
                                            const std::vector<size_t>& nativeToBioFormatsChannel)
{
  if (!nativeImg.isSameSize(bioFormatsImg) || !nativeImg.isSameType(bioFormatsImg) ||
      nativeToBioFormatsChannel.size() != nativeImg.numChannels()) {
    return firstDifferingByteDescription(nativeImg, bioFormatsImg);
  }

  for (size_t bioFormatsChannel : nativeToBioFormatsChannel) {
    if (bioFormatsChannel >= bioFormatsImg.numChannels()) {
      return firstDifferingByteDescription(nativeImg, bioFormatsImg);
    }
  }

  for (size_t t = 0; t < nativeImg.numTimes(); ++t) {
    for (size_t nativeChannel = 0; nativeChannel < nativeImg.numChannels(); ++nativeChannel) {
      const size_t bioFormatsChannel = nativeToBioFormatsChannel[nativeChannel];
      const uint8_t* nativeData = nativeImg.channelData(nativeChannel, t);
      const uint8_t* bioFormatsData = bioFormatsImg.channelData(bioFormatsChannel, t);
      if (nativeData == bioFormatsData || std::memcmp(nativeData, bioFormatsData, nativeImg.channelByteNumber()) == 0) {
        continue;
      }
      for (size_t byte = 0; byte < nativeImg.channelByteNumber(); ++byte) {
        if (nativeData[byte] != bioFormatsData[byte]) {
          return fmt::format("time={} native_channel={} bioformats_channel={} channel_byte={} native={} bioformats={}",
                             t,
                             nativeChannel,
                             bioFormatsChannel,
                             byte,
                             static_cast<uint32_t>(nativeData[byte]),
                             static_cast<uint32_t>(bioFormatsData[byte]));
        }
      }
    }
  }
  return std::nullopt;
}

std::optional<std::string>
nativeBioFormatsPixelDifferenceDescription(const ZImg& nativeImg,
                                           const ZImg& bioFormatsImg,
                                           const std::optional<std::vector<size_t>>& nativeToBioFormatsChannel)
{
  if (nativeImg == bioFormatsImg) {
    return std::nullopt;
  }
  if (!nativeImg.isSameSize(bioFormatsImg) || !nativeImg.isSameType(bioFormatsImg)) {
    return fmt::format("image size/type differs: native={} bioformats={}", nativeImg.info(), bioFormatsImg.info());
  }
  if (nativeToBioFormatsChannel.has_value()) {
    const std::optional<std::string> mappedDifference =
      firstDifferingByteDescriptionWithChannelMap(nativeImg, bioFormatsImg, *nativeToBioFormatsChannel);
    if (!mappedDifference.has_value()) {
      return std::nullopt;
    }
    return fmt::format("{} after applying native_to_bioformats_channel_map={}",
                       *mappedDifference,
                       channelMapToString(*nativeToBioFormatsChannel));
  }
  return firstDifferingByteDescription(nativeImg, bioFormatsImg).value_or("pixel bytes differ");
}

template<typename FilteredValue, typename FullValue>
void appendCorpusFieldMismatch(std::vector<std::string>& mismatches,
                               const std::string& field,
                               const FilteredValue& filteredValue,
                               const FullValue& fullValue)
{
  if (filteredValue == fullValue) {
    return;
  }
  mismatches.push_back(fmt::format("{} differs: filtered={} full={}", field, filteredValue, fullValue));
}

std::vector<std::string> compareCorpusMetadataLevelCoreInfo(const ZBioFormatsDatasetInfo& filtered,
                                                            const ZBioFormatsDatasetInfo& full)
{
  std::vector<std::string> mismatches;
  appendCorpusFieldMismatch(mismatches,
                            "format_name",
                            filtered.formatName.toStdString(),
                            full.formatName.toStdString());
  appendCorpusFieldMismatch(mismatches,
                            "reader_class",
                            filtered.readerClass.toStdString(),
                            full.readerClass.toStdString());
  appendCorpusFieldMismatch(mismatches, "series_count", filtered.series.size(), full.series.size());

  const size_t commonSeriesCount = std::min(filtered.series.size(), full.series.size());
  for (size_t seriesIndex = 0; seriesIndex < commonSeriesCount; ++seriesIndex) {
    const ZBioFormatsSeriesInfo& filteredSeries = filtered.series[seriesIndex];
    const ZBioFormatsSeriesInfo& fullSeries = full.series[seriesIndex];
    const std::string prefix = fmt::format("series[{}]", seriesIndex);
    appendCorpusFieldMismatch(mismatches, prefix + ".series", filteredSeries.series, fullSeries.series);
    appendCorpusFieldMismatch(mismatches, prefix + ".size_x", filteredSeries.sizeX, fullSeries.sizeX);
    appendCorpusFieldMismatch(mismatches, prefix + ".size_y", filteredSeries.sizeY, fullSeries.sizeY);
    appendCorpusFieldMismatch(mismatches, prefix + ".size_z", filteredSeries.sizeZ, fullSeries.sizeZ);
    appendCorpusFieldMismatch(mismatches,
                              prefix + ".effective_size_c",
                              filteredSeries.effectiveSizeC,
                              fullSeries.effectiveSizeC);
    appendCorpusFieldMismatch(mismatches, prefix + ".size_t", filteredSeries.sizeT, fullSeries.sizeT);
    appendCorpusFieldMismatch(mismatches,
                              prefix + ".rgb_channel_count",
                              filteredSeries.rgbChannelCount,
                              fullSeries.rgbChannelCount);
    appendCorpusFieldMismatch(mismatches,
                              prefix + ".bytes_per_pixel",
                              filteredSeries.bytesPerPixel,
                              fullSeries.bytesPerPixel);
    appendCorpusFieldMismatch(mismatches,
                              prefix + ".pixel_type",
                              filteredSeries.pixelType.toStdString(),
                              fullSeries.pixelType.toStdString());
    appendCorpusFieldMismatch(mismatches,
                              prefix + ".dimension_order",
                              filteredSeries.dimensionOrder.toStdString(),
                              fullSeries.dimensionOrder.toStdString());
    appendCorpusFieldMismatch(mismatches,
                              prefix + ".resolution_count",
                              filteredSeries.resolutionCount,
                              fullSeries.resolutionCount);
    appendCorpusFieldMismatch(mismatches,
                              prefix + ".optimal_tile_width",
                              filteredSeries.optimalTileWidth,
                              fullSeries.optimalTileWidth);
    appendCorpusFieldMismatch(mismatches,
                              prefix + ".optimal_tile_height",
                              filteredSeries.optimalTileHeight,
                              fullSeries.optimalTileHeight);
    appendCorpusFieldMismatch(mismatches,
                              prefix + ".resolution_info_count",
                              filteredSeries.resolutions.size(),
                              fullSeries.resolutions.size());

    const size_t commonResolutionCount = std::min(filteredSeries.resolutions.size(), fullSeries.resolutions.size());
    for (size_t resolutionIndex = 0; resolutionIndex < commonResolutionCount; ++resolutionIndex) {
      const ZBioFormatsResolutionInfo& filteredResolution = filteredSeries.resolutions[resolutionIndex];
      const ZBioFormatsResolutionInfo& fullResolution = fullSeries.resolutions[resolutionIndex];
      const std::string resolutionPrefix = fmt::format("{}.resolutions[{}]", prefix, resolutionIndex);
      appendCorpusFieldMismatch(mismatches,
                                resolutionPrefix + ".resolution",
                                filteredResolution.resolution,
                                fullResolution.resolution);
      appendCorpusFieldMismatch(mismatches,
                                resolutionPrefix + ".size_x",
                                filteredResolution.sizeX,
                                fullResolution.sizeX);
      appendCorpusFieldMismatch(mismatches,
                                resolutionPrefix + ".size_y",
                                filteredResolution.sizeY,
                                fullResolution.sizeY);
      appendCorpusFieldMismatch(mismatches,
                                resolutionPrefix + ".size_z",
                                filteredResolution.sizeZ,
                                fullResolution.sizeZ);
      appendCorpusFieldMismatch(mismatches,
                                resolutionPrefix + ".effective_size_c",
                                filteredResolution.effectiveSizeC,
                                fullResolution.effectiveSizeC);
      appendCorpusFieldMismatch(mismatches,
                                resolutionPrefix + ".size_t",
                                filteredResolution.sizeT,
                                fullResolution.sizeT);
      appendCorpusFieldMismatch(mismatches,
                                resolutionPrefix + ".image_count",
                                filteredResolution.imageCount,
                                fullResolution.imageCount);
      appendCorpusFieldMismatch(mismatches,
                                resolutionPrefix + ".optimal_tile_width",
                                filteredResolution.optimalTileWidth,
                                fullResolution.optimalTileWidth);
      appendCorpusFieldMismatch(mismatches,
                                resolutionPrefix + ".optimal_tile_height",
                                filteredResolution.optimalTileHeight,
                                fullResolution.optimalTileHeight);
    }
  }
  return mismatches;
}

double secondsSince(std::chrono::steady_clock::time_point startTime)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
}

std::string regionToString(const ZImgRegion& region)
{
  return fmt::format("x=[{},{}), y=[{},{}), z=[{},{}), c=[{},{}), t=[{},{})",
                     region.start.x,
                     region.end.x,
                     region.start.y,
                     region.end.y,
                     region.start.z,
                     region.end.z,
                     region.start.c,
                     region.end.c,
                     region.start.t,
                     region.end.t);
}

void emitCorpusFormatSummary(const QString& format,
                             const CorpusFormatStats& stats,
                             size_t processedFiles,
                             size_t targetFiles,
                             std::chrono::steady_clock::time_point startTime)
{
  emitCorpusProgress(fmt::format(
    "finished format={} processed={}/{} elapsed={:.1f}s manifest_files={} selected_drivers={} opened_drivers={} "
    "covered_drivers={} missing_files={} missing_drivers={} missing_files_outside_full_manifest={} "
    "skipped_incomplete_corpus_files={} "
    "skipped_incomplete_corpus_drivers={} "
    "skipped_non_drivers={} skipped_covered_companions={} open_failures={} region_failures={} warning_failures={} "
    "metadata_level_comparisons={} "
    "metadata_level_mismatch_failures={} expected_bioformats_open_failures={} expected_bioformats_region_failures={} "
    "unavailable_corpus_open_failures={} unavailable_corpus_region_failures={}",
    format.toStdString(),
    processedFiles,
    targetFiles,
    secondsSince(startTime),
    stats.manifestFiles,
    stats.selectedDriverFiles,
    stats.openedDriverFiles,
    stats.coveredDriverFiles,
    stats.missingFiles,
    stats.missingDriverFiles,
    stats.missingFilesOutsideFullManifest,
    stats.skippedIncompleteCorpusFiles,
    stats.skippedIncompleteCorpusDriverFiles,
    stats.skippedNonDriverFiles,
    stats.skippedCoveredCompanionFiles,
    stats.openFailures,
    stats.regionFailures,
    stats.warningFailures,
    stats.metadataLevelComparisons,
    stats.metadataLevelMismatchFailures,
    stats.expectedBioFormatsOpenFailures,
    stats.expectedBioFormatsRegionFailures,
    stats.unavailableCorpusOpenFailures,
    stats.unavailableCorpusRegionFailures));
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
  runWithBioFormatsBridge([]() {
    const QStringList extensions = ZImgBioFormats().extensions();
    ASSERT_FALSE(extensions.empty());
    EXPECT_TRUE(extensions.contains(QStringLiteral("fake"), Qt::CaseInsensitive));
  });
}

TEST(ZBioFormatsTest, FakeReaderMetadataCreatesDeterministicInfoAndSubBlocks)
{
  runWithBioFormatsBridge([]() {
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

    const ZImgInfo firstBlockInfo = firstBlock->readInfo();
    EXPECT_EQ(7u, firstBlockInfo.width);
    EXPECT_EQ(5u, firstBlockInfo.height);
    EXPECT_EQ(1u, firstBlockInfo.depth);
    EXPECT_EQ(2u, firstBlockInfo.numChannels);
    EXPECT_EQ(1u, firstBlockInfo.numTimes);

    const std::shared_ptr<ZImg> firstBlockImg = firstBlock->read();
    ASSERT_NE(nullptr, firstBlockImg);
    EXPECT_EQ(7u, firstBlockImg->width());
    EXPECT_EQ(5u, firstBlockImg->height());
    EXPECT_EQ(1u, firstBlockImg->depth());
    EXPECT_EQ(2u, firstBlockImg->numChannels());
    EXPECT_EQ(1u, firstBlockImg->numTimes());
  });
}

TEST(ZBioFormatsTest, CompleteReadImgAttachesMetadataAndPixelsOnlySkipsIt)
{
  runWithBioFormatsBridge([]() {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path =
      createFakeReaderFile(dir, QStringLiteral("read-policy&sizeX=9&sizeY=7&sizeZ=1&sizeC=1&sizeT=1&pixelType=uint8"));

    ZImgMetadata explicitMetadata = ZImg::readImgMetadata(ZImgSource(path, ZImgRegion(), 0, FileFormat::BioFormats));

    ZImg completeImg;
    ZImgIO::instance().readImg(path, completeImg, ZImgRegion(), 0, 1, 1, 1, FileFormat::BioFormats);
    ASSERT_TRUE(completeImg.metadata().hasTopLevelAttachment());
    ASSERT_TRUE(explicitMetadata.hasTopLevelAttachment());

    auto metadataNames = [](const ZImgMetadata& metadata) {
      std::vector<std::string> names;
      for (const ZImgMetatag& tag : metadata.topLevelAttachments()) {
        names.push_back(tag.name());
      }
      std::sort(names.begin(), names.end());
      return names;
    };
    EXPECT_EQ(metadataNames(explicitMetadata), metadataNames(completeImg.metadata()));

    ZImg pixelsOnlyImg = ZImg::readImgPixelsOnly(path, ZImgRegion(), 0, 1, 1, 1, FileFormat::BioFormats);
    EXPECT_TRUE(pixelsOnlyImg.metadata().isEmpty());
    EXPECT_FALSE(pixelsOnlyImg.hasThumbnail());
    EXPECT_TRUE(pixelsOnlyImg.info().isSameSize(completeImg.info()));
    EXPECT_TRUE(pixelsOnlyImg.info().isSameType(completeImg.info()));
  });
}

TEST(ZBioFormatsTest, IndexedFalseColorDataStaysSingleChannel)
{
  runWithBioFormatsBridge([]() {
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
  runWithBioFormatsBridge([]() {
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
  runWithBioFormatsBridge([]() {
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
  runWithBioFormatsBridge([]() {
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
  const ScopedBioFormatsBridgeTestProcess scopedBridge;
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
  runWithBioFormatsBridge([]() {
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
  runWithBioFormatsBridge([]() {
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

TEST(ZBioFormatsTest, NativeMicroscopyCorpusExamplesMatchBioFormats)
{
  initializePublicCorpusLogging();
  configureBioFormatsTestBridge();

  const std::optional<QString> corpusRootPath = publicCorpusRootPath();
  if (!corpusRootPath.has_value()) {
    GTEST_SKIP() << "set ATLAS_BIOFORMATS_BREADTH_DIR to run native-vs-Bio-Formats corpus comparison tests";
  }
  const std::vector<CorpusManifestFile> files = publicCorpusFiles();
  CHECK(!files.empty());
  const std::set<QString> fullManifestRelativePaths =
    corpusRelativePathsFromManifest(*corpusRootPath, QStringLiteral("full_manifest.json"));

  const CorpusScenePolicy scenePolicy = publicCorpusScenePolicy();
  const size_t sceneSampleLimit = nativeComparisonSceneSampleLimit();
  const bool failFast = publicCorpusBooleanEnv("ATLAS_NATIVE_BIOFORMATS_COMPARE_FAIL_FAST", false);
  const bool logRegions = publicCorpusBooleanEnv("ATLAS_NATIVE_BIOFORMATS_COMPARE_LOG_REGIONS", true);
  const std::vector<NativeBioFormatsComparisonCandidate> candidates = nativeBioFormatsComparisonCandidates(files);

  emitNativeComparisonProgress(
    fmt::format("starting native-vs-Bio-Formats corpus comparison manifest_files={} candidates={} candidate_counts={} "
                "scene_policy={} scene_sample_limit={} fail_fast={} log_regions={}",
                files.size(),
                candidates.size(),
                nativeComparisonCandidateCountsToString(candidates),
                corpusScenePolicyName(scenePolicy),
                sceneSampleLimit,
                failFast ? "true" : "false",
                logRegions ? "true" : "false"));
  if (candidates.empty()) {
    GTEST_SKIP() << "no native microscope corpus examples matched Atlas native microscope reader extensions";
  }

  const ScopedBioFormatsBridgeTestProcess scopedBridge;
  if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
    GTEST_SKIP() << *reason;
  }

  std::vector<CorpusFailure> failureRecords;
  size_t comparedFiles = 0;
  size_t comparedScenes = 0;
  size_t comparedRegions = 0;
  size_t missingFiles = 0;
  size_t missingFilesOutsideFullManifest = 0;
  size_t unavailableCorpusFailures = 0;
  const auto startTime = std::chrono::steady_clock::now();
  auto recordFailure = [&](const QString& path, std::string message) {
    failureRecords.push_back({path, std::move(message)});
    emitNativeComparisonProgress(
      fmt::format("failure count={} message=\"{}\"", failureRecords.size(), failureRecords.back().message));
    if (failFast) {
      ADD_FAILURE() << failureRecords.back().message;
      return true;
    }
    return false;
  };

  for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
    const NativeBioFormatsComparisonCandidate& candidate = candidates[candidateIndex];
    CHECK(candidate.rule != nullptr);
    const CorpusManifestFile& file = candidate.file;
    SCOPED_TRACE(file.relativePath.toStdString());

    if (!QFileInfo(file.absolutePath).exists()) {
      if (!fullManifestRelativePaths.contains(file.relativePath)) {
        ++missingFilesOutsideFullManifest;
        emitNativeComparisonProgress(fmt::format(
          "missing candidate is absent from full_manifest.json and will not be treated as a failure: {} ({})",
          file.relativePath.toStdString(),
          file.absolutePath.toStdString()));
      } else {
        ++missingFiles;
        if (recordFailure(file.absolutePath,
                          fmt::format("native-vs-Bio-Formats corpus candidate is missing on disk: {} ({})",
                                      file.relativePath.toStdString(),
                                      file.absolutePath.toStdString()))) {
          return;
        }
      }
      continue;
    }

    auto ignoreUnavailableCorpusFailure = [&](const char* error) {
      const std::optional<std::string> reason =
        missingReferencedFileOutsideFullManifestReason(*corpusRootPath, fullManifestRelativePaths, error);
      if (!reason.has_value()) {
        return false;
      }
      ++unavailableCorpusFailures;
      emitNativeComparisonProgress(fmt::format(
        "referenced corpus file is unavailable and will not be treated as a failure relative_path={} reason=\"{}\"",
        file.relativePath.toStdString(),
        *reason));
      return true;
    };

    emitNativeComparisonProgress(fmt::format("opening candidate={}/{} format={} native_format={} relative_path={} "
                                             "size_bytes={} elapsed={:.1f}s",
                                             candidateIndex + 1,
                                             candidates.size(),
                                             candidate.rule->topLevelFormat.toStdString(),
                                             enumToString(candidate.rule->nativeFormat),
                                             file.relativePath.toStdString(),
                                             file.sizeBytes,
                                             secondsSince(startTime)));

    std::vector<ZImgInfo> nativeInfos;
    std::vector<ZImgInfo> bioFormatsInfos;
    try {
      ZImgIO::instance().readInfos(file.absolutePath, nativeInfos, nullptr, candidate.rule->nativeFormat);
    }
    catch (const std::exception& e) {
      if (ignoreUnavailableCorpusFailure(e.what())) {
        continue;
      }
      if (recordFailure(file.absolutePath,
                        fmt::format("native {} readInfo failed for {}: {}",
                                    enumToString(candidate.rule->nativeFormat),
                                    file.relativePath.toStdString(),
                                    e.what()))) {
        return;
      }
      continue;
    }
    try {
      ZImgIO::instance().readInfos(file.absolutePath, bioFormatsInfos, nullptr, FileFormat::BioFormats);
    }
    catch (const std::exception& e) {
      if (ignoreUnavailableCorpusFailure(e.what())) {
        continue;
      }
      if (recordFailure(file.absolutePath,
                        fmt::format("Bio-Formats readInfo failed for native comparison candidate {}: {}",
                                    file.relativePath.toStdString(),
                                    e.what()))) {
        return;
      }
      continue;
    }

    ++comparedFiles;
    emitNativeComparisonProgress(fmt::format("info complete candidate={}/{} relative_path={} native_scenes={} "
                                             "bioformats_scenes={} elapsed={:.1f}s",
                                             candidateIndex + 1,
                                             candidates.size(),
                                             file.relativePath.toStdString(),
                                             nativeInfos.size(),
                                             bioFormatsInfos.size(),
                                             secondsSince(startTime)));

    if (nativeInfos.empty() || bioFormatsInfos.empty()) {
      if (recordFailure(file.absolutePath,
                        fmt::format("native-vs-Bio-Formats comparison has empty scene list for {}: native={} "
                                    "bioformats={}",
                                    file.relativePath.toStdString(),
                                    nativeInfos.size(),
                                    bioFormatsInfos.size()))) {
        return;
      }
      continue;
    }

    const NativeBioFormatsSceneMapping sceneMapping =
      buildNativeBioFormatsSceneMapping(*candidate.rule, nativeInfos, bioFormatsInfos);
    if (sceneMapping.note.has_value()) {
      emitNativeComparisonProgress(fmt::format("scene mapping candidate={}/{} relative_path={} note=\"{}\"",
                                               candidateIndex + 1,
                                               candidates.size(),
                                               file.relativePath.toStdString(),
                                               *sceneMapping.note));
    }

    if (!sceneMapping.sceneCountsAreCompatible) {
      if (recordFailure(file.absolutePath,
                        fmt::format("scene count differs for {}: native={} bioformats={}",
                                    file.relativePath.toStdString(),
                                    nativeInfos.size(),
                                    bioFormatsInfos.size()))) {
        return;
      }
    }

    const size_t commonSceneCount = sceneMapping.pairs.size();
    const std::vector<size_t> scenesToRead =
      corpusScenesToRead(commonSceneCount, scenePolicy, file.relativePath, sceneSampleLimit);
    emitNativeComparisonProgress(fmt::format("scene selection candidate={}/{} relative_path={} policy={} "
                                             "selected_scenes={} total_scenes={} selected_scene_pairs={} "
                                             "elapsed={:.1f}s",
                                             candidateIndex + 1,
                                             candidates.size(),
                                             file.relativePath.toStdString(),
                                             corpusScenePolicyName(scenePolicy),
                                             scenesToRead.size(),
                                             commonSceneCount,
                                             nativeBioFormatsScenePairListToString(scenesToRead, sceneMapping.pairs),
                                             secondsSince(startTime)));
    for (size_t pairIndex : scenesToRead) {
      CHECK(pairIndex < sceneMapping.pairs.size());
      const NativeBioFormatsScenePair& scenePair = sceneMapping.pairs[pairIndex];
      SCOPED_TRACE(
        fmt::format("native scene {} Bio-Formats scene {}", scenePair.nativeScene, scenePair.bioFormatsScene));
      const std::vector<std::string> infoMismatches =
        compareNativeBioFormatsCoreInfo(nativeInfos[scenePair.nativeScene], bioFormatsInfos[scenePair.bioFormatsScene]);
      if (!infoMismatches.empty()) {
        if (recordFailure(file.absolutePath,
                          fmt::format("scene core metadata differs for {} native scene {} Bio-Formats scene {}:\n{}",
                                      file.relativePath.toStdString(),
                                      scenePair.nativeScene,
                                      scenePair.bioFormatsScene,
                                      fmt::join(infoMismatches, "\n")))) {
          return;
        }
        continue;
      }

      ++comparedScenes;
      const std::optional<std::vector<size_t>> nativeToBioFormatsChannelMap =
        nativeToBioFormatsChannelMapByColor(*candidate.rule,
                                            nativeInfos[scenePair.nativeScene],
                                            bioFormatsInfos[scenePair.bioFormatsScene]);
      if (nativeToBioFormatsChannelMap.has_value()) {
        emitNativeComparisonProgress(
          fmt::format("channel comparison mapping candidate={}/{} relative_path={} native_scene={} "
                      "bioformats_scene={} native_to_bioformats_channels={} native_colors={} bioformats_colors={} "
                      "note=\"native Leica preserves acquisition/file channel order; Bio-Formats exposes these "
                      "channels in display-color order\"",
                      candidateIndex + 1,
                      candidates.size(),
                      file.relativePath.toStdString(),
                      scenePair.nativeScene,
                      scenePair.bioFormatsScene,
                      channelMapToString(*nativeToBioFormatsChannelMap),
                      channelColorsRgbToString(nativeInfos[scenePair.nativeScene].channelColors),
                      channelColorsRgbToString(bioFormatsInfos[scenePair.bioFormatsScene].channelColors)));
      }
      const std::vector<ZImgRegion> regions = smokeRegions(nativeInfos[scenePair.nativeScene]);
      for (size_t regionIndex = 0; regionIndex < regions.size(); ++regionIndex) {
        const ZImgRegion& region = regions[regionIndex];
        const std::string regionDescription = regionToString(region);
        if (logRegions) {
          emitNativeComparisonProgress(fmt::format("reading region candidate={}/{} relative_path={} "
                                                   "native_scene={} bioformats_scene={} "
                                                   "region={}/{} bounds=\"{}\" elapsed={:.1f}s",
                                                   candidateIndex + 1,
                                                   candidates.size(),
                                                   file.relativePath.toStdString(),
                                                   scenePair.nativeScene,
                                                   scenePair.bioFormatsScene,
                                                   regionIndex + 1,
                                                   regions.size(),
                                                   regionDescription,
                                                   secondsSince(startTime)));
        }

        ZImg nativeRegion;
        ZImg bioFormatsRegion;
        try {
          ZImgIO::instance().readImg(file.absolutePath,
                                     nativeRegion,
                                     region,
                                     scenePair.nativeScene,
                                     1,
                                     1,
                                     1,
                                     candidate.rule->nativeFormat);
        }
        catch (const std::exception& e) {
          if (ignoreUnavailableCorpusFailure(e.what())) {
            continue;
          }
          if (recordFailure(file.absolutePath,
                            fmt::format("native {} region read failed for {} scene {} region \"{}\": {}",
                                        enumToString(candidate.rule->nativeFormat),
                                        file.relativePath.toStdString(),
                                        scenePair.nativeScene,
                                        regionDescription,
                                        e.what()))) {
            return;
          }
          continue;
        }
        try {
          ZImgIO::instance().readImg(file.absolutePath,
                                     bioFormatsRegion,
                                     region,
                                     scenePair.bioFormatsScene,
                                     1,
                                     1,
                                     1,
                                     FileFormat::BioFormats);
        }
        catch (const std::exception& e) {
          if (ignoreUnavailableCorpusFailure(e.what())) {
            continue;
          }
          if (recordFailure(file.absolutePath,
                            fmt::format("Bio-Formats region read failed for {} scene {} region \"{}\": {}",
                                        file.relativePath.toStdString(),
                                        scenePair.bioFormatsScene,
                                        regionDescription,
                                        e.what()))) {
            return;
          }
          continue;
        }

        if (const std::optional<std::string> firstDifference =
              nativeBioFormatsPixelDifferenceDescription(nativeRegion, bioFormatsRegion, nativeToBioFormatsChannelMap);
            firstDifference.has_value()) {
          if (recordFailure(file.absolutePath,
                            fmt::format("native and Bio-Formats pixels differ for {} native scene {} "
                                        "Bio-Formats scene {} region \"{}\"{}{}",
                                        file.relativePath.toStdString(),
                                        scenePair.nativeScene,
                                        scenePair.bioFormatsScene,
                                        regionDescription,
                                        firstDifference->empty() ? "" : ": ",
                                        *firstDifference))) {
            return;
          }
          continue;
        }
        ++comparedRegions;
      }
    }

    emitNativeComparisonProgress(fmt::format("candidate complete candidate={}/{} relative_path={} compared_files={} "
                                             "compared_scenes={} compared_regions={} failures={} elapsed={:.1f}s",
                                             candidateIndex + 1,
                                             candidates.size(),
                                             file.relativePath.toStdString(),
                                             comparedFiles,
                                             comparedScenes,
                                             comparedRegions,
                                             failureRecords.size(),
                                             secondsSince(startTime)));
  }

  emitNativeComparisonProgress(
    fmt::format("finished native-vs-Bio-Formats corpus comparison elapsed={:.1f}s candidates={} compared_files={} "
                "compared_scenes={} compared_regions={} missing_files={} missing_files_outside_full_manifest={} "
                "unavailable_corpus_failures={} failures={}",
                secondsSince(startTime),
                candidates.size(),
                comparedFiles,
                comparedScenes,
                comparedRegions,
                missingFiles,
                missingFilesOutsideFullManifest,
                unavailableCorpusFailures,
                failureRecords.size()));

  for (const CorpusFailure& failure : failureRecords) {
    ADD_FAILURE() << failure.message;
  }
  RecordProperty("native_bioformats_candidates", static_cast<int>(candidates.size()));
  RecordProperty("native_bioformats_compared_files", static_cast<int>(comparedFiles));
  RecordProperty("native_bioformats_compared_scenes", static_cast<int>(comparedScenes));
  RecordProperty("native_bioformats_compared_regions", static_cast<int>(comparedRegions));
  RecordProperty("native_bioformats_missing_files", static_cast<int>(missingFiles));
  RecordProperty("native_bioformats_missing_files_outside_full_manifest",
                 static_cast<int>(missingFilesOutsideFullManifest));
  RecordProperty("native_bioformats_unavailable_corpus_failures", static_cast<int>(unavailableCorpusFailures));
  RecordProperty("native_bioformats_failures", static_cast<int>(failureRecords.size()));
  RecordProperty("native_bioformats_scene_policy", corpusScenePolicyName(scenePolicy));
  RecordProperty("native_bioformats_scene_sample_limit", fmt::format("{}", sceneSampleLimit));
  EXPECT_GT(comparedRegions, 0u);
  EXPECT_TRUE(failureRecords.empty());
}

TEST(ZBioFormatsTest, PublicCorpusReadsMetadataAndSmallRegions)
{
  initializePublicCorpusLogging();
  configureBioFormatsTestBridge();

  const std::optional<QString> corpusRootPath = publicCorpusRootPath();
  if (!corpusRootPath.has_value()) {
    GTEST_SKIP() << "set ATLAS_BIOFORMATS_BREADTH_DIR to run public Bio-Formats corpus smoke tests";
  }
  const std::vector<CorpusManifestFile> files = publicCorpusFiles();
  CHECK(!files.empty());
  const std::set<QString> fullManifestRelativePaths =
    corpusRelativePathsFromManifest(*corpusRootPath, QStringLiteral("full_manifest.json"));
  const std::set<QString> excludedFormats = excludedPublicCorpusFormats();
  const std::set<QString> includedFormats = includedPublicCorpusFormats();
  const QStringList pathSubstrings = publicCorpusPathSubstrings();
  const size_t progressInterval = publicCorpusProgressInterval();
  const bool logDriverFiles = publicCorpusLogDriverFiles();
  const bool logRegionReads = publicCorpusLogRegionReads();
  const bool failFast = publicCorpusFailFast();
  const bool compareMetadataLevels = publicCorpusCompareMetadataLevels();
  const CorpusScenePolicy scenePolicy = publicCorpusScenePolicy();
  const size_t sceneSampleLimit = publicCorpusSceneSampleLimit();
  std::set<QString> manifestFormats;
  size_t targetFiles = 0;
  size_t targetDriverFiles = 0;
  size_t excludedManifestFiles = 0;
  size_t filteredManifestFiles = 0;
  size_t pathFilteredManifestFiles = 0;
  auto formatIsInScope = [&](const QString& format) {
    if (excludedFormats.contains(format)) {
      return false;
    }
    return includedFormats.empty() || includedFormats.contains(format);
  };
  auto pathIsInScope = [&](const QString& relativePath) {
    if (pathSubstrings.empty()) {
      return true;
    }
    return std::any_of(pathSubstrings.begin(), pathSubstrings.end(), [&](const QString& pathSubstring) {
      return relativePath.contains(pathSubstring);
    });
  };
  for (const CorpusManifestFile& file : files) {
    const QString format = topLevelFormat(file.relativePath);
    manifestFormats.insert(format);
    if (excludedFormats.contains(format)) {
      ++excludedManifestFiles;
    } else if (!formatIsInScope(format)) {
      ++filteredManifestFiles;
    } else if (!pathIsInScope(file.relativePath)) {
      ++pathFilteredManifestFiles;
    } else {
      ++targetFiles;
      const std::optional<bool> isDriver = isCorpusDriverFile(file.relativePath);
      if (isDriver.has_value() && *isDriver) {
        ++targetDriverFiles;
      }
    }
  }
  std::set<QString> unmatchedExcludedFormats;
  for (const QString& format : excludedFormats) {
    if (!manifestFormats.contains(format)) {
      unmatchedExcludedFormats.insert(format);
    }
  }
  std::set<QString> unmatchedIncludedFormats;
  for (const QString& format : includedFormats) {
    if (!manifestFormats.contains(format)) {
      unmatchedIncludedFormats.insert(format);
    }
  }
  emitCorpusProgress(
    fmt::format("starting public corpus smoke test manifest_files={} full_manifest_files={} target_files={} "
                "target_driver_files={} excluded_files={} "
                "filtered_files={} path_filtered_files={} excluded_formats={} only_formats={} path_filters={} "
                "progress_interval={} log_driver_files={} "
                "log_region_reads={} fail_fast={} compare_metadata_levels={} scene_policy={} scene_sample_limit={} "
                "bridge_io_timeout_ms={}",
                files.size(),
                fullManifestRelativePaths.size(),
                targetFiles,
                targetDriverFiles,
                excludedManifestFiles,
                filteredManifestFiles,
                pathFilteredManifestFiles,
                corpusFormatSetToString(excludedFormats),
                corpusFormatSetToString(includedFormats),
                corpusPathSubstringListToString(pathSubstrings),
                progressInterval,
                logDriverFiles ? "true" : "false",
                logRegionReads ? "true" : "false",
                failFast ? "true" : "false",
                compareMetadataLevels ? "true" : "false",
                corpusScenePolicyName(scenePolicy),
                sceneSampleLimit,
                ::FLAGS_atlas_bioformats_bridge_io_timeout_ms));
  if (!unmatchedExcludedFormats.empty()) {
    emitCorpusProgress(fmt::format("requested excluded formats not found in manifest: {}",
                                   corpusFormatSetToString(unmatchedExcludedFormats)));
  }
  if (!unmatchedIncludedFormats.empty()) {
    emitCorpusProgress(fmt::format("requested only formats not found in manifest: {}",
                                   corpusFormatSetToString(unmatchedIncludedFormats)));
  }

  const ScopedBioFormatsBridgeTestProcess scopedBridge;
  if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
    GTEST_SKIP() << *reason;
  }

  ScopedWarningLogCapture warningCapture;
  std::set<QString> coveredCompanionFiles;
  std::set<QString> coveredCorpusDatasetGroups;
  std::set<QString> formatsWithoutDriverRules;
  std::set<QString> formatsWithoutSelectedDrivers;
  std::set<QString> formatsWithoutCheckedDrivers;
  std::map<QString, CorpusFormatStats> formatStats;
  size_t openedFiles = 0;
  size_t missingFiles = 0;
  size_t selectedDriverFiles = 0;
  size_t skippedNonDriverFiles = 0;
  size_t skippedExcludedFormatFiles = 0;
  size_t skippedFilteredFormatFiles = 0;
  size_t skippedPathFilteredFiles = 0;
  size_t skippedCoveredCompanionFiles = 0;
  size_t skippedIncompleteCorpusFiles = 0;
  size_t skippedIncompleteCorpusDriverFiles = 0;
  size_t missingFilesOutsideFullManifest = 0;
  size_t processedTargetFiles = 0;
  size_t attemptedRegionReads = 0;
  size_t successfulRegionReads = 0;
  size_t expectedBioFormatsOpenFailures = 0;
  size_t expectedBioFormatsRegionFailures = 0;
  size_t unavailableCorpusOpenFailures = 0;
  size_t unavailableCorpusRegionFailures = 0;
  size_t metadataLevelComparisons = 0;
  size_t metadataLevelMismatchFailures = 0;
  std::vector<CorpusFailure> openFailureRecords;
  std::vector<CorpusFailure> regionFailureRecords;
  std::vector<CorpusFailure> warningFailureRecords;
  std::vector<CorpusFailure> metadataLevelMismatchRecords;
  const auto startTime = std::chrono::steady_clock::now();
  std::optional<QString> activeFormat;
  auto reportActiveFormat = [&]() {
    if (!activeFormat.has_value()) {
      return;
    }
    const auto it = formatStats.find(*activeFormat);
    if (it != formatStats.end()) {
      emitCorpusFormatSummary(*activeFormat, it->second, processedTargetFiles, targetFiles, startTime);
    }
  };
  auto maybeStopAfterFailure = [&](const CorpusFailure& failure) {
    if (!failFast) {
      return false;
    }
    reportActiveFormat();
    ADD_FAILURE() << "Stopping Bio-Formats corpus smoke test after first unhandled failure because "
                     "ATLAS_BIOFORMATS_BREADTH_FAIL_FAST is enabled:\n"
                  << failure.message;
    return true;
  };
  auto maybeReportProgress = [&]() {
    if (progressInterval == 0 || processedTargetFiles == 0 || processedTargetFiles % progressInterval != 0) {
      return;
    }
    emitCorpusProgress(fmt::format("checkpoint processed={}/{} elapsed={:.1f}s selected_drivers={} opened_files={} "
                                   "regions={}/{} open_failures={} region_failures={} warning_failures={} "
                                   "skipped_incomplete_corpus_files={} skipped_incomplete_corpus_drivers={} "
                                   "metadata_level_comparisons={} metadata_level_mismatch_failures={} "
                                   "expected_bioformats_open_failures={} expected_bioformats_region_failures={} "
                                   "unavailable_corpus_open_failures={} unavailable_corpus_region_failures={}",
                                   processedTargetFiles,
                                   targetFiles,
                                   secondsSince(startTime),
                                   selectedDriverFiles,
                                   openedFiles,
                                   successfulRegionReads,
                                   attemptedRegionReads,
                                   openFailureRecords.size(),
                                   regionFailureRecords.size(),
                                   warningFailureRecords.size(),
                                   skippedIncompleteCorpusFiles,
                                   skippedIncompleteCorpusDriverFiles,
                                   metadataLevelComparisons,
                                   metadataLevelMismatchFailures,
                                   expectedBioFormatsOpenFailures,
                                   expectedBioFormatsRegionFailures,
                                   unavailableCorpusOpenFailures,
                                   unavailableCorpusRegionFailures));
  };
  for (const CorpusManifestFile& file : files) {
    const QString& path = file.absolutePath;
    const QString format = topLevelFormat(file.relativePath);
    if (excludedFormats.contains(format)) {
      ++skippedExcludedFormatFiles;
      continue;
    }
    if (!formatIsInScope(format)) {
      ++skippedFilteredFormatFiles;
      continue;
    }
    if (!pathIsInScope(file.relativePath)) {
      ++skippedPathFilteredFiles;
      continue;
    }
    ++processedTargetFiles;
    if (!activeFormat.has_value() || *activeFormat != format) {
      reportActiveFormat();
      activeFormat = format;
      emitCorpusProgress(fmt::format("starting format={} processed={}/{} elapsed={:.1f}s",
                                     format.toStdString(),
                                     processedTargetFiles,
                                     targetFiles,
                                     secondsSince(startTime)));
    }
    CorpusFormatStats& stats = formatStats[format];
    ++stats.manifestFiles;
    const std::optional<bool> isDriver = isCorpusDriverFile(file.relativePath);
    if (isDriver.has_value() && *isDriver) {
      ++selectedDriverFiles;
      ++stats.selectedDriverFiles;
    }

    SCOPED_TRACE(file.relativePath.toStdString());
    if (!QFileInfo(path).exists()) {
      if (!fullManifestRelativePaths.contains(file.relativePath)) {
        ++missingFiles;
        ++stats.missingFiles;
        ++missingFilesOutsideFullManifest;
        ++stats.missingFilesOutsideFullManifest;
        if (isDriver.has_value() && *isDriver) {
          ++stats.missingDriverFiles;
        }
        emitCorpusProgress(fmt::format("missing file is absent from full_manifest.json and will not be treated as a "
                                       "bridge failure relative_path={} absolute_path={}",
                                       file.relativePath.toStdString(),
                                       path.toStdString()));
        maybeReportProgress();
        continue;
      }
      const std::optional<std::string> incompleteReason = knownIncompleteCorpusEntryReason(file.relativePath);
      if (incompleteReason.has_value()) {
        emitCorpusProgress(fmt::format("known incomplete corpus entry is missing and will be reported as a failure "
                                       "relative_path={} absolute_path={} reason={}",
                                       file.relativePath.toStdString(),
                                       path.toStdString(),
                                       *incompleteReason));
      }
      ++missingFiles;
      ++stats.missingFiles;
      if (isDriver.has_value() && *isDriver) {
        ++stats.missingDriverFiles;
      }
      emitCorpusProgress(fmt::format("missing file relative_path={} absolute_path={}",
                                     file.relativePath.toStdString(),
                                     path.toStdString()));
      std::string message = fmt::format("Bio-Formats corpus manifest entry is missing on disk: {} ({})",
                                        file.relativePath.toStdString(),
                                        path.toStdString());
      if (incompleteReason.has_value()) {
        message += fmt::format("\nknown incomplete corpus note: {}", *incompleteReason);
      }
      const CorpusFailure failure{path, std::move(message)};
      ADD_FAILURE() << failure.message;
      if (maybeStopAfterFailure(failure)) {
        return;
      }
      maybeReportProgress();
      continue;
    }

    if (!isDriver.has_value()) {
      formatsWithoutDriverRules.insert(format);
      ++skippedNonDriverFiles;
      ++stats.skippedNonDriverFiles;
      maybeReportProgress();
      continue;
    }
    if (!*isDriver) {
      ++skippedNonDriverFiles;
      ++stats.skippedNonDriverFiles;
      maybeReportProgress();
      continue;
    }
    if (const auto reason = knownIncompleteCorpusDriverReason(file.relativePath, fullManifestRelativePaths);
        reason.has_value()) {
      emitCorpusProgress(fmt::format("known incomplete corpus driver will still be opened relative_path={} "
                                     "absolute_path={} reason={}",
                                     file.relativePath.toStdString(),
                                     path.toStdString(),
                                     *reason));
    }

    const QString canonicalPath = canonicalExistingPath(path);
    if (coveredCompanionFiles.contains(canonicalPath)) {
      ++skippedCoveredCompanionFiles;
      ++stats.skippedCoveredCompanionFiles;
      ++stats.coveredDriverFiles;
      maybeReportProgress();
      continue;
    }
    const std::optional<QString> datasetGroupKey = corpusDatasetGroupKey(file.relativePath);
    if (datasetGroupKey.has_value() && coveredCorpusDatasetGroups.contains(*datasetGroupKey)) {
      ++skippedCoveredCompanionFiles;
      ++stats.skippedCoveredCompanionFiles;
      ++stats.coveredDriverFiles;
      if (logDriverFiles) {
        emitCorpusProgress(fmt::format("skipping covered dataset companion relative_path={} group_key={}",
                                       file.relativePath.toStdString(),
                                       datasetGroupKey->toStdString()));
      }
      maybeReportProgress();
      continue;
    }

    if (logDriverFiles) {
      emitCorpusProgress(fmt::format("opening driver {}/{} corpus_file={}/{} relative_path={} elapsed={:.1f}s",
                                     selectedDriverFiles,
                                     targetDriverFiles,
                                     processedTargetFiles,
                                     targetFiles,
                                     file.relativePath.toStdString(),
                                     secondsSince(startTime)));
    }
    warningCapture.clear();
    ZBioFormatsDatasetInfo dataset;
    std::vector<ZImgInfo> infos;
    try {
      if (logDriverFiles) {
        emitCorpusProgress(fmt::format("reading dataset info driver={}/{} corpus_file={}/{} relative_path={} "
                                       "elapsed={:.1f}s",
                                       selectedDriverFiles,
                                       targetDriverFiles,
                                       processedTargetFiles,
                                       targetFiles,
                                       file.relativePath.toStdString(),
                                       secondsSince(startTime)));
      }
      dataset = ZBioFormatsBridgeClient::instance().readDatasetInfo(path, true);
      if (logDriverFiles) {
        emitCorpusProgress(fmt::format("dataset info complete driver={}/{} relative_path={} format={} reader={} "
                                       "series={} used_files={} elapsed={:.1f}s",
                                       selectedDriverFiles,
                                       targetDriverFiles,
                                       file.relativePath.toStdString(),
                                       dataset.formatName.toStdString(),
                                       dataset.readerClass.toStdString(),
                                       dataset.series.size(),
                                       dataset.usedFiles.size(),
                                       secondsSince(startTime)));
      }
      if (compareMetadataLevels) {
        ++stats.metadataLevelComparisons;
        ++metadataLevelComparisons;
        if (logDriverFiles) {
          emitCorpusProgress(fmt::format("comparing metadata levels driver={}/{} corpus_file={}/{} relative_path={} "
                                         "filtered=NO_OVERLAYS full=ALL elapsed={:.1f}s",
                                         selectedDriverFiles,
                                         targetDriverFiles,
                                         processedTargetFiles,
                                         targetFiles,
                                         file.relativePath.toStdString(),
                                         secondsSince(startTime)));
        }
        try {
          const ZBioFormatsDatasetInfo fullDataset = ZBioFormatsBridgeClient::instance().readDatasetInfo(path, false);
          const std::vector<std::string> mismatches = compareCorpusMetadataLevelCoreInfo(dataset, fullDataset);
          if (!mismatches.empty()) {
            ++stats.metadataLevelMismatchFailures;
            ++metadataLevelMismatchFailures;
            emitCorpusProgress(fmt::format("metadata level mismatch relative_path={} absolute_path={} count={} "
                                           "details=\"{}\"",
                                           file.relativePath.toStdString(),
                                           path.toStdString(),
                                           mismatches.size(),
                                           fmt::join(mismatches, "; ")));
            metadataLevelMismatchRecords.push_back(
              {canonicalPath,
               fmt::format("Bio-Formats metadata level core/tile mismatch for {}:\n{}",
                           path.toStdString(),
                           fmt::join(mismatches, "\n"))});
            if (maybeStopAfterFailure(metadataLevelMismatchRecords.back())) {
              return;
            }
          } else if (logDriverFiles) {
            emitCorpusProgress(fmt::format("metadata level comparison complete driver={}/{} relative_path={} "
                                           "mismatches=0 elapsed={:.1f}s",
                                           selectedDriverFiles,
                                           targetDriverFiles,
                                           file.relativePath.toStdString(),
                                           secondsSince(startTime)));
          }
        }
        catch (const std::exception& e) {
          ++stats.metadataLevelMismatchFailures;
          ++metadataLevelMismatchFailures;
          emitCorpusProgress(fmt::format("metadata level comparison failure relative_path={} absolute_path={} error={}",
                                         file.relativePath.toStdString(),
                                         path.toStdString(),
                                         e.what()));
          metadataLevelMismatchRecords.push_back(
            {canonicalPath,
             fmt::format("failed to compare Bio-Formats metadata levels for {}: {}", path.toStdString(), e.what())});
          if (maybeStopAfterFailure(metadataLevelMismatchRecords.back())) {
            return;
          }
        }
      }
      if (logDriverFiles) {
        emitCorpusProgress(fmt::format("reading Atlas infos driver={}/{} corpus_file={}/{} relative_path={} "
                                       "elapsed={:.1f}s",
                                       selectedDriverFiles,
                                       targetDriverFiles,
                                       processedTargetFiles,
                                       targetFiles,
                                       file.relativePath.toStdString(),
                                       secondsSince(startTime)));
      }
      ZImgIO::instance().readInfos(path, infos, nullptr, FileFormat::BioFormats);
      if (logDriverFiles) {
        emitCorpusProgress(fmt::format("Atlas infos complete driver={}/{} relative_path={} scenes={} elapsed={:.1f}s",
                                       selectedDriverFiles,
                                       targetDriverFiles,
                                       file.relativePath.toStdString(),
                                       infos.size(),
                                       secondsSince(startTime)));
      }
    }
    catch (const std::exception& e) {
      const std::optional<std::string> knownBioFormatsReason =
        expectedBioFormatsFailureReason(file.relativePath, CorpusFailureStage::Open, e.what());
      if (knownBioFormatsReason.has_value()) {
        ++stats.expectedBioFormatsOpenFailures;
        ++expectedBioFormatsOpenFailures;
        emitCorpusProgress(fmt::format("expected Bio-Formats jar open failure relative_path={} "
                                       "absolute_path={} reason={} error={}",
                                       file.relativePath.toStdString(),
                                       path.toStdString(),
                                       *knownBioFormatsReason,
                                       e.what()));
        maybeReportProgress();
        continue;
      }
      const std::optional<std::string> unavailableCorpusReason =
        missingReferencedFileOutsideFullManifestReason(*corpusRootPath, fullManifestRelativePaths, e.what());
      if (unavailableCorpusReason.has_value()) {
        ++stats.unavailableCorpusOpenFailures;
        ++unavailableCorpusOpenFailures;
        emitCorpusProgress(fmt::format("unavailable corpus companion open failure relative_path={} "
                                       "absolute_path={} reason={} error={}",
                                       file.relativePath.toStdString(),
                                       path.toStdString(),
                                       *unavailableCorpusReason,
                                       e.what()));
        maybeReportProgress();
        continue;
      }
      ++stats.openFailures;
      emitCorpusProgress(fmt::format("open failure relative_path={} absolute_path={} error={}",
                                     file.relativePath.toStdString(),
                                     path.toStdString(),
                                     e.what()));
      openFailureRecords.push_back(
        {canonicalPath, fmt::format("failed to open Bio-Formats corpus file {}: {}", path.toStdString(), e.what())});
      if (maybeStopAfterFailure(openFailureRecords.back())) {
        return;
      }
      maybeReportProgress();
      continue;
    }
    if (infos.empty()) {
      ++stats.openFailures;
      emitCorpusProgress(fmt::format("open failure relative_path={} absolute_path={} error=no readable series",
                                     file.relativePath.toStdString(),
                                     path.toStdString()));
      openFailureRecords.push_back(
        {canonicalPath, fmt::format("Bio-Formats corpus file has no readable series: {}", path.toStdString())});
      if (maybeStopAfterFailure(openFailureRecords.back())) {
        return;
      }
      maybeReportProgress();
      continue;
    }
    ++openedFiles;
    ++stats.openedDriverFiles;

    const std::vector<size_t> scenesToRead =
      corpusScenesToRead(infos.size(), scenePolicy, file.relativePath, sceneSampleLimit);
    if (logDriverFiles) {
      emitCorpusProgress(fmt::format("scene selection driver={}/{} relative_path={} policy={} selected_scenes={} "
                                     "total_scenes={} selected_scene_indices={} elapsed={:.1f}s",
                                     selectedDriverFiles,
                                     targetDriverFiles,
                                     file.relativePath.toStdString(),
                                     corpusScenePolicyName(scenePolicy),
                                     scenesToRead.size(),
                                     infos.size(),
                                     corpusSceneListToString(scenesToRead),
                                     secondsSince(startTime)));
    }
    for (size_t scene : scenesToRead) {
      SCOPED_TRACE(fmt::format("scene {}", scene));
      const std::vector<ZImgRegion> regions = smokeRegions(infos[scene]);
      for (size_t regionIndex = 0; regionIndex < regions.size(); ++regionIndex) {
        const ZImgRegion& region = regions[regionIndex];
        ZImg img;
        ++attemptedRegionReads;
        const std::string regionDescription = regionToString(region);
        if (logRegionReads) {
          emitCorpusProgress(fmt::format("reading region driver={}/{} corpus_file={}/{} relative_path={} scene={} "
                                         "region={}/{} bounds=\"{}\" elapsed={:.1f}s",
                                         selectedDriverFiles,
                                         targetDriverFiles,
                                         processedTargetFiles,
                                         targetFiles,
                                         file.relativePath.toStdString(),
                                         scene,
                                         regionIndex + 1,
                                         regions.size(),
                                         regionDescription,
                                         secondsSince(startTime)));
        }
        try {
          ZImgIO::instance().readImg(path, img, region, scene, 1, 1, 1, FileFormat::BioFormats);
        }
        catch (const std::exception& e) {
          const std::optional<std::string> knownBioFormatsReason =
            expectedBioFormatsFailureReason(file.relativePath, CorpusFailureStage::Region, e.what());
          if (knownBioFormatsReason.has_value()) {
            ++stats.expectedBioFormatsRegionFailures;
            ++expectedBioFormatsRegionFailures;
            emitCorpusProgress(fmt::format("expected Bio-Formats jar region failure relative_path={} "
                                           "absolute_path={} scene={} region=\"{}\" reason={} error={}",
                                           file.relativePath.toStdString(),
                                           path.toStdString(),
                                           scene,
                                           regionDescription,
                                           *knownBioFormatsReason,
                                           e.what()));
            continue;
          }
          const std::optional<std::string> unavailableCorpusReason =
            missingReferencedFileOutsideFullManifestReason(*corpusRootPath, fullManifestRelativePaths, e.what());
          if (unavailableCorpusReason.has_value()) {
            ++stats.unavailableCorpusRegionFailures;
            ++unavailableCorpusRegionFailures;
            emitCorpusProgress(fmt::format("unavailable corpus companion region failure relative_path={} "
                                           "absolute_path={} scene={} region=\"{}\" reason={} error={}",
                                           file.relativePath.toStdString(),
                                           path.toStdString(),
                                           scene,
                                           regionDescription,
                                           *unavailableCorpusReason,
                                           e.what()));
            continue;
          }
          ++stats.regionFailures;
          emitCorpusProgress(
            fmt::format("region failure relative_path={} absolute_path={} scene={} region=\"{}\" error={}",
                        file.relativePath.toStdString(),
                        path.toStdString(),
                        scene,
                        regionDescription,
                        e.what()));
          regionFailureRecords.push_back(
            {canonicalPath,
             fmt::format("failed to read Bio-Formats corpus region from {} scene {} region \"{}\": {}",
                         path.toStdString(),
                         scene,
                         regionDescription,
                         e.what())});
          if (maybeStopAfterFailure(regionFailureRecords.back())) {
            return;
          }
          continue;
        }
        EXPECT_FALSE(img.isEmpty());
        ++successfulRegionReads;
        if (logRegionReads) {
          emitCorpusProgress(fmt::format("region complete driver={}/{} corpus_file={}/{} relative_path={} scene={} "
                                         "region={}/{} bytes={} elapsed={:.1f}s",
                                         selectedDriverFiles,
                                         targetDriverFiles,
                                         processedTargetFiles,
                                         targetFiles,
                                         file.relativePath.toStdString(),
                                         scene,
                                         regionIndex + 1,
                                         regions.size(),
                                         img.byteNumber(),
                                         secondsSince(startTime)));
        }
      }
    }

    const std::vector<std::string> warningMessages = warningCapture.messages();
    if (!warningMessages.empty()) {
      ++stats.warningFailures;
      emitCorpusProgress(fmt::format("warnings while reading relative_path={} absolute_path={} count={}",
                                     file.relativePath.toStdString(),
                                     path.toStdString(),
                                     warningMessages.size()));
      warningFailureRecords.push_back({canonicalPath,
                                       fmt::format("warnings/errors while reading Bio-Formats corpus file {}:\n{}",
                                                   path.toStdString(),
                                                   fmt::join(warningMessages, "\n"))});
      if (maybeStopAfterFailure(warningFailureRecords.back())) {
        return;
      }
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
    if (datasetGroupKey.has_value()) {
      coveredCorpusDatasetGroups.insert(*datasetGroupKey);
    }
    maybeReportProgress();
  }
  reportActiveFormat();

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
  const size_t reportedMetadataLevelMismatchFailures = reportFailures(metadataLevelMismatchRecords);

  emitCorpusProgress(fmt::format(
    "finished public corpus smoke test processed={}/{} elapsed={:.1f}s selected_drivers={} opened_files={} "
    "regions={}/{} missing_files={} missing_files_outside_full_manifest={} skipped_incomplete_corpus_files={} "
    "skipped_incomplete_corpus_drivers={} "
    "skipped_non_drivers={} skipped_covered_companions={} suppressed_covered_companion_failures={} open_failures={} "
    "region_failures={} warning_failures={} metadata_level_comparisons={} metadata_level_mismatch_failures={} "
    "expected_bioformats_open_failures={} expected_bioformats_region_failures={} "
    "unavailable_corpus_open_failures={} unavailable_corpus_region_failures={}",
    processedTargetFiles,
    targetFiles,
    secondsSince(startTime),
    selectedDriverFiles,
    openedFiles,
    successfulRegionReads,
    attemptedRegionReads,
    missingFiles,
    missingFilesOutsideFullManifest,
    skippedIncompleteCorpusFiles,
    skippedIncompleteCorpusDriverFiles,
    skippedNonDriverFiles,
    skippedCoveredCompanionFiles,
    suppressedCoveredCompanionFailures,
    openFailures,
    regionFailures,
    warningFailures,
    metadataLevelComparisons,
    reportedMetadataLevelMismatchFailures,
    expectedBioFormatsOpenFailures,
    expectedBioFormatsRegionFailures,
    unavailableCorpusOpenFailures,
    unavailableCorpusRegionFailures));

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
    if (stats.missingDriverFiles == 0 && stats.openedDriverFiles + stats.coveredDriverFiles +
                                             stats.expectedBioFormatsOpenFailures +
                                             stats.unavailableCorpusOpenFailures ==
                                           0) {
      formatsWithoutCheckedDrivers.insert(format);
      ADD_FAILURE() << "Bio-Formats corpus top-level format checked no driver files: " << format.toStdString()
                    << " (selected drivers: " << stats.selectedDriverFiles
                    << "). Selected drivers must open, be covered by another opened driver, match a directly "
                       "verified Bio-Formats self-failure rule, or fail only because the corpus references a local "
                       "file that is absent from full_manifest.json.";
    }
  }

  RecordProperty("files", static_cast<int>(files.size()));
  RecordProperty("full_manifest_files", static_cast<int>(fullManifestRelativePaths.size()));
  RecordProperty("missing_files", static_cast<int>(missingFiles));
  RecordProperty("missing_files_outside_full_manifest", static_cast<int>(missingFilesOutsideFullManifest));
  RecordProperty("selected_driver_files", static_cast<int>(selectedDriverFiles));
  RecordProperty("skipped_non_driver_files", static_cast<int>(skippedNonDriverFiles));
  RecordProperty("excluded_format_count", static_cast<int>(excludedFormats.size()));
  RecordProperty("skipped_excluded_format_files", static_cast<int>(skippedExcludedFormatFiles));
  RecordProperty("only_format_count", static_cast<int>(includedFormats.size()));
  RecordProperty("skipped_filtered_format_files", static_cast<int>(skippedFilteredFormatFiles));
  RecordProperty("path_filter_count", pathSubstrings.size());
  RecordProperty("skipped_path_filtered_files", static_cast<int>(skippedPathFilteredFiles));
  RecordProperty("scene_policy", corpusScenePolicyName(scenePolicy));
  RecordProperty("scene_sample_limit", fmt::format("{}", sceneSampleLimit));
  RecordProperty("opened_files", static_cast<int>(openedFiles));
  RecordProperty("skipped_incomplete_corpus_files", static_cast<int>(skippedIncompleteCorpusFiles));
  RecordProperty("skipped_incomplete_corpus_driver_files", static_cast<int>(skippedIncompleteCorpusDriverFiles));
  RecordProperty("skipped_covered_companion_files", static_cast<int>(skippedCoveredCompanionFiles));
  RecordProperty("suppressed_covered_companion_failures", static_cast<int>(suppressedCoveredCompanionFailures));
  RecordProperty("formats_without_driver_rules", static_cast<int>(formatsWithoutDriverRules.size()));
  RecordProperty("formats_without_selected_drivers", static_cast<int>(formatsWithoutSelectedDrivers.size()));
  RecordProperty("formats_without_checked_drivers", static_cast<int>(formatsWithoutCheckedDrivers.size()));
  RecordProperty("open_failures", static_cast<int>(openFailures));
  RecordProperty("region_failures", static_cast<int>(regionFailures));
  RecordProperty("warning_failures", static_cast<int>(warningFailures));
  RecordProperty("compare_metadata_levels", compareMetadataLevels ? "true" : "false");
  RecordProperty("metadata_level_comparisons", static_cast<int>(metadataLevelComparisons));
  RecordProperty("metadata_level_mismatch_failures", static_cast<int>(reportedMetadataLevelMismatchFailures));
  RecordProperty("expected_bioformats_open_failures", static_cast<int>(expectedBioFormatsOpenFailures));
  RecordProperty("expected_bioformats_region_failures", static_cast<int>(expectedBioFormatsRegionFailures));
  RecordProperty("unavailable_corpus_open_failures", static_cast<int>(unavailableCorpusOpenFailures));
  RecordProperty("unavailable_corpus_region_failures", static_cast<int>(unavailableCorpusRegionFailures));
  EXPECT_GT(openedFiles + expectedBioFormatsOpenFailures + unavailableCorpusOpenFailures, 0u);
  EXPECT_EQ(missingFilesOutsideFullManifest, missingFiles);
  EXPECT_TRUE(formatsWithoutDriverRules.empty());
  EXPECT_TRUE(formatsWithoutSelectedDrivers.empty());
  EXPECT_TRUE(formatsWithoutCheckedDrivers.empty());
  EXPECT_EQ(0u, openFailures);
  EXPECT_EQ(0u, regionFailures);
  EXPECT_EQ(0u, warningFailures);
  EXPECT_EQ(0u, reportedMetadataLevelMismatchFailures);
}

} // namespace nim
