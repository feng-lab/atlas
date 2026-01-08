#include "zroitypes.h"

#include "zroiutils.h"
#include <QTransform>

namespace nim {

void ZROIShapeOperation::flipAround(double x, double y, bool hFlip, bool vFlip)
{
  if (!hFlip && !vFlip) {
    return;
  }
  if (type == ROIType::Rect || type == ROIType::Ellipse) {
    auto r = rect();
    poly.clear();
    poly.push_back(r.topLeft());
    poly.push_back(r.topRight());
    poly.push_back(r.bottomRight());
    poly.push_back(r.bottomLeft());
  }
  for (auto& pt : poly) {
    if (hFlip) {
      pt.setX(x * 2.0 - pt.x());
    }
    if (vFlip) {
      pt.setY(y * 2.0 - pt.y());
    }
  }
  if (type == ROIType::Rect || type == ROIType::Ellipse) {
    setRect(poly.boundingRect());
  }
}

QPainterPath ZROIShapeOperation::toPainterPath(double scaleX, double scaleY) const
{
  QPainterPath res;
  QTransform tfm;
  tfm.scale(scaleX, scaleY);
  if (type == ROIType::Spline || type == ROIType::Line) {
    res.addPath(ZROIUtils::splineToQPainterPath(tfm.map(poly)));
  } else if (type == ROIType::Polygon) {
    res.addPolygon(tfm.map(poly));
  } else if (type == ROIType::Rect) {
    res.addRect(tfm.mapRect(rect()));
  } else if (type == ROIType::Ellipse) {
    res.addEllipse(tfm.mapRect(rect()));
  }
  return res;
}

} // namespace nim

