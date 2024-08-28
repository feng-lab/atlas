#include "zimgjpegxr.h"

#include <folly/ScopeGuard.h>
#include <QFile>
#include <cmath>
#include <JXRGlue.h>

#ifdef Call
#undef Call
#endif
#define Call(exp)                  \
  if (auto err = (exp); err < 0) { \
    reportError(err);              \
  }

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
  switch (err) {
    case WMP_errSuccess:
      return;
    case WMP_errFail:
      throw ZException("JXR Fail", ZException::Option::CheckErrno);
    case WMP_errNotYetImplemented:
      throw ZException("Not yet implemented");
    case WMP_errAbstractMethod:
      throw ZException("Abstract method");
    case WMP_errOutOfMemory:
      throw ZException("Out of memory");
    case WMP_errFileIO:
      throw ZException("File I/O error", ZException::Option::CheckErrno);
    case WMP_errBufferOverflow:
      throw ZException("Buffer overflow");
    case WMP_errInvalidParameter:
      throw ZException("Invalid parameter");
    case WMP_errInvalidArgument:
      throw ZException("Invalid argument");
    case WMP_errUnsupportedFormat:
      throw ZException("Unsupported format");
    case WMP_errIncorrectCodecVersion:
      throw ZException("Incorrect codec version");
    case WMP_errIndexNotFound:
      throw ZException("Index not found");
    case WMP_errOutOfSequence:
      throw ZException("Out of sequence");
    case WMP_errNotInitialized:
      throw ZException("Not Initialized");
    case WMP_errMustBeMultipleOf16LinesUntilLastCall:
      throw ZException("Must be multiple of 16 lines until last call");
    case WMP_errPlanarAlphaBandedEncRequiresTempFile:
      throw ZException("Planar alpha banded encoder requires temp files", ZException::Option::CheckErrno);
    case WMP_errAlphaModeCannotBeTranscoded:
      throw ZException("Alpha mode cannot be transcoded");
    case WMP_errIncorrectCodecSubVersion:
      throw ZException("Incorrect codec subversion");
    default:
      throw ZException("Unknown JXR error", ZException::Option::CheckErrno);
  }
}

void readInfoFromDecoder(const PKImageDecode* pDecoder, const PKPixelInfo& PI, ZImgInfo& info)
{
  info.width = pDecoder->WMP.wmiI.cWidth;
  info.height = pDecoder->WMP.wmiI.cHeight;
  info.depth = 1;
  info.numChannels = PI.cChannel;
  if (PI.cfColorFormat != Y_ONLY && PI.cfColorFormat != NCOMPONENT && PI.cfColorFormat != CF_RGB &&
      PI.cfColorFormat != CF_RGBE) {
    throw ZException("Not supported color type");
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
      throw ZException("Not supported bit format");
  }

  if (PI.cbitUnit != info.bytesPerVoxel * info.numChannels * 8) {
    // might be caused by empty channel, try increase channel number
    info.numChannels += 1;
    if (PI.cbitUnit != info.bytesPerVoxel * info.numChannels * 8) {
      throw ZException("Not supported bit unit");
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

// Y, U, V, YHP, UHP, VHP
// optimized for PSNR
int DPK_QPS_420[11][6] = {
  // for 8 bit only
  {66, 65, 70, 72, 72, 77},
  {59, 58, 63, 64, 63, 68},
  {52, 51, 57, 56, 56, 61},
  {48, 48, 54, 51, 50, 55},
  {43, 44, 48, 46, 46, 49},
  {37, 37, 42, 38, 38, 43},
  {26, 28, 31, 27, 28, 31},
  {16, 17, 22, 16, 17, 21},
  {10, 11, 13, 10, 10, 13},
  {5,  5,  6,  5,  5,  6 },
  {2,  2,  3,  2,  2,  2 }
};

int DPK_QPS_8[12][6] = {
  {67, 79, 86, 72, 90, 98},
  {59, 74, 80, 64, 83, 89},
  {53, 68, 75, 57, 76, 83},
  {49, 64, 71, 53, 70, 77},
  {45, 60, 67, 48, 67, 74},
  {40, 56, 62, 42, 59, 66},
  {33, 49, 55, 35, 51, 58},
  {27, 44, 49, 28, 45, 50},
  {20, 36, 42, 20, 38, 44},
  {13, 27, 34, 13, 28, 34},
  {7,  17, 21, 8,  17, 21}, // Photoshop 100%
  {2,  5,  6,  2,  5,  6 }
};

int DPK_QPS_16[11][6] = {
  {197, 203, 210, 202, 207, 213},
  {174, 188, 193, 180, 189, 196},
  {152, 167, 173, 156, 169, 174},
  {135, 152, 157, 137, 153, 158},
  {119, 137, 141, 119, 138, 142},
  {102, 120, 125, 100, 120, 124},
  {82,  98,  104, 79,  98,  103},
  {60,  76,  81,  58,  76,  81 },
  {39,  52,  58,  36,  52,  58 },
  {16,  27,  33,  14,  27,  33 },
  {5,   8,   9,   4,   7,   8  }
};

} // namespace

namespace nim {

ZImgJpegXR& ZImgJpegXR::instance()
{
  static ZImgJpegXR imgJpegXR;
  return imgJpegXR;
}

bool ZImgJpegXR::supportRead() const
{
  return true;
}

bool ZImgJpegXR::supportWrite() const
{
  return true;
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
  res << "jxr"
      << "wdp"
      << "hdp";
  return res;
}

void ZImgJpegXR::readInfo(const QString& filename,
                          std::vector<ZImgInfo>& infos,
                          std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  PKCodecFactory* pCodecFactory = nullptr;
  auto pCodecFactoryGuard = folly::makeGuard([&pCodecFactory]() {
    if (pCodecFactory) {
      pCodecFactory->Release(&pCodecFactory);
    }
  });
  PKImageDecode* pDecoder = nullptr;
  auto pDecoderGuard = folly::makeGuard([&pDecoder]() {
    if (pDecoder) {
      pDecoder->Release(&pDecoder);
    }
  });
  PKPixelInfo PI;

  Call(PKCreateCodecFactory(&pCodecFactory, WMP_SDK_VERSION));
  Call(pCodecFactory->CreateDecoderFromFile(QFile::encodeName(filename).constData(), &pDecoder));
  PI.pGUIDPixFmt = &pDecoder->guidPixFormat;
  Call(PixelFormatLookup(&PI, LOOKUP_FORWARD));
  // Call(PixelFormatLookup(&PI, LOOKUP_BACKWARD_TIF));

  infos.resize(1);
  readInfoFromDecoder(pDecoder, PI, infos[0]);

  createDefaultSubBlocks(filename, infos, subBlocks);
}

void ZImgJpegXR::readMetadata(const QString& /*filename*/, ZImgMetadata& /*meta*/, size_t scene)
{
  if (scene != 0) {
    throw ZException("invalid scene");
  }
}

void ZImgJpegXR::readThumbnail(const QString& /*filename*/,
                               ZImgThumbernail& /*thumbnail*/,
                               const ZImgRegion& /*region*/,
                               size_t scene)
{
  if (scene != 0) {
    throw ZException("invalid scene");
  }
}

void ZImgJpegXR::readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene)
{
  if (scene != 0) {
    throw ZException("invalid scene");
  }

  PKCodecFactory* pCodecFactory = nullptr;
  auto pCodecFactoryGuard = folly::makeGuard([&pCodecFactory]() {
    if (pCodecFactory) {
      pCodecFactory->Release(&pCodecFactory);
    }
  });
  PKImageDecode* pDecoder = nullptr;
  auto pDecoderGuard = folly::makeGuard([&pDecoder]() {
    if (pDecoder) {
      pDecoder->Release(&pDecoder);
    }
  });
  PKPixelInfo PI;
  ZImgInfo resInfo;
  ZImgRegion tmpRegion = region;
  PKRect rect;
  ZImgInfo info;

  Call(PKCreateCodecFactory(&pCodecFactory, WMP_SDK_VERSION));
  Call(pCodecFactory->CreateDecoderFromFile(QFile::encodeName(filename).constData(), &pDecoder));
  PI.pGUIDPixFmt = &pDecoder->guidPixFormat;
  Call(PixelFormatLookup(&PI, LOOKUP_FORWARD));
  // Call(PixelFormatLookup(&PI, LOOKUP_BACKWARD_TIF));

  readInfoFromDecoder(pDecoder, PI, info);

  if (region.isEmpty() || !region.isValid(info)) {
    throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", info, region));
  }

  if (PI.grBit & PK_pixfmtHasAlpha) {
    pDecoder->WMP.wmiSCP.uAlphaMode = 2;
  } else {
    pDecoder->WMP.wmiSCP.uAlphaMode = 0;
  }
  // pDecoder->WMP.wmiSCP.bVerbose = true;

  tmpRegion.start.c = 0;
  tmpRegion.end.c = -1;
  resInfo = tmpRegion.clip(info);

  rect = {int32_t(tmpRegion.start.x), int32_t(tmpRegion.start.y), 0, 0};
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
      if (region.start.c < static_cast<int>(img.numChannels() - 1)) { // otherwise don't need to process color channels
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
}

void ZImgJpegXR::readMemInfo(void* mem, size_t size, ZImgInfo& info)
{
  PKFactory* pFactory = nullptr;
  auto pFactoryGuard = folly::makeGuard([&pFactory]() {
    if (pFactory) {
      pFactory->Release(&pFactory);
    }
  });
  PKImageDecode* pDecoder = nullptr;
  auto pDecoderGuard = folly::makeGuard([&pDecoder]() {
    if (pDecoder) {
      pDecoder->Release(&pDecoder);
    }
  });
  PKPixelInfo PI;

  const PKIID* pIID = &IID_PKImageWmpDecode;

  struct WMPStream* pStream = nullptr;

  Call(PKCreateFactory(&pFactory, PK_SDK_VERSION));
  Call(pFactory->CreateStreamFromMemory(&pStream, mem, size));

  // Create decoder
  Call(PKCodecFactory_CreateCodec(pIID, (void**)&pDecoder));

  // attach stream to decoder
  Call(pDecoder->Initialize(pDecoder, pStream));
  pDecoder->fStreamOwner = true;

  PI.pGUIDPixFmt = &pDecoder->guidPixFormat;
  Call(PixelFormatLookup(&PI, LOOKUP_FORWARD));
  // Call(PixelFormatLookup(&PI, LOOKUP_BACKWARD_TIF));

  readInfoFromDecoder(pDecoder, PI, info);
}

void ZImgJpegXR::readMemImg(void* mem, size_t size, void* des, size_t desSize)
{
  PKFactory* pFactory = nullptr;
  auto pFactoryGuard = folly::makeGuard([&pFactory]() {
    if (pFactory) {
      pFactory->Release(&pFactory);
    }
  });
  PKImageDecode* pDecoder = nullptr;
  auto pDecoderGuard = folly::makeGuard([&pDecoder]() {
    if (pDecoder) {
      pDecoder->Release(&pDecoder);
    }
  });
  PKPixelInfo PI;
  PKRect rect;
  ZImgInfo info;

  const PKIID* pIID = &IID_PKImageWmpDecode;

  struct WMPStream* pStream = nullptr;

  Call(PKCreateFactory(&pFactory, PK_SDK_VERSION));
  Call(pFactory->CreateStreamFromMemory(&pStream, mem, size));

  // Create decoder
  Call(PKCodecFactory_CreateCodec(pIID, (void**)&pDecoder));

  // attach stream to decoder
  Call(pDecoder->Initialize(pDecoder, pStream));
  pDecoder->fStreamOwner = true;

  PI.pGUIDPixFmt = &pDecoder->guidPixFormat;
  Call(PixelFormatLookup(&PI, LOOKUP_FORWARD));
  // Call(PixelFormatLookup(&PI, LOOKUP_BACKWARD_TIF));

  readInfoFromDecoder(pDecoder, PI, info);
  // VLOG(1) << info;

  if (desSize < info.byteNumber()) {
    throw ZException("buffer space is not enough");
  }

  if (PI.grBit & PK_pixfmtHasAlpha) {
    pDecoder->WMP.wmiSCP.uAlphaMode = 2;
  } else {
    pDecoder->WMP.wmiSCP.uAlphaMode = 0;
  }
  // pDecoder->WMP.wmiSCP.bVerbose = true;

  rect = {0, 0, 0, 0};
  rect.Width = info.width;
  rect.Height = info.height;

  pDecoder->WMP.wmiI.cROILeftX = rect.X;
  pDecoder->WMP.wmiI.cROITopY = rect.Y;
  pDecoder->WMP.wmiI.cROIWidth = rect.Width;
  pDecoder->WMP.wmiI.cROIHeight = rect.Height;

  Call(pDecoder->Copy(pDecoder, &rect, (U8*)des, info.rowByteNumber() * info.numChannels));

  if (info.numChannels > 1) {
    ZImg img;
    img.wrapData(des, info);

    ZImg imgTmp = img;
    CXYZtoXYZC(imgTmp, img, PI.grBit & PK_pixfmtBGR);

    if (PI.grBit & PK_pixfmtPreMul && info.lastChannelIsAlphaChannel) {
      img.correctPreMultipliedColor();
    }
  }
}

void ZImgJpegXR::checkImgBeforeWriting(const QString& filename, const ZImgInfo& info, const ZImgWriteParameters& paras)
{
  ZImgFormat::checkImgBeforeWriting(filename, info, paras);
  checkBeforeWriting(info, paras);
}

void ZImgJpegXR::writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, img.info(), paras);

  auto jpegXRQuality = static_cast<float>(paras.jpegXRQuality);

  CWMIStrCodecParam wmiSCP;
  memset(&wmiSCP, 0, sizeof(wmiSCP));

  wmiSCP.bVerbose = FALSE;
  if (img.numChannels() == 1) {
    wmiSCP.cfColorFormat = Y_ONLY;
  } else if (jpegXRQuality >= 0.5f || img.info().bytesPerVoxel > 1) {
    wmiSCP.cfColorFormat = YUV_444;
  } else {
    wmiSCP.cfColorFormat = YUV_420;
  }
  wmiSCP.bdBitDepth = BD_LONG;
  wmiSCP.bfBitstreamFormat = FREQUENCY;
  wmiSCP.bProgressiveMode = TRUE;
  wmiSCP.olOverlap = OL_ONE;
  wmiSCP.cNumOfSliceMinus1H = wmiSCP.cNumOfSliceMinus1V = 0;
  wmiSCP.sbSubband = SB_ALL;
  if (img.info().lastChannelIsAlphaChannel) {
    wmiSCP.uAlphaMode = 2;
  } else {
    wmiSCP.uAlphaMode = 0;
  }
  wmiSCP.uiDefaultQPIndex = 1;
  wmiSCP.uiDefaultQPIndexAlpha = 1;

  PKFactory* pFactory = nullptr;
  auto pFactoryGuard = folly::makeGuard([&pFactory]() {
    if (pFactory) {
      pFactory->Release(&pFactory);
    }
  });
  struct WMPStream* pEncodeStream = nullptr;
  PKCodecFactory* pCodecFactory = nullptr;
  auto pCodecFactoryGuard = folly::makeGuard([&pCodecFactory]() {
    if (pCodecFactory) {
      pCodecFactory->Release(&pCodecFactory);
    }
  });
  PKImageEncode* pEncoder = nullptr;
  auto pEncoderGuard = folly::makeGuard([&pEncoder]() {
    if (pEncoder) {
      pEncoder->Release(&pEncoder);
    }
  });

  Call(PKCreateFactory(&pFactory, PK_SDK_VERSION));
  Call(pFactory->CreateStreamFromFilename(&pEncodeStream, QFile::encodeName(filename).constData(), "wb"));
  Call(PKCreateCodecFactory(&pCodecFactory, WMP_SDK_VERSION));
  Call(pCodecFactory->CreateCodec(&IID_PKImageWmpEncode, (void**)&pEncoder));

  Call(pEncoder->Initialize(pEncoder, pEncodeStream, &wmiSCP, sizeof(wmiSCP)));

  // ImageQuality  Q (BD==1)  Q (BD==8)   Q (BD==16)  Q (BD==32F) Subsample   Overlap
  //[0.0, 0.5)    8-IQ*5     (see table) (see table) (see table) 4:2:0       2
  //[0.5, 1.0)    8-IQ*5     (see table) (see table) (see table) 4:4:4       1
  //[1.0, 1.0]    1          1           1           1           4:4:4       0
  if (jpegXRQuality < 1.0F) {
    // remap [0.8, 0.866, 0.933, 1.0] to [0.8, 0.9, 1.0, 1.1]
    // to use 8-bit DPK QP table (0.933 == Photoshop JPEG 100)
    if (jpegXRQuality > 0.8f && img.info().bytesPerVoxel == 1 && pEncoder->WMP.wmiSCP.cfColorFormat != YUV_420 &&
        pEncoder->WMP.wmiSCP.cfColorFormat != YUV_422) {
      jpegXRQuality = 0.8f + (jpegXRQuality - 0.8f) * 1.5f;
    }

    int qi = (int)(10.f * jpegXRQuality);
    float qf = 10.f * jpegXRQuality - (float)qi;

    int* pQPs = (pEncoder->WMP.wmiSCP.cfColorFormat == YUV_420 || pEncoder->WMP.wmiSCP.cfColorFormat == YUV_422)
                  ? DPK_QPS_420[qi]
                  : (img.info().bytesPerVoxel == 1 ? DPK_QPS_8[qi] : DPK_QPS_16[qi]);

    pEncoder->WMP.wmiSCP.uiDefaultQPIndex = (U8)(0.5f + (float)pQPs[0] * (1.f - qf) + (float)(pQPs + 6)[0] * qf);
    pEncoder->WMP.wmiSCP.uiDefaultQPIndexU = (U8)(0.5f + (float)pQPs[1] * (1.f - qf) + (float)(pQPs + 6)[1] * qf);
    pEncoder->WMP.wmiSCP.uiDefaultQPIndexV = (U8)(0.5f + (float)pQPs[2] * (1.f - qf) + (float)(pQPs + 6)[2] * qf);
    pEncoder->WMP.wmiSCP.uiDefaultQPIndexYHP = (U8)(0.5f + (float)pQPs[3] * (1.f - qf) + (float)(pQPs + 6)[3] * qf);
    pEncoder->WMP.wmiSCP.uiDefaultQPIndexUHP = (U8)(0.5f + (float)pQPs[4] * (1.f - qf) + (float)(pQPs + 6)[4] * qf);
    pEncoder->WMP.wmiSCP.uiDefaultQPIndexVHP = (U8)(0.5f + (float)pQPs[5] * (1.f - qf) + (float)(pQPs + 6)[5] * qf);
  } else {
    pEncoder->WMP.wmiSCP.uiDefaultQPIndex = (U8)jpegXRQuality;
  }
  if (pEncoder->WMP.wmiSCP.uAlphaMode == 2) {
    pEncoder->WMP.wmiSCP_Alpha.uiDefaultQPIndex = wmiSCP.uiDefaultQPIndexAlpha;
  }

  PKPixelFormatGUID guidPixFormat = GUID_PKPixelFormat8bppGray;
  if (img.info().numChannels == 1 && img.info().bytesPerVoxel == 2) {
    guidPixFormat = GUID_PKPixelFormat16bppGray;
  } else if (img.info().numChannels == 3 && img.info().bytesPerVoxel == 1) {
    guidPixFormat = GUID_PKPixelFormat24bppRGB;
  } else if (img.info().numChannels == 4 && img.info().bytesPerVoxel == 1) {
    guidPixFormat = GUID_PKPixelFormat32bppRGBA;
  } else if (img.info().numChannels == 3 && img.info().bytesPerVoxel == 2) {
    guidPixFormat = GUID_PKPixelFormat48bppRGB;
  } else if (img.info().numChannels == 4 && img.info().bytesPerVoxel == 2) {
    guidPixFormat = GUID_PKPixelFormat64bppRGBA;
  }

  Call(pEncoder->SetPixelFormat(pEncoder, guidPixFormat));
  Call(pEncoder->SetSize(pEncoder, img.width(), img.height()));
  if (img.info().numChannels == 1) {
    Call(pEncoder->WritePixels(pEncoder, img.height(), const_cast<U8*>(img.channelData(0)), img.rowByteNumber()));
  } else {
    ZImg imgTmp(img.info());
    XYZCtoCXYZ(img, imgTmp);
    Call(pEncoder->WritePixels(pEncoder,
                               img.height(),
                               const_cast<U8*>(imgTmp.channelData(0)),
                               imgTmp.rowByteNumber() * imgTmp.numChannels()));
    //      VLOG(1) << pEncoder->WMP.nOffImage;
    //      VLOG(1) << pEncoder->WMP.nCbImage;
    //      VLOG(1) << pEncoder->WMP.nOffAlpha;
    //      VLOG(1) << pEncoder->WMP.nCbAlpha;
  }
}

size_t ZImgJpegXR::writeImgToMem(const ZImg& img, const ZImgWriteParameters& paras, void* mem, size_t size)
{
  checkBeforeWriting(img.info(), paras);
  if (size < img.byteNumber()) {
    throw ZException("target buffer space is not enough");
  }
  size_t byteWritten = 0;

  auto jpegXRQuality = static_cast<float>(paras.jpegXRQuality);

  CWMIStrCodecParam wmiSCP;
  memset(&wmiSCP, 0, sizeof(wmiSCP));

  wmiSCP.bVerbose = FALSE;
  if (img.numChannels() == 1) {
    wmiSCP.cfColorFormat = Y_ONLY;
  } else if (jpegXRQuality >= 0.5f || img.info().bytesPerVoxel > 1) {
    wmiSCP.cfColorFormat = YUV_444;
  } else {
    wmiSCP.cfColorFormat = YUV_420;
  }
  wmiSCP.bdBitDepth = BD_LONG;
  wmiSCP.bfBitstreamFormat = FREQUENCY;
  wmiSCP.bProgressiveMode = TRUE;
  wmiSCP.olOverlap = OL_ONE;
  wmiSCP.cNumOfSliceMinus1H = wmiSCP.cNumOfSliceMinus1V = 0;
  wmiSCP.sbSubband = SB_ALL;
  if (img.info().lastChannelIsAlphaChannel) {
    wmiSCP.uAlphaMode = 2;
  } else {
    wmiSCP.uAlphaMode = 0;
  }
  wmiSCP.uiDefaultQPIndex = 1;
  wmiSCP.uiDefaultQPIndexAlpha = 1;

  PKFactory* pFactory = nullptr;
  auto pFactoryGuard = folly::makeGuard([&pFactory]() {
    if (pFactory) {
      pFactory->Release(&pFactory);
    }
  });
  struct WMPStream* pEncodeStream = nullptr;
  PKCodecFactory* pCodecFactory = nullptr;
  auto pCodecFactoryGuard = folly::makeGuard([&pCodecFactory]() {
    if (pCodecFactory) {
      pCodecFactory->Release(&pCodecFactory);
    }
  });
  PKImageEncode* pEncoder = nullptr;
  auto pEncoderGuard = folly::makeGuard([&pEncoder]() {
    if (pEncoder) {
      pEncoder->Release(&pEncoder);
    }
  });

  Call(PKCreateFactory(&pFactory, PK_SDK_VERSION));
  Call(pFactory->CreateStreamFromMemory(&pEncodeStream, mem, size));
  Call(PKCreateCodecFactory(&pCodecFactory, WMP_SDK_VERSION));
  Call(pCodecFactory->CreateCodec(&IID_PKImageWmpEncode, (void**)&pEncoder));

  Call(pEncoder->Initialize(pEncoder, pEncodeStream, &wmiSCP, sizeof(wmiSCP)));

  // ImageQuality  Q (BD==1)  Q (BD==8)   Q (BD==16)  Q (BD==32F) Subsample   Overlap
  //[0.0, 0.5)    8-IQ*5     (see table) (see table) (see table) 4:2:0       2
  //[0.5, 1.0)    8-IQ*5     (see table) (see table) (see table) 4:4:4       1
  //[1.0, 1.0]    1          1           1           1           4:4:4       0
  if (jpegXRQuality < 1.0F) {
    // remap [0.8, 0.866, 0.933, 1.0] to [0.8, 0.9, 1.0, 1.1]
    // to use 8-bit DPK QP table (0.933 == Photoshop JPEG 100)
    if (jpegXRQuality > 0.8f && img.info().bytesPerVoxel == 1 && pEncoder->WMP.wmiSCP.cfColorFormat != YUV_420 &&
        pEncoder->WMP.wmiSCP.cfColorFormat != YUV_422) {
      jpegXRQuality = 0.8f + (jpegXRQuality - 0.8f) * 1.5f;
    }

    int qi = (int)(10.f * jpegXRQuality);
    float qf = 10.f * jpegXRQuality - (float)qi;

    int* pQPs = (pEncoder->WMP.wmiSCP.cfColorFormat == YUV_420 || pEncoder->WMP.wmiSCP.cfColorFormat == YUV_422)
                  ? DPK_QPS_420[qi]
                  : (img.info().bytesPerVoxel == 1 ? DPK_QPS_8[qi] : DPK_QPS_16[qi]);

    pEncoder->WMP.wmiSCP.uiDefaultQPIndex = (U8)(0.5f + (float)pQPs[0] * (1.f - qf) + (float)(pQPs + 6)[0] * qf);
    pEncoder->WMP.wmiSCP.uiDefaultQPIndexU = (U8)(0.5f + (float)pQPs[1] * (1.f - qf) + (float)(pQPs + 6)[1] * qf);
    pEncoder->WMP.wmiSCP.uiDefaultQPIndexV = (U8)(0.5f + (float)pQPs[2] * (1.f - qf) + (float)(pQPs + 6)[2] * qf);
    pEncoder->WMP.wmiSCP.uiDefaultQPIndexYHP = (U8)(0.5f + (float)pQPs[3] * (1.f - qf) + (float)(pQPs + 6)[3] * qf);
    pEncoder->WMP.wmiSCP.uiDefaultQPIndexUHP = (U8)(0.5f + (float)pQPs[4] * (1.f - qf) + (float)(pQPs + 6)[4] * qf);
    pEncoder->WMP.wmiSCP.uiDefaultQPIndexVHP = (U8)(0.5f + (float)pQPs[5] * (1.f - qf) + (float)(pQPs + 6)[5] * qf);
  } else {
    pEncoder->WMP.wmiSCP.uiDefaultQPIndex = (U8)jpegXRQuality;
  }
  if (pEncoder->WMP.wmiSCP.uAlphaMode == 2) {
    pEncoder->WMP.wmiSCP_Alpha.uiDefaultQPIndex = wmiSCP.uiDefaultQPIndexAlpha;
  }

  PKPixelFormatGUID guidPixFormat = GUID_PKPixelFormat8bppGray;
  if (img.info().numChannels == 1 && img.info().bytesPerVoxel == 2) {
    guidPixFormat = GUID_PKPixelFormat16bppGray;
  } else if (img.info().numChannels == 3 && img.info().bytesPerVoxel == 1) {
    guidPixFormat = GUID_PKPixelFormat24bppRGB;
  } else if (img.info().numChannels == 4 && img.info().bytesPerVoxel == 1) {
    guidPixFormat = GUID_PKPixelFormat32bppRGBA;
  } else if (img.info().numChannels == 3 && img.info().bytesPerVoxel == 2) {
    guidPixFormat = GUID_PKPixelFormat48bppRGB;
  } else if (img.info().numChannels == 4 && img.info().bytesPerVoxel == 2) {
    guidPixFormat = GUID_PKPixelFormat64bppRGBA;
  }

  Call(pEncoder->SetPixelFormat(pEncoder, guidPixFormat));
  Call(pEncoder->SetSize(pEncoder, img.width(), img.height()));
  if (img.numChannels() == 1) {
    Call(pEncoder->WritePixels(pEncoder, img.height(), const_cast<U8*>(img.channelData(0)), img.rowByteNumber()));
    //      VLOG(1) << pEncoder->WMP.nOffImage;
    //      VLOG(1) << pEncoder->WMP.nCbImage;
    //      VLOG(1) << pEncoder->WMP.nOffAlpha;
    //      VLOG(1) << pEncoder->WMP.nCbAlpha;
  } else {
    ZImg imgTmp(img.info());
    XYZCtoCXYZ(img, imgTmp);
    Call(pEncoder->WritePixels(pEncoder,
                               img.height(),
                               const_cast<U8*>(imgTmp.channelData(0)),
                               imgTmp.rowByteNumber() * imgTmp.numChannels()));
  }

  if (pEncoder->WMP.nOffAlpha > 0) {
    byteWritten = pEncoder->WMP.nOffAlpha + pEncoder->WMP.nCbAlpha;
  } else {
    byteWritten = pEncoder->WMP.nOffImage + pEncoder->WMP.nCbImage;
  }

  return byteWritten;
}

void ZImgJpegXR::checkBeforeWriting(const ZImgInfo& info, const ZImgWriteParameters& paras)
{
  if (paras.compression != Compression::AUTO) {
    throw ZException(fmt::format("compression {} is not supported", paras.compression));
  }
  if (info.numTimes != 1 || info.depth != 1) {
    throw ZException(fmt::format("only 2d image is supported: {}", info));
  }
  if (!(info.numChannels == 1 || (info.numChannels == 4 && info.lastChannelIsAlphaChannel) ||
        (info.numChannels == 3 && !info.lastChannelIsAlphaChannel)) ||
      info.voxelFormat != VoxelFormat::Unsigned || info.bytesPerVoxel > 2) {
    throw ZException(fmt::format("image type currently not supported: {}", info));
  }
  if (paras.jpegXRQuality < 0.01 || paras.jpegXRQuality > 1.) {
    throw ZException(fmt::format("invalid jpeg xr quality: {}", paras.jpegXRQuality));
  }
}

} // namespace nim
