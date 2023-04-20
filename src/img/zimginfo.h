#pragma once

#include "zimginterface.h"
#include "zjson.h"
#include "zlog.h"
#include <vector>

namespace nim {

struct ZImgInfo
{
  ZImgInfo() = default;

  ZImgInfo(size_t w,
           size_t h,
           size_t d = 1,
           size_t c = 1,
           size_t t = 1,
           size_t bytePerVox = 1,
           VoxelFormat vf = VoxelFormat::Unsigned);

  void clear();

  void swap(ZImgInfo& other) noexcept;

  [[nodiscard]] inline QString toQString() const
  {
    return jsonToQString(*this);
  }

  [[nodiscard]] inline std::string toString() const
  {
    return jsonToString(*this);
  }

  // return img data type as string "float32", "int8" ...
  [[nodiscard]] QString typeAsQString() const
  {
    return (voxelFormat == VoxelFormat::Float    ? "float"
            : voxelFormat == VoxelFormat::Signed ? "int"
                                                 : "uint") +
           QString::number(bytesPerVoxel * 8);
  }

  // channel name for interface, prepend ch1, ch2, ...
  [[nodiscard]] QString displayChannelName(size_t c) const;

  // create default value if not filled or number don't match
  // image reader or creator should take care of this to make sure all information exist
  void createDefaultChannelNames();

  void createDefaultChannelColors();

  void createDefaultTimeStamps();

  // void createDefaultLocations();
  //  call above all
  void createDefaultDescriptions();

  // return true if one dimension is zero
  [[nodiscard]] inline bool isEmpty() const
  {
    return width == 0 || height == 0 || depth == 0 || numChannels == 0 || numTimes == 0;
  }

  [[nodiscard]] inline bool isSameSize(const ZImgInfo& other) const
  {
    return width == other.width && height == other.height && depth == other.depth && numChannels == other.numChannels &&
           numTimes == other.numTimes;
  }

  [[nodiscard]] inline bool isSameType(const ZImgInfo& other) const
  {
    return voxelFormat == other.voxelFormat && bytesPerVoxel == other.bytesPerVoxel;
  }

  static constexpr size_t numDimensions()
  {
    return 5;
  }

  static constexpr std::array<Dimension, 5> dimensions()
  {
    return {
      {Dimension::X, Dimension::Y, Dimension::Z, Dimension::C, Dimension::T}
    };
  };

  // access
  inline void setSize(Dimension dim, size_t size)
  {
    switch (to_underlying(dim)) {
      case 0:
        width = size;
        break;
      case 1:
        height = size;
        break;
      case 2:
        depth = size;
        break;
      case 3:
        numChannels = size;
        break;
      case 4:
        numTimes = size;
        break;
      default:
        CHECK(false);
    }

    if (dim >= Dimension::C) {
      createDefaultDescriptions();
    }
  }

  [[nodiscard]] inline size_t size(Dimension dim) const
  {
    switch (to_underlying(dim)) {
      case 0:
        return width;
      case 1:
        return height;
      case 2:
        return depth;
      case 3:
        return numChannels;
      case 4:
        return numTimes;
      default:
        CHECK(false);
    }
  }

  [[nodiscard]] inline size_t size(size_t dim) const
  {
    switch (dim) {
      case 0:
        return width;
      case 1:
        return height;
      case 2:
        return depth;
      case 3:
        return numChannels;
      case 4:
        return numTimes;
      default:
        CHECK(false);
    }
  }

  // note: time stride is meaningless since the memory is not contiguous
  [[nodiscard]] inline size_t stride(Dimension dim) const
  {
    size_t res = 1;
    auto ddim = to_underlying(dim);
    if (ddim > 0) {
      res = width;
    }
    if (ddim > 1) {
      res *= height;
    }
    if (ddim > 2) {
      res *= depth;
    }
    if (ddim > 3) {
      res *= numChannels;
    }
    return res;
  }

  [[nodiscard]] inline size_t stride(size_t dim) const
  {
    size_t res = 1;
    if (dim > 0) {
      res = width;
    }
    if (dim > 1) {
      res *= height;
    }
    if (dim > 2) {
      res *= depth;
    }
    if (dim > 3) {
      res *= numChannels;
    }
    return res;
  }

  inline size_t& operator[](size_t i)
  {
    switch (i) {
      case 0:
        return width;
      case 1:
        return height;
      case 2:
        return depth;
      case 3:
        return numChannels;
      case 4:
        return numTimes;
      default:
        CHECK(false);
    }
  }

  inline const size_t& operator[](size_t i) const
  {
    switch (i) {
      case 0:
        return width;
      case 1:
        return height;
      case 2:
        return depth;
      case 3:
        return numChannels;
      case 4:
        return numTimes;
      default:
        CHECK(false);
    }
  }

  inline size_t& operator[](Dimension i)
  {
    switch (to_underlying(i)) {
      case 0:
        return width;
      case 1:
        return height;
      case 2:
        return depth;
      case 3:
        return numChannels;
      case 4:
        return numTimes;
      default:
        CHECK(false);
    }
  }

  inline const size_t& operator[](Dimension i) const
  {
    switch (to_underlying(i)) {
      case 0:
        return width;
      case 1:
        return height;
      case 2:
        return depth;
      case 3:
        return numChannels;
      case 4:
        return numTimes;
      default:
        CHECK(false);
    }
  }

  [[nodiscard]] inline bool isAlphaChannel(size_t ch) const
  {
    return lastChannelIsAlphaChannel && ch + 1 == numChannels;
  }

  // if current or result voxelSizeUnit is Voxel, throw exception
  [[nodiscard]] double voxelSizeXInUnit(VoxelSizeUnit unit) const;

  [[nodiscard]] double voxelSizeYInUnit(VoxelSizeUnit unit) const;

  [[nodiscard]] double voxelSizeZInUnit(VoxelSizeUnit unit) const;

  [[nodiscard]] inline double voxelSizeXInUm() const
  {
    return voxelSizeXInUnit(VoxelSizeUnit::um);
  }

  [[nodiscard]] inline double voxelSizeYInUm() const
  {
    return voxelSizeYInUnit(VoxelSizeUnit::um);
  }

  [[nodiscard]] inline double voxelSizeZInUm() const
  {
    return voxelSizeZInUnit(VoxelSizeUnit::um);
  }

  // set voxel format from template argument, don't accept const type
  template<typename TVoxel>
  void setVoxelFormat(size_t validBitCountIn = 0);

  // set voxel format from data type string such as "float32", "int8" ...
  void setVoxelFormat(const QString& dt, size_t validBitCountIn = 0);

  // property of img type
  // intensity range of current img type, for float img, range is [0.0 1.0]
  // use template return type because img can be any type, and even double type can not represent all 64-bit integer
  // type value
  template<typename TValue = double>
  [[nodiscard]] TValue dataRangeMin() const;

  template<typename TValue = double>
  [[nodiscard]] TValue dataRangeMax() const;

  size_t width = 0;
  size_t height = 0;
  size_t depth = 0;
  size_t numChannels = 0;
  size_t numTimes = 0;
  // size_t numLocations;  // not used

  size_t bytesPerVoxel = 1; // for one channel
  VoxelFormat voxelFormat = VoxelFormat::Unsigned;
  // used with 8-bits or 16-bits images to indicate the maximum possible valid bits, 0 if default
  size_t validBitCount = 0;

  VoxelSizeUnit voxelSizeUnit = VoxelSizeUnit::none;
  double voxelSizeX = 1.;
  double voxelSizeY = 1.;
  double voxelSizeZ = 1.;

  std::vector<double> timeStamps;
  std::vector<QString> channelNames;
  std::vector<col4> channelColors;
  // std::vector<Location> locations;
  std::vector<double> position;
  bool lastChannelIsAlphaChannel = false;

  [[nodiscard]] inline size_t voxelByteNumber() const
  {
    return bytesPerVoxel;
  } // voxel of one channel

  [[nodiscard]] inline size_t rowVoxelNumber() const
  {
    return width;
  }

  [[nodiscard]] inline size_t rowByteNumber() const
  {
    return width * bytesPerVoxel;
  }

  [[nodiscard]] inline size_t planeVoxelNumber() const
  {
    return width * height;
  }

  [[nodiscard]] inline size_t planeByteNumber() const
  {
    return width * height * bytesPerVoxel;
  }

  [[nodiscard]] inline size_t channelVoxelNumber() const
  {
    return width * height * depth;
  }

  [[nodiscard]] inline size_t channelByteNumber() const
  {
    return width * height * depth * bytesPerVoxel;
  }

  [[nodiscard]] inline size_t timeVoxelNumber() const
  {
    return width * height * depth * numChannels;
  }

  [[nodiscard]] inline size_t timeByteNumber() const
  {
    return width * height * depth * numChannels * bytesPerVoxel;
  }

  // inline size_t locationVoxelNumber() const { return width * height * depth * numChannels * numTimes; }
  // inline size_t locationByteNumber() const { return width * height * depth * numChannels * numTimes * bytesPerVoxel;
  // }
  [[nodiscard]] inline size_t voxelNumber() const
  {
    return width * height * depth * numChannels * numTimes;
  }

  [[nodiscard]] inline size_t byteNumber() const
  {
    return width * height * depth * numChannels * numTimes * bytesPerVoxel;
  }

  template<typename TVoxel>
  [[nodiscard]] bool isType() const;

  // given an bin index, return data range this bin represent
  // not very accurate for 64-bit integer type
  [[nodiscard]] std::pair<double, double> binRange(size_t binIdx, size_t nbins) const
  {
    if (voxelFormat == VoxelFormat::Float) {
      return binRange(binIdx, 0.0, 1.0, nbins);
    } else if (voxelFormat == VoxelFormat::Signed) {
      return binRange(binIdx, dataRangeMin<int64_t>(), dataRangeMax<int64_t>(), nbins);
    } else {
      return binRange(binIdx, dataRangeMin<uint64_t>(), dataRangeMax<uint64_t>(), nbins);
    }
  }

  template<typename TRange>
  [[nodiscard]] std::pair<double, double>
  binRange(size_t binIdx, TRange minData, TRange maxData, size_t nbins = 0) const
  {
    if (nbins == 0) {
      nbins = bytesPerVoxel > 1 ? 65536 : 256;
    }
    double min;
    double max;
    if (voxelFormat == VoxelFormat::Float) {
      double minD = minData;
      double maxD = maxData;
      double binSize = (maxD - minD) / nbins;
      min = binIdx * binSize + minD;
      max = min + binSize;
    } else {
      double minD = minData;
      double maxD = maxData;
      double binSize = (maxD - minD + 1) / nbins;
      min = binSize * binIdx + minD;
      max = min + binSize;
    }
    return std::make_pair(min, max);
  }
};

inline void tag_invoke(const json::value_from_tag&, json::value& jv, const ZImgInfo& info)
{
  auto& jo = jv.emplace_object();
  jo["width"] = info.width;
  jo["height"] = info.height;
  jo["depth"] = info.depth;
  jo["numChannels"] = info.numChannels;
  jo["numTimes"] = info.numTimes;
  jo["bytesPerVoxel"] = info.bytesPerVoxel;
  jo["voxelFormat"] = json::value_from(enumToString(info.voxelFormat));
  jo["validBitCount"] = info.validBitCount;
  jo["voxelSizeUnit"] = json::value_from(enumToString(info.voxelSizeUnit));
  jo["voxelSizeX"] = info.voxelSizeX;
  jo["voxelSizeY"] = info.voxelSizeY;
  jo["voxelSizeZ"] = info.voxelSizeZ;
  jo["timeStamps"] = json::value_from(info.timeStamps);
  jo["channelNames"] = json::value_from(info.channelNames);
  jo["channelColors"] = json::value_from(info.channelColors);
  jo["position"] = json::value_from(info.position);
  jo["lastChannelIsAlphaChannel"] = info.lastChannelIsAlphaChannel;
}

inline ZImgInfo tag_invoke(const json::value_to_tag<ZImgInfo>&, const json::value& jv)
{
  ZImgInfo info;
  info.width = json::value_to<size_t>(jv.at("width"));
  info.height = json::value_to<size_t>(jv.at("height"));
  info.depth = json::value_to<size_t>(jv.at("depth"));
  info.numChannels = json::value_to<size_t>(jv.at("numChannels"));
  info.numTimes = json::value_to<size_t>(jv.at("numTimes"));
  info.bytesPerVoxel = json::value_to<size_t>(jv.at("bytesPerVoxel"));
  info.voxelFormat = stringToEnum<VoxelFormat>(jv.at("voxelFormat").as_string());
  info.validBitCount = json::value_to<size_t>(jv.at("validBitCount"));
  info.voxelSizeUnit = stringToEnum<VoxelSizeUnit>(jv.at("voxelSizeUnit").as_string());
  info.voxelSizeX = json::value_to<double>(jv.at("voxelSizeX"));
  info.voxelSizeY = json::value_to<double>(jv.at("voxelSizeY"));
  info.voxelSizeZ = json::value_to<double>(jv.at("voxelSizeZ"));
  info.timeStamps = json::value_to<std::vector<double>>(jv.at("timeStamps"));
  info.channelNames = json::value_to<std::vector<QString>>(jv.at("channelNames"));
  info.channelColors = json::value_to<std::vector<col4>>(jv.at("channelColors"));
  info.position = json::value_to<std::vector<double>>(jv.at("position"));
  info.lastChannelIsAlphaChannel = jv.at("lastChannelIsAlphaChannel").as_bool();
  return info;
}

} // namespace nim
