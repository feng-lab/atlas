#include "zimgpng.h"
#include <lodepng/lodepng.h>
#include "zioutils.h"
#include <memory>

namespace {

using namespace nim;

inline void checkPngError(const lodepng::State &state)
{
  if (state.error != 0) {
    throw ZIOException(lodepng_error_text(state.error));
  }
}

void readMetaDataFromState(const lodepng::State &state, ZImgMetadata &meta)
{
  if (state.info_png.time_defined) {
    LodePNGTime time = state.info_png.time;
    meta.attachToTopLevel(ZImgMetatag("Time", QString("%1-%2-%3T%4:%5:%6")
                                      .arg(time.year).arg(time.month).arg(time.day)
                                      .arg(time.hour).arg(time.minute).arg(time.second)));
  }
  for (size_t i=0; i<state.info_png.text_num; ++i) {
    meta.attachToTopLevel(ZImgMetatag(QString::fromUtf8(state.info_png.text_keys[i]),
                                      QString::fromUtf8(state.info_png.text_strings[i])));
  }
  for (size_t i=0; i<state.info_png.itext_num; ++i) {
    meta.attachToTopLevel(ZImgMetatag(QString::fromUtf8(state.info_png.itext_keys[i]),
                                      QString::fromUtf8(state.info_png.itext_strings[i])));
  }
}

void readInfoFromBuf(const std::vector<char>& buf, lodepng::State &state, ZImgInfo &info)
{
  uint32_t w;
  uint32_t h;

  state.decoder.ignore_crc = 1;
  lodepng_inspect(&w, &h, &state, reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
  checkPngError(state);

  info.width = w;
  info.height = h;
  info.depth = 1;
  switch (state.info_png.color.colortype) {
  case LCT_GREY:
    info.numChannels = 1;
    break;
  case LCT_GREY_ALPHA:
    info.numChannels = 2;
    info.lastChannelIsAlphaChannel = true;
    break;
  case LCT_RGB:
    info.numChannels = 3;
    break;
  case LCT_RGBA:
  case LCT_PALETTE:
    info.numChannels = 4;
    info.lastChannelIsAlphaChannel = true;
    break;
  default:
    throw ZIOException("not supported png colortype");
    break;
  }
  info.numTimes = 1;

  info.voxelFormat = VoxelFormat::Unsigned;
  info.bytesPerVoxel = std::max(static_cast<uint32_t>(1), state.info_png.color.bitdepth / 8);

  info.createDefaultDescriptions();

  if (state.info_png.phys_defined && state.info_png.phys_unit == 1) {
    info.voxelSizeUnit = VoxelSizeUnit::m;
    info.voxelSizeX = 1.0 / state.info_png.phys_x;
    info.voxelSizeY = 1.0 / state.info_png.phys_y;
  }
}

// convert RGBARGBA..... to RRR...GGG...BBB...AAA... and perform crop
void separateChannel(uint8_t *bufImg, const ZImgInfo &info, const ZImgRegion &region, ZImg &img)
{
  if (region.containsWholeChannel(info)) {
    for (size_t c=0; c<img.numChannels(); ++c) {
      switch (img.voxelByteNumber()) {
      case 1: {
        uint8_t *des = img.channelData<uint8_t>(c);
        const uint8_t *src = bufImg + c + region.start.c;
        size_t numCh = img.numChannels();
        size_t i=0;
        while (i++ < img.channelVoxelNumber()) {
          *des++ = *src;
          src += numCh;
        }
      }
        break;
      case 2: {
        uint16_t *des = img.channelData<uint16_t>(c);
        const uint16_t *src = reinterpret_cast<uint16_t*>(bufImg) + c + region.start.c;
        size_t numCh = img.numChannels();
        size_t i=0;
        while (i++ < img.channelVoxelNumber()) {
          *des++ = ((*src & 0xff) << 8) | ((*src & 0xff00) >> 8);
          src += numCh;
        }
      }
        break;
      default:
        throw ZIOException(QString("Not support png with voxelByteNumber %1").arg(img.voxelByteNumber()));
        break;
      }
    }
  } else {
    if (img.voxelByteNumber() == 1) {
      for (size_t c=0; c<img.numChannels(); ++c) {
        for (size_t y=0; y<img.height(); ++y) {
          for (size_t x=0; x<img.width(); ++x) {
            uint8_t *des = img.data<uint8_t>(x, y, 0, c);
            uint8_t *src = bufImg + (y+region.start.y) * info.rowVoxelNumber() * info.numChannels +
                (x+region.start.x) * info.numChannels + c + region.start.c;
            *des = *src;
          }
        }
      }
    } else if (img.voxelByteNumber() == 2) {
      for (size_t c=0; c<img.numChannels(); ++c) {
        for (size_t y=0; y<img.height(); ++y) {
          for (size_t x=0; x<img.width(); ++x) {
            uint16_t *des = img.data<uint16_t>(x, y, 0, c);
            uint16_t *src = reinterpret_cast<uint16_t*>(bufImg) + (y+region.start.y) * info.rowVoxelNumber() * info.numChannels +
                (x+region.start.x) * info.numChannels + c + region.start.c;
            *des = ((*src & 0xff) << 8) | ((*src & 0xff00) >> 8);
          }
        }
      }
    } else {
      throw ZIOException(QString("Not support png with voxelByteNumber %1").arg(img.voxelByteNumber()));
    }
  }
}

}

namespace nim {

ZImgPng::ZImgPng()
{
}

QString ZImgPng::shortName() const
{
  return "Png";
}

QString ZImgPng::fullName() const
{
  return "Png";
}

QStringList ZImgPng::extensions() const
{
  QStringList res;
  res << "png";
  return res;
}

void ZImgPng::readInfo(const QString &filename, std::vector<ZImgInfo> &infos, std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> *subBlocks,
                       std::vector<std::set<size_t>> *pyramidalRatios)
{
  std::ifstream is;
  openFileStream(is, filename, std::ios_base::in | std::ios_base::binary);

  // this will not read the physical size, to read physical size, we need to read all chunks
  std::vector<char> buf(33);
  readStream(is, buf.data(), buf.size());

  lodepng::State state;

  infos.resize(1);
  readInfoFromBuf(buf, state, infos[0]);

  createDefaultSubBlocks(filename, infos, subBlocks, pyramidalRatios);
}

void ZImgPng::readMetadata(const QString &filename, ZImgMetadata &meta, size_t scene)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
  std::ifstream ifs;
  openFileStream(ifs, filename, std::ios_base::in | std::ios_base::binary);

  std::vector<char> buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

  lodepng::State state;

  state.decoder.read_text_chunks = 1;
  state.decoder.remember_unknown_chunks = 1;

  uint8_t* outRaw = nullptr;
  uint32_t w, h;
  lodepng_decode(&outRaw, &w, &h, &state, reinterpret_cast<uint8_t*>(buf.data()), buf.size());
  free(outRaw);
  checkPngError(state);

  readMetaDataFromState(state, meta);
}

void ZImgPng::readThumbnail(const QString &, ZImgThumbernail &, const ZImgRegion &, size_t)
{
  // png does not have standard thumbnail chunk
}

void ZImgPng::readImg(const QString &filename, ZImg &img, const ZImgRegion &region, size_t scene, size_t ratio)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
  std::ifstream ifs;
  openFileStream(ifs, filename, std::ios_base::in | std::ios_base::binary);

  std::vector<char> buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

  lodepng::State state;
  ZImgInfo info;
  readInfoFromBuf(buf, state, info);

  if (region.isEmpty() || !region.isValid(info)) {
    throw ZIOException(QString("Invalid image region. Image info: '%1', region: '%2'").arg(info.toQString()).arg(region.toQString()));
  }

  state.decoder.read_text_chunks = 1;
  state.decoder.remember_unknown_chunks = 1;

  state.info_raw = state.info_png.color;
  if (state.info_raw.bitdepth < 8) {
    state.info_raw.bitdepth = 8;
  }
  if (state.info_raw.colortype == LCT_PALETTE) {
    state.info_raw.colortype = LCT_RGBA;
  }

  uint8_t* outRaw = nullptr;
  uint32_t w, h;
  lodepng_decode(&outRaw, &w, &h, &state, reinterpret_cast<uint8_t*>(buf.data()), buf.size());
  checkPngError(state);

  if (state.info_png.phys_defined && state.info_png.phys_unit == 1) {
    info.voxelSizeUnit = VoxelSizeUnit::m;
    info.voxelSizeX = 1.0 / state.info_png.phys_x;
    info.voxelSizeY = 1.0 / state.info_png.phys_y;
  }

  ZImg imgTmp(region.clip(info));
  separateChannel(outRaw, info, region, imgTmp);
  free(outRaw);
  readMetaDataFromState(state, imgTmp.metadataRef());

  imgTmp.swap(img);

  if (ratio > 1) {
    img.zoom(1.0 / ratio, 1.0 / ratio);
  }
}

bool ZImgPng::supportRead() const
{
  return true;
}

bool ZImgPng::supportWrite() const
{
  return false;
}

} // namespace nim
