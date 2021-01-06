#pragma once

#include "zeigenutils.h"
#include "zimginterface.h"
#include <QString>
#include <fmt/core.h>
#include <cmath>
#include <list>

namespace nim {

class ZPunctum
{
public:
  ZPunctum() = default;

  ZPunctum(double x, double y, double z, double r);

  ZPunctum(const Eigen::MatrixXi& loc, const Eigen::VectorXd& inten);

  ZPunctum(ZPunctum&&) = default;

  ZPunctum& operator=(ZPunctum&&) = default;

  ZPunctum(const ZPunctum&) = default;

  ZPunctum& operator=(const ZPunctum&) = default;

  void swap(ZPunctum& rhs) noexcept
  {
    m_name.swap(rhs.m_name);
    m_comment.swap(rhs.m_comment);
    std::swap(m_maxIntensity, rhs.m_maxIntensity);
    std::swap(m_meanIntensity, rhs.m_meanIntensity);
    std::swap(m_x, rhs.m_x);
    std::swap(m_y, rhs.m_y);
    std::swap(m_z, rhs.m_z);
    std::swap(m_sDevOfIntensity, rhs.m_sDevOfIntensity);
    std::swap(m_volSize, rhs.m_volSize);
    std::swap(m_mass, rhs.m_mass);
    std::swap(m_radius, rhs.m_radius);
    m_property1.swap(rhs.m_property1);
    m_property2.swap(rhs.m_property2);
    m_property3.swap(rhs.m_property3);
    std::swap(m_color, rhs.m_color);
    std::swap(m_score, rhs.m_score);
    m_voxelLocations.swap(rhs.m_voxelLocations);
    m_voxelIntensities.swap(rhs.m_voxelIntensities);
    std::swap(m_selected, rhs.m_selected);
  }

  bool operator==(const ZPunctum& rhs) const
  {
    return m_name == rhs.m_name &&
           m_comment == rhs.m_comment &&
           m_maxIntensity == rhs.m_maxIntensity &&
           m_meanIntensity == rhs.m_meanIntensity &&
           m_x == rhs.m_x &&
           m_y == rhs.m_y &&
           m_z == rhs.m_z &&
           m_sDevOfIntensity == rhs.m_sDevOfIntensity &&
           m_volSize == rhs.m_volSize &&
           m_mass == rhs.m_mass &&
           m_radius == rhs.m_radius &&
           m_property1 == rhs.m_property1 &&
           m_property2 == rhs.m_property2 &&
           m_property3 == rhs.m_property3 &&
           m_color == rhs.m_color &&
           m_score == rhs.m_score &&
           m_voxelLocations == rhs.m_voxelLocations &&
           m_voxelIntensities == rhs.m_voxelIntensities;
  }

  inline bool operator!=(const ZPunctum& rhs) const
  { return !(*this == rhs); }

  // compute fields: x, y, z, sDevOfIntensity, maxIntensity, meanIntensity, volSize,
  // mass, radius, score from voxels list. voxelLocations and voxelIntensities must
  // not be empty and must have same number of elements.
  // parameter conf is used to estimate radius of punctum (model punctum as gaussian)
  void updateFromVoxelsList(double conf = 0.95);

  [[nodiscard]] inline bool containsSignal() const
  { return m_voxelLocations.rows() > 0 && m_voxelLocations.rows() == m_voxelIntensities.size(); }

  // merge input puncta into one punctum,
  // if one of the input puncta doesn't have signal, other puncta's signal will be ignored and the merged
  // punctum will not have signal.
  // after merging, returned punctum will have detection score of 1.0
  template<class InputIterator>
  static ZPunctum merge(InputIterator first, InputIterator last, double conf = 0.95);

  // merge input punctum into current punctum, if input punctum don't have signal while current punctum has, do nothing
  // after merging, current punctum will have detection score of 1.0
  void merge(const ZPunctum& other, double conf = 0.95);

  // use gmm to split this punctum into num puncta, image signal (voxelLocations and voxelIntensities) must exist, otherwise
  // return empty list. depends on image signal, returned list size might be less than num
  [[nodiscard]] std::list<ZPunctum> split(int num, double conf = 0.95) const;

  [[nodiscard]] inline double x() const
  { return m_x; }

  [[nodiscard]] inline double y() const
  { return m_y; }

  [[nodiscard]] inline double z() const
  { return m_z; }

  [[nodiscard]] inline double sDevOfIntensity() const
  { return m_sDevOfIntensity; }

  [[nodiscard]] inline double maxIntensity() const
  { return m_maxIntensity; }

  [[nodiscard]] inline double meanIntensity() const
  { return m_meanIntensity; }

  [[nodiscard]] inline size_t volSize() const
  { return m_volSize; }

  [[nodiscard]] inline double mass() const
  { return m_mass; }

  [[nodiscard]] inline double radius() const
  { return m_radius; }

  [[nodiscard]] inline const QString& name() const
  { return m_name; }

  [[nodiscard]] inline const QString& comment() const
  { return m_comment; }

  [[nodiscard]] inline const QString& property1() const
  { return m_property1; }

  [[nodiscard]] inline const QString& property2() const
  { return m_property2; }

  [[nodiscard]] inline const QString& property3() const
  { return m_property3; }

  [[nodiscard]] inline const col4& color() const
  { return m_color; }

  [[nodiscard]] inline double score() const
  { return m_score; }

  [[nodiscard]] inline const Eigen::MatrixXi& voxelLocations() const
  { return m_voxelLocations; }

  [[nodiscard]] inline const Eigen::VectorXd& voxelIntensities() const
  { return m_voxelIntensities; }

  inline void setX(double n)
  { m_x = n; }

  inline void setY(double n)
  { m_y = n; }

  inline void setZ(double n)
  { m_z = n; }

  inline void setSDevOfIntensity(double n)
  { m_sDevOfIntensity = n; }

  inline void setMaxIntensity(double n)
  { m_maxIntensity = n; }

  inline void setMeanIntensity(double n)
  { m_meanIntensity = n; }

  inline void setVolSize(size_t n)
  { m_volSize = std::max(0_usize, n); }

  inline void setMass(double n)
  { m_mass = n; }

  inline void setRadius(double n)
  { m_radius = n; }

  inline void setName(const QString& n)
  { m_name = n; }

  inline void setComment(const QString& n)
  { m_comment = n; }

  inline void setProperty1(const QString& n)
  { m_property1 = n; }

  inline void setProperty2(const QString& n)
  { m_property2 = n; }

  inline void setProperty3(const QString& n)
  { m_property3 = n; }

  inline void setColor(const col4& n)
  { m_color = n; }

  inline void setVoxelLocations(const Eigen::MatrixXi& l)
  { m_voxelLocations = l; }

  inline void setVoxelIntensities(const Eigen::VectorXd& i)
  { m_voxelIntensities = i; }

  inline void setScore(double s)
  { m_score = s; }

  void translate(double dx, double dy, double dz);

  // update radius from volSize
  inline void updateRadius()
  { using namespace boost::math::double_constants; m_radius = std::pow(m_volSize / four_thirds_pi, 1.0 / 3.0); }

  // update volSize from radius
  inline void updateVolSize()
  { using namespace boost::math::double_constants; m_volSize = four_thirds_pi * m_radius * m_radius * m_radius; }

  // update mass
  inline void updateMass()
  { m_mass = m_volSize * m_meanIntensity; }

  [[nodiscard]] inline QString toQString() const
  { return QString("Puncta (%1): (%2, %3, %4, %5)").arg(m_name).arg(m_x).arg(m_y).arg(m_z).arg(m_radius); }

  [[nodiscard]] inline std::string toString() const
  { return fmt::format("Puncta ({}): ({}, {}, {}, {})", m_name.toStdString(), m_x, m_y, m_z, m_radius); }

  [[nodiscard]] inline bool isSelected() const
  { return m_selected; }

  inline void setSelected(bool v)
  { m_selected = v; }

private:
  QString m_name;
  QString m_comment;
  double m_maxIntensity = 255;
  double m_meanIntensity = 255;
  double m_x = -1;
  double m_y = -1;
  double m_z = -1;
  double m_sDevOfIntensity = 0;
  size_t m_volSize = 33;
  double m_mass = 8545.13201776424;
  double m_radius = 2.0;   //radius
  QString m_property1;
  QString m_property2;
  QString m_property3;
  col4 m_color{0, 255, 255};
  double m_score = 1.0;  // detection score [-1.0 1.0]

  // info of voxels belong to this punctum
  Eigen::MatrixXi m_voxelLocations;   // n x 3 matrix
  Eigen::VectorXd m_voxelIntensities;

  bool m_selected = false;
};

//   template  //
template<class InputIterator>
ZPunctum ZPunctum::merge(InputIterator first, InputIterator last, double conf)
{
  CHECK(first != last);
  bool hasSignal = true;
  InputIterator it = first;
  for (; it != last; ++it) {
    hasSignal = hasSignal && it->containsSignal();
    if (!hasSignal)
      break;
  }
  ZPunctum res(*first);
  if (hasSignal) {
    it = first;
    ++it;
    for (; it != last; ++it) {
      res.m_voxelIntensities.conservativeResize(res.m_voxelIntensities.size() + (*it).m_voxelIntensities.size());
      res.m_voxelIntensities.tail((*it).m_voxelIntensities.size()) = (*it).m_voxelIntensities;
      res.m_voxelLocations.conservativeResize(res.m_voxelLocations.rows() + (*it).m_voxelLocations.rows(),
                                              Eigen::NoChange);
      res.m_voxelLocations.bottomRows((*it).m_voxelLocations.rows()) = (*it).m_voxelLocations;
    }
    res.updateFromVoxelsList(conf);
  } else {
    res.m_voxelLocations = Eigen::MatrixXi();
    res.m_voxelIntensities = Eigen::VectorXd();
    it = first;
    ++it;
    res.m_x *= res.m_volSize;
    res.m_y *= res.m_volSize;
    res.m_z *= res.m_volSize;
    for (; it != last; ++it) {
      res.m_volSize += (*it).m_volSize;
      res.m_sDevOfIntensity = std::max((*it).m_sDevOfIntensity, res.m_sDevOfIntensity);  // no better way..
      res.m_x += (*it).m_x * (*it).m_volSize;
      res.m_y += (*it).m_y * (*it).m_volSize;
      res.m_z += (*it).m_z * (*it).m_volSize;
      res.m_maxIntensity = std::max((*it).m_maxIntensity, res.m_maxIntensity);
      res.m_mass += (*it).m_mass;
    }
    res.m_x /= res.m_volSize;
    res.m_y /= res.m_volSize;
    res.m_z /= res.m_volSize;
    res.m_meanIntensity = res.mass() / res.volSize();
    res.m_score = 1.0;
    res.updateRadius();
  }
  return res;
}

} // namespace nim

