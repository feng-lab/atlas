#pragma once

#include "zlog.h"
#include <QPainterPath>
#include <QPolygonF>
#include <QRectF>
#include <cstddef>
#include <utility>

namespace nim {

enum class ROIType
{
  Rect,
  Ellipse,
  Polygon,
  Spline,
  Line
};

struct ZROIShapeOperation
{
  ZROIShapeOperation() = default;

  ZROIShapeOperation(bool isAdd_, ROIType type_, const QRectF& rect)
    : isAdd(isAdd_)
    , type(type_)
  {
    poly.push_back(rect.topLeft());
    poly.push_back(rect.bottomRight());
  }

  ZROIShapeOperation(bool isAdd_, ROIType type_, QPolygonF poly_)
    : isAdd(isAdd_)
    , type(type_)
    , poly(std::move(poly_))
  {
    if (type != ROIType::Line) {
      CHECK(poly.isClosed());
    } else {
      isAdd = true;
    }
  }

  void translate(double x, double y)
  {
    poly.translate(x, y);
  }

  void flipAround(double x, double y, bool hFlip, bool vFlip);

  [[nodiscard]] QRectF rect() const
  {
    CHECK(poly.size() == 2);
    return QRectF(poly[0], poly[1]);
  }

  void setRect(const QRectF& rect)
  {
    poly.clear();
    poly.push_back(rect.topLeft());
    poly.push_back(rect.bottomRight());
  }

  [[nodiscard]] QPainterPath toPainterPath(double scaleX = 1.0, double scaleY = 1.0) const;

  bool isAdd = true;
  ROIType type = ROIType::Spline;
  QPolygonF poly;
};

struct ZROIControlPoint
{
  enum class Pos
  {
    TopLeft,
    MidLeft,
    BottomLeft,
    BottomMid,
    BottomRight,
    MidRight,
    TopRight,
    TopMid,
    Center,
    Any
  };

  ZROIControlPoint(int slice_, size_t shapeID_, size_t shapeIndex_, Pos pos_, size_t pointIndex_ = 0)
    : slice(slice_)
    , shapeID(shapeID_)
    , shapeIndex(shapeIndex_)
    , pos(pos_)
    , pointIndex(pointIndex_)
  {}

  int slice;
  size_t shapeID;
  size_t shapeIndex;
  Pos pos;
  size_t pointIndex;
};

} // namespace nim
