#pragma once

#include "zimginterface.h"
#include "zimgmetatag.h"

struct tiff;
using TIFF = struct tiff;

namespace nim {

class ZImg;

struct ZImgInfo;

uint16_t getTiffCompressionTag(Compression comp);

class ZTiffIFD
{
public:
  [[nodiscard]] bool isNormalImage() const
  {
    return !isReducedResolutionImage() && !isTransparencyMask();
  }

  [[nodiscard]] bool isReducedResolutionImage() const;

  [[nodiscard]] bool isMultipageDocument() const;

  [[nodiscard]] bool isTransparencyMask() const;

  [[nodiscard]] bool containsTag(uint64_t tag) const
  {
    return indexOf(tag) != -1;
  }

  [[nodiscard]] const ZImgMetatag& tag(size_t idx) const
  {
    return m_entries[idx];
  }

  // return -1 if not found
  [[nodiscard]] index_t indexOf(uint64_t tag) const;

  [[nodiscard]] VoxelFormat voxelFormat(size_t sample) const;

  [[nodiscard]] size_t samplesPerPixel() const;

  [[nodiscard]] size_t bitsPerSample(size_t sample) const;

  [[nodiscard]] size_t bitsPerSampleFromMaxSampleValue(size_t sample) const;

  [[nodiscard]] size_t imageWidth() const;

  [[nodiscard]] size_t imageHeight() const;

  [[nodiscard]] uint16_t photometricInterpretation() const;

  [[nodiscard]] QString imageDescriptionAsQString() const;

  [[nodiscard]] uint16_t orientation() const;

  [[nodiscard]] uint16_t compression() const;

  [[nodiscard]] uint16_t planarConfiguration() const;

  [[nodiscard]] int32_t extraSample() const;

  [[nodiscard]] bool isTiledImage() const;

  [[nodiscard]] uint64_t stripsPerImage() const;

  [[nodiscard]] uint64_t tilesPerImage() const;

  [[nodiscard]] uint64_t rowsPerStrip() const;

  [[nodiscard]] uint64_t stripOffsets(size_t idx) const;

  [[nodiscard]] uint64_t stripByteCounts(size_t idx) const;

  [[nodiscard]] uint64_t tileWidth() const;

  [[nodiscard]] uint64_t tileHeight() const;

  [[nodiscard]] uint64_t tileOffsets(size_t idx) const;

  [[nodiscard]] uint64_t tileByteCounts(size_t idx) const;

  [[nodiscard]] uint64_t offset() const
  {
    return m_offset;
  }

  [[nodiscard]] uint64_t nextIFDOffset() const
  {
    return m_nextIFDOffset;
  }

  void addField(const ZImgMetatag& fd)
  {
    m_entries.push_back(fd);
  }

  void addSubIFD(const ZTiffIFD& sub)
  {
    m_subIFDs.push_back(sub);
  }

  void setOffset(uint64_t off)
  {
    m_offset = off;
  }

  void setNextIFDOffset(uint64_t off)
  {
    m_nextIFDOffset = off;
  }

  [[nodiscard]] QString toQString() const;

  [[nodiscard]] std::string toString() const;

  [[nodiscard]] const std::vector<ZTiffIFD>& subIFDs() const
  {
    return m_subIFDs;
  }

  void setExifIFD(const ZTiffIFD& exif)
  {
    m_exifIFD.clear();
    m_exifIFD.push_back(exif);
  }

  [[nodiscard]] bool hasExifIFD() const
  {
    return !m_exifIFD.empty();
  }

  [[nodiscard]] const ZTiffIFD* exifIFD() const
  {
    if (m_exifIFD.empty()) {
      return nullptr;
    } else {
      return m_exifIFD.data();
    }
  }

  void setGpsIFD(const ZTiffIFD& gps)
  {
    m_gpsIFD.clear();
    m_gpsIFD.push_back(gps);
  }

  [[nodiscard]] bool hasGpsIFD() const
  {
    return !m_gpsIFD.empty();
  }

  [[nodiscard]] const ZTiffIFD* gpsIFD() const
  {
    if (m_gpsIFD.empty()) {
      return nullptr;
    } else {
      return m_gpsIFD.data();
    }
  }

  void setInteroperabilityIFD(const ZTiffIFD& interop)
  {
    m_interoperabilityIFD.clear();
    m_interoperabilityIFD.push_back(interop);
  }

  [[nodiscard]] bool hasInteroperabilityIFD() const
  {
    return !m_interoperabilityIFD.empty();
  }

  [[nodiscard]] const ZTiffIFD* InteroperabilityIFD() const
  {
    if (m_interoperabilityIFD.empty()) {
      return nullptr;
    } else {
      return m_interoperabilityIFD.data();
    }
  }

  [[nodiscard]] bool isGrayscaleColormap() const;

  // extract tags that contains meta data of this ifd
  [[nodiscard]] std::vector<ZImgMetatag> extractMetadata() const;

protected:
  [[nodiscard]] uint32_t subfileTypeData() const;

private:
  std::vector<ZImgMetatag> m_entries;
  uint64_t m_offset = 0;
  uint64_t m_nextIFDOffset = 0;
  std::vector<ZTiffIFD> m_subIFDs;
  std::vector<ZTiffIFD> m_exifIFD; // should contains 0 or 1 IFD
  std::vector<ZTiffIFD> m_gpsIFD; // should contains 0 or 1 IFD
  std::vector<ZTiffIFD> m_interoperabilityIFD; // should contains 0 or 1 IFD
};

class ZTiff
{
public:
  ZTiff();

  void addIFD(const ZTiffIFD& ifd)
  {
    m_ifds.push_back(ifd);
  }

  [[nodiscard]] QString toQString() const;

  [[nodiscard]] std::string toString() const;

  [[nodiscard]] const std::vector<ZTiffIFD>& ifds() const
  {
    return m_ifds;
  }

  // default is true, call before load and readInfo
  void setUseColormap(bool v)
  {
    m_useColormap = v;
  }

  [[nodiscard]] bool isUsingColormap() const
  {
    return m_useColormap;
  }

  // for tag only load, readImg* functions don't work
  void load(const QString& filename, bool tagOnly = false);

  void load(std::istream& fs, bool tagOnly = false);

  // close file, destructor of ZTiff or load function will do same thing
  void close()
  {
    m_ifds.clear();
    m_tif.reset();
  }

  [[nodiscard]] bool isValid() const
  {
    return !m_ifds.empty();
  }

  [[nodiscard]] bool isLsmFile() const;

  [[nodiscard]] bool isNativeEndianness() const
  {
    return m_isNativeEndianness;
  }

  // crash if file is not lsm
  [[nodiscard]] const ZImgMetatag& lsmInfoTag() const;

  // read
  void readInfoFromIFD(size_t ifdIdx, ZImgInfo& info)
  {
    readInfoFromIFD(m_ifds[ifdIdx], info);
  }

  void readInfoFromIFD(const ZTiffIFD& ifd, ZImgInfo& info) const;

  // input img dimensions should match the dimensions of ifd
  void readImgFromIFD(size_t ifdIdx, ZImg& img);

  void readImgFromIFD(const ZTiffIFD& ifd, ZImg& img);

  // do not throw ioexception, instead return empty img if failed
  ZImg readThumbnailFromIFD(const ZTiffIFD& ifd);

  // write a 16 + 8 + 20 * 8 + 8 = 192 bytes tiff header to mem, mem+stripOffset is the start of image data
  // mem+stripOffset+stripByteCount is the end of image data, stripOffset should be bigger than 192
  static void writeTiffHeader(uint8_t* mem,
                              size_t memSize,
                              size_t width,
                              size_t height,
                              size_t bitsPerSample,
                              size_t samplesPerPixel,
                              Compression compression,
                              uint64_t stripOffset,
                              uint64_t stripByteCount);

protected:
  uint64_t readIFD(std::istream& fs, ZTiffIFD& ifd, uint64_t off, bool bigtiff, bool swabflag) const;

  [[nodiscard]] static std::string tagToName(uint32_t tag) ;

  void readIFDs(const QString& filename, std::vector<ZTiffIFD>& ifds, bool& reverseEndianness) const;

  void readIFDs(std::istream& fs, std::vector<ZTiffIFD>& ifds, bool& reverseEndianness) const;

  // read img from current ifd
  void readImg(ZImg& img, bool divideByAlpha);

  // from current ifd
  size_t readStrip(uint32_t strip, uint8_t* buf, size_t width, size_t height, size_t nChannel, bool invert);

  void readTile(uint32_t tile, uint8_t* buf, size_t tileWidth, size_t tileHeight, size_t tileChannel, bool invert);

  // utils
  // convert RGBARGBA..... to RRR...GGG...BBB...AAA...
  static void separateChannel(const ZImg& bufImg, ZImg& img);

  // copy tile to correct image locastatic tion
  static void copyOneChannelTileToImg(const uint8_t* tileBuf,
                                      size_t tileWidth,
                                      size_t tileHeight,
                                      size_t voxelByteNumber,
                                      ZImg& img,
                                      size_t xStart,
                                      size_t yStart,
                                      size_t c);

  static void copyTileToImg(const uint8_t* tileBuf,
                            size_t tileWidth,
                            size_t tileHeight,
                            size_t numChannels,
                            size_t voxelByteNumber,
                            ZImg& img,
                            size_t xStart,
                            size_t yStart);

private:
  std::vector<ZTiffIFD> m_ifds;
  std::unique_ptr<TIFF, void (*)(TIFF*)> m_tif;
  bool m_useColormap = true;
  bool m_isNativeEndianness = false;
};

class ZTiffWriter
{
public:
  ZTiffWriter();

  // call in sequence
  void startWriting(const QString& filename, Compression comp, int32_t extraSample, bool bigTiff);

  // write img(z,c,t,l) to next ifd, if c == -1, write all channels
  // z, t, l must be valid index
  // if writeThumbnails is false, thumbnails of img are ignored, otherwise they will be written to subifd
  void writeIFD(const ZImg& img,
                size_t z,
                size_t t,
                index_t c = -1,
                bool writeThumbnails = true,
                const std::vector<ZImgMetatag>& additionalTags = std::vector<ZImgMetatag>());

  // close file, destructor of ZTiffWriter will do the same thing.
  // use to clean the state when use one ZTiffWriter to write multiple tifs
  void finishWriting()
  {
    m_tif.reset();
  }

private:
  // return a valid default compression
  static Compression defaultCompression(const ZImg* img);

  // return false if comp cannot be used for img
  static bool checkCompression(const ZImg* img, Compression comp);

private:
  std::unique_ptr<TIFF, void (*)(TIFF*)> m_tif;
  Compression m_compression = Compression::NONE;
  int32_t m_extraSample = 0;
};

} // namespace nim
