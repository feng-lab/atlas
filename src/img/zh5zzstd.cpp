#include "zh5zzstd.h"

#include "zexception.h"
#include "zlog.h"

#define H5Z_class_t_vers 2
#include "hdf5.h"
#include <zstd.h>

#include <bit>
#include <cstdint>
#include <limits>

namespace nim {

namespace {

int normalizeZstdLevel(int level)
{
  if (level == 0) {
    return ZSTD_CLEVEL_DEFAULT;
  }
  return level;
}

void throwIfZstdError(size_t code, const char* operation)
{
  if (ZSTD_isError(code) != 0) {
    throw ZException(fmt::format("zstd {} failed: {}", operation, ZSTD_getErrorName(code)));
  }
}

static size_t H5Z_filter_zstd(unsigned int flags,
                              size_t cd_nelmts,
                              const unsigned int cd_values[],
                              size_t nbytes,
                              size_t* buf_size,
                              void** buf)
{
  void* outBuf = nullptr;

  try {
    if (flags & H5Z_FLAG_REVERSE) {
      const unsigned long long contentSize = ZSTD_getFrameContentSize(*buf, nbytes);
      if (contentSize == ZSTD_CONTENTSIZE_ERROR) {
        throw ZException("invalid zstd frame");
      }
      if (contentSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        throw ZException("zstd frame does not report decompressed size");
      }
      if (contentSize > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
        throw ZException(fmt::format("zstd decompressed size overflows size_t: {}", contentSize));
      }

      const auto outputSize = static_cast<size_t>(contentSize);
      outBuf = H5allocate_memory(outputSize, false);
      if (outBuf == nullptr) {
        throw ZException("error calling H5allocate_memory");
      }

      const size_t decompressedSize = ZSTD_decompress(outBuf, outputSize, *buf, nbytes);
      throwIfZstdError(decompressedSize, "decompress");
      if (decompressedSize != outputSize) {
        throw ZException(
          fmt::format("zstd decompressed {} bytes but frame reports {} bytes", decompressedSize, outputSize));
      }

      H5free_memory(*buf);
      *buf = outBuf;
      *buf_size = outputSize;
      outBuf = nullptr;
      return outputSize;
    }

    const int level = cd_nelmts > 0 ? normalizeZstdLevel(std::bit_cast<int32_t>(cd_values[0])) : ZSTD_CLEVEL_DEFAULT;
    const size_t outputCapacity = ZSTD_compressBound(nbytes);
    outBuf = H5allocate_memory(outputCapacity, false);
    if (outBuf == nullptr) {
      throw ZException("error calling H5allocate_memory");
    }

    const size_t compressedSize = ZSTD_compress(outBuf, outputCapacity, *buf, nbytes, level);
    throwIfZstdError(compressedSize, "compress");
    if (compressedSize >= nbytes) {
      H5free_memory(outBuf);
      return 0;
    }

    H5free_memory(*buf);
    *buf = outBuf;
    *buf_size = compressedSize;
    outBuf = nullptr;
    return compressedSize;
  }
  catch (const ZException& e) {
    LOG(ERROR) << e.what();
    if (outBuf != nullptr) {
      H5free_memory(outBuf);
    }
    return 0;
  }
}

} // namespace

int zstd_register_h5filter()
{
  if (H5Zfilter_avail(H5Z_FILTER_ZSTD) > 0) {
    return 0;
  }

  const H5Z_class2_t H5Z_ZSTD{
    H5Z_CLASS_T_VERS,
    static_cast<H5Z_filter_t>(H5Z_FILTER_ZSTD),
    1,
    1,
    "HDF5 zstd filter",
    nullptr,
    nullptr,
    reinterpret_cast<H5Z_func_t>(H5Z_filter_zstd),
  };
  const int retval = H5Zregister(&H5Z_ZSTD);
  if (retval < 0) {
    LOG(ERROR) << "Can't register zstd filter";
  }
  return retval;
}

} // namespace nim
