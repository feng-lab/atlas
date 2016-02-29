#include "zimgjpegxr.h"

#include "JXRGlue.h"
#include <cmath>

namespace {

using namespace nim;

struct replaceInfAndNanWith0
{
  float operator()(float current) const
  {
    return std::isfinite(current) ? current : 0;
  }
};

void reportError(ERR err)
{
  switch(err) {
  case WMP_errSuccess:
    return;
  case WMP_errFail:
    throw nim::ZIOException("Fail");
  case WMP_errNotYetImplemented:
    throw nim::ZIOException("Not yet implemented");
  case WMP_errAbstractMethod:
    throw nim::ZIOException("Abstract method");
  case WMP_errOutOfMemory:
    throw nim::ZIOException("Out of memory");
  case WMP_errFileIO:
    throw nim::ZIOException("File I/O error");
  case WMP_errBufferOverflow:
    throw nim::ZIOException("Buffer overflow");
  case WMP_errInvalidParameter:
    throw nim::ZIOException("Invalid parameter");
  case WMP_errInvalidArgument:
    throw nim::ZIOException("Invalid argument");
  case WMP_errUnsupportedFormat:
    throw nim::ZIOException("Unsupported format");
  case WMP_errIncorrectCodecVersion:
    throw nim::ZIOException("Incorrect codec version");
  case WMP_errIndexNotFound:
    throw nim::ZIOException("Index not found");
  case WMP_errOutOfSequence:
    throw nim::ZIOException("Out of sequence");
  case WMP_errNotInitialized:
    throw nim::ZIOException("Not Initialized");
  case WMP_errMustBeMultipleOf16LinesUntilLastCall:
    throw nim::ZIOException("Must be multiple of 16 lines until last call");
  case WMP_errPlanarAlphaBandedEncRequiresTempFile:
    throw nim::ZIOException("Planar alpha banded encoder requires temp files");
  case WMP_errAlphaModeCannotBeTranscoded:
    throw nim::ZIOException("Alpha mode cannot be transcoded");
  case WMP_errIncorrectCodecSubVersion:
    throw nim::ZIOException("Incorrect codec subversion");
  default:
    throw nim::ZIOException("Unknown error");
  }
}

void readInfoFromDecoder(const PKImageDecode* pDecoder, const PKPixelInfo &PI, ZImgInfo &info)
{
  info.width = pDecoder->WMP.wmiI.cWidth;
  info.height = pDecoder->WMP.wmiI.cHeight;
  info.depth = 1;
  info.numChannels = PI.cChannel;
  if (PI.cfColorFormat != Y_ONLY &&
      PI.cfColorFormat != NCOMPONENT &&
      PI.cfColorFormat != CF_RGB &&
      PI.cfColorFormat != CF_RGBE) {
    throw ZIOException("Not supported color type");
  }
  if (PI.grBit & PK_pixfmtHasAlpha) {
    info.lastChannelIsAlphaChannel = true;
  }
  info.numTimes = 1;

  switch (PI.bdBitDepth) {
  case BD_8:
    info.bytesPerVoxel = 1;
    break;
  case BD_16:
    info.bytesPerVoxel = 2;
    break;
  case BD_32:
  case BD_32F:
    info.bytesPerVoxel = 4;
    break;
  default:
    throw ZIOException("Not supported bit format");
  }

  if (PI.cbitUnit != info.bytesPerVoxel * info.numChannels * 8) {
    // might caused by empty channel, try increase channel number
    info.numChannels += 1;
    if (PI.cbitUnit != info.bytesPerVoxel * info.numChannels * 8) {
      throw ZIOException("Not supported bit unit");
    }
  }

  switch (PI.bdBitDepth) {
  case BD_16F:
  case BD_16S:
  case BD_32F:
  case BD_32S:
    info.voxelFormat = VoxelFormat::Float;
    break;
  default:
    info.voxelFormat = VoxelFormat::Unsigned;
    break;
  }

  info.createDefaultDescriptions();
}

}

namespace nim {

ZImgJpegXR &ZImgJpegXR::instance()
{
  static ZImgJpegXR imgJpegXR;
  return imgJpegXR;
}

ZImgJpegXR::ZImgJpegXR()
{
}

ZImgJpegXR::~ZImgJpegXR()
{
}

bool ZImgJpegXR::supportRead() const
{
  return true;
}

bool ZImgJpegXR::supportWrite() const
{
  return false;
}

QString ZImgJpegXR::shortName() const
{
  return "Jpeg XR";
}

QString ZImgJpegXR::fullName() const
{
  return "Jpeg XR";
}

QStringList ZImgJpegXR::extensions() const
{
  QStringList res;
  res << "jxr" << "wdp" << "hdp";
  return res;
}

void ZImgJpegXR::readInfo(const QString &filename, std::vector<ZImgInfo> &infos, std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> *subBlocks,
                          std::vector<std::set<size_t>> *pyramidalRatios)
{
  ERR err = WMP_errSuccess;
  PKCodecFactory* pCodecFactory = nullptr;
  PKImageDecode* pDecoder = nullptr;
  PKPixelInfo PI;

  Call(PKCreateCodecFactory(&pCodecFactory, WMP_SDK_VERSION));
  Call(pCodecFactory->CreateDecoderFromFile(qPrintable(filename), &pDecoder));
  PI.pGUIDPixFmt = &pDecoder->guidPixFormat;
  Call(PixelFormatLookup(&PI, LOOKUP_FORWARD));
  //Call(PixelFormatLookup(&PI, LOOKUP_BACKWARD_TIF));

  infos.resize(1);
  readInfoFromDecoder(pDecoder, PI, infos[0]);

Cleanup:
  if (pDecoder)
    pDecoder->Release(&pDecoder);
  if (pCodecFactory)
    pCodecFactory->Release(&pCodecFactory);

  reportError(err);

  createDefaultSubBlocks(filename, infos, subBlocks, pyramidalRatios);
}

void ZImgJpegXR::readMetadata(const QString &filename, ZImgMetadata &meta, size_t scene)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
  Q_UNUSED(filename);
  Q_UNUSED(meta);
}

void ZImgJpegXR::readThumbnail(const QString &filename, ZImgThumbernail &thumbnail, const ZImgRegion &region, size_t scene)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
  Q_UNUSED(filename);
  Q_UNUSED(thumbnail);
  Q_UNUSED(region);
}

void ZImgJpegXR::readImg(const QString &filename, ZImg &img, const ZImgRegion &region, size_t scene, size_t ratio)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
  ERR err = WMP_errSuccess;
  PKCodecFactory* pCodecFactory = nullptr;
  PKImageDecode* pDecoder = nullptr;
  PKPixelInfo PI;
  ZImgInfo resInfo;
  ZImgRegion tmpRegion = region;
  PKRect rect;
  ZImgInfo info;

  Call(PKCreateCodecFactory(&pCodecFactory, WMP_SDK_VERSION));
  Call(pCodecFactory->CreateDecoderFromFile(qPrintable(filename), &pDecoder));
  PI.pGUIDPixFmt = &pDecoder->guidPixFormat;
  Call(PixelFormatLookup(&PI, LOOKUP_FORWARD));
  //Call(PixelFormatLookup(&PI, LOOKUP_BACKWARD_TIF));

  readInfoFromDecoder(pDecoder, PI, info);

  if (region.isEmpty() || !region.isValid(info)) {
    throw ZIOException(QString("Invalid image region. Image info: '%1', region: '%2'").arg(info.toQString()).arg(region.toQString()));
  }

  if (PI.grBit & PK_pixfmtHasAlpha) {
    pDecoder->WMP.wmiSCP.uAlphaMode = 2;
  } else {
    pDecoder->WMP.wmiSCP.uAlphaMode = 0;
  }
  //pDecoder->WMP.wmiSCP.bVerbose = true;

  tmpRegion.start.c = 0;
  tmpRegion.end.c = -1;
  resInfo = tmpRegion.clip(info);

  rect = {tmpRegion.start.x, tmpRegion.start.y, 0, 0};
  rect.Width = tmpRegion.end.x < 0 ? (info.width - tmpRegion.start.x) : (region.end.x - tmpRegion.start.x);
  rect.Height = tmpRegion.end.y < 0 ? (info.height - tmpRegion.start.y) : (region.end.y - tmpRegion.start.y);

  pDecoder->WMP.wmiI.cROILeftX = rect.X;
  pDecoder->WMP.wmiI.cROITopY = rect.Y;
  pDecoder->WMP.wmiI.cROIWidth = rect.Width;
  pDecoder->WMP.wmiI.cROIHeight = rect.Height;

  img = ZImg(resInfo);

  Call(pDecoder->Copy(pDecoder, &rect, img.channelData<uint8_t>(0), img.rowByteNumber() * img.numChannels()));

  if (img.numChannels() > 1) {
    ZImg imgTmp(resInfo);
    CXYZtoXYZC(img, imgTmp, PI.grBit & PK_pixfmtBGR);
    img.swap(imgTmp);

    if (PI.grBit & PK_pixfmtPreMul && info.lastChannelIsAlphaChannel) {
      if (region.start.c < static_cast<int>(img.numChannels()-1)) { // otherwise don't need to process color channels
        img.correctPreMultipliedColor();
      }
    }
  }

  if (region.start.c != 0 || (region.end.c > 0 && region.end.c < static_cast<int>(img.numChannels()))) {
    tmpRegion = ZImgRegion();
    tmpRegion.start.c = region.start.c;
    tmpRegion.end.c = region.end.c;
    img = img.crop(tmpRegion);
  }

  if (ratio > 1) {
    img.zoom(1.0 / ratio, 1.0 / ratio);
  }

Cleanup:
  if (pDecoder)
    pDecoder->Release(&pDecoder);
  if (pCodecFactory)
    pCodecFactory->Release(&pCodecFactory);

  reportError(err);
}

void ZImgJpegXR::readInfo(uint8_t *mem, size_t size, ZImgInfo &info)
{
  ERR err = WMP_errSuccess;
  PKFactory* pFactory = nullptr;
  PKImageDecode* pDecoder = nullptr;
  PKPixelInfo PI;

  const PKIID* pIID = &IID_PKImageWmpDecode;

  struct WMPStream* pStream = nullptr;

  Call(PKCreateFactory(&pFactory, PK_SDK_VERSION));
  Call(pFactory->CreateStreamFromMemory(&pStream, mem, size));

  // Create decoder
  Call(PKCodecFactory_CreateCodec(pIID, (void **)&pDecoder));

  // attach stream to decoder
  Call(pDecoder->Initialize(pDecoder, pStream));
  pDecoder->fStreamOwner = true;

  PI.pGUIDPixFmt = &pDecoder->guidPixFormat;
  Call(PixelFormatLookup(&PI, LOOKUP_FORWARD));
  //Call(PixelFormatLookup(&PI, LOOKUP_BACKWARD_TIF));

  readInfoFromDecoder(pDecoder, PI, info);

Cleanup:
  if (pDecoder)
    pDecoder->Release(&pDecoder);
  if (pFactory)
    pFactory->Release(&pFactory);

  reportError(err);
}

void ZImgJpegXR::readImg(uint8_t *mem, size_t size, uint8_t *des, size_t desSize)
{
  ERR err = WMP_errSuccess;
  PKFactory* pFactory = nullptr;
  PKImageDecode* pDecoder = nullptr;
  PKPixelInfo PI;
  PKRect rect;
  ZImgInfo info;

  const PKIID* pIID = &IID_PKImageWmpDecode;

  struct WMPStream* pStream = nullptr;

  Call(PKCreateFactory(&pFactory, PK_SDK_VERSION));
  Call(pFactory->CreateStreamFromMemory(&pStream, mem, size));

  // Create decoder
  Call(PKCodecFactory_CreateCodec(pIID, (void **)&pDecoder));

  // attach stream to decoder
  Call(pDecoder->Initialize(pDecoder, pStream));
  pDecoder->fStreamOwner = true;

  PI.pGUIDPixFmt = &pDecoder->guidPixFormat;
  Call(PixelFormatLookup(&PI, LOOKUP_FORWARD));
  //Call(PixelFormatLookup(&PI, LOOKUP_BACKWARD_TIF));

  readInfoFromDecoder(pDecoder, PI, info);

  if (desSize < info.byteNumber()) {
    throw ZIOException("buffer space is not enough");
  }

  if (PI.grBit & PK_pixfmtHasAlpha) {
    pDecoder->WMP.wmiSCP.uAlphaMode = 2;
  } else {
    pDecoder->WMP.wmiSCP.uAlphaMode = 0;
  }
  //pDecoder->WMP.wmiSCP.bVerbose = true;

  rect = {0, 0, 0, 0};
  rect.Width = info.width;
  rect.Height = info.height;

  pDecoder->WMP.wmiI.cROILeftX = rect.X;
  pDecoder->WMP.wmiI.cROITopY = rect.Y;
  pDecoder->WMP.wmiI.cROIWidth = rect.Width;
  pDecoder->WMP.wmiI.cROIHeight = rect.Height;

  Call(pDecoder->Copy(pDecoder, &rect, des, info.rowByteNumber() * info.numChannels));

  if (info.numChannels > 1) {
    ZImg img;
    img.wrapData(des, info);

    ZImg imgTmp = img;
    CXYZtoXYZC(imgTmp, img, PI.grBit & PK_pixfmtBGR);

    if (PI.grBit & PK_pixfmtPreMul && info.lastChannelIsAlphaChannel) {
      img.correctPreMultipliedColor();
    }
  }

Cleanup:
  if (pDecoder)
    pDecoder->Release(&pDecoder);
  if (pFactory)
    pFactory->Release(&pFactory);

  reportError(err);
}

} // namespace
