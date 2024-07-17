#include "zimginterface.h"

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

} // namespace nim
