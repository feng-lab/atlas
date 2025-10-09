#pragma once

#include "z3dcamera.h"
#include "znumericparameter.h"
#include "zoptionparameter.h"
#include "zparameter.h"

namespace nim {

class Z3DCameraParameter : public ZSingleValueParameter<Z3DCamera>
{
  Q_OBJECT

public:
  explicit Z3DCameraParameter(const QString& name, QObject* parent = nullptr);

  Z3DCameraParameter(const QString& name, const Z3DCamera& value, QObject* parent = nullptr);

  void setEye(const glm::vec3& pos)
  {
    m_value.setEye(pos);
    updatePara();
  }

  void setCenter(const glm::vec3& focus)
  {
    m_value.setCenter(focus);
    updatePara();
  }

  void setUpVector(const glm::vec3& up)
  {
    m_value.setUpVector(up);
    updatePara();
  }

  void setEyeSeparationAngle(float angle)
  {
    m_value.setEyeSeparationAngle(angle);
    updatePara();
  }

  void setFrustum(float fov, float ratio, float ndist, float fdist)
  {
    m_value.setFrustum(fov, ratio, ndist, fdist);
    updatePara();
  }

  void setNearDist(float nd)
  {
    m_value.setNearDist(nd);
    updatePara();
  }

  void setFarDist(float fd)
  {
    m_value.setFarDist(fd);
    updatePara();
  }

  void setCamera(const glm::vec3& pos, const glm::vec3& focus, const glm::vec3& up)
  {
    m_value.setCamera(pos, focus, up);
    updatePara();
  }

  void setCamera(const glm::vec3& pos, const glm::vec3& focus)
  {
    m_value.setCamera(pos, focus, m_value.upVector());
    updatePara();
  }

  void setProjectionType(Z3DCamera::ProjectionType pt)
  {
    m_projectionType.select(pt == Z3DCamera::ProjectionType::Perspective ? "Perspective" : "Orthographic");
  }

  void setTileFrustum(double normalizedLeft = 0.0,
                      double normalizedRight = 1.0,
                      double normalizedBottom = 0.0,
                      double normalizedTop = 1.0)
  {
    m_value.setTileFrustum(normalizedLeft, normalizedRight, normalizedBottom, normalizedTop);
    Q_EMIT valueChanged();
  }

  void flipViewDirection();
  void rotate90X();
  void rotate90XZ();

  void resetCamera(const ZBBox<glm::dvec3>& bound, Z3DCamera::ResetOption options = Z3DCamera::ResetOption::ResetAll)
  {
    m_value.resetCamera(bound, options);
    updatePara();
  }

  void resetCamera(double xmin,
                   double xmax,
                   double ymin,
                   double ymax,
                   double zmin,
                   double zmax,
                   Z3DCamera::ResetOption options = Z3DCamera::ResetOption::ResetAll)
  {
    m_value.resetCamera(xmin, xmax, ymin, ymax, zmin, zmax, options);
    updatePara();
  }

  void resetCameraNearFarPlane(const ZBBox<glm::dvec3>& bound)
  {
    m_value.resetCameraNearFarPlane(bound);
    updatePara();
  }

  void resetCameraNearFarPlane(double xmin, double xmax, double ymin, double ymax, double zmin, double zmax)
  {
    m_value.resetCameraNearFarPlane(xmin, xmax, ymin, ymax, zmin, zmax);
    updatePara();
  }

  void dolly(float value)
  {
    m_value.dolly(value);
    updatePara();
  }

  void dollyToCenterDistance(float cd)
  {
    m_value.dollyToCenterDistance(cd);
    updatePara();
  }

  void roll(float angle)
  {
    m_value.roll(angle);
    updatePara();
  }

  void azimuth(float angle)
  {
    m_value.azimuth(angle);
    updatePara();
  }

  void yaw(float angle)
  {
    m_value.yaw(angle);
    updatePara();
  }

  void elevation(float angle)
  {
    m_value.elevation(angle);
    updatePara();
  }

  void pitch(float angle)
  {
    m_value.pitch(angle);
    updatePara();
  }

  void zoom(float factor)
  {
    m_value.zoom(factor);
    updatePara();
  }

  void rotate(float angle, const glm::vec3& axis, const glm::vec3& point)
  {
    m_value.rotate(angle, axis, point);
    updatePara();
  }

  void rotate(const glm::quat& quat, const glm::vec3& point)
  {
    m_value.rotate(quat, point);
    updatePara();
  }

  void rotate(float angle, const glm::vec3& axis)
  {
    m_value.rotate(angle, axis);
    updatePara();
  }

  void rotate(const glm::quat& quat)
  {
    m_value.rotate(quat);
    updatePara();
  }

  void viewportChanged(const glm::uvec2& viewport);

  // ZParameter interface

public:
  void setSameAs(const ZParameter& rhs) override;

  void setValueSameAs(const ZParameter& rhs) override;

  [[nodiscard]] json::value jsonValue() const override;

  void readValue(const json::value& jsonValue) override;

Q_SIGNALS:
  void windowsAspectRatioChanged(float r);

protected:
  void setWindowsAspectRatio(float r);

  void updateProjectionType();

  void updateEye();

  void updateCenter();

  void updateUpVector();

  void updateEyeSeparationAngle();

  void updateFieldOfView();

  void updateNearDist();

  void updateFarDist();

  QWidget* actualCreateWidget(QWidget* parent) override;

  void beforeChange(Z3DCamera& value) override;

  void updateWidget(Z3DCamera& value);

  void updatePara()
  {
    updateWidget(m_value);
    Q_EMIT valueChanged();
  }

private:
  bool m_blockSubParameterSignals = false;
  ZStringIntOptionParameter m_projectionType;
  ZVec3Parameter m_eye;
  ZVec3Parameter m_center;
  ZVec3Parameter m_upVector;
  ZFloatParameter m_eyeSeparationAngle;
  ZFloatParameter m_fieldOfView;
  ZFloatParameter m_nearDist;
  ZFloatParameter m_farDist;
};

} // namespace nim
