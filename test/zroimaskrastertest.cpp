#include "zroimaskrasterizer.h"

#include "zroi.h"
#include "zroiutils.h"
#include "zrandom.h"
#include "ztest.h"

#include <QFileInfo>
#include <QPointF>
#include <array>
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

TEST(ZROIMaskRasterizer, MatchesQtMaskForNimroiFixtures)
{
  const QDir testDataDir = getTestDataDir();
  ASSERT_TRUE(testDataDir.exists()) << "ATLAS test data dir not found: " << testDataDir.absolutePath().toStdString();

  for (const char* fileName : kNimroiTestFiles) {
    const QString filePath = testDataDir.filePath(fileName);
    ASSERT_TRUE(QFileInfo::exists(filePath)) << "Missing test nimroi file: " << filePath.toStdString();

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
