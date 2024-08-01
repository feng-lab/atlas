#include "zimgfreeimage.h"

#include "zlog.h"
#include <FreeImagePlus.h>
#include <QFile>

namespace {

using namespace nim;

/**
FreeImage error handler
@param fif Format / Plugin responsible for the error
@param message Error message
*/
void FreeImageErrorHandler(FREE_IMAGE_FORMAT fif, const char* message)
{
  if (fif != FIF_UNKNOWN) {
    LOG(WARNING) << fmt::format("FreeImage {} Format: {}", FreeImage_GetFormatFromFIF(fif), message);
  } else {
    LOG(WARNING) << fmt::format("FreeImage: {}", message);;
  }
}

ZImgInfo readInfoFromFIPImage(fipImage& fipImg)
{
  ZImgInfo info;
  switch (fipImg.getImageType()) {
    case FIT_BITMAP:
      info.voxelFormat = VoxelFormat::Unsigned;
      info.bytesPerVoxel = 1;
      switch (fipImg.getBitsPerPixel()) {
        case 1:
        case 4:
        case 8:
          if (fipImg.getColorType() == FIC_PALETTE) {
            info.numChannels = 3;
          } else {
            info.numChannels = 1;
          }
          break;
        case 16:
        case 24:
          info.numChannels = 3;
          break;
        case 32:
          if (fipImg.getColorType() == FIC_CMYK) {
            info.numChannels = 3; // to RGB
          } else {
            info.numChannels = 4;
            info.lastChannelIsAlphaChannel = true;
          }
          break;
        default:
          throw ZException(fmt::format("Not supported bitsPerPixel {}", fipImg.getBitsPerPixel()));
      }
      break;
    case FIT_UINT16:
      info.bytesPerVoxel = 2;
      info.numChannels = 1;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case FIT_INT16:
      info.bytesPerVoxel = 2;
      info.numChannels = 1;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case FIT_UINT32:
      info.bytesPerVoxel = 4;
      info.numChannels = 1;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case FIT_INT32:
      info.bytesPerVoxel = 4;
      info.numChannels = 1;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case FIT_FLOAT:
      info.bytesPerVoxel = 4;
      info.numChannels = 1;
      info.voxelFormat = VoxelFormat::Float;
      break;
    case FIT_DOUBLE:
      info.bytesPerVoxel = 8;
      info.numChannels = 1;
      info.voxelFormat = VoxelFormat::Float;
      break;
    case FIT_COMPLEX:
      throw ZException("Complex format image is not supported.");
      break;
    case FIT_RGB16:
      info.bytesPerVoxel = 2;
      info.numChannels = 3;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case FIT_RGBA16:
      info.bytesPerVoxel = 2;
      info.numChannels = 4;
      info.lastChannelIsAlphaChannel = true;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case FIT_RGBF:
      info.bytesPerVoxel = 4;
      info.numChannels = 3;
      info.voxelFormat = VoxelFormat::Float;
      break;
    case FIT_RGBAF:
      info.bytesPerVoxel = 4;
      info.numChannels = 4;
      info.lastChannelIsAlphaChannel = true;
      info.voxelFormat = VoxelFormat::Float;
      break;
    default:
      throw ZException("Not supported format");
  }

  info.width = fipImg.getWidth();
  info.height = fipImg.getHeight();
  info.depth = 1;
  info.numTimes = 1;
  info.createDefaultDescriptions();
  return info;
}

} // namespace

namespace nim {

ZImgFreeImage& ZImgFreeImage::instance()
{
  static ZImgFreeImage imgFreeImage;
  return imgFreeImage;
}

ZImgFreeImage::ZImgFreeImage()
{
  FreeImage_SetOutputMessage(FreeImageErrorHandler);
}

bool ZImgFreeImage::supportRead() const
{
  return true;
}

bool ZImgFreeImage::supportWrite() const
{
  return false;
}

QString ZImgFreeImage::shortName() const
{
  return QString("Image");
}

QString ZImgFreeImage::fullName() const
{
  return QString("Image");
}

QStringList ZImgFreeImage::extensions() const
{
  QStringList res;
  res << "bmp"
      << "cut"
      << "dds"
      << "exr"
      << "g3"
      << "gif"
      << "hdr"
      << "ico"
      << "iff"
      << "lbm"
      << "j2k"
      << "j2c"
      << "jng"
      << "jp2" /*<< "jpg"*/
      << "jif"
      /*<< "jpeg"*/
      << "jpe"
      << "jxr"
      << "wdp"
      << "hdp"
      << "koa"
      << "mng"
      << "pbm"
      << "pcd"
      << "pcx"
      << "pfm"
      << "pgm"
      << "pct"
      << "pict"
      << "pic" /*<< "png"*/
      << "ppm"
      << "psd"
      << "ras"
      << "sgi"
      << "tga"
      << "targa"
      << "wap"
      << "wbmp"
      << "wbm"
      << "webp"
      << "xbm"
      << "xpm"
      << "3fr"
      << "arw"
      << "bay"
      << "bmq"
      << "cap"
      << "cine"
      << "cr2"
      << "crw"
      << "cs1"
      << "dc2"
      << "dcr"
      << "dng"
      << "drf"
      << "dsc"
      << "erf"
      << "fff"
      << "ia"
      << "iiq"
      << "k25"
      << "kc2"
      << "kdc"
      << "mdc"
      << "mef"
      << "mos"
      << "mrw"
      << "nef"
      << "nrw"
      << "orf"
      << "pef"
      << "ptx"
      << "pxn"
      << "qtk"
      << "raf"
      << "raw"
      << "rdc"
      << "rw2"
      << "rwz"
      << "sr2"
      << "srf"
      << "sti"
      << "x3f";
  return res;
}

void ZImgFreeImage::readInfo(const QString& filename,
                             std::vector<ZImgInfo>& infos,
                             std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
#if defined(_WIN32) || defined(_WIN64)
  FREE_IMAGE_FORMAT fmt = fipImage::identifyFIFU(filename.toStdWString().c_str());
#else
  FREE_IMAGE_FORMAT fmt = fipImage::identifyFIF(QFile::encodeName(filename).constData());
#endif
  if (fmt == FIF_UNKNOWN) {
    throw ZException("Can not identify image format", ZException::Option::CheckErrno);
  } /*else {
    VLOG(1) << FreeImage_GetFIFDescription(fmt);
  }*/
  fipMultiPage fipMp(true);
  bool multipage = fmt == FIF_GIF &&
                   fipMp.open(QFile::encodeName(filename).constData(), false, true, FIF_LOAD_NOPIXELS | GIF_PLAYBACK) &&
                   fipMp.getPageCount() > 1;
  if (multipage) {
    fipImage fipImg;
    fipImg = fipMp.lockPage(0);
    infos.push_back(readInfoFromFIPImage(fipImg));
    fipMp.unlockPage(fipImg, false);
    infos[0].numTimes = fipMp.getPageCount();
    fipMp.close();
  } else { // not multipage
    fipImage fipImg;
#if defined(_WIN32) || defined(_WIN64)
    if (!fipImg.loadU(fmt, filename.toStdWString().c_str(), FIF_LOAD_NOPIXELS)) {
#else
    if (!fipImg.load(fmt, QFile::encodeName(filename).constData(), FIF_LOAD_NOPIXELS)) {
#endif
      throw ZException("Can not load header", ZException::Option::CheckErrno);
    }
    infos.push_back(readInfoFromFIPImage(fipImg));
  }

  createDefaultSubBlocks(filename, infos, subBlocks);
}

void ZImgFreeImage::readMetadata(const QString& /*filename*/, ZImgMetadata& /*meta*/, size_t /*scene*/) {}

void ZImgFreeImage::readThumbnail(const QString& /*filename*/,
                                  ZImgThumbernail& /*thumbnail*/,
                                  const ZImgRegion& /*region*/,
                                  size_t /*scene*/)
{}

void ZImgFreeImage::readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene)
{
  std::vector<ZImgInfo> infos;
  readInfo(filename, infos, nullptr);
  if (scene >= infos.size()) {
    throw ZException("invalid scene");
  }
  ZImgInfo& info = infos[0];

  if (region.isEmpty() || !region.isValid(info)) {
    throw ZException(
      fmt::format("Invalid image region. Image info: '{}', region: '{}'", info.toString(), region.toString()));
  }

  img = ZImg(info);

  bool isBGA = false;
  if (info.numTimes == 1) {
    fipImage fipImg;
#if defined(_WIN32) || defined(_WIN64)
    if (!fipImg.loadU(filename.toStdWString().c_str())) {
#else
    if (!fipImg.load(QFile::encodeName(filename).constData())) {
#endif
      throw ZException("Can not load", ZException::Option::CheckErrno);
    }

    isBGA = fipImg.getImageType() == FIT_BITMAP;

    switch (fipImg.getColorType()) {
      case FIC_CMYK:
        VLOG(1) << "cmyk";
        if (!fipImg.convertTo24Bits()) {
          throw ZException("convert CMYK to 24bit error", ZException::Option::CheckErrno);
        }
        break;
      case FIC_PALETTE:
        if (!fipImg.convertTo24Bits()) {
          throw ZException("convert PALETTE image to 24bit error", ZException::Option::CheckErrno);
        }
        break;
      case FIC_MINISBLACK:
        if (!fipImg.convertToGrayscale()) {
          throw ZException("convert MINISBLACK image to Grayscale error", ZException::Option::CheckErrno);
        }
        break;
      case FIC_MINISWHITE:
        if (!fipImg.convertToGrayscale()) {
          throw ZException("convert MINISWHITE image to Grayscale error", ZException::Option::CheckErrno);
        }
        break;
      default:
        break;
    }

    auto imgBuf = img.timeData<uint8_t>(0);
    for (size_t i = 0; i < img.height(); ++i) {
      uint8_t* scanline = fipImg.getScanLine(img.height() - i - 1);
      std::memcpy(imgBuf, scanline, fipImg.getLine());
      imgBuf += fipImg.getLine();
    }
  } else {
    fipMultiPage fipMp(true);
    if (!fipMp.open(QFile::encodeName(filename).constData(), false, true, GIF_PLAYBACK)) {
      throw ZException("Can not open gif", ZException::Option::CheckErrno);
    }

    for (size_t t = 0; t < info.numTimes; ++t) {
      fipImage fipImg;
      fipImg = fipMp.lockPage(t);

      if (t == 0) {
        isBGA = fipImg.getImageType() == FIT_BITMAP;
      }

      switch (fipImg.getColorType()) {
        case FIC_CMYK:
          VLOG(1) << "cmyk";
          if (!fipImg.convertTo24Bits()) {
            throw ZException("convert CMYK to 24bit error", ZException::Option::CheckErrno);
          }
          break;
        case FIC_PALETTE:
          if (!fipImg.convertTo24Bits()) {
            throw ZException("convert PALETTE image to 24bit error", ZException::Option::CheckErrno);
          }
          break;
        case FIC_MINISBLACK:
          if (!fipImg.convertToGrayscale()) {
            throw ZException("convert MINISBLACK image to Grayscale error", ZException::Option::CheckErrno);
          }
          break;
        case FIC_MINISWHITE:
          if (!fipImg.convertToGrayscale()) {
            throw ZException("convert MINISWHITE image to Grayscale error", ZException::Option::CheckErrno);
          }
          break;
        default:
          break;
      }

      auto imgBuf = img.timeData<uint8_t>(t);
      for (size_t i = 0; i < img.height(); ++i) {
        uint8_t* scanline = fipImg.getScanLine(img.height() - i - 1);
        std::memcpy(imgBuf, scanline, fipImg.getLine());
        imgBuf += fipImg.getLine();
      }

      fipMp.unlockPage(fipImg, false);
    }
    fipMp.close();
  }

  if (img.numChannels() > 1) {
    ZImg imgTmp(info);
    CXYZtoXYZC(img, imgTmp, isBGA);

    if (region.containsWholeImg(imgTmp.info())) {
      img.swap(imgTmp);
    } else {
      img = imgTmp.crop(region);
    }
  } else if (!region.containsWholeImg(img.info())) {
    img = img.crop(region);
  }
}

void ZImgFreeImage::readMemInfo(uint8_t* mem, size_t size, ZImgInfo& info)
{
  fipImage fipImg;
  fipMemoryIO memIO(mem, size);
  if (!fipImg.loadFromMemory(memIO, FIF_LOAD_NOPIXELS)) {
    throw ZException("Can not load from memory");
  }
  info = readInfoFromFIPImage(fipImg);
}

void ZImgFreeImage::readMemImg(uint8_t* mem, size_t size, uint8_t* des, size_t desSize)
{
  fipImage fipImg;
  fipMemoryIO memIO(mem, size);
  if (!fipImg.loadFromMemory(memIO)) {
    throw ZException("Can not load from memory");
  }
  ZImgInfo info = readInfoFromFIPImage(fipImg);

  if (desSize < info.byteNumber()) {
    throw ZException("buffer space is not enough");
  }

  switch (fipImg.getColorType()) {
    case FIC_CMYK:
      VLOG(1) << "cmyk";
      if (!fipImg.convertTo24Bits()) {
        throw ZException("convert CMYK to 24bit error");
      }
      break;
    case FIC_PALETTE:
      if (!fipImg.convertTo24Bits()) {
        throw ZException("convert PALETTE image to 24bit error");
      }
      break;
    case FIC_MINISBLACK:
      if (!fipImg.convertToGrayscale()) {
        throw ZException("convert MINISBLACK image to Grayscale error");
      }
      break;
    case FIC_MINISWHITE:
      if (!fipImg.convertToGrayscale()) {
        throw ZException("convert MINISWHITE image to Grayscale error");
      }
      break;
    default:
      break;
  }

  uint8_t* imgBuf = des;
  for (size_t i = 0; i < info.height; ++i) {
    uint8_t* scanline = fipImg.getScanLine(info.height - i - 1);
    std::memcpy(imgBuf, scanline, fipImg.getLine());
    imgBuf += fipImg.getLine();
  }

  if (info.numChannels > 1) {
    ZImg img;
    img.wrapData(des, info);
    ZImg imgTmp = img;
    CXYZtoXYZC(imgTmp, img, fipImg.getImageType() == FIT_BITMAP);
  }
}

} // namespace nim
