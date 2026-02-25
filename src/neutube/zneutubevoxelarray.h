#pragma once

#include "zneutubevoxel.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nim {
class ZImg;
}

namespace nim::neutube {

class ZNeutubeVoxelArray
{
public:
  using TVector = std::vector<ZNeutubeVoxel>;

  void append(const ZNeutubeVoxel& voxel)
  {
    m_voxels.push_back(voxel);
  }

  void append(int x, int y, int z, double value)
  {
    m_voxels.emplace_back(x, y, z, value);
  }

  void prepend(const ZNeutubeVoxel& voxel)
  {
    m_voxels.insert(m_voxels.begin(), voxel);
  }

  void addValue(double delta);

  void multiplyValue(double a);

  void minimizeValue(double v);

  void clear()
  {
    m_voxels.clear();
  }

  [[nodiscard]] size_t size() const
  {
    return m_voxels.size();
  }

  [[nodiscard]] bool isEmpty() const
  {
    return m_voxels.empty();
  }

  [[nodiscard]] const TVector& data() const
  {
    return m_voxels;
  }

  TVector& data()
  {
    return m_voxels;
  }

  void sample(const ZImg& img);

  void sample(const ZImg& img, double (*f)(double));

  void labelImgWithBall(ZImg& img, uint8_t label) const;

private:
  TVector m_voxels;
};

} // namespace nim::neutube
