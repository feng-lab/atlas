#include "zroimaskrasterizer.h"

#include "zroi.h"
#include "zroiutils.h"
#include "zrandom.h"
#include "ztest.h"

#include <QFileInfo>
#include <array>
#include <random>
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
  res.poly.reserve(op.poly.size());
  for (const auto& p : op.poly) {
    res.poly.emplace_back(p.x(), p.y());
  }
  return res;
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

  [[nodiscard]] bool isEmpty() const { return mask.isEmpty(); }
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
    std::vector<ZROIMaskOperation2D> maskOps;
    maskOps.reserve(ops.size());
    for (const auto& op : ops) {
      maskOps.push_back(toMaskOp(op));
    }

    ZROIMaskRasterizerSettings settings;
    settings.supersample = 5;
    auto [mask, xStart, yStart] = ZROIMaskRasterizer::shapeToMask(maskOps, settings);
    perShape.push_back(MaskWithOffset{ std::move(mask), xStart, yStart });
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

CompareStats computeStats(const ZImg& a, const ZImg& b)
{
  CHECK(!a.isEmpty());
  CHECK(!b.isEmpty());
  CHECK(a.isSameSize(b));
  CHECK(a.isType<uint8_t>());
  CHECK(b.isType<uint8_t>());

  CompareStats stats;
  for (size_t y = 0; y < a.height(); ++y) {
    for (size_t x = 0; x < a.width(); ++x) {
      const uint8_t av = *a.data<uint8_t>(x, y, 0) ? 1_u8 : 0_u8;
      const uint8_t bv = *b.data<uint8_t>(x, y, 0) ? 1_u8 : 0_u8;
      if (av != bv) {
        ++stats.diffPixels;
      }
      if (av & bv) {
        ++stats.intersectionPixels;
      }
      if (av | bv) {
        ++stats.unionPixels;
      }
    }
  }
  if (stats.unionPixels == 0) {
    stats.iou = 1.0;
  } else {
    stats.iou = static_cast<double>(stats.intersectionPixels) / static_cast<double>(stats.unionPixels);
  }
  return stats;
}

} // namespace

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
    LOG(INFO) << "Testing nimroi: " << fileName << " slices=" << roi.numSlices()
              << " selected=" << slicesToTest.size();

    ZBenchTimer qtTimer("Qt baseline qPainterPathToMask");
    ZBenchTimer fastTimer("ZROIUtils qPainterPathToMask");
    ZBenchTimer newTimer("ZROIMaskRasterizer");
    qtTimer.reset();
    fastTimer.reset();
    newTimer.reset();

    for (const int slice : slicesToTest) {
      ZImg qtMask;
      index_t qtXStart = 0_z;
      index_t qtYStart = 0_z;
      {
        qtTimer.start();
        const QPainterPath refPath = roi.slicePaintPath(slice);
        std::tie(qtMask, qtXStart, qtYStart) = ZROIUtils::qPainterPathToMaskQt(refPath);
        qtTimer.stop();
      }

      ZImg fastMask;
      index_t fastXStart = 0_z;
      index_t fastYStart = 0_z;
      {
        fastTimer.start();
        const QPainterPath refPath = roi.slicePaintPath(slice);
        std::tie(fastMask, fastXStart, fastYStart) = ZROIUtils::qPainterPathToMask(refPath);
        fastTimer.stop();
      }

      MaskWithOffset newMask;
      {
        newTimer.start();
        newMask = rasterizeSliceMask(roi, slice);
        newTimer.stop();
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
      const index_t commonMaxX =
        std::max(std::max(qtXStart + qtMask.sWidth(), fastXStart + fastMask.sWidth()), newMask.xStart + newMask.mask.sWidth());
      const index_t commonMaxY =
        std::max(std::max(qtYStart + qtMask.sHeight(), fastYStart + fastMask.sHeight()), newMask.yStart + newMask.mask.sHeight());
      ASSERT_GT(commonMaxX, commonMinX);
      ASSERT_GT(commonMaxY, commonMinY);

      const size_t canvasW = static_cast<size_t>(commonMaxX - commonMinX);
      const size_t canvasH = static_cast<size_t>(commonMaxY - commonMinY);

      ZImg qtCanvas(ZImgInfo(canvasW, canvasH, 1));
      ZImg fastCanvas(ZImgInfo(canvasW, canvasH, 1));
      ZImg newCanvas(ZImgInfo(canvasW, canvasH, 1));
      qtCanvas.fill(0);
      fastCanvas.fill(0);
      newCanvas.fill(0);

      qtCanvas.pasteImg(qtMask, ZVoxelCoordinate(qtXStart - commonMinX, qtYStart - commonMinY));
      fastCanvas.pasteImg(fastMask, ZVoxelCoordinate(fastXStart - commonMinX, fastYStart - commonMinY));
      newCanvas.pasteImg(newMask.mask, ZVoxelCoordinate(newMask.xStart - commonMinX, newMask.yStart - commonMinY));

      const auto fastStats = computeStats(qtCanvas, fastCanvas);
      const auto newStats = computeStats(qtCanvas, newCanvas);
      LOG(INFO) << "  slice " << slice << " canvas=" << canvasW << "x" << canvasH
                << " qt_vs_fast union=" << fastStats.unionPixels << " diff=" << fastStats.diffPixels
                << " iou=" << fastStats.iou << " qt_vs_new union=" << newStats.unionPixels << " diff="
                << newStats.diffPixels << " iou=" << newStats.iou;

      // This test is meant to validate the mask result (binary). Minor edge differences can
      // happen due to rasterization details, but they should be extremely small for typical ROIs.
      EXPECT_GE(fastStats.iou, 0.999) << "Low IoU for " << fileName << " slice " << slice
                                      << " (qt_vs_fast diffPixels=" << fastStats.diffPixels
                                      << ", union=" << fastStats.unionPixels << ", canvas=" << canvasW << "x"
                                      << canvasH << ")";
      EXPECT_GE(newStats.iou, 0.999) << "Low IoU for " << fileName << " slice " << slice
                                     << " (qt_vs_new diffPixels=" << newStats.diffPixels
                                     << ", union=" << newStats.unionPixels << ", canvas=" << canvasW << "x" << canvasH
                                     << ")";
    }

    LOG(INFO) << qtTimer;
    LOG(INFO) << fastTimer;
    LOG(INFO) << newTimer;
  }
}

} // namespace nim
