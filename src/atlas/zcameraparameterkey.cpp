#include "zcameraparameterkey.h"

#include "zparameterfactory.h"

namespace nim {

ZCameraParameterKey::ZCameraParameterKey(double tm, const Z3DCameraParameter &p)
  : ZParameterKey(tm, p)
  , m_posTension(0.f)
  , m_posContinuity(0.f)
  , m_posBias(0.f)
  , m_rotTension(0.f)
  , m_rotContinuity(0.f)
  , m_rotBias(0.f)
{
}

ZCameraParameterKey::ZCameraParameterKey(double tm, Z3DCameraParameter *p)
  : ZParameterKey(tm, p)
  , m_posTension(0.f)
  , m_posContinuity(0.f)
  , m_posBias(0.f)
  , m_rotTension(0.f)
  , m_rotContinuity(0.f)
  , m_rotBias(0.f)
{
}

ZCameraParameterKey::ZCameraParameterKey()
  : ZParameterKey("3DCamera")
  , m_posTension(0.f)
  , m_posContinuity(0.f)
  , m_posBias(0.f)
  , m_rotTension(0.f)
  , m_rotContinuity(0.f)
  , m_rotBias(0.f)
{
}

ZCameraParameterKey::ZCameraParameterKey(const ZCameraParameterKey &key)
  : ZParameterKey(key)
  , m_posTension(key.posTension())
  , m_posContinuity(key.posContinuity())
  , m_posBias(key.posBias())
  , m_rotTension(key.rotTension())
  , m_rotContinuity(key.rotContinuity())
  , m_rotBias(key.rotBias())
{
  setType(key.type());
}

bool ZCameraParameterKey::readValue(const QJsonValue &value)
{
  if (!ZParameterKey::readValue(value)) {
    return false;
  }
  QJsonObject obj = value.toObject();
  m_posTension = obj.value("posTension").toDouble(m_posTension);
  m_posContinuity = obj.value("posContinuity").toDouble(m_posContinuity);
  m_posBias = obj.value("posBias").toDouble(m_posBias);
  m_rotTension = obj.value("rotTension").toDouble(m_rotTension);
  m_rotContinuity = obj.value("rotContinuity").toDouble(m_rotContinuity);
  m_rotBias = obj.value("rotBias").toDouble(m_rotBias);
  return true;
}

QJsonValue ZCameraParameterKey::jsonValue() const
{
  QJsonValue val = ZParameterKey::jsonValue();
  assert(val.isObject());
  QJsonObject obj = val.toObject();
  obj["posTension"] = m_posTension;
  obj["posContinuity"] = m_posContinuity;
  obj["posBias"] = m_posBias;
  obj["rotTension"] = m_rotTension;
  obj["rotContinuity"] = m_rotContinuity;
  obj["rotBias"] = m_rotBias;
  return obj;
}

} // namespace nim
