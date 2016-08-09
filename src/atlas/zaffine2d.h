#pragma once

#include "zeigenutils.h"

namespace nim {

class ZAffine2D
{
public:
  ZAffine2D();

  ZAffine2D(double m11, double m12, double m13, double m21, double m22, double m23);

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

  double translationX() const
  { return m_translationX; }

  double translationY() const
  { return m_translationY; }

  double rotationAngle() const
  { return m_rotateAngle; }

  double scaleX() const
  { return m_scaleX; }

  double scaleY() const
  { return m_scaleY; }

  double shearXY() const
  { return m_shearXY; }

  double shearYX() const
  { return m_shearYX; }

  //
  void setMatrix(double m11, double m12, double m13, double m21, double m22, double m23);

  // access
  const Eigen::Matrix3d& transformMatrix() const
  { return m_matrix; }

  const Eigen::Matrix3d& inverseTransformMatrix() const
  { return m_inverseMatrix; }

  //
  void reset();

  void invert();

  void transformPointsForward(double u, double v, double& x, double& y) const;

  void transformPointsInverse(double x, double y, double& u, double& v) const;

  QString toQString() const;

protected:
  Eigen::Matrix3d getInverseTransformMatrix() const;

private:
  Eigen::Matrix3d m_matrix;
  Eigen::Matrix3d m_inverseMatrix;

  double m_translationX;
  double m_translationY;
  double m_scaleX;
  double m_scaleY;
  double m_rotateAngle;
  double m_shearXY;
  double m_shearYX;
};

} // namespace nim

