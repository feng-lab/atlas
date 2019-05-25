#include "zroiutils.h"

#include "zglobal.h"
#include <Mathematics/GteNaturalSplineCurve.h>
#include <cmath>

namespace nim {

QPainterPath ZROIUtils::splineToQPainterPath(const QPolygonF& spline, bool showLastSeg)
{
  QPainterPath res;
  if (spline.size() < 2)
    return res;
  bool isClosed = spline.isClosed();
  if ((isClosed && spline.size() < 4) ||
      (!isClosed && spline.size() < 3)) {
    res.moveTo(spline[0]);
    res.lineTo(spline[1]);
    return res;
  }

  int numSegments = spline.size() - 1;
  std::vector<double> times(spline.size());
  times[0] = 0;
  for (size_t i = 1; i < times.size(); ++i) {
    times[i] = times[i - 1] + std::sqrt(QPointF::dotProduct(spline[i] - spline[i - 1], spline[i] - spline[i - 1]));
  }

  gte::NaturalSplineCurve<2, double> splineCurve(!isClosed, spline.size(), (gte::Vector<2, double> const*) spline.data(),
                                                 times.data());
  res.moveTo(spline[0]);
  int endSeg = showLastSeg ? numSegments : numSegments - 1;
  for (int i = 0; i < endSeg; ++i) {
    gte::Vector<2, double> values0[4];
    gte::Vector<2, double> values1[4];
    splineCurve.Evaluate(times[i], 1, values0);
    splineCurve.Evaluate(times[i + 1], 1, values1);
    gte::Vector<2, double>& m0 = values0[1];
    gte::Vector<2, double>& m1 = values1[1];
    m0 *= times[i + 1] - times[i];
    m1 *= times[i + 1] - times[i];
    //LOG(INFO) << m0.X() << " " << m0.Y() << " " << m1.X() << " " << m1.Y() << " " << cspline[i] << " " << cspline[i+1];
    res.cubicTo(spline[i].x() + 1. / 3. * m0[0], spline[i].y() + 1. / 3. * m0[1],
                spline[i + 1].x() - 1. / 3. * m1[0], spline[i + 1].y() - 1. / 3. * m1[1],
                spline[i + 1].x(), spline[i + 1].y());
  }

  return res;
}

std::tuple<ZImg, size_t, size_t> ZROIUtils::qPainterPathToMask(const QPainterPath& path)
{
  ZImg img;
  if (path.isEmpty()) {
    return std::make_tuple(img, 0_usize, 0_usize);
  }
  QRectF pathRect = path.boundingRect();
  int minX = std::max(static_cast<int>(std::floor(pathRect.left())), 0);
  int maxX = static_cast<int>(std::ceil(pathRect.right()));
  int minY = std::max(static_cast<int>(std::floor(pathRect.top())), 0);
  int maxY = static_cast<int>(std::ceil(pathRect.bottom()));
  if (maxX < minX || maxY < minY) {
    return std::make_tuple(img, 0_usize, 0_usize);
  }
  img = ZImg(ZImgInfo(maxX - minX + 1, maxY - minY + 1));
  for (int y = minY; y <= maxY; ++y) {
    for (int x = minX; x <= maxX; ++x) {
      if (path.contains(QPointF(x, y))) {
        *img.data<uint8_t>(x - minX, y - minY, 0) = 255;
      }
    }
  }
  return std::make_tuple(img, size_t(minX), size_t(minY));
}

std::tuple<ZROIUtils::RowMatrixXb, size_t, size_t> ZROIUtils::qPainterPathToMask_Python(const QPainterPath& path)
{
  RowMatrixXb img;
  if (path.isEmpty()) {
    return std::make_tuple(img, 0_usize, 0_usize);
  }
  QRectF pathRect = path.boundingRect();
  int minX = std::max(static_cast<int>(std::floor(pathRect.left())), 0);
  int maxX = static_cast<int>(std::ceil(pathRect.right()));
  int minY = std::max(static_cast<int>(std::floor(pathRect.top())), 0);
  int maxY = static_cast<int>(std::ceil(pathRect.bottom()));
  if (maxX < minX || maxY < minY) {
    return std::make_tuple(img, 0_usize, 0_usize);
  }
  img = RowMatrixXb(maxY - minY + 1, maxX - minX + 1);
  for (int y = minY; y <= maxY; ++y) {
    for (int x = minX; x <= maxX; ++x) {
      if (path.contains(QPointF(x, y))) {
        img(y - minY, x - minX) = true;
      } else {
        img(y - minY, x - minX) = false;
      }
    }
  }
  return std::make_tuple(img, size_t(minX), size_t(minY));
}

} // namespace nim
