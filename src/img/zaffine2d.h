#pragma once

#include "zglmutils.h"

namespace nim {

class ZAffine2D
{
public:
  ZAffine2D() = default;

  ZAffine2D(double m11, double m12, double m13, double m21, double m22, double m23)
  {
    setMatrix(m11, m12, m13, m21, m22, m23);
  }

  // only matrices are meaningful after merging
  void mergeWith(const ZAffine2D& other)
  {
    m_matrix *= other.m_matrix;
    m_inverseMatrix = getInverseTransformMatrix();
  }

  // must call makeMatrix to update, shear -> scale -> rotate -> translate
  void setTranslation(double x, double y)
  {
    m_translation = glm::dvec2(x, y);
  }

  void setRotationAngle(double radianAngle)
  {
    m_rotateAngle = radianAngle;
  }

  void setRotationCenter(double x, double y)
  {
    m_center = glm::dvec2(x, y);
  }

  void setScale(double xscale, double yscale)
  {
    m_scale = glm::dvec2(xscale, yscale);
  }

  void setShear(double x, double y)
  {
    m_shear = glm::dvec2(x, y);
  }

  void makeMatrix();

  [[nodiscard]] double translationX() const
  {
    return m_translation.x;
  }

  [[nodiscard]] double translationY() const
  {
    return m_translation.y;
  }

  [[nodiscard]] double rotationAngle() const
  {
    return m_rotateAngle;
  }

  [[nodiscard]] double scaleX() const
  {
    return m_scale.x;
  }

  [[nodiscard]] double scaleY() const
  {
    return m_scale.y;
  }

  [[nodiscard]] double shearX() const
  {
    return m_shear.x;
  }

  [[nodiscard]] double shearY() const
  {
    return m_shear.y;
  }

  //
  void setMatrix(double m11, double m12, double m13, double m21, double m22, double m23);

  // access
  [[nodiscard]] const glm::dmat3& transformMatrix() const
  {
    return m_matrix;
  }

  [[nodiscard]] const glm::dmat3& inverseTransformMatrix() const
  {
    return m_inverseMatrix;
  }

  //
  void reset();

  void invert();

  void transformPointsForward(double u, double v, double& x, double& y) const;

  void transformPointsInverse(double x, double y, double& u, double& v) const;

  [[nodiscard]] std::string toString() const
  {
    return fmt::format("translation: {} scale: {} rotation: {} center: {} shear: {}\nAffine Matrix:\n{}",
                       m_translation,
                       m_scale,
                       m_rotateAngle,
                       m_center,
                       m_shear,
                       m_matrix);
  }

protected:
  [[nodiscard]] glm::dmat3 getInverseTransformMatrix() const;

private:
  glm::dmat3 m_matrix = glm::dmat3(1);
  glm::dmat3 m_inverseMatrix = glm::dmat3(1);

  glm::dvec2 m_shear = glm::dvec2(0);
  glm::dvec2 m_scale = glm::dvec2(1, 1);
  glm::dvec2 m_translation = glm::dvec2(0);
  double m_rotateAngle = 0; // angle in degree
  glm::dvec2 m_center = glm::dvec2(0);
};

} // namespace nim
