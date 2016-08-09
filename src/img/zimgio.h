#pragma once

#include <vector>
#include <map>
#include <QString>
#include "zimg.h"
#include <set>

namespace nim {

class ZImgFormat;

class ZImgSliceProvider;

class ZImgIO
{
public:
  static ZImgIO& instance();

  ZImgIO();

  // if FileFormat is FileFormat::Unknown, use extension of filename to match correct reader or writer
  // otherwise file extension is ignored, which means you can write a tif file with '.raw' as extension...

  // note: will throw ZIOException or ZImgException if read error or an empty region is passed or image is empty or out of memory

  // only info
  void readInfo(const QString& filename, std::vector<ZImgInfo>& res,
                std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks = nullptr,
                std::vector<std::set<size_t>>* pyramidalRatios = nullptr,
                FileFormat format = FileFormat::Unknown);

  void readInfo(const QStringList& fileList, Dimension catDim, std::vector<ZImgInfo>& res,
                std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks = nullptr,
                FileFormat format = FileFormat::Unknown, bool expandXY = false);

  // only metadata
  void
  readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene = 0, FileFormat format = FileFormat::Unknown);

  // only thumbnail
  void readThumbnail(const QString& filename, ZImgThumbernail& thumbnail,
                     const ZImgRegion& region = ZImgRegion(),
                     size_t scene = 0,
                     FileFormat format = FileFormat::Unknown);

  // read everything
  void readImg(const QString& filename, ZImg& img,
               const ZImgRegion& region = ZImgRegion(),
               size_t scene = 0,
               size_t ratio = 1,
               FileFormat format = FileFormat::Unknown);

  // read image sequence, cat these imgs along dimension "catDim"
  // imgs should have same size in other dimensions and have same type
  // expandXY can not be true if catDim is Dimension::X or Dimension::Y
  void readImg(const QStringList& fileList, Dimension catDim, ZImg& img, size_t scene = 0,
               FileFormat format = FileFormat::Unknown, bool expandXY = false,
               bool expandWithMaxValue = false);

  void readImg(const QStringList& fileList, Dimension catDim, const ZImgRegion& regionIn, ZImg& img, size_t scene = 0,
               FileFormat format = FileFormat::Unknown, bool expandXY = false,
               bool expandWithMaxValue = false);

  void readImg(const ZImgSource& imgSource, ZImg& img);

  void writeImg(const QString& filename, const ZImg& img, FileFormat format = FileFormat::Unknown,
                Compression comp = Compression::AUTO);

  void writeImg(const QString& filename, const ZImgSliceProvider& img, FileFormat format = FileFormat::Unknown,
                Compression comp = Compression::AUTO);

  // qt style name filter for image open dialog
  void getQtReadNameFilter(QStringList& filters, QList<FileFormat>& formats) const;

  // write filter
  void getQtWriteNameFilter(QStringList& filters, QList<FileFormat>& formats, QList<Compression>& comps) const;

  //
  bool fileExtensionReadSupported(const QString& filename) const;

  bool fileExtensionWriteSupported(const QString& filename) const;

protected:
  std::vector<ZImgFormat*> getSupportedReader(const QString& filename) const;

  std::vector<ZImgFormat*> getSupportedWriter(const QString& filename) const;

private:
  std::map<FileFormat, std::unique_ptr<ZImgFormat>> m_ioFormats;
};

} // namespace nim

