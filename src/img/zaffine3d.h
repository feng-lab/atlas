#pragma once

#include "zeigenutils.h"

namespace nim {

class ZAffine3D
{
public:
  ZAffine3D() = default;

  ZAffine3D(double m11, double m12, double m13, double m14,
            double m21, double m22, double m23, double m24,
            double m31, double m32, double m33, double m34)
  {
    setMatrix(m11, m12, m13, m14, m21, m22, m23, m24, m31, m32, m33, m34);
  }

  // must call makeMatrix to update, shear -> scale -> rotate -> translate
  void setTranslation(double x, double y, double z)
  {
    m_translationX = x;
    m_translationY = y;
    m_translationZ = z;
  }

  void setRotationAngle(double radianAngleXY, double radianAngleXZ, double radianAngleYZ)
  {
    m_rotateAngleXY = radianAngleXY;
    m_rotateAngleXZ = radianAngleXZ;
    m_rotateAngleYZ = radianAngleYZ;
  }

  void setScale(double xscale, double yscale, double zscale)
  {
    m_scaleX = xscale;
    m_scaleY = yscale;
    m_scaleZ = zscale;
  }

  void setShear(double xy, double xz, double yx, double yz, double zx, double zy)
  {
    m_shearXY = xy;
    m_shearXZ = xz;
    m_shearYX = yx;
    m_shearYZ = yz;
    m_shearZX = zx;
    m_shearZY = zy;
  }

  void makeMatrix();

  [[nodiscard]] double translationX() const
  { return m_translationX; }

  [[nodiscard]] double translationY() const
  { return m_translationY; }

  [[nodiscard]] double translationZ() const
  { return m_translationZ; }

  [[nodiscard]] double rotationAngleXY() const
  { return m_rotateAngleXY; }

  [[nodiscard]] double rotationAngleXZ() const
  { return m_rotateAngleXZ; }

  [[nodiscard]] double rotationAngleYZ() const
  { return m_rotateAngleYZ; }

  [[nodiscard]] double scaleX() const
  { return m_scaleX; }

  [[nodiscard]] double scaleY() const
  { return m_scaleY; }

  [[nodiscard]] double scaleZ() const
  { return m_scaleZ; }

  [[nodiscard]] double shearXY() const
  { return m_shearXY; }

  [[nodiscard]] double shearXZ() const
  { return m_shearXZ; }

  [[nodiscard]] double shearYX() const
  { return m_shearYX; }

  [[nodiscard]] double shearYZ() const
  { return m_shearYZ; }

  [[nodiscard]] double shearZX() const
  { return m_shearZX; }

  [[nodiscard]] double shearZY() const
  { return m_shearZY; }

  //
  void setMatrix(double m11, double m12, double m13, double m14,
                 double m21, double m22, double m23, double m24,
                 double m31, double m32, double m33, double m34);

  // access
  [[nodiscard]] const Eigen::Matrix4d& transformMatrix() const
  { return m_matrix; }

  [[nodiscard]] const Eigen::Matrix4d& inverseTransformMatrix() const
  { return m_inverseMatrix; }

  //
  void reset();

  void invert();

  void transformPointsForward(double u, double v, double w, double& x, double& y, double& z) const;

  void transformPointsInverse(double x, double y, double z, double& u, double& v, double& w) const;

  [[nodiscard]] QString toQString() const;

protected:
  [[nodiscard]] Eigen::Matrix4d getInverseTransformMatrix() const;

private:
  Eigen::Matrix4d m_matrix = Eigen::Matrix4d::Identity();
  Eigen::Matrix4d m_inverseMatrix = Eigen::Matrix4d::Identity();

  double m_translationX = 0;
  double m_translationY = 0;
  double m_translationZ = 0;
  double m_scaleX = 1;
  double m_scaleY = 1;
  double m_scaleZ = 1;
  double m_rotateAngleXY = 0;
  double m_rotateAngleXZ = 0;
  double m_rotateAngleYZ = 0;
  double m_shearXY = 0;
  double m_shearXZ = 0;
  double m_shearYX = 0;
  double m_shearYZ = 0;
  double m_shearZX = 0;
  double m_shearZY = 0;
};

} // namespace nim

