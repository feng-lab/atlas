#include "zroiutils.h"

#include "zglobal.h"
#include <Mathematics/GteNaturalSplineCurve.h>
#include <QImage>
#include <QPainter>
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

std::tuple<ZImg, int32_t, int32_t> ZROIUtils::qPainterPathToMask(const QPainterPath& path)
{
  ZImg img;
  if (path.isEmpty()) {
    return std::make_tuple(img, 0_i32, 0_i32);
  }
  QRectF pathRect = path.boundingRect();
  int minX = static_cast<int>(std::floor(pathRect.left()));
  int maxX = static_cast<int>(std::ceil(pathRect.right()));
  int minY = static_cast<int>(std::floor(pathRect.top()));
  int maxY = static_cast<int>(std::ceil(pathRect.bottom()));
  if (maxX < minX || maxY < minY) {
    return std::make_tuple(img, 0_i32, 0_i32);
  }

  int scale = 5;
  while (scale > 0 && ((maxX - minX + 1) * scale > 32767 || (maxY - minY + 1) * scale > 32767)) {
    --scale;
  }
  if (scale == 0) {
    img = ZImg(ZImgInfo(maxX - minX + 1, maxY - minY + 1));
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        if (path.contains(QPointF(x, y))) {       // not accurate for some spline
          *img.data<uint8_t>(x - minX, y - minY, 0) = 255;
        }
      }
    }
  } else {
    QImage imageOut((maxX - minX + 1) * scale, (maxY - minY + 1) * scale, QImage::Format_Mono);
    imageOut.fill(0);
    QPainter painter(&imageOut);
    painter.setBrush(Qt::white);
    painter.setPen(Qt::NoPen);
    painter.scale(scale, scale);
    painter.translate(-minX, -minY);
    painter.drawPath(path);
    // auto image = imageOut.scaled(maxX - minX + 1, maxY - minY + 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    img = ZImg(ZImgInfo(imageOut.width(), imageOut.height()));
    for (size_t y = 0; y < img.height(); ++y) {
      for (size_t x = 0; x < img.width(); ++x) {
        *img.data<uint8_t>(x, y, 0) = imageOut.pixelIndex(x, y) ? 1 : 0;
      }
    }
    img.resize(maxX - minX + 1, maxY - minY + 1, 1);
  }

  return std::make_tuple(img, minX, minY);
}

std::tuple<ZROIUtils::RowMatrixXb, int32_t, int32_t> ZROIUtils::qPainterPathToMask_Python(const QPainterPath& path)
{
  RowMatrixXb res;
  auto [img, x_start, y_start] = qPainterPathToMask(path);
  if (!img.isEmpty()) {
    res = RowMatrixXb(img.height(), img.width());
    for (size_t y = 0; y < img.height(); ++y) {
      for (size_t x = 0; x < img.width(); ++x) {
        res(y, x) = *img.data<uint8_t>(x, y, 0);
      }
    }
  }
  return std::make_tuple(res, x_start, y_start);
}

} // namespace nim
