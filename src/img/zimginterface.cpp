#include "zimginterface.h"

#include <reflect>
#include <unordered_map>

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
  throw ZException(fmt::format("Invalid DataType {}", std::to_underlying(dt)));
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
  throw ZException(fmt::format("Invalid VoxelSizeUnit {}", std::to_underlying(vsu)));
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

template<typename TEnum>
TEnum stringToEnum(std::string_view s)
{
  static_assert(std::is_enum_v<std::remove_cvref_t<TEnum>>, "Need Enum Type");
  static constexpr auto enumerators =
    reflect::enumerators<TEnum, reflect::enum_min(TEnum{}), reflect::enum_max(TEnum{})>;
  for (size_t i = 0; i < enumerators.size(); ++i) {
    if (s == enumerators[i].second) {
      return static_cast<TEnum>(enumerators[i].first);
    }
  }
  throw ZIOException(fmt::format("invalid enum string: {}", s));
}

template std::string_view enumToString<DataType>(DataType);
template std::string_view enumToString<VoxelFormat>(VoxelFormat);
template std::string_view enumToString<VoxelSizeUnit>(VoxelSizeUnit);
template std::string_view enumToString<FileFormat>(FileFormat);
template std::string_view enumToString<Compression>(Compression);
template std::string_view enumToString<PadOption>(PadOption);
template std::string_view enumToString<Interpolant>(Interpolant);
template std::string_view enumToString<Dimension>(Dimension);
template std::string_view enumToString<ImgMergeMode>(ImgMergeMode);

template DataType stringToEnum<DataType>(std::string_view);
template VoxelFormat stringToEnum<VoxelFormat>(std::string_view);
template VoxelSizeUnit stringToEnum<VoxelSizeUnit>(std::string_view);
template FileFormat stringToEnum<FileFormat>(std::string_view);
template Compression stringToEnum<Compression>(std::string_view);
template PadOption stringToEnum<PadOption>(std::string_view);
template Interpolant stringToEnum<Interpolant>(std::string_view);
template Dimension stringToEnum<Dimension>(std::string_view);
template ImgMergeMode stringToEnum<ImgMergeMode>(std::string_view);

} // namespace nim
