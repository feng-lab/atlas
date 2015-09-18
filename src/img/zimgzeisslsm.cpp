#include "zimgzeisslsm.h"
#include "ztiff.h"
#include "QsLog.h"
#include "zioutils.h"

namespace nim {

ZImgZeissLsm::ZImgZeissLsm()
  : ZImgTiff()
{
}

QString ZImgZeissLsm::shortName() const
{
  return "Lsm";
}

QString ZImgZeissLsm::fullName() const
{
  return "Carl Zeiss Lsm";
}

QStringList ZImgZeissLsm::extensions() const
{
  QStringList res;
  res << "lsm";
  return res;
}

bool ZImgZeissLsm::supportRead() const
{
  return true;
}

bool ZImgZeissLsm::supportWrite() const
{
  return false;
}

void ZImgZeissLsm::readIntoInternalStructure(const QString &filename, ZTiff &tiff)
{
  tiff.setUseColormap(false);
  ZImgTiff::readIntoInternalStructure(filename, tiff);
  if (tiff.isLsmFile()) {
    readLsmInfo(filename, tiff);
  } else {
    throw ZIOException("Not lsm file");
  }
}

void ZImgZeissLsm::clearInternalState()
{
  m_numScenes = 0;
  m_lsmImgInfo.clear();
  m_channelDataTypes.clear();
  m_positions.clear();
  m_tilePositions.clear();
}

void ZImgZeissLsm::detectImgInfo(ZTiff &tiff)
{
  ZImgTiff::detectImgInfo(tiff);

  // overwrite some fields based on lsm information
  if (m_imgInfo[0].width != m_lsmImgInfo.width ||
      m_imgInfo[0].height != m_lsmImgInfo.height ||
      m_imgInfo[0].numChannels != m_lsmImgInfo.numChannels ||
      (m_imgInfo[0].depth != m_lsmImgInfo.depth * m_lsmImgInfo.numTimes * m_numScenes &&
       m_imgInfo[0].depth % (m_lsmImgInfo.depth * m_lsmImgInfo.numTimes) != 0) ||
      m_imgInfo[0].bytesPerVoxel != m_lsmImgInfo.bytesPerVoxel ||
      m_imgInfo[0].voxelFormat != m_lsmImgInfo.voxelFormat)
    throw ZIOException(QString("lsm meta info <%1> doesn't match image data <%2>").
                       arg(m_lsmImgInfo.toQString()).arg(m_imgInfo[0].toQString()));

  size_t numLocations = 0;
  if (m_numScenes > 0)
    numLocations = m_numScenes;
  else
    numLocations = m_imgInfo[0].depth / (m_lsmImgInfo.depth * m_lsmImgInfo.numTimes);
  if (numLocations == 0) {
    throw ZIOException("invalid number of scenes in lsm file");
  }
  m_imgInfo[0].depth = m_lsmImgInfo.depth;
  m_imgInfo[0].numTimes = m_lsmImgInfo.numTimes;

  m_imgInfo[0].voxelSizeUnit = m_lsmImgInfo.voxelSizeUnit;
  m_imgInfo[0].voxelSizeX = m_lsmImgInfo.voxelSizeX;
  m_imgInfo[0].voxelSizeY = m_lsmImgInfo.voxelSizeY;
  m_imgInfo[0].voxelSizeZ = m_lsmImgInfo.voxelSizeZ;

  m_imgInfo[0].channelColors = m_lsmImgInfo.channelColors;
  m_imgInfo[0].channelNames = m_lsmImgInfo.channelNames;
  m_imgInfo[0].timeStamps = m_lsmImgInfo.timeStamps;
  //m_imgInfo.locations = m_lsmImgInfo.locations;

  m_imgInfo[0].validBitCount = m_lsmImgInfo.validBitCount;

  m_imgInfo[0].createDefaultDescriptions();
  for (size_t i=1; i<numLocations; ++i)
    m_imgInfo.push_back(m_imgInfo[0]);
}

void ZImgZeissLsm::readLsmInfo(const QString &filename, ZTiff &tiff)
{
  const ZImgMetatag &lsmInfoTag = tiff.lsmInfoTag();
  memcpy(&m_lsmInfo, lsmInfoTag.dataArray(), lsmInfoTag.dataByteNumber());

  if (m_lsmInfo.u16ScanType == 3 || m_lsmInfo.u16ScanType == 5 ||
      m_lsmInfo.u16ScanType == 7 || m_lsmInfo.u16ScanType == 9)
    setDimensionOrder("TZL");
  else
    setDimensionOrder("ZTL");

  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

  if (m_lsmInfo.u32OffsetChannelColors != 0) {
    inputFileStream.seekg(m_lsmInfo.u32OffsetChannelColors);
    readStream(inputFileStream, reinterpret_cast<char*>(&m_lsmChannelColors), sizeof(CZ_ChannelColors));

    std::vector<char> chStruct(m_lsmChannelColors.s32BlockSize);
    inputFileStream.seekg(m_lsmInfo.u32OffsetChannelColors);
    readStream(inputFileStream, chStruct.data(), chStruct.size());
    m_lsmImgInfo.channelColors.resize(m_lsmChannelColors.s32NumberColors);
    memcpy(m_lsmImgInfo.channelColors.data(), chStruct.data()+m_lsmChannelColors.s32ColorsOffset,
        sizeof(uint32_t)*m_lsmChannelColors.s32NumberColors);

    size_t offset = m_lsmChannelColors.s32NamesOffset;
    int nameIdx = 0;
    while (nameIdx < m_lsmChannelColors.s32NumberNames) {
      offset += 4;  // skip uint32_t name length
      QString str(&chStruct[offset]);
      m_lsmImgInfo.channelNames.push_back(str);
      ++nameIdx;
      offset += str.size() + 1;
    }
  }

  if (m_lsmInfo.u32OffsetTimeStamps != 0) {
    inputFileStream.seekg(m_lsmInfo.u32OffsetTimeStamps);
    readStream(inputFileStream, reinterpret_cast<char*>(&m_lsmTimeStamps), sizeof(CZ_TimeStamps));
    m_lsmImgInfo.timeStamps.resize(m_lsmTimeStamps.s32NumberTimeStamps);
    readStream(inputFileStream, reinterpret_cast<char*>(m_lsmImgInfo.timeStamps.data()), sizeof(double) * m_lsmTimeStamps.s32NumberTimeStamps);
  }

  if (m_lsmInfo.u32OffsetChannelDataTypes != 0) {
    inputFileStream.seekg(m_lsmInfo.u32OffsetChannelDataTypes);
    m_channelDataTypes.resize(m_lsmInfo.s32DimensionChannels);
    readStream(inputFileStream, reinterpret_cast<char*>(m_channelDataTypes.data()), sizeof(uint32_t) * m_channelDataTypes.size());
  }

  if (m_lsmInfo.u32OffsetPositions != 0) {
    inputFileStream.seekg(m_lsmInfo.u32OffsetPositions);
    uint32_t numPositions;
    readStream(inputFileStream, reinterpret_cast<char*>(&numPositions), sizeof(uint32_t));
    m_positions.resize(numPositions);
    readStream(inputFileStream, reinterpret_cast<char*>(m_positions.data()), sizeof(Location) * numPositions);
  }

  if (m_lsmInfo.u32OffsetTilePositions != 0) {
    inputFileStream.seekg(m_lsmInfo.u32OffsetTilePositions);
    uint32_t numTiles;
    readStream(inputFileStream, reinterpret_cast<char*>(&numTiles), sizeof(uint32_t));
    m_tilePositions.resize(numTiles);
    readStream(inputFileStream, reinterpret_cast<char*>(m_tilePositions.data()), sizeof(Location) * numTiles);
  }

  m_lsmImgInfo.width = m_lsmInfo.s32DimensionX;
  m_lsmImgInfo.height = m_lsmInfo.s32DimensionY;
  m_lsmImgInfo.depth = m_lsmInfo.s32DimensionZ;
  m_lsmImgInfo.numChannels = m_lsmInfo.s32DimensionChannels;
  m_lsmImgInfo.numTimes = m_lsmInfo.s32DimensionTime;
  // combines two dimension, can be 0 for old lsm
  m_numScenes = m_lsmInfo.s32DimensionM * m_lsmInfo.s32DimensionP;

  m_lsmImgInfo.voxelSizeUnit = VoxelSizeUnit::um;
  m_lsmImgInfo.voxelSizeX = m_lsmInfo.f64VoxelSizeX * 1e6;
  m_lsmImgInfo.voxelSizeY = m_lsmInfo.f64VoxelSizeY * 1e6;
  m_lsmImgInfo.voxelSizeZ = m_lsmInfo.f64VoxelSizeZ * 1e6;

  if (m_lsmInfo.s32DataType == 0) {
    if (m_channelDataTypes.size() < m_lsmImgInfo.numChannels)
      throw ZIOException("lsm channel data type is not complete");
    for (size_t i=1; i<m_lsmImgInfo.numChannels; ++i)
      if (m_channelDataTypes[i] != m_channelDataTypes[0])
        throw ZIOException("lsm different channel data type is not supported");
    switch (m_channelDataTypes[0]) {
    case 1:
      m_lsmImgInfo.voxelFormat = VoxelFormat::Unsigned;
      m_lsmImgInfo.bytesPerVoxel = 1;
      break;
    case 2:
      m_lsmImgInfo.voxelFormat = VoxelFormat::Unsigned;
      m_lsmImgInfo.bytesPerVoxel = 2;
      m_lsmImgInfo.validBitCount = 12;
      break;
    case 5:
      m_lsmImgInfo.voxelFormat = VoxelFormat::Float;
      m_lsmImgInfo.bytesPerVoxel = 4;
      break;
    default:
      throw ZIOException("lsm data type is not recognized");
      break;
    }
  } else {
    switch (m_lsmInfo.s32DataType) {
    case 1:
      m_lsmImgInfo.voxelFormat = VoxelFormat::Unsigned;
      m_lsmImgInfo.bytesPerVoxel = 1;
      break;
    case 2:
      m_lsmImgInfo.voxelFormat = VoxelFormat::Unsigned;
      m_lsmImgInfo.bytesPerVoxel = 2;
      m_lsmImgInfo.validBitCount = 12;
      break;
    case 5:
      m_lsmImgInfo.voxelFormat = VoxelFormat::Float;
      m_lsmImgInfo.bytesPerVoxel = 4;
      break;
    default:
      throw ZIOException("lsm data type is not recognized");
      break;
    }
  }
}

void ZImgZeissLsm::logLsmInfo(const QString &filename)
{
  LINFO() << "Start LSM Info for" << filename;
  LINFO() << "MagicNumber:" << hex << m_lsmInfo.u32MagicNumber;
  LINFO() << "DimensionX:" << m_lsmInfo.s32DimensionX;
  LINFO() << "DimensionY:" << m_lsmInfo.s32DimensionY;
  LINFO() << "DimensionZ:" << m_lsmInfo.s32DimensionZ;
  LINFO() << "DimensionChannels:" << m_lsmInfo.s32DimensionChannels;
  LINFO() << "DimensionTime:" << m_lsmInfo.s32DimensionTime;
  LINFO() << "DimensionP:" << m_lsmInfo.s32DimensionP;
  LINFO() << "DimensionM:" << m_lsmInfo.s32DimensionM;
  switch (m_lsmInfo.s32DataType) {
  case 1: LINFO() << "DataType:" << "8-bit unsigned integer"; break;
  case 2: LINFO() << "DataType:" << "12-bit unsigned integer"; break;
  case 5: LINFO() << "DataType:" << "32-bit float(for \"Time Series Mean-of-ROIs\")"; break;
  //case 0: LINFO() << "DataType:" << "different data types for different channels, see 32OffsetChannelDataTypes"; break;
  }
  for (size_t i=0; i<m_channelDataTypes.size(); ++i) {
    switch (m_channelDataTypes[i]) {
    case 1: LINFO() << "Channel" << i+1 << "DataType:" << "8-bit unsigned integer"; break;
    case 2: LINFO() << "Channel" << i+1 << "DataType:" << "12-bit unsigned integer"; break;
    case 5: LINFO() << "Channel" << i+1 << "DataType:" << "32-bit float(for \"Time Series Mean-of-ROIs\")"; break;
    }
  }
  LINFO() << "ThumbnailX:" << m_lsmInfo.s32ThumbnailX;
  LINFO() << "ThumbnailY:" << m_lsmInfo.s32ThumbnailY;
  LINFO() << "VoxelSizeX in meter:" << m_lsmInfo.f64VoxelSizeX;
  LINFO() << "VoxelSizeY in meter:" << m_lsmInfo.f64VoxelSizeY;
  LINFO() << "VoxelSizeZ in meter:" << m_lsmInfo.f64VoxelSizeZ;
  switch (m_lsmInfo.u16ScanType) {
  case 0: LINFO() << "ScanType:" << "normal x-y-z-scan"; break;
  case 1: LINFO() << "ScanType:" << "z-Scan (x-z-plane)"; break;
  case 2: LINFO() << "ScanType:" << "line scan"; break;
  case 3: LINFO() << "ScanType:" << "time series x-y"; break;
  case 4: LINFO() << "ScanType:" << "time series x-z (release 2.0 or later)"; break;
  case 5: LINFO() << "ScanType:" << "time series \"Mean of ROIs\" (release 2.0 or later)"; break;
  case 6: LINFO() << "ScanType:" << "time series x-y-z (release 2.3 or later)"; break;
  case 7: LINFO() << "ScanType:" << "spline scan (release 2.5 or later)"; break;
  case 8: LINFO() << "ScanType:" << "spline plane x-z (release 2.5 or later)"; break;
  case 9: LINFO() << "ScanType:" << "time series spline plane x-z (release 2.5 or later)"; break;
  case 10: LINFO() << "ScanType:" << "point mode (release 3.0 or later)"; break;
  }
  switch (m_lsmInfo.u16SpectralScan) {
  case 0: LINFO() << "SpectralScan:" << "no spectral scan"; break;
  case 1: LINFO() << "SpectralScan:" << "image has been acquired in spectral scan mode with a META detector (release 3.0 or later)"; break;
  }
  switch (m_lsmInfo.u32DataType) {
  case 0: LINFO() << "DataType:" << "Original scan data"; break;
  case 1: LINFO() << "DataType:" << "Calculated data"; break;
  case 2: LINFO() << "DataType:" << "Animation"; break;
  }
  if (m_lsmInfo.f64TimeInterval != 0) {
    LINFO() << "TimeInterval in s:" << m_lsmInfo.f64TimeInterval;
  }
  for (size_t i=0; i<m_lsmImgInfo.timeStamps.size(); ++i) {
    LINFO() << "TimeStamp" << i+1 << "in s:" << m_lsmImgInfo.timeStamps[i];
  }

  for (size_t i=0; i<m_positions.size(); ++i) {
    LINFO() << "Position" << i+1 << ":" << m_positions[i].x << m_positions[i].y << m_positions[i].z;
  }
  for (size_t i=0; i<m_tilePositions.size(); ++i) {
    LINFO() << "TilePosition" << i+1 << ":" << m_tilePositions[i].x << m_tilePositions[i].y << m_tilePositions[i].z;
  }
  LINFO() << "DisplayAspectX:" << m_lsmInfo.f64DisplayAspectX;
  LINFO() << "DisplayAspectY:" << m_lsmInfo.f64DisplayAspectY;
  LINFO() << "DisplayAspectZ:" << m_lsmInfo.f64DisplayAspectZ;
  LINFO() << "DisplayAspectTime:" << m_lsmInfo.f64DisplayAspectTime;
  LINFO() << "ObjectiveSphereCorrection:" << m_lsmInfo.f64ObjectiveSphereCorrection;
  for (size_t i=0; i<m_lsmImgInfo.channelColors.size(); ++i) {
    LINFO() << "Channel" << i+1 << "Name:" << m_lsmImgInfo.channelNames[i] << "Color(RGB):" << m_lsmImgInfo.channelColors[i].r
            << m_lsmImgInfo.channelColors[i].g << m_lsmImgInfo.channelColors[i].b << m_lsmImgInfo.channelColors[i].a;
  }
  LINFO() << "End LSM Info for" << filename;
}


} // namespace
