#include "zimgpng.h"

#include "zlog.h"
#include "zioutils.h"
#include "zbenchtimer.h"
#include <png.h>
#include <algorithm>
#include <csetjmp>
#include <cstring>
#include <limits>
#include <memory>

namespace {

using namespace nim;

struct PngReadPack
{
  png_structp pngPtr = nullptr;
  png_infop infoPtr = nullptr;
  png_infop endPtr = nullptr;

  ~PngReadPack()
  {
    if (pngPtr) {
      png_destroy_read_struct(&pngPtr, &infoPtr, &endPtr);
    }
  }
};

struct PngWritePack
{
  png_structp pngPtr = nullptr;
  png_infop infoPtr = nullptr;

  ~PngWritePack()
  {
    if (pngPtr) {
      png_destroy_write_struct(&pngPtr, &infoPtr);
    }
  }
};

struct PngReadContext
{
  PngReadPack png;
  std::string errorMessage;
  ZImgInfo info;
  std::vector<png_byte, boost::alignment::aligned_allocator<png_byte, 64>> outRaw;
  std::vector<png_bytep> rowPointers;
  ZImg imgTmp;
};

struct PngWriteContext
{
  PngWritePack png;
  std::string errorMessage;
  ZImg tmp;
};

void pngErrorFunction(png_structp pngPtr, const char* message)
{
  auto* errorMessage = static_cast<std::string*>(png_get_error_ptr(pngPtr));
  if (errorMessage) {
    *errorMessage = fmt::format("Libpng error: {}", message ? message : "<unknown>");
  }
  longjmp(png_jmpbuf(pngPtr), 1);
}

void pngWarningFunction(png_structp, const char* message)
{
  LOG(WARNING) << "Libpng warning: " << message;
}

void pngSilentWarningFunction(png_structp, const char* message)
{
  // The memory decode path is used for small remote chunks; surface hard failures only.
  (void)message;
}

size_t checkedPngMul(size_t a, size_t b, const char* what)
{
  if (a == 0 || b == 0) {
    return 0;
  }
  if (a > std::numeric_limits<size_t>::max() / b) {
    throw ZException(fmt::format("Overflow computing {}", what));
  }
  return a * b;
}

struct PngMemoryReadState
{
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t offset = 0;
};

struct PngMemoryDecodeState
{
  PngReadPack png;
  PngMemoryReadState readState;
  std::string errorMessage;
  std::vector<uint8_t> interleaved;
  std::vector<png_bytep> rowPointers;
};

void pngMemoryReadCallback(png_structp pngPtr, png_bytep outBytes, png_size_t byteCountToRead)
{
  auto* state = static_cast<PngMemoryReadState*>(png_get_io_ptr(pngPtr));
  if (!state || !state->data) {
    png_error(pngPtr, "Invalid PNG read state");
    return;
  }
  const auto bytesToRead = static_cast<size_t>(byteCountToRead);
  if (bytesToRead > state->size || state->offset > state->size - bytesToRead) {
    png_error(pngPtr, "PNG read out of range");
    return;
  }
  std::memcpy(outBytes, state->data + state->offset, bytesToRead);
  state->offset += bytesToRead;
}

int skipIDATiDOTChunk(png_structp, png_unknown_chunkp chunk)
{
  // VLOG(1) << reinterpret_cast<const char*>(chunk->name);
  if (chunk->name[0] == 'I' && chunk->name[1] == 'D' && chunk->name[2] == 'A' && chunk->name[3] == 'T') {
    return 1;
  }
  if (chunk->name[0] == 'i' && chunk->name[1] == 'D' && chunk->name[2] == 'O' && chunk->name[3] == 'T') {
    return 1;
  }
  return 0;
}

void readMetaDataFromState(png_const_structrp pngPtr, png_inforp infoPtr, png_inforp endPtr, ZImgMetadata& meta)
{
  png_timep modTime;
  if (png_get_tIME(pngPtr, infoPtr, &modTime)) {
    meta.attachToTopLevel(ZImgMetatag("Time",
                                      fmt::format("{}-%{}-%{}T{}:{}:{}",
                                                  modTime->year,
                                                  modTime->month,
                                                  modTime->day,
                                                  modTime->hour,
                                                  modTime->minute,
                                                  modTime->second)));
  }
  if (png_get_tIME(pngPtr, endPtr, &modTime)) {
    meta.attachToTopLevel(ZImgMetatag("Time",
                                      fmt::format("{}-%{}-%{}T{}:{}:{}",
                                                  modTime->year,
                                                  modTime->month,
                                                  modTime->day,
                                                  modTime->hour,
                                                  modTime->minute,
                                                  modTime->second)));
  }
  png_textp textPtr;
  int numText;
  if (png_get_text(pngPtr, infoPtr, &textPtr, &numText)) {
    for (auto i = 0; i < numText; ++i) {
      meta.attachToTopLevel(ZImgMetatag(textPtr[i].key, std::string_view(textPtr[i].text, textPtr[i].text_length)));
    }
  }
  if (png_get_text(pngPtr, endPtr, &textPtr, &numText)) {
    for (auto i = 0; i < numText; ++i) {
      meta.attachToTopLevel(ZImgMetatag(textPtr[i].key, std::string_view(textPtr[i].text, textPtr[i].text_length)));
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
    throw ZException(fmt::format("invalid bit depth {}", bitDepth), ZException::Option::CheckErrno);
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
        throw ZException("invalid bit depth for gray alpha png");
      }
      break;
    case PNG_COLOR_TYPE_RGB:
      info.numChannels = 3;
      if (bitDepth < 8) {
        throw ZException("invalid bit depth for rgb png");
      }
      break;
    case PNG_COLOR_TYPE_RGB_ALPHA:
      info.numChannels = 4;
      info.lastChannelIsAlphaChannel = true;
      if (bitDepth < 8) {
        throw ZException("invalid bit depth for rgba png");
      }
      break;
    case PNG_COLOR_TYPE_PALETTE:
      info.numChannels = 3;
      if (bitDepth > 8) {
        throw ZException("invalid bit depth for palette png");
      }
      break;
    default:
      throw ZException("not supported png colortype");
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
          throw ZException(fmt::format("Not support png with voxelByteNumber {}", img.voxelByteNumber()));
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
      throw ZException(fmt::format("Not support png with voxelByteNumber {}", img.voxelByteNumber()));
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

std::vector<uint8_t> ZImgPng::readMemRaw(std::span<const uint8_t> pngBytes,
                                         size_t expectedVoxelCount,
                                         size_t expectedChannels,
                                         size_t bytesPerVoxel)
{
  if (expectedVoxelCount == 0) {
    throw ZException("Invalid PNG decode request: expectedVoxelCount must be > 0");
  }
  if (expectedChannels == 0 || expectedChannels > 4) {
    throw ZException(
      fmt::format("Invalid PNG decode request: expectedChannels must be 1..4 (got {})", expectedChannels));
  }
  if (bytesPerVoxel != 1 && bytesPerVoxel != 2) {
    throw ZException(fmt::format("Invalid PNG decode request: bytesPerVoxel must be 1 or 2 (got {})", bytesPerVoxel));
  }
  if (pngBytes.empty()) {
    throw ZException("Invalid PNG chunk: empty payload");
  }

  auto decodeState = std::make_unique<PngMemoryDecodeState>();
  decodeState->png.pngPtr = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                                   &decodeState->errorMessage,
                                                   pngErrorFunction,
                                                   pngSilentWarningFunction);
  if (decodeState->png.pngPtr) {
    decodeState->png.infoPtr = png_create_info_struct(decodeState->png.pngPtr);
    decodeState->png.endPtr = png_create_info_struct(decodeState->png.pngPtr);
  }
  if (!decodeState->png.pngPtr || !decodeState->png.infoPtr || !decodeState->png.endPtr) {
    throw ZException("Libpng: failed to create read struct");
  }
  if (setjmp(png_jmpbuf(decodeState->png.pngPtr))) {
    const std::string message = decodeState->errorMessage.empty() ? "Libpng decode failed" : decodeState->errorMessage;
    throw ZException(message);
  }

  decodeState->readState.data = pngBytes.data();
  decodeState->readState.size = pngBytes.size();
  decodeState->readState.offset = 0;

  png_set_read_fn(decodeState->png.pngPtr, &decodeState->readState, pngMemoryReadCallback);
  png_read_info(decodeState->png.pngPtr, decodeState->png.infoPtr);

  const png_uint_32 widthU32 = png_get_image_width(decodeState->png.pngPtr, decodeState->png.infoPtr);
  const png_uint_32 heightU32 = png_get_image_height(decodeState->png.pngPtr, decodeState->png.infoPtr);
  if (widthU32 == 0 || heightU32 == 0) {
    throw ZException(fmt::format("Invalid PNG chunk dimensions: width={}, height={}", widthU32, heightU32));
  }
  const size_t width = static_cast<size_t>(widthU32);
  const size_t height = static_cast<size_t>(heightU32);
  const size_t area = checkedPngMul(width, height, "PNG pixel count");
  if (area != expectedVoxelCount) {
    throw ZException(fmt::format("PNG chunk pixel count mismatch: got {} pixels ({}x{}), expected {}",
                                 area,
                                 width,
                                 height,
                                 expectedVoxelCount));
  }

  const png_byte bitDepth = png_get_bit_depth(decodeState->png.pngPtr, decodeState->png.infoPtr);
  const png_byte colorType = png_get_color_type(decodeState->png.pngPtr, decodeState->png.infoPtr);

  if (!(bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8 || bitDepth == 16)) {
    throw ZException(fmt::format("Invalid PNG bit depth: {}", static_cast<int>(bitDepth)));
  }

  size_t dataWidth = (bitDepth <= 8) ? 1 : 2;
  size_t numChannels = 1;
  switch (colorType) {
    case PNG_COLOR_TYPE_GRAY:
      numChannels = 1;
      break;
    case PNG_COLOR_TYPE_RGB:
      if (bitDepth != 8 && bitDepth != 16) {
        throw ZException(fmt::format("Invalid PNG bit depth {} for RGB", static_cast<int>(bitDepth)));
      }
      numChannels = 3;
      break;
    case PNG_COLOR_TYPE_PALETTE:
      if (bitDepth > 8) {
        throw ZException(fmt::format("Invalid PNG bit depth {} for palette", static_cast<int>(bitDepth)));
      }
      dataWidth = 1;
      numChannels = 3;
      break;
    case PNG_COLOR_TYPE_GRAY_ALPHA:
      if (bitDepth != 8 && bitDepth != 16) {
        throw ZException(fmt::format("Invalid PNG bit depth {} for grayscale+alpha", static_cast<int>(bitDepth)));
      }
      numChannels = 2;
      break;
    case PNG_COLOR_TYPE_RGB_ALPHA:
      if (bitDepth != 8 && bitDepth != 16) {
        throw ZException(fmt::format("Invalid PNG bit depth {} for RGBA", static_cast<int>(bitDepth)));
      }
      numChannels = 4;
      break;
    default:
      throw ZException(fmt::format("Unsupported PNG color type {}", static_cast<int>(colorType)));
  }

  if (bytesPerVoxel != dataWidth) {
    throw ZException(
      fmt::format("PNG chunk bytesPerVoxel mismatch: decoded {}, expected {}", dataWidth, bytesPerVoxel));
  }
  if (numChannels != expectedChannels) {
    throw ZException(fmt::format("PNG chunk channel mismatch: decoded {}, expected {}", numChannels, expectedChannels));
  }

  if (colorType == PNG_COLOR_TYPE_PALETTE) {
    png_set_palette_to_rgb(decodeState->png.pngPtr);
  }
  if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) {
    png_set_packing(decodeState->png.pngPtr);
  }
  if (bitDepth == 16) {
    // PNG stores 16-bit samples in big-endian order. Neuroglancer expects little-endian raw data.
    png_set_swap(decodeState->png.pngPtr);
  }
  png_set_interlace_handling(decodeState->png.pngPtr);
  png_read_update_info(decodeState->png.pngPtr, decodeState->png.infoPtr);

  const size_t rowBytes = static_cast<size_t>(png_get_rowbytes(decodeState->png.pngPtr, decodeState->png.infoPtr));
  const size_t expectedRowBytes =
    checkedPngMul(checkedPngMul(width, expectedChannels, "PNG row pixels"), bytesPerVoxel, "PNG row bytes");
  if (rowBytes != expectedRowBytes) {
    throw ZException(
      fmt::format("PNG chunk row byte mismatch: decoded rowBytes={}, expected {}", rowBytes, expectedRowBytes));
  }

  decodeState->interleaved.resize(checkedPngMul(rowBytes, height, "PNG decoded bytes"));
  decodeState->rowPointers.resize(height);
  for (size_t y = 0; y < height; ++y) {
    decodeState->rowPointers[y] = decodeState->interleaved.data() + y * rowBytes;
  }

  png_read_image(decodeState->png.pngPtr, decodeState->rowPointers.data());
  png_read_end(decodeState->png.pngPtr, decodeState->png.endPtr);

  if (expectedChannels == 1) {
    return std::move(decodeState->interleaved);
  }

  std::vector<uint8_t> planar(
    checkedPngMul(checkedPngMul(area, expectedChannels, "PNG planar elements"), bytesPerVoxel, "PNG planar bytes"));
  for (size_t i = 0; i < area; ++i) {
    for (size_t c = 0; c < expectedChannels; ++c) {
      const size_t srcOffset = (i * expectedChannels + c) * bytesPerVoxel;
      const size_t dstOffset = (c * area + i) * bytesPerVoxel;
      std::memcpy(planar.data() + dstOffset, decodeState->interleaved.data() + srcOffset, bytesPerVoxel);
    }
  }
  return planar;
}

void ZImgPng::readInfo(const QString& filename,
                       std::vector<ZImgInfo>& infos,
                       std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  // ZBenchTimer bt("a");
  // bt.start();
  auto infile = openFile(filename, "rb");

  auto png = std::make_unique<PngReadContext>();
  png->png.pngPtr =
    png_create_read_struct(PNG_LIBPNG_VER_STRING, &png->errorMessage, pngErrorFunction, pngWarningFunction);
  if (png->png.pngPtr) {
    png->png.infoPtr = png_create_info_struct(png->png.pngPtr);
    png->png.endPtr = png_create_info_struct(png->png.pngPtr);
  }
  if (!png->png.pngPtr || !png->png.infoPtr || !png->png.endPtr) {
    throw ZException("Libpng read error", ZException::Option::CheckErrno);
  }
  if (setjmp(png_jmpbuf(png->png.pngPtr))) {
    throw ZException(png->errorMessage.empty() ? "Libpng read error" : png->errorMessage,
                     ZException::Option::CheckErrno);
  }

  png_init_io(png->png.pngPtr, infile.get());
  png_set_crc_action(png->png.pngPtr, PNG_CRC_DEFAULT, PNG_CRC_DEFAULT);

  png_set_read_user_chunk_fn(png->png.pngPtr, nullptr, skipIDATiDOTChunk);
  png_set_keep_unknown_chunks(png->png.pngPtr, PNG_HANDLE_CHUNK_NEVER, nullptr, 0);
  unsigned char name[] = "IDAT";
  png_set_keep_unknown_chunks(png->png.pngPtr, PNG_HANDLE_CHUNK_NEVER, name, 1);
  // unsigned char name1[] = "iDOT";
  // png_set_keep_unknown_chunks(png->png.pngPtr, PNG_HANDLE_CHUNK_NEVER, name1, 1);

  png_read_info(png->png.pngPtr, png->png.infoPtr);
  png_read_end(png->png.pngPtr, png->png.endPtr);

  infos.resize(1);
  readInfoFromBuf(png->png.pngPtr, png->png.infoPtr, infos[0]);
  // looking for resolution at end part if we don't have it
  if (infos[0].voxelSizeUnit == VoxelSizeUnit::none &&
      png_get_x_pixels_per_meter(png->png.pngPtr, png->png.endPtr) > 0 &&
      png_get_y_pixels_per_meter(png->png.pngPtr, png->png.endPtr) > 0) {
    infos[0].voxelSizeUnit = VoxelSizeUnit::m;
    infos[0].voxelSizeX = 1.0 / png_get_x_pixels_per_meter(png->png.pngPtr, png->png.endPtr);
    infos[0].voxelSizeY = 1.0 / png_get_y_pixels_per_meter(png->png.pngPtr, png->png.endPtr);
  }

  createDefaultSubBlocks(filename, infos, subBlocks);
}

void ZImgPng::readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene)
{
  if (scene != 0) {
    throw ZException("invalid scene");
  }

  auto infile = openFile(filename, "rb");

  auto png = std::make_unique<PngReadContext>();
  png->png.pngPtr =
    png_create_read_struct(PNG_LIBPNG_VER_STRING, &png->errorMessage, pngErrorFunction, pngWarningFunction);
  if (png->png.pngPtr) {
    png->png.infoPtr = png_create_info_struct(png->png.pngPtr);
    png->png.endPtr = png_create_info_struct(png->png.pngPtr);
  }
  if (!png->png.pngPtr || !png->png.infoPtr || !png->png.endPtr) {
    throw ZException("Libpng read error", ZException::Option::CheckErrno);
  }
  if (setjmp(png_jmpbuf(png->png.pngPtr))) {
    throw ZException(png->errorMessage.empty() ? "Libpng read error" : png->errorMessage,
                     ZException::Option::CheckErrno);
  }

  png_init_io(png->png.pngPtr, infile.get());
  png_set_crc_action(png->png.pngPtr, PNG_CRC_DEFAULT, PNG_CRC_DEFAULT);

  png_set_read_user_chunk_fn(png->png.pngPtr, nullptr, skipIDATiDOTChunk);
  png_set_keep_unknown_chunks(png->png.pngPtr, PNG_HANDLE_CHUNK_NEVER, nullptr, 0);
  unsigned char name[] = "IDAT";
  png_set_keep_unknown_chunks(png->png.pngPtr, PNG_HANDLE_CHUNK_NEVER, name, 1);
  // unsigned char name1[] = "iDOT";
  // png_set_keep_unknown_chunks(png->png.pngPtr, PNG_HANDLE_CHUNK_NEVER, name1, 1);

  png_read_info(png->png.pngPtr, png->png.infoPtr);
  png_read_end(png->png.pngPtr, png->png.endPtr);

  readMetaDataFromState(png->png.pngPtr, png->png.infoPtr, png->png.endPtr, meta);
}

void ZImgPng::readThumbnail(const QString& /*filename*/,
                            ZImgThumbernail& /*thumbnail*/,
                            const ZImgRegion& /*region*/,
                            size_t /*scene*/)
{
  // png does not have standard thumbnail chunk
}

void ZImgPng::readImg(const QString& filename,
                      ZImg& img,
                      const ZImgRegion& region,
                      size_t scene,
                      const ZImgReadOptions& readOptions)
{
  // ZBenchTimer bt("b");
  // bt.start();
  if (scene != 0) {
    throw ZException("invalid scene");
  }

  auto infile = openFile(filename, "rb");

  auto png = std::make_unique<PngReadContext>();
  png->png.pngPtr =
    png_create_read_struct(PNG_LIBPNG_VER_STRING, &png->errorMessage, pngErrorFunction, pngWarningFunction);
  if (png->png.pngPtr) {
    png->png.infoPtr = png_create_info_struct(png->png.pngPtr);
    png->png.endPtr = png_create_info_struct(png->png.pngPtr);
  }
  if (!png->png.pngPtr || !png->png.infoPtr || !png->png.endPtr) {
    throw ZException("Libpng read error", ZException::Option::CheckErrno);
  }
  if (setjmp(png_jmpbuf(png->png.pngPtr))) {
    throw ZException(png->errorMessage.empty() ? "Libpng read error" : png->errorMessage,
                     ZException::Option::CheckErrno);
  }

  png_init_io(png->png.pngPtr, infile.get());
  png_set_crc_action(png->png.pngPtr, PNG_CRC_DEFAULT, PNG_CRC_DEFAULT);

  png_read_info(png->png.pngPtr, png->png.infoPtr);

  readInfoFromBuf(png->png.pngPtr, png->png.infoPtr, png->info);

  if (region.isEmpty() || !region.isValid(png->info)) {
    throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", png->info, region));
  }

  png_byte colorType = png_get_color_type(png->png.pngPtr, png->png.infoPtr);
  png_byte bitDepth = png_get_bit_depth(png->png.pngPtr, png->png.infoPtr);
  if (colorType == PNG_COLOR_TYPE_PALETTE) {
    png_set_palette_to_rgb(png->png.pngPtr);
  }
  if (png_get_valid(png->png.pngPtr, png->png.infoPtr, PNG_INFO_tRNS) && colorType != PNG_COLOR_TYPE_GRAY_ALPHA &&
      colorType != PNG_COLOR_TYPE_RGB_ALPHA) {
    png_set_tRNS_to_alpha(png->png.pngPtr); // will 1 2 4-bit image value to range 0-255
  }
  if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) {
    // png_set_expand_gray_1_2_4_to_8(png->png.pngPtr);  // will scale value to range 0-255
    png_set_packing(png->png.pngPtr);
  }
  // if (bitDepth == 16) {     // we do it later with separateChannel
  // png_set_swap(png->png.pngPtr);
  // }
  png_set_interlace_handling(png->png.pngPtr);
  png_read_update_info(png->png.pngPtr, png->png.infoPtr);

  size_t rowBytes = png_get_rowbytes(png->png.pngPtr, png->png.infoPtr);
  if (rowBytes * png->info.height != png->info.byteNumber()) {
    throw ZException("fatal png read error", ZException::Option::CheckErrno);
  }
  png->outRaw.resize(png->info.byteNumber());
  png->rowPointers.resize(png->info.height);
  for (size_t i = 0; i < png->rowPointers.size(); ++i) {
    png->rowPointers[i] = png->outRaw.data() + i * rowBytes;
  }
  png_read_image(png->png.pngPtr, png->rowPointers.data());
  png_read_end(png->png.pngPtr, png->png.endPtr);

  png->imgTmp = ZImg(region.clip(png->info));
  separateChannel(png->outRaw.data(), png->info, region, png->imgTmp);

  if (readOptions.includeMetadata) {
    readMetaDataFromState(png->png.pngPtr, png->png.infoPtr, png->png.endPtr, png->imgTmp.metadataRef());
  }
  // looking for resolution at end part if we don't have it
  if (png->imgTmp.infoRef().voxelSizeUnit == VoxelSizeUnit::none &&
      png_get_x_pixels_per_meter(png->png.pngPtr, png->png.endPtr) > 0 &&
      png_get_y_pixels_per_meter(png->png.pngPtr, png->png.endPtr) > 0) {
    png->imgTmp.infoRef().voxelSizeUnit = VoxelSizeUnit::m;
    png->imgTmp.infoRef().voxelSizeX = 1.0 / png_get_x_pixels_per_meter(png->png.pngPtr, png->png.endPtr);
    png->imgTmp.infoRef().voxelSizeY = 1.0 / png_get_y_pixels_per_meter(png->png.pngPtr, png->png.endPtr);
  }
  png->imgTmp.swap(img);
}

void ZImgPng::checkImgBeforeWriting(const QString& filename, const ZImgInfo& info, const ZImgWriteParameters& paras)
{
  ZImgFormat::checkImgBeforeWriting(filename, info, paras);
  if (paras.compression != Compression::AUTO && paras.compression != Compression::DEFLATE) {
    throw ZException(fmt::format("compression {} is not supported", paras.compression));
  }
  if (info.numTimes != 1 || info.depth != 1) {
    throw ZException(fmt::format("only 2d image is supported: {}", info));
  }
  if (!(info.numChannels == 1 || (info.numChannels == 2 && info.lastChannelIsAlphaChannel) ||
        (info.numChannels == 4 && info.lastChannelIsAlphaChannel) ||
        (info.numChannels == 3 && !info.lastChannelIsAlphaChannel)) ||
      info.voxelFormat != VoxelFormat::Unsigned || info.bytesPerVoxel > 2) {
    throw ZException(fmt::format("image can not be represented as png: {}", info));
  }
}

void ZImgPng::writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, img.info(), paras);

  auto outfile = openFile(filename, "wb");

  auto png = std::make_unique<PngWriteContext>();
  png->png.pngPtr =
    png_create_write_struct(PNG_LIBPNG_VER_STRING, &png->errorMessage, pngErrorFunction, pngWarningFunction);
  if (png->png.pngPtr) {
    png->png.infoPtr = png_create_info_struct(png->png.pngPtr);
  }
  if (!png->png.pngPtr || !png->png.infoPtr) {
    throw ZException("can not create Libpng write struct", ZException::Option::CheckErrno);
  }
  if (setjmp(png_jmpbuf(png->png.pngPtr))) {
    throw ZException(png->errorMessage.empty() ? "Libpng write error" : png->errorMessage,
                     ZException::Option::CheckErrno);
  }

  png_init_io(png->png.pngPtr, outfile.get());

  png_set_compression_level(png->png.pngPtr, paras.zlibCompressionLevel);

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

  png_set_IHDR(png->png.pngPtr,
               png->png.infoPtr,
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
  png_set_text(png->png.pngPtr, png->png.infoPtr, text, num_text);

  png_write_info(png->png.pngPtr, png->png.infoPtr);

  png->tmp = ZImg(img.info());
  CHECK(png->tmp.channelData<uint8_t>(0) != img.channelData<uint8_t>(0)) << img.info();
  ZImgFormat::XYZCtoCXYZ(img, png->tmp);
  for (size_t r = 0; r < png->tmp.height(); ++r) {
    png_write_row(png->png.pngPtr,
                  png->tmp.channelData<uint8_t>(0) + r * png->tmp.rowByteNumber() * png->tmp.numChannels());
  }
  png_write_end(png->png.pngPtr, nullptr);
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
