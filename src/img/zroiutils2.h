#pragma once

#include "zimg.h"
#include "zeigenutils.h"
#include <tuple>

namespace nim {

class ZROIUtils2
{
public:
// for python
  using RowMatrixXd = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  using RowMatrixXu8 = Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  using RowMatrixXb = Eigen::Matrix<bool, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  using EigenDRef = Eigen::Ref<const RowMatrixXd, 0, Eigen::Stride<Eigen::Dynamic, Eigen::Dynamic>>;

// return tight mask, x_start, y_start in which mask could be empty
  static std::tuple<ZImg, index_t, index_t> splineToMask_Python(const EigenDRef& spline);

  static std::tuple<ZImg, index_t, index_t> rectToMask_Python(const EigenDRef& rect);

  static std::tuple<ZImg, index_t, index_t> ellipseToMask_Python(const EigenDRef& ellipse);

  static std::tuple<ZImg, index_t, index_t> polygonToMask_Python(const EigenDRef& poly);

  static std::tuple<ZImg, index_t, index_t>
  shapeToMask_Python(const std::vector<std::tuple<EigenDRef, std::string, bool>>& shapeOps);
};

} // namespace nim


