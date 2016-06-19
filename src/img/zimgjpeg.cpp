#include "zimgjpeg.h"
#include "QsLog.h"
#include <cassert>
#include "ztiff.h"
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <jpeglib.h>
//#include <setjmp.h>
#include <tiff.h>
#include "zimage2dutils.h"
#include <memory>
#include <QFile>

namespace {

using namespace nim;

struct my_error_mgr {
  struct jpeg_error_mgr pub;	/* "public" fields */

  //jmp_buf setjmp_buffer;	/* for return to caller */
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
  //my_error_mgr* myerr = (my_error_mgr*) cinfo->err;

  char errbuffer[JMSG_LENGTH_MAX];

  /* Create the message */
  (cinfo->err->format_message) (cinfo, errbuffer);

  QString error = QString("Libjpeg-turbo error: %1").arg(errbuffer);

  jpeg_destroy_decompress((j_decompress_ptr)cinfo);

  throw nim::ZIOException(error);
}

void startReading(FILE *infile, jpeg_decompress_struct &cinfo, my_error_mgr &jerr)
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
  //    throw ZIOException(QString("Libjpeg-turbo error:") + QString(errbuffer));
  //  }
  /* Now we can initialize the JPEG decompression object. */
  jpeg_create_decompress(&cinfo);

  jpeg_stdio_src(&cinfo, infile);

  jpeg_save_markers(&cinfo, JPEG_APP0+1, 0xFFFF);

  /* Step 3: read file parameters with jpeg_read_header() */

  (void) jpeg_read_header(&cinfo, TRUE);
  /* We can ignore the return value from jpeg_read_header since
     *   (a) suspension is not possible with the stdio data source, and
     *   (b) we passed TRUE to reject a tables-only JPEG file as an error.
     * See libjpeg.txt for more info.
     */

  /* Step 4: set parameters for decompression */

  if (cinfo.jpeg_color_space == JCS_YCbCr ||
      cinfo.jpeg_color_space == JCS_CMYK ||
      cinfo.jpeg_color_space == JCS_YCCK ||
      cinfo.jpeg_color_space == JCS_EXT_RGBX ||
      cinfo.jpeg_color_space == JCS_EXT_BGRX ||
      cinfo.jpeg_color_space == JCS_EXT_XBGR ||
      cinfo.jpeg_color_space == JCS_EXT_XRGB)
    cinfo.out_color_space = JCS_RGB;

  cinfo.quantize_colors = FALSE;
}

void startReading(unsigned char *inbuffer, size_t insize, jpeg_decompress_struct &cinfo, my_error_mgr &jerr)
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
  //    throw ZIOException(QString("Libjpeg-turbo error:") + QString(errbuffer));
  //  }
  /* Now we can initialize the JPEG decompression object. */
  jpeg_create_decompress(&cinfo);

  jpeg_mem_src(&cinfo, inbuffer, insize);

  jpeg_save_markers(&cinfo, JPEG_APP0+1, 0xFFFF);

  /* Step 3: read file parameters with jpeg_read_header() */

  (void) jpeg_read_header(&cinfo, TRUE);
  /* We can ignore the return value from jpeg_read_header since
     *   (a) suspension is not possible with the stdio data source, and
     *   (b) we passed TRUE to reject a tables-only JPEG file as an error.
     * See libjpeg.txt for more info.
     */

  /* Step 4: set parameters for decompression */

  if (cinfo.jpeg_color_space == JCS_YCbCr ||
      cinfo.jpeg_color_space == JCS_CMYK ||
      cinfo.jpeg_color_space == JCS_YCCK ||
      cinfo.jpeg_color_space == JCS_EXT_RGBX ||
      cinfo.jpeg_color_space == JCS_EXT_BGRX ||
      cinfo.jpeg_color_space == JCS_EXT_XBGR ||
      cinfo.jpeg_color_space == JCS_EXT_XRGB)
    cinfo.out_color_space = JCS_RGB;

  cinfo.quantize_colors = FALSE;
}

uint16_t getOrientation(jpeg_decompress_struct &cinfo)
{
  uint16_t orientation = ORIENTATION_TOPLEFT;
  jpeg_saved_marker_ptr marker = cinfo.marker_list;
  while (marker) {
    if (marker->data_length > 6 && std::equal(marker->data, marker->data+6, "Exif\0\0")) {
      typedef boost::iostreams::basic_array_source<char> Device;
      boost::iostreams::stream<Device> exifs(reinterpret_cast<char*>(marker->data)+6, marker->data_length-6);
      ZTiff exif;
      exif.load(exifs, true);
      if (exif.isValid()) {
        orientation = exif.ifds()[0].orientation();
      }
      break;
    }

    marker = marker->next;
    continue;
  }
  return orientation;
}

void readInfoFromJpeg(jpeg_decompress_struct &cinfo, ZImgInfo &info)
{
  uint16_t orientation = getOrientation(cinfo);

  jpeg_calc_output_dimensions(&cinfo);

  info.bytesPerVoxel = 1;
  info.width = cinfo.output_width;
  info.height = cinfo.output_height;
  info.depth = 1;
  info.numChannels = cinfo.output_components;
  info.numTimes = 1;
  info.voxelFormat = VoxelFormat::Unsigned;
  info.createDefaultDescriptions();

  if (orientation > 4)
    std::swap(info.width, info.height);
}

void finishReading(jpeg_decompress_struct &cinfo)
{
  /* Step 8: Release JPEG decompression object */

  /* This is an important step since it will release a good deal of memory. */
  jpeg_destroy_decompress(&cinfo);
}

void readImgFromJpeg(jpeg_decompress_struct &cinfo, ZImg &img, const ZImgRegion &region, uint16_t orientation)
{
  /* Step 5: Start decompressor */
  (void) jpeg_start_decompress(&cinfo);

  ZImgInfo imgInfo;
  imgInfo.bytesPerVoxel = 1;
  imgInfo.width = cinfo.output_width;
  imgInfo.height = cinfo.output_height;
  imgInfo.depth = 1;
  imgInfo.numChannels = cinfo.output_components;
  imgInfo.numTimes = 1;
  imgInfo.voxelFormat = VoxelFormat::Unsigned;
  imgInfo.createDefaultDescriptions();

  if (region.isEmpty() || !region.isValid(imgInfo)) {
    finishReading(cinfo);
    throw ZIOException(QString("Invalid image region. Image info: '%1', region: '%2'").arg(imgInfo.toQString()).arg(region.toQString()));
  }

  ZImgInfo partialImgInfo = region.clip(imgInfo);
  ZImg imgTmp(partialImgInfo);


  /* We may need to do some setup of our own at this point before reading
     * the data.  After jpeg_start_decompress() we have the correct scaled
     * output image dimensions available, as well as the output colormap
     * if we asked for color quantization.
     * In this example, we need to make an output work buffer of the right size.
     */
  /* JSAMPLEs per row in output buffer */
  /* Make a one-row-high sample array that will go away when done with image */
  JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)
      ((j_common_ptr) &cinfo, JPOOL_IMAGE, imgInfo.rowByteNumber()*imgInfo.numChannels, cinfo.rec_outbuf_height);

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
    size_t lineRead = jpeg_read_scanlines(&cinfo, buffer, cinfo.rec_outbuf_height);

    for (size_t y=lineStart; y<lineStart+lineRead; ++y) {
      if (region.yInRegion(y)) {
        if (imgInfo.numChannels == 1) {
          memcpy(imgTmp.rowData<uint8_t>(y-region.start.y), &(buffer[y-lineStart][region.start.x]), imgTmp.rowByteNumber());
        } else {
          size_t cEnd = region.end.c == -1 ? imgInfo.numChannels : region.end.c;
          size_t xEnd = region.end.x == -1 ? imgInfo.width : region.end.x;
          for (size_t c=region.start.c; c<cEnd; ++c) {
            for (size_t x=region.start.x; x<xEnd; ++x) {
              *imgTmp.data<uint8_t>(x-region.start.x,y-region.start.y,0,c-region.start.c) = buffer[y-lineStart][x*imgInfo.numChannels + c];
            }
          }
        }
      }
    }

    lineStart += lineRead;
  }

  /* Step 7: Finish decompression */
  (void) jpeg_finish_decompress(&cinfo);

  if (orientation > 4) {
    std::swap(imgTmp.infoRef().width, imgTmp.infoRef().height);
  }

  switch (orientation) {
  case ORIENTATION_TOPLEFT:
    break;
  case ORIENTATION_TOPRIGHT:
    for (size_t i=0; i<imgTmp.numChannels(); ++i) {
      image2DFlip(imgTmp.channelData<uint8_t>(i), imgTmp.width(), imgTmp.height(), Dimension::X);
    }
    break;
  case ORIENTATION_BOTRIGHT:
    for (size_t i=0; i<imgTmp.numChannels(); ++i) {
      image2DReflect(imgTmp.channelData<uint8_t>(i), imgTmp.width(), imgTmp.height());
    }
    break;
  case ORIENTATION_BOTLEFT:
    for (size_t i=0; i<imgTmp.numChannels(); ++i) {
      image2DFlip(imgTmp.channelData<uint8_t>(i), imgTmp.width(), imgTmp.height(), Dimension::Y);
    }
    break;
  case ORIENTATION_LEFTTOP:
    for (size_t i=0; i<imgTmp.numChannels(); ++i) {
      image2DTranspose(imgTmp.channelData<uint8_t>(i), imgTmp.height(), imgTmp.width());
    }
    break;
  case ORIENTATION_RIGHTTOP:
    for (size_t i=0; i<imgTmp.numChannels(); ++i) {
      image2DTranspose(imgTmp.channelData<uint8_t>(i), imgTmp.height(), imgTmp.width());
      image2DFlip(imgTmp.channelData<uint8_t>(i), imgTmp.width(), imgTmp.height(), Dimension::X);
    }
    break;
  case ORIENTATION_RIGHTBOT:
    for (size_t i=0; i<imgTmp.numChannels(); ++i) {
      image2DTranspose(imgTmp.channelData<uint8_t>(i), imgTmp.height(), imgTmp.width());
      image2DReflect(imgTmp.channelData<uint8_t>(i), imgTmp.width(), imgTmp.height());
    }
    break;
  case ORIENTATION_LEFTBOT:
    for (size_t i=0; i<imgTmp.numChannels(); ++i) {
      image2DTranspose(imgTmp.channelData<uint8_t>(i), imgTmp.height(), imgTmp.width());
      image2DFlip(imgTmp.channelData<uint8_t>(i), imgTmp.width(), imgTmp.height(), Dimension::Y);
    }
    break;
  default:
    break;
  }

  imgTmp.swap(img);
}

}

namespace nim {

ZImgJpeg &ZImgJpeg::instance()
{
  static ZImgJpeg imgJpeg;
  return imgJpeg;
}

ZImgJpeg::ZImgJpeg()
{
}

ZImgJpeg::~ZImgJpeg()
{
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
  res << "jpg" << "jpeg";
  return res;
}

void ZImgJpeg::readInfo(const QString &filename, std::vector<ZImgInfo> &infos, std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> *subBlocks,
                        std::vector<std::set<size_t>> *pyramidalRatios)
{
  std::unique_ptr<FILE, int (*)(FILE*)> infile(fopen(QFile::encodeName(filename).constData(), "rb"), fclose);
  if (!infile) {
    throw ZIOException("can't open file");
  }

  struct jpeg_decompress_struct cinfo;
  my_error_mgr jerr;

  startReading(infile.get(), cinfo, jerr);

  infos.resize(1);
  readInfoFromJpeg(cinfo, infos[0]);

  finishReading(cinfo);

  createDefaultSubBlocks(filename, infos, subBlocks, pyramidalRatios);
}

void ZImgJpeg::readMetadata(const QString &filename, ZImgMetadata &meta, size_t scene)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
  std::unique_ptr<FILE, int (*)(FILE*)> infile(fopen(QFile::encodeName(filename).constData(), "rb"), fclose);
  if (!infile) {
    throw ZIOException("can't open file");
  }

  struct jpeg_decompress_struct cinfo;
  my_error_mgr jerr;

  startReading(infile.get(), cinfo, jerr);

  // extract exif
  jpeg_saved_marker_ptr marker = cinfo.marker_list;
  while (marker) {
    if (marker->data_length > 6 && std::equal(marker->data, marker->data+6, "Exif\0\0")) {
      typedef boost::iostreams::basic_array_source<char> Device;
      boost::iostreams::stream<Device> exifs(reinterpret_cast<char*>(marker->data)+6, marker->data_length-6);
      ZTiff exif;
      try {
        exif.load(exifs, true);
        if (exif.isValid()) {
          meta.attachToTopLevel(exif.ifds()[0].extractMetadata());
        }
      }
      catch (const ZIOException & e) {
        LWARN() << "failed to read metadata from jpeg:" << e.what();
      }
      break;
    }

    marker = marker->next;
    continue;
  }

  finishReading(cinfo);
}

void ZImgJpeg::readThumbnail(const QString &filename, ZImgThumbernail &thumbnail, const ZImgRegion &region, size_t scene)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
  std::unique_ptr<FILE, int (*)(FILE*)> infile(fopen(QFile::encodeName(filename).constData(), "rb"), fclose);
  if (!infile) {
    throw ZIOException("can't open file");
  }

  struct jpeg_decompress_struct cinfo;
  my_error_mgr jerr;

  startReading(infile.get(), cinfo, jerr);

  // extract thumbnails
  jpeg_saved_marker_ptr marker = cinfo.marker_list;
  while (marker) {
    if (marker->data_length > 6 && std::equal(marker->data, marker->data+6, "Exif\0\0")) {
      typedef boost::iostreams::basic_array_source<char> Device;
      boost::iostreams::stream<Device> exifs(reinterpret_cast<char*>(marker->data)+6, marker->data_length-6);
      ZTiff exif;
      try {
        exif.load(exifs, true);
        if (exif.isValid()) {
          for (size_t i=1; i<exif.ifds().size(); ++i) {
            const ZTiffIFD &thumbIFD = exif.ifds()[i];
            int64_t idx = thumbIFD.indexOf(TIFFTAG_JPEGIFOFFSET);
            if (idx != -1) {
              // found jpeg thumbnail stream
              std::vector<uint8_t> buf;
              const ZImgMetatag &tag = thumbIFD.tag(idx);
              size_t off = tag.dataAt<uint32_t>(0);
              buf.assign(marker->data + 6 + off, marker->data + marker->data_length);

              struct jpeg_decompress_struct thumbCinfo;
              my_error_mgr thumbJerr;

              // don't slice thumbnail in x, y, c
              ZImgRegion thumbRegion = region;
              thumbRegion.start.x = 0;
              thumbRegion.end.x = -1;
              thumbRegion.start.y = 0;
              thumbRegion.end.y = -1;
              thumbRegion.start.c = 0;
              thumbRegion.end.c = -1;

              ZImg thumbImg;

              startReading(buf.data(), buf.size(), thumbCinfo, thumbJerr);
              readImgFromJpeg(thumbCinfo, thumbImg, thumbRegion, getOrientation(cinfo));
              finishReading(thumbCinfo);

              thumbnail.attachToPlane(thumbImg, 0, 0);
            }
          }
        }
      }
      catch (const ZIOException & e) {
        LWARN() << "failed to read thumbnail from jpeg:" << e.what();
      }
      break;
    }

    marker = marker->next;
    continue;
  }

  finishReading(cinfo);
}

void ZImgJpeg::readImg(const QString &filename, ZImg &img, const ZImgRegion &region, size_t scene, size_t ratio)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
  std::unique_ptr<FILE, int (*)(FILE*)> infile(fopen(QFile::encodeName(filename).constData(), "rb"), fclose);
  if (!infile) {
    throw ZIOException("can't open file");
  }

  struct jpeg_decompress_struct cinfo;
  my_error_mgr jerr;

  startReading(infile.get(), cinfo, jerr);

  readImgFromJpeg(cinfo, img, region, getOrientation(cinfo));

  finishReading(cinfo);

  readThumbnail(filename, img.thumbnailRef(), region, scene);
  readMetadata(filename, img.metadataRef(), scene);

  if (ratio > 1) {
    img.zoom(1.0 / ratio, 1.0 / ratio);
  }
}

bool ZImgJpeg::supportRead() const
{
  return true;
}

bool ZImgJpeg::supportWrite() const
{
  return false;
}

void ZImgJpeg::readInfo(uint8_t *mem, size_t size, ZImgInfo &info)
{
  struct jpeg_decompress_struct cinfo;
  my_error_mgr jerr;

  startReading(mem, size, cinfo, jerr);

  readInfoFromJpeg(cinfo, info);

  finishReading(cinfo);
}

void ZImgJpeg::readImg(uint8_t *mem, size_t size, uint8_t *des, size_t desSize)
{
  struct jpeg_decompress_struct cinfo;
  my_error_mgr jerr;

  startReading(mem, size, cinfo, jerr);

  ZImg img;
  readImgFromJpeg(cinfo, img, ZImgRegion(), getOrientation(cinfo));

  if (desSize < img.byteNumber()) {
    throw ZIOException("buffer space is not enough");
  }

  memcpy(des, img.channelData<uint8_t>(0), img.byteNumber());

  finishReading(cinfo);
}

} // namespace
