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
  // throw ZImgException for empty img and wrong channel or location
  // *note* this function assume that float img range is [0.0 1.0]
  template<typename TValue = double>
  TValue triangleThre(const ZImg& img, size_t c = 0, size_t t = 0)
  {
    IMG_RETURN_TYPED_CALL(typedTriangleThre, img.info(), img, c, t)
    return 0;
  }

  // for big image
  // no mask if mask is empty
  template<typename TValue = double>
  TValue triangleThre(const QString& filename, size_t c = 0, size_t t = 0, size_t scene = 0,
                      const std::vector<ZVoxelCoordinate>& mask = std::vector<ZVoxelCoordinate>())
  {
    std::vector<ZImgInfo> infos = ZImg::readImgInfos(filename);
    if (scene >= infos.size()) {
      throw ZImgException("input scene incorrect");
    }
    IMG_RETURN_TYPED_CALL(typedTriangleThre, infos[scene], filename, c, t, scene, mask)
    return 0;
  }

  // in case you already know img type, call this version and pass type as template parameter
  // throw ZImgException if passed type don't match voxel type of imgIn
  template<typename TVoxel = double>
  TVoxel typedTriangleThre(const ZImg& imgIn, size_t c = 0, size_t t = 0);

  template<typename TVoxel = double>
  TVoxel typedTriangleThre(const QString& filename, size_t c = 0, size_t t = 0, size_t scene = 0,
                           const std::vector<ZVoxelCoordinate>& mask = std::vector<ZVoxelCoordinate>());

  // convert img to uint8_t with minValue and maxValue then threshold
  // minValue and maxValue are not used if img is uint8_t
  uint8_t u8TriangleThre(const QString& filename, double minValue, double maxValue,
                         size_t c = 0, size_t t = 0, size_t scene = 0,
                         const std::vector<ZVoxelCoordinate>& mask = std::vector<ZVoxelCoordinate>());

  template<typename TValue = double>
  TValue centroidThre(double& cent1, double& cent2, const ZImg& img, size_t c = 0, size_t t = 0)
  {
    IMG_RETURN_TYPED_CALL(typedCentroidThre, img.info(), cent1, cent2, img, c, t)
    return 0;
  }

  template<typename TVoxel = double>
  TVoxel typedCentroidThre(double& cent1, double& cent2, const ZImg& imgIn, size_t c = 0, size_t t = 0);

  template<typename TValue = double>
  TValue maxHistThre(const ZImg& img, size_t c = 0, size_t t = 0)
  {
    IMG_RETURN_TYPED_CALL(typedMaxHistThre, img.info(), img, c, t)
    return 0;
  }

  template<typename TVoxel = double>
  TVoxel typedMaxHistThre(const ZImg& imgIn, size_t c = 0, size_t t = 0);

private:
  // assume hist is not empty
  void histNonZeroRange(std::vector<size_t>& hist, size_t& low, size_t& high);

};

} // namespace nim

