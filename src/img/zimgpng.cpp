#include "zimgpng.h"

#include "zlog.h"
#include "zioutils.h"
#include "zbenchtimer.h"
#include <png.h>
#include <folly/ScopeGuard.h>

namespace {

using namespace nim;

struct PngPack
{
  png_structp pngPtr = nullptr;
  png_infop infoPtr = nullptr;
  png_infop endPtr = nullptr;
};

void pngReadErrorFunction(png_structp, const char* message)
{
  throw nim::ZIOException(QString("Libpng error: %1").arg(message));
}

void pngReadWarningFunction(png_structp, const char* message)
{
  LOG(WARNING) << "Libpng warning: " << message;
}

int skipIDATChunk(png_structp, png_unknown_chunkp chunk)
{
  // LOG(INFO) << reinterpret_cast<const char*>(chunk->name);
  if (chunk->name[0] == 'I' && chunk->name[1] == 'D' && chunk->name[2] == 'A' && chunk->name[3] == 'T') {
    return 1;
  }
  return 0;
}

void readMetaDataFromState(png_const_structrp pngPtr, png_inforp infoPtr, png_inforp endPtr, ZImgMetadata& meta)
{
  png_timep modTime;
  if (png_get_tIME(pngPtr, infoPtr, &modTime)) {
    meta.attachToTopLevel(ZImgMetatag("Time",
                                      QString("%1-%2-%3T%4:%5:%6")
                                        .arg(modTime->year)
                                        .arg(modTime->month)
                                        .arg(modTime->day)
                                        .arg(modTime->hour)
                                        .arg(modTime->minute)
                                        .arg(modTime->second)));
  }
  if (png_get_tIME(pngPtr, endPtr, &modTime)) {
    meta.attachToTopLevel(ZImgMetatag("Time",
                                      QString("%1-%2-%3T%4:%5:%6")
                                        .arg(modTime->year)
                                        .arg(modTime->month)
                                        .arg(modTime->day)
                                        .arg(modTime->hour)
                                        .arg(modTime->minute)
                                        .arg(modTime->second)));
  }
  png_textp textPtr;
  int numText;
  if (png_get_text(pngPtr, infoPtr, &textPtr, &numText)) {
    for (auto i = 0; i < numText; ++i) {
      meta.attachToTopLevel(ZImgMetatag(QString::fromUtf8(textPtr[i].key), QString::fromUtf8(textPtr[i].text)));
    }
  }
  if (png_get_text(pngPtr, endPtr, &textPtr, &numText)) {
    for (auto i = 0; i < numText; ++i) {
      meta.attachToTopLevel(ZImgMetatag(QString::fromUtf8(textPtr[i].key), QString::fromUtf8(textPtr[i].text)));
    }
  }
}

void readInfoFromBuf(png_const_structrp pngPtr, png_const_inforp infoPtr, ZImgInfo& info)
{
  info.width = png_get_image_width(pngPtr, infoPtr);
  info.height = png_get_image_height(pngPtr, infoPtr);
  info.depth = 1;
  auto bitDepth = png_get_bit_depth(pngPtr, infoPtr);
  if (bitDepth != 16 && bitDepth != 8 && bitDepth != 4 && bitDepth != 2 && bitDepth != 1) {
    throw nim::ZIOException(QString("invalid bit depth").arg(bitDepth));
  }
  png_byte colorType = png_get_color_type(pngPtr, infoPtr);
  switch (colorType) {
    case PNG_COLOR_TYPE_GRAY:
      info.numChannels = 1;
      break;
    case PNG_COLOR_TYPE_GRAY_ALPHA:
      info.numChannels = 2;
      info.lastChannelIsAlphaChannel = true;
      if (bitDepth < 8) {
        throw ZIOException("invalid bit depth for gray alpha png");
      }
      break;
    case PNG_COLOR_TYPE_RGB:
      info.numChannels = 3;
      if (bitDepth < 8) {
        throw ZIOException("invalid bit depth for rgb png");
      }
      break;
    case PNG_COLOR_TYPE_RGB_ALPHA:
      info.numChannels = 4;
      info.lastChannelIsAlphaChannel = true;
      if (bitDepth < 8) {
        throw ZIOException("invalid bit depth for rgba png");
      }
      break;
    case PNG_COLOR_TYPE_PALETTE:
      info.numChannels = 3;
      if (bitDepth > 8) {
        throw ZIOException("invalid bit depth for palette png");
      }
      break;
    default:
      throw ZIOException("not supported png colortype");
  }
  if (png_get_valid(pngPtr, infoPtr, PNG_INFO_tRNS) && colorType != PNG_COLOR_TYPE_GRAY_ALPHA &&
      colorType != PNG_COLOR_TYPE_RGB_ALPHA) {
    info.numChannels += 1;
    info.lastChannelIsAlphaChannel = true;
  } else if (colorType == PNG_COLOR_TYPE_GRAY) {
    info.validBitCount = bitDepth; // libpng will scale pixel to 8 bits if we convert tRNS to alpha channel
  }

  info.numTimes = 1;

  info.voxelFormat = VoxelFormat::Unsigned;
  info.bytesPerVoxel = std::max(1, bitDepth / 8);

  info.createDefaultDescriptions();

  if (png_get_x_pixels_per_meter(pngPtr, infoPtr) > 0 && png_get_y_pixels_per_meter(pngPtr, infoPtr) > 0) {
    info.voxelSizeUnit = VoxelSizeUnit::m;
    info.voxelSizeX = 1.0 / png_get_x_pixels_per_meter(pngPtr, infoPtr);
    info.voxelSizeY = 1.0 / png_get_y_pixels_per_meter(pngPtr, infoPtr);
  }
}

// convert RGBARGBA..... to RRR...GGG...BBB...AAA... and perform crop
void separateChannel(uint8_t* bufImg, const ZImgInfo& info, const ZImgRegion& region, ZImg& img)
{
  if (region.containsWholeChannel(info)) {
    for (size_t c = 0; c < img.numChannels(); ++c) {
      switch (img.voxelByteNumber()) {
        case 1: {
          auto* des = img.channelData<uint8_t>(c);
          const uint8_t* src = bufImg + c + region.start.c;
          size_t numCh = img.numChannels();
          size_t i = 0;
          while (i++ < img.channelVoxelNumber()) {
            *des++ = *src;
            src += numCh;
          }
          break;
        }
        case 2: {
          auto* des = img.channelData<uint16_t>(c);
          const uint16_t* src = reinterpret_cast<uint16_t*>(bufImg) + c + region.start.c;
          size_t numCh = img.numChannels();
          size_t i = 0;
          while (i++ < img.channelVoxelNumber()) {
            *des++ = ((*src & 0xff) << 8) | ((*src & 0xff00) >> 8);
            src += numCh;
          }
          break;
        }
        default:
          throw ZIOException(QString("Not support png with voxelByteNumber %1").arg(img.voxelByteNumber()));
      }
    }
  } else {
    if (img.voxelByteNumber() == 1) {
      for (size_t c = 0; c < img.numChannels(); ++c) {
        for (size_t y = 0; y < img.height(); ++y) {
          for (size_t x = 0; x < img.width(); ++x) {
            auto* des = img.data<uint8_t>(x, y, 0, c);
            uint8_t* src = bufImg + (y + region.start.y) * info.rowVoxelNumber() * info.numChannels +
                           (x + region.start.x) * info.numChannels + c + region.start.c;
            *des = *src;
          }
        }
      }
    } else if (img.voxelByteNumber() == 2) {
      for (size_t c = 0; c < img.numChannels(); ++c) {
        for (size_t y = 0; y < img.height(); ++y) {
          for (size_t x = 0; x < img.width(); ++x) {
            auto* des = img.data<uint16_t>(x, y, 0, c);
            uint16_t* src = reinterpret_cast<uint16_t*>(bufImg) +
                            (y + region.start.y) * info.rowVoxelNumber() * info.numChannels +
                            (x + region.start.x) * info.numChannels + c + region.start.c;
            *des = ((*src & 0xff) << 8) | ((*src & 0xff00) >> 8);
          }
        }
      }
    } else {
      throw ZIOException(QString("Not support png with voxelByteNumber %1").arg(img.voxelByteNumber()));
    }
  }
}

} // namespace

namespace nim {

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

void ZImgPng::readInfo(const QString& filename,
                       std::vector<ZImgInfo>& infos,
                       std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  // ZBenchTimer bt("a");
  // bt.start();
  auto infile = openFile(filename, "rb");

  PngPack png;
  png.pngPtr = png_create_read_struct(PNG_LIBPNG_VER_STRING, &png, pngReadErrorFunction, pngReadWarningFunction);
  if (png.pngPtr) {
    png.infoPtr = png_create_info_struct(png.pngPtr);
    png.endPtr = png_create_info_struct(png.pngPtr);
  }
  if (!png.pngPtr || !png.infoPtr || !png.endPtr) {
    png_destroy_read_struct(&png.pngPtr, &png.infoPtr, &png.endPtr);
    throw ZIOException("Libpng read error");
  }

  auto guard1 = folly::makeGuard([&png]() {
    png_destroy_read_struct(&png.pngPtr, &png.infoPtr, &png.endPtr);
  });

  png_init_io(png.pngPtr, infile.get());
  png_set_crc_action(png.pngPtr, PNG_CRC_DEFAULT, PNG_CRC_DEFAULT);

  png_set_read_user_chunk_fn(png.pngPtr, nullptr, skipIDATChunk);
  unsigned char name[] = "IDAT";
  png_set_keep_unknown_chunks(png.pngPtr, PNG_HANDLE_CHUNK_NEVER, nullptr, 0);
  png_set_keep_unknown_chunks(png.pngPtr, PNG_HANDLE_CHUNK_NEVER, name, 1);

  png_read_info(png.pngPtr, png.infoPtr);
  png_read_end(png.pngPtr, png.endPtr);

  infos.resize(1);
  readInfoFromBuf(png.pngPtr, png.infoPtr, infos[0]);
  // looking for resolution at end part if we don't have it
  if (infos[0].voxelSizeUnit == VoxelSizeUnit::none && png_get_x_pixels_per_meter(png.pngPtr, png.endPtr) > 0 &&
      png_get_y_pixels_per_meter(png.pngPtr, png.endPtr) > 0) {
    infos[0].voxelSizeUnit = VoxelSizeUnit::m;
    infos[0].voxelSizeX = 1.0 / png_get_x_pixels_per_meter(png.pngPtr, png.endPtr);
    infos[0].voxelSizeY = 1.0 / png_get_y_pixels_per_meter(png.pngPtr, png.endPtr);
  }

  createDefaultSubBlocks(filename, infos, subBlocks);
}

void ZImgPng::readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }

  auto infile = openFile(filename, "rb");

  PngPack png;
  png.pngPtr = png_create_read_struct(PNG_LIBPNG_VER_STRING, &png, pngReadErrorFunction, pngReadWarningFunction);
  if (png.pngPtr) {
    png.infoPtr = png_create_info_struct(png.pngPtr);
    png.endPtr = png_create_info_struct(png.pngPtr);
  }
  if (!png.pngPtr || !png.infoPtr || !png.endPtr) {
    png_destroy_read_struct(&png.pngPtr, &png.infoPtr, &png.endPtr);
    throw ZIOException("Libpng read error");
  }

  auto guard1 = folly::makeGuard([&png]() {
    png_destroy_read_struct(&png.pngPtr, &png.infoPtr, &png.endPtr);
  });

  png_init_io(png.pngPtr, infile.get());
  png_set_crc_action(png.pngPtr, PNG_CRC_DEFAULT, PNG_CRC_DEFAULT);

  png_set_read_user_chunk_fn(png.pngPtr, nullptr, skipIDATChunk);
  unsigned char name[] = "IDAT";
  png_set_keep_unknown_chunks(png.pngPtr, PNG_HANDLE_CHUNK_NEVER, nullptr, 0);
  png_set_keep_unknown_chunks(png.pngPtr, PNG_HANDLE_CHUNK_NEVER, name, 1);

  png_read_info(png.pngPtr, png.infoPtr);
  png_read_end(png.pngPtr, png.endPtr);

  readMetaDataFromState(png.pngPtr, png.infoPtr, png.endPtr, meta);
}

void ZImgPng::readThumbnail(const QString& /*filename*/,
                            ZImgThumbernail& /*thumbnail*/,
                            const ZImgRegion& /*region*/,
                            size_t /*scene*/)
{
  // png does not have standard thumbnail chunk
}

void ZImgPng::readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene)
{
  // ZBenchTimer bt("b");
  // bt.start();
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }

  auto infile = openFile(filename, "rb");

  PngPack png;
  png.pngPtr = png_create_read_struct(PNG_LIBPNG_VER_STRING, &png, pngReadErrorFunction, pngReadWarningFunction);
  if (png.pngPtr) {
    png.infoPtr = png_create_info_struct(png.pngPtr);
    png.endPtr = png_create_info_struct(png.pngPtr);
  }
  if (!png.pngPtr || !png.infoPtr || !png.endPtr) {
    png_destroy_read_struct(&png.pngPtr, &png.infoPtr, &png.endPtr);
    throw ZIOException("Libpng read error");
  }

  auto guard1 = folly::makeGuard([&png]() {
    png_destroy_read_struct(&png.pngPtr, &png.infoPtr, &png.endPtr);
  });

  png_init_io(png.pngPtr, infile.get());
  png_set_crc_action(png.pngPtr, PNG_CRC_DEFAULT, PNG_CRC_DEFAULT);

  png_read_info(png.pngPtr, png.infoPtr);

  ZImgInfo info;
  readInfoFromBuf(png.pngPtr, png.infoPtr, info);

  if (region.isEmpty() || !region.isValid(info)) {
    throw ZIOException(
      QString("Invalid image region. Image info: '%1', region: '%2'").arg(info.toQString()).arg(region.toQString()));
  }

  png_byte colorType = png_get_color_type(png.pngPtr, png.infoPtr);
  png_byte bitDepth = png_get_bit_depth(png.pngPtr, png.infoPtr);
  if (colorType == PNG_COLOR_TYPE_PALETTE) {
    png_set_palette_to_rgb(png.pngPtr);
  }
  if (png_get_valid(png.pngPtr, png.infoPtr, PNG_INFO_tRNS) && colorType != PNG_COLOR_TYPE_GRAY_ALPHA &&
      colorType != PNG_COLOR_TYPE_RGB_ALPHA) {
    png_set_tRNS_to_alpha(png.pngPtr); // will 1 2 4-bit image value to range 0-255
  }
  if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) {
    // png_set_expand_gray_1_2_4_to_8(png.pngPtr);  // will scale value to range 0-255
    png_set_packing(png.pngPtr);
  }
  // if (bitDepth == 16) {     // we do it later with separateChannel
  // png_set_swap(png.pngPtr);
  // }
  png_set_interlace_handling(png.pngPtr);
  png_read_update_info(png.pngPtr, png.infoPtr);

  size_t rowBytes = png_get_rowbytes(png.pngPtr, png.infoPtr);
  if (rowBytes * info.height != info.byteNumber()) {
    throw ZIOException("fatal png read error");
  }
  std::vector<png_byte, boost::alignment::aligned_allocator<png_byte, 64>> outRaw(info.byteNumber());
  std::vector<png_bytep> rowPointers(info.height);
  for (size_t i = 0; i < rowPointers.size(); ++i) {
    rowPointers[i] = outRaw.data() + i * rowBytes;
  }
  png_read_image(png.pngPtr, rowPointers.data());
  png_read_end(png.pngPtr, png.endPtr);

  ZImg imgTmp(region.clip(info));
  separateChannel(outRaw.data(), info, region, imgTmp);

  readMetaDataFromState(png.pngPtr, png.infoPtr, png.endPtr, imgTmp.metadataRef());
  // looking for resolution at end part if we don't have it
  if (imgTmp.infoRef().voxelSizeUnit == VoxelSizeUnit::none && png_get_x_pixels_per_meter(png.pngPtr, png.endPtr) > 0 &&
      png_get_y_pixels_per_meter(png.pngPtr, png.endPtr) > 0) {
    imgTmp.infoRef().voxelSizeUnit = VoxelSizeUnit::m;
    imgTmp.infoRef().voxelSizeX = 1.0 / png_get_x_pixels_per_meter(png.pngPtr, png.endPtr);
    imgTmp.infoRef().voxelSizeY = 1.0 / png_get_y_pixels_per_meter(png.pngPtr, png.endPtr);
  }
  imgTmp.swap(img);
}

void ZImgPng::checkImgBeforeWriting(const QString& filename, const ZImgInfo& info, const ZImgWriteParameters& paras)
{
  ZImgFormat::checkImgBeforeWriting(filename, info, paras);
  if (paras.compression != Compression::AUTO && paras.compression != Compression::DEFLATE) {
    throw ZIOException(fmt::format("compression {} is not supported", enumToString(paras.compression)));
  }
  if (info.numTimes != 1 || info.depth != 1) {
    throw ZIOException(QString("only 2d image is supported: %1").arg(info.toQString()));
  }
  if (!(info.numChannels == 1 || (info.numChannels == 2 && info.lastChannelIsAlphaChannel) ||
        (info.numChannels == 4 && info.lastChannelIsAlphaChannel) ||
        (info.numChannels == 3 && !info.lastChannelIsAlphaChannel)) ||
      info.voxelFormat != VoxelFormat::Unsigned || info.bytesPerVoxel > 2) {
    throw ZIOException(QString("image can not be represented as png: %1").arg(info.toQString()));
  }
}

void ZImgPng::writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, img.info(), paras);

  auto outfile = openFile(filename, "wb");

  PngPack png;
  png.pngPtr = png_create_write_struct(PNG_LIBPNG_VER_STRING, &png, pngReadErrorFunction, pngReadWarningFunction);
  if (png.pngPtr) {
    png.infoPtr = png_create_info_struct(png.pngPtr);
  }
  if (!png.pngPtr || !png.infoPtr) {
    png_destroy_write_struct(&png.pngPtr, &png.infoPtr);
    throw ZIOException("can not create Libpng write struct");
  }

  auto guard1 = folly::makeGuard([&png]() {
    png_destroy_write_struct(&png.pngPtr, &png.infoPtr);
  });

  png_init_io(png.pngPtr, outfile.get());

  png_set_compression_level(png.pngPtr, paras.zlibCompressionLevel);

  int colorType = PNG_COLOR_TYPE_RGB_ALPHA;
  if (img.info().numChannels == 1) {
    colorType = PNG_COLOR_TYPE_GRAY;
  } else if (img.info().numChannels == 2) {
    CHECK(img.info().lastChannelIsAlphaChannel);
    colorType = PNG_COLOR_TYPE_GRAY_ALPHA;
  } else if (img.info().numChannels == 3) {
    colorType = PNG_COLOR_TYPE_RGB;
  } else {
    CHECK(img.info().lastChannelIsAlphaChannel && img.info().numChannels == 4);
  }

  png_set_IHDR(png.pngPtr,
               png.infoPtr,
               img.width(),
               img.height(),
               8 * img.bytesPerVoxel(),
               colorType,
               PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_text text[1];
  int num_text = 0;
  text[num_text].compression = PNG_TEXT_COMPRESSION_NONE;
  text[num_text].key = const_cast<char*>("Description");
  text[num_text].text = const_cast<char*>("Created by zimg");
  ++num_text;
  png_set_text(png.pngPtr, png.infoPtr, text, num_text);

  png_write_info(png.pngPtr, png.infoPtr);

  ZImg tmp(img.info());
  CHECK(tmp.channelData<uint8_t>(0) != img.channelData<uint8_t>(0)) << img.info().toQString();
  ZImgFormat::XYZCtoCXYZ(img, tmp);
  for (size_t r = 0; r < tmp.height(); ++r) {
    png_write_row(png.pngPtr, tmp.channelData<uint8_t>(0) + r * tmp.rowByteNumber() * tmp.numChannels());
  }
  png_write_end(png.pngPtr, nullptr);
}

bool ZImgPng::supportRead() const
{
  return true;
}

bool ZImgPng::supportWrite() const
{
  return true;
}

} // namespace nim
