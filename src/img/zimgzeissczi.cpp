#include "zimgzeissczi.h"
#include <QFileInfo>
#include <QDir>
#include "zioutils.h"
#include <boost/uuid/uuid_io.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <array>
#include <tuple>
#include "zbbox.h"
#include "zimgjpegxr.h"
#include "zimgjpeg.h"
#include "zimgfreeimage.h"
#include "ztiff.h"
#include "zstatisticsutils.h"
#include "zimgregioniterator.h"
#include "zlog.h"

//#define DUMP_CZI_INFO
#define NO_MIXED_TILE

namespace {

using namespace nim;

inline bool pixelTypeIsBGR(int32_t pixelType)
{
  return pixelType == 3 || pixelType == 4 ||
         pixelType == 8 || pixelType == 9;
}

ZImg readCZITile(std::ifstream& inputFileStream, const CZITile& tile)
{
  SegmentHeader sh;
  inputFileStream.seekg(tile.filePosition);
  readStream(inputFileStream, &sh, sizeof(SegmentHeader));

  if (std::strncmp(sh.id, "ZISRAWSUBBLOCK", 15) != 0) {
    throw ZIOException("can not locate czi tile");
  }

  SubBlockSegment sb;
  readStream(inputFileStream, &sb, sizeof(SubBlockSegment));

  int directoryEntriesSize = sizeof(DimensionEntryDV1) * sb.directoryEntry.dimensionCount;
  int directoryEntrySize = sizeof(DirectoryEntryDV) + directoryEntriesSize;
  int fill = std::max(0, 256 - directoryEntrySize - 16);

  inputFileStream.seekg(directoryEntriesSize + fill + sb.metaDataSize, std::ios_base::cur);

  std::vector<uint8_t> fileBuf(192 + sb.dataSize);
  if (sb.directoryEntry.compression == 2) {
    readStream(inputFileStream, fileBuf.data() + 192, sb.dataSize); // wrap a tif header and use tif decoder
  } else {
    readStream(inputFileStream, fileBuf.data(), sb.dataSize);
  }

  ZImgInfo info;
  QString dimensionOrder = tile.dimensionOrder;
  if (sb.directoryEntry.compression != 1 &&
      sb.directoryEntry.compression != 4) {

    info.width = tile.storedSize.x;
    info.height = tile.storedSize.y;
    info.depth = tile.storedSize.z;
    info.numChannels = tile.storedSize.c;
    info.numTimes = tile.storedSize.t;

    switch (sb.directoryEntry.pixelType) {
      case 0:
        info.bytesPerVoxel = 1;
        info.voxelFormat = VoxelFormat::Unsigned;
        break;
      case 1:
        info.bytesPerVoxel = 2;
        info.voxelFormat = VoxelFormat::Unsigned;
        break;
      case 2:
        info.bytesPerVoxel = 4;
        info.voxelFormat = VoxelFormat::Float;
        break;
      case 3:
        CHECK(info.numChannels == 1);
        dimensionOrder.remove('C');
        dimensionOrder.push_front('C');
        info.numChannels = 3;
        info.bytesPerVoxel = 1;
        info.voxelFormat = VoxelFormat::Unsigned;
        break;
      case 4:
        CHECK(info.numChannels == 1);
        dimensionOrder.remove('C');
        dimensionOrder.push_front('C');
        info.numChannels = 3;
        info.bytesPerVoxel = 2;
        info.voxelFormat = VoxelFormat::Unsigned;
        break;
      case 8:
        CHECK(info.numChannels == 1);
        dimensionOrder.remove('C');
        dimensionOrder.push_front('C');
        info.numChannels = 3;
        info.bytesPerVoxel = 4;
        info.voxelFormat = VoxelFormat::Float;
        break;
      case 9:
        CHECK(info.numChannels == 1);
        dimensionOrder.remove('C');
        dimensionOrder.push_front('C');
        info.numChannels = 4;
        info.lastChannelIsAlphaChannel = true;
        info.bytesPerVoxel = 1;
        info.voxelFormat = VoxelFormat::Unsigned;
        break;
      case 10:
        throw ZIOException("complex gray64 image not supported");
        break;
      case 11:
        throw ZIOException("complex bgr192 image not supported");
        break;
      case 12:
        info.bytesPerVoxel = 4;
        info.voxelFormat = VoxelFormat::Unsigned;
        break;
      case 13:
        info.bytesPerVoxel = 8;
        info.voxelFormat = VoxelFormat::Float;
        break;
      default:
        throw ZIOException("Wrong Pixel Type");
        break;
    }
    info.createDefaultDescriptions();
  }

  ZImg res;
  switch (sb.directoryEntry.compression) {
    case 0:  // Uncompressed
      if (static_cast<int64_t>(info.byteNumber()) != sb.dataSize) {
        throw ZIOException("stored size and tile info don't match");
      }
      res = ZImg(info);
      ZImgFormat::fixDimensionOrder(fileBuf.data(), dimensionOrder, res, pixelTypeIsBGR(sb.directoryEntry.pixelType));
      break;
    case 2:  // LZW
    {
      res = ZImg(info);
      ZTiff::writeTiffHeader(fileBuf.data(), info.width, info.height * info.depth * info.numTimes * info.numChannels,
                             info.bytesPerVoxel * 8, 1, 5, 192, sb.dataSize);

      typedef boost::iostreams::basic_array_source<char> Device;
      // reinterpret_cast allowed (AliasedType is the (possibly cv-qualified) signed or unsigned variant of DynamicType)
      boost::iostreams::stream<Device> istr(reinterpret_cast<char*>(fileBuf.data()), fileBuf.size());
      ZTiff tif;
      tif.load(istr);
      info.height *= info.numTimes;
      info.numTimes = 1;
      ZImg img(info);
      tif.readImgFromIFD(0, img);
      ZImgFormat::fixDimensionOrder(img.timeData<uint8_t>(0), dimensionOrder, res,
                                    pixelTypeIsBGR(sb.directoryEntry.pixelType));
    }
      break;
    case 1:  // Jpeg
      ZImgJpegInstance.readInfo(fileBuf.data(), sb.dataSize, info);
      res = ZImg(info);
      ZImgJpegInstance.readImg(fileBuf.data(), sb.dataSize, res.timeData<uint8_t>(0), res.byteNumber());
      break;
    case 4:  // JpegXR
      ZImgJpegXRInstance.readInfo(fileBuf.data(), sb.dataSize, info);
      res = ZImg(info);
      ZImgJpegXRInstance.readImg(fileBuf.data(), sb.dataSize, res.timeData<uint8_t>(0), res.byteNumber());
      break;
    default:
      try {
        if (static_cast<int64_t>(info.byteNumber()) == sb.dataSize) {
          res = ZImg(info);
          ZImgFormat::fixDimensionOrder(fileBuf.data(), dimensionOrder, res,
                                        pixelTypeIsBGR(sb.directoryEntry.pixelType));
        } else {
          ZImgFreeImageInstance.readInfo(fileBuf.data(), sb.dataSize, info);
          res = ZImg(info);
          ZImgFreeImageInstance.readImg(fileBuf.data(), sb.dataSize, res.timeData<uint8_t>(0), res.byteNumber());
        }
      } catch (const ZException&) {
        throw ZIOException(QString("not supported compression type %1").arg(sb.directoryEntry.compression));
      }
      break;
  }

  return res;
}

}

namespace nim {

bool operator<(const CZITile& lhs, const CZITile& rhs)
{
  return std::tie(lhs.ratio, lhs.start.x, lhs.start.y, lhs.start.z, lhs.start.t, lhs.start.c) <
         std::tie(rhs.ratio, rhs.start.x, rhs.start.y, rhs.start.z, rhs.start.t, rhs.start.c);
}

ZImgCZISubBlock::ZImgCZISubBlock(const QString& fileName, std::vector<CZITile>& tiles,
                                 bool mixedTiles, size_t numChannels, size_t bytePerVoxel, VoxelFormat vf)
  : ZImgSubBlock(tiles[0].ratio, tiles[0].start.t, tiles[0].start.z, tiles[0].start.x, tiles[0].start.y,
                 tiles[0].size.x, tiles[0].size.y)
  , m_filename(fileName)
  , m_mixedTiles(mixedTiles)
  , m_mixedTilesStart(ZVoxelCoordinate::Init::Maximum)
  , m_numChannels(numChannels)
  , m_bytePerVoxel(bytePerVoxel)
  , m_voxelFormat(vf)
{
  m_tiles.swap(tiles);
  if (m_mixedTiles) {
    int32_t endx = std::numeric_limits<int32_t>::min();
    int32_t endy = std::numeric_limits<int32_t>::min();
    for (size_t i = 0; i < m_tiles.size(); ++i) {
      m_mixedTilesStart = min(m_mixedTilesStart, m_tiles[i].start);
      endx = std::max(endx, m_tiles[i].start.x + m_tiles[i].size.x);
      endy = std::max(endy, m_tiles[i].start.y + m_tiles[i].size.y);
    }
    m_mixedTilesStart.c = 0;
    x = m_mixedTilesStart.x;
    y = m_mixedTilesStart.y;
    width = endx - x;
    height = endy - y;
  }
}

std::shared_ptr<ZImg> ZImgCZISubBlock::read() const
{
  try {
    if (m_tiles.empty()) {
      throw ZIOException("empty czi sub block");
    }
    std::shared_ptr<ZImg> res = std::make_shared<ZImg>();
    std::ifstream inputFileStream;
    openFileStream(inputFileStream, m_filename, std::ios_base::in | std::ios_base::binary);
    if (m_mixedTiles) {
      double scale = 1.0 / ratio;
      *res = ZImg(ZImgInfo(std::ceil(width * scale), std::ceil(height * scale), 1, m_numChannels, 1, m_bytePerVoxel,
                           m_voxelFormat));
      for (size_t i = 0; i < m_tiles.size(); ++i) {
        ZImg img = readCZITile(inputFileStream, m_tiles[i]);
        ZVoxelCoordinate tileStart = m_tiles[i].start - m_mixedTilesStart;
        tileStart.x *= scale;
        tileStart.y *= scale;
        res->pasteImg(img, tileStart);
      }
    } else {
      if (m_tiles.size() == 1) {
        *res = readCZITile(inputFileStream, m_tiles[0]);
      } else {
        std::vector<ZImg> imgs(m_tiles.size());
        for (size_t i = 0; i < m_tiles.size(); ++i) {
          imgs[i] = readCZITile(inputFileStream, m_tiles[i]);
        }
        *res = ZImg::cat(imgs, Dimension::C);
      }
    }
    return res;
  } catch (const ZException& e) {
    throw ZException(QString("read %1 error: %2").arg(m_filename).arg(e.what()));
  }
}

ZImgZeissCZI& ZImgZeissCZI::instance()
{
  static ZImgZeissCZI imgCzi;
  return imgCzi;
}

ZImgZeissCZI::ZImgZeissCZI()
{

}

ZImgZeissCZI::~ZImgZeissCZI()
{

}

ZImg ZImgZeissCZI::stackTiles(const QString& filename, size_t ch, size_t scene)
{
  clearInternalState();

  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

  SegmentHeader sh;
  readStream(inputFileStream, &sh, sizeof(SegmentHeader));

  if (std::strncmp(sh.id, "ZISRAWFILE", 15) != 0) {
    throw ZIOException("incorrect czi file header");
  }
  FileHeader fh;
  readStream(inputFileStream, &fh, sizeof(FileHeader));
  if (fh.updatePending) {
    throw ZIOException("can not read czi file with pending update");
  }

  std::vector<ZImgInfo> infos;
  detectInfos(infos, inputFileStream, fh);

  if (scene >= infos.size()) {
    throw ZIOException("invalid scene");
  }

  if (ch >= infos[scene].numChannels) {
    throw ZIOException("invalid channel");
  }

  std::vector<ZImg> imgs;
  for (auto it = m_sceneTiles[scene].cbegin(); it != m_sceneTiles[scene].cend(); ++it) {
    if (it->ratio == 1_usize && it->start.c == static_cast<int>(ch)) {
      imgs.push_back(readCZITile(inputFileStream, *it));
    }
  }
  return ZImg::cat(imgs, Dimension::Z);
}

ZImg ZImgZeissCZI::stackTiles(const QString& filename, size_t ch, size_t scene, const QString& inverseMaskFile,
                              size_t maskFilePyramidalLevel)
{
  clearInternalState();

  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

  SegmentHeader sh;
  readStream(inputFileStream, &sh, sizeof(SegmentHeader));

  if (std::strncmp(sh.id, "ZISRAWFILE", 15) != 0) {
    throw ZIOException("incorrect czi file header");
  }
  FileHeader fh;
  readStream(inputFileStream, &fh, sizeof(FileHeader));
  if (fh.updatePending) {
    throw ZIOException("can not read czi file with pending update");
  }

  std::vector<ZImgInfo> infos;
  detectInfos(infos, inputFileStream, fh);

  if (scene >= infos.size()) {
    throw ZIOException("invalid scene");
  }

  if (ch >= infos[scene].numChannels) {
    throw ZIOException("invalid channel");
  }

  ZImg inverseMask(inverseMaskFile);
  CHECK(inverseMask.isType<uint8_t>());
  double scale = std::pow(0.5, maskFilePyramidalLevel);

  std::vector<ZImg> imgs;
  for (auto it = m_sceneTiles[scene].cbegin(); it != m_sceneTiles[scene].cend(); ++it) {
    if (it->ratio == 1_usize && it->start.c == static_cast<int>(ch)) {
      int startX = std::max(0.0, std::floor(it->start.x * scale));
      int endX = std::min(inverseMask.width() * 1.0, startX + std::ceil(it->size.x * scale));
      int startY = std::max(0.0, std::floor(it->start.y * scale));
      int endY = std::min(inverseMask.height() * 1.0, startY + std::ceil(it->size.y * scale));
      bool pass = true;
      ZImgRegion region(startX, endX, startY, endY);
      for (ZImgRegionIterator<uint8_t> it = ZImgRegionIterator<uint8_t>(inverseMask, region);
           !it.isAtEnd(); it += 1) {
        if (*it > 0) {
          pass = false;
          break;
        }
      }
      if (!pass) {
        continue;
      }

      imgs.push_back(readCZITile(inputFileStream, *it));
    }
  }
  return ZImg::cat(imgs, Dimension::Z);
}

ZImg ZImgZeissCZI::correctShading(const QString& filename, size_t ch, size_t scene,
                                  const ZImg& modelZ, const ZImg& modelV, ZImgZeissCZI::CorrectionMode cm)
{
  CHECK(modelZ.isType<double>() && modelV.isType<double>() && modelZ.isSameSize(modelV));
  clearInternalState();

  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

  SegmentHeader sh;
  readStream(inputFileStream, &sh, sizeof(SegmentHeader));

  if (std::strncmp(sh.id, "ZISRAWFILE", 15) != 0) {
    throw ZIOException("incorrect czi file header");
  }
  FileHeader fh;
  readStream(inputFileStream, &fh, sizeof(FileHeader));
  if (fh.updatePending) {
    throw ZIOException("can not read czi file with pending update");
  }

  std::vector<ZImgInfo> infos;
  detectInfos(infos, inputFileStream, fh);

  if (scene >= infos.size()) {
    throw ZIOException("invalid scene");
  }

  if (ch >= infos[scene].numChannels) {
    throw ZIOException("invalid channel");
  }

  ZImgInfo info = infos[scene];
  info.numChannels = 1;

  ZImg img(info);
  double meanV = 0;
  double meanZ = 0;
  if (cm == CorrectionMode::IntensityRangeCorrected || cm == CorrectionMode::ZeroLightPreserved) {
    meanV = mean(modelV.channelData<double>(0), modelV.channelData<double>(0) + modelV.channelVoxelNumber());
  }
  if (cm == CorrectionMode::ZeroLightPreserved) {
    meanZ = mean(modelZ.channelData<double>(0), modelZ.channelData<double>(0) + modelZ.channelVoxelNumber());
  }
  for (auto it = m_sceneTiles[scene].cbegin(); it != m_sceneTiles[scene].cend(); ++it) {
    if (it->ratio == 1_usize && it->start.c == static_cast<int>(ch)) {
      ZImg origtile = readCZITile(inputFileStream, *it);
      ZImg tile = origtile.castTo<double>();
      origtile.clear();
      if (tile.isSameSize(modelV)) {
        switch (cm) {
          case CorrectionMode::ZeroLightPreserved:
            tile = (tile - modelZ) / modelV * meanV + meanZ;
            break;
          case CorrectionMode::IntensityRangeCorrected:
            tile = (tile - modelZ) / modelV * meanV;
            break;
          case CorrectionMode::Direct:
            tile = (tile - modelZ) / modelV;
            break;
          default:
            throw ZIOException("invalid correction mode");
            break;
        }
      } else {
        throw ZIOException("model type or size doesn't match image tile");
      }
      img.pasteImg(tile, ZVoxelCoordinate(it->start.x, it->start.y, 0));
    }
  }
  return img;
}

bool ZImgZeissCZI::supportRead() const
{
  return true;
}

bool ZImgZeissCZI::supportWrite() const
{
  return false;
}

QString ZImgZeissCZI::shortName() const
{
  return "CZI";
}

QString ZImgZeissCZI::fullName() const
{
  return "Carl Zeiss CZI";
}

QStringList ZImgZeissCZI::extensions() const
{
  QStringList res;
  res << "czi";
  return res;
}

void ZImgZeissCZI::readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                            std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                            std::vector<std::set<size_t>>* pyramidalRatios)
{
#ifdef DUMP_CZI_INFO
  dump(filename);
#endif
  clearInternalState();

  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

  SegmentHeader sh;
  readStream(inputFileStream, &sh, sizeof(SegmentHeader));

  if (std::strncmp(sh.id, "ZISRAWFILE", 15) != 0) {
    throw ZIOException("incorrect czi file header");
  }
  FileHeader fh;
  readStream(inputFileStream, &fh, sizeof(FileHeader));
  if (fh.updatePending) {
    throw ZIOException("can not read czi file with pending update");
  }

  detectInfos(infos, inputFileStream, fh);

  if (m_someTilesAreNot2D) {
    createDefaultSubBlocks(filename, infos, subBlocks, pyramidalRatios);
  } else {
    if (subBlocks) {
      subBlocks->resize(infos.size());
      for (size_t s = 0; s < infos.size(); ++s) {
        std::vector<CZITile> tiles;
#ifdef NO_MIXED_TILE
        bool hasMissingTiles = false;

        size_t currnetRatio = 0;
        int currentX = -1;
        int currentY = -1;
        int currentZ = -1;
        int currentT = -1;
        for (auto it = m_sceneTiles[s].cbegin(); it != m_sceneTiles[s].cend(); ++it) {
          const CZITile& tile = *it;
          if (currnetRatio < 1_usize || tile.ratio != currnetRatio || tile.start.x != currentX ||
              tile.start.y != currentY ||
              tile.start.z != currentZ || tile.start.t != currentT) {
            if (tiles.size() > infos[s].numChannels) {
              throw ZIOException("invalid tiles: too many channels");
            } else if (tiles.size() == infos[s].numChannels) {
              subBlocks->at(s).emplace_back(std::make_shared<ZImgCZISubBlock>(filename, tiles));
            } else if (!tiles.empty()) {
              hasMissingTiles = true;
              subBlocks->at(s).emplace_back(
                std::make_shared<ZImgCZISubBlock>(filename, tiles, true, infos[s].numChannels, infos[s].bytesPerVoxel,
                                                  infos[s].voxelFormat));
            }
            tiles.clear();
            currnetRatio = tile.ratio;
            currentX = tile.start.x;
            currentY = tile.start.y;
            currentZ = tile.start.z;
            currentT = tile.start.t;
          }
          tiles.push_back(tile);
        }
        if (!tiles.empty()) {
          subBlocks->at(s).emplace_back(std::make_shared<ZImgCZISubBlock>(filename, tiles));
          tiles.clear();
        }

        if (hasMissingTiles) {
          LOG(INFO) << "scene " << s << " has missing tiles";
        }
#else
        std::set<CZITile, MixedTilesSort> allMixedTiles;

        // if image tiles can be grouped into multiple channel tiles (tiles has same location and size but different channel), we
        // group these tiles and build normal ZImgCZISubBlock
        // if image tiles can not be grouped into multiple channel tiles (different channel tiles of similar location are
        // slightly shifted, e.g. channel 1 tile starts at (120, 20) while channel 2 tile starts at (122, 22)), we instead
        // build mixed tiles ZImgCZISubBlock. That is we read all tiles together, which can be slow if image is large.
        // If mixAllTilesIfNecessary is false, then we group tiles that can be grouped, and build mixed tiles ZImgCZISubBlock
        // from tiles that can not be grouped. This approach has a potential problem that if one grouped tile is surrounded by
        // a mixed tile. That grouped tile has to be drawn after the mixed tile, otherwise there will be a hole in the image
        // since mixed tile occlude grouped tile.
        bool mixAllTilesIfNecessary = true;
        bool hasMixedTiles = false;

        size_t currnetRatio = 0;
        int currentX = -1;
        int currentY = -1;
        int currentZ = -1;
        int currentT = -1;
        for (auto it = m_sceneTiles[s].cbegin(); it != m_sceneTiles[s].cend(); ++it) {
          const CZITile &tile = *it;
          if (currnetRatio < 1_usize || tile.ratio != currnetRatio || tile.start.x != currentX || tile.start.y != currentY ||
              tile.start.z != currentZ || tile.start.t != currentT) {
            if (tiles.size() == infos[s].numChannels) {
              subBlocks->at(s).emplace_back(std::make_shared<ZImgCZISubBlock>(filename, tiles));
            } else if (!tiles.empty()) {
              hasMixedTiles = true;

              //              LOG(INFO) << "";
              //              LOG(INFO) << "";
              //              LOG(INFO) << "";
              //              for (size_t tp=0; tp<tiles.size(); ++tp) {
              //                LOG(INFO) << tiles[tp].ratio << " " << tiles[tp].start << " " << tiles[tp].size;
              //              }

              if(mixAllTilesIfNecessary) {
                subBlocks->at(s).clear();
                break;
              } else {
                allMixedTiles.insert(tiles.begin(), tiles.end());
              }
            }
            tiles.clear();
            currnetRatio = tile.ratio;
            currentX = tile.start.x;
            currentY = tile.start.y;
            currentZ = tile.start.z;
            currentT = tile.start.t;
          }
          tiles.push_back(tile);
        }
        if (!tiles.empty() && (!mixAllTilesIfNecessary || !hasMixedTiles)) {
          subBlocks->at(s).emplace_back(std::make_shared<ZImgCZISubBlock>(filename, tiles));
          tiles.clear();
        }

        if (hasMixedTiles) {
          LOG(INFO) << "scene " << s << " with mixed tiles";
          if (mixAllTilesIfNecessary) {
            CHECK(allMixedTiles.empty());
            allMixedTiles.insert(m_sceneTiles[s].cbegin(), m_sceneTiles[s].cend());
          }

          // process mixed tiles
          currnetRatio = 0;
          currentZ = -1;
          currentT = -1;
          for (auto it = allMixedTiles.cbegin(); it != allMixedTiles.cend(); ++it) {
            const CZITile &tile = *it;
            if (currnetRatio < 1_usize || tile.ratio != currnetRatio || tile.start.z != currentZ || tile.start.t != currentT) {
              if (!tiles.empty()) {
                subBlocks->at(s).emplace_back(std::make_shared<ZImgCZISubBlock>(filename, tiles, true, infos[s].numChannels, infos[s].bytesPerVoxel,
                                                                                infos[s].voxelFormat));
                tiles.clear();
              }
              currnetRatio = tile.ratio;
              currentZ = tile.start.z;
              currentT = tile.start.t;
            }
            tiles.push_back(tile);
          }
          if (!tiles.empty()) {
            subBlocks->at(s).emplace_back(std::make_shared<ZImgCZISubBlock>(filename, tiles, true, infos[s].numChannels, infos[s].bytesPerVoxel,
                                                                            infos[s].voxelFormat));
          }
        }
#endif
      }
    }
    if (pyramidalRatios) {
      pyramidalRatios->resize(infos.size());
      for (size_t s = 0; s < infos.size(); ++s) {
        for (auto it = m_sceneTiles[s].cbegin(); it != m_sceneTiles[s].cend(); ++it) {
          CHECK(it->ratio >= 1);
          pyramidalRatios->at(s).insert(it->ratio);
        }
      }
    }
  }

  for (size_t i = 0; i < infos.size(); ++i) {
    LOG(INFO) << infos[i].toQString();
  }
  LOG(INFO) << "";
}

void ZImgZeissCZI::readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene)
{
  clearInternalState();

  Q_UNUSED(scene)

  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

  SegmentHeader sh;
  readStream(inputFileStream, &sh, sizeof(SegmentHeader));

  if (std::strncmp(sh.id, "ZISRAWFILE", 15) != 0) {
    throw ZIOException("incorrect czi file header");
  }
  FileHeader fh;
  readStream(inputFileStream, &fh, sizeof(FileHeader));
  if (fh.updatePending) {
    throw ZIOException("can not read czi file with pending update");
  }

  inputFileStream.seekg(fh.metaDataPosition);
  readStream(inputFileStream, &sh, sizeof(SegmentHeader));

  if (std::strncmp(sh.id, "ZISRAWMETADATA", 15) == 0) {
    MetaDataSegment md;
    readStream(inputFileStream, &md, sizeof(MetaDataSegment));
    std::vector<char> xmlBuffer(md.xmlSize);
    readStream(inputFileStream, xmlBuffer.data(), md.xmlSize);
    QString xmlString = QString::fromUtf8(xmlBuffer.data(), md.xmlSize);
    xmlString.remove(QChar::Null);

    ZImgMetatag tag("metadata", xmlString);
    meta.attachToTopLevel(tag);
  }
}

void
ZImgZeissCZI::readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene)
{
  // todo
  Q_UNUSED(filename)
  Q_UNUSED(thumbnail)
  Q_UNUSED(region)
  Q_UNUSED(scene)
}

void ZImgZeissCZI::readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene, size_t ratio)
{
  clearInternalState();

  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

  SegmentHeader sh;
  readStream(inputFileStream, &sh, sizeof(SegmentHeader));

  if (std::strncmp(sh.id, "ZISRAWFILE", 15) != 0) {
    throw ZIOException("incorrect czi file header");
  }
  FileHeader fh;
  readStream(inputFileStream, &fh, sizeof(FileHeader));
  if (fh.updatePending) {
    throw ZIOException("can not read czi file with pending update");
  }

  std::vector<ZImgInfo> infos;
  detectInfos(infos, inputFileStream, fh);

  if (scene >= infos.size()) {
    throw ZIOException("invalid scene");
  }
  ZImgInfo& info = infos[scene];

  if (region.isEmpty() || !region.isValid(info)) {
    throw ZIOException(
      QString("Invalid image region. Image info: '%1', region: '%2'").arg(info.toQString()).arg(region.toQString()));
  }

  ZImgRegion rgn = region;
  rgn.resolveRegionEnd(info);

  std::set<size_t> pyRatios;
  for (auto it = m_sceneTiles[scene].cbegin(); it != m_sceneTiles[scene].cend(); ++it) {
    CHECK(it->ratio >= 1);
    pyRatios.insert(it->ratio);
  }

  CHECK(ratio >= 1);
  size_t readRatio = 0;
  for (auto it = pyRatios.cbegin(); it != pyRatios.cend(); ++it) {
    if (*it <= ratio) {
      readRatio = *it;
    } else {
      break;
    }
  }

  double scale = 1.0 / readRatio;

  if (rgn.containsWholeImg(info)) {
    if (readRatio > 1) {
      info.width = std::ceil(info.width * scale);
      info.height = std::ceil(info.height * scale);
      info.voxelSizeX /= scale;
      info.voxelSizeY /= scale;
    }
    img = ZImg(info);
    for (auto it = m_sceneTiles[scene].cbegin(); it != m_sceneTiles[scene].cend(); ++it) {
      auto& tile = *it;
      if (tile.ratio == readRatio) {
        ZImg tileImg = readCZITile(inputFileStream, tile);
        ZVoxelCoordinate start = tile.start;
        if (readRatio > 1) {
          start.x *= scale;
          start.y *= scale;
        }
        img.pasteImg(tileImg, start);
      }
      if (tile.ratio > readRatio) {
        break;
      }
    }
  } else {
    ZImgInfo resInfo = rgn.clip(info);
    if (readRatio > 1) {
      resInfo.width = std::ceil(resInfo.width * scale);
      resInfo.height = std::ceil(resInfo.height * scale);
      resInfo.voxelSizeX /= scale;
      resInfo.voxelSizeY /= scale;
    }
    img = ZImg(resInfo);
    ZBBox<ZVoxelCoordinate> imgBox(rgn.start, rgn.end - 1);
    for (auto it = m_sceneTiles[scene].cbegin(); it != m_sceneTiles[scene].cend(); ++it) {
      auto& tile = *it;
      if (tile.ratio == readRatio) {
        ZBBox<ZVoxelCoordinate> tileBox(tile.start, tile.start + tile.size - 1);
        if (imgBox.conjoint(tileBox)) {
          ZImg tileImg = readCZITile(inputFileStream, tile);
          ZVoxelCoordinate start = tile.start - rgn.start;
          if (readRatio > 1) {
            start.x *= scale;
            start.y *= scale;
          }
          img.pasteImg(tileImg, start);
        }
      }
      if (tile.ratio > readRatio) {
        break;
      }
    }
  }

  ZImgMetatag tag("metadata", m_metadataXmlString);
  img.metadataRef().attachToTopLevel(tag);

  if (ratio > readRatio) {
    img.zoom(1.0 * readRatio / ratio, 1.0 * readRatio / ratio);
  }
}

void ZImgZeissCZI::clearInternalState()
{
  m_metadataXmlString.clear();
  m_hasVoxelSizeInfo = false;
  m_voxelSizeX = -1;
  m_voxelSizeY = -1;
  m_voxelSizeZ = -1;
  m_hasChannelInfo = false;
  m_channelColors.clear();
  m_channelNames.clear();
  m_channelPixelType.clear();
  m_channelValidBitCount.clear();
  m_hasSceneInfo = false;
  m_sceneCenterX.clear();
  m_sceneCenterY.clear();

  m_channelColorsFromDisplaySettings.clear();
  m_channelNamesFromDisplaySettings.clear();
  m_shouldSeparateChannelsToDifferentScenes = false;
  m_someTilesAreNot2D = false;

  m_sceneTiles.clear();
  m_sceneStart.clear();
  m_sceneEnd.clear();
}

int64_t ZImgZeissCZI::checkFilename(const QString& filename)
{
  QFileInfo fi(filename);
  if (!fi.exists() || !fi.isFile()) {
    throw ZIOException(QString("File does not exist"));
  }
  int64_t filesize = fi.size();
  if (filesize < 16 + 8 + 8 + 512) {
    throw ZIOException(QString("Invalid CZI file"));
  }
  return filesize;
}

void ZImgZeissCZI::readCZIInfo(const QString& xmlString)
{
  // QXmlStreamReader takes any QIODevice.
  QXmlStreamReader xml(xmlString);

  // We'll parse the XML until we reach end of it.
  while (!xml.atEnd() && !xml.hasError()) {
    /* Read next element.*/
    QXmlStreamReader::TokenType token = xml.readNext();
    // If token is just StartDocument, we'll go to next.
    if (token == QXmlStreamReader::StartDocument) {
      continue;
    }
    // If token is StartElement, we'll see if we can read it.
    if (token == QXmlStreamReader::StartElement) {
      if (xml.name() != "Metadata") {
        continue;
      }
      parseMetadata(xml);
      break;
    }
  }
  // Error handling.
  if (xml.hasError()) {
    throw ZIOException(QString("error parsing czi metadata xml: %1").arg(xml.errorString()));
  }
  xml.clear();
}

void ZImgZeissCZI::parseMetadata(QXmlStreamReader& xml)
{
  Q_ASSERT(xml.isStartElement() && xml.name() == "Metadata");

  while (xml.readNextStartElement()) {
    if (xml.name() == "Information") {
      while (xml.readNextStartElement()) {
        if (xml.name() == "Image") {
          while (xml.readNextStartElement()) {
            if (xml.name() == "Dimensions") {
              while (xml.readNextStartElement()) {
                if (xml.name() == "Channels") {
                  while (xml.readNextStartElement()) {
                    if (xml.name() == "Channel") {
                      parseChannel(xml);
                    } else {
                      xml.skipCurrentElement();
                    }
                  }
                } else if (xml.name() == "S") {
                  while (xml.readNextStartElement()) {
                    if (xml.name() == "Scenes") {
                      while (xml.readNextStartElement()) {
                        if (xml.name() == "Scene") {
                          parseScene(xml);
                        } else {
                          xml.skipCurrentElement();
                        }
                      }
                    } else {
                      xml.skipCurrentElement();
                    }
                  }
                } else {
                  xml.skipCurrentElement();
                }
              }
            } else {
              xml.skipCurrentElement();
            }
          }
        } else {
          xml.skipCurrentElement();
        }
      }
    } else if (xml.name() == "Scaling") {
      while (xml.readNextStartElement()) {
        if (xml.name() == "Items") {
          while (xml.readNextStartElement()) {
            if (xml.name() == "Distance") {
              parseDistance(xml);
            } else {
              xml.skipCurrentElement();
            }
          }
        } else {
          xml.skipCurrentElement();
        }
      }
    } else if (xml.name() == "DisplaySetting") {
      while (xml.readNextStartElement()) {
        if (xml.name() == "Channels") {
          while (xml.readNextStartElement()) {
            if (xml.name() == "Channel") {
              parseDisplaySettingChannel(xml);
            } else {
              xml.skipCurrentElement();
            }
          }
        } else {
          xml.skipCurrentElement();
        }
      }
    } else {
      xml.skipCurrentElement();
    }
  }

  if (m_voxelSizeX > 0 && m_voxelSizeY > 0) {
    m_hasVoxelSizeInfo = true;
    m_voxelSizeX *= 1e6;
    m_voxelSizeY *= 1e6;
    m_voxelSizeZ = m_voxelSizeZ <= 0 ? 1 : (m_voxelSizeZ * 1e6);
    //LOG(INFO) << m_voxelSizeX << " " << m_voxelSizeY << " " << m_voxelSizeZ;
  }

  if (m_channelColors.size() < m_channelNames.size()) {
    m_channelColors.clear();
  }
  if (m_channelPixelType.size() < m_channelNames.size()) {
    m_channelPixelType.clear();
  }
  if (m_channelValidBitCount.size() < m_channelNames.size()) {
    m_channelValidBitCount.clear();
  }
  if (m_channelColorsFromDisplaySettings.size() < m_channelNamesFromDisplaySettings.size()) {
    m_channelColorsFromDisplaySettings.clear();
  }

  if (m_channelNamesFromDisplaySettings.size() == m_channelNames.size()) {
    for (size_t i = 0; i < m_channelNames.size(); ++i) {
      if (m_channelNames[i].isEmpty())
        m_channelNames[i] = m_channelNamesFromDisplaySettings[i];
    }
    if (m_channelColors.empty() && !m_channelColorsFromDisplaySettings.empty())
      m_channelColors = m_channelColorsFromDisplaySettings;
  }
  //  for (size_t i=0; i<channelNames.size(); ++i) {
  //    LOG(INFO) << channelNames[i] << " " << channelIs12Bit[i] << " " << channelPixelType[i];
  //  }
  // channels have different data types
  for (size_t i = 1; i < m_channelPixelType.size(); ++i) {
    if (m_channelPixelType[i] != m_channelPixelType[0]) {
      m_shouldSeparateChannelsToDifferentScenes = true;
      break;
    }
  }
  // more than 1 channel and each channel contains BGR image
  if (!m_shouldSeparateChannelsToDifferentScenes && m_channelPixelType.size() > 1 &&
      pixelTypeIsBGR(m_channelPixelType[0])) {
    m_shouldSeparateChannelsToDifferentScenes = true;
  }

  if (m_channelNames.size() > 0)
    m_hasChannelInfo = true;

  if (m_sceneCenterX.size() > 0 && m_sceneCenterX.size() == m_sceneCenterY.size()) {
    m_hasSceneInfo = true;
  }
}

void ZImgZeissCZI::parseChannel(QXmlStreamReader& xml)
{
  QString name;
  bool hasColor = false;
  col4 col;
  bool hasPixelType = false;
  int pixelType = -1;
  bool hasBC = false;
  int bc = 0;

  QXmlStreamAttributes attributes = xml.attributes();
  if (attributes.hasAttribute("Name")) {
    name = attributes.value("Name").toString();
  }

  bool ok;
  while (xml.readNextStartElement()) {
    if (xml.name() == "Color") {
      QString colorStr = xml.readElementText();
      if (colorStr.startsWith("#")) {
        colorStr = colorStr.mid(1);
      }
      if (colorStr.size() == 8) {
        colorStr = colorStr.mid(2);
      }
      if (colorStr.size() == 6) {
        int color = colorStr.toInt(&ok, 16);
        if (ok) {
          memcpy(&col, &color, 3);
          std::swap(col.r, col.b);
          col.a = 255;
          hasColor = true;
        } else {
          LOG(WARNING) << "can not parse czi channel color " << colorStr;
        }
      }
    } else if (xml.name() == "PixelType") {
      QString pixelTypeStr = xml.readElementText();
      if (pixelTypeStr.isEmpty()) {
        throw ZIOException("Can not parse czi channel pixel type");
      } else if (pixelTypeStr.compare("Gray8", Qt::CaseInsensitive) == 0) {
        pixelType = 0;
      } else if (pixelTypeStr.compare("Gray16", Qt::CaseInsensitive) == 0) {
        pixelType = 1;
      } else if (pixelTypeStr.compare("Gray32Float", Qt::CaseInsensitive) == 0) {
        pixelType = 2;
      } else if (pixelTypeStr.compare("Bgr24", Qt::CaseInsensitive) == 0) {
        pixelType = 3;
      } else if (pixelTypeStr.compare("Bgr48", Qt::CaseInsensitive) == 0) {
        pixelType = 4;
      } else if (pixelTypeStr.compare("Bgr96Float", Qt::CaseInsensitive) == 0) {
        pixelType = 8;
      } else if (pixelTypeStr.compare("Bgra32", Qt::CaseInsensitive) == 0) {
        pixelType = 9;
      } else if (pixelTypeStr.compare("Gray64ComplexFloat", Qt::CaseInsensitive) == 0) {
        pixelType = 10;
      } else if (pixelTypeStr.compare("Gray192ComplexFloat", Qt::CaseInsensitive) == 0) {
        pixelType = 11;
      } else if (pixelTypeStr.compare("Gray32", Qt::CaseInsensitive) == 0) {
        pixelType = 12;
      } else if (pixelTypeStr.compare("Gray64", Qt::CaseInsensitive) == 0) {
        pixelType = 13;
      } else {
        throw ZIOException(QString("Not supported czi pixel type: %1").arg(pixelTypeStr));
      }
      hasPixelType = true;
    } else if (xml.name() == "ComponentBitCount") {
      bc = xml.readElementText().toInt(&ok);
      if (!ok)
        throw ZIOException("Can not parse czi bit count");
      hasBC = true;
    } else {
      xml.skipCurrentElement();
    }
  }

  m_channelNames.push_back(name);
  if (hasBC)
    m_channelValidBitCount.push_back(bc);
  if (hasPixelType)
    m_channelPixelType.push_back(pixelType);
  if (hasColor) {
    m_channelColors.push_back(col);
  }
}

void ZImgZeissCZI::parseScene(QXmlStreamReader& xml)
{
  double sceneCenterX;
  double sceneCenterY;

  bool ok;
  while (xml.readNextStartElement()) {
    if (xml.name() == "CenterPosition") {
      QString centerPositionStr = xml.readElementText();
      QStringList nums = centerPositionStr.split(",");
      if (nums.size() != 2)
        return;
      sceneCenterX = nums[0].toDouble(&ok);
      if (!ok)
        return;
      sceneCenterY = nums[1].toDouble(&ok);
      if (!ok)
        return;
      m_sceneCenterX.push_back(sceneCenterX);
      m_sceneCenterY.push_back(sceneCenterY);
    } else {
      xml.skipCurrentElement();
    }
  }
}

void ZImgZeissCZI::parseDistance(QXmlStreamReader& xml)
{
  QXmlStreamAttributes attributes = xml.attributes();
  if (attributes.hasAttribute("Id")) {
    bool ok;
    if (attributes.value("Id").toString() == "X") {
      while (xml.readNextStartElement()) {
        if (xml.name() == "Value") {
          m_voxelSizeX = xml.readElementText().toDouble(&ok);
          if (!ok)
            throw ZIOException("Can not parse Distance X");
        } else {
          xml.skipCurrentElement();
        }
      }
    } else if (attributes.value("Id").toString() == "Y") {
      while (xml.readNextStartElement()) {
        if (xml.name() == "Value") {
          m_voxelSizeY = xml.readElementText().toDouble(&ok);
          if (!ok)
            throw ZIOException("Can not parse Distance Y");
        } else {
          xml.skipCurrentElement();
        }
      }
    } else if (attributes.value("Id").toString() == "Z") {
      while (xml.readNextStartElement()) {
        if (xml.name() == "Value") {
          m_voxelSizeZ = xml.readElementText().toDouble(&ok);
          if (!ok)
            throw ZIOException("Can not parse Distance Z");
        } else {
          xml.skipCurrentElement();
        }
      }
    } else {
      xml.skipCurrentElement();
    }
  } else {
    throw ZIOException("missing items id attribute");
  }
}

void ZImgZeissCZI::parseDisplaySettingChannel(QXmlStreamReader& xml)
{
  QString name;
  bool hasColor = false;
  col4 col;

  QXmlStreamAttributes attributes = xml.attributes();
  if (attributes.hasAttribute("Name")) {
    name = attributes.value("Name").toString();
  }

  bool ok;
  while (xml.readNextStartElement()) {
    if (xml.name() == "Color") {
      QString colorStr = xml.readElementText();
      if (colorStr.startsWith("#")) {
        colorStr = colorStr.mid(1);
      }
      if (colorStr.size() == 8) {
        colorStr = colorStr.mid(2);
      }
      if (colorStr.size() == 6) {
        int color = colorStr.toInt(&ok, 16);
        if (ok) {
          memcpy(&col, &color, 3);
          std::swap(col.r, col.b);
          col.a = 255;
          hasColor = true;
        } else {
          LOG(WARNING) << "can not parse czi channel color " << colorStr;
        }
      }
    } else {
      xml.skipCurrentElement();
    }
  }

  m_channelNamesFromDisplaySettings.push_back(name);
  if (hasColor) {
    m_channelColorsFromDisplaySettings.push_back(col);
  }
}

void ZImgZeissCZI::detectInfos(std::vector<ZImgInfo>& infos, std::ifstream& inputFileStream, FileHeader& fh)
{
  SegmentHeader sh;
  inputFileStream.seekg(fh.metaDataPosition);
  readStream(inputFileStream, &sh, sizeof(SegmentHeader));

  if (std::strncmp(sh.id, "ZISRAWMETADATA", 15) == 0) {
    MetaDataSegment md;
    readStream(inputFileStream, &md, sizeof(MetaDataSegment));
    std::vector<char> xmlBuffer(md.xmlSize);
    readStream(inputFileStream, xmlBuffer.data(), md.xmlSize);
    m_metadataXmlString = QString::fromUtf8(xmlBuffer.data(), md.xmlSize);
    m_metadataXmlString.remove(QChar::Null);

    readCZIInfo(m_metadataXmlString);
    //LOG(INFO) << m_metadataXmlString;
  } else {
    LOG(WARNING) << "no metadata in czi file";
  }

  inputFileStream.seekg(fh.directoryPosition);
  readStream(inputFileStream, &sh, sizeof(SegmentHeader));

  if (std::strncmp(sh.id, "ZISRAWDIRECTORY", 15) != 0) {
    throw ZIOException("empty czi file");
  }
  subBlockDirectorySegment sd;
  readStream(inputFileStream, &sd, sizeof(subBlockDirectorySegment));
  if (sd.entryCount <= 0) {
    throw ZIOException("no data block in czi");
  }

  std::vector<CZITile> allTiles;
  std::map<int32_t, int32_t> chPixelType;
  int idx = 0;
  while (idx < sd.entryCount) {
    CZITile tile;
    DirectoryEntryDV de;
    readStream(inputFileStream, &de, sizeof(DirectoryEntryDV));
    std::vector<DimensionEntryDV1> dimensionEntries(de.dimensionCount);
    readStream(inputFileStream, dimensionEntries.data(), sizeof(DimensionEntryDV1) * dimensionEntries.size());
    tile.compression = de.compression;
    tile.filePosition = de.filePosition;
    tile.pixelType = de.pixelType;
    if (de.pyramidType == 0)
      tile.ratio = 1;
    bool tileValid = true;
    for (size_t i = 0; i < dimensionEntries.size(); ++i) {
      DimensionEntryDV1 dimE = dimensionEntries[i];
      QString dimStr = QString::fromUtf8(dimE.dimension, 4);
      dimStr = dimStr.trimmed();
      dimStr.remove(QChar::Null);
      if (dimE.size <= 0 || dimE.storedSize <= 0) {
        tileValid = false;
        break;
      }
      if (dimStr == "X") {
        tile.start.x = dimE.start;
        tile.size.x = dimE.size;
        if (de.pyramidType != 0) {
          size_t l = std::round(double(dimE.size) / dimE.storedSize);
          if (l < 2) {
            tileValid = false;
            break;
          }
          if (tile.ratio > 1_usize && tile.ratio != l) {
            throw ZIOException("level does not match");
          } else {
            tile.ratio = l;
          }
        } else if (dimE.storedSize != dimE.size) {
          tileValid = false;
          break;
        }
        tile.storedSize.x = dimE.storedSize;
        tile.dimensionOrder.push_back('X');
      } else if (dimStr == "Y") {
        tile.start.y = dimE.start;
        tile.size.y = dimE.size;
        if (de.pyramidType != 0) {
          size_t l = std::round(double(dimE.size) / dimE.storedSize);
          if (l < 2) {
            tileValid = false;
            break;
          }
          if (tile.ratio > 1_usize && tile.ratio != l) {
            throw ZIOException("level does not match");
          } else {
            tile.ratio = l;
          }
        } else if (dimE.storedSize != dimE.size) {
          tileValid = false;
          break;
        }
        tile.storedSize.y = dimE.storedSize;
        tile.dimensionOrder.push_back('Y');
      } else if (dimStr == "C") {
        tile.start.c = dimE.start;
        tile.size.c = dimE.size;
        tile.sceneIdx[5] = dimE.start;
        // check data type of each tile
        auto it = chPixelType.find(tile.start.c);
        if (it == chPixelType.end()) {
          chPixelType[tile.start.c] = tile.pixelType;
        } else if (it->second != tile.pixelType) {
          throw ZIOException("inconsistent czi channel pixel type");
        }

        tile.storedSize.c = dimE.storedSize;
        tile.dimensionOrder.push_back('C');
      } else if (dimStr == "Z") {
        tile.start.z = dimE.start;
        tile.size.z = dimE.size;
        tile.storedSize.z = dimE.storedSize;
        tile.dimensionOrder.push_back('Z');
      } else if (dimStr == "T") {
        tile.start.t = dimE.start;
        tile.size.t = dimE.size;
        tile.storedSize.t = dimE.storedSize;
        tile.dimensionOrder.push_back('T');
      } else if (dimStr == "R") {  // V, H, I, S, R, C
        if (dimE.size != 1) {
          throw ZIOException("multiple rotations in one tile");
        }
        tile.sceneIdx[4] = dimE.start;
      } else if (dimStr == "S") {  // V, H, I, S, R, C
        if (dimE.size != 1) {
          throw ZIOException("multiple scenes in one tile");
        }
        tile.sceneIdx[3] = dimE.start;
      } else if (dimStr == "I") {  // V, H, I, S, R, C
        if (dimE.size != 1) {
          throw ZIOException("multiple illuminations in one tile");
        }
        tile.sceneIdx[2] = dimE.start;
      } else if (dimStr == "H") {  // V, H, I, S, R, C
        if (dimE.size != 1) {
          throw ZIOException("multiple phases in one tile");
        }
        tile.sceneIdx[1] = dimE.start;
      } else if (dimStr == "V") {  // V, H, I, S, R, C
        if (dimE.size != 1) {
          throw ZIOException("multiple views in one tile");
        }
        tile.sceneIdx[0] = dimE.start;
      }
    }

    if (tileValid) {
      if (tile.storedSize.z != 1 || tile.storedSize.t != 1) {
        m_someTilesAreNot2D = true;
      }

      // fix dimension order
      if (!tile.dimensionOrder.contains('T'))
        tile.dimensionOrder.push_back('T');
      if (!tile.dimensionOrder.contains('C'))
        tile.dimensionOrder.push_back('C');
      if (!tile.dimensionOrder.contains('Z'))
        tile.dimensionOrder.push_back('Z');
      if (!tile.dimensionOrder.contains('Y')) { // unlikely
        if (tile.dimensionOrder[0] == 'X') {
          tile.dimensionOrder.insert(1, 'Y');
        } else {
          tile.dimensionOrder.push_front('Y');
        }
      }
      if (!tile.dimensionOrder.contains('X'))  // unlikely
        tile.dimensionOrder.push_front('X');

      allTiles.push_back(tile);
    }

    ++idx;
  }

  // we need to check again whether we can merge channels to a single image
  if (!m_shouldSeparateChannelsToDifferentScenes && chPixelType.size() > 1) {
    auto it = chPixelType.cbegin();
    int32_t pixelType = it->second;
    for (++it; it != chPixelType.cend(); ++it) {
      if (pixelType != it->second) {
        m_shouldSeparateChannelsToDifferentScenes = true;
        break;
      }
    }
  }
  // if we have more than 1 channel and each channel has multiple channel image, then we still need to separate channels to scenes
  if (!m_shouldSeparateChannelsToDifferentScenes && chPixelType.size() > 1 &&
      pixelTypeIsBGR(chPixelType.cbegin()->second)) {
    m_shouldSeparateChannelsToDifferentScenes = true;
  }

  std::map<std::array<int32_t, 6>, std::vector<CZITile>> sceneIdxToTiles;
  for (size_t i = 0; i < allTiles.size(); ++i) {
    if (!m_shouldSeparateChannelsToDifferentScenes) { // remove channel info from scene idx
      allTiles[i].sceneIdx[5] = 0;
    } else {   // split channels to different scenes
      CHECK(allTiles[i].size.c == 1);
      allTiles[i].start.c = 0;
    }
    sceneIdxToTiles[allTiles[i].sceneIdx].push_back(allTiles[i]);
  }

  int channelStart = std::numeric_limits<int32_t>::max();
  if (m_shouldSeparateChannelsToDifferentScenes &&
      m_hasChannelInfo) { // find channel start to know how to map info from metadata to different scenes
    for (auto it = sceneIdxToTiles.cbegin(); it != sceneIdxToTiles.cend(); ++it) {
      channelStart = std::min(channelStart, it->first.at(5));
    }
  }

  for (auto it = sceneIdxToTiles.cbegin(); it != sceneIdxToTiles.cend(); ++it) {
    ZVoxelCoordinate start(ZVoxelCoordinate::Init::Maximum);
    ZVoxelCoordinate end(ZVoxelCoordinate::Init::Minimum);

    for (size_t i = 0; i < it->second.size(); ++i) {
      const CZITile& tile = it->second.at(i);
      if (tile.ratio != 1_usize)
        continue;
      start = min(start, tile.start);
      end = max(end, tile.start + tile.size);
    }

    for (size_t i = 0; i < start.size(); ++i) {
      if (start[i] == std::numeric_limits<int32_t>::max()) {
        start[i] = 0;
        end[i] = 1;
      }
    }

    ZImgInfo info;
    info.width = end.x - start.x;
    info.height = end.y - start.y;
    info.depth = end.z - start.z;
    info.numChannels = end.c - start.c;
    info.numTimes = end.t - start.t;

    if (m_hasVoxelSizeInfo) {
      info.voxelSizeX = m_voxelSizeX;
      info.voxelSizeY = m_voxelSizeY;
      info.voxelSizeZ = m_voxelSizeZ;
      info.voxelSizeUnit = VoxelSizeUnit::um;
    }

    int pixelType = it->second.at(0).pixelType;
    switch (pixelType) {
      case 0:
        info.bytesPerVoxel = 1;
        info.voxelFormat = VoxelFormat::Unsigned;
        break;
      case 1:
        info.bytesPerVoxel = 2;
        info.voxelFormat = VoxelFormat::Unsigned;
        break;
      case 2:
        info.bytesPerVoxel = 4;
        info.voxelFormat = VoxelFormat::Float;
        break;
      case 3:
        CHECK(info.numChannels == 1);
        info.numChannels = 3;
        info.bytesPerVoxel = 1;
        info.voxelFormat = VoxelFormat::Unsigned;
        break;
      case 4:
        CHECK(info.numChannels == 1);
        info.numChannels = 3;
        info.bytesPerVoxel = 2;
        info.voxelFormat = VoxelFormat::Unsigned;
        break;
      case 8:
        CHECK(info.numChannels == 1);
        info.numChannels = 3;
        info.bytesPerVoxel = 4;
        info.voxelFormat = VoxelFormat::Float;
        break;
      case 9:
        CHECK(info.numChannels == 1);
        info.numChannels = 4;
        info.lastChannelIsAlphaChannel = true;
        info.bytesPerVoxel = 1;
        info.voxelFormat = VoxelFormat::Unsigned;
        break;
      case 10:
        throw ZIOException("complex gray64 image not supported");
        break;
      case 11:
        throw ZIOException("complex bgr192 image not supported");
        break;
      case 12:
        info.bytesPerVoxel = 4;
        info.voxelFormat = VoxelFormat::Unsigned;
        break;
      case 13:
        info.bytesPerVoxel = 8;
        info.voxelFormat = VoxelFormat::Float;
        break;
      default:
        throw ZIOException("Wrong Pixel Type");
        break;
    }

    if (m_hasChannelInfo) {
      if (m_channelNames.size() == info.numChannels) {
        info.channelNames = m_channelNames;
      } else if (m_channelNames.size() > info.numChannels &&
                 info.numChannels == 1) {  // channels are separated to different scenes
        CHECK(m_shouldSeparateChannelsToDifferentScenes);
        int chIdx = it->first.at(5) - channelStart;
        CHECK(chIdx >= 0);
        if (chIdx >= static_cast<int>(m_channelNames.size())) {
          throw ZIOException("channel number does not match metadata");
        }
        info.channelNames.push_back(m_channelNames[chIdx]);
      }

      if (m_channelColors.size() == info.numChannels) {
        info.channelColors = m_channelColors;
      } else if (m_channelColors.size() > info.numChannels &&
                 info.numChannels == 1) {  // channels are separated to different scenes
        CHECK(m_shouldSeparateChannelsToDifferentScenes);
        int chIdx = it->first.at(5) - channelStart;
        CHECK(chIdx >= 0);
        if (chIdx >= static_cast<int>(m_channelColors.size())) {
          throw ZIOException("channel number does not match metadata");
        }
        info.channelColors.push_back(m_channelColors[chIdx]);
      }

      if (info.bytesPerVoxel == 2 && info.voxelFormat == VoxelFormat::Unsigned) {
        if (m_channelValidBitCount.size() == 1 || m_channelValidBitCount.size() == info.numChannels) {
          // set 12bit flag if all channels are 12 bit
          info.validBitCount = m_channelValidBitCount[0];
          for (size_t idx = 1; idx < m_channelValidBitCount.size(); ++idx)
            info.validBitCount = std::max(info.validBitCount, m_channelValidBitCount[idx]);
        } else if (m_channelValidBitCount.size() > info.numChannels &&
                   info.numChannels == 1) { // channels are separated to different scenes
          CHECK(m_shouldSeparateChannelsToDifferentScenes);
          int chIdx = it->first.at(5) - channelStart;
          CHECK(chIdx >= 0);
          if (chIdx >= static_cast<int>(m_channelValidBitCount.size())) {
            throw ZIOException("channel number does not match metadata");
          }
          info.validBitCount = m_channelValidBitCount[chIdx];
        }
      }
    }

    info.createDefaultDescriptions();
    infos.push_back(info);

    m_sceneStart.push_back(start);
    m_sceneEnd.push_back(end);
    m_sceneTiles.emplace_back();
    for (size_t j = 0; j < it->second.size(); ++j) {
      CZITile tile = it->second.at(j);
      for (size_t i = 0; i < tile.start.size(); ++i) {
        if (tile.start[i] == std::numeric_limits<int32_t>::max()) {
          tile.start[i] = 0;
          tile.size[i] = 1;
        }
      }
      tile.start -= start;
      m_sceneTiles[m_sceneTiles.size() - 1].insert(tile);
    }

#ifdef DUMP_CZI_INFO
    for (size_t j=0; j<20; ++j) {
      LOG(INFO) << "";
    }
    for (auto it=m_sceneTiles[m_sceneTiles.size()-1].cbegin();
         it != m_sceneTiles[m_sceneTiles.size()-1].cend(); ++it) {
      LOG(INFO) << it->ratio << " " << it->start << " " << it->size << " " << it->storedSize;
    }
#endif
  }

  if (m_hasSceneInfo) {
    if (m_sceneCenterX.size() == infos.size()) {
      for (size_t i = 0; i < infos.size(); ++i) {
        infos[i].position.push_back(m_sceneCenterX[i]);
        infos[i].position.push_back(m_sceneCenterY[i]);
      }
    }
  }
}

void ZImgZeissCZI::dump(const QString& filename)
{
  int64_t filesize = checkFilename(filename);

  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

  QString str("\n");
  dumpCZIStream(inputFileStream, filesize, 0, str, 0);
  LOG(INFO) << str;
}

void
ZImgZeissCZI::dumpCZIStream(std::ifstream& inputFileStream, int64_t filesize, int64_t offset, QString& str, int indent)
{
  int64_t nextSegPos = offset;
  do {
    inputFileStream.seekg(nextSegPos);
    SegmentHeader sh;
    readStream(inputFileStream, &sh, sizeof(SegmentHeader));
    dumpSegmentInfo(sh, inputFileStream, str, indent);
    nextSegPos += sizeof(SegmentHeader) + sh.allocatedSize;
  } while (nextSegPos - offset < filesize - 1);
}

void ZImgZeissCZI::dumpSegmentInfo(const SegmentHeader& sh, std::ifstream& inputFileStream, QString& str, int indent)
{
  if (std::strncmp(sh.id, "ZISRAWFILE", 15) == 0) {
    dumpFileHeaderSegment(inputFileStream, str, indent);
  } else if (std::strncmp(sh.id, "ZISRAWDIRECTORY", 15) == 0) {
    dumpSubBlockDirectory(inputFileStream, str, indent);
  } else if (std::strncmp(sh.id, "ZISRAWSUBBLOCK", 15) == 0) {
    dumpSubBlockSegment(inputFileStream, str, indent);
  } else if (std::strncmp(sh.id, "ZISRAWMETADATA", 15) == 0) {
    dumpMetadataSegment(inputFileStream, str, indent);
  } else if (std::strncmp(sh.id, "ZISRAWATTACH", 15) == 0) {
    dumpAttachmentSegment(inputFileStream, str, indent);
  } else if (std::strncmp(sh.id, "ZISRAWATTDIR", 15) == 0) {
    dumpAttachmentDirectory(inputFileStream, str, indent);
  } else if (std::strncmp(sh.id, "DELETED", 15) == 0) {
    QString ind(indent, QChar(' '));
    str += QString("%1Deleted Segment\n\n").arg(ind);
  } else {
    throw ZIOException(QString("Invalid Segment ID"));
  }
}

void ZImgZeissCZI::dumpFileHeaderSegment(std::ifstream& inputFileStream, QString& str, int indent)
{
  QString ind(indent, QChar(' '));
  FileHeader fh;
  readStream(inputFileStream, &fh, sizeof(FileHeader));
  str += QString("%1FileHeaderSegment\n").arg(ind);
  str += QString("%1Major: %2\n").arg(ind).arg(fh.major);
  str += QString("%1Minor: %2\n").arg(ind).arg(fh.minor);
  str += QString("%1PrimaryFileGuid: %2\n").arg(ind).arg(boost::uuids::to_string(fh.primaryFileGuid).c_str());
  str += QString("%1FileGuid: %2\n").arg(ind).arg(boost::uuids::to_string(fh.fileGuid).c_str());
  str += QString("%1FilePart: %2\n").arg(ind).arg(fh.filePart);
  str += QString("%1DirectoryPosition: %2\n").arg(ind).arg(fh.directoryPosition);
  str += QString("%1MetadataPosition: %2\n").arg(ind).arg(fh.metaDataPosition);
  str += QString("%1UpdatePending: %2\n").arg(ind).arg(fh.updatePending > 0);
  str += QString("%1AttachmentDirectoryPosition: %2\n").arg(ind).arg(fh.attachmentDirectoryPosition);

  str += "\n";
}

void ZImgZeissCZI::dumpMetadataSegment(std::ifstream& inputFileStream, QString& str, int indent)
{
  QString ind(indent, QChar(' '));
  MetaDataSegment md;
  readStream(inputFileStream, &md, sizeof(MetaDataSegment));
  str += QString("%1MetadataSegment\n").arg(ind);
  str += QString("%1XmlSize: %2\n").arg(ind).arg(md.xmlSize);
  str += QString("%1AttachmentSize: %2\n").arg(ind).arg(md.attachmentSize);
  std::vector<char> xmlBuffer(md.xmlSize);
  readStream(inputFileStream, xmlBuffer.data(), md.xmlSize);
  QString xmlString = QString::fromUtf8(xmlBuffer.data(), md.xmlSize);
  xmlString.remove(QChar::Null);
  str += QString("%1XML: %2\n").arg(ind).arg(xmlString);

  str += "\n";
}

void ZImgZeissCZI::dumpSubBlockSegment(std::ifstream& inputFileStream, QString& str, int indent)
{
  QString ind(indent, QChar(' '));
  SubBlockSegment sb;
  readStream(inputFileStream, &sb, sizeof(SubBlockSegment));
  str += QString("%1SubBlockSegment\n").arg(ind);
  str += QString("%1MetadataSize: %2\n").arg(ind).arg(sb.metaDataSize);
  str += QString("%1AttachmentSize: %2\n").arg(ind).arg(sb.attachmentSize);
  str += QString("%1DataSize: %2\n").arg(ind).arg(sb.dataSize);
  std::vector<DimensionEntryDV1> dimensionEntries(sb.directoryEntry.dimensionCount);
  readStream(inputFileStream, dimensionEntries.data(), sizeof(DimensionEntryDV1) * dimensionEntries.size());
  dumpDirectoryEntry(sb.directoryEntry, str, indent);
  for (size_t i = 0; i < dimensionEntries.size(); ++i) {
    dumpDimensionEntry(dimensionEntries[i], str, indent);
  }
  int directoryEntrySize = sizeof(DirectoryEntryDV) + sizeof(DimensionEntryDV1) * sb.directoryEntry.dimensionCount;
  int fill = std::max(0, 256 - directoryEntrySize - 16);
  if (fill > 0) {
    inputFileStream.seekg(fill, std::ios_base::cur);
  }
  std::vector<char> xmlBuffer(sb.metaDataSize);
  readStream(inputFileStream, xmlBuffer.data(), sb.metaDataSize);
  QString xmlString = QString::fromUtf8(xmlBuffer.data(), sb.metaDataSize);
  xmlString.remove(QChar::Null);
  str += QString("%1Metadata: %2\n").arg(ind).arg(xmlString);
  // followed by data and attachment

  //  QString filename = "/Users/feng/Downloads/czi_tile_dump_2.tif";
  //  if (!QFile::exists(filename)) {
  //    std::vector<uint8_t> fileBuf(sb.dataSize);
  //    readStream(inputFileStream, fileBuf.data(), sb.dataSize);
  //    ZImgInfo info;
  //    m_jxrReader.readInfo(fileBuf.data(), fileBuf.size(), info);
  //    ZImg img(info);
  //    m_jxrReader.readImg(fileBuf.data(), fileBuf.size(), img.channelData<uint8_t>(0));
  //    img.save(filename);
  //  }

  str += "\n";
}

void ZImgZeissCZI::dumpDirectoryEntry(const DirectoryEntryDV& de, QString& str, int indent)
{
  QString ind(indent, QChar(' '));
  str += QString("%1SchemaType: %2%3\n").arg(ind).arg(de.schemaType[0]).arg(de.schemaType[1]);
  if (de.schemaType[0] != 'D' || de.schemaType[1] != 'V') {
    str += QString("%1Not Supported Directory Entry Schema Type\n").arg(ind);
    return;
  }
  QString pixelTypeStr;
  switch (de.pixelType) {
    case 0:
      pixelTypeStr = "Gray8";
      break;
    case 1:
      pixelTypeStr = "Gray16";
      break;
    case 2:
      pixelTypeStr = "Gray32Float";
      break;
    case 3:
      pixelTypeStr = "Bgr24";
      break;
    case 4:
      pixelTypeStr = "Bgr48";
      break;
    case 8:
      pixelTypeStr = "Bgr96Float";
      break;
    case 9:
      pixelTypeStr = "Bgra32";
      break;
    case 10:
      pixelTypeStr = "Gray64ComplexFloat";
      break;
    case 11:
      pixelTypeStr = "Bgr192ComplexFloat";
      break;
    case 12:
      pixelTypeStr = "Gray32 (32 Bit integer)";
      break;
    case 13:
      pixelTypeStr = "Gray64 (Double precision floating point)";
      break;
    default:
      pixelTypeStr = "Unknown";
      throw ZIOException("Unknown Pixel Type");
      break;
  }
  str += QString("%1PixelType: %2\n").arg(ind).arg(pixelTypeStr);
  str += QString("%1FilePosition: %2\n").arg(ind).arg(de.filePosition);
  str += QString("%1FilePart: %2\n").arg(ind).arg(de.filePart);
  QString compressionStr;
  switch (de.compression) {
    case 0:
      compressionStr = "Uncompressed";
      break;
    case 2:
      compressionStr = "LZW";
      break;
    case 1:
      compressionStr = "JpgFile";
      break;
    case 4:
      compressionStr = "JpegXrFile";
      break;
    default:
      compressionStr = QString("%1").arg(de.compression);
      break;
  }
  if (compressionStr.isEmpty()) {
    if (de.compression >= 100 && de.compression < 1000) {
      compressionStr = QString("Camera specific RAW data %1").arg(de.compression);
    } else if (de.compression > 1000) {
      compressionStr = QString("System specific RAW data %1").arg(de.compression);
    }
  }
  str += QString("%1Compression: %2\n").arg(ind).arg(compressionStr);
  QString pyramidTypeStr;
  switch (de.pyramidType) {
    case 0:
      pyramidTypeStr = "None";
      break;
    case 1:
      pyramidTypeStr = "SingleSubblock";
      break;
    case 2:
      pyramidTypeStr = "Multisubblock";
      break;
    default:
      pyramidTypeStr = "Unknown";
      break;
  }
  str += QString("%1PyramidType: %2\n").arg(ind).arg(pyramidTypeStr);
  str += QString("%1DimensionCount: %2\n").arg(ind).arg(de.dimensionCount);
}

void ZImgZeissCZI::dumpDimensionEntry(const DimensionEntryDV1& de, QString& str, int indent)
{
  QString ind(indent, QChar(' '));
  QString dimStr = QString::fromUtf8(de.dimension, 4);
  dimStr = dimStr.trimmed();
  dimStr.remove(QChar::Null);
  str += QString("%1Dimension: %2; Start: %3; Size: %4; StartCoordinate: %5; StoredSize: %6\n")
    .arg(ind).arg(dimStr).arg(de.start).arg(de.size).arg(de.startCoordinate).arg(de.storedSize);
}

void ZImgZeissCZI::dumpSubBlockDirectory(std::ifstream& inputFileStream, QString& str, int indent)
{
  QString ind(indent, QChar(' '));
  subBlockDirectorySegment sd;
  readStream(inputFileStream, &sd, sizeof(subBlockDirectorySegment));
  str += QString("%1SubBlockDirectorySegment\n").arg(ind);
  str += QString("%1EntryCount: %2\n").arg(ind).arg(sd.entryCount);

  int idx = 0;
  while (idx < sd.entryCount) {
    str += QString("%1Entry %2:\n").arg(ind).arg(idx);
    DirectoryEntryDV de;
    readStream(inputFileStream, &de, sizeof(DirectoryEntryDV));
    std::vector<DimensionEntryDV1> dimensionEntries(de.dimensionCount);
    readStream(inputFileStream, dimensionEntries.data(), sizeof(DimensionEntryDV1) * dimensionEntries.size());
    dumpDirectoryEntry(de, str, indent);
    for (size_t i = 0; i < dimensionEntries.size(); ++i) {
      dumpDimensionEntry(dimensionEntries[i], str, indent);
    }
    ++idx;
  }

  str += "\n";
}

void ZImgZeissCZI::dumpAttachmentSegment(std::ifstream& inputFileStream, QString& str, int indent)
{
  QString ind(indent, QChar(' '));
  AttachmentSegment as;
  readStream(inputFileStream, &as, sizeof(AttachmentSegment));
  str += QString("%1AttachmentSegment\n").arg(ind);
  str += QString("%1DataSize: %2\n").arg(ind).arg(as.dataSize);
  dumpAttachmentEntry(as.attachmentEntry, str, indent);
  if (std::strncmp(as.attachmentEntry.name, "Experiment", 80) == 0 ||
      std::strncmp(as.attachmentEntry.name, "HardwareSetting", 80) == 0 ||
      std::strncmp(as.attachmentEntry.name, "MVM", 80) == 0) {
    std::vector<char> xmlBuffer(as.dataSize);
    readStream(inputFileStream, xmlBuffer.data(), as.dataSize);
    QString xmlString = QString::fromUtf8(xmlBuffer.data(), as.dataSize);
    xmlString.remove(QChar::Null);
    str += QString("%1Data: %2\n").arg(ind).arg(xmlString);
  } else if (std::strncmp(as.attachmentEntry.name, "TimeStamps", 80) == 0) {
    TimeStampSegment ts;
    readStream(inputFileStream, &ts, sizeof(TimeStampSegment));
    str += QString("%1Size: %2\n").arg(ind).arg(ts.size);
    str += QString("%1NumberTimeStamps: %2\n").arg(ind).arg(ts.numberTimeStamps);
    if (ts.numberTimeStamps > 0) {
      std::vector<double> timeStamps(ts.numberTimeStamps);
      readStream(inputFileStream, timeStamps.data(), 8 * ts.numberTimeStamps);
      str += QString("%1TimeStamps (s): %2").arg(ind).arg(timeStamps[0]);
      for (size_t i = 1; i < timeStamps.size(); ++i) {
        str += QString(" %1").arg(timeStamps[i]);
      }
      str += "\n";
    }
  } else if (std::strncmp(as.attachmentEntry.name, "EventList", 80) == 0) {
    EventListSegment el;
    readStream(inputFileStream, &el, sizeof(EventListSegment));
    str += QString("%1Size: %2\n").arg(ind).arg(el.size);
    str += QString("%1NumberEvents: %2\n").arg(ind).arg(el.numberEvents);
    if (el.numberEvents > 0) {
      int eventIdx = 0;
      while (eventIdx < el.numberEvents) {
        EventListEntry ele;
        readStream(inputFileStream, &ele, sizeof(EventListEntry));
        std::vector<char> descriptionBuffer(ele.descriptionSize);
        readStream(inputFileStream, descriptionBuffer.data(), ele.descriptionSize);
        QString desp = QString::fromUtf8(descriptionBuffer.data());
        str += QString("%1Size: %2\n").arg(ind).arg(ele.size);
        str += QString("%1Time (s): %2\n").arg(ind).arg(ele.time);
        QString eventTypeStr;
        switch (ele.eventType) {
          case 0:
            eventTypeStr = "Experimental annotation";
            break;
          case 1:
            eventTypeStr = "The time interval has changed";
            break;
          case 2:
            eventTypeStr = "Start of a bleach operation";
            break;
          case 3:
            eventTypeStr = "End of a bleach operation";
            break;
          case 4:
            eventTypeStr = "A trigger signal was detected on the user port of the electronic module.";
            break;
          default:
            eventTypeStr = "Unknown Type";
            break;
        }
        str += QString("%1EventType: %2\n").arg(ind).arg(eventTypeStr);
        str += QString("%1DescriptionSize: %2\n").arg(ind).arg(ele.descriptionSize);
        str += QString("%1Description: %2\n").arg(ind).arg(desp);
        ++eventIdx;
      }
    }
  } else if (std::strncmp(as.attachmentEntry.name, "FocusPositions", 80) == 0) {
    FocusPositions fp;
    readStream(inputFileStream, &fp, sizeof(FocusPositions));
    str += QString("%1Size: %2\n").arg(ind).arg(fp.size);
    str += QString("%1NumberPositions: %2\n").arg(ind).arg(fp.numberPositions);
    if (fp.numberPositions > 0) {
      std::vector<double> positions(fp.numberPositions);
      readStream(inputFileStream, positions.data(), 8 * fp.numberPositions);
      str += QString("%1Positions (um): %2").arg(ind).arg(positions[0]);
      for (size_t i = 1; i < positions.size(); ++i) {
        str += QString(" %1").arg(positions[i]);
      }
      str += "\n";
    }
  } else if (std::strncmp(as.attachmentEntry.name, "Label", 80) == 0 ||
             std::strncmp(as.attachmentEntry.name, "Prescan", 80) == 0 ||
             std::strncmp(as.attachmentEntry.name, "SlidePreview", 80) == 0) {
    dumpCZIStream(inputFileStream, as.dataSize, inputFileStream.tellg(), str, indent + 4);
  }

  str += "\n";
}

void ZImgZeissCZI::dumpAttachmentEntry(const AttachmentEntryA1& ae, QString& str, int indent)
{
  QString ind(indent, QChar(' '));
  str += QString("%1SchemaType: %2%3\n").arg(ind).arg(ae.schemaType[0]).arg(ae.schemaType[1]);
  if (ae.schemaType[0] != 'A' || ae.schemaType[1] != '1') {
    str += QString("%1Not Supported Attachment Entry Schema Type\n").arg(ind);
    return;
  }
  str += QString("%1FilePosition: %2\n").arg(ind).arg(ae.filePosition);
  str += QString("%1FilePart: %2\n").arg(ind).arg(ae.filePart);
  str += QString("%1ContentGuid: %2\n").arg(ind).arg(boost::uuids::to_string(ae.contentGuid).c_str());
  QString contentFileType = QString::fromUtf8(ae.contentFileType, 8);
  contentFileType = contentFileType.trimmed();
  contentFileType.remove(QChar::Null);
  str += QString("%1ContentFileType: %2\n").arg(ind).arg(contentFileType);
  str += QString("%1Name: %2\n").arg(ind).arg(ae.name);
}

void ZImgZeissCZI::dumpAttachmentDirectory(std::ifstream& inputFileStream, QString& str, int indent)
{
  QString ind(indent, QChar(' '));
  AttachmentDirectorySegment ad;
  readStream(inputFileStream, &ad, sizeof(AttachmentDirectorySegment));
  str += QString("%1AttachmentDirectorySegment\n").arg(ind);
  str += QString("%1EntryCount: %2\n").arg(ind).arg(ad.entryCount);
  std::vector<AttachmentEntryA1> entries(ad.entryCount);
  readStream(inputFileStream, entries.data(), ad.entryCount * sizeof(AttachmentEntryA1));
  for (size_t i = 0; i < entries.size(); ++i) {
    str += QString("%1Entry %2:\n").arg(ind).arg(i);
    dumpAttachmentEntry(entries[i], str, indent);
  }

  str += "\n";
}

} // namespace
