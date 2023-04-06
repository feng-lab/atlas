#pragma once

#include "zimginfo.h"
#include "zimgregion.h"
#include "zimgmetadatabase.h"
#include "zimgmetatag.h"
#include "zjson.h"
#include "zsaturateoperation.h"
#include "zlog.h"
#include <QStringList>

class QImage;

namespace nim {

class ZImgSliceProvider;

class ZImgBlockProvider;

// Macro to call the correct version of typed function
// To work with img of different type,
// write a function:
//   template<typename TVoxel>
//   void function(t1 arg1, t2 arg2) {
//     ...
//   }
//
// and use this macro to dispatch:
//   IMG_TYPED_CALL(function, imgInfo, arg1, arg2);
//
// if the function return something, use
//   IMG_RETURN_TYPED_CALL(function, imgInfo, arg1, arg2);
//
//
#define IMG_TYPED_CALL(function, imgInfo, ...)               \
  {                                                          \
    if (imgInfo.voxelFormat == VoxelFormat::Unsigned) {      \
      switch (imgInfo.bytesPerVoxel) {                       \
        case 1:                                              \
          function<uint8_t>(__VA_ARGS__);                    \
          break;                                             \
        case 2:                                              \
          function<uint16_t>(__VA_ARGS__);                   \
          break;                                             \
        case 4:                                              \
          function<uint32_t>(__VA_ARGS__);                   \
          break;                                             \
        case 8:                                              \
          function<uint64_t>(__VA_ARGS__);                   \
          break;                                             \
        default:                                             \
          break;                                             \
      }                                                      \
    } else if (imgInfo.voxelFormat == VoxelFormat::Float) {  \
      switch (imgInfo.bytesPerVoxel) {                       \
        case 4:                                              \
          function<float>(__VA_ARGS__);                      \
          break;                                             \
        case 8:                                              \
          function<double>(__VA_ARGS__);                     \
          break;                                             \
        default:                                             \
          break;                                             \
      }                                                      \
    } else if (imgInfo.voxelFormat == VoxelFormat::Signed) { \
      switch (imgInfo.bytesPerVoxel) {                       \
        case 1:                                              \
          function<int8_t>(__VA_ARGS__);                     \
          break;                                             \
        case 2:                                              \
          function<int16_t>(__VA_ARGS__);                    \
          break;                                             \
        case 4:                                              \
          function<int32_t>(__VA_ARGS__);                    \
          break;                                             \
        case 8:                                              \
          function<int64_t>(__VA_ARGS__);                    \
          break;                                             \
        default:                                             \
          break;                                             \
      }                                                      \
    }                                                        \
  }

#define IMG_RETURN_TYPED_CALL(function, imgInfo, ...)                                     \
  {                                                                                       \
    if (imgInfo.voxelFormat == VoxelFormat::Unsigned) {                                   \
      switch (imgInfo.bytesPerVoxel) {                                                    \
        case 1:                                                                           \
          return function<uint8_t>(__VA_ARGS__);                                          \
          break;                                                                          \
        case 2:                                                                           \
          return function<uint16_t>(__VA_ARGS__);                                         \
          break;                                                                          \
        case 4:                                                                           \
          return function<uint32_t>(__VA_ARGS__);                                         \
          break;                                                                          \
        case 8:                                                                           \
          return function<uint64_t>(__VA_ARGS__);                                         \
          break;                                                                          \
        default:                                                                          \
          throw ZException(fmt::format("unsupported image type {}", imgInfo.toString())); \
      }                                                                                   \
    } else if (imgInfo.voxelFormat == VoxelFormat::Float) {                               \
      switch (imgInfo.bytesPerVoxel) {                                                    \
        case 4:                                                                           \
          return function<float>(__VA_ARGS__);                                            \
          break;                                                                          \
        case 8:                                                                           \
          return function<double>(__VA_ARGS__);                                           \
          break;                                                                          \
        default:                                                                          \
          throw ZException(fmt::format("unsupported image type {}", imgInfo.toString())); \
      }                                                                                   \
    } else if (imgInfo.voxelFormat == VoxelFormat::Signed) {                              \
      switch (imgInfo.bytesPerVoxel) {                                                    \
        case 1:                                                                           \
          return function<int8_t>(__VA_ARGS__);                                           \
          break;                                                                          \
        case 2:                                                                           \
          return function<int16_t>(__VA_ARGS__);                                          \
          break;                                                                          \
        case 4:                                                                           \
          return function<int32_t>(__VA_ARGS__);                                          \
          break;                                                                          \
        case 8:                                                                           \
          return function<int64_t>(__VA_ARGS__);                                          \
          break;                                                                          \
        default:                                                                          \
          throw ZException(fmt::format("unsupported image type {}", imgInfo.toString())); \
      }                                                                                   \
    } else {                                                                              \
      throw ZException(fmt::format("unsupported image type {}", imgInfo.toString()));     \
    }                                                                                     \
  }

// for function that take 2 template argument
// first one is derived from img, second one is provided by user
#define IMG_TYPED_CALL_FIX2NDTYPE(function, imgInfo, T2, ...) \
  {                                                           \
    if (imgInfo.voxelFormat == VoxelFormat::Unsigned) {       \
      switch (imgInfo.bytesPerVoxel) {                        \
        case 1:                                               \
          function<uint8_t, T2>(__VA_ARGS__);                 \
          break;                                              \
        case 2:                                               \
          function<uint16_t, T2>(__VA_ARGS__);                \
          break;                                              \
        case 4:                                               \
          function<uint32_t, T2>(__VA_ARGS__);                \
          break;                                              \
        case 8:                                               \
          function<uint64_t, T2>(__VA_ARGS__);                \
          break;                                              \
        default:                                              \
          break;                                              \
      }                                                       \
    } else if (imgInfo.voxelFormat == VoxelFormat::Float) {   \
      switch (imgInfo.bytesPerVoxel) {                        \
        case 4:                                               \
          function<float, T2>(__VA_ARGS__);                   \
          break;                                              \
        case 8:                                               \
          function<double, T2>(__VA_ARGS__);                  \
          break;                                              \
        default:                                              \
          break;                                              \
      }                                                       \
    } else if (imgInfo.voxelFormat == VoxelFormat::Signed) {  \
      switch (imgInfo.bytesPerVoxel) {                        \
        case 1:                                               \
          function<int8_t, T2>(__VA_ARGS__);                  \
          break;                                              \
        case 2:                                               \
          function<int16_t, T2>(__VA_ARGS__);                 \
          break;                                              \
        case 4:                                               \
          function<int32_t, T2>(__VA_ARGS__);                 \
          break;                                              \
        case 8:                                               \
          function<int64_t, T2>(__VA_ARGS__);                 \
          break;                                              \
        default:                                              \
          break;                                              \
      }                                                       \
    }                                                         \
  }

// for function that take 3 template argument
// first one is derived from img, second and third is provided by user
#define IMG_TYPED_CALL_FIX2ND3RDTYPE(function, imgInfo, T2, T3, ...) \
  {                                                                  \
    if (imgInfo.voxelFormat == VoxelFormat::Unsigned) {              \
      switch (imgInfo.bytesPerVoxel) {                               \
        case 1:                                                      \
          function<uint8_t, T2, T3>(__VA_ARGS__);                    \
          break;                                                     \
        case 2:                                                      \
          function<uint16_t, T2, T3>(__VA_ARGS__);                   \
          break;                                                     \
        case 4:                                                      \
          function<uint32_t, T2, T3>(__VA_ARGS__);                   \
          break;                                                     \
        case 8:                                                      \
          function<uint64_t, T2, T3>(__VA_ARGS__);                   \
          break;                                                     \
        default:                                                     \
          break;                                                     \
      }                                                              \
    } else if (imgInfo.voxelFormat == VoxelFormat::Float) {          \
      switch (imgInfo.bytesPerVoxel) {                               \
        case 4:                                                      \
          function<float, T2, T3>(__VA_ARGS__);                      \
          break;                                                     \
        case 8:                                                      \
          function<double, T2, T3>(__VA_ARGS__);                     \
          break;                                                     \
        default:                                                     \
          break;                                                     \
      }                                                              \
    } else if (imgInfo.voxelFormat == VoxelFormat::Signed) {         \
      switch (imgInfo.bytesPerVoxel) {                               \
        case 1:                                                      \
          function<int8_t, T2, T3>(__VA_ARGS__);                     \
          break;                                                     \
        case 2:                                                      \
          function<int16_t, T2, T3>(__VA_ARGS__);                    \
          break;                                                     \
        case 4:                                                      \
          function<int32_t, T2, T3>(__VA_ARGS__);                    \
          break;                                                     \
        case 8:                                                      \
          function<int64_t, T2, T3>(__VA_ARGS__);                    \
          break;                                                     \
        default:                                                     \
          break;                                                     \
      }                                                              \
    }                                                                \
  }

#define IMG_RETURN_TYPED_CALL_FIX2NDTYPE(function, imgInfo, T2, ...) \
  {                                                                  \
    if (imgInfo.voxelFormat == VoxelFormat::Unsigned) {              \
      switch (imgInfo.bytesPerVoxel) {                               \
        case 1:                                                      \
          return function<uint8_t, T2>(__VA_ARGS__);                 \
          break;                                                     \
        case 2:                                                      \
          return function<uint16_t, T2>(__VA_ARGS__);                \
          break;                                                     \
        case 4:                                                      \
          return function<uint32_t, T2>(__VA_ARGS__);                \
          break;                                                     \
        case 8:                                                      \
          return function<uint64_t, T2>(__VA_ARGS__);                \
          break;                                                     \
        default:                                                     \
          break;                                                     \
      }                                                              \
    } else if (imgInfo.voxelFormat == VoxelFormat::Float) {          \
      switch (imgInfo.bytesPerVoxel) {                               \
        case 4:                                                      \
          return function<float, T2>(__VA_ARGS__);                   \
          break;                                                     \
        case 8:                                                      \
          return function<double, T2>(__VA_ARGS__);                  \
          break;                                                     \
        default:                                                     \
          break;                                                     \
      }                                                              \
    } else if (imgInfo.voxelFormat == VoxelFormat::Signed) {         \
      switch (imgInfo.bytesPerVoxel) {                               \
        case 1:                                                      \
          return function<int8_t, T2>(__VA_ARGS__);                  \
          break;                                                     \
        case 2:                                                      \
          return function<int16_t, T2>(__VA_ARGS__);                 \
          break;                                                     \
        case 4:                                                      \
          return function<int32_t, T2>(__VA_ARGS__);                 \
          break;                                                     \
        case 8:                                                      \
          return function<int64_t, T2>(__VA_ARGS__);                 \
          break;                                                     \
        default:                                                     \
          break;                                                     \
      }                                                              \
    }                                                                \
  }

// for function that process 2 types of img
// first one is derived from img1, second one is derived from img2
#define IMG_TYPED_CALL_2TYPE(function, imgInfo1, imgInfo2, ...)                \
  {                                                                            \
    if (imgInfo2.voxelFormat == VoxelFormat::Unsigned) {                       \
      switch (imgInfo2.bytesPerVoxel) {                                        \
        case 1:                                                                \
          IMG_TYPED_CALL_FIX2NDTYPE(function, imgInfo1, uint8_t, __VA_ARGS__)  \
          break;                                                               \
        case 2:                                                                \
          IMG_TYPED_CALL_FIX2NDTYPE(function, imgInfo1, uint16_t, __VA_ARGS__) \
          break;                                                               \
        case 4:                                                                \
          IMG_TYPED_CALL_FIX2NDTYPE(function, imgInfo1, uint32_t, __VA_ARGS__) \
          break;                                                               \
        case 8:                                                                \
          IMG_TYPED_CALL_FIX2NDTYPE(function, imgInfo1, uint64_t, __VA_ARGS__) \
          break;                                                               \
        default:                                                               \
          break;                                                               \
      }                                                                        \
    } else if (imgInfo2.voxelFormat == VoxelFormat::Float) {                   \
      switch (imgInfo2.bytesPerVoxel) {                                        \
        case 4:                                                                \
          IMG_TYPED_CALL_FIX2NDTYPE(function, imgInfo1, float, __VA_ARGS__)    \
          break;                                                               \
        case 8:                                                                \
          IMG_TYPED_CALL_FIX2NDTYPE(function, imgInfo1, double, __VA_ARGS__)   \
          break;                                                               \
        default:                                                               \
          break;                                                               \
      }                                                                        \
    } else if (imgInfo2.voxelFormat == VoxelFormat::Signed) {                  \
      switch (imgInfo2.bytesPerVoxel) {                                        \
        case 1:                                                                \
          IMG_TYPED_CALL_FIX2NDTYPE(function, imgInfo1, int8_t, __VA_ARGS__)   \
          break;                                                               \
        case 2:                                                                \
          IMG_TYPED_CALL_FIX2NDTYPE(function, imgInfo1, int16_t, __VA_ARGS__)  \
          break;                                                               \
        case 4:                                                                \
          IMG_TYPED_CALL_FIX2NDTYPE(function, imgInfo1, int32_t, __VA_ARGS__)  \
          break;                                                               \
        case 8:                                                                \
          IMG_TYPED_CALL_FIX2NDTYPE(function, imgInfo1, int64_t, __VA_ARGS__)  \
          break;                                                               \
        default:                                                               \
          break;                                                               \
      }                                                                        \
    }                                                                          \
  }

struct ZImgWriteParameters
{
  Compression compression = Compression::AUTO;
  // from 1 to 9
  // Lower compression levels result in faster execution, but less compression.
  // Higher levels result in greater compression, but slower execution.
  uint32_t zlibCompressionLevel = 6;
  // from 1 to 100, low to high
  uint32_t jpegQuality = 95;
  // Use progressive entropy coding in JPEG images generated by the compression and transform functions.
  // Progressive entropy coding will generally improve compression relative to baseline entropy coding (the default),
  // but it will reduce compression and decompression performance considerably.
  bool jpegProgressive = true;
  // Use the most accurate DCT/IDCT algorithm available in the underlying codec: longer encoding time
  // The default if this flag is not specified is implementation-specific. For example, the implementation of
  // TurboJPEG for libjpeg[-turbo] uses the fast algorithm by default when compressing, because this has been
  // shown to have only a very slight effect on accuracy, but it uses the accurate algorithm when decompressing,
  // because this has been shown to have a larger effect.
  bool jpegAccurateDCT = true;
  // 444 (no subsampling) or 422 or 420, only apply to RGB
  uint32_t jpegChrominanceSubsampling = 444;
  // jpeg xr quality, [0.0 - 1.0] 1.0 is lossless
  double jpegXRQuality = 0.8;
};

class ZImgMetadata : public ZImgMetadataBase<ZImgMetatag>
{
public:
  [[nodiscard]] QString toQString() const;
};

class ZImg;

class ZImgThumbernail : public ZImgMetadataBase<ZImg>
{
public:
  [[nodiscard]] QString toQString() const;
};

// throw ZIOException if file does not exist
struct ZImgSource
{
  ZImgSource() = default;

  explicit ZImgSource(const QString& fn,
                      const ZImgRegion& rgn = ZImgRegion(),
                      size_t scene_ = 0,
                      FileFormat format_ = FileFormat::Unknown);

  ZImgSource(const QStringList& fns,
             Dimension catDim_,
             bool catScenes_ = true,
             const ZImgRegion& rgn = ZImgRegion(),
             size_t scene_ = 0,
             FileFormat format_ = FileFormat::Unknown,
             bool expandXY_ = true,
             bool expandWithMaxValue_ = false);

  inline bool operator==(const ZImgSource& other) const
  {
    if (filenames == other.filenames) {
      if (filenames.size() == 1) {
        return region == other.region && scene == other.scene;
      }
      return catDim == other.catDim && catScenes == other.catScenes && region == other.region && scene == other.scene &&
             expandXY == other.expandXY && expandWithMaxValue == other.expandWithMaxValue;
    }
    return false;
  }

  inline bool operator!=(const ZImgSource& other) const
  {
    return !(*this == other);
  }

  [[nodiscard]] inline QString toQString() const
  {
    return jsonToQString(*this);
  }

  [[nodiscard]] inline std::string toString() const
  {
    return jsonToString(*this);
  }

  QStringList filenames;
  Dimension catDim = Dimension::Z;
  bool catScenes = false;
  ZImgRegion region;
  size_t scene = 0;
  FileFormat format = FileFormat::Unknown;
  bool expandXY = true;
  bool expandWithMaxValue = false;

  size_t totalFileSize = 0;
};

void tag_invoke(const json::value_from_tag&, json::value& jv, const ZImgSource& imgSource);

ZImgSource tag_invoke(const json::value_to_tag<ZImgSource>&, const json::value& jv);

class ZImgSubBlock
{
public:
  ZImgSubBlock(size_t t_,
               index_t x_,
               index_t y_,
               index_t z_,
               size_t width_,
               size_t height_,
               size_t depth_,
               size_t xRatio_,
               size_t yRatio_,
               size_t zRatio_)
    : t(t_)
    , x(x_)
    , y(y_)
    , z(z_)
    , width(width_)
    , height(height_)
    , depth(depth_)
    , xRatio(xRatio_)
    , yRatio(yRatio_)
    , zRatio(zRatio_)
  {}

  virtual ~ZImgSubBlock();

  ZImgSubBlock(ZImgSubBlock&&) = default;

  ZImgSubBlock& operator=(ZImgSubBlock&&) = default;

  ZImgSubBlock(const ZImgSubBlock&) = default;

  ZImgSubBlock& operator=(const ZImgSubBlock&) = default;

  // subclass read should depend its own members rather than member of this class
  [[nodiscard]] virtual std::shared_ptr<ZImg> read() const = 0;

  [[nodiscard]] virtual ZImgInfo readInfo() const = 0;

  virtual void prefetch() const {}

  size_t t; // start t
  index_t x; // actual start x regardless of ratio
  index_t y; // actual start y regardless of ratio
  index_t z; // actual start z regardless of ratio
  size_t width; // real image width regardless of ratio; if ratio is 2, the stored image will have width: width/2
  size_t height; // real image height regardless of ratio; if ratio is 2, the stored image will have height: height/2
  size_t depth; // real image depth regardless of ratio; if ratio is 2, the stored image will have depth: depth/2
  size_t xRatio; // realsize / storedsize, 2 means downsampled by 2
  size_t yRatio; // realsize / storedsize, 2 means downsampled by 2
  size_t zRatio; // realsize / storedsize, 2 means downsampled by 2
};

class ZImgTileSubBlock : public ZImgSubBlock
{
public:
  explicit ZImgTileSubBlock(ZImgSource source,
                            size_t xRatio = 1,
                            size_t yRatio = 1,
                            size_t zRatio = 1,
                            ImgMergeMode downsampleCombineMode = ImgMergeMode::Interpolation);

  [[nodiscard]] std::shared_ptr<ZImg> read() const override;

  [[nodiscard]] ZImgInfo readInfo() const override;

private:
  ZImgSource m_source;
  size_t m_xRatio;
  size_t m_yRatio;
  size_t m_zRatio;
  ImgMergeMode m_downsampleCombineMode;
};

// Dimension order of ZImg is XYZCT
// this class might throw ZException or ZIOException if error

class ZImg
{
public:
  enum class ThresholdMode
  {
    IncludeThreshold,
    ExcludeThreshold
  };

  // create empty image
  ZImg();

  // create image with size and attribute specified by info and set all data to default value
  // see allocate() for default voxel value
  explicit ZImg(ZImgInfo info);

  //
  ZImg(const ZImg& other);

  ZImg(ZImg&& other) noexcept;

  // read image from file, throw ZIOException if read failed, might throw ZException if can't allocate memory
  explicit ZImg(const QString& filename,
                ZImgRegion region = ZImgRegion(),
                size_t scene = 0,
                size_t xRatio = 1,
                size_t yRatio = 1,
                size_t zRatio = 1,
                FileFormat format = FileFormat::Unknown);

  explicit ZImg(const QStringList& fileList,
                Dimension catDim,
                bool catScenes,
                const ZImgRegion& region = ZImgRegion(),
                size_t scene = 0,
                size_t xRatio = 1,
                size_t yRatio = 1,
                size_t zRatio = 1,
                FileFormat format = FileFormat::Unknown,
                bool expandXY = true,
                bool expandWithMaxValue = false);

  explicit ZImg(const ZImgSource& imgSource, size_t xRatio = 1, size_t yRatio = 1, size_t zRatio = 1);

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
  static void getQtReadNameFilter(QStringList& filters, std::vector<FileFormat>& formats);

  static void
  getQtWriteNameFilter(QStringList& filters, std::vector<FileFormat>& formats, std::vector<Compression>& comps);

  // return true if file extension is supported for read
  static bool fileExtensionReadSupported(const QString& filename);

  static bool fileExtensionWriteSupported(const QString& filename);

  // throw ZIOException if io error or empty image, might throw ZException if can't allocate memory
  void load(const QString& filename,
            size_t scene = 0,
            size_t xRatio = 1,
            size_t yRatio = 1,
            size_t zRatio = 1,
            FileFormat format = FileFormat::Unknown);

  void load(const QString& filename,
            ZImgRegion region,
            size_t scene = 0,
            size_t xRatio = 1,
            size_t yRatio = 1,
            size_t zRatio = 1,
            FileFormat format = FileFormat::Unknown);

  // load a sequence of imgs, cat these imgs along dimension "catDim"
  // imgs should have same size in other dimensions and have same type
  // throw ZIOException if io error, might throw ZException if can't allocate memory or can't cat imgs
  // expandXY can not be true if catDim is Dimension::X or Dimension::Y
  void load(const QStringList& fileList,
            Dimension catDim,
            bool catScenes,
            size_t scene = 0,
            size_t xRatio = 1,
            size_t yRatio = 1,
            size_t zRatio = 1,
            FileFormat format = FileFormat::Unknown,
            bool expandXY = true,
            bool expandWithMaxValue = false);

  void load(const QStringList& fileList,
            Dimension catDim,
            bool catScenes,
            const ZImgRegion& region,
            size_t scene = 0,
            size_t xRatio = 1,
            size_t yRatio = 1,
            size_t zRatio = 1,
            FileFormat format = FileFormat::Unknown,
            bool expandXY = true,
            bool expandWithMaxValue = false);

  void load(const ZImgSource& imgSource, size_t xRatio = 1, size_t yRatio = 1, size_t zRatio = 1);

  void save(const QString& filename,
            FileFormat format = FileFormat::Unknown,
            const ZImgWriteParameters& paras = ZImgWriteParameters()) const;

  static void writeImg(const QString& filename,
                       const ZImgSliceProvider& img,
                       FileFormat format = FileFormat::Unknown,
                       const ZImgWriteParameters& paras = ZImgWriteParameters());

  static void writeImg(const QString& filename,
                       const ZImgBlockProvider& img,
                       FileFormat format = FileFormat::Unknown,
                       const ZImgWriteParameters& paras = ZImgWriteParameters());

  // convenient function to get img information from file, throw ZIOException if read error or empty image
  static std::vector<ZImgInfo>
  readImgInfos(const QString& filename,
               std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks = nullptr,
               FileFormat format = FileFormat::Unknown);

  // throw ZIOException if sequence is not valid or empty image
  static std::vector<ZImgInfo>
  readImgInfos(const QStringList& fileList,
               Dimension catDim,
               bool catScenes,
               std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks = nullptr,
               FileFormat format = FileFormat::Unknown,
               bool expandXY = true);

  static ZImgInfo readImgInfo(const ZImgSource& imgSource,
                              std::vector<std::shared_ptr<ZImgSubBlock>>* subBlocks = nullptr);

  static ZImg
  readSubBlock(const QString& filename, size_t scene, size_t blockIndex, FileFormat format = FileFormat::Unknown);

  static ZImg readSubBlock(const QStringList& fileList,
                           Dimension catDim,
                           bool catScenes,
                           size_t scene,
                           size_t blockIndex,
                           FileFormat format = FileFormat::Unknown,
                           bool expandXY = true);

  static std::vector<std::vector<ZImgRegion>> getInternalSubRegions(const QString& filename,
                                                                    FileFormat format = FileFormat::Unknown);

  static ZImgMetadata readImgMetadata(const ZImgSource& imgSource);

  static ZImgThumbernail readImgThumbnail(const QString& filename,
                                          const ZImgRegion& region = ZImgRegion(),
                                          size_t scene = 0,
                                          FileFormat format = FileFormat::Unknown);

  // wrap exist raw data as ZImg, ZImg will **not** free the memory after using
  // only accept non-const data pointer (link error if input is const)
  // previous data of current img will be cleared
  // note: use with caution as the input data might not meet the alignment requirement of some operations
  template<typename TVoxel>
  void
  wrapData(TVoxel* data, size_t width, size_t height, size_t depth = 1, size_t numChannels = 1, size_t numTimes = 1);

  void wrapData(void* data, const ZImgInfo& info);

  void wrapData(const std::vector<void*>& data, const ZImgInfo& info);

  [[nodiscard]] inline bool isEmpty() const
  {
    return m_data.empty() || m_info.isEmpty();
  }

  [[nodiscard]] inline bool hasThumbnail() const
  {
    return !m_thumbnail.isEmpty();
  }

  [[nodiscard]] inline const ZImgThumbernail& thumbnail() const
  {
    return m_thumbnail;
  }

  [[nodiscard]] inline const ZImgInfo& info() const
  {
    return m_info;
  }

  [[nodiscard]] inline const ZImgMetadata& metadata() const
  {
    return m_metadata;
  }

  inline ZImgThumbernail& thumbnailRef()
  {
    return m_thumbnail;
  }

  inline ZImgInfo& infoRef()
  {
    return m_info;
  }

  inline ZImgMetadata& metadataRef()
  {
    return m_metadata;
  }

  template<typename TVoxel>
  [[nodiscard]] bool isType() const
  {
    return m_info.isType<TVoxel>();
  }

  [[nodiscard]] inline bool isSameType(const ZImg& other) const
  {
    return m_info.isSameType(other.m_info);
  }

  [[nodiscard]] inline bool isSameType(const ZImgInfo& otherInfo) const
  {
    return m_info.isSameType(otherInfo);
  }

  [[nodiscard]] inline bool isSameSize(const ZImg& other) const
  {
    return m_info.isSameSize(other.m_info);
  }

  [[nodiscard]] inline bool contains(size_t x, size_t y, size_t z, size_t c, size_t t = 0) const
  {
    return !isEmpty() && x < m_info.width && y < m_info.height && z < m_info.depth && c < m_info.numChannels &&
           t < m_info.numTimes;
  }

  [[nodiscard]] inline size_t size(Dimension dim) const
  {
    return m_info.size(dim);
  }

  [[nodiscard]] inline size_t size(size_t dim) const
  {
    return m_info.size(dim);
  }

  // inline bool isMultiLocationsImg() const { return m_info.numLocations > 1; }
  [[nodiscard]] inline bool isTimeSeries() const
  {
    return m_info.numTimes > 1;
  }

  [[nodiscard]] inline bool isMultiChannelsImg() const
  {
    return m_info.numChannels > 1;
  }

  // imgs that have x,y,z dimensions and don't have t,l dimensions are 3D Img, x,y dimensions can be singleton
  [[nodiscard]] inline bool is3DImg() const
  {
    return !isEmpty() && m_info.numTimes == 1 && m_info.depth > 1;
  }

  // imgs that have x,y dimensions and don't have z,t,l dimensions are 2D Img, x,y dimensions can be singleton
  [[nodiscard]] inline bool is2DImg() const
  {
    return !isEmpty() && m_info.numTimes == 1 && m_info.depth == 1;
  }

  [[nodiscard]] inline size_t voxelByteNumber() const
  {
    return m_info.voxelByteNumber();
  }

  [[nodiscard]] inline size_t rowVoxelNumber() const
  {
    return m_info.rowVoxelNumber();
  }

  [[nodiscard]] inline size_t rowByteNumber() const
  {
    return m_info.rowByteNumber();
  }

  [[nodiscard]] inline size_t planeVoxelNumber() const
  {
    return m_info.planeVoxelNumber();
  }

  [[nodiscard]] inline size_t planeByteNumber() const
  {
    return m_info.planeByteNumber();
  }

  [[nodiscard]] inline size_t channelVoxelNumber() const
  {
    return m_info.channelVoxelNumber();
  }

  [[nodiscard]] inline size_t channelByteNumber() const
  {
    return m_info.channelByteNumber();
  }

  [[nodiscard]] inline size_t timeVoxelNumber() const
  {
    return m_info.timeVoxelNumber();
  }

  [[nodiscard]] inline size_t timeByteNumber() const
  {
    return m_info.timeByteNumber();
  }

  // inline size_t locationVoxelNumber() const { return m_info.locationVoxelNumber(); }
  // inline size_t locationByteNumber() const { return m_info.locationByteNumber(); }
  [[nodiscard]] inline size_t voxelNumber() const
  {
    return m_info.voxelNumber();
  }

  [[nodiscard]] inline size_t byteNumber() const
  {
    return m_info.byteNumber();
  }

  [[nodiscard]] inline size_t width() const
  {
    return m_info.width;
  }

  [[nodiscard]] inline size_t height() const
  {
    return m_info.height;
  }

  [[nodiscard]] inline size_t depth() const
  {
    return m_info.depth;
  }

  [[nodiscard]] inline size_t numChannels() const
  {
    return m_info.numChannels;
  }

  [[nodiscard]] inline size_t numTimes() const
  {
    return m_info.numTimes;
  }

  // inline size_t numLocations() const { return m_info.numLocations; }
  [[nodiscard]] inline VoxelFormat voxelFormat() const
  {
    return m_info.voxelFormat;
  }

  [[nodiscard]] inline size_t bytesPerVoxel() const
  {
    return m_info.bytesPerVoxel;
  }

  [[nodiscard]] inline size_t validBitCount() const
  {
    return m_info.validBitCount;
  }

  [[nodiscard]] inline VoxelSizeUnit voxelSizeUnit() const
  {
    return m_info.voxelSizeUnit;
  }

  [[nodiscard]] inline double voxelSizeX() const
  {
    return m_info.voxelSizeX;
  }

  [[nodiscard]] inline double voxelSizeY() const
  {
    return m_info.voxelSizeY;
  }

  [[nodiscard]] inline double voxelSizeZ() const
  {
    return m_info.voxelSizeZ;
  }

  //  // if current or result voxelSizeUnit is Voxel, throw exception
  //  inline double voxelSizeXInUnit(VoxelSizeUnit unit) const
  //  { return m_info.voxelSizeXInUnit(unit); }
  //
  //  inline double voxelSizeYInUnit(VoxelSizeUnit unit) const
  //  { return m_info.voxelSizeYInUnit(unit); }
  //
  //  inline double voxelSizeZInUnit(VoxelSizeUnit unit) const
  //  { return m_info.voxelSizeZInUnit(unit); }
  //
  //  inline double voxelSizeXInUm() const
  //  { return voxelSizeXInUnit(VoxelSizeUnit::um); }
  //
  //  inline double voxelSizeYInUm() const
  //  { return voxelSizeYInUnit(VoxelSizeUnit::um); }
  //
  //  inline double voxelSizeZInUm() const
  //  { return voxelSizeZInUnit(VoxelSizeUnit::um); }

  [[nodiscard]] inline col4 channelColor(size_t c) const
  {
    return m_info.channelColors[c];
  }

  // inline Location location(size_t l) const { return m_info.locations[l]; }
  [[nodiscard]] inline const QString& channelName(size_t c) const
  {
    return m_info.channelNames[c];
  }

  [[nodiscard]] inline QString displayChannelName(size_t c) const
  {
    return m_info.displayChannelName(c);
  }

  [[nodiscard]] inline double timeStamp(size_t t) const
  {
    return m_info.timeStamps[t];
  }

  // remove old data and allocate data space based on current info
  // if voxel type is signed integer, data will be filled with that type's minimum negative value
  // otherwise data will be set to 0
  // throw ZException if can not allocate memory
  void allocate();

  template<typename T = uint8_t>
  inline T* timeData(size_t t)
  {
    return reinterpret_cast<T*>(m_data[t]);
  }

  template<typename T = uint8_t>
  inline T* channelData(size_t c, size_t t = 0)
  {
    return reinterpret_cast<T*>(m_data[t] + c * m_info.channelByteNumber());
  }

  template<typename T = uint8_t>
  inline T* planeData(size_t z, size_t c = 0, size_t t = 0)
  {
    return reinterpret_cast<T*>(m_data[t] + c * m_info.channelByteNumber() + z * m_info.planeByteNumber());
  }

  template<typename T = uint8_t>
  inline T* rowData(size_t y, size_t z = 0, size_t c = 0, size_t t = 0)
  {
    return reinterpret_cast<T*>(m_data[t] + c * m_info.channelByteNumber() + z * m_info.planeByteNumber() +
                                y * m_info.rowByteNumber());
  }

  template<typename T = uint8_t>
  inline T* data(size_t x, size_t y, size_t z = 0, size_t c = 0, size_t t = 0)
  {
    return reinterpret_cast<T*>(m_data[t] + c * m_info.channelByteNumber() + z * m_info.planeByteNumber() +
                                y * m_info.rowByteNumber() + x * m_info.voxelByteNumber());
  }

  template<typename T = uint8_t>
  inline T* data(const ZVoxelCoordinate& coord)
  {
    return reinterpret_cast<T*>(m_data[coord.t] + coord.c * m_info.channelByteNumber() +
                                coord.z * m_info.planeByteNumber() + coord.y * m_info.rowByteNumber() +
                                coord.x * m_info.voxelByteNumber());
  }

  template<typename T = uint8_t>
  inline T* data(size_t idx)
  {
    // size_t l = idx / m_info.locationVoxelNumber();
    // idx -= l * m_info.locationVoxelNumber();
    size_t t = idx / m_info.timeVoxelNumber();
    idx -= t * m_info.timeVoxelNumber();
    return reinterpret_cast<T*>(m_data[t] + idx * m_info.voxelByteNumber());
  }

  template<typename T = uint8_t>
  [[nodiscard]] inline const T* timeData(size_t t) const
  {
    return reinterpret_cast<T*>(m_data[t]);
  }

  template<typename T = uint8_t>
  inline const T* channelData(size_t c, size_t t = 0) const
  {
    return reinterpret_cast<T*>(m_data[t] + c * m_info.channelByteNumber());
  }

  template<typename T = uint8_t>
  inline const T* planeData(size_t z, size_t c = 0, size_t t = 0) const
  {
    return reinterpret_cast<T*>(m_data[t] + c * m_info.channelByteNumber() + z * m_info.planeByteNumber());
  }

  template<typename T = uint8_t>
  inline const T* rowData(size_t y, size_t z = 0, size_t c = 0, size_t t = 0) const
  {
    return reinterpret_cast<T*>(m_data[t] + c * m_info.channelByteNumber() + z * m_info.planeByteNumber() +
                                y * m_info.rowByteNumber());
  }

  template<typename T = uint8_t>
  inline const T* data(size_t x, size_t y, size_t z = 0, size_t c = 0, size_t t = 0) const
  {
    return reinterpret_cast<T*>(m_data[t] + c * m_info.channelByteNumber() + z * m_info.planeByteNumber() +
                                y * m_info.rowByteNumber() + x * m_info.voxelByteNumber());
  }

  template<typename T = uint8_t>
  inline const T* data(const ZVoxelCoordinate& coord) const
  {
    return reinterpret_cast<T*>(m_data[coord.t] + coord.c * m_info.channelByteNumber() +
                                coord.z * m_info.planeByteNumber() + coord.y * m_info.rowByteNumber() +
                                coord.x * m_info.voxelByteNumber());
  }

  template<typename T = uint8_t>
  inline const T* data(size_t idx) const
  {
    // size_t l = idx / m_info.locationVoxelNumber();
    // idx -= l * m_info.locationVoxelNumber();
    size_t t = idx / m_info.timeVoxelNumber();
    idx -= t * m_info.timeVoxelNumber();
    return reinterpret_cast<T*>(m_data[t] + idx * m_info.voxelByteNumber());
  }

  // return out bound voxel value based on boundary condition (padOption)
  // img must be type T otherwise might crash
  template<typename T>
  inline T
  outBoundValue(const ZVoxelCoordinate& coord, PadOption padOption = PadOption::Constant, T padValue = T(0)) const
  {
    if (padOption == PadOption::Constant) {
      return padValue;
    }
    ZVoxelCoordinate coordCopy = coord;
    wrapCoord(coordCopy, padOption);
    return *(data<T>(coordCopy));
  }

  template<typename T>
  inline T outBoundValue(index_t x,
                         index_t y,
                         index_t z,
                         index_t c = 0,
                         index_t t = 0,
                         PadOption padOption = PadOption::Constant,
                         T padValue = T(0)) const
  {
    if (padOption == PadOption::Constant) {
      return padValue;
    }
    ZVoxelCoordinate coordCopy(x, y, z, c, t);
    wrapCoord(coordCopy, padOption);
    return *(data<T>(coordCopy));
  }

  // reverse endianness of img data
  void reverseEndianness();

  // output img as values row by row, for debug
  [[nodiscard]] QString toQString() const;

  // img view is a virtual img that doesn't own any img data, usually it is a channel or a time spot or a location from
  // original img operate on img view is same as operate on partial img img view will automatically become to a real img
  // after some operations that need memory reallocation use img view to work with part of img
  [[nodiscard]] ZImg createView(index_t c = -1, index_t t = -1);

  [[nodiscard]] ZImg createView(index_t c = -1, index_t t = -1) const;

  // view of one single channel slice
  [[nodiscard]] ZImg createView(size_t z, size_t c, size_t t);

  [[nodiscard]] ZImg createView(size_t z, size_t c, size_t t) const;

  [[nodiscard]] inline bool isImgView() const
  {
    return !m_ownData;
  }

  // statistics
  template<typename TValue>
  void computeMinMax(TValue& min, TValue& max) const;

  // if nbins == 0, default number of bins is used
  // default number of bins for 8bit img is 256, for other type of img is 65536
  // bin size for float img is (dataRangeMax-dataRangeMin)/nbins
  // bin size for integer img is (dataRangeMax-dataRangeMin+1)/nbins
  // if mask is specified, it should be same size as current img, in mask img, zero indicate off, non-zero indicate on
  // only mask on voxels are counted in histogram
  [[nodiscard]] std::vector<size_t> histogram(size_t nbins = 0, const ZImg& mask = ZImg()) const;

  // given an bin index, return data range this bin represent
  // not very accurate for 64-bit integer type
  [[nodiscard]] inline std::pair<double, double> binRange(size_t binIdx, size_t nbins = 0) const
  {
    return m_info.binRange(binIdx, nbins);
  }

  // take range as parameter, TRange will be cast to img data type
  // bin size for float img is (maxData-minData)/nbins
  // bin size for integer img is (maxData-minData+1)/nbins
  template<typename TRange>
  std::vector<size_t> histogram(TRange minData, TRange maxData, size_t nbins = 0, const ZImg& mask = ZImg()) const
  {
    if (nbins == 0) {
      nbins = bytesPerVoxel() > 1 ? 65536 : 256;
    }

    std::vector<size_t> res(nbins, 0);

    if (mask.isEmpty()) {
      IMG_TYPED_CALL(histogram_Impl, m_info, res, minData, maxData)
    } else if (isSameSize(mask)) {
      IMG_TYPED_CALL_2TYPE(histogramMask_Impl, m_info, mask.info(), res, minData, maxData, mask)
    } else {
      throw ZException(fmt::format("histogram mask has different size <{}> than current img <{}>",
                                   mask.info().toString(),
                                   m_info.toString()));
    }

    return res;
  }

  // overload
  template<typename TRange>
  inline std::pair<double, double> binRange(size_t binIdx, TRange minData, TRange maxData, size_t nbins = 0) const
  {
    return m_info.binRange<TRange>(binIdx, minData, maxData, nbins);
  }

  // property of img type
  // intensity range of current img type, for float img, range is [0.0 1.0]
  // use template return type because img can be any type, and even double type can not represent all 64-bit integer
  // type value
  template<typename TValue = double>
  inline TValue dataRangeMin() const
  {
    return m_info.dataRangeMin<TValue>();
  }

  template<typename TValue = double>
  inline TValue dataRangeMax() const
  {
    return m_info.dataRangeMax<TValue>();
  }

  // some utils, these functions will throw ZException if input parameters is invalid

  // if region is empty, return empty img
  // throw ZException if current img is empty or region is not valid
  [[nodiscard]] ZImg crop(const ZImgRegion& region) const;

  // crop from start coordinate to end coordinate with outside pixel padded.
  // padValue is only used when padOption is PadOption::Constant, padValue will be cast to img voxel type
  // only requirement is that coord end should be large or equal (in this case return empty img) than coord start
  // (valid) throw ZException if current img is empty or region goes wrong
  template<typename TPadValue = uint8_t>
  ZImg cropWithPad(const ZVoxelCoordinate& startCoord,
                   const ZVoxelCoordinate& endCoord,
                   PadOption padOption = PadOption::Constant,
                   TPadValue padValue = TPadValue(0)) const
  {
    ZImg res;

    if (isEmpty()) {
      throw ZException(fmt::format("Can not crop empty img <{}>", m_info.toString()));
    }
    if (endCoord.anyLessThan(startCoord)) {
      throw ZException(fmt::format("Try to crop pad img with invalid region <{}> to <{}>",
                                   startCoord.toString(),
                                   endCoord.toString()));
    }
    if (endCoord.anyEqual(startCoord)) {
      return res;
    }

    if (startCoord.allGreaterEqual(0) &&
        endCoord.allLessEqual(
          ZVoxelCoordinate(m_info.width, m_info.height, m_info.depth, m_info.numChannels, m_info.numTimes))) {
      res = crop(ZImgRegion(startCoord, endCoord));
    }

    ZImgInfo info = m_info;
    info.width = endCoord.x - startCoord.x;
    info.height = endCoord.y - startCoord.y;
    info.depth = endCoord.z - startCoord.z;
    info.numChannels = endCoord.c - startCoord.c;
    info.numTimes = endCoord.t - startCoord.t;
    info.createDefaultDescriptions();

    res = ZImg(info);

    IMG_TYPED_CALL(cropWithPad_Impl, info, res, startCoord, endCoord, padOption, padValue)

    return res;
  }

  // extract part of img
  [[nodiscard]] ZImg extractVoxel(size_t x, size_t y, index_t z = -1, index_t c = -1, index_t t = -1) const;

  [[nodiscard]] ZImg extractCol(size_t x, index_t z = -1, index_t c = -1, index_t t = -1) const;

  [[nodiscard]] ZImg extractRow(size_t y, index_t z = -1, index_t c = -1, index_t t = -1) const;

  [[nodiscard]] ZImg extractPlane(size_t z, index_t c = -1, index_t t = -1) const;

  [[nodiscard]] ZImg extractChannel(size_t c, index_t t = -1) const;

  [[nodiscard]] ZImg extractTime(size_t t) const;

  // value will be cast to img voxel type
  template<typename TFillValue>
  ZImg& fill(TFillValue value)
  {
    if (bytesPerVoxel() == 1 || value == TFillValue(0)) {
      for (size_t t = 0; t < m_info.numTimes; ++t) {
        std::memset(timeData(t), static_cast<unsigned char>(value), timeByteNumber());
      }
      return *this;
    }

    IMG_TYPED_CALL(fill_Impl, m_info, value)
    return *this;
  }

  // fill with uniform distributed value with range [dataRangeMin, dataRangeMax]
  ZImg& fillRandom();

  // paste another img into this img start from certain location (default zero)
  // type cast might happen if input img has different data type
  // do nothing if input img and current img have no overlap
  ZImg& pasteImg(const ZImg& img, const ZVoxelCoordinate& start = ZVoxelCoordinate(), bool warningOn = true);

  // similar to pasteImg, except that pasteImg keep the intensity value of input img in overlap area while
  // this function keep the maximum intensity value of current img and input img
  ZImg& pasteImgMax(const ZImg& img, const ZVoxelCoordinate& start = ZVoxelCoordinate(), bool warningOn = true);

  // cat a series of img along certain dimension
  // imgs should be same type and have same dimension size other than the dimension to cat
  // dim should be valid
  // throw ZException if can not cat
  static ZImg cat(const std::vector<ZImg>& imgs, Dimension dim);

  static ZImg cat(const std::vector<ZImg*>& imgs, Dimension dim);

  static ZImg cat(const std::vector<const ZImg*>& imgs, Dimension dim);

  // combine image of same type and same dimensions
  static ZImg combine(const std::vector<ZImg>& imgs, ImgMergeMode mode);

  static ZImg combine(const std::vector<ZImg*>& imgs, ImgMergeMode mode);

  static ZImg combine(const std::vector<const ZImg*>& imgs, ImgMergeMode mode);

  // projection
  [[nodiscard]] ZImg projectAlongDim(Dimension dim, ImgMergeMode mode, index_t start = -1, index_t end = -1) const;

  [[nodiscard]] ZImg maximumZProjection(index_t start = -1, index_t end = -1) const;

  // map [minData maxData] to [dataRangeMin, dataRangeMax]
  // for float img, dataRangeMin is 0.0, dataRangeMax is 1.0
  template<typename TRange>
  [[nodiscard]] ZImg normalized(TRange minData, TRange maxData) const
  {
    ZImg res(*this);
    res.normalize(minData, maxData);
    return res;
  }

  // first compute minimum and maximum value, then use as Range
  [[nodiscard]] ZImg normalized() const;

  // to normalize only channel 1, call img.createView(1,-1,-1).normalize(minD, maxD);
  template<typename TRange>
  ZImg& normalize(TRange minData, TRange maxData)
  {
    if (isEmpty()) {
      return *this;
    }
    size_t bytesPerVoxel = m_info.bytesPerVoxel;
    VoxelFormat vf = m_info.voxelFormat;
    if (vf == VoxelFormat::Float) {
      switch (bytesPerVoxel) {
        case 4:
          scale_Impl<float, float>(minData, maxData, this, this);
          break;
        case 8:
          scale_Impl<double, double>(minData, maxData, this, this);
          break;
        default:
          break;
      }
    } else if (vf == VoxelFormat::Signed) {
      switch (bytesPerVoxel) {
        case 1:
          scale_Impl<int8_t, int8_t>(minData, maxData, this, this);
          break;
        case 2:
          scale_Impl<int16_t, int16_t>(minData, maxData, this, this);
          break;
        case 4:
          scale_Impl<int32_t, int32_t>(minData, maxData, this, this);
          break;
        case 8:
          scale_Impl<int64_t, int64_t>(minData, maxData, this, this);
          break;
        default:
          break;
      }
    } else {
      switch (bytesPerVoxel) {
        case 1:
          scale_Impl<uint8_t, uint8_t>(minData, maxData, this, this);
          break;
        case 2:
          scale_Impl<uint16_t, uint16_t>(minData, maxData, this, this);
          break;
        case 4:
          scale_Impl<uint32_t, uint32_t>(minData, maxData, this, this);
          break;
        case 8:
          scale_Impl<uint64_t, uint64_t>(minData, maxData, this, this);
          break;
        default:
          break;
      }
    }

    m_info.validBitCount = 0;
    return *this;
  }

  // first compute minimum and maximum value, then use as Range
  ZImg& normalize();

  // img type conversion
  // make img of new type by casting current img
  // e.g. img = img.castTo<double>();
  template<typename TDesVoxel>
  [[nodiscard]] ZImg castTo() const;

  // overload, cast voxel to format, throw if combination is not supported (like 3-byte float..)
  [[nodiscard]] ZImg castTo(VoxelFormat vf, size_t bytePerVoxel);

  // make img of new type, map current data type range to target img data type range, return new img
  // note: float img data type range is [0.0 1.0]
  template<typename TDesVoxel = uint8_t>
  [[nodiscard]] ZImg convertTo() const
  {
    ZImgInfo info = m_info;
    info.setVoxelFormat<TDesVoxel>();
    ZImg res(info);

    IMG_TYPED_CALL_FIX2NDTYPE(convert_Impl, m_info, TDesVoxel, false, this, &res)
    return res;
  }

  // convert normalized img to another type
  template<typename TDesVoxel = uint8_t>
  [[nodiscard]] ZImg convertNormalizedTo() const
  {
    ZImgInfo info = m_info;
    info.setVoxelFormat<TDesVoxel>();
    ZImg res(info);

    IMG_TYPED_CALL_FIX2NDTYPE(convert_Impl, m_info, TDesVoxel, true, this, &res)
    return res;
  }

  // make img of new type, map [minData maxData] to target img data type range, return new img
  template<typename TDesVoxel = uint8_t, typename TRange>
  [[nodiscard]] ZImg convertTo(TRange minData, TRange maxData) const
  {
    ZImgInfo info = m_info;
    info.setVoxelFormat<TDesVoxel>();
    ZImg res(info);

    IMG_TYPED_CALL_FIX2NDTYPE(scale_Impl, m_info, TDesVoxel, minData, maxData, this, &res)

    return res;
  }

  // deduce TDesVoxel from img
  template<typename TRange>
  [[nodiscard]] ZImg convertTo(TRange minData, TRange maxData, const ZImg& targetImgType) const
  {
    IMG_RETURN_TYPED_CALL(convertTo, targetImgType.info(), minData, maxData)
  }

  template<typename TRange>
  [[nodiscard]] ZImg convertTo(TRange minData, TRange maxData, const ZImgInfo& targetImgTypeInfo) const
  {
    IMG_RETURN_TYPED_CALL(convertTo, targetImgTypeInfo, minData, maxData)
  }

  // resize in x-y-z dimensions
  // 'antialiasing' specifies whether to perform antialiasing when shrinking an image. For the 'nearest' method,
  // the parameter 'antialiasingForNearest' is used (default false); for all other methods, the default is true.
  [[nodiscard]] ZImg resized(size_t desWidth,
                             size_t desHeight,
                             size_t desDepth,
                             Interpolant interpolant = Interpolant::Cubic,
                             bool antialiasing = true,
                             bool antialiasingForNearest = false,
                             bool useMultithreading = true) const;

  // intel ipp version, will crash if interpolant or data type is not supported
  [[nodiscard]] ZImg
  resizedIPP(size_t desWidth, size_t desHeight, size_t desDepth, Interpolant interpolant = Interpolant::Cubic) const;

  [[nodiscard]] ZImg zoomed(double scaleX,
                            double scaleY,
                            double scaleZ,
                            Interpolant interpolant = Interpolant::Cubic,
                            bool antialiasing = true,
                            bool antialiasingForNearest = false) const;

  // combine voxels in each block into one voxel of result img
  // result img size is ceil(width/blockWidth) * ceil(height/blockHeight) * ceil(depth/blockDepth)
  [[nodiscard]] ZImg
  blockDownsampled(size_t blockWidth, size_t blockHeight, size_t blockDepth, ImgMergeMode mode) const;

  // resize zoom this img, will change img memory and make virtual img non-virtual
  ZImg& resize(size_t desWidth,
               size_t desHeight,
               size_t desDepth,
               Interpolant interpolant = Interpolant::Cubic,
               bool antialiasing = true,
               bool antialiasingForNearest = false,
               bool useMultithreading = true);

  // intel ipp version, will crash if interpolant or data type is not supported
  inline ZImg&
  resizeIPP(size_t desWidth, size_t desHeight, size_t desDepth, Interpolant interpolant = Interpolant::Cubic)
  {
    if (width() == desWidth && height() == desHeight && depth() == desDepth) {
      return *this;
    }
    ZImg res = resizedIPP(desWidth, desHeight, desDepth, interpolant);
    swap(res);
    return *this;
  }

  ZImg& zoom(double scaleX,
             double scaleY,
             double scaleZ = 1.0,
             Interpolant interpolant = Interpolant::Cubic,
             bool antialiasing = true,
             bool antialiasingForNearest = false);

  ZImg& blockDownsample(size_t blockWidth, size_t blockHeight, size_t blockDepth, ImgMergeMode mode);

  // flip along dim, dim can be normal x-y-z dim(0-1-2), channel dim(3), location dim(5) or time dimension(4)
  ZImg& flip(Dimension dim);

  // reflect img, note: will reflect all dimension includes time, location, channel..
  // use createImgView to reflect only part of img
  ZImg& reflect();

  // returns a same size img that contains the cumulative sum of the voxels along dimension dim
  // note: for integer image perform saturate arithmetic, see below
  [[nodiscard]] ZImg cumulativeSum(Dimension dim) const;

  // calculate the sum of each 3D block defined by the template size. The
  // returned img will have size (width+twidth-1) x (height+theight-1) x (depth+tdepth-1) x c x t x l
  // all locations, times, channels are processed in the same way
  // throw ZException if template size is 0
  // note: for integer image perform saturate arithmetic, see below
  [[nodiscard]] ZImg blockSum(size_t twidth, size_t theight, size_t tdepth) const;

  // same as blockSum then crop [startX, endX) * [startY, endY) * [startZ, endZ)
  // throw ZException if template size is 0 or range is wrong
  // note: for integer image perform saturate arithmetic, see below
  [[nodiscard]] ZImg blockSumPart(size_t twidth,
                                  size_t theight,
                                  size_t tdepth,
                                  size_t xStart,
                                  size_t xEnd,
                                  size_t yStart,
                                  size_t yEnd,
                                  size_t zStart,
                                  size_t zEnd) const;

  // threshold
  // if voxel >  (or >= based on ThresholdMode) threshold, voxel = outsidevalue,
  // TValue will be cast to current img data type
  template<typename TValue>
  ZImg& thresholdAbove(TValue threshold, ThresholdMode threMode, TValue outsideValue)
  {
    IMG_TYPED_CALL(thresholdAbove_Impl, m_info, threshold, threMode, outsideValue)
    return *this;
  }

  // if voxel <  (or <= based on ThresholdMode) threshold, voxel = outsidevalue,
  // TValue will be cast to current img data type
  template<typename TValue>
  ZImg& thresholdBelow(TValue threshold, ThresholdMode threMode, TValue outsideValue = TValue(0))
  {
    IMG_TYPED_CALL(thresholdBelow_Impl, m_info, threshold, threMode, outsideValue)
    return *this;
  }

  // binarize
  // return a uint8_t img with 1 and 0
  // if voxel > (or >= based on ThresholdMode) threshold, result mask voxel = 1 else mask voxel = 0
  // TValue threshold will be cast to current img data type
  template<typename TValue>
  [[nodiscard]] ZImg binarized(TValue threshold, ThresholdMode threMode) const
  {
    ZImgInfo info = m_info;
    info.voxelFormat = VoxelFormat::Unsigned;
    info.bytesPerVoxel = 1;
    ZImg res(info);

    IMG_TYPED_CALL(binarized_Impl, m_info, res, threshold, threMode)

    return res;
  }

  // return a uint8_t img with 1 and 0
  // for all img type, if voxel > 0, result mask voxel = 1
  [[nodiscard]] inline ZImg binarized() const
  {
    return binarized(0, ThresholdMode::ExcludeThreshold);
  }

  // return a uint8_t img with 1 and 0
  // for all img type, if isForeground(voxel) return true, result mask voxel = 1
  // GenericForegroundPredictor take any numeric type as parameter and return bool
  template<typename GenericForegroundPredictor>
  ZImg binarized(const GenericForegroundPredictor& isForeground) const
  {
    ZImgInfo info = m_info;
    info.voxelFormat = VoxelFormat::Unsigned;
    info.bytesPerVoxel = 1;
    ZImg res(info);

    IMG_TYPED_CALL(binarized_Impl, m_info, res, isForeground)

    return res;
  }

  // if you know the img type
  // ForegroundPredictor take TVoxel as parameter and return bool
  // throw ZException if type don't match
  template<typename TVoxel, typename ForegroundPredictor>
  ZImg typedBinarized(const ForegroundPredictor& isForeground) const
  {
    if (!isType<TVoxel>()) {
      throw ZException("Call typedBinarized with wrong type");
    }

    ZImgInfo info = m_info;
    info.voxelFormat = VoxelFormat::Unsigned;
    info.bytesPerVoxel = 1;
    ZImg res(info);

    binarized_Impl<TVoxel>(res, isForeground);
    return res;
  }

  // for integer type all perform saturate arithmetic
  // for example for a 8bit unsigned img, 128 * 2 = 255;  253 + 6 = 255; 34 - 230 = 0
  // for 8bit signed img, 12 + 120 = 127; -45-120 = -128; 23 * 1.51 = round(34.73) = 35;
  template<typename TScalar>
  ZImg& operator+=(TScalar scalar)
  {
    static_assert(std::is_arithmetic_v<TScalar>, "Arithmetic not possible on this type");
    if (scalar != TScalar(0)) {
      IMG_TYPED_CALL(addScalar_Impl, m_info, scalar)
    }
    return *this;
  }

  // add img, input should have same size, otherwise throw ZException
  ZImg& operator+=(const ZImg& rhs);

  template<typename TScalarOrZImg>
  ZImg operator+(const TScalarOrZImg& scalarOrZImg) const
  {
    ZImg res(*this);
    res += scalarOrZImg;
    return res;
  }

  template<typename TScalar>
  ZImg& operator-=(TScalar scalar)
  {
    static_assert(std::is_arithmetic_v<TScalar>, "Arithmetic not possible on this type");
    if (scalar != TScalar(0)) {
      IMG_TYPED_CALL(subScalar_Impl, m_info, scalar)
    }
    return *this;
  }

  // sub img, input should have same size, otherwise throw ZException
  ZImg& operator-=(const ZImg& rhs);

  template<typename TScalarOrZImg>
  ZImg operator-(const TScalarOrZImg& scalarOrZImg) const
  {
    ZImg res(*this);
    res -= scalarOrZImg;
    return res;
  }

  template<typename TScalar>
  ZImg& operator*=(TScalar scalar)
  {
    static_assert(std::is_arithmetic_v<TScalar>, "Arithmetic not possible on this type");
    if (scalar != TScalar(0)) {
      IMG_TYPED_CALL(mulScalar_Impl, m_info, scalar)
    } else {
      fill(0);
    }
    return *this;
  }

  // multiply img, input should have same size, otherwise throw ZException
  ZImg& operator*=(const ZImg& rhs);

  template<typename TScalarOrZImg>
  ZImg operator*(const TScalarOrZImg& scalarOrZImg) const
  {
    ZImg res(*this);
    res *= scalarOrZImg;
    return res;
  }

  template<typename TScalar>
  // throw ZException if scalar is zero and not float
  ZImg& operator/=(TScalar scalar)
  {
    static_assert(std::is_arithmetic_v<TScalar>, "Arithmetic not possible on this type");
    if (scalar != TScalar(0)) {
      IMG_TYPED_CALL(divScalar_Impl, m_info, scalar)
    } else {
      throw ZException("Can not divide img by zero");
    }
    return *this;
  }

  // divide img, input should have same size, otherwise throw ZException
  // might got hardware exception if rhs contains zero and both img is not float type
  ZImg& operator/=(const ZImg& rhs);

  template<typename TScalarOrZImg>
  ZImg operator/(const TScalarOrZImg& scalarOrZImg) const
  {
    ZImg res(*this);
    res /= scalarOrZImg;
    return res;
  }

  // divide img, input should have same size, otherwise throw ZException
  // result voxel is 0 if rhs voxel is 0
  ZImg& secureDivideBy(const ZImg& rhs);

  // return true if img are same type same size and has same content
  bool operator==(const ZImg& other) const;

  // perform custom voxel-wise unary operator
  // GenericCustomUnaryOp is a generic lambda or non-template functor that accepts current voxel as argument and return
  // the new voxel value the non-template functor might have a templated operator() to process all possible voxel types
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
  ZImg& unaryOperation(const GenericCustomUnaryOp& op)
  {
    IMG_TYPED_CALL(unaryOp_Impl, m_info, op)
    return *this;
  }

  // if you already know the img type (e.g. double) and have a function for that type like:
  // double someOpForDoubleVoxel(double current);
  // you can use the typed version which generate less code:
  // img.typedUnaryOperation<double>(someOpForDoubleVoxel);
  // op should be a unary function that accepts current voxel as argument and return the new voxel value
  // op can be either a function pointer or an instantiated function object (can have internal state)
  // **note** throw ZException if type don't match
  template<typename TVoxel, typename CustomUnaryOp>
  ZImg& typedUnaryOperation(const CustomUnaryOp& op)
  {
    if (!isType<TVoxel>()) {
      throw ZException("Call typedUnaryOperation with wrong type");
    }
    unaryOp_Impl<TVoxel>(op);
    return *this;
  }

  // perform a custom voxel-wise operator <op> of *this and other
  // GenericBinaryFunctor is a generic lambda or non-template functor that accepts current voxel from *this as first
  // argument and another voxel from same position of other as second argument and return the new value of current voxel
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
  // make sure input img has same size as current img, otherwise ZException will be thrown
  // note: this will generate about 100 switch case branches in function because we need to determine two img type
  template<typename GenericCustomBinaryOp>
  ZImg& binaryOperation(const ZImg& other, const GenericCustomBinaryOp& op)
  {
    if (!isSameSize(other)) {
      throw ZException(fmt::format("img binary operation requires same size img as input: this <{}>, other <{}>",
                                   m_info.toString(),
                                   other.info().toString()));
    }
    IMG_TYPED_CALL_2TYPE(binaryOp_Impl, m_info, other.info(), other, op)
    return *this;
  }

  // similar to unaryTypedOp, if you already know the type of current img and other img and have function like
  // double someBinaryOp(double current, int8_t otherVoxel);
  // you can use the typed version which generate less code:
  // img.binaryTypedOp<double, int8_t>(other, someBinaryOp);
  // **note** throw ZException if type don't match
  template<typename TVoxel, typename TVoxelOther, typename CustomBinaryOp>
  ZImg& typedBinaryOperation(const ZImg& other, const CustomBinaryOp& op)
  {
    if (!isType<TVoxel>() || !other.isType<TVoxelOther>()) {
      throw ZException("Call typedBinaryOperation with wrong type");
    }
    if (!isSameSize(other)) {
      throw ZException(fmt::format("img binary operation requires same size img as input: this <{}>, other <{}>",
                                   m_info.toString(),
                                   other.info().toString()));
    }
    binaryOp_Impl<TVoxel, TVoxelOther>(other, op);
    return *this;
  }

  // img coordinate
  // convert idx to coord
  // **note** if info is empty, throw ZException
  // for out of bound idx, this return a coord with only last dimension (Dimension::L) invalid, which can be seen
  // as virtual extension of img
  static ZVoxelCoordinate indexToCoord(index_t idx, const ZImgInfo& info);

  // result is undefined for invalid coord
  // if only last dimension of coord is invalid, result can still be meaningful
  static index_t coordToIndex(const ZVoxelCoordinate& coord, const ZImgInfo& info);

  [[nodiscard]] inline ZVoxelCoordinate indexToCoord(index_t idx) const
  {
    return indexToCoord(idx, m_info);
  }

  [[nodiscard]] inline index_t coordToIndex(const ZVoxelCoordinate& coord) const
  {
    return coordToIndex(coord, m_info);
  }

  // coord of one voxel pass each dimension
  [[nodiscard]] inline ZVoxelCoordinate endCoord() const
  {
    return ZVoxelCoordinate(m_info.width, m_info.height, m_info.depth, m_info.numChannels, m_info.numTimes);
  }

  // max valid coord, **note** throw ZException for empty img
  [[nodiscard]] inline ZVoxelCoordinate maxCoord() const
  {
    if (isEmpty()) {
      throw ZException("No max coord for empty img");
    }
    return ZVoxelCoordinate(m_info.width - 1,
                            m_info.height - 1,
                            m_info.depth - 1,
                            m_info.numChannels - 1,
                            m_info.numTimes - 1);
  }

  // coord will always be invalid if img is empty
  [[nodiscard]] inline bool isCoordValid(const ZVoxelCoordinate& coord) const
  {
    return !isEmpty() && coord.allGreaterEqual(0) && coord.allLessThan(endCoord());
  }

  // coord of first voxel with max img value
  template<typename TValue>
  ZVoxelCoordinate firstMaxValueCoord(TValue& max, const ZImgRegion& region = ZImgRegion()) const
  {
    ZVoxelCoordinate res;
    ZImgRegion rgn = region;
    if (rgn.isEmpty() || !rgn.isValid(m_info)) {
      throw ZException(fmt::format("Try to find max value location of img <{}> within invalid region <{}>",
                                   m_info.toString(),
                                   rgn.toString()));
    }
    rgn.resolveRegionEnd(m_info);
    IMG_TYPED_CALL(firstMaxValueCoord_Impl, m_info, res, max, rgn)
    return res;
  }

  // coord of all voxels with max img value
  template<typename TValue>
  std::vector<ZVoxelCoordinate> maxValueCoords(TValue& max, const ZImgRegion& region = ZImgRegion()) const
  {
    std::vector<ZVoxelCoordinate> res;
    ZImgRegion rgn = region;
    if (rgn.isEmpty() || !rgn.isValid(m_info)) {
      throw ZException(fmt::format("Try to find max value locations of img <{}> within invalid region <{}>",
                                   m_info.toString(),
                                   rgn.toString()));
    }
    rgn.resolveRegionEnd(m_info);
    IMG_TYPED_CALL(maxValueCoords_Impl, m_info, res, max, rgn)
    return res;
  }

  // get value as type at coordinate
  // **throw ZException** if coord is invalid or img is empty
  // note: this might be slow because we need to find the correct data type and check if coordinate is valid
  // if you know the data type and don't need to check coord, use data<> function
  template<typename TValue = double>
  TValue value(const ZVoxelCoordinate& coord) const
  {
    if (isEmpty()) {
      throw ZException(fmt::format("Can not get voxel value of empty img <{}>", m_info.toString()));
    }
    if (isCoordValid(coord)) {
      IMG_RETURN_TYPED_CALL(value_Impl, m_info, coord)
    } else {
      throw ZException(fmt::format("value: Invalid coordinate {} of img <{}>", coord.toString(), m_info.toString()));
    }
  }

  // overload
  template<typename TValue = double>
  TValue value(size_t x, size_t y, size_t z, size_t c = 0, size_t t = 0) const
  {
    if (isEmpty()) {
      throw ZException(fmt::format("Can not get voxel value of empty img <{}>", m_info.toString()));
    }
    if (x < m_info.width && y < m_info.height && z < m_info.depth && c < m_info.numChannels && t < m_info.numTimes) {
      IMG_RETURN_TYPED_CALL(value_Impl, m_info, x, y, z, c, t)
    } else {
      throw ZException(
        fmt::format("value: Invalid coordinate ({},{},{},{},{}) of img <{}>", x, y, z, c, t, m_info.toString()));
    }
  }

  // overload
  template<typename TValue = double>
  TValue value(size_t idx) const
  {
    if (isEmpty()) {
      throw ZException(fmt::format("Can not get voxel value of empty img <{}>", m_info.toString()));
    }
    if (idx < voxelNumber()) {
      IMG_RETURN_TYPED_CALL(value_Impl, m_info, idx)
    } else {
      throw ZException(fmt::format("value: Invalid voxel idx {} of img <{}>", idx, m_info.toString()));
    }
  }

  // **no throw** version of get value as type at coordinate, coord is always valid because we will pad img
  // for empty img, return 0
  // padValue is only used when padOption is PadOption::Constant, padValue will be cast to img voxel type
  template<typename TValue = double>
  TValue valueWithPad(const ZVoxelCoordinate& coord,
                      PadOption padOption = PadOption::Constant,
                      TValue padValue = TValue(0)) const
  {
    if (isEmpty()) {
      return 0;
    }
    IMG_RETURN_TYPED_CALL(valueWithPad_Impl, m_info, coord, padOption, padValue)
  }

  // overload
  template<typename TValue = double>
  inline TValue valueWithPad(index_t x,
                             index_t y,
                             index_t z,
                             index_t c = 0,
                             index_t t = 0,
                             PadOption padOption = PadOption::Constant,
                             TValue padValue = TValue(0)) const
  {
    return valueWithPad(ZVoxelCoordinate(x, y, z, c, t), padOption, padValue);
  }

  // set value of coord to value, value will be cast to current img type before set
  // **throw ZException** if coord is invalid or img is empty
  template<typename TValue>
  void setValue(TValue value, const ZVoxelCoordinate& coord)
  {
    if (isEmpty()) {
      throw ZException(fmt::format("Can not set voxel value to empty img <{}>", m_info.toString()));
    }
    if (isCoordValid(coord)) {
      IMG_TYPED_CALL(setValue_Impl, m_info, value, coord)
    } else {
      throw ZException(fmt::format("setValue: Invalid coordinate {} of img <{}>", coord.toString(), m_info.toString()));
    }
  }

  // overload
  template<typename TValue>
  void setValue(TValue value, size_t x, size_t y, size_t z, size_t c = 0, size_t t = 0)
  {
    if (isEmpty()) {
      throw ZException(fmt::format("Can not set voxel value to empty img <{}>", m_info.toString()));
    }
    if (x < m_info.width && y < m_info.height && z < m_info.depth && c < m_info.numChannels && t < m_info.numTimes) {
      IMG_TYPED_CALL(setValue_Impl, m_info, value, x, y, z, c, t)
    } else {
      throw ZException(
        fmt::format("setValue: Invalid coordinate ({},{},{},{},{}) of img <{}>", x, y, z, c, t, m_info.toString()));
    }
  }

  // overload
  template<typename TValue>
  void setValue(TValue value, size_t idx)
  {
    if (isEmpty()) {
      throw ZException(fmt::format("Can not set voxel value to empty img <{}>", m_info.toString()));
    }
    if (idx < voxelNumber()) {
      IMG_TYPED_CALL(setValue_Impl, m_info, value, idx)
    } else {
      throw ZException(fmt::format("value: Invalid voxel idx {} of img <{}>", idx, m_info.toString()));
    }
  }

  // similar to setValue, instead of throw ZException in case of error, this function do nothing and return false
  // return true if succeed
  template<typename TValue>
  bool setValueNoThrow(TValue value, const ZVoxelCoordinate& coord)
  {
    if (isCoordValid(coord)) {
      IMG_TYPED_CALL(setValue_Impl, m_info, value, coord)
      return true;
    }
    return false;
  }

  // overload
  template<typename TValue>
  bool setValueNoThrow(TValue value, size_t x, size_t y, size_t z, size_t c = 0, size_t t = 0)
  {
    if (!isEmpty() && x < m_info.width && y < m_info.height && z < m_info.depth && c < m_info.numChannels &&
        t < m_info.numTimes) {
      IMG_TYPED_CALL(setValue_Impl, m_info, value, x, y, z, c, t)
      return true;
    }
    return false;
  }

  // overload
  template<typename TValue>
  bool setValueNoThrow(TValue value, size_t idx)
  {
    if (!isEmpty() && idx < voxelNumber()) {
      IMG_TYPED_CALL(setValue_Impl, m_info, value, idx)
      return true;
    }
    return false;
  }

  // from alpha pre-multiplied color to normal color, assume last channel is alpha channel
  ZImg& correctPreMultipliedColor();

  static ZImg fromQImage(const QImage& image);

  [[nodiscard]] double sum() const;

#ifdef _NEUTUBE_
  // only for interface with zstack
  void releaseTimeData(size_t t)
  {
    m_data[t] = nullptr;
  }
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
  void cropWithPad_Impl(ZImg& res,
                        const ZVoxelCoordinate& startCoord,
                        const ZVoxelCoordinate& endCoord,
                        PadOption padOption,
                        TVoxel padValue) const;

  template<typename TVoxel>
  void fill_Impl(TVoxel value);

  template<typename TVoxel>
  void fillRandom_Impl();

  template<typename TVoxel>
  [[nodiscard]] double sum_Impl() const;

  template<typename TVoxel, typename TVoxelImg>
  void pasteImg_Impl(const ZImg& img, const ZVoxelCoordinate& start);

  template<typename TVoxel, typename TVoxelImg>
  void pasteImgMax_Impl(const ZImg& img, const ZVoxelCoordinate& start);

  template<typename TVoxel>
  static ZImg combine_Impl(const std::vector<const ZImg*>& imgs, ImgMergeMode mode);

  template<typename TVoxel, typename TDesVoxel>
  static void convert_Impl(bool normalize, const ZImg* src, ZImg* des)
  {
    TVoxel minv;
    TVoxel maxv;
    if (normalize) {
      src->computeMinMax(minv, maxv);
    } else {
      minv = src->dataRangeMin<TVoxel>();
      maxv = src->dataRangeMax<TVoxel>();
    }
    // LOG(INFO) << minv << " " << maxv;
    scale_Impl<TVoxel, TDesVoxel>(minv, maxv, src, des);
  }

  template<typename TVoxel, typename TDesVoxel>
  static void scale_Impl(TVoxel minData, TVoxel maxData, const ZImg* src, ZImg* des)
  {
    CHECK(src->isType<TVoxel>());
    CHECK(des->isType<TDesVoxel>());
    TDesVoxel dataRangeMin = std::numeric_limits<TDesVoxel>::min();
    TDesVoxel dataRangeMax = std::numeric_limits<TDesVoxel>::max();
    if (des->voxelFormat() == VoxelFormat::Float) {
      dataRangeMin = TDesVoxel(0.0);
      dataRangeMax = TDesVoxel(1.0);
    }

    if (minData == maxData) {
      if (src->voxelFormat() == VoxelFormat::Float) {
        minData = TVoxel(0.0);
        maxData = TVoxel(1.0);
      } else {
        minData = std::numeric_limits<TVoxel>::min();
        maxData = std::numeric_limits<TVoxel>::max();
      }
    }

    if (src->voxelFormat() != VoxelFormat::Float && std::is_same_v<TVoxel, TDesVoxel> &&
        dataRangeMin == TDesVoxel(minData) && dataRangeMax == TDesVoxel(maxData)) {
      if (src != des) {
        for (size_t t = 0; t < src->numTimes(); ++t) {
          std::memcpy(des->timeData(t), src->timeData(t), src->timeByteNumber());
        }
      }
      return;
    }

    // use colormap
    if constexpr (sizeof(TVoxel) <= 2) { // can not be float
      std::vector<TDesVoxel> colormap;
      buildScaleColormap(minData, maxData, dataRangeMin, dataRangeMax, colormap);
      TVoxel colormapMin = std::numeric_limits<TVoxel>::min();

      for (size_t t = 0; t < src->numTimes(); ++t) {
        for (size_t c = 0; c < src->numChannels(); ++c) {
          const TVoxel* data = src->channelData<TVoxel>(c, t);
          TDesVoxel* desData = des->channelData<TDesVoxel>(c, t);
          for (size_t v = 0; v < src->channelVoxelNumber(); ++v) {
            desData[v] = colormap[data[v] - colormapMin];
          }
        }
      }
    } else {
      for (size_t t = 0; t < src->numTimes(); ++t) {
        for (size_t c = 0; c < src->numChannels(); ++c) {
          const TVoxel* data = src->channelData<TVoxel>(c, t);
          TDesVoxel* desData = des->channelData<TDesVoxel>(c, t);
          for (size_t v = 0; v < src->channelVoxelNumber(); ++v) {
            if (data[v] <= minData) {
              desData[v] = dataRangeMin;
            } else if (data[v] >= maxData) {
              desData[v] = dataRangeMax;
            } else {
              desData[v] = (data[v] - minData) * 1.0 / (maxData - minData) * dataRangeMax + dataRangeMin;
            }
          }
        }
      }
    }
  }

  template<typename TVoxel, typename TDesVoxel>
  static void buildScaleColormap(TVoxel minData,
                                 TVoxel maxData,
                                 TDesVoxel desDataRangeMin,
                                 TDesVoxel desDataRangeMax,
                                 std::vector<TDesVoxel>& res)
  {
    if constexpr (sizeof(TVoxel) <= 2) {
      TVoxel srcDataRangeMin = std::numeric_limits<TVoxel>::min();
      TVoxel srcDataRangeMax = std::numeric_limits<TVoxel>::max();
      res.resize(srcDataRangeMax - srcDataRangeMin + 1);
      for (TVoxel v = srcDataRangeMin; v < srcDataRangeMax; ++v) {
        if (v <= minData) {
          res[v - srcDataRangeMin] = desDataRangeMin;
        } else if (v >= maxData) {
          res[v - srcDataRangeMin] = desDataRangeMax;
        } else {
          res[v - srcDataRangeMin] = (v - minData) * 1.0 / (maxData - minData) * desDataRangeMax + desDataRangeMin;
        }
      }
      res[srcDataRangeMax - srcDataRangeMin] = desDataRangeMax;
    }
  }

  template<typename TVoxel>
  ZImg& normalize_Impl();

  template<typename TVoxel, typename TDesVoxel>
  void cast_Impl(ZImg& res) const;

  template<typename TVoxel>
  void resize_Impl(ZImg& res,
                   Interpolant interpolant,
                   bool antialiasing,
                   bool antialiasingForNearest,
                   bool useMultithreading = true) const;

  template<typename TVoxel>
  void
  blockDownsampled_Impl(ZImg& res, size_t blockWidth, size_t blockHeight, size_t blockDepth, ImgMergeMode mode) const;

  template<typename TVoxel, typename TValue>
  void computeMinMax_Impl(TValue& minV, TValue& maxV) const;

  template<typename TVoxel, typename TScalar>
  void addScalar_Impl(TScalar scalar)
  {
    for (size_t t = 0; t < numTimes(); ++t) {
      TVoxel* data = timeData<TVoxel>(t);
      saturate_add(data, scalar, timeVoxelNumber(), data);
    }
  }

  template<typename TVoxel, typename TScalar>
  void subScalar_Impl(TScalar scalar)
  {
    for (size_t t = 0; t < numTimes(); ++t) {
      TVoxel* data = timeData<TVoxel>(t);
      saturate_sub(data, scalar, timeVoxelNumber(), data);
    }
  }

  template<typename TVoxel, typename TScalar>
  void mulScalar_Impl(TScalar scalar)
  {
    for (size_t t = 0; t < numTimes(); ++t) {
      TVoxel* data = timeData<TVoxel>(t);
      saturate_mul(data, scalar, timeVoxelNumber(), data);
    }
  }

  template<typename TVoxel, typename TScalar>
  void divScalar_Impl(TScalar scalar)
  {
    for (size_t t = 0; t < numTimes(); ++t) {
      TVoxel* data = timeData<TVoxel>(t);
      saturate_div(data, scalar, timeVoxelNumber(), data);
    }
  }

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

  template<typename TVoxel>
  void histogram_Impl(std::vector<size_t>& res, TVoxel minData, TVoxel maxData) const;

  template<typename TVoxel>
  inline void histogram_Impl(std::vector<size_t>& res) const
  {
    histogram_Impl(res, dataRangeMin<TVoxel>(), dataRangeMax<TVoxel>());
  }

  template<typename TVoxel, typename TMaskVoxel>
  void histogramMask_Impl(std::vector<size_t>& res, TVoxel minData, TVoxel maxData, const ZImg& mask) const;

  template<typename TVoxel, typename TMaskVoxel>
  inline void histogramMask_Impl(std::vector<size_t>& res, const ZImg& mask) const
  {
    histogramMask_Impl<TVoxel, TMaskVoxel>(res, dataRangeMin<TVoxel>(), dataRangeMax<TVoxel>(), mask);
  }

  template<typename TVoxel, typename GenericCustomUnaryOp>
  void unaryOp_Impl(const GenericCustomUnaryOp& op)
  {
    for (size_t t = 0; t < numTimes(); ++t) {
      TVoxel* data = timeData<TVoxel>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        data[v] = op(data[v]);
      }
    }
  }

  template<typename TVoxel, typename TVoxelOther, typename GenericCustomBinaryOp>
  void binaryOp_Impl(const ZImg& other, const GenericCustomBinaryOp& op)
  {
    for (size_t t = 0; t < numTimes(); ++t) {
      TVoxel* data = timeData<TVoxel>(t);
      const TVoxelOther* rhsData = other.timeData<TVoxelOther>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        data[v] = op(data[v], rhsData[v]);
      }
    }
  }

  template<typename TVoxel>
  void flip_Impl(Dimension dim);

  template<typename TVoxel>
  void reflect_Impl();

  template<typename TVoxel>
  void cumulativeSum_Impl(ZImg& res, Dimension dim) const;

  template<typename TVoxel>
  void blockSum_Impl(ZImg& res, size_t twidth, size_t theight, size_t tdepth) const;

  template<typename TVoxel>
  void blockSumPart_Impl(ZImg& res,
                         size_t twidth,
                         size_t theight,
                         size_t tdepth,
                         size_t xStart,
                         size_t yStart,
                         size_t zStart) const;

  template<typename TVoxel, typename TValue>
  void firstMaxValueCoord_Impl(ZVoxelCoordinate& res, TValue& max, const ZImgRegion& region) const
  {
    TVoxel maxValue = std::numeric_limits<TVoxel>::min();
    if (voxelFormat() == VoxelFormat::Float) {
      maxValue = std::numeric_limits<TVoxel>::lowest();
    }
    if (region.containsWholeTime(m_info)) {
      for (size_t t = region.start.t; t < static_cast<size_t>(region.end.t); ++t) {
        const TVoxel* data = timeData<TVoxel>(t);
        for (size_t v = 0; v < timeVoxelNumber(); ++v) {
          if (data[v] > maxValue) {
            maxValue = data[v];
            res = indexToCoord(v);
            res.t = t;
          }
        }
      }

    } else {
      for (size_t t = region.start.t; t < static_cast<size_t>(region.end.t); ++t) {
        for (size_t c = region.start.c; c < static_cast<size_t>(region.end.c); ++c) {
          for (size_t z = region.start.z; z < static_cast<size_t>(region.end.z); ++z) {
            for (size_t y = region.start.y; y < static_cast<size_t>(region.end.y); ++y) {
              for (size_t x = region.start.x; x < static_cast<size_t>(region.end.x); ++x) {
                TVoxel dat = *(data<TVoxel>(x, y, z, c, t));
                if (dat > maxValue) {
                  maxValue = dat;
                  res = ZVoxelCoordinate(x, y, z, c, t);
                }
              }
            }
          }
        }
      }
    }
    max = static_cast<TValue>(maxValue);
  }

  // coord of all voxels with max img value
  template<typename TVoxel, typename TValue>
  void maxValueCoords_Impl(std::vector<ZVoxelCoordinate>& res, TValue& max, const ZImgRegion& region) const
  {
    TVoxel maxValue = std::numeric_limits<TVoxel>::min();
    if (voxelFormat() == VoxelFormat::Float) {
      maxValue = std::numeric_limits<TVoxel>::lowest();
    }
    if (region.containsWholeTime(m_info)) {
      for (size_t t = region.start.t; t < static_cast<size_t>(region.end.t); ++t) {
        const TVoxel* data = timeData<TVoxel>(t);
        for (size_t v = 0; v < timeVoxelNumber(); ++v) {
          if (data[v] > maxValue) {
            maxValue = data[v];
            res.clear();
            ZVoxelCoordinate coord = indexToCoord(v);
            coord.t = t;
            res.push_back(coord);
          } else if (data[v] == maxValue) {
            ZVoxelCoordinate coord = indexToCoord(v);
            coord.t = t;
            res.push_back(coord);
          }
        }
      }
    } else {
      for (size_t t = region.start.t; t < static_cast<size_t>(region.end.t); ++t) {
        for (size_t c = region.start.c; c < static_cast<size_t>(region.end.c); ++c) {
          for (size_t z = region.start.z; z < static_cast<size_t>(region.end.z); ++z) {
            for (size_t y = region.start.y; y < static_cast<size_t>(region.end.y); ++y) {
              for (size_t x = region.start.x; x < static_cast<size_t>(region.end.x); ++x) {
                TVoxel dat = *(data<TVoxel>(x, y, z, c, t));
                if (dat > maxValue) {
                  maxValue = dat;
                  res.clear();
                  res.emplace_back(x, y, z, c, t);
                } else if (dat == maxValue) {
                  res.emplace_back(x, y, z, c, t);
                }
              }
            }
          }
        }
      }
    }
    max = static_cast<TValue>(maxValue);
  }

  template<typename TVoxel>
  [[nodiscard]] inline TVoxel value_Impl(const ZVoxelCoordinate& coord) const
  {
    return *(data<TVoxel>(coord));
  }

  template<typename TVoxel>
  [[nodiscard]] inline TVoxel value_Impl(size_t x, size_t y, size_t z, size_t c, size_t t) const
  {
    return *(data<TVoxel>(x, y, z, c, t));
  }

  template<typename TVoxel>
  [[nodiscard]] inline TVoxel value_Impl(size_t idx) const
  {
    return *(data<TVoxel>(idx));
  }

  template<typename TVoxel>
  inline TVoxel valueWithPad_Impl(const ZVoxelCoordinate& coord, PadOption padOption, TVoxel padValue) const
  {
    return (coord.allGreaterEqual(0) && coord.allLessThan(endCoord()))
             ? (*(data<TVoxel>(coord)))
             : outBoundValue<TVoxel>(coord, padOption, padValue);
  }

  template<typename TVoxel>
  inline void setValue_Impl(TVoxel value, const ZVoxelCoordinate& coord)
  {
    *(data<TVoxel>(coord)) = value;
  }

  template<typename TVoxel>
  inline void setValue_Impl(TVoxel value, size_t x, size_t y, size_t z, size_t c, size_t t)
  {
    *(data<TVoxel>(x, y, z, c, t)) = value;
  }

  template<typename TVoxel>
  inline void setValue_Impl(TVoxel value, size_t idx)
  {
    *(data<TVoxel>(idx)) = value;
  }

  template<typename TVoxel>
  void thresholdAbove_Impl(TVoxel threshold, ThresholdMode threMode, TVoxel outsideValue);

  template<typename TVoxel>
  void thresholdBelow_Impl(TVoxel threshold, ThresholdMode threMode, TVoxel outsideValue);

  template<typename TVoxel>
  void binarized_Impl(ZImg& res, TVoxel threshold, ThresholdMode threMode) const;

  template<typename TVoxel, typename GenericForegroundPredictor>
  void binarized_Impl(ZImg& res, const GenericForegroundPredictor& isForeground) const
  {
    for (size_t t = 0; t < numTimes(); ++t) {
      const TVoxel* data = timeData<TVoxel>(t);
      auto resData = res.timeData<uint8_t>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        if (isForeground(data[v])) {
          resData[v] = 1;
        }
      }
    }
  }

  template<typename TVoxel>
  void showContentAsQString_Impl(QString& res) const;

private:
  std::vector<uint8_t*> m_data;
  ZImgThumbernail m_thumbnail;
  ZImgInfo m_info;
  ZImgMetadata m_metadata;
  bool m_ownData = true;
};

template<typename TPixel>
void image2DWrite(const TPixel* data, size_t width, size_t height, const QString& filename)
{
  ZImg img;
  img.wrapData(const_cast<TPixel*>(data), width, height, 1);
  img.save(filename);
}

template<typename TPixel>
void image3DWrite(const TPixel* data, size_t width, size_t height, size_t depth, const QString& filename)
{
  ZImg img;
  img.wrapData(const_cast<TPixel*>(data), width, height, depth);
  img.save(filename);
}

void tag_invoke(const json::value_from_tag&, json::value& jv, const ZImg& img);

template<typename TVoxel>
void tag_invoke_img_Impl(json::value& jv, const ZImg& img);

ZImg tag_invoke(const json::value_to_tag<ZImg>&, const json::value& jv);

template<typename TVoxel>
ZImg tag_invoke_img_Impl(ZImg& img, const json::value& jv);

} // namespace nim
