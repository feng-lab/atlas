#include "zroiutils2.h"

#include "zglobal.h"
#include <Mathematics/NaturalSplineCurve.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkPath.h>
#include <include/pathops/SkPathOps.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSurface.h>
#include <QPointF>
#include <vector>
#include <cmath>

namespace {

using namespace nim;

SkPath splineToPath(const std::vector<QPointF>& spline, bool showLastSeg = true)
{
  SkPath res;
  if (spline.size() < 2)
    return res;
  bool isClosed = spline.front() == spline.back();
  if ((isClosed && spline.size() < 4) ||
      (!isClosed && spline.size() < 3)) {
    res.moveTo(spline[0].x(), spline[0].y());
    res.lineTo(spline[1].x(), spline[1].y());
    return res;
  }

  auto numSegments = spline.size() - 1;
  std::vector<double> times(spline.size());
  times[0] = 0;
  for (size_t i = 1; i < times.size(); ++i) {
    times[i] = times[i - 1] + std::sqrt(QPointF::dotProduct(spline[i] - spline[i - 1], spline[i] - spline[i - 1]));
  }

  gte::NaturalSplineCurve<2, double> splineCurve(!isClosed, spline.size(), (gte::Vector<2, double> const*) spline.data(),
                                                 times.data());
  res.moveTo(spline[0].x(), spline[0].y());
  auto endSeg = showLastSeg ? numSegments : numSegments - 1;
  for (size_t i = 0; i < endSeg; ++i) {
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

std::tuple<ZImg, index_t, index_t> pathToMask(const SkPath& path)
{
  ZImg img;
  if (path.isEmpty()) {
    return std::make_tuple(img, 0_z, 0_z);
  }

  path.updateBoundsCache();
  auto& pathRect = path.getBounds();
  auto minX = std::max(0_z, static_cast<index_t>(std::floor(pathRect.left())));
  auto maxX = static_cast<index_t>(std::ceil(pathRect.right()));
  auto minY = std::max(0_z, static_cast<index_t>(std::floor(pathRect.top())));
  auto maxY = static_cast<index_t>(std::ceil(pathRect.bottom()));
  if (maxX < minX || maxY < minY) {
    return std::make_tuple(img, 0_z, 0_z);
  }

  auto scale = 5;
  while (scale > 0 && ((maxX - minX + 1) * scale > 32767 || (maxY - minY + 1) * scale > 32767)) {
    --scale;
  }
  if (scale == 0) {
    img = ZImg(ZImgInfo(maxX - minX + 1, maxY - minY + 1));
    for (auto y = minY; y <= maxY; ++y) {
      for (auto x = minX; x <= maxX; ++x) {
        if (path.contains(x, y)) {       // not accurate for some spline
          *img.data<uint8_t>(x - minX, y - minY, 0) = 1;
        }
      }
    }
  } else {
    SkImageInfo info = SkImageInfo::Make((maxX - minX + 1) * scale, (maxY - minY + 1) * scale, kGray_8_SkColorType,
                                         kOpaque_SkAlphaType);
    size_t rowBytes = info.minRowBytes();
    size_t size = info.computeByteSize(rowBytes);
    std::vector<uint8_t> pixelMemory(size);  // allocate memory
    sk_sp<SkSurface> surface = SkSurface::MakeRasterDirect(info, &pixelMemory[0], rowBytes);
    SkCanvas* canvas = surface->getCanvas();

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kFill_Style);
    paint.setColor(SK_ColorWHITE);
    paint.setStrokeWidth(0);

    canvas->translate(-minX, -minY);
    canvas->drawPath(path, paint);

    img = ZImg(ZImgInfo(info.width(), info.height()));
    for (size_t y = 0; y < img.height(); ++y) {
      for (size_t x = 0; x < img.width(); ++x) {
        *img.data<uint8_t>(x, y, 0) = pixelMemory[info.computeOffset(x, y, rowBytes)] >= 128_u8 ? 1_u8 : 0_u8;
      }
    }
    img.resize(maxX - minX + 1, maxY - minY + 1, 1);
  }

  return std::make_tuple(img, minX, minY);
}

std::tuple<ZImg, index_t, index_t> pathToStroke(const SkPath& path, double width = 2.)
{
  ZImg img;
  if (path.isEmpty()) {
    return std::make_tuple(img, 0_z, 0_z);
  }

  path.updateBoundsCache();
  auto& pathRect = path.getBounds();
  auto minX = std::max(0_uz, static_cast<index_t>(std::floor(pathRect.left()) - width));
  auto maxX = static_cast<index_t>(std::ceil(pathRect.right()) + width);
  auto minY = std::max(0_uz, static_cast<index_t>(std::floor(pathRect.top()) - width));
  auto maxY = static_cast<index_t>(std::ceil(pathRect.bottom()) + width);
  if (maxX < minX || maxY < minY) {
    return std::make_tuple(img, 0_z, 0_z);
  }

  SkImageInfo info = SkImageInfo::Make((maxX - minX + 1), (maxY - minY + 1), kGray_8_SkColorType,
                                       kOpaque_SkAlphaType);
  size_t rowBytes = info.minRowBytes();
  size_t size = info.computeByteSize(rowBytes);
  std::vector<uint8_t> pixelMemory(size);  // allocate memory
  sk_sp<SkSurface> surface = SkSurface::MakeRasterDirect(info, &pixelMemory[0], rowBytes);
  SkCanvas* canvas = surface->getCanvas();

  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setColor(SK_ColorWHITE);
  paint.setStrokeWidth(width);

  canvas->translate(-minX, -minY);
  canvas->drawPath(path, paint);

  img = ZImg(ZImgInfo(info.width(), info.height()));
  for (size_t y = 0; y < img.height(); ++y) {
    for (size_t x = 0; x < img.width(); ++x) {
      *img.data<uint8_t>(x, y, 0) = pixelMemory[info.computeOffset(x, y, rowBytes)] >= 128_u8 ? 1_u8 : 0_u8;
    }
  }
  img.resize(maxX - minX + 1, maxY - minY + 1, 1);

  return std::make_tuple(img, minX, minY);
}

inline SkPath rectToPath(const std::vector<QPointF>& rect)
{
  CHECK(rect.size() == 2);
  SkPath path;
  path.addRect(rect[0].x(), rect[0].y(), rect[1].x(), rect[1].y());
  return path;
}

inline SkPath ellipseToPath(const std::vector<QPointF>& ellipse)
{
  CHECK(ellipse.size() == 2);
  SkPath path;
  path.addOval(SkRect::MakeLTRB(ellipse[0].x(), ellipse[0].y(), ellipse[1].x(), ellipse[1].y()));
  return path;
}

inline SkPath polygonToPath(const std::vector<QPointF>& poly)
{
  CHECK(poly.size() >= 4 && poly.front() == poly.back());
  SkPath path;
  std::vector<SkPoint> tmp(poly.size());
  for (size_t i = 0; i < poly.size(); ++i) {
    tmp[i].fX = poly[i].x();
    tmp[i].fY = poly[i].y();
  }
  path.addPoly(tmp.data(), static_cast<int>(tmp.size()) - 1, true);
  return path;
}

inline std::vector<QPointF> matToPoly(const ZROIUtils2::EigenDRef& mat)
{
  std::vector<QPointF> res;
  if (mat.rows() == 0 || mat.cols() == 0)
    return res;
  CHECK(mat.cols() == 2) << mat.rows() << " " << mat.cols();
  res.resize(mat.rows());
  for (Eigen::Index r = 0; r < mat.rows(); ++r) {
    res[r].setX(mat(r, 0));
    res[r].setY(mat(r, 1));
  }
  return res;
}

} // namespace

namespace nim {

std::tuple<ZImg, index_t, index_t> ZROIUtils2::splineToMask_Python(const EigenDRef& spline)
{
  return pathToMask(splineToPath(matToPoly(spline)));
}

std::tuple<ZImg, index_t, index_t> ZROIUtils2::rectToMask_Python(const EigenDRef& rect)
{
  return pathToMask(rectToPath(matToPoly(rect)));
}

std::tuple<ZImg, index_t, index_t> ZROIUtils2::ellipseToMask_Python(const EigenDRef& ellipse)
{
  return pathToMask(ellipseToPath(matToPoly(ellipse)));
}

std::tuple<ZImg, index_t, index_t> ZROIUtils2::polygonToMask_Python(const EigenDRef& poly)
{
  return pathToMask(polygonToPath(matToPoly(poly)));
}

std::tuple<ZImg, index_t, index_t> ZROIUtils2::shapeToMask_Python(const std::vector<std::tuple<EigenDRef, std::string, bool>>& shapeOps)
{
  SkPath pp;
  for (const auto&[points, type, isAdd] : shapeOps) {
    SkPath subpp;
    auto poly = matToPoly(points);
    if (type == "Rect") {
      subpp = rectToPath(poly);
    } else if (type == "Ellipse") {
      subpp = ellipseToPath(poly);
    } else if (type == "Polygon") {
      subpp = polygonToPath(poly);
    } else if (type == "Spline") {
      subpp = splineToPath(poly);
    } else if (type == "Line") {
      if(shapeOps.size() == 1) {
        return pathToStroke(splineToPath(poly));
      }
    }
    if (isAdd) {
      pp.addPath(subpp);
    } else {
      SkPath tmp;
      Op(pp, subpp, kDifference_SkPathOp, &tmp);
      pp.swap(tmp);
    }
  }
  return pathToMask(pp);
}

} // namespace nim
