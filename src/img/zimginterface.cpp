#include "zimginterface.h"

namespace nim {

template<> const char* EnumStrings<DataType>::data[] = {
  "0", "Byte", "Ascii", "Short", "Long", "Rational", "SByte", "Undefined", "SShort", "SLong",
  "SRational", "Float", "Double", "IFD", "14", "15", "Long8", "SLong8", "IFD8"
};

template<> const char* EnumStrings<VoxelFormat>::data[] = {
  "0", "Unsigned", "Signed", "Float"
};

template<> const char* EnumStrings<VoxelSizeUnit>::data[] = {
  "none", "inch", "cm", "mm", "um", "nm", "m", "hm", "km"
};

template<> const char* EnumStrings<Dimension>::data[] = {
  "X", "Y", "Z", "C", "T"
};

size_t byteNumber(DataType dt)
{
  switch (dt) {
  case DataType::Byte:
    return 1;
  case DataType::Ascii:
    return 1;
  case DataType::Short:
    return 2;
  case DataType::Long:
    return 4;
  case DataType::Rational:
    return 8;
  case DataType::SByte:
    return 1;
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
    return 8;
  case DataType::SLong8:
    return 8;
  case DataType::IFD8:
    return 8;
  default:
    throw ZException(QString("Invalid DataType %1").arg(enumToUnderlyingType(dt)));
    break;
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
    break;
  }
}

}
