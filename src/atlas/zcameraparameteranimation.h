#pragma once

#include "zparameteranimation.h"
#include "z3dcameraparameter.h"
#include "zcameraparameterkey.h"
#include "zglobal.h"

namespace nim {

// Kochanek-Bartels tension-continuity-bias spline interpolation for
// positional data.
// Kochanek-Bartels tension-continuity-bias spline interpolation adapted
// to quaternion interpolation.

class ZCameraParameterAnimation : public ZParameterAnimation
{
  Q_OBJECT

public:
  explicit ZCameraParameterAnimation(const QString& name,
                                     const QColor& color = QColor(ZRandom::instance().randInt(255),
                                                                  ZRandom::instance().randInt(255),
                                                                  ZRandom::instance().randInt(255)),
                                     QObject* parent = nullptr);

  static void configureInterpolationMethodParameter(ZStringIntOptionParameter& parameter);

  ZStringIntOptionParameter& interpolationMethodPara()
  {
    return m_interpolationMethod;
  }

  [[nodiscard]] const ZStringIntOptionParameter& interpolationMethodPara() const
  {
    return m_interpolationMethod;
  }

  [[nodiscard]] bool setInterpolationMethod(const QString& method);

  // create a new key based on current view
  [[nodiscard]] std::unique_ptr<ZParameterKey> createKey(double secs) const override;

  void updateParaToTime(double secs, ZParameter* para) const override;

  void buildSpline();

  void write(json::object& json) const override;

Q_SIGNALS:
  void interpolationMethodChanged(const QString& method);

protected:
  struct CameraKeySample
  {
    explicit CameraKeySample(const ZCameraParameterKey& key);

    float time = 0.f;
    glm::vec3 eye{0.f};
    glm::quat rot{};
    float posTension = 0.f;
    float posContinuity = 0.f;
    float posBias = 0.f;
    float rotTension = 0.f;
    float rotContinuity = 0.f;
    float rotBias = 0.f;
  };

  class Poly
  {
  public:
    [[nodiscard]] glm::vec3 Position(float fU) const; // P(u)
    [[nodiscard]] glm::vec3 Velocity(float fU) const; // P'(u)
    [[nodiscard]] glm::vec3 Acceleration(float fU) const; // P"(u)
    [[nodiscard]] float Speed(float fU) const;

    [[nodiscard]] float Length(float fU) const;

    // Time interval on which polynomial is valid, tmin <= t <= tmax.
    // The normalized time is u = (t - tmin)/(tmax - tmin). The inverse
    // range 1/(tmax-tmin) is computed once and stored to avoid having to
    // use divisions during interpolation.
    float m_fTMin, m_fTMax, m_fTInvRange;
    // P(u) = C0 + u*C1 + u^2*C2 + u^3*C3, 0 <= u <= 1
    glm::vec3 m_akC[4];
    // Legendre polynomial degree 5 for numerical integration
    static float ms_afModRoot[5];
    static float ms_afModCoeff[5];
  };

  class SquadPoly
  {
  public:
    [[nodiscard]] glm::quat Q(float fU) const;

    // Time interval on which polynomial is valid, tmin <= t <= tmax.
    // The normalized time is u = (t - tmin)/(tmax - tmin). The inverse
    // range 1/(tmax-tmin) is computed once and stored to avoid having to
    // use divisions during interpolation.
    float m_fTMin, m_fTMax, m_fTInvRange;
    // Q(u) = Squad(2u(1-u),Slerp(u,p,q),Slerp(u,a,b))
    glm::quat m_kP, m_kA, m_kB, m_kQ;
  };

  struct SplineRange
  {
    SplineRange() = default;

    explicit SplineRange(const std::vector<ZCameraParameterKey*>& kys);

    [[nodiscard]] glm::quat rotation(float fTime) const;

    [[nodiscard]] glm::vec3 position(float fTime) const; // X(t)
    glm::vec3 velocity(float fTime); // X'(t)
    glm::vec3 acceleration(float fTime); // X"(t)
    // length of the spline
    float length(float fTime);

    [[nodiscard]] float totalLength() const;

    // Evaluate position and derivatives by specifying arc length s along the
    // spline. If L is the total length of the curve, then 0 <= s <= L is
    // required.
    glm::vec3 positionAL(float fS);

    glm::vec3 velocityAL(float fS);

    glm::vec3 accelerationAL(float fS);

    void buildPosSpline();

    void buildRotSpline();

    void doPolyLookup(float fTime, size_t& riI, float& rfU) const;

    // support for arc length parameterization of spline
    void invertIntegral(float fS, size_t& riI, float& rfU);

    [[nodiscard]] float startTime() const
    {
      CHECK(!keys.empty());
      return keys[0].time;
    }

    [[nodiscard]] float endTime() const
    {
      CHECK(!keys.empty());
      return keys.back().time;
    }

    void swap(SplineRange& rhs) noexcept;

    std::vector<CameraKeySample> keys;
    std::vector<Poly> posSpline;
    std::vector<float> posSplineLengths;
    float posSplineTotalLength = 0;
    std::vector<SquadPoly> rotSpline;
    bool m_hasSpline = false;
  };

  std::vector<SplineRange> m_pathSegments;
  ZStringIntOptionParameter m_interpolationMethod;
};

} // namespace nim
