#include "zimginterface.h"

#include <magic_enum.hpp>

namespace nim {

size_t byteNumber(DataType dt)
{
  switch (dt) {
    case DataType::Byte:
    case DataType::Ascii:
      return 1;
    case DataType::Short:
      return 2;
    case DataType::Long:
      return 4;
    case DataType::Rational:
      return 8;
    case DataType::SByte:
    case DataType::Undefined:
      return 1;
    case DataType::SShort:
      return 2;
    case DataType::SLong:
      return 4;
    case DataType::SRational:
      return 8;
    case DataType::Float:
      return 4;
    case DataType::Double:
      return 8;
    case DataType::IFD:
      return 4;
    case DataType::Long8:
    case DataType::SLong8:
    case DataType::IFD8:
      return 8;
    default:
      throw ZException(QString("Invalid DataType %1").arg(enumToUnderlyingType(dt)));
  }
}

double unitSizeInMeter(VoxelSizeUnit vsu)
{
  switch (vsu) {
    case VoxelSizeUnit::none:
      return 1;
    case VoxelSizeUnit::inch:
      return 0.0254;
    case VoxelSizeUnit::cm:
      return 1e-2;
    case VoxelSizeUnit::mm:
      return 1e-3;
    case VoxelSizeUnit::um:
      return 1e-6;
    case VoxelSizeUnit::nm:
      return 1e-9;
    case VoxelSizeUnit::m:
      return 1;
    case VoxelSizeUnit::hm:
      return 1e2;
    case VoxelSizeUnit::km:
      return 1e3;
    default:
      throw ZException(QString("Invalid VoxelSizeUnit %1").arg(enumToUnderlyingType(vsu)));
  }
}

template<typename TEnum>
std::string_view enumToString(TEnum e)
{
  static_assert(std::is_enum_v<remove_cvref_t<TEnum>>, "Need Enum Type");
  auto res = magic_enum::enum_name(e);
  if (res.empty()) {
    throw ZIOException(fmt::format("invalid enum value: {}", e));
  }
  return res;
}

template<typename TEnum>
TEnum stringToEnum(std::string_view s)
{
  static_assert(std::is_enum_v<remove_cvref_t<TEnum>>, "Need Enum Type");
  auto e = magic_enum::enum_cast<TEnum>(s);
  if (!e.has_value()) {
    throw ZIOException(fmt::format("invalid enum string: {}", s));
  }
  return e.value();
}

template<>
std::string_view enumToString<Compression>(Compression e)
{
  switch (e) {
    case Compression::AUTO:
      return "Auto";
    case Compression::NONE:
      return "None";
    case Compression::LZW:
      return "LZW";
    case Compression::JPEG:
      return "JPEG";
    case Compression::T85:
      return "T85";
    case Compression::T43:
      return "T43";
    case Compression::PACKBITS:
      return "PACKBITS";
    case Compression::DEFLATE:
      return "DEFLATE";
    case Compression::ADOBE_DEFLATE:
      return "ADOBE_DEFLATE";
    case Compression::DCS:
      return "DCS";
    case Compression::JP2000:
      return "JP2000";
    case Compression::LZMA:
      return "LZMA";
    case Compression::ZSTD:
      return "ZSTD";
    case Compression::WEBP:
      return "WEBP";
    default:
      throw ZIOException(fmt::format("invalid Compression: {}", e));
  }
}

template std::string_view enumToString<DataType>(DataType);
template std::string_view enumToString<VoxelFormat>(VoxelFormat);
template std::string_view enumToString<VoxelSizeUnit>(VoxelSizeUnit);
template std::string_view enumToString<FileFormat>(FileFormat);
template std::string_view enumToString<PadOption>(PadOption);
template std::string_view enumToString<Interpolant>(Interpolant);
template std::string_view enumToString<Dimension>(Dimension);
template std::string_view enumToString<ImgMergeMode>(ImgMergeMode);

template DataType stringToEnum<DataType>(std::string_view);
template VoxelFormat stringToEnum<VoxelFormat>(std::string_view);
template VoxelSizeUnit stringToEnum<VoxelSizeUnit>(std::string_view);
template FileFormat stringToEnum<FileFormat>(std::string_view);
template PadOption stringToEnum<PadOption>(std::string_view);
template Interpolant stringToEnum<Interpolant>(std::string_view);
template Dimension stringToEnum<Dimension>(std::string_view);
template ImgMergeMode stringToEnum<ImgMergeMode>(std::string_view);


#if 0
inline QString enumToString(DataType dt)
{
  switch (dt) {
    case DataType::Byte:
      return QStringLiteral("Byte");
    case DataType::Ascii:
      return QStringLiteral("Ascii");
    case DataType::Short:
      return QStringLiteral("Short");
    case DataType::Long:
      return QStringLiteral("Long");
    case DataType::Rational:
      return QStringLiteral("Rational");
    case DataType::SByte:
      return QStringLiteral("SByte");
    case DataType::Undefined:
      return QStringLiteral("Undefined");
    case DataType::SShort:
      return QStringLiteral("SShort");
    case DataType::SLong:
      return QStringLiteral("SLong");
    case DataType::SRational:
      return QStringLiteral("SRational");
    case DataType::Float:
      return QStringLiteral("Float");
    case DataType::Double:
      return QStringLiteral("Double");
    case DataType::IFD:
      return QStringLiteral("IFD");
    case DataType::Long8:
      return QStringLiteral("Long8");
    case DataType::SLong8:
      return QStringLiteral("SLong8");
    case DataType::IFD8:
      return QStringLiteral("IFD8");
    default:
      throw ZIOException("invalid DataType");
  }
}

inline QString enumToString(VoxelFormat vf)
{
  switch (vf) {
    case VoxelFormat::Unsigned:
      return QStringLiteral("Unsigned");
    case VoxelFormat::Signed:
      return QStringLiteral("Signed");
    case VoxelFormat::Float:
      return QStringLiteral("Float");
    default:
      throw ZIOException("invalid VoxelFormat");
  }
}

inline VoxelFormat stringToVoxelFormat(const QString& str)
{
  if (str == QStringLiteral("Unsigned")) {
    return VoxelFormat::Unsigned;
  } else if (str == QStringLiteral("Signed")) {
    return VoxelFormat::Signed;
  } else if (str == QStringLiteral("Float")) {
    return VoxelFormat::Float;
  } else {
    throw ZIOException("invalid VoxelFormat " + str);
  }
}

inline QString enumToString(VoxelSizeUnit vsu)
{
  switch (vsu) {
    case VoxelSizeUnit::none:
      return QStringLiteral("none");
    case VoxelSizeUnit::inch:
      return QStringLiteral("inch");
    case VoxelSizeUnit::cm:
      return QStringLiteral("cm");
    case VoxelSizeUnit::mm:
      return QStringLiteral("mm");
    case VoxelSizeUnit::um:
      return QStringLiteral("um");
    case VoxelSizeUnit::nm:
      return QStringLiteral("nm");
    case VoxelSizeUnit::m:
      return QStringLiteral("m");
    case VoxelSizeUnit::hm:
      return QStringLiteral("hm");
    case VoxelSizeUnit::km:
      return QStringLiteral("km");
    default:
      throw ZIOException("invalid VoxelSizeUnit");
  }
}

inline VoxelSizeUnit stringToVoxelSizeUnit(const QString& str)
{
  if (str == QStringLiteral("none")) {
    return VoxelSizeUnit::none;
  } else if (str == QStringLiteral("inch")) {
    return VoxelSizeUnit::inch;
  } else if (str == QStringLiteral("cm")) {
    return VoxelSizeUnit::cm;
  } else if (str == QStringLiteral("mm")) {
    return VoxelSizeUnit::mm;
  } else if (str == QStringLiteral("um")) {
    return VoxelSizeUnit::um;
  } else if (str == QStringLiteral("nm")) {
    return VoxelSizeUnit::nm;
  } else if (str == QStringLiteral("m")) {
    return VoxelSizeUnit::m;
  } else if (str == QStringLiteral("hm")) {
    return VoxelSizeUnit::hm;
  } else if (str == QStringLiteral("km")) {
    return VoxelSizeUnit::km;
  } else {
    throw ZIOException("invalid VoxelSizeUnit " + str);
  }
}

inline QString enumToString(FileFormat f)
{
  switch (f) {
    case FileFormat::Unknown:
      return QStringLiteral("Unknown");
    case FileFormat::HDF5Img:
      return QStringLiteral("HDF5Img");
    case FileFormat::OmeTiff:
      return QStringLiteral("OmeTiff");
    case FileFormat::Tiff:
      return QStringLiteral("Tiff");
    case FileFormat::Vaa3DRaw:
      return QStringLiteral("Vaa3DRaw");
    case FileFormat::ZeissLsm:
      return QStringLiteral("ZeissLsm");
    case FileFormat::Jpeg:
      return QStringLiteral("Jpeg");
    case FileFormat::JpegXR:
      return QStringLiteral("JpegXR");
    case FileFormat::Png:
      return QStringLiteral("Png");
    case FileFormat::FreeImage:
      return QStringLiteral("FreeImage");
    case FileFormat::MetaImage:
      return QStringLiteral("MetaImage");
    case FileFormat::ZeissCZI:
      return QStringLiteral("ZeissCZI");
    case FileFormat::ITKImage:
      return QStringLiteral("ITKImage");
    case FileFormat::Leica:
      return QStringLiteral("Leica");
    default:
      throw ZIOException("invalid FileFormat");
  }
}

inline FileFormat stringToFileFormat(const QString& str)
{
  if (str == QStringLiteral("Unknown")) {
    return FileFormat::Unknown;
  } else if (str == QStringLiteral("HDF5Img")) {
    return FileFormat::HDF5Img;
  } else if (str == QStringLiteral("OmeTiff")) {
    return FileFormat::OmeTiff;
  } else if (str == QStringLiteral("Tiff")) {
    return FileFormat::Tiff;
  } else if (str == QStringLiteral("Vaa3DRaw")) {
    return FileFormat::Vaa3DRaw;
  } else if (str == QStringLiteral("ZeissLsm")) {
    return FileFormat::ZeissLsm;
  } else if (str == QStringLiteral("Jpeg")) {
    return FileFormat::Jpeg;
  } else if (str == QStringLiteral("JpegXR")) {
    return FileFormat::JpegXR;
  } else if (str == QStringLiteral("Png")) {
    return FileFormat::Png;
  } else if (str == QStringLiteral("FreeImage")) {
    return FileFormat::FreeImage;
  } else if (str == QStringLiteral("MetaImage")) {
    return FileFormat::MetaImage;
  } else if (str == QStringLiteral("ZeissCZI")) {
    return FileFormat::ZeissCZI;
  } else if (str == QStringLiteral("ITKImage")) {
    return FileFormat::ITKImage;
  } else if (str == QStringLiteral("Leica")) {
    return FileFormat::Leica;
  } else {
    throw ZIOException("invalid FileFormat " + str);
  }
}

inline QString enumToString(Compression m)
{
  switch (m) {
    case Compression::AUTO:
      return QStringLiteral("Auto");
    case Compression::NONE:
      return QStringLiteral("None");
    case Compression::LZW:
      return QStringLiteral("LZW");
    case Compression::JPEG:
      return QStringLiteral("JPEG");
    case Compression::T85:
      return QStringLiteral("T85");
    case Compression::T43:
      return QStringLiteral("T43");
    case Compression::PACKBITS:
      return QStringLiteral("PACKBITS");
    case Compression::DEFLATE:
      return QStringLiteral("DEFLATE");
    case Compression::ADOBE_DEFLATE:
      return QStringLiteral("ADOBE_DEFLATE");
    case Compression::DCS:
      return QStringLiteral("DCS");
    case Compression::JP2000:
      return QStringLiteral("JP2000");
    case Compression::LZMA:
      return QStringLiteral("LZMA");
    case Compression::ZSTD:
      return QStringLiteral("ZSTD");
    case Compression::WEBP:
      return QStringLiteral("WEBP");
    default:
      throw ZIOException("invalid Compression");
  }
}

inline QString enumToString(Dimension d)
{
  switch (d) {
    case Dimension::X:
      return QStringLiteral("X");
    case Dimension::Y:
      return QStringLiteral("Y");
    case Dimension::Z:
      return QStringLiteral("Z");
    case Dimension::C:
      return QStringLiteral("C");
    case Dimension::T:
      return QStringLiteral("T");
    default:
      throw ZIOException("invalid Dimension");
  }
}

inline Dimension stringToDimension(const QString& str)
{
  if (str == QStringLiteral("X")) {
    return Dimension::X;
  } else if (str == QStringLiteral("Y")) {
    return Dimension::Y;
  } else if (str == QStringLiteral("Z")) {
    return Dimension::Z;
  } else if (str == QStringLiteral("C")) {
    return Dimension::C;
  } else if (str == QStringLiteral("T")) {
    return Dimension::T;
  } else {
    throw ZIOException("invalid Dimension " + str);
  }
}

inline QString enumToString(ImgMergeMode m)
{
  switch (m) {
    case ImgMergeMode::Max:
      return QStringLiteral("Max");
    case ImgMergeMode::Min:
      return QStringLiteral("Min");
    case ImgMergeMode::Mean:
      return QStringLiteral("Mean");
    case ImgMergeMode::Median:
      return QStringLiteral("Median");
    case ImgMergeMode::First:
      return QStringLiteral("First");
    default:
      throw ZIOException("invalid ImgMergeMode");
  }
}

inline ImgMergeMode stringToImgMergeMode(const QString& str)
{
  if (str == QStringLiteral("Max")) {
    return ImgMergeMode::Max;
  } else if (str == QStringLiteral("Min")) {
    return ImgMergeMode::Min;
  } else if (str == QStringLiteral("Mean")) {
    return ImgMergeMode::Mean;
  } else if (str == QStringLiteral("Median")) {
    return ImgMergeMode::Median;
  } else if (str == QStringLiteral("First")) {
    return ImgMergeMode::First;
  } else {
    throw ZIOException("invalid ImgMergeMode " + str);
  }
}
#endif

}  // namespace nim
