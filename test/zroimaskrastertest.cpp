#include "zroimaskrasterizer.h"

#include "zroi.h"
#include "zroiutils.h"
#include "zregionontology.h"
#include "znaturalcubicspline2d.h"
#include "zrandom.h"
#include "ztest.h"

#include <QFileInfo>
#include <QPainterPath>
#include <QPointF>
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <string>
#include <vector>

namespace nim {
namespace {

constexpr std::array<const char*, 3> kNimroiTestFiles = {
  "20190325_ROE10_HC_.tif_fix.tif_ch2_label_2d_spline.nimroi",
  "T_Sub_20190628_OE40_Ret2_.tif.tif_label_2d_v2_spline.nimroi",
  "test1.nimroi",
};

constexpr int kSlicesPerFileToTest = 2;

std::vector<int> chooseSlicesToTest(const ZROI& roi)
{
  std::vector<int> slices;
  slices.reserve(roi.numSlices());
  for (auto it = roi.cbegin(); it != roi.cend(); ++it) {
    slices.push_back(it->first);
  }
  if (slices.empty()) {
    return slices;
  }

  const int targetCount = std::min(kSlicesPerFileToTest, static_cast<int>(slices.size()));
  if (targetCount == static_cast<int>(slices.size())) {
    return slices;
  }

  std::shuffle(slices.begin(), slices.end(), ZRandom::instance().engine());
  slices.resize(static_cast<size_t>(targetCount));
  std::sort(slices.begin(), slices.end());
  return slices;
}

void expectPathElementNear(const QPainterPath& path, int index, QPainterPath::ElementType type, double x, double y)
{
  ASSERT_LT(index, path.elementCount());
  const auto element = path.elementAt(index);
  EXPECT_EQ(element.type, type);
  EXPECT_NEAR(element.x, x, 1e-9);
  EXPECT_NEAR(element.y, y, 1e-9);
}

void expectVecNear(const glm::dvec2& actual, const glm::dvec2& expected, double tolerance)
{
  EXPECT_NEAR(actual.x, expected.x, tolerance);
  EXPECT_NEAR(actual.y, expected.y, tolerance);
}

std::vector<glm::dvec2> toPoints(const QPolygonF& poly)
{
  std::vector<glm::dvec2> points;
  points.reserve(static_cast<size_t>(poly.size()));
  for (const auto& p : poly) {
    points.emplace_back(p.x(), p.y());
  }
  return points;
}

ZImg makeBinaryMask(size_t width, size_t height)
{
  ZImg img(ZImgInfo(width, height, 1));
  for (size_t y = 0; y < height; ++y) {
    for (size_t x = 0; x < width; ++x) {
      *img.data<uint8_t>(x, y, 0) = 0;
    }
  }
  return img;
}

void fillRect(ZImg& img, int x0, int y0, int x1, int y1, uint8_t value)
{
  CHECK(x0 <= x1);
  CHECK(y0 <= y1);
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      *img.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), 0) = value;
    }
  }
}

void fillDisk(ZImg& img, int cx, int cy, int radius, uint8_t value)
{
  const int radiusSq = radius * radius;
  for (int y = cy - radius; y <= cy + radius; ++y) {
    for (int x = cx - radius; x <= cx + radius; ++x) {
      const int dx = x - cx;
      const int dy = y - cy;
      if (dx * dx + dy * dy <= radiusSq) {
        *img.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), 0) = value;
      }
    }
  }
}

[[nodiscard]] glm::dvec2 cubicBezierPoint(const ZNaturalCubicSpline2D::CubicBezier& b, double u)
{
  const double v = 1.0 - u;
  return v * v * v * b.p0 + 3.0 * v * v * u * b.p1 + 3.0 * v * u * u * b.p2 + u * u * u * b.p3;
}

[[nodiscard]] glm::dvec2 cubicBezierDerivative(const ZNaturalCubicSpline2D::CubicBezier& b, double u, double dt)
{
  CHECK(dt > 0.0);
  const double v = 1.0 - u;
  return (3.0 * v * v * (b.p1 - b.p0) + 6.0 * v * u * (b.p2 - b.p1) + 3.0 * u * u * (b.p3 - b.p2)) / dt;
}

[[nodiscard]] glm::dvec2 cubicBezierSecondDerivative(const ZNaturalCubicSpline2D::CubicBezier& b, double u, double dt)
{
  CHECK(dt > 0.0);
  const double v = 1.0 - u;
  return (6.0 * v * (b.p2 - 2.0 * b.p1 + b.p0) + 6.0 * u * (b.p3 - 2.0 * b.p2 + b.p1)) / (dt * dt);
}

void expectBezierNear(const ZNaturalCubicSpline2D::CubicBezier& actual,
                      const ZNaturalCubicSpline2D::CubicBezier& expected,
                      double tolerance)
{
  expectVecNear(actual.p0, expected.p0, tolerance);
  expectVecNear(actual.p1, expected.p1, tolerance);
  expectVecNear(actual.p2, expected.p2, tolerance);
  expectVecNear(actual.p3, expected.p3, tolerance);
}

void expectFiniteBezier(const ZNaturalCubicSpline2D::CubicBezier& b)
{
  for (const glm::dvec2 p : {b.p0, b.p1, b.p2, b.p3}) {
    EXPECT_TRUE(std::isfinite(p.x));
    EXPECT_TRUE(std::isfinite(p.y));
  }
}

void expectSplineHasConsistentKnotsAndBeziers(const std::vector<glm::dvec2>& rawPoints, double tolerance)
{
  const std::vector<glm::dvec2> points = ZNaturalCubicSpline2D::compactConsecutiveDuplicatePoints(rawPoints);
  ASSERT_GE(points.size(), 3_uz);
  const bool isClosed = points.front() == points.back();
  ASSERT_TRUE((isClosed && points.size() >= 4_uz) || (!isClosed && points.size() >= 3_uz));

  const std::vector<double> times = ZNaturalCubicSpline2D::chordLengthTimes(points);
  ASSERT_TRUE(ZNaturalCubicSpline2D::hasStrictlyIncreasingTimes(times));

  ZNaturalCubicSpline2D spline(!isClosed, points, times);
  for (size_t i = 0; i < points.size(); ++i) {
    expectVecNear(spline.position(times[i]), points[i], tolerance);
    const glm::dvec2 derivative = spline.derivative(times[i]);
    EXPECT_TRUE(std::isfinite(derivative.x));
    EXPECT_TRUE(std::isfinite(derivative.y));
  }

  const auto beziers = spline.toCubicBeziers();
  ASSERT_EQ(beziers.size() + 1, points.size());
  for (size_t i = 0; i < beziers.size(); ++i) {
    SCOPED_TRACE(fmt::format("segment={}", i));
    const double dt = times[i + 1] - times[i];
    expectFiniteBezier(beziers[i]);
    expectVecNear(beziers[i].p0, points[i], tolerance);
    expectVecNear(beziers[i].p3, points[i + 1], tolerance);
    for (double u : {0.0, 0.25, 0.5, 0.75, 1.0}) {
      const double t = times[i] + u * dt;
      expectVecNear(cubicBezierPoint(beziers[i], u), spline.position(t), tolerance);
      expectVecNear(cubicBezierDerivative(beziers[i], u, dt), spline.derivative(t), tolerance);
    }
  }

  for (size_t i = 1; i < beziers.size(); ++i) {
    SCOPED_TRACE(fmt::format("joint={}", i));
    const double prevDt = times[i] - times[i - 1];
    const double nextDt = times[i + 1] - times[i];
    expectVecNear(cubicBezierDerivative(beziers[i - 1], 1.0, prevDt),
                  cubicBezierDerivative(beziers[i], 0.0, nextDt),
                  tolerance);
    expectVecNear(cubicBezierSecondDerivative(beziers[i - 1], 1.0, prevDt),
                  cubicBezierSecondDerivative(beziers[i], 0.0, nextDt),
                  tolerance);
  }

  if (isClosed) {
    const double lastDt = times.back() - times[times.size() - 2];
    const double firstDt = times[1] - times[0];
    expectVecNear(cubicBezierDerivative(beziers.back(), 1.0, lastDt),
                  cubicBezierDerivative(beziers.front(), 0.0, firstDt),
                  tolerance);
    expectVecNear(cubicBezierSecondDerivative(beziers.back(), 1.0, lastDt),
                  cubicBezierSecondDerivative(beziers.front(), 0.0, firstDt),
                  tolerance);
  } else {
    const double firstDt = times[1] - times[0];
    const double lastDt = times.back() - times[times.size() - 2];
    expectVecNear(cubicBezierSecondDerivative(beziers.front(), 0.0, firstDt), glm::dvec2(0.0), tolerance);
    expectVecNear(cubicBezierSecondDerivative(beziers.back(), 1.0, lastDt), glm::dvec2(0.0), tolerance);
  }
}

void pasteOr(ZImg& dst, const ZImg& src, index_t startX, index_t startY)
{
  CHECK(!dst.isEmpty());
  CHECK(!src.isEmpty());
  CHECK(dst.isType<uint8_t>());
  CHECK(src.isType<uint8_t>());

  const index_t dstX0 = std::max(startX, 0_z);
  const index_t dstY0 = std::max(startY, 0_z);
  const index_t dstX1 = std::min(startX + src.sWidth(), dst.sWidth());
  const index_t dstY1 = std::min(startY + src.sHeight(), dst.sHeight());
  if (dstX1 <= dstX0 || dstY1 <= dstY0) {
    return;
  }
  const index_t srcX0 = dstX0 - startX;
  const index_t srcY0 = dstY0 - startY;

  for (index_t y = dstY0; y < dstY1; ++y) {
    const index_t sy = srcY0 + (y - dstY0);
    auto* d = dst.data<uint8_t>(static_cast<size_t>(dstX0), static_cast<size_t>(y), 0);
    const auto* s = src.data<uint8_t>(static_cast<size_t>(srcX0), static_cast<size_t>(sy), 0);
    for (index_t x = dstX0; x < dstX1; ++x) {
      d[x - dstX0] = std::max(d[x - dstX0], s[x - dstX0]);
    }
  }
}

struct MaskWithOffset
{
  ZImg mask;
  index_t xStart = 0_z;
  index_t yStart = 0_z;

  [[nodiscard]] bool isEmpty() const
  {
    return mask.isEmpty();
  }
};

MaskWithOffset unionShapeMasks(const std::vector<MaskWithOffset>& masks)
{
  MaskWithOffset out;
  bool hasAny = false;

  index_t minX = 0_z;
  index_t minY = 0_z;
  index_t maxX = 0_z;
  index_t maxY = 0_z;

  for (const auto& m : masks) {
    if (m.mask.isEmpty()) {
      continue;
    }
    if (!hasAny) {
      minX = m.xStart;
      minY = m.yStart;
      maxX = m.xStart + m.mask.sWidth();
      maxY = m.yStart + m.mask.sHeight();
      hasAny = true;
    } else {
      minX = std::min(minX, m.xStart);
      minY = std::min(minY, m.yStart);
      maxX = std::max(maxX, m.xStart + m.mask.sWidth());
      maxY = std::max(maxY, m.yStart + m.mask.sHeight());
    }
  }

  if (!hasAny) {
    return out;
  }

  out.xStart = minX;
  out.yStart = minY;
  out.mask = ZImg(ZImgInfo(static_cast<size_t>(maxX - minX), static_cast<size_t>(maxY - minY), 1));
  out.mask.fill(0);

  for (const auto& m : masks) {
    if (m.mask.isEmpty()) {
      continue;
    }
    pasteOr(out.mask, m.mask, m.xStart - out.xStart, m.yStart - out.yStart);
  }

  return out;
}

MaskWithOffset rasterizeSliceMask(const ZROI& roi, int slice)
{
  std::vector<MaskWithOffset> perShape;
  for (size_t id : roi.sliceShapeIDs(slice)) {
    const auto& ops = roi.shapeOperations(slice, id);

    ZROIMaskRasterizerSettings settings;
    settings.supersample = 5;
    auto [mask, xStart, yStart] = ZROIUtils::shapeToMask(ops, settings);
    perShape.push_back(MaskWithOffset{std::move(mask), xStart, yStart});
  }

  return unionShapeMasks(perShape);
}

struct CompareStats
{
  size_t diffPixels = 0;
  size_t unionPixels = 0;
  size_t intersectionPixels = 0;
  double iou = 1.0;
};

[[nodiscard]] size_t countOnPixels(const ZImg& img)
{
  CHECK(!img.isEmpty());
  CHECK(img.isType<uint8_t>());

  size_t onPixels = 0;
  for (size_t y = 0; y < img.height(); ++y) {
    const auto* row = img.data<uint8_t>(0, y, 0);
    for (size_t x = 0; x < img.width(); ++x) {
      onPixels += row[x] ? 1 : 0;
    }
  }
  return onPixels;
}

[[nodiscard]] size_t countIntersectionOnPixels(const ZImg& a,
                                               index_t aXStart,
                                               index_t aYStart,
                                               const ZImg& b,
                                               index_t bXStart,
                                               index_t bYStart)
{
  CHECK(!a.isEmpty());
  CHECK(!b.isEmpty());
  CHECK(a.isType<uint8_t>());
  CHECK(b.isType<uint8_t>());

  const index_t aX0 = aXStart;
  const index_t aY0 = aYStart;
  const index_t aX1 = aXStart + a.sWidth();
  const index_t aY1 = aYStart + a.sHeight();

  const index_t bX0 = bXStart;
  const index_t bY0 = bYStart;
  const index_t bX1 = bXStart + b.sWidth();
  const index_t bY1 = bYStart + b.sHeight();

  const index_t overlapX0 = std::max(aX0, bX0);
  const index_t overlapY0 = std::max(aY0, bY0);
  const index_t overlapX1 = std::min(aX1, bX1);
  const index_t overlapY1 = std::min(aY1, bY1);
  if (overlapX1 <= overlapX0 || overlapY1 <= overlapY0) {
    return 0;
  }

  const index_t aOverlapX0 = overlapX0 - aXStart;
  const index_t bOverlapX0 = overlapX0 - bXStart;
  CHECK(aOverlapX0 >= 0_z);
  CHECK(bOverlapX0 >= 0_z);
  const size_t overlapW = static_cast<size_t>(overlapX1 - overlapX0);

  size_t intersectionOnPixels = 0;
  for (index_t y = overlapY0; y < overlapY1; ++y) {
    const index_t aY = y - aYStart;
    const index_t bY = y - bYStart;
    CHECK(aY >= 0_z);
    CHECK(bY >= 0_z);

    const auto* aRow = a.data<uint8_t>(static_cast<size_t>(aOverlapX0), static_cast<size_t>(aY), 0);
    const auto* bRow = b.data<uint8_t>(static_cast<size_t>(bOverlapX0), static_cast<size_t>(bY), 0);
    for (size_t x = 0; x < overlapW; ++x) {
      intersectionOnPixels += (aRow[x] && bRow[x]) ? 1 : 0;
    }
  }

  return intersectionOnPixels;
}

[[nodiscard]] CompareStats computeStatsWithOffset(const ZImg& a,
                                                  index_t aXStart,
                                                  index_t aYStart,
                                                  size_t aOnPixels,
                                                  const ZImg& b,
                                                  index_t bXStart,
                                                  index_t bYStart,
                                                  size_t bOnPixels)
{
  CHECK(!a.isEmpty());
  CHECK(!b.isEmpty());

  CompareStats stats;
  stats.intersectionPixels = countIntersectionOnPixels(a, aXStart, aYStart, b, bXStart, bYStart);
  stats.unionPixels = aOnPixels + bOnPixels - stats.intersectionPixels;
  stats.diffPixels = aOnPixels + bOnPixels - 2 * stats.intersectionPixels;

  if (stats.unionPixels == 0) {
    stats.iou = 1.0;
  } else {
    stats.iou = static_cast<double>(stats.intersectionPixels) / static_cast<double>(stats.unionPixels);
  }
  return stats;
}

} // namespace

TEST(ZROIMaskRasterizer, SplineScaleAppliedExactlyOnce)
{
  // Regression: previously the spline rasterization path applied settings.scaleX/scaleY twice
  // (once before spline flattening and again in toHiResPoly), which could both distort results
  // and catastrophically inflate bounds/allocations when scale != 1.

  constexpr double kScaleX = 2.0;
  constexpr double kScaleY = 3.0;

  ZROIMaskOperation2D op;
  op.isAdd = true;
  op.type = ZROIMaskShapeType::Spline;
  op.poly = {
    glm::dvec2(0.0, 0.0),
    glm::dvec2(400.0, 0.0),
    glm::dvec2(400.0, 400.0),
    glm::dvec2(0.0, 400.0),
    glm::dvec2(0.0, 0.0),
  };

  ZROIMaskRasterizerSettings settings;
  settings.supersample = 5;
  settings.scaleX = kScaleX;
  settings.scaleY = kScaleY;

  auto [newMask, newXStart, newYStart] = ZROIMaskRasterizer::shapeToMask({op}, settings);
  ASSERT_FALSE(newMask.isEmpty());

  QPolygonF qpoly;
  qpoly << QPointF(0.0, 0.0) << QPointF(400.0, 0.0) << QPointF(400.0, 400.0) << QPointF(0.0, 400.0)
        << QPointF(0.0, 0.0);

  QPolygonF scaled;
  scaled.reserve(qpoly.size());
  for (const auto& p : qpoly) {
    scaled << QPointF(p.x() * kScaleX, p.y() * kScaleY);
  }
  const QPainterPath qtPath = ZROIUtils::splineToQPainterPath(scaled);

  auto [qtMask, qtXStart, qtYStart] = ZROIUtils::qPainterPathToMaskQt(qtPath);
  ASSERT_FALSE(qtMask.isEmpty());

  const size_t qtOnPixels = countOnPixels(qtMask);
  const size_t newOnPixels = countOnPixels(newMask);
  const auto stats =
    computeStatsWithOffset(qtMask, qtXStart, qtYStart, qtOnPixels, newMask, newXStart, newYStart, newOnPixels);
  EXPECT_GE(stats.iou, 0.999) << "Unexpectedly low IoU for scaled spline (diffPixels=" << stats.diffPixels
                              << ", union=" << stats.unionPixels << ")";
}

TEST(ZMaskToROI, PolygonOutputUsesApproxPolyDPShape)
{
  ZImg img = makeBinaryMask(48, 48);
  fillRect(img, 10, 12, 32, 34, 1);

  ZMaskToROIOptions options;
  options.outputType = ZMaskToROIOutputType::Polygon;
  options.epsilonPx = 1.0;

  ZROI roi;
  binaryImgToROI(img, roi, options);

  const std::vector<size_t> shapeIds = roi.sliceShapeIDs(0);
  ASSERT_EQ(shapeIds.size(), 1u);
  const auto& ops = roi.shapeOperations(0, shapeIds.front());
  ASSERT_EQ(ops.size(), 1u);
  EXPECT_EQ(ops.front().type, ROIType::Polygon);
  EXPECT_TRUE(ops.front().poly.isClosed());
  EXPECT_LE(ops.front().poly.size(), 6);
}

TEST(ZMaskToROI, PolygonToleranceIsMeasuredBeforeScale)
{
  ZImg img = makeBinaryMask(64, 64);
  fillDisk(img, 32, 32, 18, 1);

  ZMaskToROIOptions options;
  options.outputType = ZMaskToROIOutputType::Polygon;
  options.epsilonPx = 2.0;

  ZROI unscaledROI;
  binaryImgToROI(img, unscaledROI, options);
  const std::vector<size_t> unscaledShapeIds = unscaledROI.sliceShapeIDs(0);
  ASSERT_EQ(unscaledShapeIds.size(), 1u);
  const auto& unscaledOps = unscaledROI.shapeOperations(0, unscaledShapeIds.front());
  ASSERT_EQ(unscaledOps.size(), 1u);

  constexpr double kScaleX = 10.0;
  constexpr double kScaleY = 0.5;
  ZROI scaledROI;
  binaryImgToROI(img, scaledROI, options, kScaleX, kScaleY);
  const std::vector<size_t> scaledShapeIds = scaledROI.sliceShapeIDs(0);
  ASSERT_EQ(scaledShapeIds.size(), 1u);
  const auto& scaledOps = scaledROI.shapeOperations(0, scaledShapeIds.front());
  ASSERT_EQ(scaledOps.size(), 1u);

  ASSERT_EQ(scaledOps.front().poly.size(), unscaledOps.front().poly.size());
  for (int i = 0; i < unscaledOps.front().poly.size(); ++i) {
    EXPECT_NEAR(scaledOps.front().poly[i].x(), unscaledOps.front().poly[i].x() * kScaleX, 1e-9);
    EXPECT_NEAR(scaledOps.front().poly[i].y(), unscaledOps.front().poly[i].y() * kScaleY, 1e-9);
  }
}

TEST(ZMaskToROI, SplineOutputKeepsNaturalSplineControlPoints)
{
  ZImg img = makeBinaryMask(64, 64);
  fillDisk(img, 32, 32, 16, 1);

  ZMaskToROIOptions options;
  options.outputType = ZMaskToROIOutputType::Spline;
  options.epsilonPx = 100.0;

  ZROI roi;
  binaryImgToROI(img, roi, options);

  const std::vector<size_t> shapeIds = roi.sliceShapeIDs(0);
  ASSERT_EQ(shapeIds.size(), 1u);
  const auto& ops = roi.shapeOperations(0, shapeIds.front());
  ASSERT_EQ(ops.size(), 1u);
  EXPECT_EQ(ops.front().type, ROIType::Spline);
  EXPECT_TRUE(ops.front().poly.isClosed());
}

TEST(ZMaskToROI, SampledSplineOutputUsesLegacyFixedStrideControlPoints)
{
  ZImg img = makeBinaryMask(48, 48);
  fillRect(img, 10, 12, 32, 34, 1);

  ZMaskToROIOptions options;
  options.outputType = ZMaskToROIOutputType::SampledSpline;
  options.sampledSplineTargetPoints = 20;
  options.sampledSplineMaxPointSpacing = 30;

  ZROI roi;
  binaryImgToROI(img, roi, options);

  const std::vector<size_t> shapeIds = roi.sliceShapeIDs(0);
  ASSERT_EQ(shapeIds.size(), 1u);
  const auto& ops = roi.shapeOperations(0, shapeIds.front());
  ASSERT_EQ(ops.size(), 1u);
  EXPECT_EQ(ops.front().type, ROIType::Spline);
  EXPECT_TRUE(ops.front().poly.isClosed());
  EXPECT_GT(ops.front().poly.size(), 6);
}

TEST(ZMaskToROI, SplineFallsBackToPolygonWhenSpacingBlocksTolerance)
{
  ZImg img = makeBinaryMask(48, 48);
  fillRect(img, 10, 12, 32, 34, 1);

  ZMaskToROIOptions options;
  options.outputType = ZMaskToROIOutputType::Spline;
  options.epsilonPx = 0.01;
  options.minKnotSpacingPx = 1000.0;
  options.splineFallback = ZMaskToROISplineFallback::UsePolygon;

  ZROI roi;
  binaryImgToROI(img, roi, options);

  const std::vector<size_t> shapeIds = roi.sliceShapeIDs(0);
  ASSERT_EQ(shapeIds.size(), 1u);
  const auto& ops = roi.shapeOperations(0, shapeIds.front());
  ASSERT_EQ(ops.size(), 1u);
  EXPECT_EQ(ops.front().type, ROIType::Polygon);
}

TEST(ZMaskToROI, DefaultSplineFallbackKeepsBestSpline)
{
  ZImg img = makeBinaryMask(48, 48);
  fillRect(img, 10, 12, 32, 34, 1);

  ZMaskToROIOptions options;
  options.outputType = ZMaskToROIOutputType::Spline;
  options.epsilonPx = 0.01;
  options.minKnotSpacingPx = 1000.0;

  ZROI roi;
  binaryImgToROI(img, roi, options);

  const std::vector<size_t> shapeIds = roi.sliceShapeIDs(0);
  ASSERT_EQ(shapeIds.size(), 1u);
  const auto& ops = roi.shapeOperations(0, shapeIds.front());
  ASSERT_EQ(ops.size(), 1u);
  EXPECT_EQ(ops.front().type, ROIType::Spline);
}

TEST(ZMaskToROI, PreservesMaskHolesAsSubtractOperations)
{
  ZImg img = makeBinaryMask(48, 48);
  fillRect(img, 6, 6, 40, 40, 1);
  fillRect(img, 18, 18, 28, 28, 0);

  ZMaskToROIOptions options;
  options.outputType = ZMaskToROIOutputType::Polygon;
  options.epsilonPx = 1.0;
  options.preserveHoles = true;

  ZROI roi;
  binaryImgToROI(img, roi, options);

  const std::vector<size_t> shapeIds = roi.sliceShapeIDs(0);
  ASSERT_EQ(shapeIds.size(), 1u);
  const auto& ops = roi.shapeOperations(0, shapeIds.front());
  ASSERT_EQ(ops.size(), 2u);
  EXPECT_TRUE(ops[0].isAdd);
  EXPECT_FALSE(ops[1].isAdd);
  EXPECT_EQ(ops[0].type, ROIType::Polygon);
  EXPECT_EQ(ops[1].type, ROIType::Polygon);
}

TEST(ZNaturalCubicSpline2D, RepresentativeSplineMatchesRecordedBezierRegression)
{
  const std::vector<glm::dvec2> points = {
    glm::dvec2(0.0, 0.0),
    glm::dvec2(125.0, 40.0),
    glm::dvec2(260.0, -15.0),
    glm::dvec2(390.0, 95.0),
    glm::dvec2(520.0, 75.0),
  };
  const std::vector<ZNaturalCubicSpline2D::CubicBezier> expected = {
    {glm::dvec2(0.0,   0.0),
     glm::dvec2(41.332633407760,  24.354367301165),
     glm::dvec2(82.665266815520,  48.708734602330),
     glm::dvec2(125.0, 40.0) },
    {glm::dvec2(125.0, 40.0),
     glm::dvec2(172.021521625539, 30.327139877081),
     glm::dvec2(220.279305675190, -20.134742913430),
     glm::dvec2(260.0, -15.0)},
    {glm::dvec2(260.0, -15.0),
     glm::dvec2(306.401964041448, -9.001561148889),
     glm::dvec2(341.153311517232, 72.869919839366),
     glm::dvec2(390.0, 95.0) },
    {glm::dvec2(390.0, 95.0),
     glm::dvec2(427.727600013314, 112.092557110755),
     glm::dvec2(473.863800006657, 93.546278555377),
     glm::dvec2(520.0, 75.0) },
  };

  const auto actual = ZNaturalCubicSpline2D::fitChordLength(points);
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < actual.size(); ++i) {
    SCOPED_TRACE(fmt::format("segment={}", i));
    expectBezierNear(actual[i], expected[i], 1e-9);
  }
}

TEST(ZNaturalCubicSpline2D, DuplicateControlPointsDoNotChangeClosedSpline)
{
  const std::vector<glm::dvec2> clean = {
    glm::dvec2(0.0, 0.0),
    glm::dvec2(400.0, 0.0),
    glm::dvec2(400.0, 400.0),
    glm::dvec2(0.0, 400.0),
    glm::dvec2(0.0, 0.0),
  };
  const std::vector<glm::dvec2> withDuplicate = {
    glm::dvec2(0.0, 0.0),
    glm::dvec2(400.0, 0.0),
    glm::dvec2(400.0, 0.0),
    glm::dvec2(400.0, 400.0),
    glm::dvec2(0.0, 400.0),
    glm::dvec2(0.0, 0.0),
  };

  const auto cleanBeziers = ZNaturalCubicSpline2D::fitChordLength(clean);
  const auto duplicateBeziers = ZNaturalCubicSpline2D::fitChordLength(withDuplicate);
  ASSERT_EQ(cleanBeziers.size(), duplicateBeziers.size());
  for (size_t i = 0; i < cleanBeziers.size(); ++i) {
    SCOPED_TRACE(fmt::format("segment={}", i));
    expectBezierNear(duplicateBeziers[i], cleanBeziers[i], 1e-12);
  }
}

TEST(ZNaturalCubicSpline2D, NimroiSplineFixturesProduceConsistentBeziers)
{
  const QDir testDataDir = getTestDataDir();
  ASSERT_TRUE(testDataDir.exists()) << "ATLAS test data dir not found: " << testDataDir.absolutePath();

  size_t numSplines = 0;
  for (const char* fileName : kNimroiTestFiles) {
    const QString filePath = testDataDir.filePath(fileName);
    ASSERT_TRUE(QFileInfo::exists(filePath)) << "Missing test nimroi file: " << filePath;

    ZROI roi;
    roi.load(filePath);

    for (auto it = roi.cbegin(); it != roi.cend(); ++it) {
      const int slice = it->first;
      for (const size_t shapeID : roi.sliceShapeIDs(slice)) {
        const auto& ops = roi.shapeOperations(slice, shapeID);
        for (size_t opIndex = 0; opIndex < ops.size(); ++opIndex) {
          const auto& op = ops[opIndex];
          if (op.type != ROIType::Spline) {
            continue;
          }

          SCOPED_TRACE(fmt::format("file={} slice={} shapeID={} opIndex={}", fileName, slice, shapeID, opIndex));
          ++numSplines;
          expectSplineHasConsistentKnotsAndBeziers(toPoints(op.poly), 1e-8);
        }
      }
    }
  }

  EXPECT_GT(numSplines, 0_uz);
}

TEST(ZNaturalCubicSpline2D, RandomChordLengthSplinesProduceConsistentBeziers)
{
  constexpr double kPi = 3.141592653589793238462643383279502884;
  std::mt19937_64 rng(0x41544c41535f524f);
  std::uniform_real_distribution<double> stepDist(5.0, 75.0);
  std::uniform_real_distribution<double> yDist(-120.0, 120.0);
  std::uniform_real_distribution<double> radiusDist(60.0, 180.0);
  std::uniform_real_distribution<double> jitterDist(-0.18, 0.18);

  for (int numPoints = 3; numPoints <= 24; ++numPoints) {
    for (int rep = 0; rep < 12; ++rep) {
      std::vector<glm::dvec2> points;
      points.reserve(static_cast<size_t>(numPoints));
      double x = 0.0;
      for (int i = 0; i < numPoints; ++i) {
        x += stepDist(rng);
        points.emplace_back(x, yDist(rng));
      }
      expectSplineHasConsistentKnotsAndBeziers(points, 1e-7);
    }
  }

  for (int numUniquePoints = 3; numUniquePoints <= 24; ++numUniquePoints) {
    for (int rep = 0; rep < 12; ++rep) {
      std::vector<glm::dvec2> points;
      points.reserve(static_cast<size_t>(numUniquePoints) + 1);
      for (int i = 0; i < numUniquePoints; ++i) {
        const double angle = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(numUniquePoints);
        const double radius = radiusDist(rng) * (1.0 + jitterDist(rng));
        points.emplace_back(radius * std::cos(angle), 0.7 * radius * std::sin(angle));
      }
      points.push_back(points.front());
      expectSplineHasConsistentKnotsAndBeziers(points, 1e-7);
    }
  }
}

TEST(ZROIMaskRasterizer, ClosedSplineUsesStablePeriodicSolve)
{
  QPolygonF qpoly;
  qpoly << QPointF(0.0, 0.0) << QPointF(400.0, 0.0) << QPointF(400.0, 400.0) << QPointF(0.0, 400.0)
        << QPointF(0.0, 0.0);

  const QPainterPath path = ZROIUtils::splineToQPainterPath(qpoly);
  ASSERT_EQ(path.elementCount(), 13);

  expectPathElementNear(path, 0, QPainterPath::MoveToElement, 0.0, 0.0);
  expectPathElementNear(path, 1, QPainterPath::CurveToElement, 100.0, -100.0);
  expectPathElementNear(path, 2, QPainterPath::CurveToDataElement, 300.0, -100.0);
  expectPathElementNear(path, 3, QPainterPath::CurveToDataElement, 400.0, 0.0);
  expectPathElementNear(path, 10, QPainterPath::CurveToElement, -100.0, 300.0);
  expectPathElementNear(path, 11, QPainterPath::CurveToDataElement, -100.0, 100.0);
  expectPathElementNear(path, 12, QPainterPath::CurveToDataElement, 0.0, 0.0);
}

TEST(ZROIMaskRasterizer, MatchesQtMaskForNimroiFixtures)
{
  const QDir testDataDir = getTestDataDir();
  ASSERT_TRUE(testDataDir.exists()) << "ATLAS test data dir not found: " << testDataDir.absolutePath();

  for (const char* fileName : kNimroiTestFiles) {
    const QString filePath = testDataDir.filePath(fileName);
    ASSERT_TRUE(QFileInfo::exists(filePath)) << "Missing test nimroi file: " << filePath;

    ZROI roi;
    roi.load(filePath);

    const auto slicesToTest = chooseSlicesToTest(roi);
    ASSERT_FALSE(slicesToTest.empty()) << "No slices found in nimroi: " << fileName;
    LOG(INFO) << "Testing nimroi: " << fileName << " slices=" << roi.numSlices() << " selected=" << slicesToTest.size();

    ZBenchTimer qtTimer("Qt baseline qPainterPathToMask");
    ZBenchTimer fastTimer("ZROIUtils qPainterPathToMask");
    ZBenchTimer newTimer("ZROIMaskRasterizer");

    for (const int slice : slicesToTest) {
      ZImg qtMask;
      index_t qtXStart = 0_z;
      index_t qtYStart = 0_z;
      {
        qtTimer.resetAndStart(std::string("Qt baseline qPainterPathToMask slice ") + std::to_string(slice));
        const QPainterPath refPath = roi.slicePaintPath(slice);
        std::tie(qtMask, qtXStart, qtYStart) = ZROIUtils::qPainterPathToMaskQt(refPath);
        STOP_AND_LOG(qtTimer);
      }
      if (!qtMask.isEmpty()) {
        LOG(INFO) << "    qt mask start=" << qtXStart << "," << qtYStart << " size=" << qtMask.width() << "x"
                  << qtMask.height() << " bounds=[" << qtXStart << "," << qtYStart << "]-["
                  << (qtXStart + qtMask.sWidth()) << "," << (qtYStart + qtMask.sHeight())
                  << "] bytes=" << qtMask.byteNumber();
      } else {
        LOG(INFO) << "    qt mask empty";
      }

      ZImg fastMask;
      index_t fastXStart = 0_z;
      index_t fastYStart = 0_z;
      {
        fastTimer.resetAndStart(std::string("ZROIUtils qPainterPathToMask slice ") + std::to_string(slice));
        const QPainterPath refPath = roi.slicePaintPath(slice);
        std::tie(fastMask, fastXStart, fastYStart) = ZROIUtils::qPainterPathToMaskMR(refPath);
        STOP_AND_LOG(fastTimer);
      }
      if (!fastMask.isEmpty()) {
        LOG(INFO) << "    fast mask start=" << fastXStart << "," << fastYStart << " size=" << fastMask.width() << "x"
                  << fastMask.height() << " bounds=[" << fastXStart << "," << fastYStart << "]-["
                  << (fastXStart + fastMask.sWidth()) << "," << (fastYStart + fastMask.sHeight())
                  << "] bytes=" << fastMask.byteNumber();
      } else {
        LOG(INFO) << "    fast mask empty";
      }

      MaskWithOffset newMask;
      {
        newTimer.resetAndStart(std::string("ZROIMaskRasterizer slice ") + std::to_string(slice));
        newMask = rasterizeSliceMask(roi, slice);
        STOP_AND_LOG(newTimer);
      }
      if (!newMask.mask.isEmpty()) {
        LOG(INFO) << "    new mask start=" << newMask.xStart << "," << newMask.yStart
                  << " size=" << newMask.mask.width() << "x" << newMask.mask.height() << " bounds=[" << newMask.xStart
                  << "," << newMask.yStart << "]-[" << (newMask.xStart + newMask.mask.sWidth()) << ","
                  << (newMask.yStart + newMask.mask.sHeight()) << "] bytes=" << newMask.mask.byteNumber();
      } else {
        LOG(INFO) << "    new mask empty";
      }

      if (qtMask.isEmpty() && fastMask.isEmpty() && newMask.mask.isEmpty()) {
        continue;
      }
      ASSERT_FALSE(qtMask.isEmpty()) << "Qt baseline mask unexpectedly empty for " << fileName << " slice " << slice;
      ASSERT_FALSE(fastMask.isEmpty()) << "ZROIUtils::qPainterPathToMask mask unexpectedly empty for " << fileName
                                       << " slice " << slice;
      ASSERT_FALSE(newMask.mask.isEmpty())
        << "New rasterizer mask unexpectedly empty for " << fileName << " slice " << slice;

      const index_t commonMinX = std::min(std::min(qtXStart, fastXStart), newMask.xStart);
      const index_t commonMinY = std::min(std::min(qtYStart, fastYStart), newMask.yStart);
      const index_t commonMaxX = std::max(std::max(qtXStart + qtMask.sWidth(), fastXStart + fastMask.sWidth()),
                                          newMask.xStart + newMask.mask.sWidth());
      const index_t commonMaxY = std::max(std::max(qtYStart + qtMask.sHeight(), fastYStart + fastMask.sHeight()),
                                          newMask.yStart + newMask.mask.sHeight());
      LOG(INFO) << "    canvas bounds min=" << commonMinX << "," << commonMinY << " max=" << commonMaxX << ","
                << commonMaxY;
      ASSERT_GT(commonMaxX, commonMinX);
      ASSERT_GT(commonMaxY, commonMinY);

      const index_t canvasSW = commonMaxX - commonMinX;
      const index_t canvasSH = commonMaxY - commonMinY;
      LOG(INFO) << "    canvas size signed=" << canvasSW << "x" << canvasSH;
      const size_t canvasW = static_cast<size_t>(canvasSW);
      const size_t canvasH = static_cast<size_t>(canvasSH);
      const size_t bytesPerCanvas = ZImgInfo(canvasW, canvasH, 1).byteNumber();
      LOG(INFO) << "    canvas virtual size=" << canvasW << "x" << canvasH << " bytes_per_canvas=" << bytesPerCanvas
                << " bytes_total_would_be=" << (bytesPerCanvas * 3) << " (3 canvases, skipped)";

      // IMPORTANT: Avoid allocating full canvases for stats. Extremely large coordinate spans
      // (e.g. a single mask with an outlier start offset) can make canvasW*canvasH enormous and
      // explode memory. The IoU/union/diff computations depend only on the masks and their
      // relative offsets, not on materializing the full union canvas.
      const size_t qtOnPixels = countOnPixels(qtMask);
      const size_t fastOnPixels = countOnPixels(fastMask);
      const size_t newOnPixels = countOnPixels(newMask.mask);

      const auto fastStats =
        computeStatsWithOffset(qtMask, qtXStart, qtYStart, qtOnPixels, fastMask, fastXStart, fastYStart, fastOnPixels);
      const auto newStats = computeStatsWithOffset(qtMask,
                                                   qtXStart,
                                                   qtYStart,
                                                   qtOnPixels,
                                                   newMask.mask,
                                                   newMask.xStart,
                                                   newMask.yStart,
                                                   newOnPixels);
      LOG(INFO) << "  slice " << slice << " canvas=" << canvasW << "x" << canvasH
                << " qt_vs_fast union=" << fastStats.unionPixels << " diff=" << fastStats.diffPixels
                << " iou=" << fastStats.iou << " qt_vs_new union=" << newStats.unionPixels
                << " diff=" << newStats.diffPixels << " iou=" << newStats.iou;

      // This test is meant to validate the mask result (binary). Minor edge differences can
      // happen due to rasterization details, but they should be extremely small for typical ROIs.
      EXPECT_GE(fastStats.iou, 0.999) << "Low IoU for " << fileName << " slice " << slice
                                      << " (qt_vs_fast diffPixels=" << fastStats.diffPixels
                                      << ", union=" << fastStats.unionPixels << ", canvas=" << canvasW << "x" << canvasH
                                      << ")";
      EXPECT_GE(newStats.iou, 0.999) << "Low IoU for " << fileName << " slice " << slice
                                     << " (qt_vs_new diffPixels=" << newStats.diffPixels
                                     << ", union=" << newStats.unionPixels << ", canvas=" << canvasW << "x" << canvasH
                                     << ")";
    }
  }
}

} // namespace nim
