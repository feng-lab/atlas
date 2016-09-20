#include "zaffine3d.h"

namespace nim {

void ZAffine3D::makeMatrix()
{
  Eigen::Matrix4d matTrans;
  matTrans << 1, 0, 0, m_translationX,
    0, 1, 0, m_translationY,
    0, 0, 1, m_translationZ,
    0, 0, 0, 1;
  Eigen::Matrix4d matScale;
  matScale << m_scaleX, 0, 0, 0,
    0, m_scaleY, 0, 0,
    0, 0, m_scaleZ, 0,
    0, 0, 0, 1;
  Eigen::Matrix4d matRotationXY;
  Eigen::Matrix4d matRotationXZ;
  Eigen::Matrix4d matRotationYZ;
  matRotationXY << std::cos(m_rotateAngleXY), std::sin(m_rotateAngleXY), 0, 0,
    -std::sin(m_rotateAngleXY), std::cos(m_rotateAngleXY), 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1;
  matRotationXZ << std::cos(m_rotateAngleXZ), 0, std::sin(m_rotateAngleXZ), 0,
    0, 1, 0, 0,
    -std::sin(m_rotateAngleXZ), 0, std::cos(m_rotateAngleXZ), 0,
    0, 0, 0, 1;
  matRotationYZ << 1, 0, 0, 0,
    0, std::cos(m_rotateAngleYZ), std::sin(m_rotateAngleYZ), 0,
    0, -std::sin(m_rotateAngleYZ), std::cos(m_rotateAngleYZ), 0,
    0, 0, 0, 1;
  Eigen::Matrix4d matShear;
  matShear << 1, m_shearXY, m_shearXZ, 0,
    m_shearYX, 1, m_shearYZ, 0,
    m_shearZX, m_shearZY, 1, 0,
    0, 0, 0, 1;
  m_matrix = matTrans * matRotationYZ * matRotationXZ * matRotationXY * matScale * matShear;
  m_inverseMatrix = getInverseTransformMatrix();
}

void ZAffine3D::setMatrix(double m11, double m12, double m13, double m14,
                          double m21, double m22, double m23, double m24,
                          double m31, double m32, double m33, double m34)
{
  m_matrix << m11, m12, m13, m14,
    m21, m22, m23, m24,
    m31, m32, m33, m34,
    0, 0, 0, 1;
  m_inverseMatrix = getInverseTransformMatrix();
}

void ZAffine3D::reset()
{
  m_matrix = Eigen::Matrix4d::Identity();
  m_inverseMatrix = Eigen::Matrix4d::Identity();
  m_translationX = 0;
  m_translationY = 0;
  m_translationZ = 0;
  m_scaleX = 1;
  m_scaleY = 1;
  m_scaleZ = 1;
  m_rotateAngleXY = 0;
  m_rotateAngleXZ = 0;
  m_rotateAngleYZ = 0;
  m_shearXY = 0;
  m_shearXZ = 0;
  m_shearYX = 0;
  m_shearYZ = 0;
  m_shearZX = 0;
  m_shearZY = 0;
}

void ZAffine3D::invert()
{
  std::swap(m_matrix, m_inverseMatrix);
}

void ZAffine3D::transformPointsForward(double u, double v, double w, double& x, double& y, double& z) const
{
  Eigen::Vector4d pt = m_matrix * Eigen::Vector4d(u, v, w, 1.0);
  x = pt(0);
  y = pt(1);
  z = pt(2);
}

void ZAffine3D::transformPointsInverse(double x, double y, double z, double& u, double& v, double& w) const
{
  Eigen::Vector4d pt = m_inverseMatrix * Eigen::Vector4d(x, y, z, 1.0);
  u = pt(0);
  v = pt(1);
  w = pt(2);
}

QString ZAffine3D::toQString() const
{
  return QString("translation: %1 %2 %3 scale: %4 %5 %6 rotation: %7 %8 %9\nAffine Matrix:\n%10")
    .arg(m_translationX).arg(m_translationY).arg(m_translationZ)
    .arg(m_scaleX).arg(m_scaleY).arg(m_scaleZ)
    .arg(m_rotateAngleXY).arg(m_rotateAngleXZ).arg(m_rotateAngleYZ)
    .arg(matrixToQString(m_matrix));
}

Eigen::Matrix4d ZAffine3D::getInverseTransformMatrix() const
{
  Eigen::Matrix4d res;
  Eigen::Matrix3d tl = m_matrix.topLeftCorner<3, 3>();
  tl = tl.inverse();
  Eigen::Vector3d r = m_matrix.topRightCorner<3, 1>();
  res.topLeftCorner(3, 3) = tl;
  res.topRightCorner(3, 1) = -tl * r;
  return res;
}

} // namespace nim
