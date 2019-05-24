#pragma once

#include "zimg.h"
#include "zeigenutils.h"
#include <QPolygonF>
#include <QPainterPath>
#include <tuple>

namespace nim {

class ZROIUtils
{
public:
  static QPainterPath splineToQPainterPath(const QPolygonF& spline, bool showLastSeg = true);

  // return tight mask, x_start, y_start in which mask could be empty
  static std::tuple<ZImg, size_t, size_t> qPainterPathToMask(const QPainterPath& path);

  inline static std::tuple<ZImg, size_t, size_t> splineToMask(const QPolygonF& spline)
  {
    return qPainterPathToMask(splineToQPainterPath(spline));
  }

  inline static QPainterPath rectToQPainterPath(const QPolygonF& rect)
  {
    CHECK(rect.size() == 2);
    QPainterPath path;
    path.addRect(QRectF(rect[0], rect[1]));
    return path;
  }

  inline static std::tuple<ZImg, size_t, size_t> rectToMask(const QPolygonF& rect)
  {
    return qPainterPathToMask(rectToQPainterPath(rect));
  }

  inline static QPainterPath ellipseToQPainterPath(const QPolygonF& ellipse)
  {
    CHECK(ellipse.size() == 2);
    QPainterPath path;
    path.addEllipse(QRectF(ellipse[0], ellipse[1]));
    return path;
  }

  inline static std::tuple<ZImg, size_t, size_t> ellipseToMask(const QPolygonF& ellipse)
  {
    return qPainterPathToMask(ellipseToQPainterPath(ellipse));
  }

  inline static QPainterPath polygonToQPainterPath(const QPolygonF& poly)
  {
    CHECK(poly.isClosed());
    QPainterPath path;
    path.addPolygon(poly);
    return path;
  }

  inline static std::tuple<ZImg, size_t, size_t> polygonToMask(const QPolygonF& poly)
  {
    return qPainterPathToMask(polygonToQPainterPath(poly));
  }

// for python
  using RowMatrixXd = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  using RowMatrixXu8 = Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  using RowMatrixXb = Eigen::Matrix<bool, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  using EigenDRef = Eigen::Ref<const Eigen::MatrixXd, 0, Eigen::Stride<Eigen::Dynamic, Eigen::Dynamic>>;

// return tight mask, x_start, y_start in which mask could be empty
  static std::tuple<RowMatrixXb, size_t, size_t> qPainterPathToMask_Python(const QPainterPath& path);

  inline static RowMatrixXd polyToMat(const QPolygonF& poly)
  {
    RowMatrixXd res(poly.size(), 2);
    for (int i = 0; i < poly.size(); ++i) {
      res(i, 0) = poly[i].x();
      res(i, 1) = poly[i].y();
    }
    return res;
  }

  inline static QPolygonF matToPoly(const EigenDRef& mat)
  {
    QPolygonF res;
    if (mat.rows() == 0 || mat.cols() == 0)
      return res;
    CHECK(mat.cols() == 2);
    res = QPolygonF(mat.rows());
    for (Eigen::Index r = 0; r < mat.rows(); ++r) {
      res[r].setX(mat(r, 0));
      res[r].setY(mat(r, 1));
    }
    return res;
  }

  inline static std::tuple<RowMatrixXb, size_t, size_t> splineToMask_Python(const EigenDRef& spline)
  {
    return qPainterPathToMask_Python(splineToQPainterPath(matToPoly(spline)));
  }

  inline static std::tuple<RowMatrixXb, size_t, size_t> rectToMask_Python(const EigenDRef& rect)
  {
    return qPainterPathToMask_Python(rectToQPainterPath(matToPoly(rect)));
  }

  inline static std::tuple<RowMatrixXb, size_t, size_t> ellipseToMask_Python(const EigenDRef& ellipse)
  {
    return qPainterPathToMask_Python(ellipseToQPainterPath(matToPoly(ellipse)));
  }

  inline static std::tuple<RowMatrixXb, size_t, size_t> polygonToMask_Python(const EigenDRef& poly)
  {
    return qPainterPathToMask_Python(polygonToQPainterPath(matToPoly(poly)));
  }

};

} // namespace nim


