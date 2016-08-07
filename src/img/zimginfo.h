#ifndef ZIMGINFO_H
#define ZIMGINFO_H

#include <vector>
#include "zimginterface.h"
#include <QString>

namespace nim {

#pragma pack(push, 1)

struct ZImgInfo
{
  ZImgInfo();

  ZImgInfo(size_t width, size_t height, size_t depth = 1,
           size_t numChannels = 1, size_t numTimes = 1,
           size_t bytePerVox = 1, VoxelFormat vf = VoxelFormat::Unsigned);

  void clear();

  void swap(ZImgInfo& other) noexcept;

  ZImgInfo(ZImgInfo&&) = default;

  ZImgInfo& operator=(ZImgInfo&&) = default;

  ZImgInfo(const ZImgInfo&) = default;

  ZImgInfo& operator=(const ZImgInfo&) = default;

  QString toQString() const;

  // return img data type as string "float32", "int8" ...
  QString typeAsQString() const
  {
    return (voxelFormat == VoxelFormat::Float ? "float" : voxelFormat == VoxelFormat::Signed ? "int" : "uint") +
           QString::number(bytesPerVoxel * 8);
  }

  // channel name for interface, prepend ch1, ch2, ...
  QString displayChannelName(size_t c) const;

  // create default value if not filled or number don't match
  // image reader or creator should take care of this to make sure all information exist
  void createDefaultChannelNames();

  void createDefaultChannelColors();

  void createDefaultTimeStamps();

  //void createDefaultLocations();
  // call above all
  void createDefaultDescriptions();

  // return true if one dimension is zero
  inline bool isEmpty() const
  { return width == 0 || height == 0 || depth == 0 || numChannels == 0 || numTimes == 0; }

  inline bool isSameSize(const ZImgInfo& other) const
  {
    return width == other.width && height == other.height && depth == other.depth &&
           numChannels == other.numChannels && numTimes == other.numTimes;
  }

  inline bool isSameType(const ZImgInfo& other) const
  { return voxelFormat == other.voxelFormat && bytesPerVoxel == other.bytesPerVoxel; }

  inline static size_t numDimensions()
  { return 5; }

  // access
  inline void setSize(Dimension dim, size_t size)
  {
    (&width)[enumToUnderlyingType(dim)] = size;
    if (dim >= Dimension::C) createDefaultDescriptions();
  }

  // sz should has at least numDimensions() elements
  inline void setSize(size_t* sz)
  {
    memcpy(&width, sz, sizeof(size_t) * 6);
    createDefaultDescriptions();
  }

  inline size_t size(Dimension dim) const
  { return (&width)[enumToUnderlyingType(dim)]; }

  inline size_t size(size_t dim) const
  { return (&width)[dim]; }

  // note: time stride is meaningless since the memory is not contiguous
  inline size_t stride(Dimension dim) const
  {
    size_t res = 1;
    for (int i = 0; i < enumToUnderlyingType(dim); ++i) res *= (&width)[i];
    return res;
  }

  inline size_t stride(size_t dim) const
  {
    size_t res = 1;
    for (size_t i = 0; i < dim; ++i) res *= (&width)[i];
    return res;
  }

  inline size_t& operator[](size_t i)
  { return (&width)[i]; }

  inline const size_t& operator[](size_t i) const
  { return (&width)[i]; }

  inline size_t& operator[](Dimension i)
  { return (&width)[enumToUnderlyingType(i)]; }

  inline const size_t& operator[](Dimension i) const
  { return (&width)[enumToUnderlyingType(i)]; }

  inline bool isAlphaChannel(size_t ch) const
  { return lastChannelIsAlphaChannel && ch + 1 == numChannels; }

  // if current or result voxelSizeUnit is Voxel, result is meaningless
  double voxelSizeXInUnit(VoxelSizeUnit unit) const;

  double voxelSizeYInUnit(VoxelSizeUnit unit) const;

  double voxelSizeZInUnit(VoxelSizeUnit unit) const;

  inline double voxelSizeXInUm() const
  { return voxelSizeXInUnit(VoxelSizeUnit::um); }

  inline double voxelSizeYInUm() const
  { return voxelSizeYInUnit(VoxelSizeUnit::um); }

  inline double voxelSizeZInUm() const
  { return voxelSizeZInUnit(VoxelSizeUnit::um); }

  // set voxel format from template argument, don't accept const type
  template<typename TVoxel>
  void setVoxelFormat();

  // property of img type
  // intensity range of current img type, for float img, range is [0.0 1.0]
  // use template return type because img can be any type, and even double type can not represent all 64-bit integer type value
  template<typename TValue = double>
  TValue dataRangeMin() const;

  template<typename TValue = double>
  TValue dataRangeMax() const;

  size_t width;
  size_t height;
  size_t depth;
  size_t numChannels;
  size_t numTimes;
  //size_t numLocations;  // not used

  size_t bytesPerVoxel;   // for one channel
  VoxelFormat voxelFormat;
  size_t validBitCount;    // used with 8-bits or 16-bits images to indicate the maximum possible valid bits, 0 if default

  VoxelSizeUnit voxelSizeUnit;
  double voxelSizeX;
  double voxelSizeY;
  double voxelSizeZ;

  std::vector<double> timeStamps;
  std::vector<QString> channelNames;
  std::vector<col4> channelColors;
  //std::vector<Location> locations;
  std::vector<double> position;
  bool lastChannelIsAlphaChannel;

  inline size_t voxelByteNumber() const
  { return bytesPerVoxel; } // // voxel of one channel
  inline size_t rowVoxelNumber() const
  { return width; }

  inline size_t rowByteNumber() const
  { return width * bytesPerVoxel; }

  inline size_t planeVoxelNumber() const
  { return width * height; }

  inline size_t planeByteNumber() const
  { return width * height * bytesPerVoxel; }

  inline size_t channelVoxelNumber() const
  { return width * height * depth; }

  inline size_t channelByteNumber() const
  { return width * height * depth * bytesPerVoxel; }

  inline size_t timeVoxelNumber() const
  { return width * height * depth * numChannels; }

  inline size_t timeByteNumber() const
  { return width * height * depth * numChannels * bytesPerVoxel; }

  //inline size_t locationVoxelNumber() const { return width * height * depth * numChannels * numTimes; }
  //inline size_t locationByteNumber() const { return width * height * depth * numChannels * numTimes * bytesPerVoxel; }
  inline size_t voxelNumber() const
  { return width * height * depth * numChannels * numTimes; }

  inline size_t byteNumber() const
  { return width * height * depth * numChannels * numTimes * bytesPerVoxel; }
};

#pragma pack(pop)

} // namespace

#endif // ZIMGINFO_H
