#include "zcameraparameteranimation.h"

#include <memory>

namespace nim {

namespace {

[[nodiscard]] QString normalizeInterpolationMethod(QString s)
{
  s = s.toLower().trimmed();
  QString out;
  out.reserve(s.size());
  for (QChar c : s) {
    if (c.isSpace() || c == QChar('_') || c == QChar('-')) {
      continue;
    }
    out.append(c);
  }
  return out;
}

} // namespace

// Legendre polynomial information for Gaussian quadrature of speed on domain
// [0,u], 0 <= u <= 1. The polynomial is degree 5.
float ZCameraParameterAnimation::Poly::ms_afModRoot[5] = {
  // Legendre roots mapped to (root+1)/2
  0.046910077f,
  0.230765345f,
  0.5f,
  0.769234655f,
  0.953089922f};
float ZCameraParameterAnimation::Poly::ms_afModCoeff[5] = {
  // original coefficients divided by 2
  0.118463442f,
  0.239314335f,
  0.284444444f,
  0.239314335f,
  0.118463442f};

ZCameraParameterAnimation::ZCameraParameterAnimation(const QString& name, const QColor& color, QObject* parent)
  : ZParameterAnimation(name, "3DCamera", color, parent)
  , m_interpolationMethod("Interpolation Method")
{
  configureInterpolationMethodParameter(m_interpolationMethod);
  connect(&m_interpolationMethod, &ZStringIntOptionParameter::valueChanged, this, [this]() {
    Q_EMIT interpolationMethodChanged(m_interpolationMethod.get());
  });

  connect(this, &ZCameraParameterAnimation::keysChanged, this, &ZCameraParameterAnimation::buildSpline);
  connect(this, &ZCameraParameterAnimation::keyChanged, this, &ZCameraParameterAnimation::buildSpline);
}

void ZCameraParameterAnimation::configureInterpolationMethodParameter(ZStringIntOptionParameter& parameter)
{
  parameter.clearOptions();
  parameter.addOptions("Center", "Position Spline", "Position Rotation Spline");
  parameter.select("Center");
  parameter.setDescription(QStringLiteral(
    "Camera track interpolation method. Center is strongly advised for normal trackball/orbit animation because it "
    "interpolates the look-at center, center distance, and orientation predictably. Use Position Spline or Position "
    "Rotation Spline only for intentional free-camera paths that need TCB spline shaping."));
}

bool ZCameraParameterAnimation::setInterpolationMethod(const QString& method)
{
  const QString wantNorm = normalizeInterpolationMethod(method);
  for (const auto& opt : m_interpolationMethod.options()) {
    if (normalizeInterpolationMethod(opt) == wantNorm) {
      m_interpolationMethod.select(opt);
      return true;
    }
  }
  return false;
}

std::unique_ptr<ZParameterKey> ZCameraParameterAnimation::createKey(double secs) const
{
  CHECK(secs >= 0);
  CHECK(m_boundPara);

  return std::make_unique<ZCameraParameterKey>(secs, *static_cast<Z3DCameraParameter*>(m_boundPara));
}

void ZCameraParameterAnimation::updateParaToTime(double secs, ZParameter* para) const
{
  std::lock_guard<std::recursive_mutex> lock(m_keysMutex);
  CHECK(para->type() == m_type);
  CHECK(secs >= 0);

  if (m_keys.empty()) {
    return;
  }
  if (secs <= m_keys[0]->time()) {
    para->setValueSameAs(m_keys[0]->value());
    return;
  }
  if (secs >= m_keys.back()->time()) {
    para->setValueSameAs(m_keys.back()->value());
    return;
  }
  float centerDist = 1.f;
  glm::vec3 center;
  glm::quat rot;
  for (size_t i = 1; i < m_keys.size(); ++i) {
    if (secs < m_keys[i]->time()) {
      double progress = m_keys[i]->timeToProgress(*m_keys[i - 1], secs);
      auto key1 = static_cast<ZCameraParameterKey*>(m_keys[i - 1].get());
      auto key2 = static_cast<ZCameraParameterKey*>(m_keys[i].get());
      centerDist = glm::mix(key1->para()->get().centerDist(), key2->para()->get().centerDist(), progress);
      center = glm::mix(key1->para()->get().center(), key2->para()->get().center(), progress);
      Z3DCamera::ProjectionType pt = key1->para()->get().projectionType();
      float eyeSepAngle =
        glm::mix(key1->para()->get().eyeSeparationAngle(), key2->para()->get().eyeSeparationAngle(), progress);
      float fieldOfView = glm::mix(key1->para()->get().fieldOfView(), key2->para()->get().fieldOfView(), progress);
      rot = glm::slerp(key1->rot(), key2->rot(), static_cast<float>(progress));
      // will send signal later
      static_cast<Z3DCameraParameter*>(para)->get().setEyeSeparationAngle(eyeSepAngle);
      static_cast<Z3DCameraParameter*>(para)->get().setProjectionType(pt);
      static_cast<Z3DCameraParameter*>(para)->get().setFieldOfView(fieldOfView);
      break;
    }
  }

  if (m_interpolationMethod.isSelected("Center")) {
    glm::mat3 rotMat = glm::mat3_cast(rot);
    glm::vec3 viewVector = glm::vec3(-rotMat[0][2], -rotMat[1][2], -rotMat[2][2]);
    glm::vec3 upVector = glm::vec3(rotMat[0][1], rotMat[1][1], rotMat[2][1]);
    static_cast<Z3DCameraParameter*>(para)->setCamera(center - viewVector * centerDist, center, upVector);
  } else if (m_interpolationMethod.isSelected("Position Spline")) {
    if (m_pathSegments.empty()) {
      // Spline data is derived from keysChanged/keyChanged on the UI thread.
      // If it is temporarily unavailable (e.g., during rebuild), fall back to
      // the stable center+distance interpolation to avoid crashes.
      glm::mat3 rotMat = glm::mat3_cast(rot);
      glm::vec3 viewVector = glm::vec3(-rotMat[0][2], -rotMat[1][2], -rotMat[2][2]);
      glm::vec3 upVector = glm::vec3(rotMat[0][1], rotMat[1][1], rotMat[2][1]);
      static_cast<Z3DCameraParameter*>(para)->setCamera(center - viewVector * centerDist, center, upVector);
      return;
    }
    glm::vec3 pos;
    for (size_t i = 1; i < m_pathSegments.size(); ++i) {
      if (secs < m_pathSegments[i].startTime()) { // belongs to prev segment
        pos = m_pathSegments[i - 1].position(secs);
        break;
      }
    }
    if (secs >= m_pathSegments.back().startTime()) {
      // belongs to last segment
      pos = m_pathSegments.back().position(secs);
    }
    // update camera
    glm::mat3 rotMat = glm::mat3_cast(rot);
    glm::vec3 viewVector = glm::vec3(-rotMat[0][2], -rotMat[1][2], -rotMat[2][2]);
    glm::vec3 upVector = glm::vec3(rotMat[0][1], rotMat[1][1], rotMat[2][1]);
    // send signal
    static_cast<Z3DCameraParameter*>(para)->setCamera(pos, pos + viewVector * centerDist, upVector);
  } else {
    if (m_pathSegments.empty()) {
      glm::mat3 rotMat = glm::mat3_cast(rot);
      glm::vec3 viewVector = glm::vec3(-rotMat[0][2], -rotMat[1][2], -rotMat[2][2]);
      glm::vec3 upVector = glm::vec3(rotMat[0][1], rotMat[1][1], rotMat[2][1]);
      static_cast<Z3DCameraParameter*>(para)->setCamera(center - viewVector * centerDist, center, upVector);
      return;
    }
    glm::vec3 pos;
    for (size_t i = 1; i < m_pathSegments.size(); ++i) {
      if (secs < m_pathSegments[i].startTime()) { // belongs to prev segment
        pos = m_pathSegments[i - 1].position(secs);
        rot = m_pathSegments[i - 1].rotation(secs);
        break;
      }
    }
    if (secs >= m_pathSegments.back().startTime()) {
      // belongs to last segment
      pos = m_pathSegments.back().position(secs);
      rot = m_pathSegments.back().rotation(secs);
    }
    // update camera
    glm::mat3 rotMat = glm::mat3_cast(rot);
    glm::vec3 viewVector = glm::vec3(-rotMat[0][2], -rotMat[1][2], -rotMat[2][2]);
    glm::vec3 upVector = glm::vec3(rotMat[0][1], rotMat[1][1], rotMat[2][1]);
    // send signal
    static_cast<Z3DCameraParameter*>(para)->setCamera(pos, pos + viewVector * centerDist, upVector);
  }
}

void ZCameraParameterAnimation::write(json::object& json) const
{
  ZParameterAnimation::write(json);
  auto it = json.find(jsonKey().toStdString());
  CHECK(it != json.end());
  CHECK(it->value().is_object());
  it->value().as_object()["interpolationMethod"] = json::value_from(m_interpolationMethod.get());
}

void ZCameraParameterAnimation::buildSpline()
{
  std::lock_guard<std::recursive_mutex> lock(m_keysMutex);
  m_pathSegments.clear();

  if (m_keys.size() < 2) {
    return;
  }

  size_t start = 0;
  for (size_t i = 1; i < m_keys.size(); ++i) {
    if (m_keys[i]->type() == "Switch") {
      // end of one segment, build spline with range
      std::vector<ZCameraParameterKey*> res;
      for (size_t j = start; j < i; ++j) {
        res.push_back(static_cast<ZCameraParameterKey*>(m_keys[j].get()));
      }
      m_pathSegments.emplace_back();
      SplineRange sr(res);
      m_pathSegments.back().swap(sr);

      start = i;
    }
  }
  // last segment
  std::vector<ZCameraParameterKey*> res;
  for (size_t j = start; j < m_keys.size(); ++j) {
    res.push_back(static_cast<ZCameraParameterKey*>(m_keys[j].get()));
  }
  m_pathSegments.emplace_back();
  SplineRange sr(res);
  m_pathSegments.back().swap(sr);
}

ZCameraParameterAnimation::CameraKeySample::CameraKeySample(const ZCameraParameterKey& key)
  : time(static_cast<float>(key.time()))
  , eye(key.eye())
  , rot(key.rot())
  , posTension(key.posTension())
  , posContinuity(key.posContinuity())
  , posBias(key.posBias())
  , rotTension(key.rotTension())
  , rotContinuity(key.rotContinuity())
  , rotBias(key.rotBias())
{}

glm::vec3 ZCameraParameterAnimation::Poly::Position(float fU) const
{
  return m_akC[0] + fU * (m_akC[1] + fU * (m_akC[2] + fU * m_akC[3]));
}

glm::vec3 ZCameraParameterAnimation::Poly::Velocity(float fU) const
{
  return m_akC[1] + fU * (2.0f * m_akC[2] + 3.0f * fU * m_akC[3]);
}

glm::vec3 ZCameraParameterAnimation::Poly::Acceleration(float fU) const
{
  return 2.0f * m_akC[2] + 6.0f * fU * m_akC[3];
}

float ZCameraParameterAnimation::Poly::Speed(float fU) const
{
  return glm::length(Velocity(fU));
}

float ZCameraParameterAnimation::Poly::Length(float fU) const
{
  // Need to transform domain [0,u] to [-1,1]. If 0 <= x <= u
  // and -1 <= t <= 1, then x = u*(t+1)/2.
  float fResult = 0.0f;
  for (int i = 0; i < 5; ++i) {
    fResult += ms_afModCoeff[i] * Speed(fU * ms_afModRoot[i]);
  }
  fResult *= fU;
  return fResult;
}

glm::quat ZCameraParameterAnimation::SquadPoly::Q(float fU) const
{
  return glm::squad(m_kP, m_kQ, m_kA, m_kB, fU);
}

ZCameraParameterAnimation::SplineRange::SplineRange(const std::vector<ZCameraParameterKey*>& kys)
  : m_hasSpline(kys.size() >= 2)
{
  keys.reserve(kys.size());
  for (const auto* key : kys) {
    CHECK(key);
    keys.emplace_back(*key);
  }
  if (m_hasSpline) {
    buildPosSpline();
    buildRotSpline();
  }
}

glm::quat ZCameraParameterAnimation::SplineRange::rotation(float fTime) const
{
  if (!m_hasSpline) {
    CHECK(!keys.empty());
    return keys[0].rot;
  }

  // find the interpolating polynomial (clamping used, modify for looping)
  size_t i;
  float fU;
  if (startTime() < fTime) {
    if (fTime < endTime()) {
      for (i = 0; i < rotSpline.size(); ++i) {
        if (fTime < rotSpline[i].m_fTMax) {
          break;
        }
      }
      fU = (fTime - rotSpline[i].m_fTMin) * rotSpline[i].m_fTInvRange;
    } else {
      i = rotSpline.size() - 1_uz;
      fU = 1.0f;
    }
  } else {
    i = 0;
    fU = 0.0f;
  }
  return rotSpline[i].Q(fU);
}

glm::vec3 ZCameraParameterAnimation::SplineRange::position(float fTime) const
{
  if (!m_hasSpline) {
    CHECK(!keys.empty());
    return keys[0].eye;
  }

  size_t i;
  float fU;
  doPolyLookup(fTime, i, fU);
  return posSpline[i].Position(fU);
}

glm::vec3 ZCameraParameterAnimation::SplineRange::velocity(float fTime)
{
  if (!m_hasSpline) {
    return glm::vec3(0.f);
  }

  size_t i;
  float fU;
  doPolyLookup(fTime, i, fU);
  return posSpline[i].Velocity(fU);
}

glm::vec3 ZCameraParameterAnimation::SplineRange::acceleration(float fTime)
{
  if (!m_hasSpline) {
    return glm::vec3(0.f);
  }

  size_t i;
  float fU;
  doPolyLookup(fTime, i, fU);
  return posSpline[i].Acceleration(fU);
}

float ZCameraParameterAnimation::SplineRange::length(float fTime)
{
  if (!m_hasSpline) {
    return 0;
  }

  size_t i;
  float fU;
  doPolyLookup(fTime, i, fU);
  return posSpline[i].Length(fU);
}

float ZCameraParameterAnimation::SplineRange::totalLength() const
{
  return posSplineTotalLength;
}

glm::vec3 ZCameraParameterAnimation::SplineRange::positionAL(float fS)
{
  if (!m_hasSpline) {
    CHECK(!keys.empty());
    return keys[0].eye;
  }

  size_t i = 0;
  float fU = 0;
  invertIntegral(fS, i, fU);
  return posSpline[i].Position(fU);
}

glm::vec3 ZCameraParameterAnimation::SplineRange::velocityAL(float fS)
{
  if (!m_hasSpline) {
    return glm::vec3(0.f);
  }

  size_t i = 0;
  float fU = 0;
  invertIntegral(fS, i, fU);
  return posSpline[i].Velocity(fU);
}

glm::vec3 ZCameraParameterAnimation::SplineRange::accelerationAL(float fS)
{
  if (!m_hasSpline) {
    return glm::vec3(0.f);
  }

  size_t i = 0;
  float fU = 0;
  invertIntegral(fS, i, fU);
  return posSpline[i].Acceleration(fU);
}

void ZCameraParameterAnimation::SplineRange::buildPosSpline()
{
  CHECK(keys.size() >= 2);
  posSpline.resize(keys.size() - 1);

  std::vector<glm::vec3> inTangents(keys.size(), glm::vec3(0.f));
  std::vector<glm::vec3> outTangents(keys.size(), glm::vec3(0.f));

  {
    const auto& key0 = keys[0];
    const auto& key1 = keys[1];
    const float delta = key1.time - key0.time;
    CHECK(delta > 0.f);
    const float coeff = (1.0f - key0.posTension) * (1.0f - key0.posContinuity) * (1.0f - key0.posBias) / (2.0f * delta);
    outTangents[0] = coeff * (key1.eye - key0.eye);
    inTangents[0] = outTangents[0];
  }

  for (size_t k = 1; k + 1 < keys.size(); ++k) {
    const auto& keyPrev = keys[k - 1];
    const auto& key = keys[k];
    const auto& keyNext = keys[k + 1];
    const glm::vec3 prevDiff = key.eye - keyPrev.eye;
    const glm::vec3 nextDiff = keyNext.eye - key.eye;
    const float prevDelta = key.time - keyPrev.time;
    const float nextDelta = keyNext.time - key.time;
    CHECK(prevDelta > 0.f);
    CHECK(nextDelta > 0.f);

    const float omT = 1.0f - key.posTension;
    const float omC = 1.0f - key.posContinuity;
    const float opC = 1.0f + key.posContinuity;
    const float omB = 1.0f - key.posBias;
    const float opB = 1.0f + key.posBias;
    const float twoPrevDelta = 2.0f * prevDelta;
    const float twoNextDelta = 2.0f * nextDelta;
    inTangents[k] = (omT * omC * opB / twoPrevDelta) * prevDiff + (omT * opC * omB / twoNextDelta) * nextDiff;
    outTangents[k] = (omT * opC * opB / twoPrevDelta) * prevDiff + (omT * omC * omB / twoNextDelta) * nextDiff;
  }

  {
    const size_t last = keys.size() - 1;
    const auto& keyPrev = keys[last - 1];
    const auto& key = keys[last];
    const float delta = key.time - keyPrev.time;
    CHECK(delta > 0.f);
    const float coeff = (1.0f - key.posTension) * (1.0f - key.posContinuity) * (1.0f + key.posBias) / (2.0f * delta);
    inTangents[last] = coeff * (key.eye - keyPrev.eye);
    outTangents[last] = inTangents[last];
  }

  for (size_t k = 0; k + 1 < keys.size(); ++k) {
    const auto& key0 = keys[k];
    const auto& key1 = keys[k + 1];
    const float delta = key1.time - key0.time;
    CHECK(delta > 0.f);
    const glm::vec3 diff = key1.eye - key0.eye;
    const glm::vec3 outTangent = outTangents[k];
    const glm::vec3 inTangent = inTangents[k + 1];

    posSpline[k].m_akC[0] = key0.eye;
    posSpline[k].m_akC[1] = delta * outTangent;
    posSpline[k].m_akC[2] = 3.0f * diff - delta * (2.0f * outTangent + inTangent);
    posSpline[k].m_akC[3] = -2.0f * diff + delta * (outTangent + inTangent);
    posSpline[k].m_fTMin = key0.time;
    posSpline[k].m_fTMax = key1.time;
    posSpline[k].m_fTInvRange = 1.0f / delta;
  }
  // compute arc lengths of polynomials and total length of spline
  posSplineLengths.resize(posSpline.size() + 1, 0.f);
  for (size_t i = 0; i < posSpline.size(); ++i) {
    // length of current polynomial
    float fPolyLength = posSpline[i].Length(1.0f);
    // total length of curve between poly[0] and poly[i+1]
    posSplineLengths[i + 1] = posSplineLengths[i] + fPolyLength;
  }
  posSplineTotalLength = posSplineLengths[posSpline.size()];
}

void ZCameraParameterAnimation::SplineRange::buildRotSpline()
{
  CHECK(keys.size() >= 2);
  rotSpline.resize(keys.size() - 1);

  // Consecutive quaternions should form an acute angle. Changing sign on a quaternion does not
  // change the rotation it represents, but it *does* affect log/exp-based spline construction.
  std::vector<glm::quat> qs;
  qs.reserve(keys.size());
  for (const auto& key : keys) {
    qs.emplace_back(key.rot);
  }
  for (size_t i = 1; i < qs.size(); ++i) {
    if (glm::dot(qs[i], qs[i - 1]) < 0.0f) {
      qs[i] = -qs[i];
    }
  }

  std::vector<glm::quat> inTangents(keys.size(), glm::quat(0.f, 0.f, 0.f, 0.f));
  std::vector<glm::quat> outTangents(keys.size(), glm::quat(0.f, 0.f, 0.f, 0.f));

  {
    const auto& key0 = keys[0];
    const auto& key1 = keys[1];
    const float delta = key1.time - key0.time;
    CHECK(delta > 0.f);
    const glm::quat log01 = glm::log(glm::conjugate(qs[0]) * qs[1]);
    const float coeff = (1.0f - key0.rotTension) * (1.0f - key0.rotContinuity) * (1.0f - key0.rotBias) / (2.0f * delta);
    outTangents[0] = coeff * log01;
    inTangents[0] = outTangents[0];
  }

  for (size_t k = 1; k + 1 < keys.size(); ++k) {
    const auto& keyPrev = keys[k - 1];
    const auto& key = keys[k];
    const auto& keyNext = keys[k + 1];
    const float prevDelta = key.time - keyPrev.time;
    const float nextDelta = keyNext.time - key.time;
    CHECK(prevDelta > 0.f);
    CHECK(nextDelta > 0.f);
    const glm::quat prevLog = glm::log(glm::conjugate(qs[k - 1]) * qs[k]);
    const glm::quat nextLog = glm::log(glm::conjugate(qs[k]) * qs[k + 1]);

    const float omT = 1.0f - key.rotTension;
    const float omC = 1.0f - key.rotContinuity;
    const float opC = 1.0f + key.rotContinuity;
    const float omB = 1.0f - key.rotBias;
    const float opB = 1.0f + key.rotBias;
    const float twoPrevDelta = 2.0f * prevDelta;
    const float twoNextDelta = 2.0f * nextDelta;
    inTangents[k] = (omT * omC * opB / twoPrevDelta) * prevLog + (omT * opC * omB / twoNextDelta) * nextLog;
    outTangents[k] = (omT * opC * opB / twoPrevDelta) * prevLog + (omT * omC * omB / twoNextDelta) * nextLog;
  }

  {
    const size_t last = keys.size() - 1;
    const auto& keyPrev = keys[last - 1];
    const auto& key = keys[last];
    const float delta = key.time - keyPrev.time;
    CHECK(delta > 0.f);
    const glm::quat log = glm::log(glm::conjugate(qs[last - 1]) * qs[last]);
    const float coeff = (1.0f - key.rotTension) * (1.0f - key.rotContinuity) * (1.0f + key.rotBias) / (2.0f * delta);
    inTangents[last] = coeff * log;
    outTangents[last] = inTangents[last];
  }

  for (size_t k = 0; k + 1 < keys.size(); ++k) {
    const auto& key0 = keys[k];
    const auto& key1 = keys[k + 1];
    const float delta = key1.time - key0.time;
    CHECK(delta > 0.f);
    const glm::quat segmentLog = glm::log(glm::conjugate(qs[k]) * qs[k + 1]);
    const glm::quat outTangent = delta * outTangents[k];
    const glm::quat inTangent = delta * inTangents[k + 1];

    rotSpline[k].m_kP = qs[k];
    rotSpline[k].m_kQ = qs[k + 1];
    rotSpline[k].m_kA = qs[k] * glm::exp(0.5f * (outTangent - segmentLog));
    rotSpline[k].m_kB = qs[k + 1] * glm::exp(0.5f * (segmentLog - inTangent));
    rotSpline[k].m_fTMin = key0.time;
    rotSpline[k].m_fTMax = key1.time;
    rotSpline[k].m_fTInvRange = 1.0f / delta;
  }
}

void ZCameraParameterAnimation::SplineRange::doPolyLookup(float fTime, size_t& riI, float& rfU) const
{
  // Lookup the polynomial that contains the input time in its domain of
  // evaluation. Clamp to [tmin,tmax].
  if (startTime() < fTime) {
    if (fTime < endTime()) {
      for (riI = 0; riI < posSpline.size(); riI++) {
        if (fTime < posSpline[riI].m_fTMax) {
          break;
        }
      }
      rfU = (fTime - posSpline[riI].m_fTMin) * posSpline[riI].m_fTInvRange;
    } else {
      riI = posSpline.size() - 1_uz;
      rfU = 1.0f;
    }
  } else {
    riI = 0;
    rfU = 0.0f;
  }
}

void ZCameraParameterAnimation::SplineRange::invertIntegral(float fS, size_t& riI, float& rfU)
{
  // clamp s to [0,L] so that t in [tmin,tmax]
  if (fS <= 0.0f) {
    riI = 0;
    rfU = 0.0f;
    return;
  }
  if (fS >= posSplineTotalLength) {
    riI = posSpline.size() - 1_uz;
    rfU = 1.0f;
    return;
  }
  // determine which polynomial corresponds to s
  float fDist = 0.f;

  for (riI = 0; riI < posSpline.size(); riI++) {
    if (fS <= posSplineLengths[riI + 1]) {
      // distance along segment
      fDist = fS - posSplineLengths[riI];
      // initial guess for inverting the arc length integral
      rfU = fDist / (posSplineLengths[riI + 1] - posSplineLengths[riI]);
      break;
    }
  }
  // use Newton's method to invert the arc length integral
  const float fTolerance = 1e-06f;
  const int iMax = 32;
  for (int i = 0; i < iMax; ++i) {
    float fDiff = posSpline[riI].Length(rfU) - fDist;
    if (std::abs(fDiff) <= fTolerance) {
      break;
    }

    rfU -= fDiff / posSpline[riI].Speed(rfU);
  }
}

void ZCameraParameterAnimation::SplineRange::swap(ZCameraParameterAnimation::SplineRange& rhs) noexcept
{
  keys.swap(rhs.keys);
  posSpline.swap(rhs.posSpline);
  posSplineLengths.swap(rhs.posSplineLengths);
  std::swap(posSplineTotalLength, rhs.posSplineTotalLength);
  rotSpline.swap(rhs.rotSpline);
  std::swap(m_hasSpline, rhs.m_hasSpline);
}

} // namespace nim
