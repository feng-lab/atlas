#include "zimginterface.h"

#include <magic_enum.hpp>
#include <map>

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

template<>
Compression stringToEnum<Compression>(std::string_view s)
{
  static const std::map<std::string_view, Compression> compressionMap = {
    {"AUTO",          Compression::AUTO},
    {"NONE",          Compression::NONE},
    {"LZW",           Compression::LZW},
    {"JPEG",          Compression::JPEG},
    {"T85",           Compression::T85},
    {"T43",           Compression::T43},
    {"PACKBITS",      Compression::PACKBITS},
    {"DEFLATE",       Compression::DEFLATE},
    {"ADOBE_DEFLATE", Compression::ADOBE_DEFLATE},
    {"DCS",           Compression::DCS},
    {"JP2000",        Compression::JP2000},
    {"LZMA",          Compression::LZMA},
    {"ZSTD",          Compression::ZSTD},
    {"WEBP",          Compression::WEBP},
  };
  auto it = compressionMap.find(s);
  if (it == compressionMap.end()) {
    throw ZIOException(fmt::format("invalid Compression string: {}", s));
  }
  return it->second;
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

}  // namespace nim
