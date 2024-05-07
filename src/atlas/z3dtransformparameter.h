#pragma once

#include "zglmutils.h"
#include "znumericparameter.h"
#include "zparameter.h"

namespace nim {

class Z3DTransformParameter : public ZSingleValueParameter<glm::mat4>
{
  Q_OBJECT

public:
  explicit Z3DTransformParameter(const QString& name, QObject* parent = nullptr);

  Z3DTransformParameter(const QString& name, const glm::mat4& value, QObject* parent = nullptr);

  void setScale(const glm::vec3& v)
  {
    m_scale.set(v);
  }

  void setTranslation(const glm::vec3& v)
  {
    m_translation.set(v);
  }

  void setRotationCenter(const glm::vec3& c)
  {
    m_center.set(c);
  }

  // angle in degree and axis
  void setRotation(const glm::vec4& v)
  {
    m_rotation.set(v);
  }

  void setRotation(const glm::quat& v)
  {
    m_rotation.set(glm::vec4(glm::degrees(glm::angle(v)), glm::axis(v)));
  }

  glm::vec3 rotationCenter()
  {
    return m_center.get();
  }

  [[nodiscard]] glm::vec3 scale() const
  {
    return m_scale.get();
  }

  [[nodiscard]] glm::vec3 translation() const
  {
    return m_translation.get();
  }

  [[nodiscard]] glm::quat rotation() const;

  void setXScale(float s)
  {
    m_scale.set(glm::vec3(s, m_scale.get().y, m_scale.get().z));
  }

  void setYScale(float s)
  {
    m_scale.set(glm::vec3(m_scale.get().x, s, m_scale.get().z));
  }

  void setZScale(float s)
  {
    m_scale.set(glm::vec3(m_scale.get().x, m_scale.get().y, s));
  }

  void translate(float x, float y, float z)
  {
    m_translation.set(glm::vec3(x, y, z) + m_translation.get());
  }

  void translate(const glm::vec3& t)
  {
    m_translation.set(t + m_translation.get());
  }

  void rotate(const glm::vec3& axis, float ang);

  void rotate(const glm::vec3& axis, float ang, const glm::vec3& center);

  void setValueSameAs(const ZParameter& rhs) override;

  void interpolate(const ZParameter& prev, double progress, ZParameter& dest) override;

  // ZParameter interface

public:
  void setSameAs(const ZParameter& rhs) override;

  [[nodiscard]] json::value jsonValue() const override;

  void readValue(const json::value& jsonValue) override;

protected:
  void updateMatrix();

  void showTransformMatrix();

  QWidget* actualCreateWidget(QWidget* parent) override;

  void beforeChange(glm::mat4& value) override;

  void updateWidget(const glm::mat4& value);

private:
  ZVec3Parameter m_scale;
  ZVec3Parameter m_translation;
  ZVec4Parameter m_rotation; // angle in degree and axis
  ZVec3Parameter m_center;
};

} // namespace nim
