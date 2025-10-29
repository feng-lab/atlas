#include "zcameraparameterkey.h"

#include "zparameterfactory.h"
#include "zlog.h"

namespace nim {

ZCameraParameterKey::ZCameraParameterKey(double tm, const Z3DCameraParameter& p)
  : ZParameterKey(tm, p)
{}

ZCameraParameterKey::ZCameraParameterKey(double tm, Z3DCameraParameter* p)
  : ZParameterKey(tm, p)
{}

ZCameraParameterKey::ZCameraParameterKey()
  : ZParameterKey("3DCamera")
{}

ZCameraParameterKey::ZCameraParameterKey(const ZCameraParameterKey& key)
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

bool ZCameraParameterKey::readValue(const json::value& value)
{
  if (!ZParameterKey::readValue(value)) {
    return false;
  }
  const auto& obj = value.as_object();
  // TCB fields are optional; when unspecified, keep current defaults (0).
  if (auto it = obj.if_contains("posTension"); it) m_posTension = json::value_to<float>(*it);
  if (auto it = obj.if_contains("posContinuity"); it) m_posContinuity = json::value_to<float>(*it);
  if (auto it = obj.if_contains("posBias"); it) m_posBias = json::value_to<float>(*it);
  if (auto it = obj.if_contains("rotTension"); it) m_rotTension = json::value_to<float>(*it);
  if (auto it = obj.if_contains("rotContinuity"); it) m_rotContinuity = json::value_to<float>(*it);
  if (auto it = obj.if_contains("rotBias"); it) m_rotBias = json::value_to<float>(*it);
  return true;
}

json::value ZCameraParameterKey::jsonValue() const
{
  auto val = ZParameterKey::jsonValue();
  CHECK(val.is_object());
  auto& obj = val.as_object();
  obj["posTension"] = m_posTension;
  obj["posContinuity"] = m_posContinuity;
  obj["posBias"] = m_posBias;
  obj["rotTension"] = m_rotTension;
  obj["rotContinuity"] = m_rotContinuity;
  obj["rotBias"] = m_rotBias;
  return obj;
}

} // namespace nim
