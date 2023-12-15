#pragma once

#include "zglmutils.h"

namespace nim {

class ZAffine3D
{
public:
  ZAffine3D() = default;

  ZAffine3D(double m11,
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
    setMatrix(m11, m12, m13, m14, m21, m22, m23, m24, m31, m32, m33, m34);
  }

  // only matrices are meaningful after merging
  void mergeWith(const ZAffine3D& other)
  {
    m_matrix *= other.m_matrix;
    m_inverseMatrix = getInverseTransformMatrix();
  }

  // must call makeMatrix to update, shear -> scale -> rotate -> translate
  void setTranslation(double x, double y, double z)
  {
    m_translation = glm::dvec3(x, y, z);
  }

  void setRotationAngle(double radianAngleX, double radianAngleY, double radianAngleZ)
  {
    m_rotateAngle = glm::dvec3(radianAngleX, radianAngleY, radianAngleZ);
  }

  void setRotationCenter(double x, double y, double z)
  {
    m_center = glm::dvec3(x, y, z);
  }

  void setScale(double xscale, double yscale, double zscale)
  {
    m_scale = glm::dvec3(xscale, yscale, zscale);
  }

  void setShear(double lx1, double lx2, double ly1, double ly2, double lz1, double lz2)
  {
    m_shearX = glm::dvec2(lx1, lx2);
    m_shearY = glm::dvec2(ly1, ly2);
    m_shearZ = glm::dvec2(lz1, lz2);
  }

  void makeMatrix();

  [[nodiscard]] double scaleX() const
  {
    return m_scale.x;
  }

  [[nodiscard]] double scaleY() const
  {
    return m_scale.y;
  }

  [[nodiscard]] double scaleZ() const
  {
    return m_scale.z;
  }

  //
  void setMatrix(double m11,
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
                 double m34);

  // access
  [[nodiscard]] const glm::dmat4& transformMatrix() const
  {
    return m_matrix;
  }

  [[nodiscard]] const glm::dmat4& inverseTransformMatrix() const
  {
    return m_inverseMatrix;
  }

  //
  void reset();

  void invert();

  void transformPointsForward(double u, double v, double w, double& x, double& y, double& z) const;

  void transformPointsInverse(double x, double y, double z, double& u, double& v, double& w) const;

  [[nodiscard]] std::string toString() const
  {
    return fmt::format("translation: {} scale: {} rotation: {} center: {} shear: {} {} {}\nAffine Matrix:\n{}",
                       m_translation,
                       m_scale,
                       m_rotateAngle,
                       m_center,
                       m_shearX,
                       m_shearY,
                       m_shearZ,
                       m_matrix);
  }

protected:
  [[nodiscard]] glm::dmat4 getInverseTransformMatrix() const;

private:
  glm::dmat4 m_matrix = glm::dmat4(1);
  glm::dmat4 m_inverseMatrix = glm::dmat4(1);

  glm::dvec2 m_shearX = glm::dvec2(0);
  glm::dvec2 m_shearY = glm::dvec2(0);
  glm::dvec2 m_shearZ = glm::dvec2(0);
  glm::dvec3 m_scale = glm::dvec3(1, 1, 1);
  glm::dvec3 m_translation = glm::dvec3(0);
  glm::dvec3 m_center = glm::dvec3(0);
  glm::dvec3 m_rotateAngle = glm::dvec3(0);
};

} // namespace nim
