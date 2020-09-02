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
  static std::tuple<ZImg, int32_t, int32_t> qPainterPathToMask(const QPainterPath& path);

  static std::tuple<ZImg, int32_t, int32_t> qPainterPathToStroke(const QPainterPath& path, double width = 2.);

  inline static std::tuple<ZImg, int32_t, int32_t> splineToMask(const QPolygonF& spline)
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

  inline static std::tuple<ZImg, int32_t, int32_t> rectToMask(const QPolygonF& rect)
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

  inline static std::tuple<ZImg, int32_t, int32_t> ellipseToMask(const QPolygonF& ellipse)
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

  inline static std::tuple<ZImg, int32_t, int32_t> polygonToMask(const QPolygonF& poly)
  {
    return qPainterPathToMask(polygonToQPainterPath(poly));
  }

// for python
  using RowMatrixXd = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  using RowMatrixXu8 = Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  using RowMatrixXb = Eigen::Matrix<bool, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  using EigenDRef = Eigen::Ref<const RowMatrixXd, 0, Eigen::Stride<Eigen::Dynamic, Eigen::Dynamic>>;

// return tight mask, x_start, y_start in which mask could be empty
  // static std::tuple<RowMatrixXb, int32_t, int32_t> qPainterPathToMask_Python(const QPainterPath& path);

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
    CHECK(mat.cols() == 2) << mat.rows() << " " << mat.cols();
    res = QPolygonF(mat.rows());
    for (Eigen::Index r = 0; r < mat.rows(); ++r) {
      res[r].setX(mat(r, 0));
      res[r].setY(mat(r, 1));
    }
    return res;
  }

#if 0
  inline static std::tuple<RowMatrixXb, int32_t, int32_t> splineToMask_Python(const EigenDRef& spline)
  {
    return qPainterPathToMask_Python(splineToQPainterPath(matToPoly(spline)));
  }

  inline static std::tuple<RowMatrixXb, int32_t, int32_t> rectToMask_Python(const EigenDRef& rect)
  {
    return qPainterPathToMask_Python(rectToQPainterPath(matToPoly(rect)));
  }

  inline static std::tuple<RowMatrixXb, int32_t, int32_t> ellipseToMask_Python(const EigenDRef& ellipse)
  {
    return qPainterPathToMask_Python(ellipseToQPainterPath(matToPoly(ellipse)));
  }

  inline static std::tuple<RowMatrixXb, int32_t, int32_t> polygonToMask_Python(const EigenDRef& poly)
  {
    return qPainterPathToMask_Python(polygonToQPainterPath(matToPoly(poly)));
  }
#else
  inline static std::tuple<ZImg, int32_t, int32_t> splineToMask_Python(const EigenDRef& spline)
  {
    return qPainterPathToMask(splineToQPainterPath(matToPoly(spline)));
  }

  inline static std::tuple<ZImg, int32_t, int32_t> rectToMask_Python(const EigenDRef& rect)
  {
    return qPainterPathToMask(rectToQPainterPath(matToPoly(rect)));
  }

  inline static std::tuple<ZImg, int32_t, int32_t> ellipseToMask_Python(const EigenDRef& ellipse)
  {
    return qPainterPathToMask(ellipseToQPainterPath(matToPoly(ellipse)));
  }

  inline static std::tuple<ZImg, int32_t, int32_t> polygonToMask_Python(const EigenDRef& poly)
  {
    return qPainterPathToMask(polygonToQPainterPath(matToPoly(poly)));
  }

  inline static std::tuple<ZImg, int32_t, int32_t> shapeToMask_Python(const std::vector<std::tuple<EigenDRef, std::string, bool>>& shapeOps)
  {
    QPainterPath pp;
    for (const auto&[points, type, isAdd] : shapeOps) {
      QPainterPath subpp;
      auto poly = matToPoly(points);
      if (type == "Rect") {
        subpp = rectToQPainterPath(poly);
      } else if (type == "Ellipse") {
        subpp = ellipseToQPainterPath(poly);
      } else if (type == "Polygon") {
        subpp = polygonToQPainterPath(poly);
      } else if (type == "Spline") {
        subpp = splineToQPainterPath(poly);
      } else if (type == "Line") {
        CHECK(shapeOps.size() == 1);
        return qPainterPathToStroke(splineToQPainterPath(poly));
      }
      if (isAdd) {
        pp += subpp;
      } else {
        pp -= subpp;
      }
    }
    return qPainterPathToMask(pp);
  }
#endif

};

} // namespace nim


