#ifndef ZAFFINE3D_H
#define ZAFFINE3D_H

#include "zeigenutils.h"

namespace nim {

class ZAffine3D
{
public:
  ZAffine3D();
  ZAffine3D(double m11, double m12, double m13, double m14,
            double m21, double m22, double m23, double m24,
            double m31, double m32, double m33, double m34);

  // must call makeMatrix to update, shear -> scale -> rotate -> translate
  void setTranslation(double x, double y, double z)
  { m_translationX = x; m_translationY = y; m_translationZ = z; }
  void setRotationAngle(double radianAngleXY, double radianAngleXZ, double radianAngleYZ)
  { m_rotateAngleXY = radianAngleXY; m_rotateAngleXZ = radianAngleXZ; m_rotateAngleYZ = radianAngleYZ; }
  void setScale(double xscale, double yscale, double zscale)
  { m_scaleX = xscale; m_scaleY = yscale; m_scaleZ = zscale; }
  void setShear(double xy, double xz, double yx, double yz, double zx, double zy)
  { m_shearXY = xy; m_shearXZ = xz; m_shearYX = yx; m_shearYZ = yz; m_shearZX = zx; m_shearZY = zy; }
  void makeMatrix();

  double translationX() const { return m_translationX; }
  double translationY() const { return m_translationY; }
  double translationZ() const { return m_translationZ; }
  double rotationAngleXY() const { return m_rotateAngleXY; }
  double rotationAngleXZ() const { return m_rotateAngleXZ; }
  double rotationAngleYZ() const { return m_rotateAngleYZ; }
  double scaleX() const { return m_scaleX; }
  double scaleY() const { return m_scaleY; }
  double scaleZ() const { return m_scaleZ; }
  double shearXY() const { return m_shearXY; }
  double shearXZ() const { return m_shearXZ; }
  double shearYX() const { return m_shearYX; }
  double shearYZ() const { return m_shearYZ; }
  double shearZX() const { return m_shearZX; }
  double shearZY() const { return m_shearZY; }

  //
  void setMatrix(double m11, double m12, double m13, double m14,
                 double m21, double m22, double m23, double m24,
                 double m31, double m32, double m33, double m34);

  // access
  const Eigen::Matrix4d& transformMatrix() const { return m_matrix; }
  const Eigen::Matrix4d& inverseTransformMatrix() const { return m_inverseMatrix; }

  //
  void reset();
  void invert();
  void transformPointsForward(double u, double v, double w, double &x, double &y, double &z) const;
  void transformPointsInverse(double x, double y, double z, double &u, double &v, double &w) const;
  QString toQString() const;

protected:
  Eigen::Matrix4d getInverseTransformMatrix() const;

private:
  Eigen::Matrix4d m_matrix;
  Eigen::Matrix4d m_inverseMatrix;

  double m_translationX;
  double m_translationY;
  double m_translationZ;
  double m_scaleX;
  double m_scaleY;
  double m_scaleZ;
  double m_rotateAngleXY;
  double m_rotateAngleXZ;
  double m_rotateAngleYZ;
  double m_shearXY;
  double m_shearXZ;
  double m_shearYX;
  double m_shearYZ;
  double m_shearZX;
  double m_shearZY;
};

} // namespace nim

#endif // ZAFFINE3D_H
