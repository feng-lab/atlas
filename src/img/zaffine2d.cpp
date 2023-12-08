#include "zaffine2d.h"

namespace nim {

void ZAffine2D::makeMatrix()
{
  //  Eigen::Matrix3d matShear;
  //  matShear << 1, m_shearXY, 0, m_shearYX, 1, 0, 0, 0, 1;
  glm::dmat3 shearX = glm::shearX(glm::dmat3(1), m_shear.y);
  glm::dmat3 shearY = glm::shearY(glm::dmat3(1), m_shear.x);
  glm::dmat3 trans1 = glm::translate(glm::dmat3(1), -m_center * m_scale);
  glm::dmat3 trans = glm::translate(glm::dmat3(1), m_translation + m_center * m_scale);
  glm::dmat3 scale = glm::scale(glm::dmat3(1), m_scale);
  glm::dmat3 rot = glm::rotate(glm::dmat3(1), m_rotateAngle);

  m_matrix = trans * rot * trans1 * scale * shearY * shearX;
  m_inverseMatrix = getInverseTransformMatrix();
}

void ZAffine2D::setMatrix(double m11, double m12, double m13, double m21, double m22, double m23)
{
  // m_matrix << m11, m12, m13, m21, m22, m23, 0, 0, 1;
  m_matrix = glm::dmat3(glm::dvec3(m11, m21, 0), glm::dvec3(m12, m22, 0), glm::dvec3(m13, m23, 1));
  m_inverseMatrix = getInverseTransformMatrix();
}

void ZAffine2D::reset()
{
  m_matrix = glm::dmat3(1);
  m_inverseMatrix = glm::dmat3(1);

  m_shear = glm::dvec2(0);
  m_scale = glm::dvec2(1, 1);
  m_translation = glm::dvec2(0);
  m_rotateAngle = 0;
  m_center = glm::dvec2(0);
}

void ZAffine2D::invert()
{
  std::swap(m_matrix, m_inverseMatrix);
}

void ZAffine2D::transformPointsForward(double u, double v, double& x, double& y) const
{
  auto pt = m_matrix * glm::dvec3(u, v, 1.0);
  x = pt.x;
  y = pt.y;
}

void ZAffine2D::transformPointsInverse(double x, double y, double& u, double& v) const
{
  auto pt = m_inverseMatrix * glm::dvec3(x, y, 1.0);
  u = pt.x;
  v = pt.y;
}

std::string ZAffine2D::toString() const
{
  return fmt::format("translation: {} scale: {} rotation: {} center: {} shear: {}\nAffine Matrix:\n{}",
                     m_translation,
                     m_scale,
                     m_rotateAngle,
                     m_center,
                     m_shear,
                     m_matrix);
}

QString ZAffine2D::toQString() const
{
  return QString::fromStdString(toString());
}

glm::dmat3 ZAffine2D::getInverseTransformMatrix() const
{
  auto tl = glm::inverse(glm::dmat2(m_matrix));
  auto res = glm::dmat3(tl);
  res[2] = glm::dvec3(-tl * glm::dvec2(m_matrix[2]), 1);
  return res;
}

} // namespace nim
