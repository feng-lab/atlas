#include "zcameraparameteranimation.h"

#include "z3dcameraparameter.h"
#include "zglmutils.h"
#include "ztest.h"

#include <string>
#include <utility>
#include <vector>

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
  CHECK(keys.size() >= 2);

  // Ensure consecutive quaternions form an acute angle. This is required for
  // stable log/exp spline construction (q and -q represent the same rotation).
  for (size_t i = 1; i < keys.size(); ++i) {
    if (glm::dot(keys[i].rot, keys[i - 1].rot) < 0.0f) {
      keys[i].rot = -keys[i].rot;
    }
  }

  std::vector<SquadPoly> out;
  out.resize(keys.size() - 1);

  std::vector<glm::quat> inTangents(keys.size(), glm::quat(0.f, 0.f, 0.f, 0.f));
  std::vector<glm::quat> outTangents(keys.size(), glm::quat(0.f, 0.f, 0.f, 0.f));

  {
    const float delta = keys[1].time - keys[0].time;
    CHECK(delta > 0.f);
    const float coeff = (1.0f - keys[0].tension) * (1.0f - keys[0].continuity) * (1.0f - keys[0].bias) / (2.0f * delta);
    outTangents[0] = coeff * glm::log(glm::conjugate(keys[0].rot) * keys[1].rot);
    inTangents[0] = outTangents[0];
  }

  for (size_t k = 1; k + 1 < keys.size(); ++k) {
    const float prevDelta = keys[k].time - keys[k - 1].time;
    const float nextDelta = keys[k + 1].time - keys[k].time;
    CHECK(prevDelta > 0.f);
    CHECK(nextDelta > 0.f);
    const glm::quat prevLog = glm::log(glm::conjugate(keys[k - 1].rot) * keys[k].rot);
    const glm::quat nextLog = glm::log(glm::conjugate(keys[k].rot) * keys[k + 1].rot);
    const float omT = 1.0f - keys[k].tension;
    const float omC = 1.0f - keys[k].continuity;
    const float opC = 1.0f + keys[k].continuity;
    const float omB = 1.0f - keys[k].bias;
    const float opB = 1.0f + keys[k].bias;
    inTangents[k] = (omT * omC * opB / (2.0f * prevDelta)) * prevLog + (omT * opC * omB / (2.0f * nextDelta)) * nextLog;
    outTangents[k] =
      (omT * opC * opB / (2.0f * prevDelta)) * prevLog + (omT * omC * omB / (2.0f * nextDelta)) * nextLog;
  }

  {
    const size_t last = keys.size() - 1;
    const float delta = keys[last].time - keys[last - 1].time;
    CHECK(delta > 0.f);
    const float coeff =
      (1.0f - keys[last].tension) * (1.0f - keys[last].continuity) * (1.0f + keys[last].bias) / (2.0f * delta);
    inTangents[last] = coeff * glm::log(glm::conjugate(keys[last - 1].rot) * keys[last].rot);
    outTangents[last] = inTangents[last];
  }

  for (size_t k = 0; k + 1 < keys.size(); ++k) {
    const float delta = keys[k + 1].time - keys[k].time;
    CHECK(delta > 0.f);
    const glm::quat segmentLog = glm::log(glm::conjugate(keys[k].rot) * keys[k + 1].rot);
    out[k].p = keys[k].rot;
    out[k].q = keys[k + 1].rot;
    out[k].a = keys[k].rot * glm::exp(0.5f * (delta * outTangents[k] - segmentLog));
    out[k].b = keys[k + 1].rot * glm::exp(0.5f * (segmentLog - delta * inTangents[k + 1]));
    out[k].tmin = keys[k].time;
    out[k].tmax = keys[k + 1].time;
    out[k].invRange = 1.0f / delta;
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

class TestAnim final : public ZCameraParameterAnimation
{
public:
  using ZCameraParameterAnimation::ZCameraParameterAnimation;

  [[nodiscard]] glm::quat rotationAt(float t) const
  {
    CHECK(!m_pathSegments.empty());
    return m_pathSegments.front().rotation(t);
  }

  [[nodiscard]] glm::vec3 positionAt(float t) const
  {
    CHECK(!m_pathSegments.empty());
    return m_pathSegments.front().position(t);
  }

  [[nodiscard]] size_t numPathSegments() const
  {
    return m_pathSegments.size();
  }
};

[[nodiscard]] std::unique_ptr<ZCameraParameterKey> makeCameraKey(double time, const glm::vec3& eye)
{
  Z3DCameraParameter cam("Camera");
  cam.setCamera(eye, eye + glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  return std::make_unique<ZCameraParameterKey>(time, cam);
}

void expectVecNear(const glm::vec3& actual, const glm::vec3& expected, float tolerance)
{
  EXPECT_NEAR(actual.x, expected.x, tolerance);
  EXPECT_NEAR(actual.y, expected.y, tolerance);
  EXPECT_NEAR(actual.z, expected.z, tolerance);
}

} // namespace

TEST(ZCameraParameterAnimationTest, RotationSplineUsesSignConsistentQuaternions)
{
  // Construct a sequence of camera rotations that crosses 180° so that the raw
  // quaternion representation may flip sign (q and -q are the same rotation).
  // The rotation spline builder must enforce sign-consistency before log/exp.

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

TEST(ZCameraParameterAnimationTest, PositionSplineMatchesRecordedNonUniformTcbRegression)
{
  TestAnim anim("CameraAnim");
  anim.interpolationMethodPara().select("Position Spline");

  struct PosKey
  {
    float time;
    glm::vec3 eye;
    float tension;
    float continuity;
    float bias;
  };
  const std::vector<PosKey> keys = {
    {.time = 0.0f, .eye = {0.0f, 0.0f, 0.0f}, .tension = 0.25f, .continuity = -0.2f,  .bias = 0.1f },
    {.time = 0.7f, .eye = {1.0f, 2.0f, 0.5f}, .tension = -0.1f, .continuity = 0.35f,  .bias = -0.3f},
    {.time = 2.4f, .eye = {3.0f, 1.0f, 2.0f}, .tension = 0.4f,  .continuity = -0.25f, .bias = 0.2f },
    {.time = 5.0f, .eye = {4.0f, 3.0f, 1.0f}, .tension = -0.2f, .continuity = 0.1f,   .bias = 0.45f},
  };

  for (const auto& src : keys) {
    auto key = makeCameraKey(src.time, src.eye);
    key->setPosTension(src.tension);
    key->setPosContinuity(src.continuity);
    key->setPosBias(src.bias);
    anim.addKey(std::move(key), /*keepRedundant=*/true);
  }

  const std::vector<std::pair<float, glm::vec3>> expectedSamples = {
    {0.1f,  glm::vec3(0.079618161550f, 0.193999228263f, 0.036332790259f)},
    {0.7f,  glm::vec3(1.000000000000f, 2.000000000000f, 0.500000000000f)},
    {1.25f, glm::vec3(1.745001922392f, 2.073859145771f, 1.026615597453f)},
    {2.4f,  glm::vec3(3.000000000000f, 1.000000000000f, 2.000000000000f)},
    {3.6f,  glm::vec3(3.503274197435f, 1.730251840745f, 1.690133390452f)},
    {4.9f,  glm::vec3(3.969434004257f, 2.935931182896f, 1.032621773675f)},
  };
  for (const auto& [t, expected] : expectedSamples) {
    SCOPED_TRACE(std::string("t=") + std::to_string(t));
    expectVecNear(anim.positionAt(t), expected, 1e-5f);
  }
}

TEST(ZCameraParameterAnimationTest, DeleteKeyRebuildsSplineCache)
{
  TestAnim anim("CameraAnim");
  anim.interpolationMethodPara().select("Position Spline");
  anim.addKey(makeCameraKey(0.0, {0.0f, 0.0f, 0.0f}), /*keepRedundant=*/true);
  anim.addKey(makeCameraKey(1.0, {10.0f, 0.0f, 0.0f}), /*keepRedundant=*/true);
  anim.addKey(makeCameraKey(2.0, {0.0f, 2.0f, 0.0f}), /*keepRedundant=*/true);
  ASSERT_EQ(anim.numPathSegments(), 1_uz);

  ZParameterKey* middle = anim.keys()[1].get();
  anim.deleteKey(middle);
  ASSERT_EQ(anim.keys().size(), 2_uz);
  ASSERT_EQ(anim.numPathSegments(), 1_uz);

  expectVecNear(anim.positionAt(1.5f), glm::vec3(0.0f, 1.59375f, 0.0f), 1e-5f);
}

TEST(ZCameraParameterAnimationTest, CameraInterpolationMethodPersistsInTrackJson)
{
  ZCameraParameterAnimation anim("Camera");
  ASSERT_TRUE(anim.setInterpolationMethod("Position Rotation Spline"));

  json::object root;
  anim.write(root);
  std::unique_ptr<ZParameterAnimation> loaded(
    ZParameterAnimation::create(anim.jsonKey(), root.at(anim.jsonKey().toStdString())));
  ASSERT_TRUE(loaded);
  auto* loadedCamera = qobject_cast<ZCameraParameterAnimation*>(loaded.get());
  ASSERT_TRUE(loadedCamera);
  EXPECT_EQ(loadedCamera->interpolationMethodPara().get(), QStringLiteral("Position Rotation Spline"));
}

TEST(ZCameraParameterAnimationTest, RejectsOutOfRangeTcbOnRead)
{
  const auto source = makeCameraKey(0.0, {0.0f, 0.0f, 0.0f});
  json::value value = source->jsonValue();
  value.as_object()["posTension"] = 2.0;

  ZCameraParameterKey loaded;
  EXPECT_FALSE(loaded.readValue(value));

  value = source->jsonValue();
  value.as_object()["rotBias"] = "bad";
  EXPECT_FALSE(loaded.readValue(value));
}

TEST(ZCameraParameterAnimationTest, ReadValueAcceptsIntegerJsonForCameraNumbers)
{
  json::object cameraValue;
  cameraValue["Center Position Vec3"] = json::array{4, 5, 6};
  cameraValue["Eye Position Vec3"] = json::array{1, 2, 3};
  cameraValue["Eye Separation Angle Float"] = 8;
  cameraValue["Field of View Float"] = 45;
  cameraValue["Projection Type StringIntOption"] = "Perspective";
  cameraValue["Up Vector Vec3"] = json::array{0, 1, 0};

  json::object keyValue;
  keyValue["posBias"] = 0;
  keyValue["posContinuity"] = 0;
  keyValue["posTension"] = 0;
  keyValue["rotBias"] = 0;
  keyValue["rotContinuity"] = 0;
  keyValue["rotTension"] = 0;
  keyValue["time"] = 5;
  keyValue["type"] = "Linear";
  keyValue["value"] = cameraValue;

  ZCameraParameterKey loaded;
  ASSERT_TRUE(loaded.readValue(keyValue));
  EXPECT_DOUBLE_EQ(loaded.time(), 5.0);
  EXPECT_FLOAT_EQ(loaded.posTension(), 0.0f);
  EXPECT_FLOAT_EQ(loaded.rotBias(), 0.0f);

  const auto& cameraParam = dynamic_cast<const Z3DCameraParameter&>(loaded.value());
  expectVecNear(cameraParam.get().eye(), glm::vec3(1.0f, 2.0f, 3.0f), 0.0f);
  expectVecNear(cameraParam.get().center(), glm::vec3(4.0f, 5.0f, 6.0f), 0.0f);
}
