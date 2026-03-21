#include "zneuroglancerprecomputedchunkdecoder.h"

#include "zexception.h"
#include "zlog.h"

#include <png.h>
#include <turbojpeg.h>

#include <algorithm>
#include <array>
#include <bit>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace nim {
namespace {

size_t checkedMul(size_t a, size_t b, const char* what)
{
  if (a == 0 || b == 0) {
    return 0;
  }
  if (a > std::numeric_limits<size_t>::max() / b) {
    throw ZException(fmt::format("Overflow computing {}", what));
  }
  return a * b;
}

uint32_t readU32LE(const uint8_t* p)
{
  return (static_cast<uint32_t>(p[0]) << 0) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void writeU32LE(uint8_t* p, uint32_t v)
{
  p[0] = static_cast<uint8_t>(v >> 0);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

template<typename T>
T readLE(const uint8_t* p)
{
  static_assert(std::is_unsigned_v<T>);
  T out = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    out |= static_cast<T>(p[i]) << (8 * i);
  }
  return out;
}

} // namespace

namespace {

struct PngReadState
{
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t offset = 0;
};

struct PngDecodeState;

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

struct PngDecodeState
{
  PngReadPack png;
  PngReadState readState;
  std::string errorMessage;
  std::vector<uint8_t> interleaved;
  std::vector<png_bytep> rowPointers;
};

void pngReadErrorFunction(png_structp pngPtr, const char* message)
{
  auto* state = static_cast<PngDecodeState*>(png_get_error_ptr(pngPtr));
  if (state) {
    state->errorMessage = fmt::format("Libpng error: {}", message ? message : "<unknown>");
  }
  longjmp(png_jmpbuf(pngPtr), 1);
}

void pngReadWarningFunction(png_structp, const char* message)
{
  // Keep libpng warnings out of logs in a hot decode path; errors are surfaced via exceptions.
  (void)message;
}

void pngReadCallback(png_structp pngPtr, png_bytep outBytes, png_size_t byteCountToRead)
{
  auto* st = static_cast<PngReadState*>(png_get_io_ptr(pngPtr));
  if (!st || !st->data) {
    png_error(pngPtr, "Invalid PNG read state");
    return;
  }
  if (byteCountToRead > st->size || st->offset > st->size - static_cast<size_t>(byteCountToRead)) {
    png_error(pngPtr, "PNG read out of range");
    return;
  }
  std::memcpy(outBytes, st->data + st->offset, static_cast<size_t>(byteCountToRead));
  st->offset += static_cast<size_t>(byteCountToRead);
}

} // namespace

std::vector<uint8_t> ZNeuroglancerPrecomputedChunkDecoder::decodePngToRaw(std::span<const uint8_t> pngBytes,
                                                                          size_t expectedVoxelCount,
                                                                          size_t expectedChannels,
                                                                          size_t bytesPerVoxel)
{
  if (expectedVoxelCount == 0) {
    throw ZException("Invalid PNG decode request: expectedVoxelCount must be > 0");
  }
  if (expectedChannels == 0 || expectedChannels > 4) {
    throw ZException(fmt::format("Invalid PNG decode request: expectedChannels must be 1..4 (got {})", expectedChannels));
  }
  if (bytesPerVoxel != 1 && bytesPerVoxel != 2) {
    throw ZException(fmt::format("Invalid PNG decode request: bytesPerVoxel must be 1 or 2 (got {})", bytesPerVoxel));
  }
  if (pngBytes.empty()) {
    throw ZException("Invalid PNG chunk: empty payload");
  }

  auto decodeState = std::make_unique<PngDecodeState>();
  decodeState->png.pngPtr =
    png_create_read_struct(PNG_LIBPNG_VER_STRING, decodeState.get(), pngReadErrorFunction, pngReadWarningFunction);
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

  png_set_read_fn(decodeState->png.pngPtr, &decodeState->readState, pngReadCallback);
  png_read_info(decodeState->png.pngPtr, decodeState->png.infoPtr);

  const png_uint_32 widthU32 = png_get_image_width(decodeState->png.pngPtr, decodeState->png.infoPtr);
  const png_uint_32 heightU32 = png_get_image_height(decodeState->png.pngPtr, decodeState->png.infoPtr);
  if (widthU32 == 0 || heightU32 == 0) {
    throw ZException(fmt::format("Invalid PNG chunk dimensions: width={}, height={}", widthU32, heightU32));
  }
  const size_t width = static_cast<size_t>(widthU32);
  const size_t height = static_cast<size_t>(heightU32);
  const size_t area = checkedMul(width, height, "PNG pixel count");
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
    throw ZException(fmt::format("PNG chunk bytesPerVoxel mismatch: decoded {}, expected {}", dataWidth, bytesPerVoxel));
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
  const size_t expectedRowBytes = checkedMul(checkedMul(width, expectedChannels, "PNG row pixels"), bytesPerVoxel, "PNG row bytes");
  if (rowBytes != expectedRowBytes) {
    throw ZException(fmt::format("PNG chunk row byte mismatch: decoded rowBytes={}, expected {}", rowBytes, expectedRowBytes));
  }

  decodeState->interleaved.resize(checkedMul(rowBytes, height, "PNG decoded bytes"));
  decodeState->rowPointers.resize(height);
  for (size_t y = 0; y < height; ++y) {
    decodeState->rowPointers[y] = decodeState->interleaved.data() + y * rowBytes;
  }

  png_read_image(decodeState->png.pngPtr, decodeState->rowPointers.data());
  png_read_end(decodeState->png.pngPtr, decodeState->png.endPtr);

  if (expectedChannels == 1) {
    return std::move(decodeState->interleaved);
  }

  std::vector<uint8_t> planar(checkedMul(checkedMul(area, expectedChannels, "PNG planar elements"), bytesPerVoxel, "PNG planar bytes"));
  for (size_t i = 0; i < area; ++i) {
    for (size_t c = 0; c < expectedChannels; ++c) {
      const size_t srcOffset = (i * expectedChannels + c) * bytesPerVoxel;
      const size_t dstOffset = (c * area + i) * bytesPerVoxel;
      std::memcpy(planar.data() + dstOffset, decodeState->interleaved.data() + srcOffset, bytesPerVoxel);
    }
  }
  return planar;
}

std::vector<uint8_t> ZNeuroglancerPrecomputedChunkDecoder::decodeJpegToRaw(std::span<const uint8_t> jpegBytes,
                                                                           size_t expectedVoxelCount,
                                                                           size_t expectedChannels)
{
  if (expectedVoxelCount == 0) {
    throw ZException("Invalid JPEG decode request: expectedVoxelCount must be > 0");
  }
  if (expectedChannels != 1 && expectedChannels != 3) {
    throw ZException(fmt::format("Invalid JPEG decode request: expectedChannels must be 1 or 3 (got {})", expectedChannels));
  }
  if (jpegBytes.empty()) {
    throw ZException("Invalid JPEG chunk: empty payload");
  }
  if (jpegBytes.size() > static_cast<size_t>(std::numeric_limits<unsigned long>::max())) {
    throw ZException("Invalid JPEG chunk: payload too large");
  }

  tjhandle handle = tjInitDecompress();
  if (!handle) {
    throw ZException("libjpeg-turbo: tjInitDecompress failed");
  }
  auto handleGuard = std::unique_ptr<void, decltype(&tjDestroy)>(handle, &tjDestroy);

  int width = 0;
  int height = 0;
  int subsamp = 0;
  int colorspace = 0;
  if (tjDecompressHeader3(handle,
                          reinterpret_cast<const unsigned char*>(jpegBytes.data()),
                          static_cast<unsigned long>(jpegBytes.size()),
                          &width,
                          &height,
                          &subsamp,
                          &colorspace) < 0) {
    throw ZException(fmt::format("libjpeg-turbo: decode header failed: {}", tjGetErrorStr2(handle)));
  }
  if (width <= 0 || height <= 0) {
    throw ZException(fmt::format("Invalid JPEG chunk dimensions: width={}, height={}", width, height));
  }

  const size_t area = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (area != expectedVoxelCount) {
    throw ZException(fmt::format("JPEG chunk pixel count mismatch: got {} pixels ({}x{}), expected {}",
                                 area,
                                 width,
                                 height,
                                 expectedVoxelCount));
  }

  size_t actualComponents = 0;
  switch (colorspace) {
  case TJCS_GRAY:
    actualComponents = 1;
    break;
  case TJCS_CMYK:
  case TJCS_YCCK:
    actualComponents = 4;
    break;
  default:
    actualComponents = 3;
    break;
  }
  if (actualComponents != expectedChannels) {
    throw ZException(fmt::format("JPEG chunk component mismatch: got {} components, expected {}",
                                 actualComponents,
                                 expectedChannels));
  }

  const int pixelFormat = (expectedChannels == 1) ? TJPF_GRAY : TJPF_RGB;
  std::vector<uint8_t> interleaved(area * expectedChannels);
  if (tjDecompress2(handle,
                    reinterpret_cast<const unsigned char*>(jpegBytes.data()),
                    static_cast<unsigned long>(jpegBytes.size()),
                    interleaved.data(),
                    width,
                    /*pitch=*/0,
                    height,
                    pixelFormat,
                    /*flags=*/0) < 0) {
    throw ZException(fmt::format("libjpeg-turbo: decode failed: {}", tjGetErrorStr2(handle)));
  }

  if (expectedChannels == 1) {
    return interleaved;
  }

  std::vector<uint8_t> planar(area * 3);
  for (size_t i = 0; i < area; ++i) {
    planar[i] = interleaved[i * 3 + 0];
    planar[i + area] = interleaved[i * 3 + 1];
    planar[i + 2 * area] = interleaved[i * 3 + 2];
  }
  return planar;
}

std::vector<uint8_t> ZNeuroglancerPrecomputedChunkDecoder::decodeCompressoToRaw(std::span<const uint8_t> bytes,
                                                                                const std::array<size_t, 3>& expectedChunkSize,
                                                                                size_t bytesPerVoxel)
{
  constexpr size_t kHeaderSize = 36;
  if (bytes.size() < kHeaderSize) {
    throw ZException("Invalid compresso chunk: payload too small for header");
  }
  if (bytesPerVoxel != 1 && bytesPerVoxel != 2 && bytesPerVoxel != 4 && bytesPerVoxel != 8) {
    throw ZException(fmt::format("Invalid compresso decode request: bytesPerVoxel must be 1,2,4,8 (got {})", bytesPerVoxel));
  }
  for (size_t d = 0; d < 3; ++d) {
    if (expectedChunkSize[d] == 0) {
      throw ZException("Invalid compresso decode request: expected chunk size must be > 0");
    }
  }

  const uint8_t* p = bytes.data();
  if (!(p[0] == 'c' && p[1] == 'p' && p[2] == 's' && p[3] == 'o')) {
    throw ZException("Invalid compresso chunk: magic mismatch");
  }

  const uint8_t formatVersion = p[4];
  if (formatVersion > 1) {
    throw ZException(fmt::format("Invalid compresso chunk: unsupported format version {}", static_cast<int>(formatVersion)));
  }

  const uint8_t dataWidth = p[5];
  if (!(dataWidth == 1 || dataWidth == 2 || dataWidth == 4 || dataWidth == 8)) {
    throw ZException(fmt::format("Invalid compresso chunk: unsupported data width {}", static_cast<int>(dataWidth)));
  }
  if (static_cast<size_t>(dataWidth) != bytesPerVoxel) {
    throw ZException(fmt::format("Compresso chunk data width mismatch: header {} bytes, expected {} bytes",
                                 static_cast<int>(dataWidth),
                                 bytesPerVoxel));
  }

  const size_t sx = static_cast<size_t>(readLE<uint16_t>(p + 6));
  const size_t sy = static_cast<size_t>(readLE<uint16_t>(p + 8));
  const size_t sz = static_cast<size_t>(readLE<uint16_t>(p + 10));
  if (sx == 0 || sy == 0 || sz == 0) {
    throw ZException(fmt::format("Invalid compresso chunk dimensions: {}x{}x{}", sx, sy, sz));
  }
  if (expectedChunkSize != std::array<size_t, 3>{sx, sy, sz}) {
    throw ZException(fmt::format("Compresso chunk dimension mismatch: decoded {}x{}x{}, expected {}x{}x{}",
                                 sx,
                                 sy,
                                 sz,
                                 expectedChunkSize[0],
                                 expectedChunkSize[1],
                                 expectedChunkSize[2]));
  }

  const size_t xstep = static_cast<size_t>(p[12]);
  const size_t ystep = static_cast<size_t>(p[13]);
  const size_t zstep = static_cast<size_t>(p[14]);
  if (xstep == 0 || ystep == 0 || zstep == 0) {
    throw ZException("Invalid compresso chunk: step sizes must be > 0");
  }
  const size_t windowBitCount = checkedMul(checkedMul(xstep, ystep, "compresso window step product"), zstep, "compresso window step product");
  if (windowBitCount > 64) {
    throw ZException(fmt::format("Invalid compresso chunk: xstep*ystep*zstep must be <= 64 (got {})", windowBitCount));
  }

  const uint64_t idSizeU64 = readLE<uint64_t>(p + 15);
  const uint32_t valueSizeU32 = readLE<uint32_t>(p + 23);
  const uint64_t locationSizeU64 = readLE<uint64_t>(p + 27);
  const uint8_t connectivity = p[35];
  if (!(connectivity == 4 || connectivity == 6)) {
    throw ZException(fmt::format("Invalid compresso chunk: unsupported connectivity {}", static_cast<int>(connectivity)));
  }
  if (connectivity == 6 && formatVersion != 0) {
    throw ZException("Invalid compresso chunk: connectivity 6 requires format_version 0");
  }

  const size_t idSize = static_cast<size_t>(idSizeU64);
  const size_t valueSize = static_cast<size_t>(valueSizeU32);
  const size_t locationSize = static_cast<size_t>(locationSizeU64);

  const size_t sxy = checkedMul(sx, sy, "compresso sxy");
  const size_t voxelCount = checkedMul(sxy, sz, "compresso voxel count");

  const size_t nx = (sx + xstep - 1) / xstep;
  const size_t ny = (sy + ystep - 1) / ystep;
  const size_t nz = (sz + zstep - 1) / zstep;
  const size_t nblocks = checkedMul(checkedMul(nx, ny, "compresso nblocks"), nz, "compresso nblocks");

  size_t windowTypeBytes = 8;
  if (windowBitCount <= 8) {
    windowTypeBytes = 1;
  } else if (windowBitCount <= 16) {
    windowTypeBytes = 2;
  } else if (windowBitCount <= 32) {
    windowTypeBytes = 4;
  }

  const uint64_t worstCase = static_cast<uint64_t>(sxy) * 2ULL;
  size_t indexWidth = 8;
  if (worstCase < std::numeric_limits<uint8_t>::max()) {
    indexWidth = 1;
  } else if (worstCase < std::numeric_limits<uint16_t>::max()) {
    indexWidth = 2;
  } else if (worstCase < std::numeric_limits<uint32_t>::max()) {
    indexWidth = 4;
  }

  const bool hasZIndex = (formatVersion == 1);
  const size_t zIndexBytesPerTable = hasZIndex ? checkedMul(sz, indexWidth, "compresso z-index bytes") : 0;
  const size_t zIndexBytes = hasZIndex ? checkedMul(zIndexBytesPerTable, 2, "compresso z-index bytes") : 0;

  const size_t idsBytes = checkedMul(idSize, bytesPerVoxel, "compresso ids bytes");
  const size_t windowValuesBytes = checkedMul(valueSize, windowTypeBytes, "compresso window_values bytes");
  const size_t locationsBytes = checkedMul(locationSize, bytesPerVoxel, "compresso locations bytes");

  const size_t fixedBytes = checkedMul(1, kHeaderSize, "compresso header bytes") + idsBytes + windowValuesBytes + locationsBytes;
  if (bytes.size() < fixedBytes + zIndexBytes) {
    throw ZException("Invalid compresso chunk: payload too small for declared header sizes");
  }

  const size_t windowBytes = bytes.size() - fixedBytes - zIndexBytes;
  if ((windowBytes % windowTypeBytes) != 0) {
    throw ZException("Invalid compresso chunk: window data size is not a multiple of element size");
  }
  const size_t condensedWindowCount = windowBytes / windowTypeBytes;

  size_t offset = kHeaderSize;
  auto readU64OfWidth = [&](size_t widthBytes, const char* what) -> uint64_t {
    if (offset > bytes.size() || bytes.size() - offset < widthBytes) {
      throw ZException(fmt::format("Invalid compresso chunk: truncated while reading {}", what));
    }
    uint64_t v = 0;
    for (size_t i = 0; i < widthBytes; ++i) {
      v |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
    }
    offset += widthBytes;
    return v;
  };

  std::vector<uint64_t> ids;
  ids.resize(idSize + 1, 0);
  for (size_t i = 0; i < idSize; ++i) {
    ids[i + 1] = readU64OfWidth(bytesPerVoxel, "ids");
  }

  std::vector<uint64_t> windowValues;
  windowValues.resize(valueSize);
  for (size_t i = 0; i < valueSize; ++i) {
    windowValues[i] = readU64OfWidth(windowTypeBytes, "window_values");
  }

  std::vector<uint64_t> locations;
  locations.resize(locationSize);
  for (size_t i = 0; i < locationSize; ++i) {
    locations[i] = readU64OfWidth(bytesPerVoxel, "locations");
  }

  const size_t windowDataStart = offset;
  const size_t windowDataEnd = windowDataStart + windowBytes;
  if (windowDataEnd > bytes.size()) {
    throw ZException("Invalid compresso chunk: window region out of range");
  }

  std::vector<uint8_t> boundaries(voxelCount, 0);
  if (!windowValues.empty()) {
    std::vector<uint64_t> rleWindows;
    rleWindows.resize(condensedWindowCount);
    for (size_t i = 0; i < condensedWindowCount; ++i) {
      if (offset > bytes.size() || bytes.size() - offset < windowTypeBytes) {
        throw ZException("Invalid compresso chunk: truncated while reading windows");
      }
      rleWindows[i] = readU64OfWidth(windowTypeBytes, "windows");
    }
    if (offset != windowDataEnd) {
      throw ZException("Invalid compresso chunk: window decode did not consume expected bytes");
    }

    std::vector<uint64_t> windows(nblocks, 0);
    size_t blockIndex = 0;
    for (uint64_t word : rleWindows) {
      if ((word & 1ULL) != 0) {
        blockIndex += static_cast<size_t>(word >> 1ULL);
        if (blockIndex > nblocks) {
          throw ZException("Invalid compresso chunk: RLE windows overflow");
        }
      } else {
        if (blockIndex >= nblocks) {
          throw ZException("Invalid compresso chunk: RLE windows overflow");
        }
        windows[blockIndex] = word >> 1ULL;
        ++blockIndex;
      }
    }

    const bool xstepIsPowerOfTwo = std::has_single_bit(xstep);
    const size_t xshift = xstepIsPowerOfTwo ? static_cast<size_t>(std::countr_zero(static_cast<unsigned>(xstep))) : 0;

    for (size_t z = 0; z < sz; ++z) {
      const size_t zblock = checkedMul(nx, ny, "compresso zblock") * (z / zstep);
      const size_t zoffset = checkedMul(xstep, ystep, "compresso zoffset") * (z % zstep);
      for (size_t y = 0; y < sy; ++y) {
        const size_t yblock = nx * (y / ystep);
        const size_t yoffset = xstep * (y % ystep);
        for (size_t x = 0; x < sx; ++x) {
          const size_t iv = x + sx * y + sxy * z;

          const size_t xblock = xstepIsPowerOfTwo ? (x >> xshift) : (x / xstep);
          const size_t xoffset = xstepIsPowerOfTwo ? (x & (xstep - 1)) : (x % xstep);

          const size_t block = xblock + yblock + zblock;
          if (block >= nblocks) {
            throw ZException("Invalid compresso chunk: block index out of range");
          }
          const size_t bitOffset = xoffset + yoffset + zoffset;

          const uint64_t windowIndex = windows[block];
          if (windowIndex >= windowValues.size()) {
            throw ZException("Invalid compresso chunk: window index out of range");
          }
          const uint64_t windowValue = windowValues[windowIndex];
          boundaries[iv] = static_cast<uint8_t>((windowValue >> bitOffset) & 1ULL);
        }
      }
    }
  } else {
    // Still need to skip the window bytes to keep a consistent parse.
    offset = windowDataEnd;
  }

  if (offset != windowDataEnd) {
    throw ZException("Invalid compresso chunk: parse did not reach expected window end");
  }

  std::vector<uint32_t> components(voxelCount, 0);
  uint32_t nextLabel = 0;
  std::vector<size_t> stack;

  auto floodFill = [&](size_t startIdx) {
    CHECK(startIdx < voxelCount);
    CHECK(boundaries[startIdx] == 0);
    CHECK(components[startIdx] == 0);
    if (nextLabel == std::numeric_limits<uint32_t>::max()) {
      throw ZException("Compresso decode failed: too many connected components");
    }
    ++nextLabel;
    components[startIdx] = nextLabel;
    stack.clear();
    stack.push_back(startIdx);
    while (!stack.empty()) {
      const size_t idx = stack.back();
      stack.pop_back();

      const size_t z = idx / sxy;
      const size_t rem = idx - z * sxy;
      const size_t y = rem / sx;
      const size_t x = rem - y * sx;

      auto tryPush = [&](size_t nidx) {
        if (boundaries[nidx] != 0 || components[nidx] != 0) {
          return;
        }
        components[nidx] = nextLabel;
        stack.push_back(nidx);
      };

      if (x > 0) {
        tryPush(idx - 1);
      }
      if (x + 1 < sx) {
        tryPush(idx + 1);
      }
      if (y > 0) {
        tryPush(idx - sx);
      }
      if (y + 1 < sy) {
        tryPush(idx + sx);
      }
      if (connectivity == 6) {
        if (z > 0) {
          tryPush(idx - sxy);
        }
        if (z + 1 < sz) {
          tryPush(idx + sxy);
        }
      }
    }
  };

  // Raster scan order matches compresso reference implementation: z-major, then y, then x.
  for (size_t z = 0; z < sz; ++z) {
    for (size_t y = 0; y < sy; ++y) {
      for (size_t x = 0; x < sx; ++x) {
        const size_t idx = x + sx * y + sxy * z;
        if (boundaries[idx] != 0 || components[idx] != 0) {
          continue;
        }
        floodFill(idx);
      }
    }
  }

  if (static_cast<size_t>(nextLabel) > idSize) {
    throw ZException(fmt::format("Invalid compresso chunk: connected component count {} exceeds id_size {}", nextLabel, idSize));
  }

  std::vector<uint64_t> labels(voxelCount, 0);
  for (size_t i = 0; i < voxelCount; ++i) {
    const uint32_t comp = components[i];
    CHECK(comp <= nextLabel);
    labels[i] = ids[static_cast<size_t>(comp)];
  }

  size_t locIndex = 0;
  for (size_t z = 0; z < sz; ++z) {
    for (size_t y = 0; y < sy; ++y) {
      for (size_t x = 0; x < sx; ++x) {
        const size_t loc = x + sx * y + sxy * z;
        if (boundaries[loc] == 0) {
          continue;
        }

        if (x > 0 && boundaries[loc - 1] == 0) {
          labels[loc] = labels[loc - 1];
          continue;
        }
        if (y > 0 && boundaries[loc - sx] == 0) {
          labels[loc] = labels[loc - sx];
          continue;
        }
        if (connectivity == 6 && z > 0 && boundaries[loc - sxy] == 0) {
          labels[loc] = labels[loc - sxy];
          continue;
        }

        if (locations.empty()) {
          throw ZException("Invalid compresso chunk: missing locations data for boundary voxels");
        }
        if (locIndex >= locations.size()) {
          throw ZException("Invalid compresso chunk: locations index out of range");
        }
        const uint64_t offsetWord = locations[locIndex];

        if (offsetWord == 0) {
          if (x == 0) {
            throw ZException("Invalid compresso chunk: location offset 0 at x==0");
          }
          labels[loc] = labels[loc - 1];
        } else if (offsetWord == 1) {
          if (x + 1 >= sx) {
            throw ZException("Invalid compresso chunk: location offset 1 at x==sx-1");
          }
          labels[loc] = labels[loc + 1];
        } else if (offsetWord == 2) {
          if (y == 0) {
            throw ZException("Invalid compresso chunk: location offset 2 at y==0");
          }
          labels[loc] = labels[loc - sx];
        } else if (offsetWord == 3) {
          if (y + 1 >= sy) {
            throw ZException("Invalid compresso chunk: location offset 3 at y==sy-1");
          }
          labels[loc] = labels[loc + sx];
        } else if (offsetWord == 4) {
          if (z == 0) {
            throw ZException("Invalid compresso chunk: location offset 4 at z==0");
          }
          labels[loc] = labels[loc - sxy];
        } else if (offsetWord == 5) {
          if (z + 1 >= sz) {
            throw ZException("Invalid compresso chunk: location offset 5 at z==sz-1");
          }
          labels[loc] = labels[loc + sxy];
        } else if (offsetWord == 6) {
          if (locIndex + 1 >= locations.size()) {
            throw ZException("Invalid compresso chunk: location offset 6 requires a second word");
          }
          labels[loc] = locations[locIndex + 1];
          ++locIndex;
        } else {
          if (offsetWord < 7) {
            throw ZException("Invalid compresso chunk: location literal underflow");
          }
          labels[loc] = offsetWord - 7;
        }
        ++locIndex;
      }
    }
  }

  std::vector<uint8_t> out(checkedMul(voxelCount, bytesPerVoxel, "compresso decoded bytes"));
  for (size_t i = 0; i < voxelCount; ++i) {
    const uint64_t v = labels[i];
    for (size_t b = 0; b < bytesPerVoxel; ++b) {
      out[i * bytesPerVoxel + b] = static_cast<uint8_t>((v >> (8 * b)) & 0xFFULL);
    }
  }
  return out;
}

std::vector<uint8_t> ZNeuroglancerPrecomputedChunkDecoder::decodeCompressedSegmentationToRaw(
  std::span<const uint8_t> bytes,
  const std::array<size_t, 3>& chunkSize,
  size_t numChannels,
  const std::array<size_t, 3>& blockSize,
  bool isUint64)
{
  if (numChannels == 0) {
    throw ZException("Invalid compressed_segmentation decode request: numChannels must be > 0");
  }
  for (size_t d = 0; d < 3; ++d) {
    if (chunkSize[d] == 0) {
      throw ZException("Invalid compressed_segmentation decode request: chunk size must be > 0");
    }
    if (blockSize[d] == 0) {
      throw ZException("Invalid compressed_segmentation decode request: block size must be > 0");
    }
  }
  if ((bytes.size() % 4) != 0) {
    throw ZException(fmt::format("Invalid compressed_segmentation chunk length: {} (expected multiple of 4)", bytes.size()));
  }

  const size_t sx = chunkSize[0];
  const size_t sy = chunkSize[1];
  const size_t sz = chunkSize[2];
  const size_t voxelCount = sx * sy * sz;
  const size_t bytesPerVoxel = isUint64 ? 8 : 4;
  const uint32_t uint32sPerElement = isUint64 ? 2 : 1;

  const size_t wordCount = bytes.size() / 4;
  auto readWord = [&](uint64_t wordIndex) -> uint32_t {
    const uint64_t byteOffset = wordIndex * 4;
    if (byteOffset + 4 > bytes.size()) {
      throw ZException("Invalid compressed_segmentation chunk: read past end of buffer");
    }
    return readU32LE(bytes.data() + byteOffset);
  };

  if (wordCount < numChannels) {
    throw ZException(fmt::format("Invalid compressed_segmentation chunk: too small for {}-channel header", numChannels));
  }

  std::vector<uint32_t> channelOffsetsWords(numChannels);
  for (size_t c = 0; c < numChannels; ++c) {
    channelOffsetsWords[c] = readWord(c);
  }
  if (channelOffsetsWords[0] != numChannels) {
    throw ZException(fmt::format("Invalid compressed_segmentation header: channel 0 offset is {}, expected {}",
                                 channelOffsetsWords[0],
                                 numChannels));
  }
  for (size_t c = 0; c < numChannels; ++c) {
    if (channelOffsetsWords[c] < numChannels || channelOffsetsWords[c] > wordCount) {
      throw ZException(fmt::format("Invalid compressed_segmentation header: channel {} offset {} out of range",
                                   c,
                                   channelOffsetsWords[c]));
    }
    if (c > 0 && channelOffsetsWords[c] < channelOffsetsWords[c - 1]) {
      throw ZException("Invalid compressed_segmentation header: channel offsets are not sorted");
    }
  }

  const size_t gx = (sx + blockSize[0] - 1) / blockSize[0];
  const size_t gy = (sy + blockSize[1] - 1) / blockSize[1];
  const size_t gz = (sz + blockSize[2] - 1) / blockSize[2];

  const uint64_t totalBlocks = static_cast<uint64_t>(gx) * static_cast<uint64_t>(gy) * static_cast<uint64_t>(gz);
  const uint64_t headerWords = totalBlocks * 2ULL;

  std::vector<uint8_t> out(voxelCount * numChannels * bytesPerVoxel);

  auto writeValueAt = [&](uint8_t* channelBase, size_t voxelIndex, uint64_t tableEntryWordOffset) {
    if (!isUint64) {
      const uint32_t v = readWord(tableEntryWordOffset);
      writeU32LE(channelBase + voxelIndex * bytesPerVoxel, v);
    } else {
      const uint32_t lo = readWord(tableEntryWordOffset);
      const uint32_t hi = readWord(tableEntryWordOffset + 1);
      uint8_t* p = channelBase + voxelIndex * bytesPerVoxel;
      writeU32LE(p, lo);
      writeU32LE(p + 4, hi);
    }
  };

  auto isAllowedEncodingBits = [](uint32_t b) -> bool {
    return b == 0 || b == 1 || b == 2 || b == 4 || b == 8 || b == 16 || b == 32;
  };

  const size_t strideY = sx;
  const size_t strideZ = sx * sy;

  for (size_t c = 0; c < numChannels; ++c) {
    const uint64_t baseOffset = channelOffsetsWords[c];
    if (baseOffset + headerWords > wordCount) {
      throw ZException(fmt::format("Invalid compressed_segmentation chunk: channel {} data too small for headers", c));
    }

    const uint64_t channelEnd = (c + 1 < numChannels) ? channelOffsetsWords[c + 1] : wordCount;
    if (channelEnd < baseOffset) {
      throw ZException("Invalid compressed_segmentation chunk: channel end < start");
    }
    if (baseOffset + headerWords > channelEnd) {
      throw ZException(fmt::format("Invalid compressed_segmentation chunk: channel {} header region overlaps next channel", c));
    }

    uint8_t* channelBase = out.data() + voxelCount * c * bytesPerVoxel;

    for (size_t bz = 0; bz < gz; ++bz) {
      const size_t z0 = bz * blockSize[2];
      const size_t z1 = std::min(z0 + blockSize[2], sz);
      for (size_t by = 0; by < gy; ++by) {
        const size_t y0 = by * blockSize[1];
        const size_t y1 = std::min(y0 + blockSize[1], sy);
        for (size_t bx = 0; bx < gx; ++bx) {
          const size_t x0 = bx * blockSize[0];
          const size_t x1 = std::min(x0 + blockSize[0], sx);

          const uint64_t blockIndex = static_cast<uint64_t>(bx) + static_cast<uint64_t>(gx) * (static_cast<uint64_t>(by) + static_cast<uint64_t>(gy) * static_cast<uint64_t>(bz));
          const uint64_t headerWordOffset = baseOffset + blockIndex * 2ULL;
          const uint32_t header0 = readWord(headerWordOffset);
          const uint32_t header1 = readWord(headerWordOffset + 1);

          const uint32_t tableOffsetRel = header0 & 0x00FFFFFFu;
          const uint32_t encodingBits = (header0 >> 24) & 0xFFu;
          if (!isAllowedEncodingBits(encodingBits)) {
            throw ZException(fmt::format("Invalid compressed_segmentation header: encodingBits={} is not supported", encodingBits));
          }

          const uint64_t tableOffsetAbs = baseOffset + tableOffsetRel;
          const uint64_t encodedValuesOffsetAbs = baseOffset + static_cast<uint64_t>(header1);
          if (tableOffsetAbs >= channelEnd) {
            throw ZException("Invalid compressed_segmentation header: lookup table offset out of range");
          }
          if (encodingBits > 0 && encodedValuesOffsetAbs >= channelEnd) {
            throw ZException("Invalid compressed_segmentation header: encoded values offset out of range");
          }

          if (encodingBits == 0) {
            // Single-value block.
            const uint64_t tableEntry = tableOffsetAbs;
            if (isUint64 && (tableEntry + 1) >= channelEnd) {
              throw ZException("Invalid compressed_segmentation chunk: uint64 lookup table entry out of range");
            }
            for (size_t z = z0; z < z1; ++z) {
              for (size_t y = y0; y < y1; ++y) {
                for (size_t x = x0; x < x1; ++x) {
                  const size_t voxelIndex = x + strideY * y + strideZ * z;
                  writeValueAt(channelBase, voxelIndex, tableEntry);
                }
              }
            }
            continue;
          }

          const uint64_t mask = (encodingBits == 32) ? 0xFFFFFFFFull : ((1ULL << encodingBits) - 1ULL);

          for (size_t z = z0; z < z1; ++z) {
            const size_t lz = z - z0;
            for (size_t y = y0; y < y1; ++y) {
              const size_t ly = y - y0;
              for (size_t x = x0; x < x1; ++x) {
                const size_t lx = x - x0;
                const uint64_t subchunkOffset =
                  static_cast<uint64_t>(lx) +
                  static_cast<uint64_t>(blockSize[0]) * (static_cast<uint64_t>(ly) + static_cast<uint64_t>(blockSize[1]) * static_cast<uint64_t>(lz));
                const uint64_t bitOffset = subchunkOffset * static_cast<uint64_t>(encodingBits);
                const uint64_t wordIndex = encodedValuesOffsetAbs + (bitOffset / 32ULL);
                const uint32_t wordOffset = static_cast<uint32_t>(bitOffset % 32ULL);
                if (wordIndex >= channelEnd) {
                  throw ZException("Invalid compressed_segmentation chunk: encoded value offset out of range");
                }
                const uint32_t encodedWord = readWord(wordIndex);
                const uint32_t decodedIndex =
                  (encodingBits == 32) ? encodedWord : static_cast<uint32_t>((encodedWord >> wordOffset) & mask);

                const uint64_t tableEntry = tableOffsetAbs + static_cast<uint64_t>(decodedIndex) * uint32sPerElement;
                if (tableEntry >= channelEnd) {
                  throw ZException("Invalid compressed_segmentation chunk: lookup table entry out of range");
                }
                if (isUint64 && (tableEntry + 1) >= channelEnd) {
                  throw ZException("Invalid compressed_segmentation chunk: uint64 lookup table entry out of range");
                }

                const size_t voxelIndex = x + strideY * y + strideZ * z;
                writeValueAt(channelBase, voxelIndex, tableEntry);
              }
            }
          }
        }
      }
    }
  }

  return out;
}

} // namespace nim
