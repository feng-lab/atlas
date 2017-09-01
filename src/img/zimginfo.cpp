#include "zimginfo.h"

#include "zrandom.h"
#include <QLocale>
#include <type_traits>

namespace nim {

ZImgInfo::ZImgInfo(size_t w, size_t h, size_t d,
                   size_t c, size_t t,
                   size_t bytePerVox, VoxelFormat vf)
  : width(w), height(h), depth(d), numChannels(c), numTimes(t)
{
  bytesPerVoxel = bytePerVox;
  voxelFormat = vf;
  validBitCount = 0;

  voxelSizeUnit = VoxelSizeUnit::none;
  voxelSizeX = 1;
  voxelSizeY = 1;
  voxelSizeZ = 1;
  lastChannelIsAlphaChannel = false;

  createDefaultDescriptions();
}

void ZImgInfo::clear()
{
  width = 0;
  height = 0;
  depth = 0;
  numChannels = 0;
  numTimes = 0;

  bytesPerVoxel = 1;
  voxelFormat = VoxelFormat::Unsigned;
  validBitCount = 0;

  voxelSizeUnit = VoxelSizeUnit::none;
  voxelSizeX = 1;
  voxelSizeY = 1;
  voxelSizeZ = 1;

  timeStamps.clear();
  channelNames.clear();
  channelColors.clear();
  //locations.clear();
  position.clear();
  lastChannelIsAlphaChannel = false;
}

void ZImgInfo::swap(ZImgInfo& other) noexcept
{
  std::swap(width, other.width);
  std::swap(height, other.height);
  std::swap(depth, other.depth);
  std::swap(numChannels, other.numChannels);
  std::swap(numTimes, other.numTimes);
  //std::swap(numLocations, other.numLocations);
  std::swap(bytesPerVoxel, other.bytesPerVoxel);
  std::swap(voxelFormat, other.voxelFormat);
  std::swap(validBitCount, other.validBitCount);
  std::swap(voxelSizeUnit, other.voxelSizeUnit);
  std::swap(voxelSizeX, other.voxelSizeX);
  std::swap(voxelSizeY, other.voxelSizeY);
  std::swap(voxelSizeZ, other.voxelSizeZ);

  timeStamps.swap(other.timeStamps);
  channelNames.swap(other.channelNames);
  channelColors.swap(other.channelColors);
  //locations.swap(other.locations);
  position.swap(other.position);
  std::swap(lastChannelIsAlphaChannel, other.lastChannelIsAlphaChannel);
}

//std::string ZImgInfo::toString() const
//{
//  std::ostringstream res;
//  res << "width:" << width
//      << ", height:" << height
//      << ", depth:" << depth
//      << ", numChannels:" << numChannels
//      << ", numTimes:" << numTimes
//      << ", numLocations:" << numLocations
//      << ", bytesPerVoxel:" << bytesPerVoxel
//      << ", voxelFormat:" << VoxelFormatString[voxelFormat]
//      << ", voxelSizeUnit:" << VoxelSizeUnitString[voxelSizeUnit]
//      << ", voxelSizeX:" << voxelSizeX
//      << ", voxelSizeY:" << voxelSizeY
//      << ", voxelSizeZ:" << voxelSizeZ;

//  return res.str();
//}

QString ZImgInfo::toQString() const
{
  return "width: " + QString::number(width) +
         ", height: " + QString::number(height) +
         ", depth: " + QString::number(depth) +
         ", numChannels: " + QString::number(numChannels) +
         ", numTimes: " + QString::number(numTimes) +
         //", numLocations: " + QString::number(numLocations) +
         ", bytesPerVoxel: " + QString::number(bytesPerVoxel) +
         ", voxelFormat: " + enumToString(voxelFormat) +
         ", voxelSizeUnit: " + enumToString(voxelSizeUnit) +
         ", voxelSizeX: " + QString::number(voxelSizeX, 'g', QLocale::FloatingPointShortest) +
         ", voxelSizeY: " + QString::number(voxelSizeY, 'g', QLocale::FloatingPointShortest) +
         ", voxelSizeZ: " + QString::number(voxelSizeZ, 'g', QLocale::FloatingPointShortest) +
         (lastChannelIsAlphaChannel ? QString(", alphaChannel: %1").arg(numChannels - 1) : QString("")) +
         (validBitCount > 0 ? QString(", validBitCount: %1").arg(validBitCount) : QString(""));
}

QString ZImgInfo::displayChannelName(size_t c) const
{
  QString res;
  if (channelNames[c].isEmpty()) {
    res = QString("Ch%1").arg(c + 1);
  } else {
    res = QString("Ch%1 (%2)").arg(c + 1).arg(channelNames[c]);
  }

  if (lastChannelIsAlphaChannel && c + 1 == numChannels) {
    res += QString(" (Alpha)");
  }
  return res;
}

void ZImgInfo::createDefaultChannelNames()
{
  channelNames.resize(numChannels);
}

void ZImgInfo::createDefaultChannelColors()
{
  if (channelColors.size() != numChannels) {
    channelColors.resize(numChannels);
    for (size_t i = 0; i < numChannels; ++i) {
      if (i == 0) {
        if (numChannels == 1 || (numChannels == 2 && lastChannelIsAlphaChannel)) {
          channelColors[i] = col4(255, 255, 255);
        } else {
          channelColors[i] = col4(255, 0, 0);
        }
      } else if (i == 1) {
        channelColors[i] = col4(0, 255, 0);
      } else if (i == 2) {
        channelColors[i] = col4(0, 0, 255);
      } else if (i == 3) {
        channelColors[i] = col4(255, 255, 0);
      } else if (i == 4) {
        channelColors[i] = col4(255, 0, 255);
      } else if (i == 5) {
        channelColors[i] = col4(0, 255, 255);
      } else {
        channelColors[i] = col4(ZRandom::instance().randInt(255),
                                ZRandom::instance().randInt(255),
                                ZRandom::instance().randInt(255));
      }
    }
    if (lastChannelIsAlphaChannel && numChannels > 0)
      channelColors[numChannels - 1] = col4(0, 0, 0);
  }
}

void ZImgInfo::createDefaultTimeStamps()
{
  if (timeStamps.size() != numTimes) {
    timeStamps.resize(numTimes);
    for (size_t i = 0; i < numTimes; ++i) {
      timeStamps[i] = i;
    }
  }
}

//void ZImgInfo::createDefaultLocations()
//{
//  if (locations.size() != numLocations) {
//    locations.resize(numLocations);
//    for (size_t i=0; i<numLocations; ++i)
//      locations[i] = Location();
//  }
//}

void ZImgInfo::createDefaultDescriptions()
{
  createDefaultChannelColors();
  createDefaultChannelNames();
  //createDefaultLocations();
  createDefaultTimeStamps();
}

double ZImgInfo::voxelSizeXInUnit(VoxelSizeUnit unit) const
{
  if (voxelSizeUnit == VoxelSizeUnit::none || unit == VoxelSizeUnit::none || voxelSizeUnit == unit) {
    return voxelSizeX;
  }
  return voxelSizeX * unitSizeInMeter(voxelSizeUnit) / unitSizeInMeter(unit);
}

double ZImgInfo::voxelSizeYInUnit(VoxelSizeUnit unit) const
{
  if (voxelSizeUnit == VoxelSizeUnit::none || unit == VoxelSizeUnit::none || voxelSizeUnit == unit) {
    return voxelSizeY;
  }
  return voxelSizeY * unitSizeInMeter(voxelSizeUnit) / unitSizeInMeter(unit);
}

double ZImgInfo::voxelSizeZInUnit(VoxelSizeUnit unit) const
{
  if (voxelSizeUnit == VoxelSizeUnit::none || unit == VoxelSizeUnit::none || voxelSizeUnit == unit) {
    return voxelSizeZ;
  }
  return voxelSizeZ * unitSizeInMeter(voxelSizeUnit) / unitSizeInMeter(unit);
}

template<typename TVoxel>
void ZImgInfo::setVoxelFormat(size_t validBitCountIn)
{
  static_assert(std::is_arithmetic<TVoxel>::value &&
                (sizeof(TVoxel) == 1 || sizeof(TVoxel) == 2 || sizeof(TVoxel) == 4 || sizeof(TVoxel) == 8),
                "need arithmetic type");
  validBitCount = validBitCountIn;
  bytesPerVoxel = sizeof(TVoxel);
  if (std::is_integral<TVoxel>::value) {
    voxelFormat = std::is_signed<TVoxel>::value ? VoxelFormat::Signed : VoxelFormat::Unsigned;
  } else {
    voxelFormat = VoxelFormat::Float;
  }
}

template void ZImgInfo::setVoxelFormat<uint8_t>(size_t validBitCountIn);

template void ZImgInfo::setVoxelFormat<uint16_t>(size_t validBitCountIn);

template void ZImgInfo::setVoxelFormat<uint32_t>(size_t validBitCountIn);

template void ZImgInfo::setVoxelFormat<uint64_t>(size_t validBitCountIn);

template void ZImgInfo::setVoxelFormat<int8_t>(size_t validBitCountIn);

template void ZImgInfo::setVoxelFormat<int16_t>(size_t validBitCountIn);

template void ZImgInfo::setVoxelFormat<int32_t>(size_t validBitCountIn);

template void ZImgInfo::setVoxelFormat<int64_t>(size_t validBitCountIn);

template void ZImgInfo::setVoxelFormat<float>(size_t validBitCountIn);

template void ZImgInfo::setVoxelFormat<double>(size_t validBitCountIn);

void ZImgInfo::setVoxelFormat(const QString& dt, size_t validBitCountIn)
{
  if (dt.startsWith("uint")) {
    voxelFormat = VoxelFormat::Unsigned;
  } else if (dt.startsWith("int")) {
    voxelFormat = VoxelFormat::Signed;
  } else if (dt.startsWith("float")) {
    voxelFormat = VoxelFormat::Float;
  } else {
    throw ZIOException(QString("invalid data type string %1").arg(dt));
  }
  if (dt.endsWith("8")) {
    bytesPerVoxel = 1;
  } else if (dt.endsWith("16")) {
    bytesPerVoxel = 2;
  } else if (dt.endsWith("32")) {
    bytesPerVoxel = 4;
  } else if (dt.endsWith("64")) {
    bytesPerVoxel = 8;
  } else {
    throw ZIOException(QString("invalid data type string %1").arg(dt));
  }
  validBitCount = validBitCountIn;
}

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconstant-conversion"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverflow"
#endif

template<typename TValue>
TValue ZImgInfo::dataRangeMin() const
{
  if (voxelFormat == VoxelFormat::Signed) {
    switch (bytesPerVoxel) {
      case 1:
        return std::numeric_limits<int8_t>::min();
        break;
      case 2:
        return std::numeric_limits<int16_t>::min();
        break;
      case 4:
        return std::numeric_limits<int32_t>::min();
        break;
      case 8:
        return std::numeric_limits<int64_t>::min();
        break;
      default:
        break;
    }
  }

  return static_cast<TValue>(0);
}

template uint8_t ZImgInfo::dataRangeMin<uint8_t>() const;

template uint16_t ZImgInfo::dataRangeMin<uint16_t>() const;

template uint32_t ZImgInfo::dataRangeMin<uint32_t>() const;

template uint64_t ZImgInfo::dataRangeMin<uint64_t>() const;

template int8_t ZImgInfo::dataRangeMin<int8_t>() const;

template int16_t ZImgInfo::dataRangeMin<int16_t>() const;

template int32_t ZImgInfo::dataRangeMin<int32_t>() const;

template int64_t ZImgInfo::dataRangeMin<int64_t>() const;

template float ZImgInfo::dataRangeMin<float>() const;

template double ZImgInfo::dataRangeMin<double>() const;

template<typename TValue>
TValue ZImgInfo::dataRangeMax() const
{
  if (voxelFormat == VoxelFormat::Float) {
    return static_cast<TValue>(1);
  }

  if (voxelFormat == VoxelFormat::Signed) {
    switch (bytesPerVoxel) {
      case 1:
        return std::numeric_limits<int8_t>::max();
        break;
      case 2:
        return std::numeric_limits<int16_t>::max();
        break;
      case 4:
        return std::numeric_limits<int32_t>::max();
        break;
      case 8:
        return std::numeric_limits<int64_t>::max();
        break;
      default:
        break;
    }
  } else {
    switch (bytesPerVoxel) {
      case 1:
        return validBitCount ? ((1 << validBitCount) - 1) : std::numeric_limits<uint8_t>::max();
        break;
      case 2:
        return validBitCount ? ((1 << validBitCount) - 1) : std::numeric_limits<uint16_t>::max();
        break;
      case 4:
        return std::numeric_limits<uint32_t>::max();
        break;
      case 8:
        return std::numeric_limits<uint64_t>::max();
        break;
      default:
        break;
    }
  }

  return static_cast<TValue>(0);
}

template uint8_t ZImgInfo::dataRangeMax<uint8_t>() const;

template uint16_t ZImgInfo::dataRangeMax<uint16_t>() const;

template uint32_t ZImgInfo::dataRangeMax<uint32_t>() const;

template uint64_t ZImgInfo::dataRangeMax<uint64_t>() const;

template int8_t ZImgInfo::dataRangeMax<int8_t>() const;

template int16_t ZImgInfo::dataRangeMax<int16_t>() const;

template int32_t ZImgInfo::dataRangeMax<int32_t>() const;

template int64_t ZImgInfo::dataRangeMax<int64_t>() const;

template float ZImgInfo::dataRangeMax<float>() const;

template double ZImgInfo::dataRangeMax<double>() const;

#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

} // namespace nim
