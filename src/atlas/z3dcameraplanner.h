#pragma once

#include "zbbox.h"
#include "zglmutils.h"
#include "zjson.h"

#include <optional>
#include <string>
#include <vector>

namespace nim {

class Z3DCamera;
class Z3DCameraParameter;

struct Z3DCameraPlannerSolveKey
{
  double time = 0.0;
  json::value value;
};

struct Z3DCameraPlannerValidateResult
{
  double time = 0.0;
  bool withinFrame = false;
  double frameCoverage = 0.0;
  bool adjusted = false;
  std::string reason;
  std::optional<json::value> adjustedValue;
};

struct Z3DCameraPlannerPathWaypoint
{
  enum class EyeMode
  {
    Keep,
    World,
    BBoxFraction,
  };

  enum class LookAtMode
  {
    KeepPrevDirection,
    World,
    BBoxCenter,
    BBoxFraction,
  };

  double time = 0.0;
  int index = 0;

  EyeMode eyeMode = EyeMode::Keep;
  glm::vec3 eyeWorld{0.f, 0.f, 0.f};
  glm::dvec3 eyeBBoxFraction{0.0, 0.0, 0.0};

  LookAtMode lookAtMode = LookAtMode::KeepPrevDirection;
  glm::vec3 lookAtWorld{0.f, 0.f, 0.f};
  glm::dvec3 lookAtBBoxFraction{0.0, 0.0, 0.0};
};

class Z3DCameraPlanner
{
public:
  enum class SolveMode
  {
    Fit,
    Static,
    Orbit,
    Dolly,
  };

  struct OrbitParams
  {
    // 'x', 'y', or 'z' (lowercase).
    char axis = 'y';
    double degrees = 360.0;
    double maxStepDegrees = 90.0;
  };

  struct DollyParams
  {
    double startDist = 0.0;
    double endDist = 0.0;
  };

  struct SolveRequest
  {
    SolveMode mode = SolveMode::Static;
    double t0 = 0.0;
    double t1 = 0.0;
    ZBBox<glm::dvec3> bbox;
    double margin = 0.0;
    OrbitParams orbit;
    DollyParams dolly;
  };

  struct ValidatePolicies
  {
    bool adjustDistance = false;
  };

  struct ValidateConstraints
  {
    bool keepVisible = true;
    double minFrameCoverage = 0.0;
    double margin = 0.0;
  };

  [[nodiscard]] static std::vector<Z3DCameraPlannerSolveKey> solve(const Z3DCameraParameter& base,
                                                                   const SolveRequest& request,
                                                                   std::string& error);

  [[nodiscard]] static std::vector<Z3DCameraPlannerSolveKey> solvePath(
    const Z3DCameraParameter& base,
    const std::vector<Z3DCameraPlannerPathWaypoint>& waypoints,
    const std::optional<ZBBox<glm::dvec3>>& bboxOrNull,
    std::string& error);

  [[nodiscard]] static std::vector<Z3DCameraPlannerValidateResult> validate(
    const Z3DCameraParameter& base,
    const ZBBox<glm::dvec3>& bbox,
    const std::vector<double>& times,
    const std::vector<json::value>& values,
    const ValidateConstraints& constraints,
    const ValidatePolicies& policies,
    std::string& error);

  [[nodiscard]] static double bboxEnclosingSphereRadius(const ZBBox<glm::dvec3>& bb);
  [[nodiscard]] static glm::vec3 bboxFractionToWorld(const ZBBox<glm::dvec3>& bb, const glm::dvec3& frac);

private:
  [[nodiscard]] static ZBBox<glm::dvec3> expandedByMarginFraction(const ZBBox<glm::dvec3>& bb, double marginFrac);
  [[nodiscard]] static double requiredCenterDistanceForCoverage(const Z3DCamera& cam, double radius);
  static void setCameraDistance(Z3DCameraParameter& cam, double centerDist);
};

} // namespace nim
