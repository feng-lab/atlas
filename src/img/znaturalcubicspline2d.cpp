#include "znaturalcubicspline2d.h"

#include "zeigenutils.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace nim {

namespace {

[[nodiscard]] glm::dvec2 rowToVec2(const Eigen::MatrixXd& matrix, Eigen::Index row)
{
  return glm::dvec2(matrix(row, 0), matrix(row, 1));
}

void fillRightHandSideRow(Eigen::MatrixXd& rhs, Eigen::Index row, const glm::dvec2& value)
{
  rhs(row, 0) = value.x;
  rhs(row, 1) = value.y;
}

[[nodiscard]] bool hasFinitePoints(const std::vector<glm::dvec2>& points)
{
  return std::all_of(points.begin(), points.end(), [](const glm::dvec2& point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
  });
}

} // namespace

ZNaturalCubicSpline2D::ZNaturalCubicSpline2D(bool isFree, std::vector<glm::dvec2> points, std::vector<double> times)
  : m_points(std::move(points))
  , m_times(std::move(times))
{
  CHECK(m_points.size() == m_times.size());
  CHECK(m_points.size() >= 2);
  CHECK(hasFinitePoints(m_points));
  CHECK(hasStrictlyIncreasingTimes(m_times));
  CHECK(isFree || m_points.size() >= 4);
  CHECK(isFree || m_points.front() == m_points.back());

  const size_t numS = numSegments();
  m_b.resize(numS);
  m_c.resize(numS + 1);
  m_d.resize(numS);

  if (isFree) {
    createFree();
  } else {
    createClosed();
  }
}

size_t ZNaturalCubicSpline2D::numPoints() const
{
  return m_points.size();
}

size_t ZNaturalCubicSpline2D::numSegments() const
{
  CHECK(!m_points.empty());
  return m_points.size() - 1;
}

const std::vector<glm::dvec2>& ZNaturalCubicSpline2D::points() const
{
  return m_points;
}

const std::vector<double>& ZNaturalCubicSpline2D::times() const
{
  return m_times;
}

glm::dvec2 ZNaturalCubicSpline2D::position(double t) const
{
  const auto [key, dt] = keyInfo(t);
  return m_points[key] + dt * (m_b[key] + dt * (m_c[key] + dt * m_d[key]));
}

glm::dvec2 ZNaturalCubicSpline2D::derivative(double t) const
{
  const auto [key, dt] = keyInfo(t);
  return m_b[key] + dt * (2.0 * m_c[key] + 3.0 * dt * m_d[key]);
}

std::vector<ZNaturalCubicSpline2D::CubicBezier> ZNaturalCubicSpline2D::toCubicBeziers() const
{
  std::vector<CubicBezier> beziers;
  const size_t numS = numSegments();
  beziers.reserve(numS);
  for (size_t i = 0; i < numS; ++i) {
    const double dt = m_times[i + 1] - m_times[i];
    const glm::dvec2 d1 = (i + 1 < numS) ? m_b[i + 1] : m_b[i] + dt * (2.0 * m_c[i] + 3.0 * dt * m_d[i]);
    const glm::dvec2 m0 = dt * m_b[i];
    const glm::dvec2 m1 = dt * d1;
    beziers.push_back(CubicBezier{m_points[i], m_points[i] + m0 / 3.0, m_points[i + 1] - m1 / 3.0, m_points[i + 1]});
  }
  return beziers;
}

std::vector<double> ZNaturalCubicSpline2D::chordLengthTimes(const std::vector<glm::dvec2>& points)
{
  std::vector<double> times(points.size(), 0.0);
  for (size_t i = 1; i < points.size(); ++i) {
    times[i] = times[i - 1] + glm::length(points[i] - points[i - 1]);
  }
  return times;
}

bool ZNaturalCubicSpline2D::hasStrictlyIncreasingTimes(const std::vector<double>& times)
{
  for (size_t i = 1; i < times.size(); ++i) {
    if (!(times[i] > times[i - 1]) || !std::isfinite(times[i])) {
      return false;
    }
  }
  return !times.empty() && std::isfinite(times.front());
}

void ZNaturalCubicSpline2D::compactConsecutiveDuplicatePointsInPlace(std::vector<glm::dvec2>& points)
{
  points.erase(std::unique(points.begin(), points.end()), points.end());
}

std::vector<glm::dvec2> ZNaturalCubicSpline2D::compactConsecutiveDuplicatePoints(std::vector<glm::dvec2> points)
{
  compactConsecutiveDuplicatePointsInPlace(points);
  return points;
}

std::vector<ZNaturalCubicSpline2D::CubicBezier> ZNaturalCubicSpline2D::fitChordLength(std::vector<glm::dvec2> points)
{
  // ROI files can contain repeated consecutive control points, which create
  // zero-length chord intervals. They do not change the curve shape.
  compactConsecutiveDuplicatePointsInPlace(points);
  if (points.size() < 2) {
    return {};
  }

  const bool isClosed = (points.front() == points.back());
  if ((isClosed && points.size() < 4) || (!isClosed && points.size() < 3)) {
    return {};
  }

  std::vector<double> times = chordLengthTimes(points);
  if (!hasStrictlyIncreasingTimes(times)) {
    return {};
  }

  return ZNaturalCubicSpline2D(!isClosed, std::move(points), std::move(times)).toCubicBeziers();
}

void ZNaturalCubicSpline2D::createFree()
{
  const size_t numP = numPoints();
  const size_t numS = numSegments();

  Eigen::MatrixXd matrix = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(numP), static_cast<Eigen::Index>(numP));
  Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(numP), 2);

  matrix(0, 0) = 1.0;
  matrix(static_cast<Eigen::Index>(numS), static_cast<Eigen::Index>(numS)) = 1.0;

  for (size_t i = 1; i < numS; ++i) {
    const double dt0 = m_times[i] - m_times[i - 1];
    const double dt1 = m_times[i + 1] - m_times[i];
    const Eigen::Index row = static_cast<Eigen::Index>(i);
    matrix(row, static_cast<Eigen::Index>(i - 1)) = dt0;
    matrix(row, row) = 2.0 * (dt0 + dt1);
    matrix(row, static_cast<Eigen::Index>(i + 1)) = dt1;
    fillRightHandSideRow(rhs,
                         row,
                         3.0 * ((m_points[i + 1] - m_points[i]) / dt1 - (m_points[i] - m_points[i - 1]) / dt0));
  }

  auto lu = matrix.fullPivLu();
  CHECK(lu.isInvertible());
  const Eigen::MatrixXd solution = lu.solve(rhs);
  for (size_t i = 0; i < numP; ++i) {
    m_c[i] = rowToVec2(solution, static_cast<Eigen::Index>(i));
  }

  for (size_t i = 0; i < numS; ++i) {
    const double dt = m_times[i + 1] - m_times[i];
    m_b[i] = (m_points[i + 1] - m_points[i]) / dt - dt * (m_c[i + 1] + 2.0 * m_c[i]) / 3.0;
    m_d[i] = (m_c[i + 1] - m_c[i]) / (3.0 * dt);
  }
}

void ZNaturalCubicSpline2D::createClosed()
{
  const size_t numP = numPoints();
  const size_t numS = numSegments();
  const size_t numSm1 = numS - 1;

  Eigen::MatrixXd matrix = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(numP), static_cast<Eigen::Index>(numP));
  Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(numP), 2);

  matrix(0, 0) = 1.0;
  matrix(0, static_cast<Eigen::Index>(numS)) = -1.0;

  for (size_t i = 1; i <= numSm1; ++i) {
    const double dt0 = m_times[i] - m_times[i - 1];
    const double dt1 = m_times[i + 1] - m_times[i];
    const Eigen::Index row = static_cast<Eigen::Index>(i);
    matrix(row, static_cast<Eigen::Index>(i - 1)) = dt0;
    matrix(row, row) = 2.0 * (dt0 + dt1);
    matrix(row, static_cast<Eigen::Index>(i + 1)) = dt1;
    fillRightHandSideRow(rhs,
                         row,
                         3.0 * ((m_points[i + 1] - m_points[i]) / dt1 - (m_points[i] - m_points[i - 1]) / dt0));
  }

  const double dtLast = m_times[numS] - m_times[numS - 1];
  const double dt0 = m_times[1] - m_times[0];
  const Eigen::Index lastRow = static_cast<Eigen::Index>(numS);
  matrix(lastRow, static_cast<Eigen::Index>(numSm1)) = dtLast;
  matrix(lastRow, 0) = 2.0 * (dtLast + dt0);
  matrix(lastRow, 1) = dt0;
  fillRightHandSideRow(rhs,
                       lastRow,
                       3.0 * ((m_points[1] - m_points[0]) / dt0 - (m_points[0] - m_points[numSm1]) / dtLast));

  auto lu = matrix.fullPivLu();
  CHECK(lu.isInvertible());
  const Eigen::MatrixXd solution = lu.solve(rhs);
  for (size_t i = 0; i < numP; ++i) {
    m_c[i] = rowToVec2(solution, static_cast<Eigen::Index>(i));
  }

  for (size_t i = 0; i < numS; ++i) {
    const double dt = m_times[i + 1] - m_times[i];
    m_b[i] = (m_points[i + 1] - m_points[i]) / dt - dt * (m_c[i + 1] + 2.0 * m_c[i]) / 3.0;
    m_d[i] = (m_c[i + 1] - m_c[i]) / (3.0 * dt);
  }
}

std::pair<size_t, double> ZNaturalCubicSpline2D::keyInfo(double t) const
{
  const size_t numS = numSegments();
  if (t <= m_times.front()) {
    return {0, 0.0};
  }
  if (t >= m_times.back()) {
    return {numS - 1, m_times[numS] - m_times[numS - 1]};
  }

  const auto upper = std::upper_bound(m_times.begin(), m_times.end(), t);
  CHECK(upper != m_times.begin());
  CHECK(upper != m_times.end());
  const size_t key = static_cast<size_t>(std::distance(m_times.begin(), upper) - 1);
  return {key, t - m_times[key]};
}

} // namespace nim
