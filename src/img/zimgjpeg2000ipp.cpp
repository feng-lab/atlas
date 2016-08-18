#include "zimgjpeg2000.h"

#include "uic_base_stream_input.h"
#include "uic_base_stream_output.h"
#include "ippdefs.h"
#include "stdfilein.h"
#include "stdfileout.h"
#include "uic_jpeg2000_dec.h"
#include "uic_jp2_dec.h"
#include "uic_jp2_enc.h"
#include "ippcore.h"
#include "ippi.h"
#include "ipps.h"
#include "ippcc.h"
#include <iostream>
#include "diagndescr.h"
#include "zcpuinfo.h"
#include <cmath>

using namespace UIC;
using namespace nim;

namespace {

bool IsJP2(BaseStreamInput& in)
{
  unsigned char buf[4];
  UIC::BaseStream::TSize cnt;

  if(UIC::BaseStream::StatusOk != in.Seek(4, UIC::BaseStreamInput::Beginning))
    throw ZIOException("unsupported Image format");

  if(UIC::BaseStream::StatusOk != in.Read(buf, 4*sizeof(char),cnt))
    throw ZIOException("unsupported Image format");

  if(UIC::BaseStream::StatusOk != in.Seek(0, UIC::BaseStreamInput::Beginning))
    throw ZIOException("unsupported Image format");

  if(buf[0] == 0x6a && buf[1] == 0x50 && buf[2] == 0x20 && buf[3] == 0x20)
    return true;

  return false;
} // IsJP2()

inline void checkIPPError(IppStatus status)
{
  if (status != ippStsNoErr) {
    throw ZIOException(ippGetStatusString(status));
  }
}

class LogDiagn: public BaseStreamDiagn {
  void Write(const BaseDiagnDescriptor &descr)
  {
    const BaseDiagnDescriptor *pD = &descr;
    LWARN() << pD->GetMessage() << "\n";
  }
};

BaseImageDecoder* initDecoder(CStdFileInput &in, JP2Decoder &JP2decoder, JPEG2000Decoder &JP2Kdecoder, LogDiagn &diagnOutput)
{
  BaseImageDecoder* decoder = nullptr;
  if (IsJP2(in))
    decoder = &JP2decoder;
  else
    decoder = &JP2Kdecoder;

  if (ExcStatusOk != decoder->Init())
    throw ZIOException("can not initialize codec");

  decoder->SetNOfThreads(ZCpuInfoInstance.nPhysicalCores);

  if (ExcStatusOk != decoder->AttachStream(in))
    throw ZIOException("can not attach stream");

  decoder->AttachDiagnOut(diagnOutput);

  return decoder;
}

ZImgInfo infoFromHead(const ImageColorSpec &colorSpec, const ImageSamplingGeometry &geometry)
{
  ZImgInfo info;
  info.bytesPerVoxel = std::ceil((colorSpec.DataRange()->BitDepth() + 1) / 8);
  info.width = geometry.RefGridRect().Width();
  info.height = geometry.RefGridRect().Height();
  info.depth = 1;
  info.numChannels = geometry.NOfComponents();
  info.numTimes = 1;
  info.numLocations = 1;
  info.voxelFormat = colorSpec.DataRange()->IsSigned() ? VF_Signed : VF_Unsigned;
  info.createDefaultDescriptions();
  return info;
}

}

namespace nim {

ZImgJpeg2000::ZImgJpeg2000()
{
}

ZImgJpeg2000::~ZImgJpeg2000()
{
}

QString ZImgJpeg2000::shortName() const
{
  return "Jpeg2000";
}

QString ZImgJpeg2000::fullName() const
{
  return "Jpeg2000";
}

QStringList ZImgJpeg2000::extensions() const
{
  QStringList res;
  res << "jp2" << "j2c";
  return res;
}

void ZImgJpeg2000::readInfo(const QString &filename, ZImgInfo &info)
{
  CStdFileInput in;
  if (!BaseStream::IsOk(in.Open(qPrintable(filename)))) {
    throw ZIOException("can not open file");
  }
  JP2Decoder JP2decoder;
  JPEG2000Decoder JP2Kdecoder;
  LogDiagn diagnOutput;
  BaseImageDecoder *decoder = initDecoder(in, JP2decoder, JP2Kdecoder, diagnOutput);
  ImageColorSpec colorSpec;
  ImageSamplingGeometry geometry;
  if(ExcStatusOk != decoder->ReadHeader(colorSpec, geometry))
    throw ZIOException("can not read head");
  geometry.ReduceByGCD();
  info = infoFromHead(colorSpec, geometry);
}

void ZImgJpeg2000::readMetadata(const QString &, ZImgMetadata &)
{
}

void ZImgJpeg2000::readThumbnail(const QString &, ZImgThumbernail &, const ZImgRegion &)
{
}

void ZImgJpeg2000::readImg(const QString &filename, ZImg &img, const ZImgRegion &region)
{
  CStdFileInput in;
  if (!BaseStream::IsOk(in.Open(qPrintable(filename)))) {
    throw ZIOException("can not open file");
  }
  JP2Decoder JP2decoder;
  JPEG2000Decoder JP2Kdecoder;
  LogDiagn diagnOutput;
  BaseImageDecoder *decoder = initDecoder(in, JP2decoder, JP2Kdecoder, diagnOutput);
  ImageColorSpec colorSpec;
  ImageSamplingGeometry geometry;
  if(ExcStatusOk != decoder->ReadHeader(colorSpec, geometry))
    throw ZIOException("can not read head");
  geometry.ReduceByGCD();
  ZImgInfo info = infoFromHead(colorSpec, geometry);

  if (region.isEmpty() || !region.isValid(info)) {
    throw ZIOException(QString("Invalid image region. Image info: '%1', region: '%2'").arg(info.toQString()).arg(region.toQString()));
  }

  Image imagePn;

  J2KPRECISION j2kArithmetic = J2K_16;
  if (colorSpec.DataRange()->BitDepth() + 1 >= 14)
    j2kArithmetic = J2K_32;

  if (J2K_32 == j2kArithmetic)
    imagePn.Buffer().ReAlloc(T32s, Plane, geometry);
  else //J2K_16 == j2kArithmetic)
    imagePn.Buffer().ReAlloc(T16s, Plane, geometry);

  const ImageDataOrder &dataOrderPn = imagePn.Buffer().BufferFormat().DataOrder();
  //LINFO() << colorSpec.DataRange()->BitDepth() << colorSpec.DataRange()->DataType() << colorSpec.DataRange()->Max().v32u << dataOrderPn.ComponentOrder()
  //           << geometry.NOfComponents();

  geometry.SetEnumSampling(S444);

  imagePn.ColorSpec().ReAlloc(info.numChannels);
  imagePn.ColorSpec().SetColorSpecMethod(Enumerated);
  imagePn.ColorSpec().SetComponentToColorMap(Direct);
  imagePn.ColorSpec().SetEnumColorSpace(colorSpec.EnumColorSpace());

  if (ExcStatusOk != decoder->ReadData(imagePn.Buffer().DataPtr(), dataOrderPn))
    throw ZIOException("can not read data");

  ZImg res(info);

  IppiSize size;

  size.width  = res.width();
  size.height = res.height();

  for (size_t c=0; c<res.numChannels(); ++c) {
    if (res.bytesPerVoxel() <= 1) {
      if (J2K_32 == j2kArithmetic) {
        checkIPPError(ippiConvert_32s8u_C1R(imagePn.Buffer().DataPtr()[c].p32s, dataOrderPn.LineStep()[c],
                                            res.channelData<uint8_t>(c), res.rowByteNumber(), size));
      } else { // J2K_16 == j2kArithmetic
        checkIPPError(ippiConvert_16s8u_C1R(imagePn.Buffer().DataPtr()[c].p16s, dataOrderPn.LineStep()[c],
                                            res.channelData<uint8_t>(c), res.rowByteNumber(), size));
      }
    } else {
      if (J2K_32 == j2kArithmetic) {
        int32_t *src = imagePn.Buffer().DataPtr()[c].p32s;
        uint16_t *pDst = res.channelData<uint16_t>(c);

        for(int i = 0; i < size.height; ++i) {
          int32_t *pSrc = (int32_t*)((uint8_t*)src + i*dataOrderPn.LineStep()[c]);
          for(int j = 0; j < size.width; ++j)
            *pDst++ = saturate_cast<uint16_t>(*pSrc++);
        }
      } else { // J2K_16 == j2kArithmetic
        checkIPPError(ippiCopy_16s_C1R(imagePn.Buffer().DataPtr()[c].p16s, dataOrderPn.LineStep()[c],
                                       res.channelData<int16_t>(c), res.rowByteNumber(), size));
      }
    }
  }

  ImageEnumColorSpace colorSpace = colorSpec.EnumColorSpace();
  if (colorSpace == YCbCr) {
    const Ipp8u* pSrc[3];
    pSrc[0] = res.channelData<uint8_t>(0);
    pSrc[1] = res.channelData<uint8_t>(1);
    pSrc[2] = res.channelData<uint8_t>(2);

    ZImg tmpImg(info);
    Ipp8u* pDes[3];
    pDes[0] = tmpImg.channelData<uint8_t>(0);
    pDes[1] = tmpImg.channelData<uint8_t>(1);
    pDes[2] = tmpImg.channelData<uint8_t>(2);

    checkIPPError(ippiYCbCrToRGB_8u_P3R(pSrc, res.rowByteNumber(), pDes, tmpImg.rowByteNumber(), size));
    //LINFO() << "convert";
    res.swap(tmpImg);
  }

  if (region.containsWholeImg(info))
    img.swap(res);
  else
    img = res.crop(region);
}

void ZImgJpeg2000::writeImg(const QString &filename, const ZImg &img, Compression)
{
  if (!img.is2DImg() || (img.numChannels() != 1 && img.numChannels() != 3) || img.bytesPerVoxel() > 2) {
    throw ZIOException("only support 8 and 16 bit grayscale or RGB 2d image");
  }
  CStdFileOutput out;
  if (!BaseStream::IsOk(out.Open(qPrintable(filename)))) {
    throw ZIOException("can not open output file");
  }

  IppiSize roi;
  roi.height = img.height();
  roi.width  = img.width();

  JP2Encoder jp2enc;
  if (ExcStatusOk != jp2enc.Init())
    throw ZIOException("can not init codec");

  jp2enc.SetNOfThreads(ZCpuInfoInstance.nPhysicalCores);

  if (ExcStatusOk != jp2enc.AttachStream(out))
    throw ZIOException("can not attach stream");

  RectSize size;
  size.SetWidth(img.width());
  size.SetHeight(img.height());

  Point origin;
  origin.SetX(0);
  origin.SetY(0);

  Rect refgrid;
  refgrid.SetOrigin(origin);
  refgrid.SetSize(size);

  ImageSamplingGeometry geometry;
  geometry.SetRefGridRect(refgrid);
  geometry.ReAlloc(img.numChannels());
  geometry.SetEnumSampling(S444);

  size_t du = sizeof(Ipp32s);
  ImageDataOrder dataOrder;
  dataOrder.SetDataType(T32s);
  dataOrder.ReAlloc(Plane, img.numChannels());

  for (size_t i = 0; i < img.numChannels(); ++i) {
    dataOrder.PixelStep()[i] = NOfBytes(dataOrder.DataType());
    dataOrder.LineStep() [i] = geometry.RefGridRect().Width() * du;
  }

  Image imagePn;
  imagePn.Buffer().ReAlloc(dataOrder, geometry);

  imagePn.ColorSpec().ReAlloc(img.numChannels());
  imagePn.ColorSpec().SetColorSpecMethod(Enumerated);
  imagePn.ColorSpec().SetComponentToColorMap(Direct);

  for (size_t i = 0; i < img.numChannels(); ++i) {
    if(img.bytesPerVoxel() <= 1) {
      imagePn.ColorSpec().DataRange()[i].SetAsRange8u(255);
    } else {
      imagePn.ColorSpec().DataRange()[i].SetAsRange16u(65535);
    }
  }

  imagePn.ColorSpec().SetEnumColorSpace((img.numChannels() == 1) ? Grayscale : RGB);

  for (size_t c=0; c<img.numChannels(); ++c) {
    if (img.bytesPerVoxel() <= 1) {
      checkIPPError(ippiConvert_8u32s_C1R(img.channelData<uint8_t>(c), img.rowByteNumber(),
                                          imagePn.Buffer().DataPtr()[c].p32s, dataOrder.LineStep()[c], roi));
    } else if (img.voxelFormat() == VF_Unsigned) {
      checkIPPError(ippiConvert_16u32s_C1R(img.channelData<uint16_t>(c), img.rowByteNumber(),
                                           imagePn.Buffer().DataPtr()[c].p32s, dataOrder.LineStep()[c], roi));
    } else {
      checkIPPError(ippiConvert_16s32s_C1R(img.channelData<int16_t>(c), img.rowByteNumber(),
                                           imagePn.Buffer().DataPtr()[c].p32s, dataOrder.LineStep()[c], roi));
    }
  }

  if (ExcStatusOk != jp2enc.AttachImage(imagePn))
    throw ZIOException("can not attach image");

  if (ExcStatusOk != jp2enc.SetParams(5, false, true ,false, 0, 0, img.byteNumber()))
    throw ZIOException("can not set parameters");

  if (ExcStatusOk != jp2enc.WriteHeader())
    throw ZIOException("can not write header");

  if (ExcStatusOk != jp2enc.WriteData())
    throw ZIOException("can not write data");
}

bool ZImgJpeg2000::supportRead() const
{
  return true;
}

bool ZImgJpeg2000::supportWrite() const
{
  return true;
}

} // namespace

