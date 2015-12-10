#ifndef ZCAMERAPARAMETER_H
#define ZCAMERAPARAMETER_H

#include "zparameter.h"
#include "znumericparameter.h"
#include "zoptionparameter.h"
#include "z3dcamera.h"

namespace nim {

class Z3DCameraParameter : public ZSingleValueParameter<Z3DCamera>
{
  Q_OBJECT
public:
  Z3DCameraParameter(const QString& name, QObject* parent = NULL);
  Z3DCameraParameter(const QString& name, const Z3DCamera& value, QObject* parent = NULL);

  inline void setEye(const glm::vec3& pos) { m_value.setEye(pos); updatePara(); }
  inline void setCenter(const glm::vec3& focus) { m_value.setCenter(focus); updatePara(); }
  inline void setUpVector(const glm::vec3& up) { m_value.setUpVector(up); updatePara(); }
  inline void setEyeSeparationAngle(float angle) { m_value.setEyeSeparationAngle(angle); updatePara(); }
  inline void setFrustum(float fov, float ratio, float ndist, float fdist)
      { m_value.setFrustum(fov, ratio, ndist, fdist); updatePara(); }
  inline void setNearDist(float nd) { m_value.setNearDist(nd); updatePara(); }
  inline void setFarDist(float fd) { m_value.setFarDist(fd); updatePara(); }
  inline void setCamera(const glm::vec3& pos, const glm::vec3& focus, const glm::vec3& up)
      { m_value.setCamera(pos, focus, up); updatePara(); }
  inline void setCamera(const glm::vec3 &pos, const glm::vec3 &focus)
      { m_value.setCamera(pos, focus, m_value.upVector()); updatePara(); }
  inline void setProjectionType(Z3DCamera::ProjectionType pt)
      { m_projectionType.select(pt == Z3DCamera::ProjectionType::Perspective ? "Perspective" : "Orthographic"); }

  void flipViewDirection();

  inline void resetCamera(const std::vector<double> &bound,
                          Z3DCamera::ResetCameraOptions options = Z3DCamera::ResetAll)
      { m_value.resetCamera(bound, options); updatePara(); }
  inline void resetCamera(double xmin, double xmax, double ymin, double ymax, double zmin, double zmax,
                          Z3DCamera::ResetCameraOptions options = Z3DCamera::ResetAll)
      { m_value.resetCamera(xmin, xmax, ymin, ymax, zmin, zmax, options); updatePara(); }

  inline void resetCameraNearFarPlane(const std::vector<double> &bound)
      { m_value.resetCameraNearFarPlane(bound); updatePara(); }
  inline void resetCameraNearFarPlane(double xmin, double xmax, double ymin, double ymax,
                                      double zmin, double zmax)
      { m_value.resetCameraNearFarPlane(xmin, xmax, ymin, ymax, zmin, zmax); updatePara(); }

  inline void dolly(float value) { m_value.dolly(value); updatePara(); }
  inline void dollyToCenterDistance(float cd) { m_value.dollyToCenterDistance(cd); updatePara(); }
  inline void roll(float angle) { m_value.roll(angle); updatePara(); }
  inline void azimuth(float angle) { m_value.azimuth(angle); updatePara(); }
  inline void yaw(float angle) { m_value.yaw(angle); updatePara(); }
  inline void elevation(float angle) { m_value.elevation(angle); updatePara(); }
  inline void pitch(float angle) { m_value.pitch(angle); updatePara(); }
  inline void zoom(float factor) { m_value.zoom(factor); updatePara(); }
  inline void rotate(float angle, glm::vec3 axis, glm::vec3 point) { m_value.rotate(angle, axis, point); updatePara(); }
  inline void rotate(glm::quat quat, glm::vec3 point) { m_value.rotate(quat, point); updatePara(); }
  inline void rotate(float angle, glm::vec3 axis) { m_value.rotate(angle, axis); updatePara(); }
  inline void rotate(glm::quat quat) { m_value.rotate(quat); updatePara(); }

signals:
  void windowsAspectRatioChanged(float r);

public slots:
  void viewportChanged(const glm::uvec2 &viewport);
  void setWindowsAspectRatio(float r);

  void updateProjectionType();
  void updateEye();
  void updateCenter();
  void updateUpVector();
  void updateEyeSeparationAngle();
  void updateFieldOfView();
  void updateNearDist();
  void updateFarDist();

protected:
  virtual QWidget* actualCreateWidget(QWidget *parent) override;
  virtual void beforeChange(Z3DCamera &value) override;

  void updateWidget(Z3DCamera &value);

  inline void updatePara() { updateWidget(m_value); emit valueChanged(); }

private:
  ZStringIntOptionParameter m_projectionType;
  ZVec3Parameter m_eye;
  ZVec3Parameter m_center;
  ZVec3Parameter m_upVector;
  ZFloatParameter m_eyeSeparationAngle;
  ZFloatParameter m_fieldOfView;
  ZFloatParameter m_nearDist;
  ZFloatParameter m_farDist;
  bool m_receiveWidgetSignal;

  // ZParameter interface
public:
  virtual void setSameAs(const ZParameter &rhs) override;
  virtual void setValueSameAs(const ZParameter &rhs) override;
  virtual QJsonValue jsonValue() const override;
  virtual void readValue(const QJsonValue &jsonValue) override;
};

} // namespace nim

#endif // ZCAMERAPARAMETER_H
