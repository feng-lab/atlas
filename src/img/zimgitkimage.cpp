#include "zimgitkimage.h"

#include <QFile>
#include <QStringList>
#include <QFileInfo>
#include <QDir>
#include "zimgsliceprovider.h"
#include <itkImageIOBase.h>
#include <itkNiftiImageIOFactory.h>
#include <itkNrrdImageIOFactory.h>
#include <itkImageIOFactory.h>

//#define _SUPPORT_DICOM_

#ifdef _SUPPORT_DICOM_
#include <itkGDCMImageIOFactory.h>
#endif

namespace nim {

ZImgITKImage::ZImgITKImage()
  : ZImgFormat()
{
  if (itk::ObjectFactoryBase::GetRegisteredFactories().empty()) {
    itk::NiftiImageIOFactory::RegisterOneFactory();
    itk::NrrdImageIOFactory::RegisterOneFactory();
  #ifdef _SUPPORT_DICOM_
    itk::GDCMImageIOFactory::RegisterOneFactory();
  #endif
  }
}

ZImgITKImage::~ZImgITKImage()
{
}

QString ZImgITKImage::shortName() const
{
  return "ITKImage";
}

QString ZImgITKImage::fullName() const
{
  return "ITKImage";
}

QStringList ZImgITKImage::extensions() const
{
  QStringList res;

  typedef itk::ImageIOBase                        IOBaseType;
  typedef std::list<itk::LightObject::Pointer>    ArrayOfImageIOType;
  typedef IOBaseType::ArrayOfExtensionsType       ArrayOfExtensionsType;

  ArrayOfImageIOType allobjects = itk::ObjectFactoryBase::CreateAllInstance("itkImageIOBase");

  ArrayOfImageIOType::iterator itr = allobjects.begin();

  while( itr != allobjects.end() ) {
    IOBaseType * io = dynamic_cast< IOBaseType * >( itr->GetPointer() );
    if( io ) {
      //LINFO() << "ImageIO: " << io->GetNameOfClass();
      const ArrayOfExtensionsType & readExtensions  = io->GetSupportedReadExtensions();
      ArrayOfExtensionsType::const_iterator readItr  = readExtensions.begin();

      while( readItr != readExtensions.end() ) {
        res.push_back(readItr->c_str());
        res.last().remove(0,1); // remove '.'
        ++readItr;
      }
    }
    ++itr;
  }
#ifdef _SUPPORT_DICOM_
  res.push_back("dcm");
#endif

  return res;
}

void ZImgITKImage::readInfo(const QString &filename, std::vector<ZImgInfo> &infos, std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> *subBlocks,
                            std::vector<std::set<size_t>> *pyramidalRatios)
{
  try {
  itk::ImageIOBase::Pointer imageIO = itk::ImageIOFactory::CreateImageIO(filename.toLocal8Bit(), itk::ImageIOFactory::ReadMode);

  if (imageIO.IsNull())
    throw ZIOException("can not create reader");

  imageIO->SetFileName(filename.toLocal8Bit());
  imageIO->ReadImageInformation();

  bool isNrrd = QString(imageIO->GetNameOfClass()).contains("Nrrd");

  infos.resize(1);
  parseInfo(imageIO.GetPointer(), infos[0]);

  if (isNrrd) {
    createEmptySubBlocks(infos, subBlocks, pyramidalRatios);
  } else {
    createDefaultSubBlocks(filename, infos, subBlocks, pyramidalRatios);
  }
  }
  catch ( itk::ExceptionObject & err ) {
    throw ZIOException(err.GetDescription());
  }
}

void ZImgITKImage::readMetadata(const QString &, ZImgMetadata &, size_t )
{
  try {
  }
  catch ( itk::ExceptionObject & err ) {
    throw ZIOException(err.GetDescription());
  }
}

void ZImgITKImage::readThumbnail(const QString &, ZImgThumbernail &, const ZImgRegion &, size_t )
{
  try {
  }
  catch ( itk::ExceptionObject & err ) {
    throw ZIOException(err.GetDescription());
  }
}

void ZImgITKImage::readImg(const QString &filename, ZImg &img, const ZImgRegion &region, size_t scene, size_t ratio)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }

  try {
  itk::ImageIOBase::Pointer imageIO = itk::ImageIOFactory::CreateImageIO(filename.toLocal8Bit(), itk::ImageIOFactory::ReadMode);

  if (imageIO.IsNull())
    throw ZIOException("can not create reader");

  imageIO->SetFileName(filename.toLocal8Bit());
  imageIO->ReadImageInformation();

  bool isNrrd = QString(imageIO->GetNameOfClass()).contains("Nrrd");

  ZImgInfo imgInfo;
  parseInfo(imageIO.GetPointer(), imgInfo);

  if (region.isEmpty() || !region.isValid(imgInfo)) {
    throw ZIOException(QString("Invalid image region. Image info: '%1', region: '%2'").arg(imgInfo.toQString()).arg(region.toQString()));
  }

  if (region.containsWholeImg(imgInfo) || isNrrd) {
    img = ZImg(imgInfo);
    itk::ImageIORegion ioRegion(4);
    ioRegion.SetIndex(0, 0);
    ioRegion.SetIndex(1, 0);
    ioRegion.SetIndex(2, 0);
    ioRegion.SetIndex(3, 0);
    ioRegion.SetSize(0, img.width());
    ioRegion.SetSize(1, img.height());
    ioRegion.SetSize(2, img.depth());
    ioRegion.SetSize(3, img.numTimes());
    imageIO->SetIORegion(ioRegion);
    if (imgInfo.numTimes > 1) {
      std::vector<uint8_t> buf(img.byteNumber());
      imageIO->Read(buf.data());
      fixDimensionOrder(buf.data(), "CXYZT", img);
    } else {
      imageIO->Read(img.channelData(0));
    }
    if (imgInfo.numChannels > 1 && imgInfo.numTimes == 1) { // if numTimes > 1 then dimension order is already fixed
      ZImg tpImg(imgInfo);
      CXYZtoXYZC(img, tpImg);
      img.swap(tpImg);
    }

    if (isNrrd && !region.containsWholeImg(imgInfo))
      img = img.crop(region);
  } else {
    ZImgInfo clipInfo = region.clip(imgInfo);
    bool clipChannel = clipInfo.numChannels != imgInfo.numChannels;
    if (clipChannel) {
      clipInfo.numChannels = imgInfo.numChannels;
    }

    ZImg tmpImg(clipInfo);
    ZImgRegion rgn = region;
    rgn.resolveRegionEnd(imgInfo);
    itk::ImageIORegion ioRegion(4);
    ioRegion.SetIndex(0, rgn.start.x);
    ioRegion.SetIndex(1, rgn.start.y);
    ioRegion.SetIndex(2, rgn.start.z);
    ioRegion.SetIndex(3, rgn.start.t);
    ioRegion.SetSize(0, rgn.end.x - rgn.start.x);
    ioRegion.SetSize(1, rgn.end.y - rgn.start.y);
    ioRegion.SetSize(2, rgn.end.z - rgn.start.z);
    ioRegion.SetSize(3, rgn.end.t - rgn.start.t);
    imageIO->SetIORegion(ioRegion);
    if (clipInfo.numTimes > 1) {
      std::vector<uint8_t> buf(tmpImg.byteNumber());
      imageIO->Read(buf.data());
      fixDimensionOrder(buf.data(), "CXYZT", tmpImg);
    } else {
      imageIO->Read(tmpImg.channelData(0));
    }
    if (clipInfo.numChannels > 1 && clipInfo.numTimes == 1) { // if numTimes > 1 then dimension order is already fixed
      ZImg tpImg(clipInfo);
      CXYZtoXYZC(tmpImg, tpImg);
      tmpImg.swap(tpImg);
    }

    if (clipChannel) {
      ZImgRegion crgn;
      crgn.start.c = rgn.start.c;
      crgn.end.c = rgn.end.c;
      img = tmpImg.crop(rgn);
    } else {
      img.swap(tmpImg);
    }
  }
  }
  catch ( itk::ExceptionObject & err ) {
    throw ZIOException(err.GetDescription());
  }

  if (ratio > 1) {
    img.zoom(1.0 / ratio, 1.0 / ratio);
  }
}

void ZImgITKImage::writeImg(const QString &filename, const ZImg &img, Compression comp)
{
//  assert(comp == Compression::AUTO || comp == Compression::NONE || comp == Compression::DEFLATE);
//  if (img.numTimes() != 1) {
//    throw ZIOException("time sequence image is not supported");
//  }

//  bool multipleChannel = img.numChannels() > 1;

//  ZImg tmpImg;
//  if (multipleChannel) {
//    tmpImg = ZImg(img.info());
//    XYZCtoCXYZ(img, tmpImg);
//  }

//  int nDims = 3;
//  if (img.depth() == 1) {
//    nDims = 2;
//  }
//  int dimSize[3];
//  dimSize[0] = img.width();
//  dimSize[1] = img.height();
//  dimSize[2] = img.depth();
//  MET_DistanceUnitsEnumType distanceUnitType = MET_DISTANCE_UNITS_UNKNOWN;
//  float elementSpacing[3];
//  if (img.voxelSizeUnit() == VoxelSizeUnit::Voxel) {
//    elementSpacing[0] = img.voxelSizeX();
//    elementSpacing[1] = img.voxelSizeY();
//    elementSpacing[2] = img.voxelSizeZ();
//  } else {
//    distanceUnitType = MET_DISTANCE_UNITS_UM;
//    elementSpacing[0] = img.voxelSizeXInUm();
//    elementSpacing[1] = img.voxelSizeYInUm();
//    elementSpacing[2] = img.voxelSizeZInUm();
//  }
//  MET_ValueEnumType elementType = MET_CHAR;
//  switch (img.voxelFormat()) {
//  case VoxelFormat::Signed:
//    switch (img.bytesPerVoxel()) {
//    case 1:
//      elementType = multipleChannel ? MET_CHAR_ARRAY : MET_CHAR;
//      break;
//    case 2:
//      elementType = multipleChannel ? MET_SHORT_ARRAY : MET_SHORT;
//      break;
//    case 4:
//      elementType = multipleChannel ? MET_INT_ARRAY : MET_INT;
//      break;
//    case 8:
//      elementType = multipleChannel ? MET_LONG_LONG_ARRAY : MET_LONG_LONG;
//      break;
//    default:
//      assert(false);
//      break;
//    }
//    break;
//  case VoxelFormat::Unsigned:
//    switch (img.bytesPerVoxel()) {
//    case 1:
//      elementType = multipleChannel ? MET_UCHAR_ARRAY : MET_UCHAR;
//      break;
//    case 2:
//      elementType = multipleChannel ? MET_USHORT_ARRAY : MET_USHORT;
//      break;
//    case 4:
//      elementType = multipleChannel ? MET_UINT_ARRAY : MET_UINT;
//      break;
//    case 8:
//      elementType = multipleChannel ? MET_ULONG_LONG_ARRAY : MET_ULONG_LONG;
//      break;
//    default:
//      assert(false);
//      break;
//    }
//    break;
//  case VoxelFormat::Float:
//    switch (img.bytesPerVoxel()) {
//    case 4:
//      elementType = multipleChannel ? MET_FLOAT_ARRAY : MET_FLOAT;
//      break;
//    case 8:
//      elementType = multipleChannel ? MET_DOUBLE_ARRAY : MET_DOUBLE;
//      break;
//    default:
//      assert(false);
//      break;
//    }
//  }

//  MetaImage metaImage(nDims, dimSize, elementSpacing, elementType, img.numChannels(),
//                      multipleChannel ? const_cast<uint8_t*>(tmpImg.channelData(0)) : const_cast<uint8_t*>(img.channelData(0)));

//  metaImage.DistanceUnits(distanceUnitType);
//  metaImage.CompressedData(comp != Compression::NONE);

//  if (!metaImage.Write(QFile::encodeName(filename).constData())) {
//    throw ZIOException("Can not write metaimage");
//  }
}

void ZImgITKImage::writeImg(const QString &filename, const ZImgSliceProvider &imgSliceProvider, Compression comp)
{
//  assert(comp == Compression::AUTO || comp == Compression::NONE || comp == Compression::DEFLATE);
//  if (imgSliceProvider.imgInfo().numTimes != 1) {
//    throw ZIOException("time sequence image is not supported");
//  }
//  writeImg(filename, imgSliceProvider.allSlices(0), comp);
}

bool ZImgITKImage::supportRead() const
{
  return true;
}

bool ZImgITKImage::supportWrite() const
{
  return false;
}

void ZImgITKImage::parseInfo(const itk::ImageIOBase *imageIO, ZImgInfo &info)
{
  uint32_t ndims = imageIO->GetNumberOfDimensions();
  if (ndims == 1) {
    info.width = imageIO->GetDimensions(0);
    info.height = 1;
    info.depth = 1;
    info.numTimes = 1;
  } else if (ndims == 2) {
    info.width = imageIO->GetDimensions(0);
    info.height = imageIO->GetDimensions(1);
    info.depth = 1;
    info.numTimes = 1;
  } else if (ndims == 3) {
    info.width = imageIO->GetDimensions(0);
    info.height = imageIO->GetDimensions(1);
    info.depth = imageIO->GetDimensions(2);
    info.numTimes = 1;
  } else if (ndims == 4) {
    info.width = imageIO->GetDimensions(0);
    info.height = imageIO->GetDimensions(1);
    info.depth = imageIO->GetDimensions(2);
    info.numTimes = imageIO->GetDimensions(3);
  } else {
    throw ZIOException(QString("NDims not supported: %1.").arg(ndims));
  }
  info.numChannels = imageIO->GetNumberOfComponents();
  switch (imageIO->GetComponentType()) {
  case itk::ImageIOBase::CHAR:
    info.bytesPerVoxel = 1;
    info.voxelFormat = VoxelFormat::Signed;
    break;
  case itk::ImageIOBase::UCHAR:
    info.bytesPerVoxel = 1;
    info.voxelFormat = VoxelFormat::Unsigned;
    break;
  case itk::ImageIOBase::SHORT:
    info.bytesPerVoxel = 2;
    info.voxelFormat = VoxelFormat::Signed;
    break;
  case itk::ImageIOBase::USHORT:
    info.bytesPerVoxel = 2;
    info.voxelFormat = VoxelFormat::Unsigned;
    break;
  case itk::ImageIOBase::INT:
    info.bytesPerVoxel = 4;
    info.voxelFormat = VoxelFormat::Signed;
    break;
  case itk::ImageIOBase::UINT:
    info.bytesPerVoxel = 4;
    info.voxelFormat = VoxelFormat::Unsigned;
    break;
  case itk::ImageIOBase::LONG:
    info.bytesPerVoxel = 8;
    info.voxelFormat = VoxelFormat::Signed;
    break;
  case itk::ImageIOBase::ULONG:
    info.bytesPerVoxel = 8;
    info.voxelFormat = VoxelFormat::Unsigned;
    break;
  case itk::ImageIOBase::FLOAT:
    info.bytesPerVoxel = 4;
    info.voxelFormat = VoxelFormat::Float;
    break;
  case itk::ImageIOBase::DOUBLE:
    info.bytesPerVoxel = 8;
    info.voxelFormat = VoxelFormat::Float;
    break;
  default:
    throw ZIOException("Not supported ElementType");
    break;
  }

  info.voxelSizeUnit = VoxelSizeUnit::mm;
  if (ndims == 1) {
    info.voxelSizeX = imageIO->GetSpacing(0);
  } else if (ndims == 2) {
    info.voxelSizeX = imageIO->GetSpacing(0);
    info.voxelSizeY = imageIO->GetSpacing(1);
  } else if (ndims > 3) {
    info.voxelSizeX = imageIO->GetSpacing(0);
    info.voxelSizeY = imageIO->GetSpacing(1);
    info.voxelSizeZ = imageIO->GetSpacing(2);
  }
  info.createDefaultDescriptions();
  if (ndims == 4) {
    for (size_t i=0; i<info.timeStamps.size(); ++i)
      info.timeStamps[i] *= imageIO->GetSpacing(3);
  }

  if (info.isEmpty()) {
    throw ZIOException("Empty Image");
  }
}

} // namespace

