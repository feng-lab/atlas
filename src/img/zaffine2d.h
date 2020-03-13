#pragma once

#include "zeigenutils.h"

namespace nim {

class ZAffine2D
{
public:
  ZAffine2D() = default;

  ZAffine2D(double m11, double m12, double m13, double m21, double m22, double m23)
  { setMatrix(m11, m12, m13, m21, m22, m23); }

  // must call makeMatrix to update, shear -> scale -> rotate -> translate
  void setTranslation(double x, double y)
  {
    m_translationX = x;
    m_translationY = y;
  }

  void setRotationAngle(double radianAngle)
  { m_rotateAngle = radianAngle; }

  void setScale(double xscale, double yscale)
  {
    m_scaleX = xscale;
    m_scaleY = yscale;
  }

  void setShear(double xy, double yx)
  {
    m_shearXY = xy;
    m_shearYX = yx;
  }

  void makeMatrix();

  [[nodiscard]] double translationX() const
  { return m_translationX; }

  [[nodiscard]] double translationY() const
  { return m_translationY; }

  [[nodiscard]] double rotationAngle() const
  { return m_rotateAngle; }

  [[nodiscard]] double scaleX() const
  { return m_scaleX; }

  [[nodiscard]] double scaleY() const
  { return m_scaleY; }

  [[nodiscard]] double shearXY() const
  { return m_shearXY; }

  [[nodiscard]] double shearYX() const
  { return m_shearYX; }

  //
  void setMatrix(double m11, double m12, double m13, double m21, double m22, double m23);

  // access
  [[nodiscard]] const Eigen::Matrix3d& transformMatrix() const
  { return m_matrix; }

  [[nodiscard]] const Eigen::Matrix3d& inverseTransformMatrix() const
  { return m_inverseMatrix; }

  //
  void reset();

  void invert();

  void transformPointsForward(double u, double v, double& x, double& y) const;

  void transformPointsInverse(double x, double y, double& u, double& v) const;

  [[nodiscard]] QString toQString() const;

protected:
  [[nodiscard]] Eigen::Matrix3d getInverseTransformMatrix() const;

private:
  Eigen::Matrix3d m_matrix = Eigen::Matrix3d::Identity();
  Eigen::Matrix3d m_inverseMatrix = Eigen::Matrix3d::Identity();

  double m_translationX = 0;
  double m_translationY = 0;
  double m_scaleX = 1;
  double m_scaleY = 1;
  double m_rotateAngle = 0;
  double m_shearXY = 0;
  double m_shearYX = 0;
};

} // namespace nim

