#pragma once

#include "zimg.h"
#include "zroitypes.h"
#include "zroimaskrasterizer.h"
#include <QPolygonF>
#include <QPainterPath>
#include <string>
#include <tuple>
#include <vector>

namespace nim {

class ZROIUtils
{
public:
  static QPainterPath splineToQPainterPath(const QPolygonF& spline, bool showLastSeg = true);

  // Rasterize a single ROI shape (add/sub operations) into a tight binary mask using the
  // scanline-based ZROIMaskRasterizer backend.
  [[nodiscard]] static std::tuple<ZImg, index_t, index_t>
  shapeToMask(const std::vector<ZROIShapeOperation>& shapeOps, const ZROIMaskRasterizerSettings& settings);

  // Rasterize a filled QPainterPath into a tight binary mask.
  //
  // Note: this uses the fast scanline-based ZROIMaskRasterizer backend (via polygons extracted
  // from the QPainterPath) and may differ slightly from Qt's QPainter/QImage rasterization at edges.
  static std::tuple<ZImg, index_t, index_t> qPainterPathToMaskMR(const QPainterPath& path);

  // Reference implementation that rasterizes via Qt (QPainter -> QImage::Format_Mono).
  static std::tuple<ZImg, index_t, index_t> qPainterPathToMaskQt(const QPainterPath& path);

  static std::tuple<ZImg, index_t, index_t> qPainterPathToMask(const QPainterPath& path)
  {
    return qPainterPathToMaskMR(path);
  }

  static std::tuple<ZImg, index_t, index_t> qPainterPathToStroke(const QPainterPath& path, double width = 2.);

  static std::tuple<ZImg, index_t, index_t> splineToMask(const QPolygonF& spline)
  {
    return qPainterPathToMask(splineToQPainterPath(spline));
  }

  static QPainterPath rectToQPainterPath(const QPolygonF& rect)
  {
    CHECK(rect.size() == 2);
    QPainterPath path;
    path.addRect(QRectF(rect[0], rect[1]));
    return path;
  }

  static std::tuple<ZImg, index_t, index_t> rectToMask(const QPolygonF& rect)
  {
    return qPainterPathToMask(rectToQPainterPath(rect));
  }

  static QPainterPath ellipseToQPainterPath(const QPolygonF& ellipse)
  {
    CHECK(ellipse.size() == 2);
    QPainterPath path;
    path.addEllipse(QRectF(ellipse[0], ellipse[1]));
    return path;
  }

  static std::tuple<ZImg, index_t, index_t> ellipseToMask(const QPolygonF& ellipse)
  {
    return qPainterPathToMask(ellipseToQPainterPath(ellipse));
  }

  static QPainterPath polygonToQPainterPath(const QPolygonF& poly)
  {
    CHECK(poly.isClosed());
    QPainterPath path;
    path.addPolygon(poly);
    return path;
  }

  static std::tuple<ZImg, index_t, index_t> polygonToMask(const QPolygonF& poly)
  {
    return qPainterPathToMask(polygonToQPainterPath(poly));
  }

  static std::tuple<ZImg, index_t, index_t>
  shapeToMask(const std::vector<std::tuple<QPolygonF, std::string, bool>>& shapeOps)
  {
    QPainterPath pp;
    for (const auto& [poly, type, isAdd] : shapeOps) {
      QPainterPath subpp;
      if (type == "Rect") {
        subpp = rectToQPainterPath(poly);
      } else if (type == "Ellipse") {
        subpp = ellipseToQPainterPath(poly);
      } else if (type == "Polygon") {
        subpp = polygonToQPainterPath(poly);
      } else if (type == "Spline") {
        subpp = splineToQPainterPath(poly);
      } else if (type == "Line") {
        if (shapeOps.size() == 1) {
          return qPainterPathToStroke(splineToQPainterPath(poly));
        }
      }
      if (isAdd) {
        pp += subpp;
      } else {
        pp -= subpp;
      }
    }
    return qPainterPathToMask(pp);
  }
};

} // namespace nim
