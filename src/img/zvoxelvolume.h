#pragma once

#include <cstddef>
#include <cstdint>

namespace nim {

// Minimal read-only 3D voxel volume interface used by tracing codepaths that need to operate on
// disk-cached / larger-than-RAM datasets. This is intentionally small: the legacy neuTube tracing
// algorithm mostly needs random access reads, dimensions, and a stable notion of voxel type.
enum class ZVoxelValueType
{
  Uint8,
  Uint16,
  Float32,
  Float64,
};

class ZVoxelVolume
{
public:
  virtual ~ZVoxelVolume() = default;

  [[nodiscard]] virtual bool isEmpty() const = 0;

  [[nodiscard]] virtual size_t width() const = 0;
  [[nodiscard]] virtual size_t height() const = 0;
  [[nodiscard]] virtual size_t depth() const = 0;

  // Physical voxel spacing in the source dataset coordinate system.
  // When unknown or not applicable (e.g. masks), implementations should return 1.0.
  [[nodiscard]] virtual double voxelSizeX() const = 0;
  [[nodiscard]] virtual double voxelSizeY() const = 0;
  [[nodiscard]] virtual double voxelSizeZ() const = 0;

  [[nodiscard]] virtual ZVoxelValueType valueType() const = 0;

  // Returns the voxel value as double.
  // Out-of-bounds coordinates must return 0.0 (legacy code often treats OOB as background).
  [[nodiscard]] virtual double valueAsDouble(int x, int y, int z) const = 0;
};

// Minimal writeable mask interface used by tracing codepaths.
//
// Values are interpreted as uint8 (e.g. binary occupancy). This intentionally avoids
// uint16 "label image" semantics; if we ever need large label maps in the future,
// they should use a separate data structure rather than an object named "*Mask".
class ZVoxelMaskMutable : public ZVoxelVolume
{
public:
  // Sets a voxel value (uint8 semantics). Out-of-bounds writes are ignored.
  virtual void setValueU8(int x, int y, int z, std::uint8_t value) = 0;

  // Clears the entire volume to `value` (typically 0). Implementations may drop internal storage.
  virtual void clearU8(std::uint8_t value) = 0;
};

} // namespace nim
