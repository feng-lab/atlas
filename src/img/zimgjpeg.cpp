#include "zimgjpeg.h"

#include "zlog.h"
#include "ztiff.h"
#include "zioutils.h"
#include "zimage2dutils.h"
#include "../3rdparty/build/include/jpeglib.h"
#include <folly/ScopeGuard.h>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <algorithm>
#include <limits>
#include <vector>

namespace {

#define TIFFTAG_ORIENTATION 274 /* +image orientation */
#define ORIENTATION_TOPLEFT 1 /* row 0 top, col 0 lhs */
#define ORIENTATION_TOPRIGHT 2 /* row 0 top, col 0 rhs */
#define ORIENTATION_BOTRIGHT 3 /* row 0 bottom, col 0 rhs */
#define ORIENTATION_BOTLEFT 4 /* row 0 bottom, col 0 lhs */
#define ORIENTATION_LEFTTOP 5 /* row 0 lhs, col 0 top */
#define ORIENTATION_RIGHTTOP 6 /* row 0 rhs, col 0 top */
#define ORIENTATION_RIGHTBOT 7 /* row 0 rhs, col 0 bottom */
#define ORIENTATION_LEFTBOT 8 /* row 0 lhs, col 0 bottom */
#define TIFFTAG_JPEGIFOFFSET 513 /* !pointer to SOI marker */

using namespace nim;

constexpr int JPEG_LOSSLESS_PREDICTOR = 1;
constexpr int JPEG_LOSSLESS_POINT_TRANSFORM = 0;

struct my_error_mgr
{
  struct jpeg_error_mgr pub; /* "public" fields */

  // jmp_buf setjmp_buffer;	/* for return to caller */
};

/*
 * ERROR HANDLING:
 *
 * The JPEG library's standard error handler (jerror.c) is divided into
 * several "methods" which you can override individually.  This lets you
 * adjust the behavior without duplicating a lot of code, which you might
 * have to update with each future release.
 *
 * Our example here shows how to override the "error_exit" method so that
 * control is returned to the library's caller when a fatal error occurs,
 * rather than calling exit() as the standard error_exit method does.
 *
 * We use C's setjmp/longjmp facility to return control.  This means that the
 * routine which calls the JPEG library must first execute a setjmp() call to
 * establish the return point.  We want the replacement error_exit to do a
 * longjmp().  But we need to make the setjmp buffer accessible to the
 * error_exit routine.  To do this, we make a private extension of the
 * standard JPEG error handler object.  (If we were using C++, we'd say we
 * were making a subclass of the regular error handler.)
 *
 * Here's the extended error handler struct:
 */

/*
 * Here's the routine that will replace the standard error_exit method:
 */

void my_error_exit(j_common_ptr cinfo)
{
  /* cinfo->err really points to a my_error_mgr struct, so coerce pointer */
  // my_error_mgr* myerr = (my_error_mgr*) cinfo->err;

  char errbuffer[JMSG_LENGTH_MAX];

  /* Create the message */
  (cinfo->err->format_message)(cinfo, errbuffer);
  throw ZException(fmt::format("Libjpeg-turbo error: {}", errbuffer), ZException::Option::CheckErrno);
}

void createcinfo(jpeg_decompress_struct& cinfo, my_error_mgr& jerr)
{
  /* We set up the normal JPEG error routines, then override error_exit. */
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = my_error_exit;
  //  /* Establish the setjmp return context for my_error_exit to use. */
  //  if (setjmp(jerr.setjmp_buffer)) {
  //    char errbuffer[JMSG_LENGTH_MAX];

  //    /* Create the message */
  //    (cinfo.err->format_message) ((j_common_ptr)(&cinfo), errbuffer);

  //    /* If we get here, the JPEG code has signaled an error.
  //       * We need to clean up the JPEG object, close the input file, and return.
  //       */
  //    jpeg_destroy_decompress(&cinfo);
  //    throw ZException(fmt::format("Libjpeg-turbo error: {}", errbuffer), ZException::Option::CheckErrno);
  //  }
  /* Now we can initialize the JPEG decompression object. */
  jpeg_create_decompress(&cinfo);
}

void createcinfo(jpeg_compress_struct& cinfo, my_error_mgr& jerr)
{
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = my_error_exit;
  jpeg_create_compress(&cinfo);
}

void checkSupportedDataPrecision(int dataPrecision)
{
  if (dataPrecision < 2 || dataPrecision > 16) {
    throw ZException(
      fmt::format("Unsupported JPEG data precision: {} bits; Atlas JPEG reader supports 2-16 bit samples",
                  dataPrecision));
  }
}

size_t bytesPerVoxelForDataPrecision(int dataPrecision)
{
  checkSupportedDataPrecision(dataPrecision);
  return dataPrecision <= 8 ? 1 : 2;
}

bool canUseJpegColorConversion(const jpeg_decompress_struct& cinfo)
{
  // Lossless JPEG does not support color-space conversion. 8-bit and 12-bit
  // streams are the only precisions that can be lossy DCT streams, so keep the
  // historical YCbCr/CMYK-to-RGB behavior there.
  return cinfo.data_precision == 8 || cinfo.data_precision == 12;
}

void setRgbOutputIfSupported(jpeg_decompress_struct& cinfo)
{
  if (!canUseJpegColorConversion(cinfo)) {
    return;
  }

  if (cinfo.jpeg_color_space == JCS_YCbCr || cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK ||
      cinfo.jpeg_color_space == JCS_EXT_RGBX || cinfo.jpeg_color_space == JCS_EXT_BGRX ||
      cinfo.jpeg_color_space == JCS_EXT_XBGR || cinfo.jpeg_color_space == JCS_EXT_XRGB) {
    cinfo.out_color_space = JCS_RGB;
  }
}

void startReading(FILE* infile, jpeg_decompress_struct& cinfo)
{
  jpeg_stdio_src(&cinfo, infile);

  jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);

  /* Step 3: read file parameters with jpeg_read_header() */

  (void)jpeg_read_header(&cinfo, TRUE);
  checkSupportedDataPrecision(cinfo.data_precision);
  /* We can ignore the return value from jpeg_read_header since
   *   (a) suspension is not possible with the stdio data source, and
   *   (b) we passed TRUE to reject a tables-only JPEG file as an error.
   * See libjpeg.txt for more info.
   */

  /* Step 4: set parameters for decompression */

  setRgbOutputIfSupported(cinfo);
  cinfo.quantize_colors = FALSE;
}

void startReading(std::span<const uint8_t> jpegBytes, jpeg_decompress_struct& cinfo)
{
  if (jpegBytes.empty()) {
    throw ZException("Invalid JPEG memory buffer: empty payload");
  }
  if (jpegBytes.size() > static_cast<size_t>(std::numeric_limits<unsigned long>::max())) {
    throw ZException("Invalid JPEG memory buffer: payload too large");
  }
  jpeg_mem_src(&cinfo, jpegBytes.data(), static_cast<unsigned long>(jpegBytes.size()));

  jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);

  /* Step 3: read file parameters with jpeg_read_header() */

  (void)jpeg_read_header(&cinfo, TRUE);
  checkSupportedDataPrecision(cinfo.data_precision);
  /* We can ignore the return value from jpeg_read_header since
   *   (a) suspension is not possible with the stdio data source, and
   *   (b) we passed TRUE to reject a tables-only JPEG file as an error.
   * See libjpeg.txt for more info.
   */

  /* Step 4: set parameters for decompression */

  setRgbOutputIfSupported(cinfo);
  cinfo.quantize_colors = FALSE;
}

uint16_t getOrientation(jpeg_decompress_struct& cinfo)
{
  uint16_t orientation = ORIENTATION_TOPLEFT;
  jpeg_saved_marker_ptr marker = cinfo.marker_list;
  while (marker) {
    if (marker->data_length > 6 && std::equal(marker->data, marker->data + 6, "Exif\0\0")) {
      try {
        using Device = boost::iostreams::basic_array_source<char>;
        // reinterpret_cast allowed (AliasedType is char or unsigned char: this permits
        // examination of the object representation of any object as an array of unsigned char.)
        boost::iostreams::stream<Device> exifs(reinterpret_cast<char*>(marker->data) + 6, marker->data_length - 6);
        ZTiff exif;
        exif.load(exifs, true);
        if (exif.isValid()) {
          orientation = exif.ifds()[0].orientation();
        }
      }
      catch (const ZException& e) {
        LOG(WARNING) << "failed to read Orientation from Exif: " << e.what();
      }
      break;
    }

    marker = marker->next;
  }
  return orientation;
}

void readInfoFromJpeg(jpeg_decompress_struct& cinfo, ZImgInfo& info)
{
  uint16_t orientation = getOrientation(cinfo);

  jpeg_calc_output_dimensions(&cinfo);

  info.bytesPerVoxel = bytesPerVoxelForDataPrecision(cinfo.data_precision);
  info.validBitCount = cinfo.data_precision;
  info.width = cinfo.output_width;
  info.height = cinfo.output_height;
  info.depth = 1;
  info.numChannels = cinfo.output_components;
  info.numTimes = 1;
  info.voxelFormat = VoxelFormat::Unsigned;
  info.createDefaultDescriptions();

  if (orientation > 4) {
    std::swap(info.width, info.height);
  }
}

ZImgInfo readStartedInfoFromJpeg(const jpeg_decompress_struct& cinfo)
{
  ZImgInfo imgInfo;
  imgInfo.bytesPerVoxel = bytesPerVoxelForDataPrecision(cinfo.data_precision);
  imgInfo.validBitCount = cinfo.data_precision;
  imgInfo.width = cinfo.output_width;
  imgInfo.height = cinfo.output_height;
  imgInfo.depth = 1;
  imgInfo.numChannels = cinfo.output_components;
  imgInfo.numTimes = 1;
  imgInfo.voxelFormat = VoxelFormat::Unsigned;
  imgInfo.createDefaultDescriptions();
  return imgInfo;
}

template<typename TJpegSample>
JDIMENSION readJpegScanlines(jpeg_decompress_struct& cinfo, TJpegSample** scanlines, JDIMENSION maxLines);

template<>
JDIMENSION readJpegScanlines<JSAMPLE>(jpeg_decompress_struct& cinfo, JSAMPLE** scanlines, JDIMENSION maxLines)
{
  return jpeg_read_scanlines(&cinfo, scanlines, maxLines);
}

template<>
JDIMENSION readJpegScanlines<J12SAMPLE>(jpeg_decompress_struct& cinfo, J12SAMPLE** scanlines, JDIMENSION maxLines)
{
  return jpeg12_read_scanlines(&cinfo, scanlines, maxLines);
}

template<>
JDIMENSION readJpegScanlines<J16SAMPLE>(jpeg_decompress_struct& cinfo, J16SAMPLE** scanlines, JDIMENSION maxLines)
{
  return jpeg16_read_scanlines(&cinfo, scanlines, maxLines);
}

template<typename TVoxel>
void applyOrientation(ZImg& imgTmp, uint16_t orientation)
{
  if (orientation > 4) {
    std::swap(imgTmp.infoRef().width, imgTmp.infoRef().height);
  }

  switch (orientation) {
    case ORIENTATION_TOPLEFT:
      break;
    case ORIENTATION_TOPRIGHT:
      for (size_t i = 0; i < imgTmp.numChannels(); ++i) {
        image2DFlip(imgTmp.channelData<TVoxel>(i), imgTmp.width(), imgTmp.height(), Dimension::X);
      }
      break;
    case ORIENTATION_BOTRIGHT:
      for (size_t i = 0; i < imgTmp.numChannels(); ++i) {
        image2DReflect(imgTmp.channelData<TVoxel>(i), imgTmp.width(), imgTmp.height());
      }
      break;
    case ORIENTATION_BOTLEFT:
      for (size_t i = 0; i < imgTmp.numChannels(); ++i) {
        image2DFlip(imgTmp.channelData<TVoxel>(i), imgTmp.width(), imgTmp.height(), Dimension::Y);
      }
      break;
    case ORIENTATION_LEFTTOP:
      for (size_t i = 0; i < imgTmp.numChannels(); ++i) {
        image2DTranspose(imgTmp.channelData<TVoxel>(i), imgTmp.height(), imgTmp.width());
      }
      break;
    case ORIENTATION_RIGHTTOP:
      for (size_t i = 0; i < imgTmp.numChannels(); ++i) {
        image2DTranspose(imgTmp.channelData<TVoxel>(i), imgTmp.height(), imgTmp.width());
        image2DFlip(imgTmp.channelData<TVoxel>(i), imgTmp.width(), imgTmp.height(), Dimension::X);
      }
      break;
    case ORIENTATION_RIGHTBOT:
      for (size_t i = 0; i < imgTmp.numChannels(); ++i) {
        image2DTranspose(imgTmp.channelData<TVoxel>(i), imgTmp.height(), imgTmp.width());
        image2DReflect(imgTmp.channelData<TVoxel>(i), imgTmp.width(), imgTmp.height());
      }
      break;
    case ORIENTATION_LEFTBOT:
      for (size_t i = 0; i < imgTmp.numChannels(); ++i) {
        image2DTranspose(imgTmp.channelData<TVoxel>(i), imgTmp.height(), imgTmp.width());
        image2DFlip(imgTmp.channelData<TVoxel>(i), imgTmp.width(), imgTmp.height(), Dimension::Y);
      }
      break;
    default:
      break;
  }
}

template<typename TJpegSample, typename TVoxel>
void readImgFromJpegTyped(jpeg_decompress_struct& cinfo, ZImg& img, const ZImgRegion& region, uint16_t orientation)
{
  /* Step 5: Start decompressor */
  (void)jpeg_start_decompress(&cinfo);

  ZImgInfo imgInfo = readStartedInfoFromJpeg(cinfo);

  if (region.isEmpty() || !region.isValid(imgInfo)) {
    throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", imgInfo, region));
  }

  ZImgInfo partialImgInfo = region.clip(imgInfo);
  ZImg imgTmp(partialImgInfo);

  const size_t scanlineSampleCount = imgInfo.width * imgInfo.numChannels;
  std::vector<TJpegSample> scanlineBuffer(scanlineSampleCount * cinfo.rec_outbuf_height);
  std::vector<TJpegSample*> scanlineRows(cinfo.rec_outbuf_height);
  for (size_t row = 0; row < scanlineRows.size(); ++row) {
    scanlineRows[row] = scanlineBuffer.data() + row * scanlineSampleCount;
  }

  /* Step 6: while (scan lines remain to be read) */
  /*           jpeg_read_scanlines(...); */

  /* Here we use the library's state variable cinfo.output_scanline as the
   * loop counter, so that we don't have to keep track ourselves.
   */
  size_t lineStart = 0;
  while (cinfo.output_scanline < cinfo.output_height) {
    /* jpeg_read_scanlines expects an array of pointers to scanlines.
     * Here the array is only one element long, but you could ask for
     * more than one scanline at a time if that's more convenient.
     */
    size_t lineRead = readJpegScanlines(cinfo, scanlineRows.data(), cinfo.rec_outbuf_height);

    for (size_t y = lineStart; y < lineStart + lineRead; ++y) {
      if (region.yInRegion(y)) {
        const TJpegSample* row = scanlineRows[y - lineStart];
        if (imgInfo.numChannels == 1) {
          std::copy_n(row + region.start.x, imgTmp.width(), imgTmp.rowData<TVoxel>(y - region.start.y));
        } else {
          size_t cEnd = region.end.c == -1 ? imgInfo.numChannels : region.end.c;
          size_t xEnd = region.end.x == -1 ? imgInfo.width : region.end.x;
          for (size_t c = region.start.c; c < cEnd; ++c) {
            for (size_t x = region.start.x; x < xEnd; ++x) {
              *imgTmp.data<TVoxel>(x - region.start.x, y - region.start.y, 0, c - region.start.c) =
                static_cast<TVoxel>(row[x * imgInfo.numChannels + c]);
            }
          }
        }
      }
    }

    lineStart += lineRead;
  }

  /* Step 7: Finish decompression */
  (void)jpeg_finish_decompress(&cinfo);

  applyOrientation<TVoxel>(imgTmp, orientation);

  imgTmp.swap(img);
}

void readImgFromJpeg(jpeg_decompress_struct& cinfo, ZImg& img, const ZImgRegion& region, uint16_t orientation)
{
  if (cinfo.data_precision <= 8) {
    readImgFromJpegTyped<JSAMPLE, uint8_t>(cinfo, img, region, orientation);
  } else if (cinfo.data_precision <= 12) {
    readImgFromJpegTyped<J12SAMPLE, uint16_t>(cinfo, img, region, orientation);
  } else {
    readImgFromJpegTyped<J16SAMPLE, uint16_t>(cinfo, img, region, orientation);
  }
}

size_t jpegInputComponentCount(const ZImgInfo& info)
{
  return info.numChannels == 1 ? 1 : 3;
}

int dataPrecisionForWrite(const ZImgInfo& info)
{
  if (info.bytesPerVoxel == 1) {
    if (info.validBitCount > 8) {
      throw ZException(fmt::format("uint8 JPEG image has invalid validBitCount: {}", info.validBitCount));
    }
    return 8;
  }

  if (info.bytesPerVoxel == 2) {
    const size_t precision = info.validBitCount == 0 ? 16 : info.validBitCount;
    if (precision < 2 || precision > 16) {
      throw ZException(fmt::format("uint16 JPEG image has unsupported validBitCount: {}", info.validBitCount));
    }
    return static_cast<int>(precision);
  }

  throw ZException(fmt::format("JPEG writer supports uint8 and uint16 images only: {}", info));
}

bool usesLosslessJpeg(int dataPrecision)
{
  return dataPrecision != 8 && dataPrecision != 12;
}

uint64_t maxSampleValueForPrecision(int dataPrecision)
{
  CHECK(dataPrecision >= 2 && dataPrecision <= 16);
  return (1ULL << dataPrecision) - 1;
}

void setChrominanceSubsampling(jpeg_compress_struct& cinfo, uint32_t jpegChrominanceSubsampling)
{
  if (cinfo.input_components == 1) {
    return;
  }

  CHECK(cinfo.num_components >= 3);
  cinfo.comp_info[0].h_samp_factor = 1;
  cinfo.comp_info[0].v_samp_factor = 1;
  cinfo.comp_info[1].h_samp_factor = 1;
  cinfo.comp_info[1].v_samp_factor = 1;
  cinfo.comp_info[2].h_samp_factor = 1;
  cinfo.comp_info[2].v_samp_factor = 1;

  if (jpegChrominanceSubsampling == 422) {
    cinfo.comp_info[0].h_samp_factor = 2;
  } else if (jpegChrominanceSubsampling == 420) {
    cinfo.comp_info[0].h_samp_factor = 2;
    cinfo.comp_info[0].v_samp_factor = 2;
  } else {
    CHECK(jpegChrominanceSubsampling == 444);
  }
}

void configureCompression(jpeg_compress_struct& cinfo,
                          const ZImg& img,
                          int dataPrecision,
                          const ZImgWriteParameters& paras)
{
  cinfo.image_width = static_cast<JDIMENSION>(img.width());
  cinfo.image_height = static_cast<JDIMENSION>(img.height());
  cinfo.input_components = static_cast<int>(jpegInputComponentCount(img.info()));
  cinfo.in_color_space = cinfo.input_components == 1 ? JCS_GRAYSCALE : JCS_RGB;
  cinfo.data_precision = dataPrecision;

  jpeg_set_defaults(&cinfo);

  if (usesLosslessJpeg(dataPrecision)) {
    jpeg_enable_lossless(&cinfo, JPEG_LOSSLESS_PREDICTOR, JPEG_LOSSLESS_POINT_TRANSFORM);
    return;
  }

  jpeg_set_quality(&cinfo, static_cast<int>(paras.jpegQuality), TRUE);
  cinfo.dct_method = paras.jpegAccurateDCT ? JDCT_ISLOW : JDCT_FASTEST;
  setChrominanceSubsampling(cinfo, paras.jpegChrominanceSubsampling);
  if (paras.jpegProgressive) {
    jpeg_simple_progression(&cinfo);
  }
}

template<typename TJpegSample>
JDIMENSION writeJpegScanlines(jpeg_compress_struct& cinfo, TJpegSample** scanlines, JDIMENSION numLines);

template<>
JDIMENSION writeJpegScanlines<JSAMPLE>(jpeg_compress_struct& cinfo, JSAMPLE** scanlines, JDIMENSION numLines)
{
  return jpeg_write_scanlines(&cinfo, scanlines, numLines);
}

template<>
JDIMENSION writeJpegScanlines<J12SAMPLE>(jpeg_compress_struct& cinfo, J12SAMPLE** scanlines, JDIMENSION numLines)
{
  return jpeg12_write_scanlines(&cinfo, scanlines, numLines);
}

template<>
JDIMENSION writeJpegScanlines<J16SAMPLE>(jpeg_compress_struct& cinfo, J16SAMPLE** scanlines, JDIMENSION numLines)
{
  return jpeg16_write_scanlines(&cinfo, scanlines, numLines);
}

template<typename TJpegSample, typename TVoxel>
TJpegSample checkedJpegSample(TVoxel value, uint64_t maxSampleValue, bool validateSampleRange, int dataPrecision)
{
  if (validateSampleRange && static_cast<uint64_t>(value) > maxSampleValue) {
    throw ZException(fmt::format("Image value {} exceeds the maximum value {} for {}-bit JPEG output",
                                 static_cast<uint64_t>(value),
                                 maxSampleValue,
                                 dataPrecision));
  }
  return static_cast<TJpegSample>(value);
}

template<typename TJpegSample, typename TVoxel>
void fillWriteScanline(const ZImg& img,
                       size_t y,
                       std::vector<TJpegSample>& row,
                       uint64_t maxSampleValue,
                       bool validateSampleRange,
                       int dataPrecision)
{
  if (img.numChannels() == 1) {
    const TVoxel* src = img.rowData<TVoxel>(y);
    for (size_t x = 0; x < img.width(); ++x) {
      row[x] = checkedJpegSample<TJpegSample>(src[x], maxSampleValue, validateSampleRange, dataPrecision);
    }
    return;
  }

  const TVoxel* r = img.rowData<TVoxel>(y, 0, 0);
  const TVoxel* g = img.rowData<TVoxel>(y, 0, 1);
  const TVoxel* b = img.rowData<TVoxel>(y, 0, 2);
  for (size_t x = 0; x < img.width(); ++x) {
    row[x * 3] = checkedJpegSample<TJpegSample>(r[x], maxSampleValue, validateSampleRange, dataPrecision);
    row[x * 3 + 1] = checkedJpegSample<TJpegSample>(g[x], maxSampleValue, validateSampleRange, dataPrecision);
    row[x * 3 + 2] = checkedJpegSample<TJpegSample>(b[x], maxSampleValue, validateSampleRange, dataPrecision);
  }
}

template<typename TJpegSample, typename TVoxel>
void writeImgToJpeg(jpeg_compress_struct& cinfo, const ZImg& img, int dataPrecision)
{
  const size_t rowSampleCount = img.width() * jpegInputComponentCount(img.info());
  std::vector<TJpegSample> row(rowSampleCount);
  TJpegSample* rowPointer[1] = {row.data()};
  const uint64_t maxSampleValue = maxSampleValueForPrecision(dataPrecision);
  const bool validateSampleRange = maxSampleValue < std::numeric_limits<TVoxel>::max();

  while (cinfo.next_scanline < cinfo.image_height) {
    fillWriteScanline<TJpegSample, TVoxel>(img,
                                           cinfo.next_scanline,
                                           row,
                                           maxSampleValue,
                                           validateSampleRange,
                                           dataPrecision);
    (void)writeJpegScanlines(cinfo, rowPointer, 1);
  }
}

} // namespace

namespace nim {

ZImgJpeg& ZImgJpeg::instance()
{
  static ZImgJpeg imgJpeg;
  return imgJpeg;
}

QString ZImgJpeg::shortName() const
{
  return "Jpeg";
}

QString ZImgJpeg::fullName() const
{
  return "Jpeg";
}

QStringList ZImgJpeg::extensions() const
{
  QStringList res;
  res << "jpg"
      << "jpeg";
  return res;
}

void ZImgJpeg::readInfo(const QString& filename,
                        std::vector<ZImgInfo>& infos,
                        std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  auto infile = openFile(filename, "rb");

  struct jpeg_decompress_struct cinfo;
  my_error_mgr jerr;
  createcinfo(cinfo, jerr);
  auto guard1 = folly::makeGuard([&cinfo]() {
    /* Step 8: Release JPEG decompression object */

    /* This is an important step since it will release a good deal of memory. */
    jpeg_destroy_decompress(&cinfo);
  });

  startReading(infile.get(), cinfo);

  infos.resize(1);
  readInfoFromJpeg(cinfo, infos[0]);

  createDefaultSubBlocks(filename, infos, subBlocks);
}

void ZImgJpeg::readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene)
{
  if (scene != 0) {
    throw ZException("invalid scene");
  }
  auto infile = openFile(filename, "rb");

  struct jpeg_decompress_struct cinfo;
  my_error_mgr jerr;
  createcinfo(cinfo, jerr);
  auto guard1 = folly::makeGuard([&cinfo]() {
    /* Step 8: Release JPEG decompression object */

    /* This is an important step since it will release a good deal of memory. */
    jpeg_destroy_decompress(&cinfo);
  });

  startReading(infile.get(), cinfo);

  // extract exif
  jpeg_saved_marker_ptr marker = cinfo.marker_list;
  while (marker) {
    if (marker->data_length > 6 && std::equal(marker->data, marker->data + 6, "Exif\0\0")) {
      using Device = boost::iostreams::basic_array_source<char>;
      boost::iostreams::stream<Device> exifs(reinterpret_cast<char*>(marker->data) + 6, marker->data_length - 6);
      ZTiff exif;
      try {
        exif.load(exifs, true);
        if (exif.isValid()) {
          meta.attachToTopLevel(exif.ifds()[0].extractMetadata());
        }
      }
      catch (const ZException& e) {
        LOG(WARNING) << "failed to read metadata from jpeg: " << e.what();
      }
      break;
    }

    marker = marker->next;
  }
}

void ZImgJpeg::readThumbnail(const QString& filename,
                             ZImgThumbernail& thumbnail,
                             const ZImgRegion& region,
                             size_t scene)
{
  if (scene != 0) {
    throw ZException("invalid scene");
  }
  auto infile = openFile(filename, "rb");

  struct jpeg_decompress_struct cinfo;
  my_error_mgr jerr;
  createcinfo(cinfo, jerr);
  auto guard1 = folly::makeGuard([&cinfo]() {
    /* Step 8: Release JPEG decompression object */

    /* This is an important step since it will release a good deal of memory. */
    jpeg_destroy_decompress(&cinfo);
  });

  startReading(infile.get(), cinfo);

  // extract thumbnails
  jpeg_saved_marker_ptr marker = cinfo.marker_list;
  while (marker) {
    if (marker->data_length > 6 && std::equal(marker->data, marker->data + 6, "Exif\0\0")) {
      using Device = boost::iostreams::basic_array_source<char>;
      boost::iostreams::stream<Device> exifs(reinterpret_cast<char*>(marker->data) + 6, marker->data_length - 6);
      ZTiff exif;
      try {
        exif.load(exifs, true);
        if (exif.isValid()) {
          for (size_t i = 1; i < exif.ifds().size(); ++i) {
            const ZTiffIFD& thumbIFD = exif.ifds()[i];
            auto idx = thumbIFD.indexOf(TIFFTAG_JPEGIFOFFSET);
            if (idx != -1) {
              // found jpeg thumbnail stream
              std::vector<uint8_t> buf;
              const ZImgMetatag& tag = thumbIFD.tag(idx);
              auto off = tag.dataAt<uint32_t>(0);
              buf.assign(marker->data + 6 + off, marker->data + marker->data_length);

              struct jpeg_decompress_struct thumbCinfo;
              my_error_mgr thumbJerr;
              createcinfo(thumbCinfo, thumbJerr);
              auto guard2 = folly::makeGuard([&thumbCinfo]() {
                /* Step 8: Release JPEG decompression object */

                /* This is an important step since it will release a good deal of memory. */
                jpeg_destroy_decompress(&thumbCinfo);
              });

              // don't slice thumbnail in x, y, c
              ZImgRegion thumbRegion = region;
              thumbRegion.start.x = 0;
              thumbRegion.end.x = -1;
              thumbRegion.start.y = 0;
              thumbRegion.end.y = -1;
              thumbRegion.start.c = 0;
              thumbRegion.end.c = -1;

              ZImg thumbImg;

              startReading(std::span<const uint8_t>(buf.data(), buf.size()), thumbCinfo);
              readImgFromJpeg(thumbCinfo, thumbImg, thumbRegion, getOrientation(cinfo));

              thumbnail.attachToPlane(thumbImg, 0, 0);
            }
          }
        }
      }
      catch (const ZException& e) {
        LOG(WARNING) << "failed to read thumbnail from jpeg: " << e.what();
      }
      break;
    }

    marker = marker->next;
  }
}

void ZImgJpeg::readImg(const QString& filename,
                       ZImg& img,
                       const ZImgRegion& region,
                       size_t scene,
                       const ZImgReadOptions& readOptions)
{
  if (scene != 0) {
    throw ZException("invalid scene");
  }
  auto infile = openFile(filename, "rb");

  struct jpeg_decompress_struct cinfo;
  my_error_mgr jerr;
  createcinfo(cinfo, jerr);
  auto guard1 = folly::makeGuard([&cinfo]() {
    /* Step 8: Release JPEG decompression object */

    /* This is an important step since it will release a good deal of memory. */
    jpeg_destroy_decompress(&cinfo);
  });

  startReading(infile.get(), cinfo);

  readImgFromJpeg(cinfo, img, region, getOrientation(cinfo));

  if (readOptions.includeThumbnails) {
    readThumbnail(filename, img.thumbnailRef(), region, scene);
  }
  if (readOptions.includeMetadata) {
    readMetadata(filename, img.metadataRef(), scene);
  }
}

void ZImgJpeg::checkImgBeforeWriting(const QString& filename, const ZImgInfo& info, const ZImgWriteParameters& paras)
{
  ZImgFormat::checkImgBeforeWriting(filename, info, paras);
  if (paras.compression != Compression::AUTO) {
    throw ZException(fmt::format("compression {} is not supported", paras.compression));
  }
  if (info.numTimes != 1 || info.depth != 1) {
    throw ZException(fmt::format("only 2d image is supported: {}", info));
  }
  if (!(info.numChannels == 1 || (info.numChannels == 4 && info.lastChannelIsAlphaChannel) ||
        (info.numChannels == 3 && !info.lastChannelIsAlphaChannel)) ||
      info.voxelFormat != VoxelFormat::Unsigned || (info.bytesPerVoxel != 1 && info.bytesPerVoxel != 2)) {
    throw ZException(fmt::format("image can not be represented as jpeg: {}", info));
  }
  const int dataPrecision = dataPrecisionForWrite(info);
  if (usesLosslessJpeg(dataPrecision) && paras.jpegChrominanceSubsampling != 444) {
    throw ZException(
      fmt::format("chrominance subsampling is not supported for {}-bit lossless JPEG output", dataPrecision));
  }
  if (paras.jpegChrominanceSubsampling != 444 && paras.jpegChrominanceSubsampling != 422 &&
      paras.jpegChrominanceSubsampling != 420) {
    throw ZException(fmt::format("unsupported chrominance subsampling: {}", paras.jpegChrominanceSubsampling));
  }
  if (paras.jpegQuality < 1 || paras.jpegQuality > 100) {
    throw ZException(fmt::format("invalid jpeg quality: {}", paras.jpegQuality));
  }
}

void ZImgJpeg::writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, img.info(), paras);

  if (img.numChannels() == 4) {
    LOG(WARNING) << "Alpha Channel will be ignored when encoding as jpeg";
  }

  const int dataPrecision = dataPrecisionForWrite(img.info());
  auto outfile = openFile(filename, "wb");

  struct jpeg_compress_struct cinfo;
  my_error_mgr jerr;
  createcinfo(cinfo, jerr);
  auto guard1 = folly::makeGuard([&cinfo]() {
    jpeg_destroy_compress(&cinfo);
  });

  jpeg_stdio_dest(&cinfo, outfile.get());
  configureCompression(cinfo, img, dataPrecision, paras);

  jpeg_start_compress(&cinfo, TRUE);
  if (img.bytesPerVoxel() == 1) {
    writeImgToJpeg<JSAMPLE, uint8_t>(cinfo, img, dataPrecision);
  } else if (dataPrecision <= 8) {
    writeImgToJpeg<JSAMPLE, uint16_t>(cinfo, img, dataPrecision);
  } else if (dataPrecision <= 12) {
    writeImgToJpeg<J12SAMPLE, uint16_t>(cinfo, img, dataPrecision);
  } else {
    writeImgToJpeg<J16SAMPLE, uint16_t>(cinfo, img, dataPrecision);
  }
  jpeg_finish_compress(&cinfo);
}

bool ZImgJpeg::supportRead() const
{
  return true;
}

bool ZImgJpeg::supportWrite() const
{
  return true;
}

ZImgInfo ZImgJpeg::readMemInfo(std::span<const uint8_t> jpegBytes)
{
  struct jpeg_decompress_struct cinfo;
  my_error_mgr jerr;
  createcinfo(cinfo, jerr);
  auto guard1 = folly::makeGuard([&cinfo]() {
    /* Step 8: Release JPEG decompression object */

    /* This is an important step since it will release a good deal of memory. */
    jpeg_destroy_decompress(&cinfo);
  });

  startReading(jpegBytes, cinfo);

  ZImgInfo info;
  readInfoFromJpeg(cinfo, info);
  return info;
}

void ZImgJpeg::readMemImg(std::span<const uint8_t> jpegBytes, std::span<uint8_t> des)
{
  struct jpeg_decompress_struct cinfo;
  my_error_mgr jerr;
  createcinfo(cinfo, jerr);
  auto guard1 = folly::makeGuard([&cinfo]() {
    /* Step 8: Release JPEG decompression object */

    /* This is an important step since it will release a good deal of memory. */
    jpeg_destroy_decompress(&cinfo);
  });

  startReading(jpegBytes, cinfo);

  ZImg img;
  readImgFromJpeg(cinfo, img, ZImgRegion(), getOrientation(cinfo));

  if (des.size() < img.byteNumber()) {
    throw ZException("buffer space is not enough");
  }

  std::copy_n(img.channelData<uint8_t>(0), img.byteNumber(), des.data());
}

} // namespace nim
