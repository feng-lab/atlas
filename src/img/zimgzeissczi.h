#pragma once

#include "zimgformat.h"
#include <QUuid>
#include <QXmlStreamReader>
#include <array>

// todo : support multiple files czi

namespace nim {

static_assert(sizeof(QUuid) == 16 && std::is_trivially_copyable_v<QUuid>, "wrong uuid type");

struct SegmentHeader
{
  // A sequence of up to 15 Ansi – characters 'A'...'Z', e.g. "ZISSUBBLOCK".
  // The special name "DELETED" marks a segment as deleted - readers should ignore or skip this segment.
  std::array<char, 16> id;
  int64_t allocatedSize; // The total numer of bytes allocated for this segment.
  int64_t usedSize; // The currently used number of bytes.
};

// SID = ZISRAWFILE  512 bytes
struct FileHeader
{
  int32_t major; // "1"
  int32_t minor; // "0"
  int32_t reserved1;
  int32_t reserved2;
  QUuid primaryFileGuid; // Unique Guid of Master file (FilePart 0)
  QUuid fileGuid; // Unique Per file
  int32_t filePart; // Part number in multi-file scenarios
  int64_t directoryPosition; // File position of the SubBlockDirectory Segment
  int64_t metaDataPosition; // File position of the Metadata Segment.
  // 0xffff, 0
  // This flag indicates a currently inconsistent situation
  // (e.g. updating Index, Directory or Metadata segment).
  // Readers should either wait until this flag is reset (in case that a
  // writer is still accessing the file), or try a recovery
  // procedure by scanning all segments.
  uint32_t updatePending;
  int64_t attachmentDirectoryPosition; // File position of the AttachmentDirectory Segment.
};

// SID = ZISRAWMETADATA  256 bytes
struct MetaDataSegment
{
  int32_t xmlSize; // Size of the XML data.
  int32_t attachmentSize; // Size of the the (binary) attachments. NOT USED CURRENTLY.
  std::array<uint8_t, 248> spare;
};

// 20 bytes
struct DimensionEntryDV1
{
  std::array<char, 4> dimension; // Typically 1 Byte ANSI e.g. 'X', see Dimensions / dimensions indices
  int32_t start; // Start position / index. May be < 0.
  int32_t size; // Size in units of pixels (logical size). Must be > 0.
  float startCoordinate; // Physical start coordinate (units e.g. micrometers or seconds)
  int32_t storedSize; // Stored size (if sub / supersampling, else 0)
};

// 32 bytes + EntryCount * 20
struct DirectoryEntryDV
{
  std::array<char, 2> schemaType; // "DV"
  int32_t pixelType; // The type of the image pixels, see PixelTypes.
  int64_t filePosition; // Seek offset of the referenced SubBlockSegment relative to the first byte of the file
  int32_t filePart; // Reserved.
  int32_t compression; // See Compression Constants
  // [INTERNAL] Contains information for automatic image pyramids using SubBlocks of different resolution,
  // current values are: None=0, SingleSubblock=1, MultiSubblock=2.
  uint8_t pyramidType;
  uint8_t spare1;
  std::array<uint8_t, 4> spare2;
  // Number of entries. Minimum is 1.
  int32_t dimensionCount;
  // DimensionEntries of type DimensionEntryDV1[dimensionCount] follows
};

// SID = ZISRAWSUBBLOCK  at least 256 bytes
struct SubBlockSegment
{
  int32_t metaDataSize; // Size of the metadata section.
  int32_t attachmentSize; // Size of the optional attachment section.
  int64_t dataSize; // Size of the data section.
  // Subset indices and size information, a 1:1 copy will be stored as part of the File's
  // SubBlockDirectory Segment. The length of this information depends on the directory schema.
  DirectoryEntryDV directoryEntry;
};

struct subBlockDirectorySegment
{
  int32_t entryCount; // The number of entries
  std::array<uint8_t, 124> reserved;
  // List of EntryCount DirectoryEntryDV follows.
  // Each item is a copy of the DirectoryEntry in the referenced SubBlock segment.
};

// 128 bytes
struct AttachmentEntryA1
{
  std::array<char, 2> schemaType; // "A1"
  std::array<uint8_t, 10> reserved;
  int64_t filePosition; // Seek offset relative to the first byte of the file
  int32_t filePart; // Reserved;
  QUuid contentGuid; // Unique Id to be used in strong, fully qualified references
  std::array<char, 8> contentFileType; // Unique file type Identifier (see table below)
  // Null terminated (80-1) character UTF8 encoded string defining a name for this item.
  // May be used in references instead of GUID.
  std::array<char, 80> name;
};

// SID = ZISRAWATTACH
struct AttachmentSegment
{
  int32_t dataSize; // Size of the data section.
  std::array<uint8_t, 12> spare1;
  // Core information, an 1:1 copy will be stored as part of the
  // File's AttachmentDirectory Segment.
  AttachmentEntryA1 attachmentEntry;
  std::array<uint8_t, 112> spare2;
  // [Data] follows
};

struct TimeStampSegment
{
  int32_t size; // Size of the whole block used for time stamps.
  int32_t numberTimeStamps; // Number of time stamps in the list.
  // timeStamps of type double[NumberTimeStamps] follows
  // Time stamps in seconds relative to the start time of the acquisition engine.
};

struct FocusPositions
{
  int32_t size; // Size of the whole block used for focus positions.
  int32_t numberPositions; // Number of positions in the list.
  // positions of type double[numberPositions] follows
  // Focus positions in micrometers relative to the Z start position of the acquisition engine.
};

struct EventListEntry
{
  int32_t size; // Size of the entry in bytes.
  double
    time; // Time of the event in seconds relative to the start time of the LSM electronic module controller program.
  // Can be one of the following values:
  //  EV_TYPE_MARKER (= 0)
  //  - Experimental annotation
  //  EV_TYPE_TIMER_CHANGE (= 1)
  //  - The time interval has changed
  //  EV_TYPE_BLEACH_START ( = 2 )
  //  - Start of a bleach operation
  //  EV_TYPE_BLEACH_STOP ( = 3 )
  //  - End of a bleach operation
  //  EV_TYPE_TRIGGER ( = 4 )
  //  - A trigger signal was detected on the
  //  user port of the electronic module.
  int32_t eventType;
  int32_t descriptionSize; // Size of the description character array.
  // Null terminated descriptionSize character UTF8 encoded string defining a description for this event.
};

struct EventListSegment
{
  int32_t size;
  int32_t numberEvents;
  // events of type EventListEntry[numberEvents] follows
};

// SID = ZISRAWATTDIR
struct AttachmentDirectorySegment
{
  int32_t entryCount;
  std::array<uint8_t, 252> reserved;
  // AttachmentEntryA1[entryCount] follows
};

struct CZITile
{
  CZITile()
  {
    sceneIdx[0] = std::numeric_limits<int32_t>::max();
    sceneIdx[1] = std::numeric_limits<int32_t>::max();
    sceneIdx[2] = std::numeric_limits<int32_t>::max();
    sceneIdx[3] = std::numeric_limits<int32_t>::max();
    sceneIdx[4] = std::numeric_limits<int32_t>::max();
    sceneIdx[5] = std::numeric_limits<int32_t>::max();
  }

  size_t ratio = 0; // size / storedsize
  ZVoxelCoordinate start = ZVoxelCoordinate(std::numeric_limits<int32_t>::max(),
                                            std::numeric_limits<int32_t>::max(),
                                            std::numeric_limits<int32_t>::max(),
                                            std::numeric_limits<int32_t>::max(),
                                            std::numeric_limits<int32_t>::max());
  ZVoxelCoordinate size = ZVoxelCoordinate(1, 1, 1, 1, 1); // all 1
  ZVoxelCoordinate storedSize = ZVoxelCoordinate(1, 1, 1, 1, 1); // all 1
  int32_t pixelType; // The type of the image pixels, see PixelTypes.
  int64_t filePosition; // Seek offset of the referenced SubBlockSegment relative to the first byte of the file
  int32_t compression; // See Compression Constants
  std::array<int32_t, 6> sceneIdx; // V, H, I, S, R, C
  QString dimensionOrder;
};

bool operator<(const CZITile& lhs, const CZITile& rhs);

struct MixedTilesSort
{
  bool operator()(const CZITile& lhs, const CZITile& rhs) const
  {
    return std::tie(lhs.ratio, lhs.start.z, lhs.start.t, lhs.start.c, lhs.start.x, lhs.start.y) <
           std::tie(rhs.ratio, rhs.start.z, rhs.start.t, rhs.start.c, rhs.start.x, rhs.start.y);
  }
};

class ZImgCZISubBlock : public ZImgSubBlock
{
public:
  // mixed tiles has different x and y location
  ZImgCZISubBlock(QString fileName,
                  std::vector<CZITile>& tiles,
                  bool mixedTiles = false,
                  size_t numChannels = 0,
                  size_t bytePerVoxel = 0,
                  VoxelFormat vf = VoxelFormat::Unsigned);

  [[nodiscard]] std::shared_ptr<ZImg> read() const override;

  [[nodiscard]] ZImgInfo readInfo() const override;

protected:
  QString m_filename;
  std::vector<CZITile> m_tiles; // cat in Dimension::C or mixed
  bool m_mixedTiles;
  ZVoxelCoordinate m_mixedTilesStart;
  size_t m_numChannels;
  size_t m_bytePerVoxel;
  VoxelFormat m_voxelFormat;
  double m_scale = 0.0;
  size_t m_width = 0;
  size_t m_height = 0;
};

class ZImgZeissCZI : public ZImgFormat
{
public:
  static ZImgZeissCZI& instance();

  enum class CorrectionMode
  {
    ZeroLightPreserved,
    IntensityRangeCorrected,
    Direct
  };

  // stack tiles to make a 3d stack
  ZImg stackTiles(const QString& filename, size_t ch, size_t scene, std::vector<ZVoxelCoordinate>& coords);

  ZImg stackTiles(const QString& filename,
                  size_t ch,
                  size_t scene,
                  const QString& inverseMaskFile,
                  size_t maskFilePyramidalLevel = 0);

  ZImg correctShading(const QString& filename,
                      size_t ch,
                      size_t scene,
                      const ZImg& modelZ,
                      const ZImg& modelV,
                      CorrectionMode cm);

  // ZImgFormat interface

public:
  bool supportRead() const override;

  bool supportWrite() const override;

  QString shortName() const override;

  QString fullName() const override;

  QStringList extensions() const override;

  FileFormat format() const override
  {
    return FileFormat::ZeissCZI;
  }

  void readInfo(const QString& filename,
                std::vector<ZImgInfo>& infos,
                std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks) override;

  void readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene) override;

  void
  readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene) override;

  void readImg(const QString& filename,
               ZImg& img,
               const ZImgRegion& region,
               size_t scene,
               size_t xRatio,
               size_t yRatio,
               size_t zRatio) override;

private:
  void clearInternalState();

  static int64_t checkFilename(const QString& filename);

  void readCZIInfo(const QString& xmlString);

  void parseMetadata(QXmlStreamReader& xml);

  void parseChannel(QXmlStreamReader& xml);

  void parseScene(QXmlStreamReader& xml);

  void parseDistance(QXmlStreamReader& xml);

  void parseDisplaySettingChannel(QXmlStreamReader& xml);

  void detectInfos(std::vector<ZImgInfo>& infos, std::ifstream& inputFileStream, FileHeader& fh);

  static std::string toString(const QString& filename);

  static void
  dumpCZIStream(std::ifstream& inputFileStream, int64_t filesize, int64_t offset, std::string& str, index_t indent = 0);

  static void
  dumpSegmentInfo(const SegmentHeader& sh, std::ifstream& inputFileStream, std::string& str, index_t indent = 0);

  static void dumpFileHeaderSegment(std::ifstream& inputFileStream, std::string& str, index_t indent = 0);

  static void dumpMetadataSegment(std::ifstream& inputFileStream, std::string& str, index_t indent = 0);

  static void dumpSubBlockSegment(std::ifstream& inputFileStream, std::string& str, index_t indent = 0);

  static void dumpDirectoryEntry(const DirectoryEntryDV& de, std::string& str, index_t indent = 0);

  static void dumpDimensionEntry(const DimensionEntryDV1& de, std::string& str, index_t indent = 0);

  static void dumpSubBlockDirectory(std::ifstream& inputFileStream, std::string& str, index_t indent = 0);

  static void dumpAttachmentSegment(std::ifstream& inputFileStream, std::string& str, index_t indent = 0);

  static void dumpAttachmentEntry(const AttachmentEntryA1& ae, std::string& str, index_t indent = 0);

  static void dumpAttachmentDirectory(std::ifstream& inputFileStream, std::string& str, index_t indent = 0);

private:
  QString m_metadataXmlString;
  bool m_hasVoxelSizeInfo;
  double m_voxelSizeX;
  double m_voxelSizeY;
  double m_voxelSizeZ;
  bool m_hasChannelInfo;
  std::vector<col4> m_channelColors;
  std::vector<QString> m_channelNames;
  std::vector<int> m_channelPixelType;
  std::vector<size_t> m_channelValidBitCount;
  bool m_hasSceneInfo;
  std::vector<double> m_sceneCenterX;
  std::vector<double> m_sceneCenterY;

  std::vector<col4> m_channelColorsFromDisplaySettings;
  std::vector<QString> m_channelNamesFromDisplaySettings;
  bool m_shouldSeparateChannelsToDifferentScenes;
  bool m_someTilesAreNot2D;

  std::vector<std::set<CZITile>> m_sceneTiles;
  std::vector<ZVoxelCoordinate> m_sceneStart;
  std::vector<ZVoxelCoordinate> m_sceneEnd;
};

} // namespace nim
