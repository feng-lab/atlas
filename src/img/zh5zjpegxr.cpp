#include "zh5zjpegxr.h"

#include "zimgjpegxr.h"
#include <QTemporaryFile>

#define H5Z_class_t_vers 2
#include "hdf5.h"

namespace nim {

static size_t H5Z_filter_jpegxr(unsigned int flags,
                                size_t cd_nelmts,
                                const unsigned int cd_values[],
                                size_t nbytes,
                                size_t* buf_size,
                                void** buf)
{
  void* outBuf = nullptr;
  size_t retValue = 0;

  try {
    if (flags & H5Z_FLAG_REVERSE) {
      // read data, e.g., decompress data
      ZImgInfo info;
      ZImgJpegXR::readMemInfo(*buf, *buf_size, info);
      if (nullptr == (outBuf = H5allocate_memory(info.byteNumber(), false))) {
        throw ZException("error calling H5allocate_memory");
      }
      ZImgJpegXR::readMemImg(*buf, nbytes, outBuf, info.byteNumber());

      H5free_memory(*buf);
      *buf = outBuf;
      *buf_size = info.byteNumber();
      outBuf = nullptr;
      retValue = info.byteNumber();
    } else {
      // write data, e.g., compress data
      ZImgWriteParameters paras;
      if (cd_nelmts < 4) {
        throw ZException("not enough parameters");
      }
      paras.jpegXRQuality = std::bit_cast<float>(cd_values[0]);
      ZImgInfo info(cd_values[3], cd_values[2], 1, 1, 1, cd_values[1]);
      if (nbytes < info.byteNumber()) {
        throw ZException("not enough data to compress");
      }
      ZImg img;
      img.wrapData(*buf, info);

      //      QTemporaryFile tempFile("XXXXXX.jxr");
      //      if (tempFile.open()) {
      //        ZImgJpegXR::instance().writeImg(tempFile.fileName(), img, paras);
      //        VLOG(1) << QFile(tempFile.fileName()).size();
      //      }

      auto memBuf = std::make_unique_for_overwrite<std::byte[]>(info.byteNumber());
      auto byteWritten = ZImgJpegXR::writeImgToMem(img, paras, memBuf.get(), info.byteNumber());
      // VLOG(1) << byteWritten;
      if (byteWritten > info.byteNumber()) {
        throw ZException("compression overflow");
      }
      memcpy(*buf, memBuf.get(), byteWritten);
      retValue = byteWritten;
    }
  }
  catch (const ZException& e) {
    LOG(ERROR) << e.what();
    if (outBuf) {
      H5free_memory(outBuf);
    }
  }

  return retValue;
}

int jpegxr_register_h5filter()
{
  const H5Z_class2_t H5Z_JPEGXR{
    H5Z_CLASS_T_VERS, /* H5Z_class_t version */
    (H5Z_filter_t)H5Z_FILTER_JPEGXR, /* Filter id number             */
    1, /* encoder_present flag (set to true) */
    1, /* decoder_present flag (set to true) */
    "HDF5 jpegxr filter", /* Filter name for debugging    */
    nullptr, /* The "can apply" callback     */
    nullptr, /* The "set local" callback     */
    (H5Z_func_t)H5Z_filter_jpegxr, /* The actual filter function   */
  };
  int retval = H5Zregister(&H5Z_JPEGXR);
  if (retval < 0) {
    LOG(ERROR) << "Can't register jpegxr filter";
  }
  return retval;
}

} // namespace nim
