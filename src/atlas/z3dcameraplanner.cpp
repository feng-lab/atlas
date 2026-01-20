#include "z3dcameraplanner.h"

#include "z3dcamera.h"
#include "z3dcameraparameter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nim {

ZBBox<glm::dvec3> Z3DCameraPlanner::expandedByMarginFraction(const ZBBox<glm::dvec3>& bb, double marginFrac)
{
  if (bb.empty() || marginFrac <= 0.0) {
    return bb;
  }
  const glm::dvec3 half = (bb.maxCorner - bb.minCorner) * 0.5;
  const glm::dvec3 grow = half * marginFrac;
  ZBBox<glm::dvec3> out = bb;
  out.expand(bb.minCorner - grow);
  out.expand(bb.maxCorner + grow);
  return out;
}

double Z3DCameraPlanner::bboxEnclosingSphereRadius(const ZBBox<glm::dvec3>& bb)
{
  if (bb.empty()) {
    return 0.0;
  }
  const glm::dvec3 sz = (bb.maxCorner - bb.minCorner);
  return 0.5 * std::sqrt(sz.x * sz.x + sz.y * sz.y + sz.z * sz.z);
}

double Z3DCameraPlanner::requiredCenterDistanceForCoverage(const Z3DCamera& cam, double radius)
{
  if (radius <= 0.0) {
    return 0.0;
  }
  double angle = cam.fieldOfView();
  // Match Z3DCamera::resetCamera logic: use horizontal angle when AR < 1
  if (cam.aspectRatio() < 1.0f) {
    angle = 2.0 * std::atan(std::tan(angle * 0.5) * cam.aspectRatio());
  }
  const double s = std::sin(angle * 0.5);
  if (s <= 1e-6) {
    return std::numeric_limits<double>::infinity();
  }
  return radius / s;
}

void Z3DCameraPlanner::setCameraDistance(Z3DCameraParameter& cam, double centerDist)
{
  const glm::vec3 c = cam.get().center();
  const glm::vec3 v = cam.get().viewVector();
  const glm::vec3 eye = c - static_cast<float>(centerDist) * v;
  const glm::vec3 up = cam.get().upVector();
  cam.setCamera(eye, c, up);
}

glm::vec3 Z3DCameraPlanner::bboxFractionToWorld(const ZBBox<glm::dvec3>& bb, const glm::dvec3& frac)
{
  const auto clamp01 = [](double v) { return std::max(0.0, std::min(1.0, v)); };
  const double fx = clamp01(frac.x);
  const double fy = clamp01(frac.y);
  const double fz = clamp01(frac.z);
  const glm::dvec3 sz = (bb.maxCorner - bb.minCorner);
  const glm::dvec3 p = bb.minCorner + glm::dvec3(fx * sz.x, fy * sz.y, fz * sz.z);
  return glm::vec3(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
}

std::vector<Z3DCameraPlannerSolveKey> Z3DCameraPlanner::solve(const Z3DCameraParameter& base,
                                                              const SolveRequest& request,
                                                              std::string& error)
{
  error.clear();

  ZBBox<glm::dvec3> bb = request.bbox;
  if (!bb.empty() && request.margin > 0.0) {
    bb = expandedByMarginFraction(bb, request.margin);
  }

  std::vector<Z3DCameraPlannerSolveKey> keys;

  switch (request.mode) {
    case SolveMode::Fit: {
      if (bb.empty()) {
        return keys;
      }
      Z3DCameraParameter cam("Camera");
      cam.setValueSameAs(base);
      cam.resetCamera(bb, Z3DCamera::ResetOption::PreserveViewVector);
      keys.push_back(Z3DCameraPlannerSolveKey{request.t0, cam.jsonValue()});
      return keys;
    }
    case SolveMode::Static: {
      keys.push_back(Z3DCameraPlannerSolveKey{request.t0, base.jsonValue()});
      return keys;
    }
    case SolveMode::Orbit: {
      if (bb.empty()) {
        return keys;
      }
      if (!std::isfinite(request.orbit.maxStepDegrees) || request.orbit.maxStepDegrees <= 0.0) {
        error = "max_step_degrees must be a finite number > 0";
        return {};
      }

      glm::vec3 ax(0.f, 1.f, 0.f);
      if (request.orbit.axis == 'x') {
        ax = glm::vec3(1.f, 0.f, 0.f);
      } else if (request.orbit.axis == 'y') {
        ax = glm::vec3(0.f, 1.f, 0.f);
      } else if (request.orbit.axis == 'z') {
        ax = glm::vec3(0.f, 0.f, 1.f);
      } else {
        error = "axis must be one of: x, y, z";
        return {};
      }

      const glm::vec3 center = glm::vec3((bb.minCorner + bb.maxCorner) * 0.5);
      const double angDeg = request.orbit.degrees;
      const double aabs = std::abs(angDeg);
      const double segD = std::ceil(aabs / request.orbit.maxStepDegrees);
      if (!std::isfinite(segD) || segD < 0.0 || segD > static_cast<double>(std::numeric_limits<int>::max())) {
        error = "invalid orbit segmentation (check max_step_degrees/degrees)";
        return {};
      }
      const int segments = std::max(1, static_cast<int>(segD));
      const double stepDeg = angDeg / static_cast<double>(segments);
      const double dt = (request.t1 - request.t0) / static_cast<double>(segments);

      Z3DCameraParameter cam("Camera");
      cam.setValueSameAs(base);
      cam.resetCamera(bb, Z3DCamera::ResetOption::PreserveViewVector);
      keys.push_back(Z3DCameraPlannerSolveKey{request.t0, cam.jsonValue()});
      for (int i = 1; i <= segments; ++i) {
        cam.rotate(glm::radians(static_cast<float>(stepDeg)), ax, center);
        keys.push_back(Z3DCameraPlannerSolveKey{request.t0 + dt * static_cast<double>(i), cam.jsonValue()});
      }
      return keys;
    }
    case SolveMode::Dolly: {
      if (bb.empty()) {
        return keys;
      }
      const glm::vec3 center = glm::vec3((bb.minCorner + bb.maxCorner) * 0.5);
      Z3DCameraParameter cam0("Camera");
      cam0.setValueSameAs(base);
      cam0.setCenter(center);
      if (request.dolly.startDist > 0.0) {
        cam0.dollyToCenterDistance(static_cast<float>(request.dolly.startDist));
      }
      Z3DCameraParameter cam1("Camera");
      cam1.setValueSameAs(cam0);
      if (request.dolly.endDist > 0.0) {
        cam1.dollyToCenterDistance(static_cast<float>(request.dolly.endDist));
      }
      keys.push_back(Z3DCameraPlannerSolveKey{request.t0, cam0.jsonValue()});
      keys.push_back(Z3DCameraPlannerSolveKey{request.t1, cam1.jsonValue()});
      return keys;
    }
  }

  error = "unsupported solve mode";
  return {};
}

std::vector<Z3DCameraPlannerSolveKey> Z3DCameraPlanner::solvePath(const Z3DCameraParameter& base,
                                                                  const std::vector<Z3DCameraPlannerPathWaypoint>& waypoints,
                                                                  const std::optional<ZBBox<glm::dvec3>>& bboxOrNull,
                                                                  std::string& error)
{
  error.clear();
  if (waypoints.empty()) {
    error = "waypoints must be non-empty";
    return {};
  }

  std::vector<Z3DCameraPlannerPathWaypoint> wps = waypoints;
  std::stable_sort(wps.begin(), wps.end(), [](const auto& a, const auto& b) { return a.time < b.time; });

  std::vector<Z3DCameraPlannerSolveKey> keys;
  keys.reserve(wps.size());

  Z3DCameraParameter prev("Camera");
  prev.setValueSameAs(base);
  bool havePrev = false;

  for (const auto& w : wps) {
    const double t = w.time;
    if (!(t >= 0.0) || !std::isfinite(t)) {
      error = "waypoint time must be finite and >= 0";
      return {};
    }

    glm::vec3 eye = havePrev ? prev.get().eye() : base.get().eye();
    if (w.eyeMode == Z3DCameraPlannerPathWaypoint::EyeMode::World) {
      eye = w.eyeWorld;
    } else if (w.eyeMode == Z3DCameraPlannerPathWaypoint::EyeMode::BBoxFraction) {
      if (!bboxOrNull.has_value() || bboxOrNull->empty()) {
        error = "bbox required for bbox_fraction_eye";
        return {};
      }
      eye = bboxFractionToWorld(*bboxOrNull, w.eyeBBoxFraction);
    }

    glm::vec3 center;
    if (w.lookAtMode == Z3DCameraPlannerPathWaypoint::LookAtMode::World) {
      center = w.lookAtWorld;
    } else if (w.lookAtMode == Z3DCameraPlannerPathWaypoint::LookAtMode::BBoxCenter) {
      if (!bboxOrNull.has_value() || bboxOrNull->empty()) {
        error = "bbox required for look_at_bbox_center";
        return {};
      }
      const glm::dvec3 c = (bboxOrNull->minCorner + bboxOrNull->maxCorner) * 0.5;
      center = glm::vec3(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z));
    } else if (w.lookAtMode == Z3DCameraPlannerPathWaypoint::LookAtMode::BBoxFraction) {
      if (!bboxOrNull.has_value() || bboxOrNull->empty()) {
        error = "bbox required for bbox_fraction_look_at";
        return {};
      }
      center = bboxFractionToWorld(*bboxOrNull, w.lookAtBBoxFraction);
    } else {
      // Default: keep previous view direction and center distance (first key uses base).
      const glm::vec3 view = havePrev ? prev.get().viewVector() : base.get().viewVector();
      const float dist = havePrev ? prev.get().centerDist() : base.get().centerDist();
      center = eye + view * dist;
    }

    const glm::vec3 up = havePrev ? prev.get().upVector() : base.get().upVector();

    Z3DCameraParameter cam("Camera");
    cam.setValueSameAs(base);
    cam.setCamera(eye, center, up);
    keys.push_back(Z3DCameraPlannerSolveKey{t, cam.jsonValue()});

    prev.setValueSameAs(cam);
    havePrev = true;
  }

  return keys;
}

std::vector<Z3DCameraPlannerValidateResult> Z3DCameraPlanner::validate(const Z3DCameraParameter& base,
                                                                       const ZBBox<glm::dvec3>& bbox,
                                                                       const std::vector<double>& times,
                                                                       const std::vector<json::value>& values,
                                                                       const ValidateConstraints& constraints,
                                                                       const ValidatePolicies& policies,
                                                                       std::string& error)
{
  error.clear();

  ZBBox<glm::dvec3> bb = bbox;
  if (!bb.empty() && constraints.margin > 0.0) {
    bb = expandedByMarginFraction(bb, constraints.margin);
  }
  const double R = bboxEnclosingSphereRadius(bb);

  const bool keepVisible = constraints.keepVisible;
  const double minCov = keepVisible ? (constraints.minCoverage > 0.0 ? constraints.minCoverage : 0.95) : 0.0;

  std::vector<Z3DCameraPlannerValidateResult> results;
  const int n = static_cast<int>(std::min(times.size(), values.size()));
  results.reserve(static_cast<size_t>(n));

  for (int i = 0; i < n; ++i) {
    const double t = times[static_cast<size_t>(i)];
    const json::value& jv = values[static_cast<size_t>(i)];

    Z3DCameraPlannerValidateResult r;
    r.time = t;

    if (!jv.is_object()) {
      r.withinFrame = false;
      r.coverage = 0.0;
      r.adjusted = false;
      r.reason = "invalid_value";
      results.push_back(std::move(r));
      continue;
    }

    Z3DCameraParameter cam("Camera");
    cam.setValueSameAs(base);
    try {
      cam.readValue(jv);
    }
    catch (...) {
      r.withinFrame = false;
      r.coverage = 0.0;
      r.adjusted = false;
      r.reason = "invalid_value";
      results.push_back(std::move(r));
      continue;
    }

    // Coverage heuristic.
    const double required = (R > 0.0) ? requiredCenterDistanceForCoverage(cam.get(), R) : 0.0;
    const double current = static_cast<double>(cam.get().centerDist());
    double cov = 1.0;
    if (required > 1e-9) {
      cov = std::min(1.0, current / required);
    }
    bool ok = (cov + 1e-6) >= minCov;
    r.withinFrame = ok;
    r.coverage = cov;

    // Adjustment policy: keep withinFrame/coverage for the original input camera
    // (suggested adjustment is surfaced separately).
    bool adjusted = false;
    std::optional<json::value> adjustedValue;
    if (!ok && R > 0.0) {
      if (policies.adjustDistance && required > 0.0) {
        setCameraDistance(cam, required);
        adjusted = true;
        adjustedValue = cam.jsonValue();
        // Recompute ok based on suggested adjustment (used for reason semantics).
        const double cur2 = static_cast<double>(cam.get().centerDist());
        const double cov2 = (required > 0.0) ? std::min(1.0, cur2 / required) : 1.0;
        ok = (cov2 + 1e-6) >= minCov;
      } else if (policies.adjustFov && current > 1e-9) {
        // Solve desired FOV to achieve coverage with current distance.
        const double angleUsed = 2.0 * std::asin(std::min(1.0, R / current));
        double desiredFov = angleUsed;
        if (cam.get().aspectRatio() < 1.0f) {
          // angleUsed is horizontal; convert back to vertical FOV.
          desiredFov = 2.0 * std::atan(std::tan(angleUsed * 0.5) / cam.get().aspectRatio());
        }
        Z3DCameraParameter cam2("Camera");
        cam2.setValueSameAs(cam);
        cam2.setFrustum(static_cast<float>(desiredFov),
                        cam.get().aspectRatio(),
                        cam.get().nearDist(),
                        cam.get().farDist());
        adjusted = true;
        adjustedValue = cam2.jsonValue();
        // Recompute ok (used for reason semantics).
        const double req2 = requiredCenterDistanceForCoverage(cam2.get(), R);
        const double cur2 = static_cast<double>(cam2.get().centerDist());
        const double cov2 = (req2 > 1e-9) ? std::min(1.0, cur2 / req2) : 1.0;
        ok = (cov2 + 1e-6) >= minCov;
      }
    }

    r.adjusted = adjusted;
    r.adjustedValue = std::move(adjustedValue);

    if (!ok) {
      if (current < required) {
        r.reason = policies.adjustDistance ? "too_close"
                                           : (policies.adjustFov ? "fov_too_small" : "coverage_below_threshold");
      } else {
        r.reason = "coverage_below_threshold";
      }
    } else {
      r.reason = "";
    }

    results.push_back(std::move(r));
  }

  return results;
}

} // namespace nim

