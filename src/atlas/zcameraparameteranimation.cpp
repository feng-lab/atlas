#include "zcameraparameteranimation.h"

namespace nim {

// Legendre polynomial information for Gaussian quadrature of speed on domain
// [0,u], 0 <= u <= 1. The polynomial is degree 5.
float ZCameraParameterAnimation::Poly::ms_afModRoot[5] =
  {
    // Legendre roots mapped to (root+1)/2
    0.046910077f,
    0.230765345f,
    0.5f,
    0.769234655f,
    0.953089922f
  };
float ZCameraParameterAnimation::Poly::ms_afModCoeff[5] =
  {
    // original coefficients divided by 2
    0.118463442f,
    0.239314335f,
    0.284444444f,
    0.239314335f,
    0.118463442f
  };


ZCameraParameterAnimation::ZCameraParameterAnimation(const QString& name, const QColor& color, QObject* parent)
  : ZParameterAnimation(name, "3DCamera", color, parent)
  , m_interpolationMethod("Interpolation Method")
{
  m_interpolationMethod.addOptions("Center", "Position Spline", "Position Rotation Spline");
  m_interpolationMethod.select("Center");
  connect(&m_interpolationMethod, &ZStringIntOptionParameter::valueChanged,
          this, &ZCameraParameterAnimation::interpolationMethodChanged);

  connect(this, &ZCameraParameterAnimation::keysChanged, this, &ZCameraParameterAnimation::buildSpline);
  connect(this, &ZCameraParameterAnimation::keyChanged, this, &ZCameraParameterAnimation::buildSpline);
}

ZParameterKey* ZCameraParameterAnimation::createKey(double secs) const
{
  CHECK(secs >= 0);
  CHECK(m_boundPara);

  return new ZCameraParameterKey(secs, *static_cast<Z3DCameraParameter*>(m_boundPara));
}

void ZCameraParameterAnimation::updateParaToTime(double secs, ZParameter* para) const
{
  CHECK(para->type() == m_type);
  CHECK(secs >= 0);

  if (m_keys.empty())
    return;
  if (secs <= m_keys[0]->time()) {
    para->setValueSameAs(m_keys[0]->value());
    return;
  }
  if (secs >= m_keys[m_keys.size() - 1]->time()) {
    para->setValueSameAs(m_keys[m_keys.size() - 1]->value());
    return;
  }
  float centerDist = 1.f;
  glm::vec3 center;
  glm::quat rot;
  for (size_t i = 1; i < m_keys.size(); ++i) {
    if (secs < m_keys[i]->time()) {
      double progress = m_keys[i]->timeToProgress(*m_keys[i - 1], secs);
      ZCameraParameterKey* key1 = static_cast<ZCameraParameterKey*>(m_keys[i - 1].get());
      ZCameraParameterKey* key2 = static_cast<ZCameraParameterKey*>(m_keys[i].get());
      centerDist = glm::mix(key1->para()->get().centerDist(), key2->para()->get().centerDist(), progress);
      center = glm::mix(key1->para()->get().center(), key2->para()->get().center(), progress);
      Z3DCamera::ProjectionType pt = key1->para()->get().projectionType();
      float eyeSepAngle = glm::mix(key1->para()->get().eyeSeparationAngle(),
                                   key2->para()->get().eyeSeparationAngle(), progress);
      float fieldOfView = glm::mix(key1->para()->get().fieldOfView(),
                                   key2->para()->get().fieldOfView(), progress);
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
    glm::vec3 pos;
    for (size_t i = 1; i < m_pathSegments.size(); ++i) {
      if (secs < m_pathSegments[i].startTime()) {  // belongs to prev segment
        pos = m_pathSegments[i - 1].position(secs);
        break;
      }
    }
    if (secs >= m_pathSegments[m_pathSegments.size() - 1].startTime()) {
      // belongs to last segment
      pos = m_pathSegments[m_pathSegments.size() - 1].position(secs);
    }
    // update camera
    glm::mat3 rotMat = glm::mat3_cast(rot);
    glm::vec3 viewVector = glm::vec3(-rotMat[0][2], -rotMat[1][2], -rotMat[2][2]);
    glm::vec3 upVector = glm::vec3(rotMat[0][1], rotMat[1][1], rotMat[2][1]);
    // send signal
    static_cast<Z3DCameraParameter*>(para)->setCamera(pos, pos + viewVector * centerDist, upVector);
  } else {
    glm::vec3 pos;
    for (size_t i = 1; i < m_pathSegments.size(); ++i) {
      if (secs < m_pathSegments[i].startTime()) {  // belongs to prev segment
        pos = m_pathSegments[i - 1].position(secs);
        rot = m_pathSegments[i - 1].rotation(secs);
        break;
      }
    }
    if (secs >= m_pathSegments[m_pathSegments.size() - 1].startTime()) {
      // belongs to last segment
      pos = m_pathSegments[m_pathSegments.size() - 1].position(secs);
      rot = m_pathSegments[m_pathSegments.size() - 1].rotation(secs);
    }
    // update camera
    glm::mat3 rotMat = glm::mat3_cast(rot);
    glm::vec3 viewVector = glm::vec3(-rotMat[0][2], -rotMat[1][2], -rotMat[2][2]);
    glm::vec3 upVector = glm::vec3(rotMat[0][1], rotMat[1][1], rotMat[2][1]);
    // send signal
    static_cast<Z3DCameraParameter*>(para)->setCamera(pos, pos + viewVector * centerDist, upVector);
  }
}

void ZCameraParameterAnimation::buildSpline()
{
  m_pathSegments.clear();

  if (m_keys.size() < 2)
    return;

  size_t start = 0;
  for (size_t i = 1; i < m_keys.size(); ++i) {
    if (m_keys[i]->type() == "Switch") {
      // end of one segment, build spline with range
      QList<ZCameraParameterKey*> res;
      for (size_t j = start; j < i; ++j) {
        // make sure no overlap key
        if (j > start && m_keys[j]->time() - m_keys[j - 1]->time() < 0.0001)
          m_keys[j]->setTime(m_keys[j - 1]->time() + 0.0001);
        res.push_back(static_cast<ZCameraParameterKey*>(m_keys[j].get()));
      }
      m_pathSegments.push_back(SplineRange());
      SplineRange sr(res);
      m_pathSegments[m_pathSegments.size() - 1].swap(sr);

      start = i;
    }
  }
  // last segment
  QList<ZCameraParameterKey*> res;
  for (size_t j = start; j < m_keys.size(); ++j) {
    // make sure no overlap key
    if (j > start && m_keys[j]->time() - m_keys[j - 1]->time() < 0.0001)
      m_keys[j]->setTime(m_keys[j - 1]->time() + 0.0001);
    res.push_back(static_cast<ZCameraParameterKey*>(m_keys[j].get()));
  }
  m_pathSegments.push_back(SplineRange());
  SplineRange sr(res);
  m_pathSegments[m_pathSegments.size() - 1].swap(sr);
}

glm::vec3 ZCameraParameterAnimation::Poly::Position(float fU) const
{
  return m_akC[0] + fU * (m_akC[1] + fU * (m_akC[2] + fU * m_akC[3]));
}

glm::vec3 ZCameraParameterAnimation::Poly::Velocity(float fU)
{
  return m_akC[1] + fU * (2.0f * m_akC[2] + 3.0f * fU * m_akC[3]);
}

glm::vec3 ZCameraParameterAnimation::Poly::Acceleration(float fU)
{
  return 2.0f * m_akC[2] + 6.0f * fU * m_akC[3];
}

float ZCameraParameterAnimation::Poly::Speed(float fU)
{
  return glm::length(Velocity(fU));
}

float ZCameraParameterAnimation::Poly::Length(float fU)
{
  // Need to transform domain [0,u] to [-1,1]. If 0 <= x <= u
  // and -1 <= t <= 1, then x = u*(t+1)/2.
  float fResult = 0.0f;
  for (int i = 0; i < 5; ++i)
    fResult += ms_afModCoeff[i] * Speed(fU * ms_afModRoot[i]);
  fResult *= fU;
  return fResult;
}

glm::quat ZCameraParameterAnimation::SquadPoly::Q(float fU) const
{
  return glm::squad(m_kP, m_kQ, m_kA, m_kB, fU);
}

ZCameraParameterAnimation::SplineRange::SplineRange(QList<ZCameraParameterKey*>& kys)
  : m_hasSpline(kys.size() >= 2)
{
  keys.swap(kys);
  if (m_hasSpline) {
    firstKey.reset(new ZCameraParameterKey(*keys[0]));
    lastKey.reset(new ZCameraParameterKey(*keys[keys.size() - 1]));
    keys.push_front(firstKey.get());
    keys.push_back(lastKey.get());
    buildPosSpline();
    buildRotSpline();
  }
}

glm::quat ZCameraParameterAnimation::SplineRange::rotation(float fTime) const
{
  if (!m_hasSpline)
    return keys[0]->rot();

  // find the interpolating polynomial (clamping used, modify for looping)
  int i;
  float fU;
  if (startTime() < fTime) {
    if (fTime < endTime()) {
      for (i = 0; i < rotSpline.size(); ++i) {
        if (fTime < rotSpline[i].m_fTMax)
          break;
      }
      fU = (fTime - rotSpline[i].m_fTMin) * rotSpline[i].m_fTInvRange;
    }
    else {
      i = rotSpline.size() - 1;
      fU = 1.0f;
    }
  }
  else {
    i = 0;
    fU = 0.0f;
  }
  return rotSpline[i].Q(fU);
}

glm::vec3 ZCameraParameterAnimation::SplineRange::position(double fTime) const
{
  if (!m_hasSpline)
    return keys[0]->eye();

  int i;
  float fU;
  doPolyLookup(fTime, i, fU);
  return posSpline[i].Position(fU);
}

glm::vec3 ZCameraParameterAnimation::SplineRange::velocity(double fTime)
{
  if (!m_hasSpline)
    return glm::vec3(0.f);

  int i;
  float fU;
  doPolyLookup(fTime, i, fU);
  return posSpline[i].Velocity(fU);
}

glm::vec3 ZCameraParameterAnimation::SplineRange::acceleration(double fTime)
{
  if (!m_hasSpline)
    return glm::vec3(0.f);

  int i;
  float fU;
  doPolyLookup(fTime, i, fU);
  return posSpline[i].Acceleration(fU);
}

double ZCameraParameterAnimation::SplineRange::length(double fTime)
{
  if (!m_hasSpline)
    return 0;

  int i;
  float fU;
  doPolyLookup(fTime, i, fU);
  return posSpline[i].Length(fU);
}

double ZCameraParameterAnimation::SplineRange::totalLength()
{
  return posSplineTotalLength;
}

glm::vec3 ZCameraParameterAnimation::SplineRange::positionAL(double fS)
{
  if (!m_hasSpline)
    return keys[0]->eye();

  int i = 0;
  float fU = 0;
  invertIntegral(fS, i, fU);
  return posSpline[i].Position(fU);
}

glm::vec3 ZCameraParameterAnimation::SplineRange::velocityAL(double fS)
{
  if (!m_hasSpline)
    return glm::vec3(0.f);

  int i = 0;
  float fU = 0;
  invertIntegral(fS, i, fU);
  return posSpline[i].Velocity(fU);
}

glm::vec3 ZCameraParameterAnimation::SplineRange::accelerationAL(double fS)
{
  if (!m_hasSpline)
    return glm::vec3(0.f);

  int i = 0;
  float fU = 0;
  invertIntegral(fS, i, fU);
  return posSpline[i].Acceleration(fU);
}

void ZCameraParameterAnimation::SplineRange::buildPosSpline()
{
  for (int i = 0; i < keys.size() - 3; ++i) {
    posSpline.push_back(Poly());
  }

  for (int i0 = 0, i1 = 1, i2 = 2, i3 = 3; i0 < posSpline.size(); i0++, i1++, i2++, i3++) {
    glm::vec3 kDiff10 = keys[i1]->eye() - keys[i0]->eye();
    glm::vec3 kDiff21 = keys[i2]->eye() - keys[i1]->eye();
    glm::vec3 kDiff32 = keys[i3]->eye() - keys[i2]->eye();
    // build multipliers at point P[i1]
    float fOmT0 = 1.0f - keys[i1]->posTension();
    float fOmC0 = 1.0f - keys[i1]->posContinuity();
    float fOpC0 = 1.0f + keys[i1]->posContinuity();
    float fOmB0 = 1.0f - keys[i1]->posBias();
    float fOpB0 = 1.0f + keys[i1]->posBias();
    float fAdj0 = 2.0f * (keys[i2]->time() - keys[i1]->time()) /
                  (keys[i2]->time() - keys[i0]->time());
    float fOut0 = 0.5f * fAdj0 * fOmT0 * fOpC0 * fOpB0;
    float fOut1 = 0.5f * fAdj0 * fOmT0 * fOmC0 * fOmB0;
    // build outgoing tangent at P[i1]
    glm::vec3 kTOut = fOut1 * kDiff21 + fOut0 * kDiff10;
    // build multipliers at point P[i2]
    float fOmT1 = 1.0f - keys[i2]->posTension();
    float fOmC1 = 1.0f - keys[i2]->posContinuity();
    float fOpC1 = 1.0f + keys[i2]->posContinuity();
    float fOmB1 = 1.0f - keys[i2]->posBias();
    float fOpB1 = 1.0f + keys[i2]->posBias();
    float fAdj1 = 2.0f * (keys[i2]->time() - keys[i1]->time()) /
                  (keys[i3]->time() - keys[i1]->time());
    float fIn0 = 0.5f * fAdj1 * fOmT1 * fOmC1 * fOpB1;
    float fIn1 = 0.5f * fAdj1 * fOmT1 * fOpC1 * fOmB1;
    // build incoming tangent at P[i2]
    glm::vec3 kTIn = fIn1 * kDiff32 + fIn0 * kDiff21;
    posSpline[i0].m_akC[0] = keys[i1]->eye();
    posSpline[i0].m_akC[1] = kTOut;
    posSpline[i0].m_akC[2] = 3.0f * kDiff21 - 2.0f * kTOut - kTIn;
    posSpline[i0].m_akC[3] = -2.0f * kDiff21 + kTOut + kTIn;
    posSpline[i0].m_fTMin = keys[i1]->time();
    posSpline[i0].m_fTMax = keys[i2]->time();
    posSpline[i0].m_fTInvRange = 1.0f / (keys[i2]->time() - keys[i1]->time());
  }
  // compute arc lengths of polynomials and total length of spline
  posSplineLengths.resize(posSpline.size() + 1, 0.f);
  for (int i = 0; i < posSpline.size(); ++i) {
    // length of current polynomial
    float fPolyLength = posSpline[i].Length(1.0f);
    // total length of curve between poly[0] and poly[i+1]
    posSplineLengths[i + 1] = posSplineLengths[i] + fPolyLength;
  }
  posSplineTotalLength = posSplineLengths[posSpline.size()];
}

void ZCameraParameterAnimation::SplineRange::buildRotSpline()
{
  for (int i = 0; i < keys.size() - 3; ++i) {
    rotSpline.push_back(SquadPoly());
  }

  // Consecutive quaterions should form an acute angle. Changing sign on
  // a quaternion does not change the rotation it represents.
  int i;
  for (i = 1; i < keys.size(); ++i) {
    if (glm::dot(keys[i]->rot(), keys[i - 1]->rot()) < 0.0f)
      keys[i]->rot() = -keys[i]->rot();
  }
  for (int i0 = 0, i1 = 1, i2 = 2, i3 = 3; i0 < rotSpline.size(); i0++, i1++, i2++, i3++) {
    glm::quat kQ0 = keys[i0]->rot();
    glm::quat kQ1 = keys[i1]->rot();
    glm::quat kQ2 = keys[i2]->rot();
    glm::quat kQ3 = keys[i3]->rot();
    glm::quat kLog10 = glm::log(glm::conjugate(kQ0) * kQ1);
    glm::quat kLog21 = glm::log(glm::conjugate(kQ1) * kQ2);
    glm::quat kLog32 = glm::log(glm::conjugate(kQ2) * kQ3);
    // build multipliers at q[i1]
    float fOmT0 = 1.0f - keys[i1]->rotTension();
    float fOmC0 = 1.0f - keys[i1]->rotContinuity();
    float fOpC0 = 1.0f + keys[i1]->rotContinuity();
    float fOmB0 = 1.0f - keys[i1]->rotBias();
    float fOpB0 = 1.0f + keys[i1]->rotBias();
    float fAdj0 = 2.0f * (keys[i2]->time() - keys[i1]->time()) /
                  (keys[i2]->time() - keys[i0]->time());
    float fOut0 = 0.5f * fAdj0 * fOmT0 * fOpC0 * fOpB0;
    float fOut1 = 0.5f * fAdj0 * fOmT0 * fOmC0 * fOmB0;
    // build outgoing tangent at q[i1]
    glm::quat kTOut = fOut1 * kLog21 + fOut0 * kLog10;
    // build multipliers at q[i2]
    float fOmT1 = 1.0f - keys[i2]->rotTension();
    float fOmC1 = 1.0f - keys[i2]->rotContinuity();
    float fOpC1 = 1.0f + keys[i2]->rotContinuity();
    float fOmB1 = 1.0f - keys[i2]->rotBias();
    float fOpB1 = 1.0f + keys[i2]->rotBias();
    float fAdj1 = 2.0f * (keys[i2]->time() - keys[i1]->time()) /
                  (keys[i3]->time() - keys[i1]->time());
    float fIn0 = 0.5f * fAdj1 * fOmT1 * fOmC1 * fOpB1;
    float fIn1 = 0.5f * fAdj1 * fOmT1 * fOpC1 * fOmB1;
    // build incoming tangent at q[i2]
    glm::quat kTIn = fIn1 * kLog32 + fIn0 * kLog21;
    rotSpline[i0].m_kP = kQ1;
    rotSpline[i0].m_kQ = kQ2;
    rotSpline[i0].m_kA = kQ1 * glm::exp((kTOut + (-kLog21)) * 0.5f);
    rotSpline[i0].m_kB = kQ2 * glm::exp(0.5f * (kLog21 + (-kTIn)));
    rotSpline[i0].m_fTMin = keys[i1]->time();
    rotSpline[i0].m_fTMax = keys[i2]->time();
    rotSpline[i0].m_fTInvRange = 1.0f / (keys[i2]->time() - keys[i1]->time());
  }
}

void ZCameraParameterAnimation::SplineRange::doPolyLookup(float fTime, int& riI, float& rfU) const
{
  // Lookup the polynomial that contains the input time in its domain of
  // evaluation. Clamp to [tmin,tmax].
  if (startTime() < fTime) {
    if (fTime < endTime()) {
      for (riI = 0; riI < posSpline.size(); riI++) {
        if (fTime < posSpline[riI].m_fTMax)
          break;
      }
      rfU = (fTime - posSpline[riI].m_fTMin) * posSpline[riI].m_fTInvRange;
    }
    else {
      riI = posSpline.size() - 1;
      rfU = 1.0f;
    }
  }
  else {
    riI = 0;
    rfU = 0.0f;
  }
}

void ZCameraParameterAnimation::SplineRange::invertIntegral(float fS, int& riI, float& rfU)
{
  // clamp s to [0,L] so that t in [tmin,tmax]
  if (fS <= 0.0f) {
    riI = 0;
    rfU = 0.0f;
    return;
  }
  if (fS >= posSplineTotalLength) {
    riI = posSpline.size() - 1;
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
    if (std::abs(fDiff) <= fTolerance)
      break;

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
}

} // namespace nim
