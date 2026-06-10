#include "zcameraparameterkey.h"

#include "zparameterfactory.h"
#include "zlog.h"

#include <cmath>

namespace nim {

const std::array<ZCameraParameterKey::TcbFieldInfo, 6>& ZCameraParameterKey::tcbFieldInfos()
{
  static constexpr std::array<TcbFieldInfo, 6> kInfos = {
    {
     {"posTension",
       "Advanced camera eye-position TCB tension in [-1,1]. 0 is neutral; keep 0 unless intentionally shaping a "
       "free-camera spline path."},
     {"posContinuity",
       "Advanced camera eye-position TCB continuity in [-1,1]. 0 is neutral; use only when intentionally changing "
       "how the spline turns through this key."},
     {"posBias",
       "Advanced camera eye-position TCB bias in [-1,1]. 0 is neutral; use only when intentionally biasing the path "
       "toward the previous or next segment."},
     {"rotTension",
       "Advanced camera rotation TCB tension in [-1,1]. 0 is neutral; keep 0 unless intentionally shaping a rotation "
       "spline."},
     {"rotContinuity",
       "Advanced camera rotation TCB continuity in [-1,1]. 0 is neutral; use only when intentionally changing how "
       "rotation turns through this key."},
     {"rotBias",
       "Advanced camera rotation TCB bias in [-1,1]. 0 is neutral; use only when intentionally biasing rotation "
       "toward the previous or next segment."},
     }
  };
  return kInfos;
}

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
  const auto readTcb = [&](const char* name, auto setter) {
    if (auto it = obj.if_contains(name); it) {
      if (!it->is_number()) {
        LOG(WARNING) << "Invalid camera TCB value " << name << "; expected numeric value in [-1, 1]";
        return false;
      }
      const double v = it->to_number<double>();
      if (!std::isfinite(v) || v < -1.0 || v > 1.0) {
        LOG(WARNING) << "Invalid camera TCB value " << name << "=" << v << "; expected [-1, 1]";
        return false;
      }
      setter(static_cast<float>(v));
    }
    return true;
  };
  if (!readTcb("posTension",
               [this](float v) {
                 setPosTension(v);
               }) ||
      !readTcb("posContinuity",
               [this](float v) {
                 setPosContinuity(v);
               }) ||
      !readTcb("posBias",
               [this](float v) {
                 setPosBias(v);
               }) ||
      !readTcb("rotTension",
               [this](float v) {
                 setRotTension(v);
               }) ||
      !readTcb("rotContinuity",
               [this](float v) {
                 setRotContinuity(v);
               }) ||
      !readTcb("rotBias", [this](float v) {
        setRotBias(v);
      })) {
    return false;
  }
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
