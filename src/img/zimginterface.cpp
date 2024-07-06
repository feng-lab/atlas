#include "zimginterface.h"

#define REFLECT_ENUM_MIN (-1)
#define REFLECT_ENUM_MAX 100
#include <reflect>
#include <map>

namespace nim {

ZImgGlobal& ZImgGlobal::instance()
{
  static ZImgGlobal singleInstance;
  return singleInstance;
}

size_t byteNumber(DataType dt)
{
  static const std::unordered_map<DataType, size_t> dataTypeToByteNumber = {
    {DataType::Byte,      1},
    {DataType::Ascii,     1},
    {DataType::Short,     2},
    {DataType::Long,      4},
    {DataType::Rational,  8},
    {DataType::SByte,     1},
    {DataType::Undefined, 1},
    {DataType::SShort,    2},
    {DataType::SLong,     4},
    {DataType::SRational, 8},
    {DataType::Float,     4},
    {DataType::Double,    8},
    {DataType::IFD,       4},
    {DataType::Long8,     8},
    {DataType::SLong8,    8},
    {DataType::IFD8,      8}
  };

  auto it = dataTypeToByteNumber.find(dt);
  if (it != dataTypeToByteNumber.end()) {
    return it->second;
  }
  throw ZException(QString("Invalid DataType %1").arg(std::to_underlying(dt)));
}

double unitSizeInMeter(VoxelSizeUnit vsu)
{
  static const std::unordered_map<VoxelSizeUnit, double> voxelSizeUnitToMeter = {
    {VoxelSizeUnit::none, 1     },
    {VoxelSizeUnit::inch, 0.0254},
    {VoxelSizeUnit::cm,   1e-2  },
    {VoxelSizeUnit::mm,   1e-3  },
    {VoxelSizeUnit::um,   1e-6  },
    {VoxelSizeUnit::nm,   1e-9  },
    {VoxelSizeUnit::m,    1     },
    {VoxelSizeUnit::hm,   1e2   },
    {VoxelSizeUnit::km,   1e3   }
  };

  auto it = voxelSizeUnitToMeter.find(vsu);
  if (it != voxelSizeUnitToMeter.end()) {
    return it->second;
  }
  throw ZException(QString("Invalid VoxelSizeUnit %1").arg(std::to_underlying(vsu)));
}

template<typename TEnum>
std::string_view enumToString(TEnum e)
{
  static_assert(std::is_enum_v<std::remove_cvref_t<TEnum>>, "Need Enum Type");
  auto res = reflect::enum_name(e);
  if (res.empty()) {
    throw ZIOException(fmt::format("invalid enum value: {}", std::to_underlying(e)));
  }
  return res;
}

template<class E>
  requires(std::is_enum_v<E>)
constexpr auto enumNameValue = []() {
  constexpr auto enumCases = reflect::detail::enum_cases<E, reflect::enum_min(E{}), reflect::enum_max(E{})>;
  reflect::detail::static_vector<std::tuple<std::string_view, E>, enumCases.size_> nameValue{};
  for (size_t i = 0; i < enumCases.size_; ++i) {
    nameValue.push_back(
      std::make_tuple(reflect::enum_name(static_cast<E>(enumCases[i])), static_cast<E>(enumCases[i])));
  }
  return nameValue;
}();

template<typename TEnum>
TEnum stringToEnum(std::string_view s)
{
  static_assert(std::is_enum_v<std::remove_cvref_t<TEnum>>, "Need Enum Type");
  static constexpr auto enumNameValues = enumNameValue<TEnum>;
  for (size_t i = 0; i < enumNameValues.size_; ++i) {
    if (s == std::get<0>(enumNameValues[i])) {
      return std::get<1>(enumNameValues[i]);
    }
  }
  throw ZIOException(fmt::format("invalid enum string: {}", s));
}

template<>
std::string_view enumToString<Compression>(Compression e)
{
  static const std::unordered_map<Compression, std::string_view> compressionToStringMap = {
    {Compression::AUTO,          "AUTO"         },
    {Compression::NONE,          "NONE"         },
    {Compression::LZW,           "LZW"          },
    {Compression::JPEG,          "JPEG"         },
    {Compression::T85,           "T85"          },
    {Compression::T43,           "T43"          },
    {Compression::PACKBITS,      "PACKBITS"     },
    {Compression::DEFLATE,       "DEFLATE"      },
    {Compression::ADOBE_DEFLATE, "ADOBE_DEFLATE"},
    {Compression::DCS,           "DCS"          },
    {Compression::JP2000,        "JP2000"       },
    {Compression::LZMA,          "LZMA"         },
    {Compression::ZSTD,          "ZSTD"         },
    {Compression::WEBP,          "WEBP"         },
    {Compression::JPEGXR,        "JPEGXR"       }
  };

  auto it = compressionToStringMap.find(e);
  if (it != compressionToStringMap.end()) {
    return it->second;
  }
  throw ZIOException(fmt::format("invalid Compression: {}", std::to_underlying(e)));
}

template<>
Compression stringToEnum<Compression>(std::string_view s)
{
  static const std::unordered_map<std::string_view, Compression> stringToCompressionMap = {
    {"AUTO",          Compression::AUTO         },
    {"NONE",          Compression::NONE         },
    {"LZW",           Compression::LZW          },
    {"JPEG",          Compression::JPEG         },
    {"T85",           Compression::T85          },
    {"T43",           Compression::T43          },
    {"PACKBITS",      Compression::PACKBITS     },
    {"DEFLATE",       Compression::DEFLATE      },
    {"ADOBE_DEFLATE", Compression::ADOBE_DEFLATE},
    {"DCS",           Compression::DCS          },
    {"JP2000",        Compression::JP2000       },
    {"LZMA",          Compression::LZMA         },
    {"ZSTD",          Compression::ZSTD         },
    {"WEBP",          Compression::WEBP         },
    {"JPEGXR",        Compression::JPEGXR       },
  };
  auto it = stringToCompressionMap.find(s);
  if (it != stringToCompressionMap.end()) {
    return it->second;
  }
  throw ZIOException(fmt::format("invalid Compression string: {}", s));
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

} // namespace nim
