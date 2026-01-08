#include "zroimaskrasterizer.h"

#include "zlog.h"

#include <NaturalSplineCurve.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>

namespace nim {
namespace {

struct ZROIBounds2D
{
  double minX = std::numeric_limits<double>::infinity();
  double minY = std::numeric_limits<double>::infinity();
  double maxX = -std::numeric_limits<double>::infinity();
  double maxY = -std::numeric_limits<double>::infinity();

  void expand(const glm::dvec2& p)
  {
    minX = std::min(minX, p.x);
    minY = std::min(minY, p.y);
    maxX = std::max(maxX, p.x);
    maxY = std::max(maxY, p.y);
  }

  void expand(double x, double y)
  {
    minX = std::min(minX, x);
    minY = std::min(minY, y);
    maxX = std::max(maxX, x);
    maxY = std::max(maxY, y);
  }

  void expand(const ZROIBounds2D& other)
  {
    minX = std::min(minX, other.minX);
    minY = std::min(minY, other.minY);
    maxX = std::max(maxX, other.maxX);
    maxY = std::max(maxY, other.maxY);
  }

  [[nodiscard]] bool isEmpty() const
  {
    return !(minX <= maxX && minY <= maxY);
  }
};

class ZBitMask2D
{
public:
  ZBitMask2D() = default;
  ZBitMask2D(int width, int height)
    : m_width(width)
    , m_height(height)
  {
    CHECK(width >= 0);
    CHECK(height >= 0);

    const size_t w = static_cast<size_t>(width);
    const size_t h = static_cast<size_t>(height);
    CHECK(w == 0 || (std::numeric_limits<size_t>::max() / w) >= h);
    const size_t bitCount = w * h;
    const size_t wordCount = (bitCount + 63) / 64;
    m_words.assign(wordCount, 0);
  }

  [[nodiscard]] int width() const { return m_width; }
  [[nodiscard]] int height() const { return m_height; }

  void set(int x, int y)
  {
    const size_t bitIndex = static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x);
    m_words[bitIndex / 64] |= (1ULL << (bitIndex % 64));
  }

  void clear(int x, int y)
  {
    const size_t bitIndex = static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x);
    m_words[bitIndex / 64] &= ~(1ULL << (bitIndex % 64));
  }

  [[nodiscard]] bool get(int x, int y) const
  {
    const size_t bitIndex = static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x);
    return (m_words[bitIndex / 64] >> (bitIndex % 64)) & 1ULL;
  }

  void setSpan(int y, int xStart, int xEndExclusive)
  {
    if (xStart >= xEndExclusive) {
      return;
    }
    CHECK(y >= 0 && y < m_height);
    CHECK(xStart >= 0 && xEndExclusive <= m_width);

    size_t startBit = static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(xStart);
    size_t endBit = static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(xEndExclusive);
    setSpanBits(startBit, endBit);
  }

  void clearSpan(int y, int xStart, int xEndExclusive)
  {
    if (xStart >= xEndExclusive) {
      return;
    }
    CHECK(y >= 0 && y < m_height);
    CHECK(xStart >= 0 && xEndExclusive <= m_width);

    size_t startBit = static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(xStart);
    size_t endBit = static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(xEndExclusive);
    clearSpanBits(startBit, endBit);
  }

private:
  void setSpanBits(size_t startBit, size_t endBitExclusive)
  {
    CHECK(startBit <= endBitExclusive);
    if (startBit == endBitExclusive) {
      return;
    }

    const size_t startWord = startBit / 64;
    const size_t endWord = (endBitExclusive - 1) / 64;
    const int startOffset = static_cast<int>(startBit % 64);
    const int endOffset = static_cast<int>((endBitExclusive - 1) % 64);

    if (startWord == endWord) {
      const uint64_t mask = ((endOffset == 63 ? ~0ULL : ((1ULL << (endOffset + 1)) - 1ULL)) &
                             (startOffset == 0 ? ~0ULL : ~((1ULL << startOffset) - 1ULL)));
      m_words[startWord] |= mask;
      return;
    }

    if (startOffset != 0) {
      m_words[startWord] |= ~((1ULL << startOffset) - 1ULL);
    } else {
      m_words[startWord] = ~0ULL;
    }

    for (size_t w = startWord + 1; w < endWord; ++w) {
      m_words[w] = ~0ULL;
    }

    if (endOffset != 63) {
      m_words[endWord] |= (1ULL << (endOffset + 1)) - 1ULL;
    } else {
      m_words[endWord] = ~0ULL;
    }
  }

  void clearSpanBits(size_t startBit, size_t endBitExclusive)
  {
    CHECK(startBit <= endBitExclusive);
    if (startBit == endBitExclusive) {
      return;
    }

    const size_t startWord = startBit / 64;
    const size_t endWord = (endBitExclusive - 1) / 64;
    const int startOffset = static_cast<int>(startBit % 64);
    const int endOffset = static_cast<int>((endBitExclusive - 1) % 64);

    if (startWord == endWord) {
      const uint64_t mask = ((endOffset == 63 ? ~0ULL : ((1ULL << (endOffset + 1)) - 1ULL)) &
                             (startOffset == 0 ? ~0ULL : ~((1ULL << startOffset) - 1ULL)));
      m_words[startWord] &= ~mask;
      return;
    }

    if (startOffset != 0) {
      m_words[startWord] &= (1ULL << startOffset) - 1ULL;
    } else {
      m_words[startWord] = 0ULL;
    }

    for (size_t w = startWord + 1; w < endWord; ++w) {
      m_words[w] = 0ULL;
    }

    if (endOffset != 63) {
      m_words[endWord] &= ~((1ULL << (endOffset + 1)) - 1ULL);
    } else {
      m_words[endWord] = 0ULL;
    }
  }

  int m_width = 0;
  int m_height = 0;
  std::vector<uint64_t> m_words;
};

[[nodiscard]] glm::dvec2 applyScale(const glm::dvec2& p, double scaleX, double scaleY)
{
  return glm::dvec2(p.x * scaleX, p.y * scaleY);
}

[[nodiscard]] double distancePointToLine(const glm::dvec2& p, const glm::dvec2& a, const glm::dvec2& b)
{
  const glm::dvec2 ab = b - a;
  const double denom = glm::dot(ab, ab);
  if (denom <= 0.0) {
    return glm::length(p - a);
  }
  const double t = glm::clamp(glm::dot(p - a, ab) / denom, 0.0, 1.0);
  const glm::dvec2 proj = a + t * ab;
  return glm::length(p - proj);
}

[[nodiscard]] bool cubicFlatEnough(const glm::dvec2& p0,
                                   const glm::dvec2& p1,
                                   const glm::dvec2& p2,
                                   const glm::dvec2& p3,
                                   double tol)
{
  const double d1 = distancePointToLine(p1, p0, p3);
  const double d2 = distancePointToLine(p2, p0, p3);
  return d1 <= tol && d2 <= tol;
}

void flattenCubicBezier(const glm::dvec2& p0,
                        const glm::dvec2& p1,
                        const glm::dvec2& p2,
                        const glm::dvec2& p3,
                        double tol,
                        std::vector<glm::dvec2>& out)
{
  struct Segment
  {
    glm::dvec2 p0, p1, p2, p3;
  };

  std::vector<Segment> stack;
  stack.push_back(Segment{ p0, p1, p2, p3 });
  while (!stack.empty()) {
    Segment s = stack.back();
    stack.pop_back();

    if (cubicFlatEnough(s.p0, s.p1, s.p2, s.p3, tol)) {
      out.push_back(s.p3);
      continue;
    }

    // Subdivide at t=0.5 using de Casteljau.
    const glm::dvec2 p01 = 0.5 * (s.p0 + s.p1);
    const glm::dvec2 p12 = 0.5 * (s.p1 + s.p2);
    const glm::dvec2 p23 = 0.5 * (s.p2 + s.p3);
    const glm::dvec2 p012 = 0.5 * (p01 + p12);
    const glm::dvec2 p123 = 0.5 * (p12 + p23);
    const glm::dvec2 p0123 = 0.5 * (p012 + p123);

    // Push second half first so the first half is processed first.
    stack.push_back(Segment{ p0123, p123, p23, s.p3 });
    stack.push_back(Segment{ s.p0, p01, p012, p0123 });
  }
}

std::vector<std::array<glm::dvec2, 4>> splineToCubicBeziers(const std::vector<glm::dvec2>& points)
{
  std::vector<std::array<glm::dvec2, 4>> beziers;
  if (points.size() < 2) {
    return beziers;
  }
  const bool isClosed = (points.front() == points.back());
  if ((isClosed && points.size() < 4) || (!isClosed && points.size() < 3)) {
    return beziers;
  }

  const size_t numSegments = points.size() - 1;
  std::vector<double> times(points.size(), 0.0);
  for (size_t i = 1; i < points.size(); ++i) {
    times[i] = times[i - 1] + glm::length(points[i] - points[i - 1]);
  }

  std::vector<gte::Vector<2, double>> gtePoints(points.size());
  for (size_t i = 0; i < points.size(); ++i) {
    gtePoints[i][0] = points[i].x;
    gtePoints[i][1] = points[i].y;
  }

  gte::NaturalSplineCurve<2, double> curve(!isClosed, points.size(), gtePoints.data(), times.data());

  for (size_t i = 0; i < numSegments; ++i) {
    gte::Vector<2, double> values0[4];
    gte::Vector<2, double> values1[4];
    curve.Evaluate(times[i], 1, values0);
    curve.Evaluate(times[i + 1], 1, values1);

    gte::Vector<2, double> m0 = values0[1];
    gte::Vector<2, double> m1 = values1[1];

    const double dt = times[i + 1] - times[i];
    m0 *= dt;
    m1 *= dt;

    const glm::dvec2 p0 = points[i];
    const glm::dvec2 p3 = points[i + 1];
    const glm::dvec2 p1 = glm::dvec2(points[i].x + (1.0 / 3.0) * m0[0], points[i].y + (1.0 / 3.0) * m0[1]);
    const glm::dvec2 p2 =
      glm::dvec2(points[i + 1].x - (1.0 / 3.0) * m1[0], points[i + 1].y - (1.0 / 3.0) * m1[1]);

    beziers.push_back({ p0, p1, p2, p3 });
  }

  return beziers;
}

std::vector<glm::dvec2> splineToPolyline(const std::vector<glm::dvec2>& points, double tol)
{
  std::vector<glm::dvec2> res;
  if (points.size() < 2) {
    return res;
  }

  const bool isClosed = (points.front() == points.back());
  if ((isClosed && points.size() < 4) || (!isClosed && points.size() < 3)) {
    res.push_back(points[0]);
    res.push_back(points[1]);
    return res;
  }

  const auto beziers = splineToCubicBeziers(points);
  if (beziers.empty()) {
    res.push_back(points[0]);
    res.push_back(points[1]);
    return res;
  }

  res.push_back(beziers.front()[0]);
  for (const auto& b : beziers) {
    flattenCubicBezier(b[0], b[1], b[2], b[3], tol, res);
  }
  return res;
}

void applySpanOp(ZBitMask2D& mask, bool setBits, int y, int xStart, int xEndExclusive)
{
  xStart = std::clamp(xStart, 0, mask.width());
  xEndExclusive = std::clamp(xEndExclusive, 0, mask.width());
  if (xStart >= xEndExclusive || y < 0 || y >= mask.height()) {
    return;
  }
  if (setBits) {
    mask.setSpan(y, xStart, xEndExclusive);
  } else {
    mask.clearSpan(y, xStart, xEndExclusive);
  }
}

void rasterizePolygonEvenOdd(const std::vector<glm::dvec2>& poly, ZBitMask2D& mask, bool setBits)
{
  if (poly.size() < 3) {
    return;
  }

  double minY = poly.front().y;
  double maxY = poly.front().y;
  for (const auto& p : poly) {
    minY = std::min(minY, p.y);
    maxY = std::max(maxY, p.y);
  }

  const int yStart = std::max(0, static_cast<int>(std::floor(minY - 0.5)));
  const int yEndExclusive = std::min(mask.height(), static_cast<int>(std::ceil(maxY - 0.5)));

  std::vector<double> xIntersections;
  xIntersections.reserve(poly.size());

  for (int y = yStart; y < yEndExclusive; ++y) {
    const double yCoord = static_cast<double>(y) + 0.5;
    xIntersections.clear();

    for (size_t i = 0; i < poly.size(); ++i) {
      const glm::dvec2& p0 = poly[i];
      const glm::dvec2& p1 = poly[(i + 1) % poly.size()];
      const double y0 = p0.y;
      const double y1 = p1.y;

      const bool crosses = ((y0 <= yCoord) && (y1 > yCoord)) || ((y1 <= yCoord) && (y0 > yCoord));
      if (!crosses) {
        continue;
      }
      const double t = (yCoord - y0) / (y1 - y0);
      const double x = p0.x + t * (p1.x - p0.x);
      xIntersections.push_back(x);
    }

    if (xIntersections.size() < 2) {
      continue;
    }
    std::sort(xIntersections.begin(), xIntersections.end());

    for (size_t k = 0; k + 1 < xIntersections.size(); k += 2) {
      const double xL = xIntersections[k];
      const double xR = xIntersections[k + 1];
      const int xStart = static_cast<int>(std::ceil(xL - 0.5));
      const int xEnd = static_cast<int>(std::ceil(xR - 0.5));
      applySpanOp(mask, setBits, y, xStart, xEnd);
    }
  }
}

void rasterizeRect(const glm::dvec2& p0, const glm::dvec2& p1, ZBitMask2D& mask, bool setBits)
{
  const double left = std::min(p0.x, p1.x);
  const double right = std::max(p0.x, p1.x);
  const double top = std::min(p0.y, p1.y);
  const double bottom = std::max(p0.y, p1.y);

  const int yStart = std::max(0, static_cast<int>(std::ceil(top - 0.5)));
  const int yEndExclusive = std::min(mask.height(), static_cast<int>(std::ceil(bottom - 0.5)));
  const int xStart = static_cast<int>(std::ceil(left - 0.5));
  const int xEndExclusive = static_cast<int>(std::ceil(right - 0.5));

  for (int y = yStart; y < yEndExclusive; ++y) {
    applySpanOp(mask, setBits, y, xStart, xEndExclusive);
  }
}

void rasterizeEllipse(const glm::dvec2& p0, const glm::dvec2& p1, ZBitMask2D& mask, bool setBits)
{
  const double left = std::min(p0.x, p1.x);
  const double right = std::max(p0.x, p1.x);
  const double top = std::min(p0.y, p1.y);
  const double bottom = std::max(p0.y, p1.y);

  const double rx = (right - left) * 0.5;
  const double ry = (bottom - top) * 0.5;
  if (rx <= 0.0 || ry <= 0.0) {
    return;
  }
  const double cx = (left + right) * 0.5;
  const double cy = (top + bottom) * 0.5;

  const int yStart = std::max(0, static_cast<int>(std::ceil(top - 0.5)));
  const int yEndExclusive = std::min(mask.height(), static_cast<int>(std::ceil(bottom - 0.5)));

  for (int y = yStart; y < yEndExclusive; ++y) {
    const double yCoord = static_cast<double>(y) + 0.5;
    const double dy = (yCoord - cy) / ry;
    if (std::abs(dy) > 1.0) {
      continue;
    }
    const double xDelta = rx * std::sqrt(std::max(0.0, 1.0 - dy * dy));
    const double xL = cx - xDelta;
    const double xR = cx + xDelta;
    const int xStart = static_cast<int>(std::ceil(xL - 0.5));
    const int xEndExclusive = static_cast<int>(std::ceil(xR - 0.5));
    applySpanOp(mask, setBits, y, xStart, xEndExclusive);
  }
}

void rasterizeLineStroke(const std::vector<glm::dvec2>& polyline,
                         double strokeWidth,
                         ZBitMask2D& mask,
                         bool setBits)
{
  if (polyline.size() < 2 || strokeWidth <= 0.0) {
    return;
  }
  const double radius = strokeWidth * 0.5;
  // Conservative approach: per-pixel test against all segments in the bounding box.
  // This path is expected to be rare (Line-only ROI).
  ZROIBounds2D b;
  for (const auto& p : polyline) {
    b.expand(p);
  }
  b.minX -= radius;
  b.maxX += radius;
  b.minY -= radius;
  b.maxY += radius;

  const int xStart = std::max(0, static_cast<int>(std::floor(b.minX - 0.5)));
  const int xEndExclusive = std::min(mask.width(), static_cast<int>(std::ceil(b.maxX - 0.5)));
  const int yStart = std::max(0, static_cast<int>(std::floor(b.minY - 0.5)));
  const int yEndExclusive = std::min(mask.height(), static_cast<int>(std::ceil(b.maxY - 0.5)));

  const double r2 = radius * radius;
  for (int y = yStart; y < yEndExclusive; ++y) {
    const double yCoord = static_cast<double>(y) + 0.5;
    for (int x = xStart; x < xEndExclusive; ++x) {
      const double xCoord = static_cast<double>(x) + 0.5;
      const glm::dvec2 p(xCoord, yCoord);
      bool hit = false;
      for (size_t i = 0; i + 1 < polyline.size(); ++i) {
        const glm::dvec2 a = polyline[i];
        const glm::dvec2 c = polyline[i + 1];
        const glm::dvec2 ac = c - a;
        const double denom = glm::dot(ac, ac);
        double t = 0.0;
        if (denom > 0.0) {
          t = glm::clamp(glm::dot(p - a, ac) / denom, 0.0, 1.0);
        }
        const glm::dvec2 proj = a + t * ac;
        const glm::dvec2 d = p - proj;
        if (glm::dot(d, d) <= r2) {
          hit = true;
          break;
        }
      }
      if (hit) {
        if (setBits) {
          mask.set(x, y);
        } else {
          mask.clear(x, y);
        }
      }
    }
  }
}

[[nodiscard]] ZROIBounds2D boundsForOp(const ZROIMaskOperation2D& op, const ZROIMaskRasterizerSettings& settings)
{
  ZROIBounds2D b;
  const auto expandScaled = [&](const glm::dvec2& p) { b.expand(applyScale(p, settings.scaleX, settings.scaleY)); };

  if (op.type == ZROIMaskShapeType::Rect || op.type == ZROIMaskShapeType::Ellipse) {
    if (op.poly.size() < 2) {
      return b;
    }
    expandScaled(op.poly[0]);
    expandScaled(op.poly[1]);
    return b;
  }

  if (op.type == ZROIMaskShapeType::Polygon) {
    for (const auto& p : op.poly) {
      expandScaled(p);
    }
    return b;
  }

  if (op.type == ZROIMaskShapeType::Line) {
    for (const auto& p : op.poly) {
      expandScaled(p);
    }
    if (!b.isEmpty()) {
      const double r = settings.lineStrokeWidth * 0.5;
      b.minX -= r;
      b.maxX += r;
      b.minY -= r;
      b.maxY += r;
    }
    return b;
  }

  if (op.type == ZROIMaskShapeType::Spline) {
    std::vector<glm::dvec2> scaled;
    scaled.reserve(op.poly.size());
    for (const auto& p : op.poly) {
      scaled.push_back(applyScale(p, settings.scaleX, settings.scaleY));
    }
    const bool isClosed = !scaled.empty() && (scaled.front() == scaled.back());
    if ((isClosed && scaled.size() < 4) || (!isClosed && scaled.size() < 3)) {
      for (const auto& p : scaled) {
        b.expand(p);
      }
      return b;
    }

    // Note: For spline bounds we deliberately avoid the tighter extremum-based Bezier bounds
    // computation (roots of derivative) because it can be numerically fragile in degenerate
    // cases (e.g. very small dt segments). Bounding by the Bezier control points is guaranteed
    // to contain the curve (convex-hull property) and is stable.
    const auto beziers = splineToCubicBeziers(scaled);
    for (const auto& bez : beziers) {
      b.expand(bez[0]);
      b.expand(bez[1]);
      b.expand(bez[2]);
      b.expand(bez[3]);
    }
    return b;
  }

  return b;
}

[[nodiscard]] std::vector<glm::dvec2> toHiResPoly(const std::vector<glm::dvec2>& poly,
                                                  const ZROIMaskRasterizerSettings& settings,
                                                  const glm::dvec2& origin,
                                                  int supersample)
{
  std::vector<glm::dvec2> res;
  res.reserve(poly.size());
  for (const auto& p : poly) {
    const glm::dvec2 scaled = applyScale(p, settings.scaleX, settings.scaleY);
    const glm::dvec2 hr = (scaled - origin) * static_cast<double>(supersample);
    res.push_back(hr);
  }
  return res;
}

[[nodiscard]] ZImg downsampleToMask(const ZBitMask2D& hi, int supersample)
{
  CHECK(supersample > 0);
  CHECK(hi.width() % supersample == 0);
  CHECK(hi.height() % supersample == 0);

  const int outW = hi.width() / supersample;
  const int outH = hi.height() / supersample;
  ZImg out(ZImgInfo(static_cast<size_t>(outW), static_cast<size_t>(outH), 1));

  const int total = supersample * supersample;
  const int threshold = (total + 1) / 2; // round(coverage)

  for (int y = 0; y < outH; ++y) {
    for (int x = 0; x < outW; ++x) {
      int count = 0;
      const int baseX = x * supersample;
      const int baseY = y * supersample;
      for (int dy = 0; dy < supersample; ++dy) {
        for (int dx = 0; dx < supersample; ++dx) {
          count += hi.get(baseX + dx, baseY + dy) ? 1 : 0;
        }
      }
      *out.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), 0) = (count >= threshold) ? 1_u8 : 0_u8;
    }
  }
  return out;
}

void trimMaskToTightBBox(ZImg& mask, index_t& xStart, index_t& yStart)
{
  if (mask.isEmpty()) {
    xStart = 0_z;
    yStart = 0_z;
    return;
  }

  int minX = std::numeric_limits<int>::max();
  int minY = std::numeric_limits<int>::max();
  int maxX = -1;
  int maxY = -1;

  for (int y = 0; y < static_cast<int>(mask.height()); ++y) {
    for (int x = 0; x < static_cast<int>(mask.width()); ++x) {
      if (*mask.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), 0) != 0_u8) {
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
      }
    }
  }

  if (maxX < minX || maxY < minY) {
    mask = ZImg();
    xStart = 0_z;
    yStart = 0_z;
    return;
  }

  const ZImgRegion rgn(static_cast<size_t>(minX),
                       static_cast<size_t>(maxX + 1),
                       static_cast<size_t>(minY),
                       static_cast<size_t>(maxY + 1),
                       0,
                       1);
  mask = mask.crop(rgn);
  xStart += static_cast<index_t>(minX);
  yStart += static_cast<index_t>(minY);
}

} // namespace

std::tuple<ZImg, index_t, index_t> ZROIMaskRasterizer::shapeToMask(const std::vector<ZROIMaskOperation2D>& shapeOps,
                                                                   const ZROIMaskRasterizerSettings& settings)
{
  CHECK(settings.supersample > 0);
  if (shapeOps.empty()) {
    return std::make_tuple(ZImg(), 0_z, 0_z);
  }

  // Special-case: a single Line op is treated as a stroked curve (matches existing ZROIUtils behavior).
  if (shapeOps.size() == 1 && shapeOps.front().type == ZROIMaskShapeType::Line) {
    const auto& op = shapeOps.front();
    if (op.poly.size() < 2) {
      return std::make_tuple(ZImg(), 0_z, 0_z);
    }
    ZROIBounds2D b = boundsForOp(op, settings);
    if (b.isEmpty()) {
      return std::make_tuple(ZImg(), 0_z, 0_z);
    }

    const index_t minX = std::max(0_z, static_cast<index_t>(std::floor(b.minX)));
    const index_t minY = std::max(0_z, static_cast<index_t>(std::floor(b.minY)));
    const index_t maxX = static_cast<index_t>(std::ceil(b.maxX));
    const index_t maxY = static_cast<index_t>(std::ceil(b.maxY));
    if (maxX < minX || maxY < minY) {
      return std::make_tuple(ZImg(), 0_z, 0_z);
    }

    const index_t outSW = maxX - minX + 1;
    const index_t outSH = maxY - minY + 1;
    CHECK(outSW > 0_z);
    CHECK(outSH > 0_z);
    CHECK(outSW <= static_cast<index_t>(std::numeric_limits<int>::max()));
    CHECK(outSH <= static_cast<index_t>(std::numeric_limits<int>::max()));

    CHECK(outSW <= std::numeric_limits<index_t>::max() / static_cast<index_t>(settings.supersample));
    CHECK(outSH <= std::numeric_limits<index_t>::max() / static_cast<index_t>(settings.supersample));
    const index_t hrSW = outSW * static_cast<index_t>(settings.supersample);
    const index_t hrSH = outSH * static_cast<index_t>(settings.supersample);
    CHECK(hrSW <= static_cast<index_t>(std::numeric_limits<int>::max()));
    CHECK(hrSH <= static_cast<index_t>(std::numeric_limits<int>::max()));

    // const int outW = static_cast<int>(outSW);
    // const int outH = static_cast<int>(outSH);
    const int hrW = static_cast<int>(hrSW);
    const int hrH = static_cast<int>(hrSH);

    ZBitMask2D hi(hrW, hrH);
    const glm::dvec2 origin(static_cast<double>(minX), static_cast<double>(minY));
    const auto hrPolyline = toHiResPoly(op.poly, settings, origin, settings.supersample);
    rasterizeLineStroke(hrPolyline, settings.lineStrokeWidth * settings.supersample, hi, true);

    ZImg mask = downsampleToMask(hi, settings.supersample);
    index_t xStart = minX;
    index_t yStart = minY;
    trimMaskToTightBBox(mask, xStart, yStart);
    return std::make_tuple(mask, xStart, yStart);
  }

  // Compute bounds from additive ops only (subtractive ops cannot expand the final region).
  ZROIBounds2D b;
  bool hasAdd = false;
  for (const auto& op : shapeOps) {
    if (!op.isAdd) {
      continue;
    }
    if (op.type == ZROIMaskShapeType::Line) {
      // Non-area path; ignore unless it's the only op (handled above).
      continue;
    }
    const ZROIBounds2D opBounds = boundsForOp(op, settings);
    if (opBounds.isEmpty()) {
      continue;
    }
    if (!hasAdd) {
      b = opBounds;
      hasAdd = true;
    } else {
      b.expand(opBounds);
    }
  }

  if (!hasAdd || b.isEmpty()) {
    return std::make_tuple(ZImg(), 0_z, 0_z);
  }

  const index_t minX = std::max(0_z, static_cast<index_t>(std::floor(b.minX)));
  const index_t minY = std::max(0_z, static_cast<index_t>(std::floor(b.minY)));
  const index_t maxX = static_cast<index_t>(std::ceil(b.maxX));
  const index_t maxY = static_cast<index_t>(std::ceil(b.maxY));
  if (maxX < minX || maxY < minY) {
    return std::make_tuple(ZImg(), 0_z, 0_z);
  }

  const index_t outSW = maxX - minX + 1;
  const index_t outSH = maxY - minY + 1;
  CHECK(outSW > 0_z);
  CHECK(outSH > 0_z);
  CHECK(outSW <= static_cast<index_t>(std::numeric_limits<int>::max()));
  CHECK(outSH <= static_cast<index_t>(std::numeric_limits<int>::max()));

  CHECK(outSW <= std::numeric_limits<index_t>::max() / static_cast<index_t>(settings.supersample));
  CHECK(outSH <= std::numeric_limits<index_t>::max() / static_cast<index_t>(settings.supersample));
  const index_t hrSW = outSW * static_cast<index_t>(settings.supersample);
  const index_t hrSH = outSH * static_cast<index_t>(settings.supersample);
  CHECK(hrSW <= static_cast<index_t>(std::numeric_limits<int>::max()));
  CHECK(hrSH <= static_cast<index_t>(std::numeric_limits<int>::max()));

  // const int outW = static_cast<int>(outSW);
  // const int outH = static_cast<int>(outSH);
  const int hrW = static_cast<int>(hrSW);
  const int hrH = static_cast<int>(hrSH);

  ZBitMask2D hi(hrW, hrH);
  const glm::dvec2 origin(static_cast<double>(minX), static_cast<double>(minY));

  // Spline flattening tolerance: quarter of a supersampled pixel.
  // This ties approximation quality to the chosen supersample factor.
  const double tol = 0.25;

  for (const auto& op : shapeOps) {
    const bool setBits = op.isAdd;
    if (op.type == ZROIMaskShapeType::Rect) {
      if (op.poly.size() < 2) {
        continue;
      }
      const auto p0 = applyScale(op.poly[0], settings.scaleX, settings.scaleY);
      const auto p1 = applyScale(op.poly[1], settings.scaleX, settings.scaleY);
      const glm::dvec2 hr0 = (p0 - origin) * static_cast<double>(settings.supersample);
      const glm::dvec2 hr1 = (p1 - origin) * static_cast<double>(settings.supersample);
      rasterizeRect(hr0, hr1, hi, setBits);
    } else if (op.type == ZROIMaskShapeType::Ellipse) {
      if (op.poly.size() < 2) {
        continue;
      }
      const auto p0 = applyScale(op.poly[0], settings.scaleX, settings.scaleY);
      const auto p1 = applyScale(op.poly[1], settings.scaleX, settings.scaleY);
      const glm::dvec2 hr0 = (p0 - origin) * static_cast<double>(settings.supersample);
      const glm::dvec2 hr1 = (p1 - origin) * static_cast<double>(settings.supersample);
      rasterizeEllipse(hr0, hr1, hi, setBits);
    } else if (op.type == ZROIMaskShapeType::Polygon) {
      if (op.poly.size() < 3) {
        continue;
      }
      auto hrPoly = toHiResPoly(op.poly, settings, origin, settings.supersample);
      // For closed polygons stored with a duplicate endpoint, drop the final point to avoid a zero-length edge.
      if (hrPoly.size() >= 2 && hrPoly.front() == hrPoly.back()) {
        hrPoly.pop_back();
      }
      rasterizePolygonEvenOdd(hrPoly, hi, setBits);
    } else if (op.type == ZROIMaskShapeType::Spline) {
      if (op.poly.size() < 2) {
        continue;
      }
      std::vector<glm::dvec2> scaled;
      scaled.reserve(op.poly.size());
      for (const auto& p : op.poly) {
        scaled.push_back(applyScale(p, settings.scaleX, settings.scaleY));
      }
      auto polyline = splineToPolyline(scaled, tol / static_cast<double>(settings.supersample));
      auto hrPolyline = toHiResPoly(polyline, settings, origin, settings.supersample);
      if (hrPolyline.size() >= 2 && hrPolyline.front() == hrPolyline.back()) {
        hrPolyline.pop_back();
      }
      rasterizePolygonEvenOdd(hrPolyline, hi, setBits);
    } else if (op.type == ZROIMaskShapeType::Line) {
      // Line-only handled above; here it contributes no filled area.
      continue;
    }
  }

  ZImg mask = downsampleToMask(hi, settings.supersample);
  index_t xStart = minX;
  index_t yStart = minY;
  trimMaskToTightBBox(mask, xStart, yStart);
  return std::make_tuple(mask, xStart, yStart);
}

} // namespace nim
