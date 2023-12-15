#include "zaffine3d.h"

namespace nim {

void ZAffine3D::makeMatrix()
{
  auto shear = glm::shear(glm::dmat4(1), m_center, m_shearX, m_shearY, m_shearZ);
  auto trans1 = glm::translate(glm::dmat4(1), -m_center * m_scale);
  auto trans = glm::translate(glm::dmat4(1), m_translation + m_center * m_scale);
  auto scale = glm::scale(glm::dmat4(1), m_scale);
  auto rotX = glm::rotate(m_rotateAngle.x, glm::dvec3(1, 0, 0));
  auto rotY = glm::rotate(m_rotateAngle.y, glm::dvec3(0, 1, 0));
  auto rotZ = glm::rotate(m_rotateAngle.z, glm::dvec3(0, 0, 1));

  m_matrix = trans * rotX * rotY * rotZ * trans1 * scale * shear;
  m_inverseMatrix = getInverseTransformMatrix();
}

void ZAffine3D::setMatrix(double m11,
                          double m12,
                          double m13,
                          double m14,
                          double m21,
                          double m22,
                          double m23,
                          double m24,
                          double m31,
                          double m32,
                          double m33,
                          double m34)
{
  m_matrix = glm::dmat4(glm::dvec4(m11, m21, m31, 0),
                        glm::dvec4(m12, m22, m32, 0),
                        glm::dvec4(m13, m23, m33, 0),
                        glm::dvec4(m14, m24, m34, 1));
  m_inverseMatrix = getInverseTransformMatrix();
}

void ZAffine3D::reset()
{
  m_matrix = glm::dmat4(1);
  m_inverseMatrix = glm::dmat4(1);

  m_shearX = glm::dvec2(0);
  m_shearY = glm::dvec2(0);
  m_shearZ = glm::dvec2(0);
  m_scale = glm::dvec3(1, 1, 1);
  m_translation = glm::dvec3(0);
  m_center = glm::dvec3(0);
  m_rotateAngle = glm::dvec3(0);
}

void ZAffine3D::invert()
{
  std::swap(m_matrix, m_inverseMatrix);
}

void ZAffine3D::transformPointsForward(double u, double v, double w, double& x, double& y, double& z) const
{
  auto pt = m_matrix * glm::dvec4(u, v, w, 1.0);
  x = pt.x;
  y = pt.y;
  z = pt.z;
}

void ZAffine3D::transformPointsInverse(double x, double y, double z, double& u, double& v, double& w) const
{
  auto pt = m_inverseMatrix * glm::dvec4(x, y, z, 1.0);
  u = pt.x;
  v = pt.y;
  w = pt.z;
}

glm::dmat4 ZAffine3D::getInverseTransformMatrix() const
{
  auto tl = glm::inverse(glm::dmat3(m_matrix));
  auto res = glm::dmat4(tl);
  res[3] = glm::dvec4(-tl * glm::dvec3(m_matrix[3]), 1);
  return res;
}

} // namespace nim
