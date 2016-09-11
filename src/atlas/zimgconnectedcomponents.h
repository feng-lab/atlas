#pragma once

#include "zimg.h"
#include "zimgalgorithm.h"

namespace nim {

struct ConnComp
{
  ConnComp();

  // what connectivity we use for processing
  size_t connectivity;
  // info of original img
  ZImgInfo imgInfo;
  // ConnComp process one channel at a time, c, t and l
  // record which part of img we are processing
  size_t channel;
  size_t time;
  // voxelIdxList.size() is number of connected components (objects) we found
  // kth vector contains the linear indices of the voxels in the kth object
  std::vector<std::vector<size_t>> voxelIdxList;

  void clear();

  // remove all objects that has size < or <= (if includeThre is true) sizeThre
  void removeSmallObject(size_t sizeThre, bool includeThre = true);

  size_t toatalNumVoxels() const;

  size_t labelImgBytesPerVoxel() const;

  // return a label img with smallest possible voxel type, use labelImgBytesPerVoxel to get type
  ZImg createLabelImg() const;

  // user decide what type
  template<typename TVoxel>
  ZImg createTypedLabelImg() const;
};

template<bool ReportProgress = false>
class ZImgConnectedComponents : public ZImgAlgorithm<ReportProgress>
{
public:
  ZImgConnectedComponents() = default;

  // input can be any type of img, all > 0 voxel are foreground
  // conn can be 4, 8 (2D) or 6, 18, 26 (3D)
  // if conn == 0, uses a default connectivity of 8 for two dimensions, 26 for three dimensions
  ConnComp run(const ZImg& img, size_t conn = 0, size_t c = 0, size_t t = 0);

  // input can be any type of img, it will be binarized by GenericForegroundPredictor first
  // conn can be 4, 8 (2D) or 6, 18, 26 (3D)
  // if conn == 0, uses a default connectivity of 8 for two dimensions, 26 for three dimensions
  template<typename GenericForegroundPredictor>
  ConnComp run(const ZImg& img, size_t conn, size_t c, size_t t,
               const GenericForegroundPredictor& isForeground)
  {
    ConnComp res = createRes(img, conn, c, t);
    ZImg bimg = img.createView(c, t).binarized(isForeground);

    getConnectedComponents_Impl(bimg, res, 1);

    return res;
  }

  // input can be any type, voxel equal to label are foreground, default label is 1
  // conn can be 4, 8 (2D) or 6, 18, 26 (3D)
  // if conn == 0, uses a default connectivity of 8 for two dimensions, 26 for three dimensions
  ConnComp runLabel(const ZImg& img, size_t conn = 0, size_t label = 1, size_t c = 0, size_t t = 0);

  // input can be any type, voxel equal to label are foreground, default label is 1
  // **note** input will be modified if it is uint8_t type, this version can save some memory space
  // conn can be 4, 8 (2D) or 6, 18, 26 (3D)
  // if conn == 0, uses a default connectivity of 8 for two dimensions, 26 for three dimensions
  ConnComp runLabelModifyInput(ZImg& img, size_t conn = 0, size_t label = 1, size_t c = 0, size_t t = 0);

private:
  ConnComp createRes(const ZImg& img, size_t conn, size_t c, size_t t) const;

  void getConnectedComponents_Impl(ZImg& markerImg, ConnComp& res, size_t label);
};

} // namespace nim

