#include "zimginfo.h"
//#include <sstream>
#include <type_traits>
#include "zrandom.h"

namespace nim {

ZImgInfo::ZImgInfo()
{
  clear();
}

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
  return QString("width: %1").arg(width) %
         QString(", height: %1").arg(height) %
         QString(", depth: %1").arg(depth) %
         QString(", numChannels: %1").arg(numChannels) %
         QString(", numTimes: %1").arg(numTimes) %
         //QString(", numLocations: %1").arg(numLocations) %
         QString(", bytesPerVoxel: %1").arg(bytesPerVoxel) %
         QString(", voxelFormat: %1").arg(enumToString(voxelFormat)) %
         QString(", voxelSizeUnit: %1").arg(enumToString(voxelSizeUnit)) %
         QString(", voxelSizeX: %1").arg(voxelSizeX) %
         QString(", voxelSizeY: %1").arg(voxelSizeY) %
         QString(", voxelSizeZ: %1").arg(voxelSizeZ) %
         (lastChannelIsAlphaChannel ? QString(", alphaChannel: %1").arg(numChannels - 1) : QString("")) %
         (validBitCount > 0 ? QString(", validBitCount: %1").arg(validBitCount) : QString(""));
}

QString ZImgInfo::typeAsQString() const
{
  if (voxelFormat == VoxelFormat::Float) {
    switch (bytesPerVoxel) {
      case 4:
        return "float32";
      case 8:
        return "float64";
      default:
        break;
    }
  } else if (voxelFormat == VoxelFormat::Signed) {
    switch (bytesPerVoxel) {
      case 1:
        return "int8";
      case 2:
        return "int16";
      case 4:
        return "int32";
      case 8:
        return "int64";
      default:
        break;
    }
  } else {
    switch (bytesPerVoxel) {
      case 1:
        return "uint8";
      case 2:
        return "uint16";
      case 4:
        return "uint32";
      case 8:
        return "uint64";
      default:
        break;
    }
  }
  return "invalid image type";
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
        channelColors[i] = col4(ZRandomInstance.randInt(255),
                                ZRandomInstance.randInt(255),
                                ZRandomInstance.randInt(255));
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
void ZImgInfo::setVoxelFormat()
{
  validBitCount = 0;
  bytesPerVoxel = sizeof(TVoxel);
  if (std::is_integral<TVoxel>::value) {
    if (std::is_signed<TVoxel>::value)
      voxelFormat = VoxelFormat::Signed;
    else
      voxelFormat = VoxelFormat::Unsigned;
  } else if (std::is_floating_point<TVoxel>::value) {
    voxelFormat = VoxelFormat::Float;
  } else {
    throw ZImgException("set voxel format need numeric type");
  }
}

template void ZImgInfo::setVoxelFormat<uint8_t>();

template void ZImgInfo::setVoxelFormat<uint16_t>();

template void ZImgInfo::setVoxelFormat<uint32_t>();

template void ZImgInfo::setVoxelFormat<uint64_t>();

template void ZImgInfo::setVoxelFormat<int8_t>();

template void ZImgInfo::setVoxelFormat<int16_t>();

template void ZImgInfo::setVoxelFormat<int32_t>();

template void ZImgInfo::setVoxelFormat<int64_t>();

template void ZImgInfo::setVoxelFormat<float>();

template void ZImgInfo::setVoxelFormat<double>();

#ifndef _MSC_VER
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconstant-conversion"
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
  } else if (voxelFormat == VoxelFormat::Signed) {
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

#ifndef _MSC_VER
#pragma clang diagnostic pop
#endif

} // namespace
