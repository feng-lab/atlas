#pragma once

#include "zimg.h"
#include "zimgalgorithm.h"

namespace nim {

template<bool ReportProgress = false>
class ZImgAutoThreshold : public ZImgAlgorithm<ReportProgress>
{
public:
  // ting's triangle auto threshold
  // threshold is calculated as img voxel type and returned as TValue because we don't know img type
  // throw ZException for empty img and wrong channel or location
  // *note* this function assume that float img range is [0.0 1.0]
  template<typename TValue = double>
  TValue triangleThre(const ZImg& img, size_t c = 0, size_t t = 0)
  {
    return imgTypeDispatcher(img.info(), [&]<typename TVoxel>() {
      return static_cast<TValue>(typedTriangleThre<TVoxel>(img, c, t));
    });
  }

  // for big image
  // no mask if mask is empty
  template<typename TValue = double>
  TValue triangleThre(const QString& filename,
                      size_t c = 0,
                      size_t t = 0,
                      size_t scene = 0,
                      const std::vector<ZVoxelCoordinate>& mask = std::vector<ZVoxelCoordinate>())
  {
    std::vector<ZImgInfo> infos = ZImg::readImgInfos(filename);
    if (scene >= infos.size()) {
      throw ZException("input scene incorrect");
    }
    return imgTypeDispatcher(infos[scene], [&]<typename TVoxel>() {
      return static_cast<TValue>(typedTriangleThre<TVoxel>(filename, c, t, scene, mask));
    });
  }

  // in case you already know img type, call this version and pass type as template parameter
  // throw ZException if passed type don't match voxel type of imgIn
  template<typename TVoxel = double>
  TVoxel typedTriangleThre(const ZImg& imgIn, size_t c = 0, size_t t = 0);

  template<typename TVoxel = double>
  TVoxel typedTriangleThre(const QString& filename,
                           size_t c = 0,
                           size_t t = 0,
                           size_t scene = 0,
                           const std::vector<ZVoxelCoordinate>& mask = std::vector<ZVoxelCoordinate>());

  // convert img to uint8_t with minValue and maxValue then threshold
  // minValue and maxValue are not used if img is uint8_t
  uint8_t u8TriangleThre(const QString& filename,
                         double minValue,
                         double maxValue,
                         size_t c = 0,
                         size_t t = 0,
                         size_t scene = 0,
                         const std::vector<ZVoxelCoordinate>& mask = std::vector<ZVoxelCoordinate>());

  template<typename TValue = double>
  TValue centroidThre(double& cent1, double& cent2, const ZImg& img, size_t c = 0, size_t t = 0)
  {
    return imgTypeDispatcher(img.info(), [&]<typename TVoxel>() {
      return static_cast<TValue>(typedCentroidThre<TVoxel>(cent1, cent2, img, c, t));
    });
  }

  template<typename TVoxel = double>
  TVoxel typedCentroidThre(double& cent1, double& cent2, const ZImg& imgIn, size_t c = 0, size_t t = 0);

  template<typename TValue = double>
  TValue maxHistThre(const ZImg& img, size_t c = 0, size_t t = 0)
  {
    return imgTypeDispatcher(img.info(), [&]<typename TVoxel>() {
      return static_cast<TValue>(typedMaxHistThre<TVoxel>(img, c, t));
    });
  }

  template<typename TVoxel = double>
  TVoxel typedMaxHistThre(const ZImg& imgIn, size_t c = 0, size_t t = 0);

private:
  // assume hist is not empty
  void histNonZeroRange(std::vector<size_t>& hist, size_t& low, size_t& high);
};

} // namespace nim
