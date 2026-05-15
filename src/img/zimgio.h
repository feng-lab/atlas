#pragma once

#include "zimg.h"
#include "zimgformat.h"
#include <map>

namespace nim {

class ZImgSliceProvider;

class ZImgBlockProvider;

class ZImgIO
{
public:
  // instance is thread_local so safe to use in multiple threads
  static ZImgIO& instance();

  // use new instance in case of nested call in the same thread
  ZImgIO();

  // if FileFormat is FileFormat::Unknown, use extension of filename to match correct reader or writer
  // otherwise file extension is ignored, which means you can write a tif file with '.raw' as extension...

  // note: will throw ZException if read error or an empty region is passed or image is empty or out of memory

  // only info
  void readInfos(const QString& filename,
                 std::vector<ZImgInfo>& res,
                 std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks = nullptr,
                 FileFormat format = FileFormat::Unknown);

  void readInfos(const QStringList& fileList,
                 Dimension catDim,
                 bool catScenes,
                 std::vector<ZImgInfo>& res,
                 std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks = nullptr,
                 FileFormat format = FileFormat::Unknown,
                 bool expandXY = true);

  void readInfo(const ZImgSource& imgSource,
                ZImgInfo& info,
                std::vector<std::shared_ptr<ZImgSubBlock>>* subBlocks = nullptr);

  //
  std::vector<std::vector<ZImgRegion>> getInternalSubRegions(const QString& filename,
                                                             FileFormat format = FileFormat::Unknown);

  // only metadata
  void readMetadata(const ZImgSource& imgSource, ZImgMetadata& meta);

  // only thumbnail
  void readThumbnail(const QString& filename,
                     ZImgThumbernail& thumbnail,
                     const ZImgRegion& region = ZImgRegion(),
                     size_t scene = 0,
                     FileFormat format = FileFormat::Unknown);

  // Read a complete image container by default. Use readImgPixelsOnly() or
  // ZImgReadOptions::pixelsOnly() for display/cache paths that do not need
  // metadata or thumbnail attachments.
  void readImg(const QString& filename,
               ZImg& img,
               const ZImgRegion& region = ZImgRegion(),
               size_t scene = 0,
               size_t xRatio = 1,
               size_t yRatio = 1,
               size_t zRatio = 1,
               FileFormat format = FileFormat::Unknown,
               const ZImgReadOptions& readOptions = ZImgReadOptions::complete());

  void readImgPixelsOnly(const QString& filename,
                         ZImg& img,
                         const ZImgRegion& region = ZImgRegion(),
                         size_t scene = 0,
                         size_t xRatio = 1,
                         size_t yRatio = 1,
                         size_t zRatio = 1,
                         FileFormat format = FileFormat::Unknown);

  // read image sequence, cat these imgs along dimension "catDim"
  // imgs should have same size in other dimensions and have same type
  // expandXY can not be true if catDim is Dimension::X or Dimension::Y
  void readImg(const QStringList& fileList,
               Dimension catDim,
               bool catScenes,
               ZImg& img,
               size_t scene = 0,
               size_t xRatio = 1,
               size_t yRatio = 1,
               size_t zRatio = 1,
               FileFormat format = FileFormat::Unknown,
               bool expandXY = true,
               bool expandWithMaxValue = false,
               const ZImgReadOptions& readOptions = ZImgReadOptions::complete());

  void readImg(const QStringList& fileList,
               Dimension catDim,
               bool catScenes,
               const ZImgRegion& regionIn,
               ZImg& img,
               size_t scene = 0,
               size_t xRatio = 1,
               size_t yRatio = 1,
               size_t zRatio = 1,
               FileFormat format = FileFormat::Unknown,
               bool expandXY = true,
               bool expandWithMaxValue = false,
               const ZImgReadOptions& readOptions = ZImgReadOptions::complete());

  void readImg(const ZImgSource& imgSource,
               ZImg& img,
               size_t xRatio = 1,
               size_t yRatio = 1,
               size_t zRatio = 1,
               const ZImgReadOptions& readOptions = ZImgReadOptions::complete());

  void
  readImgPixelsOnly(const ZImgSource& imgSource, ZImg& img, size_t xRatio = 1, size_t yRatio = 1, size_t zRatio = 1);

  void writeImg(const QString& filename,
                const ZImg& img,
                FileFormat format = FileFormat::Unknown,
                const ZImgWriteParameters& paras = ZImgWriteParameters());

  void writeImg(const QString& filename,
                const ZImgSliceProvider& img,
                FileFormat format = FileFormat::Unknown,
                const ZImgWriteParameters& paras = ZImgWriteParameters());

  void writeImg(const QString& filename,
                const ZImgBlockProvider& img,
                FileFormat format = FileFormat::Unknown,
                const ZImgWriteParameters& paras = ZImgWriteParameters());

  // qt style name filter for image open dialog
  void getQtReadNameFilter(QStringList& filters, std::vector<FileFormat>& formats) const;

  [[nodiscard]] QStringList readExtensions() const;

  // write filter
  void
  getQtWriteNameFilter(QStringList& filters, std::vector<FileFormat>& formats, std::vector<Compression>& comps) const;

  //
  [[nodiscard]] bool fileExtensionReadSupported(const QString& filename) const;

  [[nodiscard]] bool fileExtensionWriteSupported(const QString& filename) const;

protected:
  [[nodiscard]] std::vector<ZImgFormat*> getSupportedReader(const QString& filename) const;

  [[nodiscard]] std::vector<ZImgFormat*> getSupportedWriter(const QString& filename) const;

private:
  std::map<FileFormat, std::unique_ptr<ZImgFormat>> m_ioFormats;
};

} // namespace nim
