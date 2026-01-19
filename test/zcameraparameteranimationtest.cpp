#include "zcameraparameteranimation.h"

#include "z3dcameraparameter.h"
#include "zglmutils.h"
#include "ztest.h"

using namespace nim;

namespace {

struct QuatKey
{
  float time = 0.0f;
  glm::quat rot{};
  float tension = 0.0f;
  float continuity = 0.0f;
  float bias = 0.0f;
};

struct SquadPoly
{
  float tmin = 0.0f;
  float tmax = 0.0f;
  float invRange = 1.0f;
  glm::quat p{};
  glm::quat a{};
  glm::quat b{};
  glm::quat q{};

  [[nodiscard]] glm::quat eval(float t) const
  {
    const float u = (t - tmin) * invRange;
    return glm::squad(p, q, a, b, u);
  }
};

[[nodiscard]] std::vector<SquadPoly> buildExpectedRotSpline(std::vector<QuatKey> keys)
{
  // Mirror ZCameraParameterAnimation::SplineRange behavior:
  // - Duplicate first/last keys to support endpoint tangent construction.
  CHECK(keys.size() >= 2);
  keys.insert(keys.begin(), keys.front());
  keys.push_back(keys.back());

  // Ensure consecutive quaternions form an acute angle. This is required for
  // stable log/exp spline construction (q and -q represent the same rotation).
  for (size_t i = 1; i < keys.size(); ++i) {
    if (glm::dot(keys[i].rot, keys[i - 1].rot) < 0.0f) {
      keys[i].rot = -keys[i].rot;
    }
  }

  std::vector<SquadPoly> out;
  out.reserve(keys.size() - 3);

  for (size_t i0 = 0, i1 = 1, i2 = 2, i3 = 3; i3 < keys.size(); i0++, i1++, i2++, i3++) {
    const glm::quat kQ0 = keys[i0].rot;
    const glm::quat kQ1 = keys[i1].rot;
    const glm::quat kQ2 = keys[i2].rot;
    const glm::quat kQ3 = keys[i3].rot;

    const glm::quat kLog10 = glm::log(glm::conjugate(kQ0) * kQ1);
    const glm::quat kLog21 = glm::log(glm::conjugate(kQ1) * kQ2);
    const glm::quat kLog32 = glm::log(glm::conjugate(kQ2) * kQ3);

    // Build multipliers at q[i1]
    const float fOmT0 = 1.0f - keys[i1].tension;
    const float fOmC0 = 1.0f - keys[i1].continuity;
    const float fOpC0 = 1.0f + keys[i1].continuity;
    const float fOmB0 = 1.0f - keys[i1].bias;
    const float fOpB0 = 1.0f + keys[i1].bias;
    const float fAdj0 =
      2.0f * (keys[i2].time - keys[i1].time) / (keys[i2].time - keys[i0].time);
    const float fOut0 = 0.5f * fAdj0 * fOmT0 * fOpC0 * fOpB0;
    const float fOut1 = 0.5f * fAdj0 * fOmT0 * fOmC0 * fOmB0;
    const glm::quat kTOut = fOut1 * kLog21 + fOut0 * kLog10;

    // Build multipliers at q[i2]
    const float fOmT1 = 1.0f - keys[i2].tension;
    const float fOmC1 = 1.0f - keys[i2].continuity;
    const float fOpC1 = 1.0f + keys[i2].continuity;
    const float fOmB1 = 1.0f - keys[i2].bias;
    const float fOpB1 = 1.0f + keys[i2].bias;
    const float fAdj1 =
      2.0f * (keys[i2].time - keys[i1].time) / (keys[i3].time - keys[i1].time);
    const float fIn0 = 0.5f * fAdj1 * fOmT1 * fOmC1 * fOpB1;
    const float fIn1 = 0.5f * fAdj1 * fOmT1 * fOpC1 * fOmB1;
    const glm::quat kTIn = fIn1 * kLog32 + fIn0 * kLog21;

    SquadPoly poly;
    poly.p = kQ1;
    poly.q = kQ2;
    poly.a = kQ1 * glm::exp((kTOut + (-kLog21)) * 0.5f);
    poly.b = kQ2 * glm::exp(0.5f * (kLog21 + (-kTIn)));
    poly.tmin = keys[i1].time;
    poly.tmax = keys[i2].time;
    poly.invRange = 1.0f / (keys[i2].time - keys[i1].time);
    out.push_back(poly);
  }

  return out;
}

[[nodiscard]] glm::quat evalExpected(const std::vector<SquadPoly>& spline, float t)
{
  CHECK(!spline.empty());
  if (t < spline.front().tmin) {
    return spline.front().eval(spline.front().tmin);
  }
  if (t >= spline.back().tmax) {
    return spline.back().eval(spline.back().tmax);
  }
  for (const auto& poly : spline) {
    if (t < poly.tmax) {
      return poly.eval(t);
    }
  }
  return spline.back().eval(t);
}

} // namespace

TEST(ZCameraParameterAnimationTest, RotationSplineUsesSignConsistentQuaternions)
{
  // Construct a sequence of camera rotations that crosses 180° so that the raw
  // quaternion representation may flip sign (q and -q are the same rotation).
  // The rotation spline builder must enforce sign-consistency before log/exp.

  class TestAnim final : public ZCameraParameterAnimation
  {
  public:
    using ZCameraParameterAnimation::ZCameraParameterAnimation;

    [[nodiscard]] glm::quat rotationAt(float t) const
    {
      CHECK(!m_pathSegments.empty());
      return m_pathSegments.front().rotation(t);
    }
  };

  const glm::vec3 eye(0.0f, 0.0f, 0.0f);
  const glm::vec3 center(0.0f, 0.0f, -1.0f);
  const glm::vec3 up(0.0f, 1.0f, 0.0f);

  Z3DCameraParameter cam0("Camera");
  cam0.setCamera(eye, center, up);
  Z3DCameraParameter cam1("Camera");
  cam1.setCamera(eye, center, up);
  cam1.yaw(glm::radians(200.0f));
  Z3DCameraParameter cam2("Camera");
  cam2.setCamera(eye, center, up);
  cam2.yaw(glm::radians(10.0f));

  const float sampleT = 1.5f;
  TestAnim anim("CameraAnim");
  anim.interpolationMethodPara().select("Position Rotation Spline");
  anim.addKey(std::make_unique<ZCameraParameterKey>(0.0, cam0), /*keepRedundant=*/true);
  anim.addKey(std::make_unique<ZCameraParameterKey>(1.0, cam1), /*keepRedundant=*/true);
  anim.addKey(std::make_unique<ZCameraParameterKey>(2.0, cam2), /*keepRedundant=*/true);
  anim.buildSpline();
  const glm::quat actual = anim.rotationAt(sampleT);

  std::vector<QuatKey> ks;
  ks.push_back({.time = 0.0f, .rot = glm::quat_cast(cam0.get().viewMatrix(MonoEye))});
  ks.push_back({.time = 1.0f, .rot = glm::quat_cast(cam1.get().viewMatrix(MonoEye))});
  ks.push_back({.time = 2.0f, .rot = glm::quat_cast(cam2.get().viewMatrix(MonoEye))});

  const std::vector<SquadPoly> expectedSpline = buildExpectedRotSpline(std::move(ks));
  const glm::quat expected = evalExpected(expectedSpline, sampleT);

  // Compare rotations up to the quaternion sign ambiguity.
  const float dot = std::abs(glm::dot(glm::normalize(actual), glm::normalize(expected)));
  EXPECT_GE(dot, 1.0f - 1e-4f);
}
