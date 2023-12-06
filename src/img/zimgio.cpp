#include "zimgio.h"

#include "zimgsliceprovider.h"
#include "zimgblockprovider.h"
#include "zimgv3draw.h"
#include "zimgometiff.h"
#include "zimgtiff.h"
#include "zimgzeisslsm.h"
#include "zimgzeissczi.h"
#include "zimgjpeg.h"
#include "zimgjpegxr.h"
#include "zimgpng.h"
#include "zimgfreeimage.h"
#include "zimgmetaimage.h"
#include "zimgitkimage.h"
#include "zimghdf5.h"
#include "zimgleica.h"
#include "zlog.h"
#include "zioutils.h"
#include <folly/futures/Future.h>
#include <folly/executors/CPUThreadPoolExecutor.h>

DEFINE_bool(zimg_use_multithreaded_image_sequence_reading,
            true,
            "Use multithreading for image sequence reading, default is true");

namespace nim {

ZImgIO& ZImgIO::instance()
{
  thread_local static ZImgIO imgIO;
  return imgIO;
}

ZImgIO::ZImgIO()
{
  m_ioFormats[FileFormat::Vaa3DRaw] = std::make_unique<ZImgV3DRaw>();
  m_ioFormats[FileFormat::OmeTiff] = std::make_unique<ZImgOmeTiff>();
  m_ioFormats[FileFormat::Tiff] = std::make_unique<ZImgTiff>();
  m_ioFormats[FileFormat::ZeissLsm] = std::make_unique<ZImgZeissLsm>();
  m_ioFormats[FileFormat::ZeissCZI] = std::make_unique<ZImgZeissCZI>();
  m_ioFormats[FileFormat::Jpeg] = std::make_unique<ZImgJpeg>();
  m_ioFormats[FileFormat::JpegXR] = std::make_unique<ZImgJpegXR>();
  m_ioFormats[FileFormat::Png] = std::make_unique<ZImgPng>();
  m_ioFormats[FileFormat::FreeImage] = std::make_unique<ZImgFreeImage>();
  m_ioFormats[FileFormat::MetaImage] = std::make_unique<ZImgMetaImage>();
  m_ioFormats[FileFormat::ITKImage] = std::make_unique<ZImgITKImage>();
  m_ioFormats[FileFormat::HDF5Img] = std::make_unique<ZImgHDF5>();
  m_ioFormats[FileFormat::Leica] = std::make_unique<ZImgLeica>();
}

void ZImgIO::readInfos(const QString& filename,
                       std::vector<ZImgInfo>& res,
                       std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                       FileFormat format)
{
  res.clear();
  QString error;

  if (format == FileFormat::Unknown) {
    std::vector<ZImgFormat*> readers = getSupportedReader(filename);
    if (readers.empty()) {
      error = QString("File %1 is not supported.").arg(filename);
    } else {
      for (auto reader : readers) {
        try {
          std::vector<ZImgInfo> tmpInfo;
          if (subBlocks) {
            subBlocks->clear();
          }
          reader->readInfo(filename, tmpInfo, subBlocks);
          if (!tmpInfo.empty()) {
            tmpInfo.swap(res);
            return;
          } else {
            throw ZIOException("empty image");
          }
        }
        catch (const ZIOException& e) {
          error += QString("\nTry read file %1 as '%2' format, failed: %3 ")
                     .arg(filename)
                     .arg(reader->fullName())
                     .arg(e.what());
        }
      }
    }
  } else if (m_ioFormats.find(format) == m_ioFormats.end() || !m_ioFormats[format]->supportRead()) {
    error = QString("Read format '%1' is not supported").arg(m_ioFormats[format]->fullName());
  } else {
    try {
      std::vector<ZImgInfo> tmpInfo;
      if (subBlocks) {
        subBlocks->clear();
      }
      m_ioFormats[format]->readInfo(filename, tmpInfo, subBlocks);
      if (!tmpInfo.empty()) {
        tmpInfo.swap(res);
        return;
      }
      throw ZIOException("empty image");
    }
    catch (const ZIOException& e) {
      error = QString("Try read file %1 as '%2' format, failed: %3")
                .arg(filename)
                .arg(m_ioFormats[format]->fullName())
                .arg(e.what());
    }
  }

  throw ZIOException(error);
}

void ZImgIO::readInfos(const QStringList& fileList,
                       Dimension catDim,
                       bool catScenes,
                       std::vector<ZImgInfo>& res,
                       std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                       FileFormat format,
                       bool expandXY)
{
  if (fileList.size() == 1 && !catScenes) {
    readInfos(fileList[0], res, subBlocks, format);
    return;
  }
  if (fileList.empty()) {
    throw ZIOException("Read sequence failed: empty file list");
  }

  readInfos(fileList[0], res, subBlocks, format);
  if (res.empty()) {
    throw ZIOException("Read sequence failed: img 0 is empty");
  }
  if (expandXY) {
    CHECK(catDim != Dimension::X && catDim != Dimension::Y);
  }
  if (FLAGS_zimg_use_multithreaded_image_sequence_reading) {
    auto cpuExecutor = folly::getGlobalCPUExecutor();
    auto p = dynamic_cast<folly::CPUThreadPoolExecutor*>(cpuExecutor.get());
    CHECK(p);

    std::vector<
      folly::Future<std::tuple<std::vector<ZImgInfo>, std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>>>>
      blockFutures;
    blockFutures.reserve(fileList.size() - 1);
    for (index_t i = 1; i < fileList.size(); ++i) {
      blockFutures.push_back(folly::via(cpuExecutor, [=]() {
        std::vector<ZImgInfo> tmpInfo;
        std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> tmpSubBlocks;
        ZImgIO::instance().readInfos(fileList[i], tmpInfo, subBlocks ? &tmpSubBlocks : nullptr, format);
        if (tmpInfo.empty()) {
          throw ZIOException(QString("Read sequence failed: img %1 is empty").arg(i));
        }
        return std::make_tuple(std::move(tmpInfo), std::move(tmpSubBlocks));
      }));
    }
    auto f =
      folly::collect(blockFutures)
        .via(cpuExecutor)
        .thenValue([=, &res](auto&& infoTuple) {
          if (catScenes) {
            for (const auto& [tmpInfo, tmpSubBlocks] : infoTuple) {
              res.insert(res.end(), tmpInfo.begin(), tmpInfo.end());
              if (subBlocks) {
                subBlocks->insert(subBlocks->end(), tmpSubBlocks.begin(), tmpSubBlocks.end());
              }
            }

            for (size_t s = 1; s < res.size(); ++s) {
              // check whether type match
              if (!res[s].isSameType(res[0])) {
                throw ZIOException(
                  QString("Read sequence failed: image type don't match, can not cat Img <%1> to Img 0 <%2>")
                    .arg(res[s].toQString())
                    .arg(res[0].toQString()));
              }
              // check whether dimension size match
              for (auto dim : ZImgInfo::dimensions()) {
                if (expandXY) {
                  if (dim != Dimension::X && dim != Dimension::Y && dim != catDim &&
                      res[s].size(dim) != res[0].size(dim)) {
                    throw ZIOException(
                      QString("Read sequence failed: image dimension don't match, can not cat Img <%1> to Img 0 <%2>")
                        .arg(res[s].toQString())
                        .arg(res[0].toQString()));
                  }
                } else {
                  if (dim != catDim && res[s].size(dim) != res[0].size(dim)) {
                    throw ZIOException(
                      QString("Read sequence failed: image dimension don't match, can not cat Img <%1> to Img 0 <%2>")
                        .arg(res[s].toQString())
                        .arg(res[0].toQString()));
                  }
                }
                if (dim == catDim) {
                  if (subBlocks) {
                    for (size_t tsidx = 0; tsidx < (*subBlocks)[s].size(); ++tsidx) {
                      switch (catDim) {
                        case Dimension::X:
                          (*subBlocks)[s][tsidx]->x += res[0].size(dim);
                          break;
                        case Dimension::Y:
                          (*subBlocks)[s][tsidx]->y += res[0].size(dim);
                          break;
                        case Dimension::Z:
                          (*subBlocks)[s][tsidx]->z += res[0].size(dim);
                          break;
                        case Dimension::T:
                          (*subBlocks)[s][tsidx]->t += res[0].size(dim);
                          break;
                        default:
                          break;
                      }
                      (*subBlocks)[0].push_back((*subBlocks)[s][tsidx]);
                    }
                  }
                  res[0].setSize(catDim, res[0].size(dim) + res[s].size(dim));
                }
              }
              // get final width and height
              if (expandXY) {
                res[0].width = std::max(res[0].width, res[s].width);
                res[0].height = std::max(res[0].height, res[s].height);
              }
            }
            res.resize(1);
            if (subBlocks) {
              subBlocks->resize(1);
            }
          } else {
            int i = 0;
            for (const auto& [tmpInfo, tmpSubBlocks] : infoTuple) {
              ++i;
              // check whether number of scenes match
              if (tmpInfo.size() != res.size()) {
                throw ZIOException("Read sequence failed: images have different number of scenes");
              }
              for (size_t s = 0; s < res.size(); ++s) {
                // check whether type match
                if (!tmpInfo[s].isSameType(res[s])) {
                  throw ZIOException(
                    QString("Read sequence failed: image type don't match, can not cat Img %1 <%2> to Img 0 <%3>")
                      .arg(i)
                      .arg(tmpInfo[s].toQString())
                      .arg(res[s].toQString()));
                }
                // check whether dimension size match
                for (auto dim : ZImgInfo::dimensions()) {
                  if (expandXY) {
                    if (dim != Dimension::X && dim != Dimension::Y && dim != catDim &&
                        res[s].size(dim) != tmpInfo[s].size(dim)) {
                      throw ZIOException(
                        QString(
                          "Read sequence failed: image dimension don't match, can not cat Img %1 <%2> to Img 0 <%3>")
                          .arg(i)
                          .arg(tmpInfo[s].toQString())
                          .arg(res[s].toQString()));
                    }
                  } else {
                    if (dim != catDim && res[s].size(dim) != tmpInfo[s].size(dim)) {
                      throw ZIOException(
                        QString(
                          "Read sequence failed: image dimension don't match, can not cat Img %1 <%2> to Img 0 <%3>")
                          .arg(i)
                          .arg(tmpInfo[s].toQString())
                          .arg(res[s].toQString()));
                    }
                  }
                  if (dim == catDim) {
                    if (subBlocks) {
                      for (size_t tsidx = 0; tsidx < tmpSubBlocks[s].size(); ++tsidx) {
                        switch (catDim) {
                          case Dimension::X:
                            tmpSubBlocks[s][tsidx]->x += res[s].size(dim);
                            break;
                          case Dimension::Y:
                            tmpSubBlocks[s][tsidx]->y += res[s].size(dim);
                            break;
                          case Dimension::Z:
                            tmpSubBlocks[s][tsidx]->z += res[s].size(dim);
                            break;
                          case Dimension::T:
                            tmpSubBlocks[s][tsidx]->t += res[s].size(dim);
                            break;
                          default:
                            break;
                        }
                        (*subBlocks)[s].push_back(tmpSubBlocks[s][tsidx]);
                      }
                    }
                    res[s].setSize(catDim, res[s].size(dim) + tmpInfo[s].size(dim));
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

          VLOG(1) << "image sequence reading finished.";
        })
        .thenError(folly::tag_t<ZException>{}, [](auto&& e) {
          LOG(ERROR) << "read image sequence error: " << e.what();
          throw std::forward<decltype(e)>(e);
        });

    while (!f.wait(std::chrono::seconds(5)).isReady()) {
      auto poolStats = p->getPoolStats();
      LOG(INFO) << fmt::format("pending/total task count: {}/{}, active/idle thread count: {}/{}",
                               poolStats.pendingTaskCount,
                               poolStats.totalTaskCount,
                               poolStats.activeThreadCount,
                               poolStats.idleThreadCount);
    }
    if (f.hasException()) {
      f.value();
    }
  } else {
    if (catScenes) {
      for (index_t i = 1; i < fileList.size(); ++i) {
        std::vector<ZImgInfo> tmpInfo;
        std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> tmpSubBlocks;
        readInfos(fileList[i], tmpInfo, subBlocks ? &tmpSubBlocks : nullptr, format);
        if (tmpInfo.empty()) {
          throw ZIOException(QString("Read sequence failed: img %1 is empty").arg(i));
        }
        res.insert(res.end(), tmpInfo.begin(), tmpInfo.end());
        if (subBlocks) {
          subBlocks->insert(subBlocks->end(), tmpSubBlocks.begin(), tmpSubBlocks.end());
        }
      }
      for (size_t s = 1; s < res.size(); ++s) {
        // check whether type match
        if (!res[s].isSameType(res[0])) {
          throw ZIOException(
            fmt::format("Read sequence failed: image type don't match, can not cat Img <{}> to Img 0 <{}>",
                        res[s].toString(),
                        res[0].toString()));
        }
        // check whether dimension size match
        for (auto dim : ZImgInfo::dimensions()) {
          if (expandXY) {
            if (dim != Dimension::X && dim != Dimension::Y && dim != catDim && res[s].size(dim) != res[0].size(dim)) {
              throw ZIOException(
                fmt::format("Read sequence failed: image dimension don't match, can not cat Img <{}> to Img 0 <{}>",
                            res[s].toString(),
                            res[0].toString()));
            }
          } else {
            if (dim != catDim && res[s].size(dim) != res[0].size(dim)) {
              throw ZIOException(
                fmt::format("Read sequence failed: image dimension don't match, can not cat Img <{}> to Img 0 <{}>",
                            res[s].toString(),
                            res[0].toString()));
            }
          }
          if (dim == catDim) {
            if (subBlocks) {
              for (size_t tsidx = 0; tsidx < (*subBlocks)[s].size(); ++tsidx) {
                switch (catDim) {
                  case Dimension::X:
                    (*subBlocks)[s][tsidx]->x += res[0].size(dim);
                    break;
                  case Dimension::Y:
                    (*subBlocks)[s][tsidx]->y += res[0].size(dim);
                    break;
                  case Dimension::Z:
                    (*subBlocks)[s][tsidx]->z += res[0].size(dim);
                    break;
                  case Dimension::T:
                    (*subBlocks)[s][tsidx]->t += res[0].size(dim);
                    break;
                  default:
                    break;
                }
                (*subBlocks)[0].push_back((*subBlocks)[s][tsidx]);
              }
            }
            res[0].setSize(catDim, res[0].size(dim) + res[s].size(dim));
          }
        }
        // get final width and height
        if (expandXY) {
          res[0].width = std::max(res[0].width, res[s].width);
          res[0].height = std::max(res[0].height, res[s].height);
        }
      }
      res.resize(1);
      if (subBlocks) {
        subBlocks->resize(1);
      }
    } else {
      for (index_t i = 1; i < fileList.size(); ++i) {
        std::vector<ZImgInfo> tmpInfo;
        std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> tmpSubBlocks;
        readInfos(fileList[i], tmpInfo, subBlocks ? &tmpSubBlocks : nullptr, format);
        if (tmpInfo.empty()) {
          throw ZIOException(QString("Read sequence failed: img %1 is empty").arg(i));
        }
        // check whether number of scenes match
        if (tmpInfo.size() != res.size()) {
          throw ZIOException("Read sequence failed: images have different number of scenes");
        }
        for (size_t s = 0; s < res.size(); ++s) {
          // check whether type match
          if (!tmpInfo[s].isSameType(res[s])) {
            throw ZIOException(
              QString("Read sequence failed: image type don't match, can not cat Img %1 <%2> to Img 0 <%3>")
                .arg(i)
                .arg(tmpInfo[s].toQString())
                .arg(res[s].toQString()));
          }
          // check whether dimension size match
          for (auto dim : ZImgInfo::dimensions()) {
            if (expandXY) {
              if (dim != Dimension::X && dim != Dimension::Y && dim != catDim &&
                  res[s].size(dim) != tmpInfo[s].size(dim)) {
                throw ZIOException(
                  QString("Read sequence failed: image dimension don't match, can not cat Img %1 <%2> to Img 0 <%3>")
                    .arg(i)
                    .arg(tmpInfo[s].toQString())
                    .arg(res[s].toQString()));
              }
            } else {
              if (dim != catDim && res[s].size(dim) != tmpInfo[s].size(dim)) {
                throw ZIOException(
                  QString("Read sequence failed: image dimension don't match, can not cat Img %1 <%2> to Img 0 <%3>")
                    .arg(i)
                    .arg(tmpInfo[s].toQString())
                    .arg(res[s].toQString()));
              }
            }
            if (dim == catDim) {
              if (subBlocks) {
                for (size_t tsidx = 0; tsidx < tmpSubBlocks[s].size(); ++tsidx) {
                  switch (catDim) {
                    case Dimension::X:
                      tmpSubBlocks[s][tsidx]->x += res[s].size(dim);
                      break;
                    case Dimension::Y:
                      tmpSubBlocks[s][tsidx]->y += res[s].size(dim);
                      break;
                    case Dimension::Z:
                      tmpSubBlocks[s][tsidx]->z += res[s].size(dim);
                      break;
                    case Dimension::T:
                      tmpSubBlocks[s][tsidx]->t += res[s].size(dim);
                      break;
                    default:
                      break;
                  }
                  (*subBlocks)[s].push_back(tmpSubBlocks[s][tsidx]);
                }
              }
              res[s].setSize(catDim, res[s].size(dim) + tmpInfo[s].size(dim));
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
  }

  // destroy all and build simple one
  if (subBlocks && catDim == Dimension::C) {
    subBlocks->clear();
    subBlocks->resize(res.size());
    for (size_t s = 0; s < res.size(); ++s) {
      for (size_t t = 0; t < res[s].numTimes; ++t) {
        for (size_t z = 0; z < res[s].depth; ++z) {
          (*subBlocks)[s].emplace_back(std::make_shared<ZImgTileSubBlock>(
            ZImgSource(fileList,
                       catDim,
                       catScenes,
                       ZImgRegion(ZVoxelCoordinate(0, 0, z, 0, t),
                                  ZVoxelCoordinate(res[s].width, res[s].height, z + 1, res[s].numChannels, t + 1)),
                       s)));
        }
      }
    }
  }
}

void ZImgIO::readInfo(const ZImgSource& imgSource,
                      ZImgInfo& info,
                      std::vector<std::shared_ptr<ZImgSubBlock>>* subBlocks)
{
  if (imgSource.filenames.size() == 1 && !imgSource.catScenes) {
    std::vector<ZImgInfo> res;
    std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> tmpSubBlocks;
    readInfos(imgSource.filenames[0], res, subBlocks ? &tmpSubBlocks : nullptr, imgSource.format);
    if (imgSource.scene >= res.size()) {
      throw ZIOException("invalid scene");
    }
    info = res[imgSource.scene];
    info = imgSource.region.clip(info);
    if (subBlocks) {
      *subBlocks = tmpSubBlocks[imgSource.scene];
    }
  } else if (!imgSource.filenames.empty()) {
    std::vector<ZImgInfo> res;
    std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> tmpSubBlocks;
    readInfos(imgSource.filenames,
              imgSource.catDim,
              imgSource.catScenes,
              res,
              subBlocks ? &tmpSubBlocks : nullptr,
              imgSource.format,
              imgSource.expandXY);
    if (imgSource.scene >= res.size()) {
      throw ZIOException("invalid scene");
    }
    info = res[imgSource.scene];
    info = imgSource.region.clip(info);
    if (subBlocks) {
      *subBlocks = tmpSubBlocks[imgSource.scene];
    }
  } else {
    throw ZIOException("invalid image source");
  }
}

std::vector<std::vector<ZImgRegion>> ZImgIO::getInternalSubRegions(const QString& filename, FileFormat format)
{
  std::vector<ZImgInfo> infos;
  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  readInfos(filename, infos, &subBlocks, format);
  std::vector<std::vector<ZImgRegion>> res(infos.size());
  for (size_t i = 0; i < res.size(); ++i) {
    auto& blocks = subBlocks[i];
    const auto& info = infos[i];
    std::set<std::tuple<size_t, size_t, size_t, size_t, size_t, size_t, size_t>> tiles;
    for (const auto& block : blocks) {
      if (block->xRatio != 1 || block->yRatio != 1 || block->zRatio != 1) {
        continue;
      }
      tiles.insert(std::tuple<size_t, size_t, size_t, size_t, size_t, size_t, size_t>(block->t,
                                                                                      block->x,
                                                                                      block->y,
                                                                                      block->width,
                                                                                      block->height,
                                                                                      block->z,
                                                                                      block->depth));
    }
    auto lastTile = std::tuple<size_t, size_t, size_t, size_t, size_t, size_t, size_t>();
    for (const auto& tile : tiles) {
      auto [t, x, y, width, height, z, depth] = tile;
      if (res[i].empty()) {
        ZVoxelCoordinate startC(x, y, z, 0, t);
        ZVoxelCoordinate endC(x + width, y + height, z + depth, info.numChannels, t + 1);
        res[i].push_back(ZImgRegion(startC, endC));
        lastTile = tile;
        continue;
      }
      auto [lt, lx, ly, lwidth, lheight, lz, ldepth] = lastTile;
      if (lt == t && lx == x && ly == y && lwidth == width && lheight == height) {
        auto& rgn = res[i].back();
        if (z != size_t(rgn.end.z)) {
          throw ZIOException("z jumping");
        } else {
          rgn.end.z += depth;
        }
      } else {
        ZVoxelCoordinate startC(x, y, z, 0, t);
        ZVoxelCoordinate endC(x + width, y + height, z + depth, info.numChannels, t + 1);
        res[i].push_back(ZImgRegion(startC, endC));
        lastTile = tile;
      }
    }
  }
  return res;
}

void ZImgIO::readMetadata(const ZImgSource& imgSource, ZImgMetadata& meta)
{
  meta.clear();
  QString error;

  if (imgSource.format == FileFormat::Unknown) {
    for (const auto& filename : imgSource.filenames) {
      std::vector<ZImgFormat*> readers = getSupportedReader(filename);
      if (readers.empty()) {
        error = QString("File %1 is not supported.").arg(filename);
      } else {
        for (auto reader : readers) {
          try {
            ZImgMetadata tmpMeta;
            reader->readMetadata(filename, tmpMeta, imgSource.catScenes ? 0 : imgSource.scene);
            meta.merge(tmpMeta);
            return;
          }
          catch (const ZIOException& e) {
            error += QString("\nTry read file %1 as '%2' format, failed: %3 ")
                       .arg(filename)
                       .arg(reader->fullName())
                       .arg(e.what());
          }
        }
      }
    }
  } else if (m_ioFormats.find(imgSource.format) == m_ioFormats.end() || !m_ioFormats[imgSource.format]->supportRead()) {
    error = QString("Read format '%1' is not supported").arg(m_ioFormats[imgSource.format]->fullName());
  } else {
    for (const auto& filename : imgSource.filenames) {
      try {
        ZImgMetadata tmpMeta;
        m_ioFormats[imgSource.format]->readMetadata(filename, tmpMeta, imgSource.catScenes ? 0 : imgSource.scene);
        meta.merge(tmpMeta);
        return;
      }
      catch (const ZIOException& e) {
        error = QString("Try read file %1 as '%2' format, failed: %3")
                  .arg(filename)
                  .arg(m_ioFormats[imgSource.format]->fullName())
                  .arg(e.what());
      }
    }
  }

  throw ZIOException(error);
}

void ZImgIO::readThumbnail(const QString& filename,
                           ZImgThumbernail& thumbnail,
                           const ZImgRegion& region,
                           size_t scene,
                           FileFormat format)
{
  thumbnail.clear();
  QString error;

  if (format == FileFormat::Unknown) {
    std::vector<ZImgFormat*> readers = getSupportedReader(filename);
    if (readers.empty()) {
      error = QString("File %1 is not supported.").arg(filename);
    } else {
      for (auto reader : readers) {
        try {
          ZImgThumbernail tmpThumbnail;
          reader->readThumbnail(filename, tmpThumbnail, region, scene);
          tmpThumbnail.swap(thumbnail);
          return;
        }
        catch (const ZIOException& e) {
          error += QString("\nTry read file %1 as '%2' format, failed: %3 ")
                     .arg(filename)
                     .arg(reader->fullName())
                     .arg(e.what());
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
    catch (const ZIOException& e) {
      error = QString("Try read file %1 as '%2' format, failed: %3")
                .arg(filename)
                .arg(m_ioFormats[format]->fullName())
                .arg(e.what());
    }
  }

  throw ZIOException(error);
}

void ZImgIO::readImg(const QString& filename,
                     ZImg& img,
                     const ZImgRegion& region,
                     size_t scene,
                     size_t xRatio,
                     size_t yRatio,
                     size_t zRatio,
                     FileFormat format)
{
  img.clear();
  QString error;

  if (format == FileFormat::Unknown) {
    std::vector<ZImgFormat*> readers = getSupportedReader(filename);
    if (readers.empty()) {
      error = QString("File %1 is not supported.").arg(filename);
    } else {
      for (auto reader : readers) {
        try {
          ZImg tmpImg;
          reader->readImg(filename, tmpImg, region, scene, xRatio, yRatio, zRatio);
          tmpImg.swap(img);
          return;
        }
        catch (const ZIOException& e) {
          error += QString("\nTry read file %1 as '%2' format, failed: %3 ")
                     .arg(filename)
                     .arg(reader->fullName())
                     .arg(e.what());
        }
      }
    }
  } else if (m_ioFormats.find(format) == m_ioFormats.end() || !m_ioFormats[format]->supportRead()) {
    error = QString("Read format '%1' is not supported").arg(m_ioFormats[format]->fullName());
  } else {
    try {
      ZImg tmpImg;
      m_ioFormats[format]->readImg(filename, tmpImg, region, scene, xRatio, yRatio, zRatio);
      tmpImg.swap(img);
      return;
    }
    catch (const ZIOException& e) {
      error = QString("Try read file %1 as '%2' format, failed: %3")
                .arg(filename)
                .arg(m_ioFormats[format]->fullName())
                .arg(e.what());
    }
  }

  throw ZIOException(error);
}

void ZImgIO::readImg(const QStringList& fileList,
                     Dimension catDim,
                     bool catScenes,
                     ZImg& img,
                     size_t scene,
                     size_t xRatio,
                     size_t yRatio,
                     size_t zRatio,
                     FileFormat format,
                     bool expandXY,
                     bool expandWithMaxValue)
{
  if (fileList.size() == 1 && !catScenes) {
    readImg(fileList[0], img, ZImgRegion(), scene, xRatio, yRatio, zRatio, format);
    return;
  }

  std::vector<ZImgInfo> infos;
  readInfos(fileList, catDim, catScenes, infos, nullptr, format, expandXY);
  if (scene >= infos.size()) {
    throw ZIOException("invalid scene for image sequence");
  }
  ZImgInfo& info = infos[scene];

  std::vector<ZImg> imgs;
  std::vector<ZImgSource> imgSources;
  if (catScenes) {
    for (const auto& fn : fileList) {
      std::vector<ZImgInfo> fninfos;
      readInfos(fn, fninfos);
      for (size_t s = 0; s < fninfos.size(); ++s) {
        imgSources.emplace_back(fn, ZImgRegion(), s, format);
      }
    }
  } else {
    for (const auto& fn : fileList) {
      imgSources.emplace_back(fn, ZImgRegion(), scene, format);
    }
  }

  imgs.resize(imgSources.size());
  for (size_t i = 0; i < imgs.size(); ++i) {
    readImg(imgSources[i], imgs[i]);
    if (expandXY && (imgs[i].width() < info.width || imgs[i].height() < info.height)) {
      auto widthPadBefore = index_t(info.width - imgs[i].width()) / 2;
      auto widthPadAfter = index_t(info.width - imgs[i].width()) - widthPadBefore;
      auto heightPadBefore = index_t(info.height - imgs[i].height()) / 2;
      auto heightPadAfter = index_t(info.height - imgs[i].height()) - heightPadBefore;
      if (info.voxelFormat == VoxelFormat::Float) {
        double min;
        double max;
        imgs[i].computeMinMax(min, max);
        imgs[i] = imgs[i].cropWithPad(ZVoxelCoordinate(-widthPadBefore, -heightPadBefore, 0, 0, 0),
                                      imgs[i].endCoord() + ZVoxelCoordinate(widthPadAfter, heightPadAfter),
                                      PadOption::Constant,
                                      expandWithMaxValue ? max : min);
      } else if (info.voxelFormat == VoxelFormat::Unsigned) {
        uint64_t min;
        uint64_t max;
        imgs[i].computeMinMax(min, max);
        imgs[i] = imgs[i].cropWithPad(ZVoxelCoordinate(-widthPadBefore, -heightPadBefore, 0, 0, 0),
                                      imgs[i].endCoord() + ZVoxelCoordinate(widthPadAfter, heightPadAfter),
                                      PadOption::Constant,
                                      expandWithMaxValue ? max : min);
      } else {
        int64_t min;
        int64_t max;
        imgs[i].computeMinMax(min, max);
        imgs[i] = imgs[i].cropWithPad(ZVoxelCoordinate(-widthPadBefore, -heightPadBefore, 0, 0, 0),
                                      imgs[i].endCoord() + ZVoxelCoordinate(widthPadAfter, heightPadAfter),
                                      PadOption::Constant,
                                      expandWithMaxValue ? max : min);
      }
    }
  }

  if (imgs.size() == 1) {
    img.swap(imgs[0]);
  } else if (imgs.size() > 1) {
    ZImg catImg = ZImg::cat(imgs, catDim);
    img.swap(catImg);
  }

  CHECK(xRatio >= 1 && yRatio >= 1 && zRatio >= 1);
  if (xRatio > 1 || yRatio > 1 || zRatio > 1) {
    img.zoom(1.0 / xRatio, 1.0 / yRatio, 1.0 / zRatio);
  }
}

void ZImgIO::readImg(const QStringList& fileList,
                     Dimension catDim,
                     bool catScenes,
                     const ZImgRegion& regionIn,
                     ZImg& img,
                     size_t scene,
                     size_t xRatio,
                     size_t yRatio,
                     size_t zRatio,
                     FileFormat format,
                     bool expandXY,
                     bool expandWithMaxValue)
{
  if (fileList.size() == 1 && !catScenes) {
    readImg(fileList[0], img, regionIn, scene, xRatio, yRatio, zRatio, format);
    return;
  }

  if (regionIn.isDefault()) {
    readImg(fileList, catDim, catScenes, img, scene, xRatio, yRatio, zRatio, format, expandXY, expandWithMaxValue);
    return;
  }

  std::vector<ZImgInfo> infos;
  readInfos(fileList, catDim, catScenes, infos, nullptr, format, expandXY);
  if (infos.size() <= scene) {
    throw ZIOException("invalid scene for image sequence");
  }
  ZImgInfo& info = infos[scene];
  if (regionIn.isEmpty() || !regionIn.isValid(info)) {
    throw ZIOException(
      QString("Invalid image region. Image info: '%1', region: '%2'").arg(info.toQString()).arg(regionIn.toQString()));
  }
  ZImgRegion region = regionIn;
  region.resolveRegionEnd(info);

  std::vector<ZImg> imgs;
  std::vector<ZImgSource> imgSources;
  std::vector<ZImgInfo> sliceInfos;
  if (catScenes) {
    for (const auto& fn : fileList) {
      std::vector<ZImgInfo> fninfos;
      readInfos(fn, fninfos);
      for (size_t s = 0; s < fninfos.size(); ++s) {
        imgSources.emplace_back(fn, ZImgRegion(), s, format);
        sliceInfos.push_back(fninfos[s]);
      }
    }
  } else {
    for (const auto& fn : fileList) {
      std::vector<ZImgInfo> fninfos;
      readInfos(fn, fninfos);
      imgSources.emplace_back(fn, ZImgRegion(), scene, format);
      sliceInfos.push_back(fninfos[scene]);
    }
  }
  imgs.resize(imgSources.size());

  size_t sliceCatDimStart = 0;
  size_t sliceCatDimEnd = 0;
  for (size_t i = 0; i < imgs.size(); ++i) {
    ZImgInfo& sliceInfo = sliceInfos[i];
    sliceCatDimStart = i == 0 ? 0 : sliceCatDimEnd;
    sliceCatDimEnd = sliceCatDimStart + sliceInfo.size(catDim);
    if (sliceCatDimStart >= static_cast<size_t>(region.end[to_underlying(catDim)]) ||
        sliceCatDimEnd <= static_cast<size_t>(region.start[to_underlying(catDim)])) {
      continue;
    }
    ZImgRegion sliceRegion = region;
    if (static_cast<size_t>(region.start[to_underlying(catDim)]) > sliceCatDimStart) {
      sliceRegion.start[to_underlying(catDim)] =
        static_cast<size_t>(region.start[to_underlying(catDim)]) - sliceCatDimStart;
    } else {
      sliceRegion.start[to_underlying(catDim)] = 0;
    }
    if (static_cast<size_t>(region.end[to_underlying(catDim)]) < sliceCatDimEnd) {
      sliceRegion.end[to_underlying(catDim)] =
        sliceInfo.size(catDim) - (sliceCatDimEnd - static_cast<size_t>(region.end[to_underlying(catDim)]));
    } else {
      sliceRegion.end[to_underlying(catDim)] = sliceInfo.size(catDim);
    }

    if (expandXY && (sliceInfo.width < info.width || sliceInfo.height < info.height)) {
      auto widthPadBefore = index_t(info.width - sliceInfo.width) / 2;
      auto heightPadBefore = index_t(info.height - sliceInfo.height) / 2;
      ZImgRegion sliceRegionFullXY = sliceRegion;
      sliceRegionFullXY.start.x = 0;
      sliceRegionFullXY.end.x = -1;
      sliceRegionFullXY.start.y = 0;
      sliceRegionFullXY.end.y = -1;
      auto imgSource = imgSources[i];
      imgSource.region = sliceRegionFullXY;
      readImg(imgSource, imgs[i]);
      if (info.voxelFormat == VoxelFormat::Float) {
        double min;
        double max;
        imgs[i].computeMinMax(min, max);
        imgs[i] = imgs[i].cropWithPad(
          ZVoxelCoordinate(-widthPadBefore + sliceRegion.start.x, -heightPadBefore + sliceRegion.start.y, 0),
          ZVoxelCoordinate(-widthPadBefore + sliceRegion.end.x, -heightPadBefore + sliceRegion.end.y, imgs[i].depth()),
          PadOption::Constant,
          expandWithMaxValue ? max : min);
      } else if (info.voxelFormat == VoxelFormat::Unsigned) {
        uint64_t min;
        uint64_t max;
        imgs[i].computeMinMax(min, max);
        imgs[i] = imgs[i].cropWithPad(
          ZVoxelCoordinate(-widthPadBefore + sliceRegion.start.x, -heightPadBefore + sliceRegion.start.y, 0),
          ZVoxelCoordinate(-widthPadBefore + sliceRegion.end.x, -heightPadBefore + sliceRegion.end.y, imgs[i].depth()),
          PadOption::Constant,
          expandWithMaxValue ? max : min);
      } else {
        int64_t min;
        int64_t max;
        imgs[i].computeMinMax(min, max);
        imgs[i] = imgs[i].cropWithPad(
          ZVoxelCoordinate(-widthPadBefore + sliceRegion.start.x, -heightPadBefore + sliceRegion.start.y, 0),
          ZVoxelCoordinate(-widthPadBefore + sliceRegion.end.x, -heightPadBefore + sliceRegion.end.y, imgs[i].depth()),
          PadOption::Constant,
          expandWithMaxValue ? max : min);
      }
    } else {
      auto imgSource = imgSources[i];
      imgSource.region = sliceRegion;
      readImg(imgSource, imgs[i]);
    }
  }

  if (imgs.size() == 1) {
    img.swap(imgs[0]);
  } else if (imgs.size() > 1) {
    ZImg catImg = ZImg::cat(imgs, catDim);
    img.swap(catImg);
  }

  CHECK(xRatio >= 1 && yRatio >= 1 && zRatio >= 1);
  // todo: bug here, check if img info is correct
  if (xRatio > 1 || yRatio > 1 || zRatio > 1) {
    img.zoom(1.0 / xRatio, 1.0 / yRatio, 1.0 / zRatio);
  }
}

void ZImgIO::readImg(const ZImgSource& imgSource, ZImg& img, size_t xRatio, size_t yRatio, size_t zRatio)
{
  if (imgSource.filenames.size() == 1 && !imgSource.catScenes) {
    readImg(imgSource.filenames[0], img, imgSource.region, imgSource.scene, xRatio, yRatio, zRatio, imgSource.format);
  } else if (!imgSource.filenames.empty()) {
    readImg(imgSource.filenames,
            imgSource.catDim,
            imgSource.catScenes,
            imgSource.region,
            img,
            imgSource.scene,
            xRatio,
            yRatio,
            zRatio,
            imgSource.format,
            imgSource.expandXY,
            imgSource.expandWithMaxValue);
  } else {
    throw ZIOException("invalid image source");
  }
}

void ZImgIO::writeImg(const QString& filename, const ZImg& img, FileFormat format, const ZImgWriteParameters& paras)
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
      for (auto writer : writers) {
        try {
          if (filename.endsWith(".mhd", Qt::CaseInsensitive)) {
            writer->writeImg(filename, img, paras);
          } else {
            auto tfn = getTemporaryFilename(filename);
            writer->writeImg(tfn, img, paras);
            renameFile(tfn, filename);
          }
          return;
        }
        catch (const ZIOException& e) {
          error +=
            QString("Try write %1 as '%2' format, failed: %3 ").arg(filename).arg(writer->fullName()).arg(e.what());
        }
      }
    }
  } else if (m_ioFormats.find(format) == m_ioFormats.end() || !m_ioFormats[format]->supportWrite()) {
    error = QString("Write format '%1' is not supported").arg(m_ioFormats[format]->fullName());
  } else {
    try {
      if (format == FileFormat::MetaImage) {
        m_ioFormats[format]->writeImg(filename, img, paras);
      } else {
        auto tfn = getTemporaryFilename(filename);
        m_ioFormats[format]->writeImg(tfn, img, paras);
        renameFile(tfn, filename);
      }
      return;
    }
    catch (const ZIOException& e) {
      error = QString("Try write file %1 as '%2' format, failed: %3")
                .arg(filename)
                .arg(m_ioFormats[format]->fullName())
                .arg(e.what());
    }
  }

  throw ZIOException(error);
}

void ZImgIO::writeImg(const QString& filename,
                      const ZImgSliceProvider& img,
                      FileFormat format,
                      const ZImgWriteParameters& paras)
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
      for (auto writer : writers) {
        try {
          if (filename.endsWith(".mhd", Qt::CaseInsensitive)) {
            writer->writeImg(filename, img, paras);
          } else {
            auto tfn = getTemporaryFilename(filename);
            writer->writeImg(tfn, img, paras);
            renameFile(tfn, filename);
          }
          return;
        }
        catch (const ZIOException& e) {
          error +=
            QString("\nTry write %1 as '%2' format, failed: %3 ").arg(filename).arg(writer->fullName()).arg(e.what());
        }
      }
    }
  } else if (m_ioFormats.find(format) == m_ioFormats.end() || !m_ioFormats[format]->supportWrite()) {
    error = QString("Write format '%1' is not supported").arg(m_ioFormats[format]->fullName());
  } else {
    try {
      if (format == FileFormat::MetaImage) {
        m_ioFormats[format]->writeImg(filename, img, paras);
      } else {
        auto tfn = getTemporaryFilename(filename);
        m_ioFormats[format]->writeImg(tfn, img, paras);
        renameFile(tfn, filename);
      }
      return;
    }
    catch (const ZIOException& e) {
      error = QString("Try write file %1 as '%2' format, failed: %3")
                .arg(filename)
                .arg(m_ioFormats[format]->fullName())
                .arg(e.what());
    }
  }

  throw ZIOException(error);
}

void ZImgIO::writeImg(const QString& filename,
                      const ZImgBlockProvider& img,
                      FileFormat format,
                      const ZImgWriteParameters& paras)
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
      for (auto writer : writers) {
        try {
          if (filename.endsWith(".mhd", Qt::CaseInsensitive)) {
            writer->writeImg(filename, img, paras);
          } else {
            auto tfn = getTemporaryFilename(filename);
            writer->writeImg(tfn, img, paras);
            renameFile(tfn, filename);
          }
          return;
        }
        catch (const ZIOException& e) {
          error +=
            QString("\nTry write %1 as '%2' format, failed: %3 ").arg(filename).arg(writer->fullName()).arg(e.what());
        }
      }
    }
  } else if (m_ioFormats.find(format) == m_ioFormats.end() || !m_ioFormats[format]->supportWrite()) {
    error = QString("Write format '%1' is not supported").arg(m_ioFormats[format]->fullName());
  } else {
    try {
      if (format == FileFormat::MetaImage) {
        m_ioFormats[format]->writeImg(filename, img, paras);
      } else {
        auto tfn = getTemporaryFilename(filename);
        m_ioFormats[format]->writeImg(tfn, img, paras);
        renameFile(tfn, filename);
      }
      return;
    }
    catch (const ZIOException& e) {
      error = QString("Try write file %1 as '%2' format, failed: %3")
                .arg(filename)
                .arg(m_ioFormats[format]->fullName())
                .arg(e.what());
    }
  }

  throw ZIOException(error);
}

void ZImgIO::getQtReadNameFilter(QStringList& filters, std::vector<FileFormat>& formats) const
{
  filters.clear();
  formats.clear();
  formats.push_back(FileFormat::Unknown); // for filter 'all'

  QString all = "Images";
  QStringList lst;
  for (const auto& fmt : m_ioFormats) {
    if (fmt.second->supportRead()) {
      QString filter = QString("*.") + fmt.second->extensions().join(" *.");
      lst.push_back(filter);

      filter = fmt.second->fullName() + QString(" (*.") + fmt.second->extensions().join(" *.") + QString(")");
      filters.push_back(filter);
      formats.push_back(fmt.first);
    }
  }
  all += " (";
  all += lst.join(" ");
  all += ")";
  filters.prepend(all);
}

void ZImgIO::getQtWriteNameFilter(QStringList& filters,
                                  std::vector<FileFormat>& formats,
                                  std::vector<Compression>& comps) const
{
  filters.clear();
  formats.clear();
  comps.clear();
  for (const auto& fmt : m_ioFormats) {
    if (fmt.second->supportWrite()) {
      QString filter = fmt.second->fullName() + QString(" (*.") + fmt.second->extensions().join(" *.") + QString(")");
      if (fmt.first == FileFormat::OmeTiff || fmt.first == FileFormat::Tiff) {
        QString flt = QString("LZW Compressed ") + filter; // todo: use custom dialog and set compression as option
        filters.push_back(flt);
        formats.push_back(fmt.first);
        comps.push_back(Compression::LZW);
        flt = QString("ADOBE DEFLATE Compressed ") + filter;
        filters.push_back(flt);
        formats.push_back(fmt.first);
        comps.push_back(Compression::ADOBE_DEFLATE);
      }
      if (fmt.first == FileFormat::MetaImage) {
        QString flt = QString("Compressed ") + filter; // todo: use custom dialog and set compression as option
        filters.push_back(flt);
        formats.push_back(fmt.first);
        comps.push_back(Compression::AUTO);
        filters.push_back(filter);
        formats.push_back(fmt.first);
        comps.push_back(Compression::NONE);
      } else {
        filters.push_back(filter);
        formats.push_back(fmt.first);
        comps.push_back(Compression::AUTO);
      }
    }
  }
}

bool ZImgIO::fileExtensionReadSupported(const QString& filename) const
{
  for (const auto& fmt : m_ioFormats) {
    if (fmt.second->canRead(filename)) {
      return true;
    }
  }
  return false;
}

bool ZImgIO::fileExtensionWriteSupported(const QString& filename) const
{
  for (const auto& fmt : m_ioFormats) {
    if (fmt.second->canWrite(filename)) {
      return true;
    }
  }
  return false;
}

std::vector<ZImgFormat*> ZImgIO::getSupportedReader(const QString& filename) const
{
  std::vector<ZImgFormat*> res;
  for (const auto& fmt : m_ioFormats) {
    if (fmt.second->canRead(filename)) {
      res.push_back(fmt.second.get());
    }
  }
  return res;
}

std::vector<ZImgFormat*> ZImgIO::getSupportedWriter(const QString& filename) const
{
  std::vector<ZImgFormat*> res;
  for (const auto& fmt : m_ioFormats) {
    if (fmt.second->canWrite(filename)) {
      res.push_back(fmt.second.get());
    }
  }
  return res;
}

} // namespace nim
