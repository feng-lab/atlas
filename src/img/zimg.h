#pragma once

#include <QStringList>
#include "zimginfo.h"
#include "zimgregion.h"
#include "zimgmetatag.h"
#include "zimgmetadatabase.h"

namespace nim {

class ZImgMetadata : public ZImgMetadataBase<ZImgMetatag>
{
public:
  ZImgMetadata();

  QString toQString() const;
};

class ZImg;

class ZImgThumbernail : public ZImgMetadataBase<ZImg>
{
public:
  ZImgThumbernail();

  QString toQString() const;
};

// throw ZIOException if file does not exist
struct ZImgSource
{
  ZImgSource();

  ZImgSource(const QString& fn, const ZImgRegion& rgn = ZImgRegion(), size_t scene = 0,
             FileFormat format = FileFormat::Unknown);

  ZImgSource(const QStringList& fns, Dimension catDim, const ZImgRegion& rgn = ZImgRegion(), size_t scene = 0,
             FileFormat format = FileFormat::Unknown,
             bool expandXY = false, bool expandWithMaxValue = false);

  inline bool operator==(const ZImgSource& other) const
  {
    if (filenames == other.filenames) {
      if (filenames.size() == 1) {
        return region == other.region && scene == other.scene;
      } else {
        return catDim == other.catDim && region == other.region &&
               scene == other.scene && expandXY == other.expandXY &&
               expandWithMaxValue == other.expandWithMaxValue;
      }
    } else {
      return false;
    }
  }

  inline bool operator!=(const ZImgSource& other) const
  {
    return !(*this == other);
  }

  QString toQString() const;

  QStringList filenames;
  Dimension catDim = Dimension::Z;
  ZImgRegion region;
  size_t scene = 0;
  FileFormat format = FileFormat::Unknown;
  bool expandXY = false;
  bool expandWithMaxValue = false;
  int64_t totalFileSize = 0;
};

class ZImgSubBlock
{
public:
  ZImgSubBlock(size_t ratio_, size_t t_, int64_t z_,
               int64_t x_, int64_t y_, int64_t width_, int64_t height_)
    : ratio(ratio_), t(t_), z(z_), x(x_), y(y_), width(width_), height(height_)
  {}

  virtual ~ZImgSubBlock();

  // subclass read should depend its own members rather than member of this class
  virtual std::shared_ptr<ZImg> read() const = 0;

  size_t ratio;  // realsize / storedsize
  size_t t;
  int64_t z;
  int64_t x;
  int64_t y;
  int64_t width;
  int64_t height;
};

// Dimension order of ZImg is XYZCT
// this class might throw ZImgException or ZIOException if error

class ZImg
{
public:
  enum class CombineMode
  {
    Max, Min, Mean, Median
  };

  enum class ThresholdMode
  {
    IncludeThreshold, ExcludeThreshold
  };

  // create empty image
  ZImg();

  // create image with size and attribute specified by info and set all data to default value
  // see allocate() for default voxel value
  explicit ZImg(const ZImgInfo& info);

  //
  ZImg(const ZImg& other);

  ZImg(ZImg&& other) noexcept;

  // read image from file, throw ZIOException if read failed, might throw ZImgException if can't allocate memory
  explicit ZImg(const QString& filename, ZImgRegion region = ZImgRegion(), size_t scene = 0,
                FileFormat format = FileFormat::Unknown);

  explicit ZImg(const ZImgSource& imgSource);

  ~ZImg();

  // clear all data
  void clear();

  void swap(ZImg& other) noexcept;

  ZImg& operator=(ZImg other) noexcept
  {
    swap(other);
    return *this;
  }

  // qt style read write name filter for filedialog
  static void getQtReadNameFilter(QStringList& filters, QList<FileFormat>& formats);

  static void getQtWriteNameFilter(QStringList& filters, QList<FileFormat>& formats, QList<Compression>& comps);

  // return true if file extension is supported for read
  static bool fileExtensionReadSupported(const QString& filename);

  static bool fileExtensionWriteSupported(const QString& filename);

  // throw ZIOException if io error or empty image, might throw ZImgException if can't allocate memory
  void load(const QString& filename, size_t scene = 0, FileFormat format = FileFormat::Unknown);

  void load(const QString& filename, ZImgRegion region, size_t scene = 0, FileFormat format = FileFormat::Unknown);

  // load a sequence of imgs, cat these imgs along dimension "catDim"
  // imgs should have same size in other dimensions and have same type
  // throw ZIOException if io error, might throw ZImgException if can't allocate memory or can't cat imgs
  // expandXY can not be true if catDim is Dimension::X or Dimension::Y
  void load(const QStringList& fileList, Dimension catDim, size_t scene = 0, FileFormat format = FileFormat::Unknown,
            bool expandXY = false,
            bool expandWithMaxValue = false);

  void load(const QStringList& fileList, Dimension catDim, const ZImgRegion& region, size_t scene = 0,
            FileFormat format = FileFormat::Unknown, bool expandXY = false,
            bool expandWithMaxValue = false);

  void load(const ZImgSource& imgSource);

  void
  save(const QString& filename, FileFormat format = FileFormat::Unknown, Compression comp = Compression::AUTO) const;

  // convenient function to get img information from file, throw ZIOException if read error or empty image
  static std::vector<ZImgInfo>
  readImgInfo(const QString& filename, std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks = nullptr,
              FileFormat format = FileFormat::Unknown);

  // throw ZIOException if sequence is not valid or empty image
  static std::vector<ZImgInfo> readImgInfo(const QStringList& fileList, Dimension catDim,
                                           std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks = nullptr,
                                           FileFormat format = FileFormat::Unknown, bool expandXY = false);

  // wrap exist raw data as ZImg, ZImg will **not** free the memory after using
  // only accept non-const data pointer (link error if input is const)
  // previous data of current img will be cleared
  template<typename TVoxel>
  void wrapData(TVoxel* data, size_t width, size_t height, size_t depth = 1,
                size_t numChannels = 1, size_t numTimes = 1);

  void wrapData(void* data, const ZImgInfo& info);

  inline bool isEmpty() const
  { return m_data.empty() || m_info.isEmpty(); }

  inline bool hasThumbnail() const
  { return !m_thumbnail.isEmpty(); }

  inline const ZImgThumbernail& thumbnail() const
  { return m_thumbnail; }

  inline const ZImgInfo& info() const
  { return m_info; }

  inline const ZImgMetadata& metadata() const
  { return m_metadata; }

  inline ZImgThumbernail& thumbnailRef()
  { return m_thumbnail; }

  inline ZImgInfo& infoRef()
  { return m_info; }

  inline ZImgMetadata& metadataRef()
  { return m_metadata; }

  template<typename TVoxel>
  bool isType() const;

  inline bool isSameType(const ZImg& other) const
  { return m_info.isSameType(other.m_info); }

  inline bool isSameSize(const ZImg& other) const
  { return m_info.isSameSize(other.m_info); }

  inline bool contains(size_t x, size_t y, size_t z, size_t c, size_t t = 0) const
  {
    return !isEmpty() && x < m_info.width && y < m_info.height && z < m_info.depth &&
           c < m_info.numChannels && t < m_info.numTimes;
  }

  inline size_t size(Dimension dim) const
  { return m_info.size(dim); }

  inline size_t size(size_t dim) const
  { return m_info.size(dim); }

  //inline bool isMultiLocationsImg() const { return m_info.numLocations > 1; }
  inline bool isTimeSeries() const
  { return m_info.numTimes > 1; }

  inline bool isMultiChannelsImg() const
  { return m_info.numChannels > 1; }

  // imgs that have x,y,z dimensions and don't have t,l dimensions are 3D Img, x,y dimensions can be singleton
  inline bool is3DImg() const
  { return !isEmpty() && m_info.numTimes == 1 && m_info.depth > 1; }

  // imgs that have x,y dimensions and don't have z,t,l dimensions are 2D Img, x,y dimensions can be singleton
  inline bool is2DImg() const
  { return !isEmpty() && m_info.numTimes == 1 && m_info.depth == 1; }

  inline size_t voxelByteNumber() const
  { return m_info.voxelByteNumber(); }

  inline size_t rowVoxelNumber() const
  { return m_info.rowVoxelNumber(); }

  inline size_t rowByteNumber() const
  { return m_info.rowByteNumber(); }

  inline size_t planeVoxelNumber() const
  { return m_info.planeVoxelNumber(); }

  inline size_t planeByteNumber() const
  { return m_info.planeByteNumber(); }

  inline size_t channelVoxelNumber() const
  { return m_info.channelVoxelNumber(); }

  inline size_t channelByteNumber() const
  { return m_info.channelByteNumber(); }

  inline size_t timeVoxelNumber() const
  { return m_info.timeVoxelNumber(); }

  inline size_t timeByteNumber() const
  { return m_info.timeByteNumber(); }

  //inline size_t locationVoxelNumber() const { return m_info.locationVoxelNumber(); }
  //inline size_t locationByteNumber() const { return m_info.locationByteNumber(); }
  inline size_t voxelNumber() const
  { return m_info.voxelNumber(); }

  inline size_t byteNumber() const
  { return m_info.byteNumber(); }

  inline size_t width() const
  { return m_info.width; }

  inline size_t height() const
  { return m_info.height; }

  inline size_t depth() const
  { return m_info.depth; }

  inline size_t numChannels() const
  { return m_info.numChannels; }

  inline size_t numTimes() const
  { return m_info.numTimes; }

  //inline size_t numLocations() const { return m_info.numLocations; }
  inline VoxelFormat voxelFormat() const
  { return m_info.voxelFormat; }

  inline size_t bytesPerVoxel() const
  { return m_info.bytesPerVoxel; }

  inline size_t validBitCount() const
  { return m_info.validBitCount; }

  inline VoxelSizeUnit voxelSizeUnit() const
  { return m_info.voxelSizeUnit; }

  inline double voxelSizeX() const
  { return m_info.voxelSizeX; }

  inline double voxelSizeY() const
  { return m_info.voxelSizeY; }

  inline double voxelSizeZ() const
  { return m_info.voxelSizeZ; }

  // if current or result voxelSizeUnit is Voxel, result is meaningless
  inline double voxelSizeXInUnit(VoxelSizeUnit unit) const
  { return m_info.voxelSizeXInUnit(unit); }

  inline double voxelSizeYInUnit(VoxelSizeUnit unit) const
  { return m_info.voxelSizeYInUnit(unit); }

  inline double voxelSizeZInUnit(VoxelSizeUnit unit) const
  { return m_info.voxelSizeZInUnit(unit); }

  inline double voxelSizeXInUm() const
  { return voxelSizeXInUnit(VoxelSizeUnit::um); }

  inline double voxelSizeYInUm() const
  { return voxelSizeYInUnit(VoxelSizeUnit::um); }

  inline double voxelSizeZInUm() const
  { return voxelSizeZInUnit(VoxelSizeUnit::um); }

  inline col4 channelColor(size_t c) const
  { return m_info.channelColors[c]; }

  //inline Location location(size_t l) const { return m_info.locations[l]; }
  inline const QString& channelName(size_t c) const
  { return m_info.channelNames[c]; }

  inline QString displayChannelName(size_t c) const
  { return m_info.displayChannelName(c); }

  inline double timeStamp(size_t t) const
  { return m_info.timeStamps[t]; }

  // remove old data and allocate data space based on current info
  // if voxel type is signed integer, data will be filled with that type's minimum negative value
  // otherwise data will be set to 0
  // throw ZImgException if can not allocate memory
  void allocate();

  template<typename T = uint8_t>
  inline T* timeData(size_t t)
  { return bit_cast<T*>(&(m_data[t][0])); }

  template<typename T = uint8_t>
  inline T* channelData(size_t c, size_t t = 0)
  { return bit_cast<T*>(&(m_data[t][0]) + c * m_info.channelByteNumber()); }

  template<typename T = uint8_t>
  inline T* planeData(size_t z, size_t c = 0, size_t t = 0)
  { return bit_cast<T*>(&(m_data[t][0]) + c * m_info.channelByteNumber() + z * m_info.planeByteNumber()); }

  template<typename T = uint8_t>
  inline T* rowData(size_t y, size_t z = 0, size_t c = 0, size_t t = 0)
  {
    return bit_cast<T*>(
      &(m_data[t][0]) + c * m_info.channelByteNumber() + z * m_info.planeByteNumber() + y * m_info.rowByteNumber());
  }

  template<typename T = uint8_t>
  inline T* data(size_t x, size_t y, size_t z = 0, size_t c = 0, size_t t = 0)
  {
    return bit_cast<T*>(
      &(m_data[t][0]) + c * m_info.channelByteNumber() + z * m_info.planeByteNumber() + y * m_info.rowByteNumber()
      + x * m_info.voxelByteNumber());
  }

  template<typename T = uint8_t>
  inline T* data(const ZVoxelCoordinate& coord)
  {
    return bit_cast<T*>(
      &(m_data[coord.t][0]) + coord.c * m_info.channelByteNumber() + coord.z * m_info.planeByteNumber() +
      coord.y * m_info.rowByteNumber() + coord.x * m_info.voxelByteNumber());
  }

  template<typename T = uint8_t>
  inline T* data(size_t idx)
  {
    //size_t l = idx / m_info.locationVoxelNumber();
    //idx -= l * m_info.locationVoxelNumber();
    size_t t = idx / m_info.timeVoxelNumber();
    idx -= t * m_info.timeVoxelNumber();
    return bit_cast<T*>(&(m_data[t][0]) + idx * m_info.voxelByteNumber());
  }

  template<typename T = uint8_t>
  inline const T* timeData(size_t t) const
  { return bit_cast<T*>(&(m_data[t][0])); }

  template<typename T = uint8_t>
  inline const T* channelData(size_t c, size_t t = 0) const
  { return bit_cast<T*>(&(m_data[t][0]) + c * m_info.channelByteNumber()); }

  template<typename T = uint8_t>
  inline const T* planeData(size_t z, size_t c = 0, size_t t = 0) const
  { return bit_cast<T*>(&(m_data[t][0]) + c * m_info.channelByteNumber() + z * m_info.planeByteNumber()); }

  template<typename T = uint8_t>
  inline const T* rowData(size_t y, size_t z = 0, size_t c = 0, size_t t = 0) const
  {
    return bit_cast<T*>(
      &(m_data[t][0]) + c * m_info.channelByteNumber() + z * m_info.planeByteNumber() + y * m_info.rowByteNumber());
  }

  template<typename T = uint8_t>
  inline const T* data(size_t x, size_t y, size_t z = 0, size_t c = 0, size_t t = 0) const
  {
    return bit_cast<T*>(
      &(m_data[t][0]) + c * m_info.channelByteNumber() + z * m_info.planeByteNumber() + y * m_info.rowByteNumber()
      + x * m_info.voxelByteNumber());
  }

  template<typename T = uint8_t>
  inline const T* data(const ZVoxelCoordinate& coord) const
  {
    return bit_cast<T*>(
      &(m_data[coord.t][0]) + coord.c * m_info.channelByteNumber() + coord.z * m_info.planeByteNumber() +
      coord.y * m_info.rowByteNumber() + coord.x * m_info.voxelByteNumber());
  }

  template<typename T = uint8_t>
  inline const T* data(size_t idx) const
  {
    //size_t l = idx / m_info.locationVoxelNumber();
    //idx -= l * m_info.locationVoxelNumber();
    size_t t = idx / m_info.timeVoxelNumber();
    idx -= t * m_info.timeVoxelNumber();
    return bit_cast<T*>(&(m_data[t][0]) + idx * m_info.voxelByteNumber());
  }

  // return out bound voxel value based on boundary condition (padOption)
  // img must be type T otherwise might crash
  template<typename T>
  inline T outBoundValue(const ZVoxelCoordinate& coord,
                         PadOption padOption = PadOption::Constant, T padValue = T(0)) const
  {
    if (padOption == PadOption::Constant) {
      return padValue;
    } else {
      ZVoxelCoordinate coordCopy = coord;
      wrapCoord(coordCopy, padOption);
      return *(data<T>(coordCopy));
    }
  }

  template<typename T>
  inline T outBoundValue(int x, int y, int z, int c = 0, int t = 0,
                         PadOption padOption = PadOption::Constant, T padValue = T(0)) const
  {
    if (padOption == PadOption::Constant) {
      return padValue;
    } else {
      ZVoxelCoordinate coordCopy(x, y, z, c, t);
      wrapCoord(coordCopy, padOption);
      return *(data<T>(coordCopy));
    }
  }

  // output img as values row by row, for debug
  QString toQString() const;

  // img view is a virtual img that doesn't own any img data, usually it is a channel or a time spot or a location from original img
  // operate on img view is same as operate on partial img
  // img view will automatically become to a real img after some operations that need memory reallocation
  // use img view to work with part of img
  ZImg createView(int c = -1, int t = -1);

  const ZImg createView(int c = -1, int t = -1) const;

  // view of one single channel slice
  ZImg createView(size_t z, size_t c, size_t t);

  const ZImg createView(size_t z, size_t c, size_t t) const;

  inline bool isImgView() const
  { return !m_ownData; }

  // statistics
  template<typename TValue>
  void computeMinMax(TValue& min, TValue& max) const;

  // if nbins == 0, default number of bins is used
  // default number of bins for 8bit img is 256, for other type of img is 65536
  // bin size for float img is (dataRangeMax-dataRangeMin)/nbins
  // bin size for integer img is (dataRangeMax-dataRangeMin+1)/nbins
  // if mask is specified, it should be same size as current img, in mask img, zero indicate off, non-zero indicate on
  // only mask on voxels are counted in histogram
  std::vector<size_t> histogram(size_t nbins = 0, const ZImg& mask = ZImg()) const;

  // given an bin index, return data range this bin represent
  // not very accurate for 64-bit integer type
  std::pair<double, double> binRange(size_t binIdx, size_t nbins = 0) const;

  // take range as parameter, TRange will be cast to img data type
  // bin size for float img is (maxData-minData)/nbins
  // bin size for integer img is (maxData-minData+1)/nbins
  template<typename TRange>
  std::vector<size_t> histogram(TRange minData, TRange maxData, size_t nbins = 0, const ZImg& mask = ZImg()) const;

  // overload
  template<typename TRange>
  std::pair<double, double> binRange(size_t binIdx, TRange minData, TRange maxData, size_t nbins = 0) const;

  // property of img type
  // intensity range of current img type, for float img, range is [0.0 1.0]
  // use template return type because img can be any type, and even double type can not represent all 64-bit integer type value
  template<typename TValue = double>
  TValue dataRangeMin() const
  { return m_info.dataRangeMin<TValue>(); }

  template<typename TValue = double>
  TValue dataRangeMax() const
  { return m_info.dataRangeMax<TValue>(); }

  // some utils, these functions will throw ZImgException if input parameters is invalid

  // if region is empty, return empty img
  // throw ZImgException if current img is empty or region is not valid
  ZImg crop(const ZImgRegion& region) const;

  // crop from start coordinate to end coordinate with outside pixel padded.
  // padValue is only used when padOption is PadOption::Constant, padValue will be cast to img voxel type
  // only requirement is that coord end should be large or equal (in this case return empty img) than coord start (valid)
  // throw ZImgException if current img is empty or region goes wrong
  template<typename TPadValue = uint8_t>
  ZImg cropWithPad(const ZVoxelCoordinate& startCoord, const ZVoxelCoordinate& endCoord,
                   PadOption padOption = PadOption::Constant, TPadValue padValue = TPadValue(0)) const;

  // extract part of img
  ZImg extractVoxel(size_t x, size_t y, int z = -1, int c = -1, int t = -1) const;

  ZImg extractCol(size_t x, int z = -1, int c = -1, int t = -1) const;

  ZImg extractRow(size_t y, int z = -1, int c = -1, int t = -1) const;

  ZImg extractPlane(size_t z, int c = -1, int t = -1) const;

  ZImg extractChannel(size_t c, int t = -1) const;

  ZImg extractTime(size_t t) const;

  // value will be cast to img voxel type
  template<typename TFillValue>
  ZImg& fill(TFillValue value);

  // fill with uniform distributed value with range [dataRangeMin, dataRangeMax]
  ZImg& fillRandom();

  // paste another img into this img start from certain location (default zero)
  // type cast might happen if input img has different data type
  // do nothing if input img and current img have no overlap
  ZImg& pasteImg(const ZImg& img, const ZVoxelCoordinate& start = ZVoxelCoordinate());

  // similar to pasteImg, except that pasteImg keep the intensity value of input img in overlap area while
  // this function keep the maximum intensity value of current img and input img
  ZImg& pasteImgMax(const ZImg& img, const ZVoxelCoordinate& start = ZVoxelCoordinate());

  // cat a series of img along certain dimension
  // imgs should be same type and have same dimension size other than the dimension to cat
  // dim should be valid
  // throw ZImgException if can not cat
  static ZImg cat(const std::vector<ZImg>& imgs, Dimension dim);

  static ZImg cat(const std::vector<ZImg*>& imgs, Dimension dim);

  static ZImg cat(const std::vector<const ZImg*>& imgs, Dimension dim);

  static ZImg cat(const ZImg& img1, const ZImg& img2, Dimension dim);

  static ZImg cat(const ZImg& img1, const ZImg& img2, const ZImg& img3, Dimension dim);

  static ZImg cat(const ZImg& img1, const ZImg& img2, const ZImg& img3, const ZImg& img4, Dimension dim);

  // combine image of same type and same dimensions
  static ZImg combine(const std::vector<ZImg>& imgs, CombineMode mode);

  static ZImg combine(const std::vector<ZImg*>& imgs, CombineMode mode);

  static ZImg combine(const std::vector<const ZImg*>& imgs, CombineMode mode);

  static ZImg combine(const ZImg& img1, const ZImg& img2, CombineMode mode);

  static ZImg combine(const ZImg& img1, const ZImg& img2, const ZImg& img3, CombineMode mode);

  static ZImg combine(const ZImg& img1, const ZImg& img2, const ZImg& img3, const ZImg& img4, CombineMode mode);

  // projection
  ZImg projectAlongDim(Dimension dim, CombineMode mode) const;

  ZImg maximumZProjection() const;

  // map [minData maxData] to [dataRangeMin, dataRangeMax]
  // for float img, dataRangeMin is 0.0, dataRangeMax is 1.0
  template<typename TRange>
  ZImg normalized(TRange minData, TRange maxData) const;

  // first compute minimum and maximum value, then use as Range
  ZImg normalized() const;

  // to normalize only channel 1, call img.createView(1,-1,-1).normalize(minD, maxD);
  template<typename TRange>
  ZImg& normalize(TRange minData, TRange maxData);

  // first compute minimum and maximum value, then use as Range
  ZImg& normalize();

  // img type conversion
  // make img of new type by casting current img
  // e.g. img = img.castTo<double>();
  template<typename TDesVoxel>
  ZImg __warn_unused_result castTo() const;

  // overload, cast voxel to format, throw if combination is not supported (like 3-byte float..)
  ZImg __warn_unused_result castTo(VoxelFormat vf, size_t bytesPerVoxel);

  // make img of new type, map current data type range to target img data type range, return new img
  // note: float img data type range is [0.0 1.0]
  template<typename TDesVoxel>
  ZImg __warn_unused_result convertTo() const;

  // convert normalized img to another type
  template<typename TDesVoxel>
  ZImg __warn_unused_result convertNormalizedTo() const;

  // make img of new type, map [minData maxData] to target img data type range, return new img
  template<typename TDesVoxel, typename TRange>
  ZImg __warn_unused_result convertTo(TRange minData, TRange maxData) const;

  // deduce TDesVoxel from img
  template<typename TRange>
  ZImg __warn_unused_result convertTo(TRange minData, TRange maxData, const ZImg& targetImgType) const;

  // resize in x-y-z dimensions
  // 'antialiasing' specifies whether to perform antialiasing when shrinking an image. For the 'nearest' method,
  // the parameter 'antialiasingForNearest' is used (default false); for all other methods, the default is true.
  ZImg resized(size_t desWidth, size_t desHeight, size_t desDepth,
               Interpolant interpolant = Interpolant::Cubic, bool antialiasing = true,
               bool antialiasingForNearest = false) const;

  ZImg zoomed(double scaleX, double scaleY, double scaleZ,
              Interpolant interpolant = Interpolant::Cubic, bool antialiasing = true,
              bool antialiasingForNearest = false) const;

  // combine voxels in each block into one voxel of result img
  // result img size is ceil(width/blockWidth) * ceil(height/blockHeight) * ceil(depth/blockDepth)
  ZImg blockDownsampled(size_t blockWidth, size_t blockHeight, size_t blockDepth, CombineMode mode) const;

  // resize zoom this img, will change img memory and make virtual img non-virtual
  ZImg& resize(size_t desWidth, size_t desHeight, size_t desDepth,
               Interpolant interpolant = Interpolant::Cubic, bool antialiasing = true,
               bool antialiasingForNearest = false);

  ZImg& zoom(double scaleX, double scaleY, double scaleZ = 1.0,
             Interpolant interpolant = Interpolant::Cubic, bool antialiasing = true,
             bool antialiasingForNearest = false);

  ZImg& blockDownsample(size_t blockWidth, size_t blockHeight, size_t blockDepth, CombineMode mode);

  // flip along dim, dim can be normal x-y-z dim(0-1-2), channel dim(3), location dim(5) or time dimension(4)
  ZImg& flip(Dimension dim);

  // reflect img, note: will reflect all dimension includes time, location, channel..
  // use createImgView to reflect only part of img
  ZImg& reflect();

  // returns a same size img that contains the cumulative sum of the voxels along dimension dim
  // note: for integer image perform saturate arithmetic, see below
  ZImg cumulativeSum(Dimension dim) const;

  // calculate the sum of each 3D block defined by the template size. The
  // returned img will have size (width+twidth-1) x (height+theight-1) x (depth+tdepth-1) x c x t x l
  // all locations, times, channels are processed in the same way
  // throw ZImgException if template size is 0
  // note: for integer image perform saturate arithmetic, see below
  ZImg blockSum(size_t twidth, size_t theight, size_t tdepth) const;

  // same as blockSum then crop [startX, endX) * [startY, endY) * [startZ, endZ)
  // throw ZImgException if template size is 0 or range is wrong
  // note: for integer image perform saturate arithmetic, see below
  ZImg blockSumPart(size_t twidth, size_t theight, size_t tdepth, size_t xStart, size_t xEnd,
                    size_t yStart, size_t yEnd, size_t zStart, size_t zEnd) const;

  // threshold
  // if voxel >  (or >= based on ThresholdMode) threshold, voxel = outsidevalue,
  // TValue will be cast to current img data type
  template<typename TValue>
  ZImg& thresholdAbove(TValue threshold, ThresholdMode threMode, TValue outsideValue);

  // if voxel <  (or <= based on ThresholdMode) threshold, voxel = outsidevalue,
  // TValue will be cast to current img data type
  template<typename TValue>
  ZImg& thresholdBelow(TValue threshold, ThresholdMode threMode, TValue outsideValue);

  // binarize
  // return a uint8_t img with 1 and 0
  // if voxel > (or >= based on ThresholdMode) threshold, result mask voxel = 1 else mask voxel = 0
  // TValue threshold will be cast to current img data type
  template<typename TValue>
  ZImg binarized(TValue threshold, ThresholdMode threMode) const;

  // return a uint8_t img with 1 and 0
  // for all img type, if voxel > 0, result mask voxel = 1
  inline ZImg binarized() const
  { return binarized(0, ThresholdMode::ExcludeThreshold); }

  // return a uint8_t img with 1 and 0
  // for all img type, if isForeground(voxel) return true, result mask voxel = 1
  // GenericForegroundPredictor take any numeric type as parameter and return bool
  template<typename GenericForegroundPredictor>
  ZImg binarized(const GenericForegroundPredictor& isForeground) const;

  // if you know the img type
  // ForegroundPredictor take TVoxel as parameter and return bool
  // throw ZImgException if type don't match
  template<typename TVoxel, typename ForegroundPredictor>
  ZImg typedBinarized(const ForegroundPredictor& isForeground) const;

  // for integer type all perform saturate arithmetic
  // for example for a 8bit unsigned img, 128 * 2 = 255;  253 + 6 = 255; 34 - 230 = 0
  // for 8bit signed img, 12 + 120 = 127; -45-120 = -128; 23 * 1.51 = round(34.73) = 35;
  template<typename TScalar>
  ZImg& operator+=(TScalar scalar);

  // add img, input should has same size, otherwise throw ZImgException
  ZImg& operator+=(const ZImg& rhs);

  template<typename TScalarOrZImg>
  ZImg operator+(const TScalarOrZImg& scalarOrZImg) const;

  template<typename TScalar>
  ZImg& operator-=(TScalar scalar);

  // sub img, input should has same size, otherwise throw ZImgException
  ZImg& operator-=(const ZImg& rhs);

  template<typename TScalarOrZImg>
  ZImg operator-(const TScalarOrZImg& scalarOrZImg) const;

  template<typename TScalar>
  ZImg& operator*=(TScalar scalar);

  // multiply img, input should has same size, otherwise throw ZImgException
  ZImg& operator*=(const ZImg& rhs);

  template<typename TScalarOrZImg>
  ZImg operator*(const TScalarOrZImg& scalarOrZImg) const;

  template<typename TScalar>
  // throw ZImgException if scalar is zero and not float
  ZImg& operator/=(TScalar scalar);

  // divide img, input should has same size, otherwise throw ZImgException
  // might got hardware exception if rhs contains zero and both img is not float type
  ZImg& operator/=(const ZImg& rhs);

  template<typename TScalarOrZImg>
  ZImg operator/(const TScalarOrZImg& scalarOrZImg) const;

  // divide img, input should has same size, otherwise throw ZImgException
  // result voxel is 0 if rhs voxel is 0
  ZImg& secureDivideBy(const ZImg& rhs);

  // return true if img are same type same size and has same content
  bool operator==(const ZImg& other) const;

  // perform custom voxel-wise unary operator
  // GenericCustomUnaryOp is a generic lambda or non-template functor that accepts current voxel as argument and return the new voxel value
  // the non-template functor might have a templated operator() to process all possible voxel types
  // a typical prototype of unary function looks like:
  //
  // struct someGenericOp {
  //   template<typename TVoxel>
  //   TVoxel operator()(TVoxel current) const {}
  // };
  // and it can be used like:
  // img.unaryOperation(someGenericOp());
  // note: this will generate about 10 switch case branches in function because we need to determine current img type
  template<typename GenericCustomUnaryOp>
  ZImg& unaryOperation(const GenericCustomUnaryOp& op);

  // if you already know the img type (e.g. double) and have a function for that type like:
  // double someOpForDoubleVoxel(double current);
  // you can use the typed version which generate less code:
  // img.typedUnaryOperation<double>(someOpForDoubleVoxel);
  // op should be a unary function that accepts current voxel as argument and return the new voxel value
  // op can be either a function pointer or an instantiated function object (can have internal state)
  // **note** throw ZImgException if type don't match
  template<typename TVoxel, typename CustomUnaryOp>
  ZImg& typedUnaryOperation(const CustomUnaryOp& op);

  // perform a custom voxel-wise operator <op> of *this and other
  // GenericBinaryFunctor is a generic lambda or non-template functor that accepts current voxel from *this as first argument
  // and another voxel from same position of other as second argument and return the new value of current voxel
  // the non-template functor might have a templated operator() to process all possible voxel types
  // the functor should not modify the second parameter (if take by reference)
  // a typical prototype of binary function looks like:
  //
  // struct someOp {
  //   template<typename TVoxel, typename TVoxelOther>
  //   TVoxel operator()(TVoxel voxelRef, TVoxelOther otherVoxel) const {}
  // };
  // and it can be used like:
  // img.binaryOperation(someOp());
  // make sure input img has same size as current img, otherwise ZImgException will be thrown
  // note: this will generate about 100 switch case branches in function because we need to determine two img type
  template<typename GenericCustomBinaryOp>
  ZImg& binaryOperation(const ZImg& other, const GenericCustomBinaryOp& op);

  // similar to unaryTypedOp, if you already know the type of current img and other img and have function like
  // double someBinaryOp(double current, int8_t otherVoxel);
  // you can use the typed version which generate less code:
  // img.binaryTypedOp<double, int8_t>(other, someBinaryOp);
  // **note** throw ZImgException if type don't match
  template<typename TVoxel, typename TVoxelOther, typename CustomBinaryOp>
  ZImg& typedBinaryOperation(const ZImg& other, const CustomBinaryOp& op);

  // img coordinate
  // convert idx to coord
  // **note** if info is empty, throw ZImgException
  // for out of bound idx, this return a coord with only last dimension (Dimension::L) invalid, which can be seen
  // as virtual extension of img
  static ZVoxelCoordinate indexToCoord(int64_t idx, const ZImgInfo& info);

  // result is undefined for invalid coord
  // if only last dimension of coord is invalid, result can still be meaningful
  static int64_t coordToIndex(const ZVoxelCoordinate& coord, const ZImgInfo& info);

  inline ZVoxelCoordinate indexToCoord(int64_t idx) const
  { return indexToCoord(idx, m_info); }

  inline int64_t coordToIndex(const ZVoxelCoordinate& coord) const
  { return coordToIndex(coord, m_info); }

  // coord of one voxel pass each dimension
  inline ZVoxelCoordinate endCoord() const
  {
    return ZVoxelCoordinate(m_info.width, m_info.height, m_info.depth, m_info.numChannels, m_info.numTimes);
  }

  // max valid coord, **note** throw ZImgException for empty img
  inline ZVoxelCoordinate maxCoord() const
  {
    if (isEmpty()) throw ZImgException("No max coord for empty img");
    return ZVoxelCoordinate(m_info.width - 1, m_info.height - 1, m_info.depth - 1, m_info.numChannels - 1,
                            m_info.numTimes - 1);
  }

  // coord will always be invalid if img is empty
  inline bool isCoordValid(const ZVoxelCoordinate& coord) const
  {
    return !isEmpty() && coord.allGreaterEqual(0) && coord.allLessThan(endCoord());
  }

  // coord of first voxel with max img value
  template<typename TValue>
  ZVoxelCoordinate firstMaxValueCoord(TValue& max, const ZImgRegion& region = ZImgRegion()) const;

  // coord of all voxels with max img value
  template<typename TValue>
  std::vector<ZVoxelCoordinate> maxValueCoords(TValue& max, const ZImgRegion& region = ZImgRegion()) const;

  // get value as type at coordinate
  // **throw ZImgException** if coord is invalid or img is empty
  // note: this might be slow because we need to find the correct data type and check if coordinate is valid
  // if you know the data type and don't need to check coord, use data<> function
  template<typename TValue = double>
  TValue value(const ZVoxelCoordinate& coord) const;

  // overload
  template<typename TValue = double>
  TValue value(size_t x, size_t y, size_t z, size_t c = 0, size_t t = 0) const;

  // overload
  template<typename TValue = double>
  TValue value(size_t idx) const;

  // **no throw** version of get value as type at coordinate, coord is always valid because we will pad img
  // for empty img, return 0
  // padValue is only used when padOption is PadOption::Constant, padValue will be cast to img voxel type
  template<typename TValue = double>
  TValue valueWithPad(const ZVoxelCoordinate& coord, PadOption padOption = PadOption::Constant,
                      TValue padValue = TValue(0)) const;

  // overload
  template<typename TValue = double>
  inline TValue valueWithPad(int x, int y, int z, int c = 0, int t = 0,
                             PadOption padOption = PadOption::Constant, TValue padValue = TValue(0)) const
  {
    return valueWithPad(ZVoxelCoordinate(x, y, z, c, t), padOption, padValue);
  }

  // set value of coord to value, value will be cast to current img type before set
  // **throw ZImgException** if coord is invalid or img is empty
  template<typename TValue>
  void setValue(TValue value, const ZVoxelCoordinate& coord);

  // overload
  template<typename TValue>
  void setValue(TValue value, size_t x, size_t y, size_t z, size_t c = 0, size_t t = 0);

  // overload
  template<typename TValue>
  void setValue(TValue value, size_t idx);

  // similar to setValue, instead of throw ZImgException in case of error, this function do nothing and return false
  // return true if succeed
  template<typename TValue>
  bool setValueNoThrow(TValue value, const ZVoxelCoordinate& coord);

  // overload
  template<typename TValue>
  bool setValueNoThrow(TValue value, size_t x, size_t y, size_t z, size_t c = 0, size_t t = 0);

  // overload
  template<typename TValue>
  bool setValueNoThrow(TValue value, size_t idx);

  // from alpha pre-multiplied color to normal color, assume last channel is alpha channel
  ZImg& correctPreMultipliedColor();

#ifdef _NEUTUBE_
  // only for interface with zstack
  void releaseTimeData(size_t t) { m_data[t] = nullptr; }
#endif

protected:
  void clearData();

  // wrap a pixel coordinate to a valid pixel coordinate inside current image using padOption
  // if padOption is constant, do nothing
  void wrapCoord(ZVoxelCoordinate& coord, PadOption padOption) const;

  // conn might be changed if 3d conn is used for 2d img
  void checkConnInput(size_t& conn) const;

private:
  template<typename TVoxel>
  void cropWithPad_Impl(ZImg& res, const ZVoxelCoordinate& startCoord, const ZVoxelCoordinate& endCoord,
                        PadOption padOption, TVoxel padValue) const;

  template<typename TVoxel>
  void fill_Impl(TVoxel value);

  template<typename TVoxel>
  void fillRandom_Impl();

  template<typename TVoxel, typename TVoxelImg>
  void pasteImg_Impl(const ZImg& img, const ZVoxelCoordinate& start);

  template<typename TVoxel, typename TVoxelImg>
  void pasteImgMax_Impl(const ZImg& img, const ZVoxelCoordinate& start);

  template<typename TVoxel>
  static ZImg combine_Impl(const std::vector<const ZImg*>& imgs, CombineMode mode);

  template<typename TVoxel, typename TDesVoxel>
  static void convert_Impl(bool normalize, const ZImg* src, ZImg* des);

  template<typename TVoxel, typename TDesVoxel>
  static void scale_Impl(TVoxel minData, TVoxel maxData, const ZImg* src, ZImg* des);

  template<typename TVoxel, typename TDesVoxel>
  static void buildScaleColormap(TVoxel minData, TVoxel maxData, TDesVoxel desDataRangeMin, TDesVoxel desDataRangeMax,
                                 std::vector<TDesVoxel>& res);

  template<typename TVoxel>
  ZImg& normalize_Impl();

  template<typename TVoxel, typename TDesVoxel>
  void cast_Impl(ZImg& res) const;

  template<typename TVoxel>
  void resize_Impl(ZImg& res, Interpolant interpolant, bool antialiasing, bool antialiasingForNearest) const;

  template<typename TVoxel>
  void
  blockDownsampled_Impl(ZImg& res, size_t blockWidth, size_t blockHeight, size_t blockDepth, CombineMode mode) const;

  template<typename TVoxel, typename TValue>
  void computeMinMax_Impl(TValue& min, TValue& max) const;

  template<typename TVoxel, typename TScalar>
  void addScalar_Impl(TScalar scalar);

  template<typename TVoxel, typename TScalar>
  void subScalar_Impl(TScalar scalar);

  template<typename TVoxel, typename TScalar>
  void mulScalar_Impl(TScalar scalar);

  template<typename TVoxel, typename TScalar>
  void divScalar_Impl(TScalar scalar);

  template<typename TVoxel, typename TVoxelRhs>
  void addImg_Impl(const ZImg& rhs);

  template<typename TVoxel, typename TVoxelRhs>
  void subImg_Impl(const ZImg& rhs);

  template<typename TVoxel, typename TVoxelRhs>
  void mulImg_Impl(const ZImg& rhs);

  template<typename TVoxel, typename TVoxelRhs>
  void divImg_Impl(const ZImg& rhs);

  template<typename TVoxel, typename TVoxelRhs>
  void secureDivImg_Impl(const ZImg& rhs);

  template<typename TVoxel,
    typename std::enable_if_t<std::is_integral<std::remove_reference_t<TVoxel>>::value, int> = 0>
  void histogram_Impl(std::vector<size_t>& res, TVoxel minData, TVoxel maxData) const;

  template<typename TVoxel,
    typename std::enable_if_t<std::is_floating_point<std::remove_reference_t<TVoxel>>::value, int> = 0>
  void histogram_Impl(std::vector<size_t>& res, TVoxel minData, TVoxel maxData) const;

  template<typename TVoxel>
  inline void histogram_Impl(std::vector<size_t>& res) const
  { histogram_Impl(res, dataRangeMin<TVoxel>(), dataRangeMax<TVoxel>()); }

  template<typename TVoxel, typename TMaskVoxel,
    typename std::enable_if_t<std::is_integral<std::remove_reference_t<TVoxel>>::value, int> = 0>
  void histogramMask_Impl(std::vector<size_t>& res, TVoxel minData, TVoxel maxData, const ZImg& mask) const;

  template<typename TVoxel, typename TMaskVoxel,
    typename std::enable_if_t<std::is_floating_point<std::remove_reference_t<TVoxel>>::value, int> = 0>
  void histogramMask_Impl(std::vector<size_t>& res, TVoxel minData, TVoxel maxData, const ZImg& mask) const;

  template<typename TVoxel, typename TMaskVoxel>
  inline void histogramMask_Impl(std::vector<size_t>& res, const ZImg& mask) const
  { histogramMask_Impl<TVoxel, TMaskVoxel>(res, dataRangeMin<TVoxel>(), dataRangeMax<TVoxel>(), mask); }

  template<typename TVoxel, typename GenericCustomUnaryOp>
  void unaryOp_Impl(const GenericCustomUnaryOp& op);

  template<typename TVoxel, typename TVoxelOther, typename GenericCustomBinaryOp>
  void binaryOp_Impl(const ZImg& other, const GenericCustomBinaryOp& op);

  template<typename TVoxel>
  void flip_Impl(Dimension dim);

  template<typename TVoxel>
  void reflect_Impl();

  template<typename TVoxel>
  void cumulativeSum_Impl(ZImg& res, Dimension dim) const;

  template<typename TVoxel>
  void blockSum_Impl(ZImg& res, size_t twidth, size_t theight, size_t tdepth) const;

  template<typename TVoxel>
  void blockSumPart_Impl(ZImg& res, size_t twidth, size_t theight, size_t tdepth, size_t xStart,
                         size_t yStart, size_t zStart) const;

  template<typename TVoxel, typename TValue>
  void firstMaxValueCoord_Impl(ZVoxelCoordinate& res, TValue& max, const ZImgRegion& region) const;

  // coord of all voxels with max img value
  template<typename TVoxel, typename TValue>
  void maxValueCoords_Impl(std::vector<ZVoxelCoordinate>& res, TValue& max, const ZImgRegion& region) const;

  template<typename TVoxel>
  inline TVoxel value_Impl(const ZVoxelCoordinate& coord) const
  { return *(data<TVoxel>(coord)); }

  template<typename TVoxel>
  inline TVoxel value_Impl(size_t x, size_t y, size_t z, size_t c, size_t t) const
  { return *(data<TVoxel>(x, y, z, c, t)); }

  template<typename TVoxel>
  inline TVoxel value_Impl(size_t idx) const
  { return *(data<TVoxel>(idx)); }

  template<typename TVoxel>
  inline TVoxel valueWithPad_Impl(const ZVoxelCoordinate& coord, PadOption padOption, TVoxel padValue) const
  {
    return (coord.allGreaterEqual(0) && coord.allLessThan(endCoord())) ?
           (*(data<TVoxel>(coord))) : outBoundValue<TVoxel>(coord, padOption, padValue);
  }

  template<typename TVoxel>
  inline void setValue_Impl(TVoxel value, const ZVoxelCoordinate& coord)
  { *(data<TVoxel>(coord)) = value; }

  template<typename TVoxel>
  inline void setValue_Impl(TVoxel value, size_t x, size_t y, size_t z, size_t c, size_t t)
  { *(data<TVoxel>(x, y, z, c, t)) = value; }

  template<typename TVoxel>
  inline void setValue_Impl(TVoxel value, size_t idx)
  { *(data<TVoxel>(idx)) = value; }

  template<typename TVoxel>
  void thresholdAbove_Impl(TVoxel threshold, ThresholdMode threMode, TVoxel outsideValue);

  template<typename TVoxel>
  void thresholdBelow_Impl(TVoxel threshold, ThresholdMode threMode, TVoxel outsideValue);

  template<typename TVoxel>
  void binarized_Impl(ZImg& res, TVoxel threshold, ThresholdMode threMode) const;

  template<typename TVoxel, typename GenericForegroundPredictor>
  void binarized_Impl(ZImg& res, const GenericForegroundPredictor& isForeground) const;

  template<typename TVoxel>
  void showContentAsQString_Impl(QString& res) const;

private:
  std::vector<uint8_t*> m_data;
  ZImgThumbernail m_thumbnail;
  ZImgInfo m_info;
  ZImgMetadata m_metadata;
  bool m_ownData;
};

template<typename TPixel>
void image2DWrite(const TPixel* data, int width, int height,
                  const QString& filename)
{
  ZImg img;
  img.wrapData(const_cast<TPixel*>(data), width, height, 1);
  img.save(filename);
}

template<typename TPixel>
void image3DWrite(const TPixel* data, size_t width, size_t height, size_t depth,
                  const QString& filename)
{
  ZImg img;
  img.wrapData(const_cast<TPixel*>(data), width, height, depth);
  img.save(filename);
}

// Macro to call the correct version of typed function
// To work with img of different type,
// write a function:
//   template<typename TVoxel>
//   void function(t1 arg1, t2 arg2) {
//     ...
//   }
//
// and use this macro to dispatch:
//   IMG_TYPED_CALL(function, img, arg1, arg2);
//
// if the function return something, use
//   IMG_RETURN_TYPED_CALL(function, img, arg1, arg2);
//
//
#define IMG_TYPED_CALL(function, img, ...) {         \
  if (img.voxelFormat() == VoxelFormat::Unsigned) {            \
    switch (img.bytesPerVoxel()) {                   \
    case 1:                                          \
      function<uint8_t>(__VA_ARGS__);                \
      break;                                         \
    case 2:                                          \
      function<uint16_t>(__VA_ARGS__);               \
      break;                                         \
    case 4:                                          \
      function<uint32_t>(__VA_ARGS__);               \
      break;                                         \
    case 8:                                          \
      function<uint64_t>(__VA_ARGS__);               \
      break;                                         \
    default:                                         \
      break;                                         \
    }                                                \
  } else if (img.voxelFormat() == VoxelFormat::Float) {        \
    switch (img.bytesPerVoxel()) {                   \
    case 4:                                          \
      function<float>(__VA_ARGS__);                  \
      break;                                         \
    case 8:                                          \
      function<double>(__VA_ARGS__);                 \
      break;                                         \
    default:                                         \
      break;                                         \
    }                                                \
  } else if (img.voxelFormat() == VoxelFormat::Signed) {       \
    switch (img.bytesPerVoxel()) {                   \
    case 1:                                          \
      function<int8_t>(__VA_ARGS__);                 \
      break;                                         \
    case 2:                                          \
      function<int16_t>(__VA_ARGS__);                \
      break;                                         \
    case 4:                                          \
      function<int32_t>(__VA_ARGS__);                \
      break;                                         \
    case 8:                                          \
      function<int64_t>(__VA_ARGS__);                \
      break;                                         \
    default:                                         \
      break;                                         \
    }                                                \
  }                                                  \
}

#define IMG_RETURN_TYPED_CALL(function, img, ...) {  \
  if (img.voxelFormat() == VoxelFormat::Unsigned) {            \
    switch (img.bytesPerVoxel()) {                   \
    case 1:                                          \
      return function<uint8_t>(__VA_ARGS__);         \
      break;                                         \
    case 2:                                          \
      return function<uint16_t>(__VA_ARGS__);        \
      break;                                         \
    case 4:                                          \
      return function<uint32_t>(__VA_ARGS__);        \
      break;                                         \
    case 8:                                          \
      return function<uint64_t>(__VA_ARGS__);        \
      break;                                         \
    default:                                         \
      break;                                         \
    }                                                \
  } else if (img.voxelFormat() == VoxelFormat::Float) {        \
    switch (img.bytesPerVoxel()) {                   \
    case 4:                                          \
      return function<float>(__VA_ARGS__);           \
      break;                                         \
    case 8:                                          \
      return function<double>(__VA_ARGS__);          \
      break;                                         \
    default:                                         \
      break;                                         \
    }                                                \
  } else if (img.voxelFormat() == VoxelFormat::Signed) {       \
    switch (img.bytesPerVoxel()) {                   \
    case 1:                                          \
      return function<int8_t>(__VA_ARGS__);          \
      break;                                         \
    case 2:                                          \
      return function<int16_t>(__VA_ARGS__);         \
      break;                                         \
    case 4:                                          \
      return function<int32_t>(__VA_ARGS__);         \
      break;                                         \
    case 8:                                          \
      return function<int64_t>(__VA_ARGS__);         \
      break;                                         \
    default:                                         \
      break;                                         \
    }                                                \
  }                                                  \
}

// for function that take 2 template argument
// first one is derived from img, second one is provided by user
#define IMG_TYPED_CALL_FIX2NDTYPE(function, img, T2ND, ...) {         \
  if (img.voxelFormat() == VoxelFormat::Unsigned) {                             \
    switch (img.bytesPerVoxel()) {                                    \
    case 1:                                                           \
      function<uint8_t, T2ND>(__VA_ARGS__);                           \
      break;                                                          \
    case 2:                                                           \
      function<uint16_t, T2ND>(__VA_ARGS__);                          \
      break;                                                          \
    case 4:                                                           \
      function<uint32_t, T2ND>(__VA_ARGS__);                          \
      break;                                                          \
    case 8:                                                           \
      function<uint64_t, T2ND>(__VA_ARGS__);                          \
      break;                                                          \
    default:                                                          \
      break;                                                          \
    }                                                                 \
  } else if (img.voxelFormat() == VoxelFormat::Float) {                         \
    switch (img.bytesPerVoxel()) {                                    \
    case 4:                                                           \
      function<float, T2ND>(__VA_ARGS__);                             \
      break;                                                          \
    case 8:                                                           \
      function<double, T2ND>(__VA_ARGS__);                            \
      break;                                                          \
    default:                                                          \
      break;                                                          \
    }                                                                 \
  } else if (img.voxelFormat() == VoxelFormat::Signed) {                        \
    switch (img.bytesPerVoxel()) {                                    \
    case 1:                                                           \
      function<int8_t, T2ND>(__VA_ARGS__);                            \
      break;                                                          \
    case 2:                                                           \
      function<int16_t, T2ND>(__VA_ARGS__);                           \
      break;                                                          \
    case 4:                                                           \
      function<int32_t, T2ND>(__VA_ARGS__);                           \
      break;                                                          \
    case 8:                                                           \
      function<int64_t, T2ND>(__VA_ARGS__);                           \
      break;                                                          \
    default:                                                          \
      break;                                                          \
    }                                                                 \
  }                                                                   \
}

// for function that take 3 template argument
// first one is derived from img, second and third is provided by user
#define IMG_TYPED_CALL_FIX2ND3RDTYPE(function, img, T2ND, T3RD, ...) {   \
  if (img.voxelFormat() == VoxelFormat::Unsigned) {                                \
    switch (img.bytesPerVoxel()) {                                       \
    case 1:                                                              \
      function<uint8_t, T2ND, T3RD>(__VA_ARGS__);                        \
      break;                                                             \
    case 2:                                                              \
      function<uint16_t, T2ND, T3RD>(__VA_ARGS__);                       \
      break;                                                             \
    case 4:                                                              \
      function<uint32_t, T2ND, T3RD>(__VA_ARGS__);                       \
      break;                                                             \
    case 8:                                                              \
      function<uint64_t, T2ND, T3RD>(__VA_ARGS__);                       \
      break;                                                             \
    default:                                                             \
      break;                                                             \
    }                                                                    \
  } else if (img.voxelFormat() == VoxelFormat::Float) {                            \
    switch (img.bytesPerVoxel()) {                                       \
    case 4:                                                              \
      function<float, T2ND, T3RD>(__VA_ARGS__);                          \
      break;                                                             \
    case 8:                                                              \
      function<double, T2ND, T3RD>(__VA_ARGS__);                         \
      break;                                                             \
    default:                                                             \
      break;                                                             \
    }                                                                    \
  } else if (img.voxelFormat() == VoxelFormat::Signed) {                           \
    switch (img.bytesPerVoxel()) {                                       \
    case 1:                                                              \
      function<int8_t, T2ND, T3RD>(__VA_ARGS__);                         \
      break;                                                             \
    case 2:                                                              \
      function<int16_t, T2ND, T3RD>(__VA_ARGS__);                        \
      break;                                                             \
    case 4:                                                              \
      function<int32_t, T2ND, T3RD>(__VA_ARGS__);                        \
      break;                                                             \
    case 8:                                                              \
      function<int64_t, T2ND, T3RD>(__VA_ARGS__);                        \
      break;                                                             \
    default:                                                             \
      break;                                                             \
    }                                                                    \
  }                                                                      \
}

#define IMG_RETURN_TYPED_CALL_FIX2NDTYPE(function, img, T2ND, ...) {         \
  if (img.voxelFormat() == VoxelFormat::Unsigned) {                                    \
    switch (img.bytesPerVoxel()) {                                           \
    case 1:                                                                  \
      return function<uint8_t, T2ND>(__VA_ARGS__);                           \
      break;                                                                 \
    case 2:                                                                  \
      return function<uint16_t, T2ND>(__VA_ARGS__);                          \
      break;                                                                 \
    case 4:                                                                  \
      return function<uint32_t, T2ND>(__VA_ARGS__);                          \
      break;                                                                 \
    case 8:                                                                  \
      return function<uint64_t, T2ND>(__VA_ARGS__);                          \
      break;                                                                 \
    default:                                                                 \
      break;                                                                 \
    }                                                                        \
  } else if (img.voxelFormat() == VoxelFormat::Float) {                                \
    switch (img.bytesPerVoxel()) {                                           \
    case 4:                                                                  \
      return function<float, T2ND>(__VA_ARGS__);                             \
      break;                                                                 \
    case 8:                                                                  \
      return function<double, T2ND>(__VA_ARGS__);                            \
      break;                                                                 \
    default:                                                                 \
      break;                                                                 \
    }                                                                        \
  } else if (img.voxelFormat() == VoxelFormat::Signed) {                               \
    switch (img.bytesPerVoxel()) {                                           \
    case 1:                                                                  \
      return function<int8_t, T2ND>(__VA_ARGS__);                            \
      break;                                                                 \
    case 2:                                                                  \
      return function<int16_t, T2ND>(__VA_ARGS__);                           \
      break;                                                                 \
    case 4:                                                                  \
      return function<int32_t, T2ND>(__VA_ARGS__);                           \
      break;                                                                 \
    case 8:                                                                  \
      return function<int64_t, T2ND>(__VA_ARGS__);                           \
      break;                                                                 \
    default:                                                                 \
      break;                                                                 \
    }                                                                        \
  }                                                                          \
}

// for function that process 2 types of img
// first one is derived from img1, second one is derived from img2
#define IMG_TYPED_CALL_2TYPE(function, img1, img2, ...) {                    \
  if (img2.voxelFormat() == VoxelFormat::Unsigned) {                                   \
    switch (img2.bytesPerVoxel()) {                                          \
    case 1:                                                                  \
      IMG_TYPED_CALL_FIX2NDTYPE(function, img1, uint8_t, __VA_ARGS__)        \
      break;                                                                 \
    case 2:                                                                  \
      IMG_TYPED_CALL_FIX2NDTYPE(function, img1, uint16_t, __VA_ARGS__)       \
      break;                                                                 \
    case 4:                                                                  \
      IMG_TYPED_CALL_FIX2NDTYPE(function, img1, uint32_t, __VA_ARGS__)       \
      break;                                                                 \
    case 8:                                                                  \
      IMG_TYPED_CALL_FIX2NDTYPE(function, img1, uint64_t, __VA_ARGS__)       \
      break;                                                                 \
    default:                                                                 \
      break;                                                                 \
    }                                                                        \
  } else if (img2.voxelFormat() == VoxelFormat::Float) {                               \
    switch (img2.bytesPerVoxel()) {                                          \
    case 4:                                                                  \
      IMG_TYPED_CALL_FIX2NDTYPE(function, img1, float, __VA_ARGS__)          \
      break;                                                                 \
    case 8:                                                                  \
      IMG_TYPED_CALL_FIX2NDTYPE(function, img1, double, __VA_ARGS__)         \
      break;                                                                 \
    default:                                                                 \
      break;                                                                 \
    }                                                                        \
  } else if (img2.voxelFormat() == VoxelFormat::Signed) {                              \
    switch (img2.bytesPerVoxel()) {                                          \
    case 1:                                                                  \
      IMG_TYPED_CALL_FIX2NDTYPE(function, img1, int8_t, __VA_ARGS__)         \
      break;                                                                 \
    case 2:                                                                  \
      IMG_TYPED_CALL_FIX2NDTYPE(function, img1, int16_t, __VA_ARGS__)        \
      break;                                                                 \
    case 4:                                                                  \
      IMG_TYPED_CALL_FIX2NDTYPE(function, img1, int32_t, __VA_ARGS__)        \
      break;                                                                 \
    case 8:                                                                  \
      IMG_TYPED_CALL_FIX2NDTYPE(function, img1, int64_t, __VA_ARGS__)        \
      break;                                                                 \
    default:                                                                 \
      break;                                                                 \
    }                                                                        \
  }                                                                          \
}


}  // namespace

#include "zimg.inl"
