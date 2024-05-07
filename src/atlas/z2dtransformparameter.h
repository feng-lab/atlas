#pragma once

#include "zglmutils.h"
#include "znumericparameter.h"
#include "zparameter.h"
#include "zquatparameter.h"

namespace nim {

class Z2DTransformParameter : public ZSingleValueParameter<glm::dmat3>
{
  Q_OBJECT

public:
  explicit Z2DTransformParameter(const QString& name, QObject* parent = nullptr);

  Z2DTransformParameter(const QString& name, const glm::dmat3& value, QObject* parent = nullptr);

  void setScale(const glm::dvec2& v)
  {
    m_scale.set(v);
  }

  void setTranslation(const glm::dvec2& v)
  {
    m_translation.set(v);
  }

  void setRotationCenter(const glm::dvec2& c)
  {
    m_center.set(c);
  }

  void setRotation(double v)
  {
    m_rotation.set(glm::degrees(v));
  }

  glm::dvec2 rotationCenter()
  {
    return m_center.get();
  }

  glm::dvec2 scale() const
  {
    return m_scale.get();
  }

  glm::dvec2 translation() const
  {
    return m_translation.get();
  }

  double rotation() const
  {
    return glm::radians(m_rotation.get());
  }

  void setXScale(double s)
  {
    m_scale.set(glm::dvec2(s, m_scale.get().y));
  }

  void setYScale(double s)
  {
    m_scale.set(glm::dvec2(m_scale.get().x, s));
  }

  void setScale(double sx, double xy)
  {
    m_scale.set(glm::dvec2(sx, xy));
  }

  void translate(double x, double y)
  {
    m_translation.set(glm::dvec2(x, y) + m_translation.get());
  }

  void translate(const glm::dvec2& t)
  {
    m_translation.set(t + m_translation.get());
  }

  void rotate(double ang);

  void rotate(double ang, const glm::dvec2& center);

  void flipHorizontally(const QRectF& boundRect);

  void flipVertically(const QRectF& boundRect);

  void setValueSameAs(const ZParameter& rhs) override;

  void interpolate(const ZParameter& prev, double progress, ZParameter& dest) override;

  // ZParameter interface

public:
  void setSameAs(const ZParameter& rhs) override;

  json::value jsonValue() const override;

  void readValue(const json::value& jsonValue) override;

protected:
  void updateMatrix();

  void showTransformMatrix();

  QWidget* actualCreateWidget(QWidget* parent) override;

  void beforeChange(glm::dmat3& value) override;

  void updateWidget(const glm::dmat3& value);

private:
  ZDVec2Parameter m_scale;
  ZDVec2Parameter m_translation;
  ZDoubleParameter m_rotation; // angle in degree
  ZDVec2Parameter m_center;
};

} // namespace nim
