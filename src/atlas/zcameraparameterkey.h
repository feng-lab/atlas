#pragma once

#include "zparameterkey.h"
#include "z3dcameraparameter.h"

namespace nim {

// Kochanek-Bartels tension-continuity-bias spline interpolation for
// positional data.

class ZCameraParameterKey : public ZParameterKey
{
public:
  ZCameraParameterKey(double tm, const Z3DCameraParameter& p);

  ZCameraParameterKey(double tm, Z3DCameraParameter* p);

  ZCameraParameterKey();

  ZCameraParameterKey(const ZCameraParameterKey& key);

  inline Z3DCameraParameter* para()
  { return static_cast<Z3DCameraParameter*>(m_value.get()); }

  inline glm::vec3 eye() const
  { return static_cast<Z3DCameraParameter*>(m_value.get())->get().eye(); }

  inline glm::quat rot() const
  { return glm::quat_cast(static_cast<Z3DCameraParameter*>(m_value.get())->get().viewMatrix(Z3DEye::Mono)); }

  float posTension() const
  { return m_posTension; }

  void setPosTension(float v)
  { m_posTension = std::min(std::max(v, -1.f), 1.f); }

  float posContinuity() const
  { return m_posContinuity; }

  void setPosContinuity(float v)
  { m_posContinuity = std::min(std::max(v, -1.f), 1.f); }

  float posBias() const
  { return m_posBias; }

  void setPosBias(float v)
  { m_posBias = std::min(std::max(v, -1.f), 1.f); }

  float rotTension() const
  { return m_rotTension; }

  void setRotTension(float v)
  { m_rotTension = std::min(std::max(v, -1.f), 1.f); }

  float rotContinuity() const
  { return m_rotContinuity; }

  void setRotContinuity(float v)
  { m_rotContinuity = std::min(std::max(v, -1.f), 1.f); }

  float rotBias() const
  { return m_rotBias; }

  void setRotBias(float v)
  { m_rotBias = std::min(std::max(v, -1.f), 1.f); }

  virtual bool readValue(const QJsonValue& value) override;

  virtual QJsonValue jsonValue() const override;

protected:
  float m_posTension;
  float m_posContinuity;
  float m_posBias;
  float m_rotTension;
  float m_rotContinuity;
  float m_rotBias;
};

} // namespace nim

