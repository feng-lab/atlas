#ifndef ZCAMERAPARAMETERANIMATION_H
#define ZCAMERAPARAMETERANIMATION_H

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
  ZCameraParameterAnimation(const QString &name,
                            const QColor &color = QColor(ZRandomInstance.randInt(255),
                                                         ZRandomInstance.randInt(255),
                                                         ZRandomInstance.randInt(255)),
                            QObject *parent = 0);
  ~ZCameraParameterAnimation();

  ZStringIntOptionParameter* interpolationMethodPara() { return &m_interpolationMethod; }

  // create a new key based on current view
  virtual ZParameterKey* createKey(double secs) const override;

signals:
  void interpolationMethodChanged();

public slots:
  virtual void updateParaToTime(double secs, ZParameter* para) const override;
  void buildSpline();

protected:
  glm::vec3 accelerationAL (double fS);
  class Poly
  {
  public:
    glm::vec3 Position(float fU) const; // P(u)
    glm::vec3 Velocity(float fU); // P'(u)
    glm::vec3 Acceleration(float fU); // P"(u)
    float Speed(float fU);
    float Length(float fU);
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
    glm::quat Q(float fU) const;
    // Time interval on which polynomial is valid, tmin <= t <= tmax.
    // The normalized time is u = (t - tmin)/(tmax - tmin). The inverse
    // range 1/(tmax-tmin) is computed once and stored to avoid having to
    // use divisions during interpolation.
    float m_fTMin, m_fTMax, m_fTInvRange;
    // Q(u) = Squad(2u(1-u),Slerp(u,p,q),Slerp(u,a,b))
    glm::quat m_kP, m_kA, m_kB, m_kQ;
  };

  struct SplineRange {
    SplineRange();
    SplineRange(QList<ZCameraParameterKey*> &kys);
    ~SplineRange();

    glm::quat rotation(float fTime) const;
    glm::vec3 position(double fTime) const; // X(t)
    glm::vec3 velocity(double fTime); // X'(t)
    glm::vec3 acceleration(double fTime); // X"(t)
    // length of the spline
    double length(double fTime);
    double totalLength();
    // Evaluate position and derivatives by specifying arc length s along the
    // spline. If L is the total length of the curve, then 0 <= s <= L is
    // required.
    glm::vec3 positionAL(double fS);
    glm::vec3 velocityAL(double fS);
    glm::vec3 accelerationAL (double fS);

    void buildPosSpline();
    void buildRotSpline();

    void doPolyLookup(float fTime, int& riI, float& rfU) const;
    // support for arc length parameterization of spline
    void invertIntegral(float fS, int& riI, float& rfU);

    double startTime() const { return keys[0]->time(); }
    double endTime() const { return keys[keys.size()-1]->time(); }

    bool hasSpline() const { return keys.size() >= 2; }

    void swap(SplineRange &rhs) noexcept;

    QList<ZCameraParameterKey*> keys;
    QList<Poly> posSpline;
    std::vector<float> posSplineLengths;
    float posSplineTotalLength;
    QList<SquadPoly> rotSpline;
  };
  QList<SplineRange> m_pathSegments;

protected:
  ZStringIntOptionParameter m_interpolationMethod;
};

} // namespace

#endif // ZCAMERAPARAMETERANIMATION_H
