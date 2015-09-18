#include "zimgio.h"

#include "zimgsliceprovider.h"
#include "zimgv3draw.h"
#include "zimgometiff.h"
#include "zimgtiff.h"
#include "zimgzeisslsm.h"
#include "zimgzeissczi.h"
#ifndef _NEUTUBE_
#include "zimgjpeg.h"
#include "zimgjpegxr.h"
#include "zimgpng.h"
#include "zimgfreeimage.h"
#include "zimgmetaimage.h"
#include "zimgitkimage.h"
#endif

namespace nim {

ZImgIO &ZImgIO::instance()
{
  static ZImgIO imgIO;
  return imgIO;
}

ZImgIO::ZImgIO()
{
  m_ioFormats[FileFormat::Vaa3DRaw] = std::make_unique<ZImgV3DRaw>();
  m_ioFormats[FileFormat::OmeTiff] = std::make_unique<ZImgOmeTiff>();
  m_ioFormats[FileFormat::Tiff] = std::make_unique<ZImgTiff>();
  m_ioFormats[FileFormat::ZeissLsm] = std::make_unique<ZImgZeissLsm>();
  m_ioFormats[FileFormat::ZeissCZI] = std::make_unique<ZImgZeissCZI>();
#ifndef _NEUTUBE_
  m_ioFormats[FileFormat::Jpeg] = std::make_unique<ZImgJpeg>();
  m_ioFormats[FileFormat::JpegXR] = std::make_unique<ZImgJpegXR>();
  m_ioFormats[FileFormat::Png] = std::make_unique<ZImgPng>();
  m_ioFormats[FileFormat::FreeImage] = std::make_unique<ZImgFreeImage>();
  m_ioFormats[FileFormat::MetaImage] = std::make_unique<ZImgMetaImage>();
  m_ioFormats[FileFormat::ITKImage] = std::make_unique<ZImgITKImage>();
#endif
}

void ZImgIO::readInfo(const QString &filename, std::vector<ZImgInfo> &res,
                      std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> *subBlocks,
                      std::vector<std::set<size_t> > *pyramidalRatios,
                      FileFormat format)
{
  res.clear();
  QString error;

  if (format == FileFormat::Unknown) {
    std::vector<ZImgFormat*> readers = getSupportedReader(filename);
    if (readers.empty()) {
      error = QString("File %1 is not supported.").arg(filename);
    } else {
      for (size_t i=0; i<readers.size(); ++i) {
        try {
          std::vector<ZImgInfo> tmpInfo;
          if (subBlocks)
            subBlocks->clear();
          readers[i]->readInfo(filename, tmpInfo, subBlocks, pyramidalRatios);
          if (!tmpInfo.empty()) {
            tmpInfo.swap(res);
            return;
          } else {
            throw ZIOException("empty image");
          }
        }
        catch (const ZIOException & e) {
          error += QString("\nTry read file %1 as '%2' format, failed: %3 ").arg(filename).arg(readers[i]->fullName()).arg(e.what());
        }
      }
    }
  } else if (m_ioFormats.find(format) == m_ioFormats.end() || !m_ioFormats[format]->supportRead()) {
    error = QString("Read format '%1' is not supported").arg(m_ioFormats[format]->fullName());
  } else {
    try {
      std::vector<ZImgInfo> tmpInfo;
      if (subBlocks)
        subBlocks->clear();
      m_ioFormats[format]->readInfo(filename, tmpInfo, subBlocks, pyramidalRatios);
      if (!tmpInfo.empty()) {
        tmpInfo.swap(res);
        return;
      } else {
        throw ZIOException("empty image");
      }
    }
    catch (const ZIOException & e) {
      error = QString("Try read file %1 as '%2' format, failed: %3").arg(filename).arg(m_ioFormats[format]->fullName()).arg(e.what());
    }
  }

  throw ZIOException(error);
}

void ZImgIO::readInfo(const QStringList &fileList, Dimension catDim, std::vector<ZImgInfo> &res,
                      std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> *subBlocks,
                      FileFormat format, bool expandXY)
{
  if (fileList.size() == 1) {
    readInfo(fileList[0], res, subBlocks, nullptr, format);
    return;
  } else if (fileList.empty()) {
    throw ZIOException("Read sequence failed: empty file list");
  }

  if (catDim == Dimension::C) {
    assert(!subBlocks);   // not supported
  }

  readInfo(fileList[0], res, subBlocks, nullptr, format);
  if (res.empty()) {
    throw ZIOException("Read sequence failed: img 0 is empty");
  }
  if (expandXY) {
    assert(catDim != Dimension::X && catDim != Dimension::Y);
  }
  for (int i=1; i<fileList.size(); ++i) {
    std::vector<ZImgInfo> tmpInfo;
    std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> tmpSubBlocks;
    readInfo(fileList[i], tmpInfo, subBlocks ? &tmpSubBlocks : nullptr, nullptr, format);
    if (tmpInfo.empty()) {
      throw ZIOException(QString("Read sequence failed: img %1 is empty").arg(i));
    }
    // check whether number of scenes match
    if (tmpInfo.size() != res.size()) {
      throw ZIOException("Read sequence failed: images have different number of scenes");
    }
    for (size_t s=0; s<res.size(); ++s) {
      // check whether type match
      if (!tmpInfo[s].isSameType(res[s])) {
        throw ZIOException(QString("Read sequence failed: image type don't match, can not cat Img %1 <%2> to Img 0 <%3>")
                           .arg(i).arg(tmpInfo[s].toQString()).arg(res[s].toQString()));
      }
      // check whether dimension size match
      for (size_t d=0; d<res[s].numDimensions(); ++d) {
        if (expandXY) {
          if (d != enumToType<size_t>(Dimension::X) && d != enumToType<size_t>(Dimension::Y) && d != enumToType<size_t>(catDim) && res[s].size(d) != tmpInfo[s].size(d)) {
            throw ZIOException(QString("Read sequence failed: image dimension don't match, can not cat Img %1 <%2> to Img 0 <%3>")
                               .arg(i).arg(tmpInfo[s].toQString()).arg(res[s].toQString()));
          }
        } else {
          if (d != enumToType<size_t>(catDim) && res[s].size(d) != tmpInfo[s].size(d)) {
            throw ZIOException(QString("Read sequence failed: image dimension don't match, can not cat Img %1 <%2> to Img 0 <%3>")
                               .arg(i).arg(tmpInfo[s].toQString()).arg(res[s].toQString()));
          }
        }
        if (d == enumToType<size_t>(catDim)) {
          if (subBlocks) {
            for (size_t tsidx=0; tsidx < tmpSubBlocks[s].size(); ++tsidx) {
              switch (catDim) {
              case Dimension::X:
                tmpSubBlocks[s][tsidx]->x += res[s].size(d);
                break;
              case Dimension::Y:
                tmpSubBlocks[s][tsidx]->y += res[s].size(d);
                break;
              case Dimension::Z:
                tmpSubBlocks[s][tsidx]->z += res[s].size(d);
                break;
              case Dimension::T:
                tmpSubBlocks[s][tsidx]->t += res[s].size(d);
                break;
              default:
                assert(false);
                break;
              }
              subBlocks->at(s).push_back(tmpSubBlocks[s][tsidx]);
            }
          }
          res[s].setSize(catDim, res[s].size(d) + tmpInfo[s].size(d));
        }
      }
      // get final width and height
      if (expandXY) {
        res[s].width = std::max(res[s].width, tmpInfo[s].width);
        res[s].height = std::max(res[s].height, tmpInfo[s].height);
      }
    }
  }
}

void ZImgIO::readMetadata(const QString &filename, ZImgMetadata &meta, size_t scene, FileFormat format)
{
  meta.clear();
  QString error;

  if (format == FileFormat::Unknown) {
    std::vector<ZImgFormat*> readers = getSupportedReader(filename);
    if (readers.empty()) {
      error = QString("File %1 is not supported.").arg(filename);
    } else {
      for (size_t i=0; i<readers.size(); ++i) {
        try {
          ZImgMetadata tmpMeta;
          readers[i]->readMetadata(filename, tmpMeta, scene);
          tmpMeta.swap(meta);
          return;
        }
        catch (const ZIOException & e) {
          error += QString("\nTry read file %1 as '%2' format, failed: %3 ").arg(filename).arg(readers[i]->fullName()).arg(e.what());
        }
      }
    }
  } else if (m_ioFormats.find(format) == m_ioFormats.end() || !m_ioFormats[format]->supportRead()) {
    error = QString("Read format '%1' is not supported").arg(m_ioFormats[format]->fullName());
  } else {
    try {
      ZImgMetadata tmpMeta;
      m_ioFormats[format]->readMetadata(filename, tmpMeta, scene);
      tmpMeta.swap(meta);
      return;
    }
    catch (const ZIOException & e) {
      error = QString("Try read file %1 as '%2' format, failed: %3").arg(filename).arg(m_ioFormats[format]->fullName()).arg(e.what());
    }
  }

  throw ZIOException(error);
}

void ZImgIO::readThumbnail(const QString &filename, ZImgThumbernail &thumbnail, const ZImgRegion &region, size_t scene, FileFormat format)
{
  thumbnail.clear();
  QString error;

  if (format == FileFormat::Unknown) {
    std::vector<ZImgFormat*> readers = getSupportedReader(filename);
    if (readers.empty()) {
      error = QString("File %1 is not supported.").arg(filename);
    } else {
      for (size_t i=0; i<readers.size(); ++i) {
        try {
          ZImgThumbernail tmpThumbnail;
          readers[i]->readThumbnail(filename, tmpThumbnail, region, scene);
          tmpThumbnail.swap(thumbnail);
          return;
        }
        catch (const ZIOException & e) {
          error += QString("\nTry read file %1 as '%2' format, failed: %3 ").arg(filename).arg(readers[i]->fullName()).arg(e.what());
        }
      }
    }
  } else if (m_ioFormats.find(format) == m_ioFormats.end() || !m_ioFormats[format]->supportRead()) {
    error = QString("Read format '%1' is not supported").arg(m_ioFormats[format]->fullName());
  } else {
    try {
      ZImgThumbernail tmpThumbnail;
      m_ioFormats[format]->readThumbnail(filename, tmpThumbnail, region, scene);
      tmpThumbnail.swap(thumbnail);
      return;
    }
    catch (const ZIOException & e) {
      error = QString("Try read file %1 as '%2' format, failed: %3").arg(filename).arg(m_ioFormats[format]->fullName()).arg(e.what());
    }
  }

  throw ZIOException(error);
}

void ZImgIO::readImg(const QString &filename, ZImg &img, const ZImgRegion &region, size_t scene, size_t ratio, FileFormat format)
{
  img.clear();
  QString error;

  if (format == FileFormat::Unknown) {
    std::vector<ZImgFormat*> readers = getSupportedReader(filename);
    if (readers.empty()) {
      error = QString("File %1 is not supported.").arg(filename);
    } else {
      for (size_t i=0; i<readers.size(); ++i) {
        try {
          ZImg tmpImg;
          readers[i]->readImg(filename, tmpImg, region, scene, ratio);
          tmpImg.swap(img);
          return;
        }
        catch (const ZIOException & e) {
          error += QString("\nTry read file %1 as '%2' format, failed: %3 ").arg(filename).arg(readers[i]->fullName()).arg(e.what());
        }
      }
    }
  } else if (m_ioFormats.find(format) == m_ioFormats.end() || !m_ioFormats[format]->supportRead()) {
    error = QString("Read format '%1' is not supported").arg(m_ioFormats[format]->fullName());
  } else {
    try {
      ZImg tmpImg;
      m_ioFormats[format]->readImg(filename, tmpImg, region, scene, ratio);
      tmpImg.swap(img);
      return;
    }
    catch (const ZIOException & e) {
      error = QString("Try read file %1 as '%2' format, failed: %3").arg(filename).arg(m_ioFormats[format]->fullName()).arg(e.what());
    }
  }

  throw ZIOException(error);
}

void ZImgIO::readImg(const QStringList &fileList, Dimension catDim, ZImg &img, size_t scene,
                     FileFormat format, bool expandXY, bool expandWithMaxValue)
{
  if (fileList.size() == 1) {
    readImg(fileList[0], img, ZImgRegion(), scene, 1, format);
    return;
  }

  std::vector<ZImgInfo> infos;
  readInfo(fileList, catDim, infos, nullptr, format, expandXY);
  if (scene >= infos.size()) {
    throw ZIOException("invalid scene for image sequence");
  }
  ZImgInfo& info = infos[scene];

  std::vector<ZImg> imgs(fileList.size());
  for (size_t i=0; i<imgs.size(); ++i) {
    imgs[i].load(fileList[i], scene, format);
    if (expandXY && (imgs[i].width() < info.width || imgs[i].height() < info.height)) {
      int widthPadBefore = (info.width - imgs[i].width()) / 2;
      int widthPadAfter = info.width - imgs[i].width() - widthPadBefore;
      int heightPadBefore = (info.height - imgs[i].height()) / 2;
      int heightPadAfter = info.height - imgs[i].height() - heightPadBefore;
      if (info.voxelFormat == VoxelFormat::Float) {
        double min;
        double max;
        imgs[i].computeMinMax(min, max);
        imgs[i] = imgs[i].cropWithPad(ZVoxelCoordinate(-widthPadBefore, -heightPadBefore, 0, 0, 0),
                                      imgs[i].endCoord() + ZVoxelCoordinate(widthPadAfter, heightPadAfter),
                                      PadOption::Constant, expandWithMaxValue ? max : min);
      } else if (info.voxelFormat == VoxelFormat::Unsigned) {
        uint64_t min;
        uint64_t max;
        imgs[i].computeMinMax(min, max);
        imgs[i] = imgs[i].cropWithPad(ZVoxelCoordinate(-widthPadBefore, -heightPadBefore, 0, 0, 0),
                                      imgs[i].endCoord() + ZVoxelCoordinate(widthPadAfter, heightPadAfter),
                                      PadOption::Constant, expandWithMaxValue ? max : min);
      } else {
        int64_t min;
        int64_t max;
        imgs[i].computeMinMax(min, max);
        imgs[i] = imgs[i].cropWithPad(ZVoxelCoordinate(-widthPadBefore, -heightPadBefore, 0, 0, 0),
                                      imgs[i].endCoord() + ZVoxelCoordinate(widthPadAfter, heightPadAfter),
                                      PadOption::Constant, expandWithMaxValue ? max : min);
      }
    }
  }
  if (imgs.size() == 1) {
    img.swap(imgs[0]);
  } else if (imgs.size() > 1) {
    ZImg catImg = ZImg::cat(imgs, catDim);
    img.swap(catImg);
  }
}

void ZImgIO::readImg(const QStringList &fileList, Dimension catDim, const ZImgRegion &regionIn, ZImg &img, size_t scene,
                     FileFormat format, bool expandXY, bool expandWithMaxValue)
{
  if (fileList.size() == 1) {
    readImg(fileList[0], img, regionIn, scene, 1, format);
    return;
  }

  if (regionIn.isDefault()) {
    readImg(fileList, catDim, img, scene, format, expandXY, expandWithMaxValue);
    return;
  }

  std::vector<ZImgInfo> infos;
  readInfo(fileList, catDim, infos, nullptr, format, expandXY);
  if (infos.size() <= scene) {
    throw ZIOException("invalid scene for image sequence");
  }
  ZImgInfo& info = infos[scene];
  if (regionIn.isEmpty() || !regionIn.isValid(info)) {
    throw ZIOException(QString("Invalid image region. Image info: '%1', region: '%2'").arg(info.toQString()).arg(regionIn.toQString()));
  }
  ZImgRegion region = regionIn;
  region.resolveRegionEnd(info);

  std::vector<ZImg> imgs;
  size_t sliceCatDimStart = 0;
  size_t sliceCatDimEnd = 0;
  for (size_t i=0; i<imgs.size(); ++i) {
    std::vector<ZImgInfo> sliceInfos;
    readInfo(fileList[i], sliceInfos, nullptr, nullptr, format);
    ZImgInfo &sliceInfo = sliceInfos[scene];
    sliceCatDimStart = i == 0 ? 0 : sliceCatDimEnd;
    sliceCatDimEnd = sliceCatDimStart + sliceInfo.size(catDim);
    if (sliceCatDimStart >= static_cast<size_t>(region.end[enumToUnderlyingType(catDim)]) || sliceCatDimEnd <= static_cast<size_t>(region.start[enumToUnderlyingType(catDim)])) {
      continue;
    }
    ZImgRegion sliceRegion = region;
    if (static_cast<size_t>(region.start[enumToUnderlyingType(catDim)]) > sliceCatDimStart) {
      sliceRegion.start[enumToUnderlyingType(catDim)] = static_cast<size_t>(region.start[enumToUnderlyingType(catDim)]) - sliceCatDimStart;
    } else {
      sliceRegion.start[enumToUnderlyingType(catDim)] = 0;
    }
    if (static_cast<size_t>(region.end[enumToUnderlyingType(catDim)]) < sliceCatDimEnd) {
      sliceRegion.end[enumToUnderlyingType(catDim)] = sliceInfo.size(catDim) - (sliceCatDimEnd - static_cast<size_t>(region.end[enumToUnderlyingType(catDim)]));
    } else {
      sliceRegion.end[enumToUnderlyingType(catDim)] = sliceInfo.size(catDim);
    }

    imgs.push_back(ZImg());
    if (expandXY && (sliceInfo.width < info.width || sliceInfo.height < info.height)) {
      int widthPadBefore = (info.width - sliceInfo.width) / 2;
      int heightPadBefore = (info.height - sliceInfo.height) / 2;
      ZImgRegion sliceRegionFullXY = sliceRegion;
      sliceRegionFullXY.start.x = 0;
      sliceRegionFullXY.end.x = -1;
      sliceRegionFullXY.start.y = 0;
      sliceRegionFullXY.end.y = -1;
      imgs[imgs.size()-1].load(fileList[i], sliceRegionFullXY, scene, format);
      if (info.voxelFormat == VoxelFormat::Float) {
        double min;
        double max;
        imgs[i].computeMinMax(min, max);
        imgs[i] = imgs[i].cropWithPad(ZVoxelCoordinate(-widthPadBefore + sliceRegion.start.x,
                                                       -heightPadBefore + sliceRegion.start.y,
                                                       0),
                                      ZVoxelCoordinate(-widthPadBefore + sliceRegion.end.x,
                                                       -heightPadBefore + sliceRegion.end.y,
                                                       imgs[i].depth()),
                                      PadOption::Constant, expandWithMaxValue ? max : min);
      } else if (info.voxelFormat == VoxelFormat::Unsigned) {
        uint64_t min;
        uint64_t max;
        imgs[i].computeMinMax(min, max);
        imgs[i] = imgs[i].cropWithPad(ZVoxelCoordinate(-widthPadBefore + sliceRegion.start.x,
                                                       -heightPadBefore + sliceRegion.start.y,
                                                       0),
                                      ZVoxelCoordinate(-widthPadBefore + sliceRegion.end.x,
                                                       -heightPadBefore + sliceRegion.end.y,
                                                       imgs[i].depth()),
                                      PadOption::Constant, expandWithMaxValue ? max : min);
      } else {
        int64_t min;
        int64_t max;
        imgs[i].computeMinMax(min, max);
        imgs[i] = imgs[i].cropWithPad(ZVoxelCoordinate(-widthPadBefore + sliceRegion.start.x,
                                                       -heightPadBefore + sliceRegion.start.y,
                                                       0),
                                      ZVoxelCoordinate(-widthPadBefore + sliceRegion.end.x,
                                                       -heightPadBefore + sliceRegion.end.y,
                                                       imgs[i].depth()),
                                      PadOption::Constant, expandWithMaxValue ? max : min);
      }
    } else {
      imgs[imgs.size()-1].load(fileList[i], sliceRegion, scene, format);
    }
  }
  if (imgs.size() == 1) {
    img.swap(imgs[0]);
  } else if (imgs.size() > 1) {
    ZImg catImg = ZImg::cat(imgs, catDim);
    img.swap(catImg);
  }
}

void ZImgIO::readImg(const ZImgSource &imgSource, ZImg &img)
{
  if (imgSource.filenames.size() == 1) {
    readImg(imgSource.filenames[0], img, imgSource.region, imgSource.scene, 1, imgSource.format);
  } else if (imgSource.filenames.size() > 1) {
    readImg(imgSource.filenames, imgSource.catDim, imgSource.region, img, imgSource.scene,
            imgSource.format, imgSource.expandXY, imgSource.expandWithMaxValue);
  } else {
    throw ZIOException("invalid image source");
  }
}

void ZImgIO::writeImg(const QString &filename, const ZImg &img, FileFormat format, Compression comp)
{
  if (img.isEmpty()) {
    throw ZIOException("Can not write empty image.");
  }

  QString error;
  if (format == FileFormat::Unknown) {
    std::vector<ZImgFormat*> writers = getSupportedWriter(filename);
    if (writers.empty()) {
      error = QString("Write file %1 is not supported.").arg(filename);
    } else {
      for (size_t i=0; i<writers.size(); ++i) {
        try {
          writers[i]->writeImg(filename, img, comp);
          return;
        }
        catch (const ZIOException & e) {
          error += QString("Try write %1 as '%2' format, failed: %3 ").arg(filename).arg(writers[i]->fullName()).arg(e.what());
        }
      }
    }
  } else if (m_ioFormats.find(format) == m_ioFormats.end() || !m_ioFormats[format]->supportWrite()) {
    error = QString("Write format '%1' is not supported").arg(m_ioFormats[format]->fullName());
  } else {
    try {
      m_ioFormats[format]->writeImg(filename, img, comp);
      return;
    }
    catch (const ZIOException & e) {
      error = QString("Try write file %1 as '%2' format, failed: %3").arg(filename).arg(m_ioFormats[format]->fullName()).arg(e.what());
    }
  }

  throw ZIOException(error);
}

void ZImgIO::writeImg(const QString &filename, const ZImgSliceProvider &img, FileFormat format, Compression comp)
{
  if (img.imgInfo().isEmpty()) {
    throw ZIOException("Can not write empty image.");
  }

  QString error;
  if (format == FileFormat::Unknown) {
    std::vector<ZImgFormat*> writers = getSupportedWriter(filename);
    if (writers.empty()) {
      error = QString("Write file %1 is not supported.").arg(filename);
    } else {
      for (size_t i=0; i<writers.size(); ++i) {
        try {
          writers[i]->writeImg(filename, img, comp);
          return;
        }
        catch (const ZIOException & e) {
          error += QString("\nTry write %1 as '%2' format, failed: %3 ").arg(filename).arg(writers[i]->fullName()).arg(e.what());
        }
      }
    }
  } else if (m_ioFormats.find(format) == m_ioFormats.end() || !m_ioFormats[format]->supportWrite()) {
    error = QString("Write format '%1' is not supported").arg(m_ioFormats[format]->fullName());
  } else {
    try {
      m_ioFormats[format]->writeImg(filename, img, comp);
      return;
    }
    catch (const ZIOException & e) {
      error = QString("Try write file %1 as '%2' format, failed: %3").arg(filename).arg(m_ioFormats[format]->fullName()).arg(e.what());
    }
  }

  throw ZIOException(error);
}

void ZImgIO::getQtReadNameFilter(QStringList &filters, QList<FileFormat> &formats) const
{
  filters.clear();
  formats.clear();

  QString all = "Images";
  QStringList lst;
  for (auto it = m_ioFormats.cbegin(); it != m_ioFormats.cend(); ++it) {
    if (it->second->supportRead()) {
      QString filter = QString("*.") + it->second->extensions().join(" *.");
      lst.push_back(filter);

      filter = it->second->fullName() + QString(" (*.") +
          it->second->extensions().join(" *.") + QString(")");
      filters.push_back(filter);
      formats.push_back(it->first);
    }
  }
  all += " (";
  all += lst.join(" ");
  all += ")";
  filters.prepend(all);
  formats.prepend(FileFormat::Unknown);
}

void ZImgIO::getQtWriteNameFilter(QStringList &filters, QList<FileFormat> &formats, QList<Compression> &comps) const
{
  filters.clear();
  formats.clear();
  comps.clear();
  for (auto it = m_ioFormats.cbegin(); it != m_ioFormats.cend(); ++it) {
#ifdef _QT4_
    if (it->second->supportWrite() && it->first != FileFormat::OmeTiff) {  // skip ome tiff since mac file dialog doesn't support double extension like .ome.tiff todo: find workaround
#else
    if (it->second->supportWrite()) {
#endif
      QString filter = it->second->fullName() + QString(" (*.") +
          it->second->extensions().join(" *.") + QString(")");
      if (it->first == FileFormat::OmeTiff || it->first == FileFormat::Tiff) {
        QString flt = QString("LZW Compressed ") + filter;  // todo: use custom dialog and set compression as option
        filters.push_back(flt);
        formats.push_back(it->first);
        comps.push_back(Compression::LZW);
        flt = QString("ADOBE DEFLATE Compressed ") + filter;
        filters.push_back(flt);
        formats.push_back(it->first);
        comps.push_back(Compression::ADOBE_DEFLATE);
      }
      if (it->first == FileFormat::MetaImage) {
        QString flt = QString("Compressed ") + filter;  // todo: use custom dialog and set compression as option
        filters.push_back(flt);
        formats.push_back(it->first);
        comps.push_back(Compression::AUTO);
      }
      filters.push_back(filter);
      formats.push_back(it->first);
      comps.push_back(Compression::NONE);
    }
  }
#ifdef _QT4_
  // add ome tiff
  std::map<FileFormat, ZImgFormat*>::const_iterator it = m_ioFormats.find(FileFormat::OmeTiff);
  QString filter = it->second->fullName() + QString(" (*.") +
      it->second->extensions().join(" *.") + QString(")");
  QString flt = QString("LZW Compressed ") + filter;  // todo: use custom dialog and set compression as option
  filters.push_back(flt);
  formats.push_back(it->first);
  comps.push_back(Compression::LZW);
  flt = QString("DEFLATE Compressed ") + filter;
  filters.push_back(flt);
  formats.push_back(it->first);
  comps.push_back(Compression::DEFLATE);
  filters.push_back(filter);
  formats.push_back(it->first);
  comps.push_back(Compression::NONE);
#endif
}

bool ZImgIO::fileExtensionReadSupported(const QString &filename) const
{
  for (auto it = m_ioFormats.cbegin(); it != m_ioFormats.cend(); ++it) {
    if (it->second->canRead(filename))
      return true;
  }
  return false;
}

bool ZImgIO::fileExtensionWriteSupported(const QString &filename) const
{
  for (auto it = m_ioFormats.cbegin(); it != m_ioFormats.cend(); ++it) {
    if (it->second->canWrite(filename))
      return true;
  }
  return false;
}

std::vector<ZImgFormat*> ZImgIO::getSupportedReader(const QString &filename) const
{
  std::vector<ZImgFormat*> res;
  for (auto it = m_ioFormats.cbegin(); it != m_ioFormats.cend(); ++it) {
    if (it->second->canRead(filename))
      res.push_back(it->second.get());
  }
  return res;
}

std::vector<ZImgFormat*> ZImgIO::getSupportedWriter(const QString &filename) const
{
  std::vector<ZImgFormat*> res;
  for (auto it = m_ioFormats.cbegin(); it != m_ioFormats.cend(); ++it) {
    if (it->second->canWrite(filename))
      res.push_back(it->second.get());
  }
  return res;
}

} // namespace
