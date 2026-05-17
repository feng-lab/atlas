#pragma once

#include "zglmutils.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace nim {

class ZNaturalCubicSpline2D
{
public:
  struct CubicBezier
  {
    glm::dvec2 p0;
    glm::dvec2 p1;
    glm::dvec2 p2;
    glm::dvec2 p3;
  };

  ZNaturalCubicSpline2D(bool isFree, std::vector<glm::dvec2> points, std::vector<double> times);

  [[nodiscard]] size_t numPoints() const;
  [[nodiscard]] size_t numSegments() const;
  [[nodiscard]] const std::vector<glm::dvec2>& points() const;
  [[nodiscard]] const std::vector<double>& times() const;

  [[nodiscard]] glm::dvec2 position(double t) const;
  [[nodiscard]] glm::dvec2 derivative(double t) const;
  [[nodiscard]] std::vector<CubicBezier> toCubicBeziers() const;

  [[nodiscard]] static std::vector<double> chordLengthTimes(const std::vector<glm::dvec2>& points);
  [[nodiscard]] static bool hasStrictlyIncreasingTimes(const std::vector<double>& times);
  static void compactConsecutiveDuplicatePointsInPlace(std::vector<glm::dvec2>& points);
  [[nodiscard]] static std::vector<glm::dvec2> compactConsecutiveDuplicatePoints(std::vector<glm::dvec2> points);
  [[nodiscard]] static std::vector<CubicBezier> fitChordLength(std::vector<glm::dvec2> points);

private:
  void createFree();
  void createClosed();
  [[nodiscard]] std::pair<size_t, double> keyInfo(double t) const;

  std::vector<glm::dvec2> m_points;
  std::vector<double> m_times;
  std::vector<glm::dvec2> m_b;
  std::vector<glm::dvec2> m_c;
  std::vector<glm::dvec2> m_d;
};

} // namespace nim
