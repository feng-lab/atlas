#include "zroiutils.h"

#include "zglobal.h"
#include "zroimaskrasterizer.h"
#include <NaturalSplineCurve.h>
#include <QImage>
#include <QPainter>
#include <cmath>

namespace nim {
namespace {

ZROIMaskShapeType toMaskType(ROIType t)
{
  switch (t) {
    case ROIType::Rect:
      return ZROIMaskShapeType::Rect;
    case ROIType::Ellipse:
      return ZROIMaskShapeType::Ellipse;
    case ROIType::Polygon:
      return ZROIMaskShapeType::Polygon;
    case ROIType::Spline:
      return ZROIMaskShapeType::Spline;
    case ROIType::Line:
      return ZROIMaskShapeType::Line;
  }
  CHECK(false);
  return ZROIMaskShapeType::Polygon;
}

ZROIMaskOperation2D toMaskOp(const ZROIShapeOperation& op)
{
  ZROIMaskOperation2D res;
  res.isAdd = op.isAdd;
  res.type = toMaskType(op.type);
  res.poly.reserve(static_cast<size_t>(op.poly.size()));
  for (const auto& p : op.poly) {
    res.poly.emplace_back(p.x(), p.y());
  }
  return res;
}

} // namespace

QPainterPath ZROIUtils::splineToQPainterPath(const QPolygonF& spline, bool showLastSeg)
{
  QPainterPath res;
  if (spline.size() < 2) {
    return res;
  }
  bool isClosed = spline.isClosed();
  if ((isClosed && spline.size() < 4) || (!isClosed && spline.size() < 3)) {
    res.moveTo(spline[0]);
    res.lineTo(spline[1]);
    return res;
  }

  auto numSegments = spline.size() - 1;
  std::vector<double> times(spline.size());
  times[0] = 0;
  for (size_t i = 1; i < times.size(); ++i) {
    times[i] = times[i - 1] + std::sqrt(QPointF::dotProduct(spline[i] - spline[i - 1], spline[i] - spline[i - 1]));
  }

  gte::NaturalSplineCurve<2, double> splineCurve(!isClosed,
                                                 spline.size(),
                                                 (const gte::Vector<2, double>*)spline.data(),
                                                 times.data());
  res.moveTo(spline[0]);
  auto endSeg = showLastSeg ? numSegments : numSegments - 1;
  for (index_t i = 0; i < endSeg; ++i) {
    gte::Vector<2, double> values0[4];
    gte::Vector<2, double> values1[4];
    splineCurve.Evaluate(times[i], 1, values0);
    splineCurve.Evaluate(times[i + 1], 1, values1);
    gte::Vector<2, double>& m0 = values0[1];
    gte::Vector<2, double>& m1 = values1[1];
    m0 *= times[i + 1] - times[i];
    m1 *= times[i + 1] - times[i];
    // VLOG(1) << m0.X() << " " << m0.Y() << " " << m1.X() << " " << m1.Y() << " " << cspline[i] << " " <<
    // cspline[i+1];
    res.cubicTo(spline[i].x() + 1. / 3. * m0[0],
                spline[i].y() + 1. / 3. * m0[1],
                spline[i + 1].x() - 1. / 3. * m1[0],
                spline[i + 1].y() - 1. / 3. * m1[1],
                spline[i + 1].x(),
                spline[i + 1].y());
  }

  return res;
}

std::tuple<ZImg, index_t, index_t> ZROIUtils::shapeToMask(const std::vector<ZROIShapeOperation>& shapeOps,
                                                          const ZROIMaskRasterizerSettings& settings)
{
  if (shapeOps.empty()) {
    return std::make_tuple(ZImg(), 0_z, 0_z);
  }

  std::vector<ZROIMaskOperation2D> ops;
  ops.reserve(shapeOps.size());
  for (const auto& op : shapeOps) {
    ops.push_back(toMaskOp(op));
  }

  return ZROIMaskRasterizer::shapeToMask(ops, settings);
}

std::tuple<ZImg, index_t, index_t> ZROIUtils::qPainterPathToMaskMR(const QPainterPath& path)
{
  if (path.isEmpty()) {
    return std::make_tuple(ZImg(), 0_z, 0_z);
  }

  const auto polys = path.toFillPolygons();
  if (polys.isEmpty()) {
    return std::make_tuple(ZImg(), 0_z, 0_z);
  }

  std::vector<ZROIMaskOperation2D> ops;
  ops.reserve(static_cast<size_t>(polys.size()));

  for (const auto& poly : polys) {
    if (poly.size() < 3) {
      continue;
    }
    ZROIMaskOperation2D op;
    op.isAdd = true;
    op.type = ZROIMaskShapeType::Polygon;
    op.poly.reserve(static_cast<size_t>(poly.size()));
    for (const auto& p : poly) {
      op.poly.emplace_back(p.x(), p.y());
    }
    ops.push_back(std::move(op));
  }

  if (ops.empty()) {
    return std::make_tuple(ZImg(), 0_z, 0_z);
  }

  ZROIMaskRasterizerSettings settings;
  settings.supersample = 5;
  return ZROIMaskRasterizer::shapeToMask(ops, settings);
}

std::tuple<ZImg, index_t, index_t> ZROIUtils::qPainterPathToMaskQt(const QPainterPath& path)
{
  ZImg img;
  if (path.isEmpty()) {
    return std::make_tuple(img, 0_z, 0_z);
  }

  const QRectF pathRect = path.boundingRect();
  const auto minX = std::max(0_z, static_cast<index_t>(std::floor(pathRect.left())));
  const auto maxX = static_cast<index_t>(std::ceil(pathRect.right()));
  const auto minY = std::max(0_z, static_cast<index_t>(std::floor(pathRect.top())));
  const auto maxY = static_cast<index_t>(std::ceil(pathRect.bottom()));
  if (maxX < minX || maxY < minY) {
    return std::make_tuple(img, 0_z, 0_z);
  }

  constexpr index_t kDefaultSupersample = 5;
  constexpr index_t kMaxQImageDim = 32767; // QImage uses signed 16-bit dimensions internally.

  index_t scale = kDefaultSupersample;
  while (scale > 0 && ((maxX - minX + 1) * scale > kMaxQImageDim || (maxY - minY + 1) * scale > kMaxQImageDim)) {
    --scale;
  }

  if (scale == 0) {
    img = ZImg(ZImgInfo(static_cast<size_t>(maxX - minX + 1), static_cast<size_t>(maxY - minY + 1), 1));
    for (auto y = minY; y <= maxY; ++y) {
      for (auto x = minX; x <= maxX; ++x) {
        if (path.contains(QPointF(x, y))) { // not accurate for some spline
          *img.data<uint8_t>(static_cast<size_t>(x - minX), static_cast<size_t>(y - minY), 0) = 1_u8;
        }
      }
    }
  } else {
    QImage imageOut(static_cast<int>((maxX - minX + 1) * scale),
                    static_cast<int>((maxY - minY + 1) * scale),
                    QImage::Format_Mono);
    imageOut.fill(0);
    QPainter painter(&imageOut);
    painter.setBrush(Qt::white);
    painter.setPen(Qt::NoPen);
    painter.scale(static_cast<double>(scale), static_cast<double>(scale));
    painter.translate(-static_cast<double>(minX), -static_cast<double>(minY));
    painter.drawPath(path);

    img = ZImg(ZImgInfo(imageOut.width(), imageOut.height(), 1));
    for (size_t y = 0; y < img.height(); ++y) {
      for (size_t x = 0; x < img.width(); ++x) {
        *img.data<uint8_t>(x, y, 0) = imageOut.pixelIndex(static_cast<int>(x), static_cast<int>(y)) ? 1_u8 : 0_u8;
      }
    }
    img.resize(static_cast<size_t>(maxX - minX + 1), static_cast<size_t>(maxY - minY + 1), 1);
  }

  return std::make_tuple(img, minX, minY);
}

std::tuple<ZImg, index_t, index_t> ZROIUtils::qPainterPathToStroke(const QPainterPath& path, double width)
{
  ZImg img;
  if (path.isEmpty()) {
    return std::make_tuple(img, 0_z, 0_z);
  }
  QRectF pathRect = path.boundingRect();
  auto minX = std::max(0_z, static_cast<index_t>(std::floor(pathRect.left()) - width));
  auto maxX = static_cast<index_t>(std::ceil(pathRect.right()) + width);
  auto minY = std::max(0_z, static_cast<index_t>(std::floor(pathRect.top()) - width));
  auto maxY = static_cast<index_t>(std::ceil(pathRect.bottom()) + width);
  if (maxX < minX || maxY < minY) {
    return std::make_tuple(img, 0_z, 0_z);
  }

  QImage imageOut((maxX - minX + 1), (maxY - minY + 1), QImage::Format_Mono);
  imageOut.fill(0);
  QPainter painter(&imageOut);
  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(QBrush(Qt::white), width));
  painter.translate(-minX, -minY);
  painter.drawPath(path);
  // auto image = imageOut.scaled(maxX - minX + 1, maxY - minY + 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  img = ZImg(ZImgInfo(imageOut.width(), imageOut.height()));
  for (size_t y = 0; y < img.height(); ++y) {
    for (size_t x = 0; x < img.width(); ++x) {
      *img.data<uint8_t>(x, y, 0) = imageOut.pixelIndex(x, y) ? 1_u8 : 0_u8;
    }
  }
  img.resize(maxX - minX + 1, maxY - minY + 1, 1);

  return std::make_tuple(img, minX, minY);
}

// std::tuple<ZROIUtils::RowMatrixXb, index_t, index_t> ZROIUtils::qPainterPathToMask_Python(const QPainterPath& path)
//{
//   RowMatrixXb res;
//   auto [img, x_start, y_start] = qPainterPathToMask(path);
//   if (!img.isEmpty()) {
//     res = RowMatrixXb(img.height(), img.width());
//     for (size_t y = 0; y < img.height(); ++y) {
//       for (size_t x = 0; x < img.width(); ++x) {
//         res(y, x) = *img.data<uint8_t>(x, y, 0);
//       }
//     }
//   }
//   return std::make_tuple(res, x_start, y_start);
// }

} // namespace nim
