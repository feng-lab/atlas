#include "z3dcameraplanner.h"

#include "z3dcamera.h"
#include "z3dcameraparameter.h"

#include <array>
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

  const bool keepVisible = constraints.keepVisible;
  const double minCov = keepVisible ? std::max(0.0, constraints.minFrameCoverage) : 0.0;

  struct FrameMetrics
  {
    bool withinFrame = true;
    double frameCoverage = 0.0;
  };

  const auto measureFrame = [&](const Z3DCamera& cam) -> FrameMetrics {
    FrameMetrics m;
    if (bb.empty()) {
      return m;
    }

    const glm::dvec3 mn = bb.minCorner;
    const glm::dvec3 mx = bb.maxCorner;
    const std::array<glm::vec3, 8> corners = {
      glm::vec3(static_cast<float>(mn.x), static_cast<float>(mn.y), static_cast<float>(mn.z)),
      glm::vec3(static_cast<float>(mx.x), static_cast<float>(mn.y), static_cast<float>(mn.z)),
      glm::vec3(static_cast<float>(mn.x), static_cast<float>(mx.y), static_cast<float>(mn.z)),
      glm::vec3(static_cast<float>(mx.x), static_cast<float>(mx.y), static_cast<float>(mn.z)),
      glm::vec3(static_cast<float>(mn.x), static_cast<float>(mn.y), static_cast<float>(mx.z)),
      glm::vec3(static_cast<float>(mx.x), static_cast<float>(mn.y), static_cast<float>(mx.z)),
      glm::vec3(static_cast<float>(mn.x), static_cast<float>(mx.y), static_cast<float>(mx.z)),
      glm::vec3(static_cast<float>(mx.x), static_cast<float>(mx.y), static_cast<float>(mx.z)),
    };

    const glm::mat4 pv = cam.projectionViewMatrix(MonoEye);
    double minX = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();

    bool within = true;
    for (const auto& p : corners) {
      const glm::vec4 clip = pv * glm::vec4(p, 1.f);
      if (!std::isfinite(clip.w) || clip.w <= 1e-6f) {
        // Treat anything behind the camera (or numerically unstable) as not within frame.
        m.withinFrame = false;
        m.frameCoverage = 0.0;
        return m;
      }
      const glm::vec3 ndc = clip.xyz() / clip.w;
      if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(ndc.z)) {
        m.withinFrame = false;
        m.frameCoverage = 0.0;
        return m;
      }
      minX = std::min(minX, static_cast<double>(ndc.x));
      maxX = std::max(maxX, static_cast<double>(ndc.x));
      minY = std::min(minY, static_cast<double>(ndc.y));
      maxY = std::max(maxY, static_cast<double>(ndc.y));

      if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f) {
        within = false;
      }
    }

    const auto clamp01 = [](double v) {
      return std::max(0.0, std::min(1.0, v));
    };
    const double widthFrac = clamp01((maxX - minX) * 0.5); // NDC spans [-1,1] => width 2
    const double heightFrac = clamp01((maxY - minY) * 0.5); // NDC spans [-1,1] => height 2
    m.withinFrame = within;
    // Dominant-dimension fill: bigger of width/height fraction.
    m.frameCoverage = clamp01(std::max(widthFrac, heightFrac));
    return m;
  };

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
      r.frameCoverage = 0.0;
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
      r.frameCoverage = 0.0;
      r.adjusted = false;
      r.reason = "invalid_value";
      results.push_back(std::move(r));
      continue;
    }

    const FrameMetrics metrics = measureFrame(cam.get());
    r.withinFrame = metrics.withinFrame;
    r.frameCoverage = metrics.frameCoverage;

    bool ok = true;
    if (keepVisible) {
      if (!metrics.withinFrame) {
        ok = false;
      } else if (minCov > 0.0 && (metrics.frameCoverage + 1e-6) < minCov) {
        ok = false;
      }
    }

    // Adjustment policy: keep withinFrame/frameCoverage for the original input camera
    // (suggested adjustment is surfaced separately).
    bool adjusted = false;
    std::optional<json::value> adjustedValue;
    if (policies.adjustDistance && keepVisible && !ok && !bb.empty()) {
      const Z3DCamera baseCam = cam.get();
      const double currentDist = static_cast<double>(baseCam.centerDist());
      if (std::isfinite(currentDist) && currentDist > 0.0) {
        const auto cameraAtDistance = [&](double centerDist) -> Z3DCamera {
          Z3DCamera out = baseCam;
          const glm::vec3 c = out.center();
          const glm::vec3 v = out.viewVector();
          const glm::vec3 eye = c - static_cast<float>(centerDist) * v;
          out.setCamera(eye, c, out.upVector());
          return out;
        };

        const auto metricsAtDistance = [&](double centerDist) -> FrameMetrics {
          return measureFrame(cameraAtDistance(centerDist));
        };

        constexpr double kMinCenterDist = 1e-6;
        const auto clampCenterDist = [&](double d) {
          return std::max(kMinCenterDist, d);
        };

        // Start from either the current distance (when just outside frame) or a
        // dolly-in estimate (when the subject is too small).
        double candidate = currentDist;
        if (metrics.withinFrame && minCov > 0.0 && metrics.frameCoverage > 1e-9 &&
            (metrics.frameCoverage + 1e-6) < minCov) {
          candidate = clampCenterDist(currentDist * (metrics.frameCoverage / minCov));
        }

        // Find the smallest distance >= candidate that keeps the bbox fully within frame.
        double lo = clampCenterDist(candidate);
        FrameMetrics loMetrics = metricsAtDistance(lo);
        if (!loMetrics.withinFrame) {
          double hi = lo;
          FrameMetrics hiMetrics = loMetrics;
          bool found = false;
          for (int iter = 0; iter < 32; ++iter) {
            hi = clampCenterDist(hi * 1.25);
            hiMetrics = metricsAtDistance(hi);
            if (hiMetrics.withinFrame) {
              found = true;
              break;
            }
          }
          if (found) {
            // Binary search for the boundary (minimal within-frame distance).
            for (int iter = 0; iter < 40; ++iter) {
              const double mid = 0.5 * (lo + hi);
              if (metricsAtDistance(mid).withinFrame) {
                hi = mid;
              } else {
                lo = mid;
              }
            }
            Z3DCameraParameter camAdj("Camera");
            camAdj.setSameAs(cam);
            setCameraDistance(camAdj, hi);
            adjusted = true;
            adjustedValue = camAdj.jsonValue();
          }
        } else {
          // Already within frame at candidate; suggest the candidate dolly when it changes the camera.
          if (std::abs(lo - currentDist) > 1e-9) {
            Z3DCameraParameter camAdj("Camera");
            camAdj.setSameAs(cam);
            setCameraDistance(camAdj, lo);
            adjusted = true;
            adjustedValue = camAdj.jsonValue();
          }
        }
      }
    }

    r.adjusted = adjusted;
    r.adjustedValue = std::move(adjustedValue);

    if (!ok) {
      r.reason = metrics.withinFrame ? "coverage_below_threshold" : "outside_frame";
    } else {
      r.reason = "";
    }

    results.push_back(std::move(r));
  }

  return results;
}

} // namespace nim
