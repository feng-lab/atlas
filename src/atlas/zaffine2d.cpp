#include "zaffine2d.h"

namespace nim {

ZAffine2D::ZAffine2D()
  : m_matrix(Eigen::Matrix3d::Identity())
  , m_inverseMatrix(Eigen::Matrix3d::Identity())
  , m_translationX(0)
  , m_translationY(0)
  , m_scaleX(1)
  , m_scaleY(1)
  , m_rotateAngle(0)
  , m_shearXY(0)
  , m_shearYX(0)
{
}

ZAffine2D::ZAffine2D(double m11, double m12, double m13, double m21, double m22, double m23)
  : m_matrix(Eigen::Matrix3d::Identity())
  , m_inverseMatrix(Eigen::Matrix3d::Identity())
  , m_translationX(0)
  , m_translationY(0)
  , m_scaleX(1)
  , m_scaleY(1)
  , m_rotateAngle(0)
  , m_shearXY(0)
  , m_shearYX(0)
{
  setMatrix(m11, m12, m13, m21, m22, m23);
}

void ZAffine2D::makeMatrix()
{
  Eigen::Matrix3d matTrans;
  matTrans << 1, 0, m_translationX,
      0, 1, m_translationY,
      0, 0, 1;
  Eigen::Matrix3d matScale;
  matScale << m_scaleX, 0, 0,
      0, m_scaleY, 0,
      0, 0, 1;
  Eigen::Matrix3d matRot;
  matRot << std::cos(m_rotateAngle), std::sin(m_rotateAngle), 0,
      -std::sin(m_rotateAngle), std::cos(m_rotateAngle), 0,
      0, 0, 1;
  Eigen::Matrix3d matShear;
  matShear << 1, m_shearXY, 0,
      m_shearYX, 1, 0,
      0, 0, 1;
  m_matrix = matTrans * matRot * matScale * matShear;
  m_inverseMatrix = getInverseTransformMatrix();
}

void ZAffine2D::setMatrix(double m11, double m12, double m13, double m21, double m22, double m23)
{
  m_matrix << m11, m12, m13,
      m21, m22, m23,
      0, 0, 1;
  m_inverseMatrix = getInverseTransformMatrix();
}

void ZAffine2D::reset()
{
  m_matrix = Eigen::Matrix3d::Identity();
  m_inverseMatrix = Eigen::Matrix3d::Identity();
  m_translationX = 0;
  m_translationY = 0;
  m_scaleX = 1;
  m_scaleY = 1;
  m_rotateAngle = 0;
  m_shearXY = 0;
  m_shearYX = 0;
}

void ZAffine2D::invert()
{
  std::swap(m_matrix, m_inverseMatrix);
}

void ZAffine2D::transformPointsForward(double u, double v, double &x, double &y) const
{
  Eigen::Vector3d pt = m_matrix * Eigen::Vector3d(u, v, 1.0);
  x = pt(0);
  y = pt(1);
}

void ZAffine2D::transformPointsInverse(double x, double y, double &u, double &v) const
{
  Eigen::Vector3d pt = m_inverseMatrix * Eigen::Vector3d(x, y, 1.0);
  u = pt(0);
  v = pt(1);
}

QString ZAffine2D::toQString() const
{
  return QString("translation: %1 %2 scale: %3 %4 rotation: %5 shear: %6 %7\nAffine Matrix:\n%8")
      .arg(m_translationX).arg(m_translationY)
      .arg(m_scaleX).arg(m_scaleY)
      .arg(m_rotateAngle)
      .arg(m_shearXY).arg(m_shearYX)
      .arg(matrixToQString(m_matrix));
}

Eigen::Matrix3d ZAffine2D::getInverseTransformMatrix() const
{
  Eigen::Matrix3d res;
  Eigen::Matrix2d tl = m_matrix.topLeftCorner<2,2>();
  tl = tl.inverse();
  Eigen::Vector2d r = m_matrix.topRightCorner<2,1>();
  res.topLeftCorner(2,2) = tl;
  res.topRightCorner(2,1) = -tl*r;
  return res;
}

} // namespace nim
