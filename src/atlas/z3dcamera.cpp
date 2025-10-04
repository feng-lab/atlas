#include "z3dcamera.h"

#include "zlog.h"
#include <cmath>

namespace nim {

Z3DCamera::Z3DCamera(Z3DCoordinateSystem coordinateSystem)
    : m_coordinateSystem(coordinateSystem)
{
  updateCamera();
  updateFrustum();
}

void Z3DCamera::setCoordinateSystem(Z3DCoordinateSystem system)
{
  if (m_coordinateSystem != system) {
    m_coordinateSystem = system;
    // Recalculate projection matrices for the new coordinate system
    makeProjectionMatrices();
  }
}

void Z3DCamera::setCamera(const glm::vec3& eye, const glm::vec3& center, const glm::vec3& upVector)
{
  m_eye = eye;
  m_center = center;
  m_upVector = glm::normalize(upVector);
  updateCamera();
}

void Z3DCamera::setFrustum(float fov, float ratio, float nearDist, float farDist)
{
  m_fieldOfView = fov;
  m_aspectRatio = ratio;
  m_nearDist = nearDist;
  m_farDist = farDist;
  updateFrustum();
}

void Z3DCamera::setTileFrustum(double normalizedLeft,
                               double normalizedRight,
                               double normalizedBottom,
                               double normalizedTop)
{
  float halfheight = std::tan(0.5f * m_fieldOfView) * m_nearDist;
  float halfwidth = halfheight * m_aspectRatio * m_windowAspectRatio;

  m_normalizedLeft = normalizedLeft;
  m_normalizedRight = normalizedRight;
  m_normalizedBottom = normalizedBottom;
  m_normalizedTop = normalizedTop;

  m_left = -halfwidth + 2.f * halfwidth * m_normalizedLeft;
  m_right = -halfwidth + 2.f * halfwidth * m_normalizedRight;
  m_bottom = -halfheight + 2.f * halfheight * m_normalizedBottom;
  m_top = -halfheight + 2.f * halfheight * m_normalizedTop;

  // VLOG(1) << m_left << m_right << m_bottom << m_top << halfheight << halfwidth;

  makeProjectionMatrices();
}

void Z3DCamera::resetCamera(const ZBBox<glm::dvec3>& bound, ResetOption options)
{
  glm::vec3 center = glm::vec3((bound.minCorner + bound.maxCorner) / 2.0);

  if (!isFlagSet(options, ResetOption::PreserveCenterDistance)) {
    auto boundSize = bound.size();
    float w1 = boundSize.x;
    float w2 = boundSize.y;
    float w3 = boundSize.z;
    w1 *= w1;
    w2 *= w2;
    w3 *= w3;
    float radius = w1 + w2 + w3;
    radius = (radius == 0) ? (1.0f) : (radius);

    // compute the radius of the enclosing sphere
    // radius = std::sqrt(radius)*0.5 + m_eyeSeparation/2.f;
    radius = std::sqrt(radius) * 0.5f;

    // (from VTK) compute the distance from the intersection of the view frustum with the
    // bounding sphere. Basically in 2D draw a circle representing the bounding
    // sphere in 2D then draw a horizontal line going out from the center of
    // the circle. That is the camera view. Then draw a line from the camera
    // position to the point where it intersects the circle. (it will be tangent
    // to the circle at this point, this is important, only go to the tangent
    // point, do not draw all the way to the view plane). Then draw the radius
    // from the tangent point to the center of the circle. You will note that
    // this forms a right triangle with one side being the radius, another being
    // the target distance for the camera, then just find the target dist using
    // a sin.
    double angle = m_fieldOfView;
    if (m_aspectRatio < 1.0) { // use horizontal angle to calculate
      angle = 2.0 * std::atan(std::tan(angle * 0.5) * m_aspectRatio);
    }

    m_centerDist = radius / std::sin(angle * 0.5);
  }
  if (!isFlagSet(options, ResetOption::PreserveViewVector)) {
    m_viewVector = glm::vec3(0.f, 0.f, 1.f);
    m_upVector = glm::vec3(0.f, -1.f, 0.f);
  }
  glm::vec3 eye = center - m_centerDist * m_viewVector;
  setCamera(eye, center, m_upVector);

  resetCameraNearFarPlane(bound);
}

void Z3DCamera::resetCamera(double xmin,
                            double xmax,
                            double ymin,
                            double ymax,
                            double zmin,
                            double zmax,
                            ResetOption options)
{
  resetCamera(ZBBox<glm::dvec3>(glm::dvec3(xmin, ymin, zmin), glm::dvec3(xmax, ymax, zmax)), options);
}

void Z3DCamera::resetCameraNearFarPlane(const ZBBox<glm::dvec3>& bound)
{
  double a = m_viewVector[0];
  double b = m_viewVector[1];
  double c = m_viewVector[2];
  double d = -(a * m_eye[0] + b * m_eye[1] + c * m_eye[2]);

  double bd[6];
  bd[0] = bound.minCorner.x;
  bd[1] = bound.maxCorner.x;
  bd[2] = bound.minCorner.y;
  bd[3] = bound.maxCorner.y;
  bd[4] = bound.minCorner.z;
  bd[5] = bound.maxCorner.z;

  // Set the max near clipping plane and the min far clipping plane
  double range[2];
  range[0] = std::numeric_limits<double>::max();
  range[1] = 1e-18;

  // Find the closest / farthest bounding box vertex
  for (auto k = 0; k < 2; ++k) {
    for (auto j = 0; j < 2; ++j) {
      for (auto i = 0; i < 2; ++i) {
        double dist = a * bd[i] + b * bd[2 + j] + c * bd[4 + k] + d;
        range[0] = std::min(dist, range[0]);
        range[1] = std::max(dist, range[1]);
      }
    }
  }

  // Do not let the range behind the camera throw off the calculation.
  if (range[0] < 0.0) {
    range[0] = 0.0;
  }

  // Give ourselves a little breathing room
  range[0] = 0.99 * range[0]; // - (range[1] - range[0])*0.5;
  range[1] = 1.01 * range[1]; // + (range[1] - range[0])*0.5;

  // Make sure near is not bigger than far
  // range[0] = (range[0] >= range[1])?(0.01*range[1]):(range[0]);

  // Make sure near is at least some fraction of far - this prevents near
  // from being behind the camera or too close in front.
  double nearClippingPlaneTolerance = 0.001;

  // make sure the front clipping range is not too far from the far clippnig
  // range, this is to make sure that the zbuffer resolution is effectively
  // used
  if (range[0] < nearClippingPlaneTolerance * range[1]) {
    range[0] = nearClippingPlaneTolerance * range[1];
  }

  constexpr double kNearFarEpsilon = 1e-6;
  if (std::abs(range[0] - static_cast<double>(m_nearDist)) <= kNearFarEpsilon &&
      std::abs(range[1] - static_cast<double>(m_farDist)) <= kNearFarEpsilon) {
    return;
  }

  m_nearDist = static_cast<float>(range[0]);
  m_farDist = static_cast<float>(range[1]);
  updateFrustum();
}

void Z3DCamera::resetCameraNearFarPlane(double xmin, double xmax, double ymin, double ymax, double zmin, double zmax)
{
  resetCameraNearFarPlane(ZBBox<glm::dvec3>(glm::dvec3(xmin, ymin, zmin), glm::dvec3(xmax, ymax, zmax)));
}

bool Z3DCamera::operator==(const Z3DCamera& rhs) const
{
  return (m_eye == rhs.m_eye) && (m_center == rhs.m_center) && (m_upVector == rhs.m_upVector) &&
         (m_projectionType == rhs.m_projectionType) && (m_fieldOfView == rhs.m_fieldOfView) &&
         (m_aspectRatio == rhs.m_aspectRatio) && (m_nearDist == rhs.m_nearDist) && (m_farDist == rhs.m_farDist) &&
         (m_windowAspectRatio == rhs.m_windowAspectRatio) && (m_eyeSeparationAngle == rhs.m_eyeSeparationAngle);
}

void Z3DCamera::dolly(float value)
{
  if (value <= 0.f || (m_centerDist < 0.01f && value > 1.f)) {
    return;
  }
  glm::vec3 pos = m_center - m_viewVector * (m_centerDist / value);
  float maxV = 1e15;
  if (std::abs(pos.x) < maxV && std::abs(pos.y) < maxV && std::abs(pos.z) < maxV) {
    setEye(pos);
  }
}

void Z3DCamera::dollyToCenterDistance(float centerDist)
{
  centerDist = std::max(0.01f, std::min(m_centerDist * 100.f, centerDist));
  glm::vec3 pos = m_center - m_viewVector * centerDist;
  float maxV = 1e15;
  if (std::abs(pos.x) < maxV && std::abs(pos.y) < maxV && std::abs(pos.z) < maxV) {
    setEye(pos);
  }
}

void Z3DCamera::roll(float angle)
{
  glm::vec3 up = glm::rotate(glm::angleAxis(angle, m_viewVector), m_upVector);
  setUpVector(up);
}

void Z3DCamera::azimuth(float angle)
{
  glm::vec3 eye = m_eye - m_center;
  eye = glm::rotate(glm::angleAxis(angle, m_upVector), eye);
  eye += m_center;
  setEye(eye);
}

void Z3DCamera::yaw(float angle)
{
  glm::vec3 center = m_center - m_eye;
  center = glm::rotate(glm::angleAxis(angle, m_upVector), center);
  center += m_eye;
  setCenter(center);
}

void Z3DCamera::elevation(float angle)
{
  rotate(angle, -m_strafeVector);
}

void Z3DCamera::pitch(float angle)
{
  rotate(angle, m_strafeVector, m_eye);
}

void Z3DCamera::zoom(float factor)
{
  if (factor <= 0.f) {
    return;
  }
  setFieldOfView(m_fieldOfView / factor);
}

void Z3DCamera::rotate(float angle, const glm::vec3& axis, const glm::vec3& point)
{
  rotate(glm::angleAxis(angle, glm::normalize(axis)), point);
}

void Z3DCamera::rotate(const glm::quat& quat, const glm::vec3& point)
{
  glm::vec3 eye = m_eye - point;
  eye = glm::rotate(quat, eye);
  eye += point;

  glm::vec3 center = m_center - point;
  center = glm::rotate(quat, center);
  center += point;

  glm::vec3 upVector = glm::rotate(quat, m_upVector);

  setCamera(eye, center, upVector);
}

void Z3DCamera::rotate(float angle, const glm::vec3& axis)
{
  rotate(glm::angleAxis(angle, glm::normalize(axis)));
}

void Z3DCamera::rotate(const glm::quat& quat)
{
  glm::vec3 eye = m_eye - m_center;
  eye = glm::rotate(quat, eye);
  eye += m_center;

  glm::vec3 upVector = glm::rotate(quat, m_upVector);

  setCamera(eye, m_center, upVector);
}

glm::vec3 Z3DCamera::vectorEyeToWorld(const glm::vec3& vec, Z3DEye eye) const
{
  return glm::inverse(glm::mat3(viewMatrix(eye))) * vec;
}

glm::vec3 Z3DCamera::vectorWorldToEye(const glm::vec3& vec, Z3DEye eye) const
{
  return glm::mat3(viewMatrix(eye)) * vec;
}

glm::vec3 Z3DCamera::pointEyeToWorld(const glm::vec3& pt, Z3DEye eye) const
{
  return glm::applyMatrix(glm::inverse(viewMatrix(eye)), pt);
}

glm::vec3 Z3DCamera::pointWorldToEye(const glm::vec3& pt, Z3DEye eye) const
{
  return glm::applyMatrix(viewMatrix(eye), pt);
}

glm::vec3 Z3DCamera::worldToScreen(const glm::vec3& wpt, const glm::ivec4& viewport, Z3DEye eye) const
{
  glm::vec4 clipSpacePos = projectionMatrix(eye) * viewMatrix(eye) * glm::vec4(wpt, 1.f);
  if (clipSpacePos.w == 0.f) {
    return {-1.f, -1.f, -1.f};
  }
  glm::vec3 ndcSpacePos = glm::vec3(clipSpacePos.xyz()) / clipSpacePos.w;
  return ((ndcSpacePos + 1.f) / 2.f) * glm::vec3(viewport.z, viewport.w, 1.f) + glm::vec3(viewport.x, viewport.y, 0.f);
}

glm::vec3 Z3DCamera::screenToWorld(const glm::vec3& spt, const glm::ivec4& viewport, Z3DEye eye) const
{
  return glm::unProject(spt, viewMatrix(eye), projectionMatrix(eye), viewport);
}

void Z3DCamera::updateCamera()
{
  m_viewVector = glm::normalize(m_center - m_eye);
  m_centerDist = glm::length(m_center - m_eye);
  // make sure upVector is not parallel to viewVector
  if (std::abs(glm::dot(m_upVector, m_viewVector)) >= 0.9) {
    LOG(WARNING) << "Resetting view up since view plane normal is parallel";
    LOG(INFO) << m_upVector << " " << m_viewVector;
    m_upVector = glm::cross(m_viewVector, glm::vec3(1.f, 0.f, 0.f));
    if (glm::dot(m_upVector, m_upVector) < 0.001) {
      m_upVector = glm::cross(m_viewVector, glm::vec3(0.f, 1.f, 0.f));
    }
    m_upVector = glm::normalize(m_upVector);
  }
  m_strafeVector = glm::cross(m_viewVector, m_upVector);
  m_eyeSeparation = 2.f * m_farDist * std::tan(m_eyeSeparationAngle / 2.f);
  m_focusDistance = std::min((m_farDist - m_nearDist) * 0.75f + m_nearDist, m_nearDist * 2.f);
  m_eyeSeparation = m_focusDistance / 30.f;

  makeViewMatrices();
}

void Z3DCamera::updateFrustum()
{
  float halfheight = std::tan(0.5f * m_fieldOfView) * m_nearDist;
  float halfwidth = halfheight * m_aspectRatio * m_windowAspectRatio;
  //  m_top = halfheight;
  //  m_bottom = -halfheight;
  //  m_left = -halfwidth;
  //  m_right = halfwidth;

  // VLOG(1) << halfwidth << " " << halfheight << " " << m_aspectRatio << " " << m_windowAspectRatio;

  m_left = -halfwidth + 2.f * halfwidth * m_normalizedLeft;
  m_right = -halfwidth + 2.f * halfwidth * m_normalizedRight;
  m_bottom = -halfheight + 2.f * halfheight * m_normalizedBottom;
  m_top = -halfheight + 2.f * halfheight * m_normalizedTop;

  makeProjectionMatrices();
}

void Z3DCamera::makeViewMatrices()
{
  glm::vec3 adjust = m_strafeVector * -m_eyeSeparation / 2.f;
  m_viewMatrices[LeftEye] = glm::lookAt(m_eye + adjust, m_center + adjust, m_upVector);
  m_viewMatrices[MonoEye] = glm::lookAt(m_eye, m_center, m_upVector);
  adjust = m_strafeVector * m_eyeSeparation / 2.f;
  m_viewMatrices[RightEye] = glm::lookAt(m_eye + adjust, m_center + adjust, m_upVector);

  m_inverseViewMatrices[LeftEye] = glm::inverse(m_viewMatrices[LeftEye]);
  m_inverseViewMatrices[MonoEye] = glm::inverse(m_viewMatrices[MonoEye]);
  m_inverseViewMatrices[RightEye] = glm::inverse(m_viewMatrices[RightEye]);

  m_projectionViewMatrices[LeftEye] = m_projectionMatrices[LeftEye] * m_viewMatrices[LeftEye];
  m_projectionViewMatrices[MonoEye] = m_projectionMatrices[MonoEye] * m_viewMatrices[MonoEye];
  m_projectionViewMatrices[RightEye] = m_projectionMatrices[RightEye] * m_viewMatrices[RightEye];

  m_normalMatrices[LeftEye] = glm::transpose(glm::inverse(glm::mat3(m_viewMatrices[LeftEye])));
  m_normalMatrices[MonoEye] = glm::transpose(glm::inverse(glm::mat3(m_viewMatrices[MonoEye])));
  m_normalMatrices[RightEye] = glm::transpose(glm::inverse(glm::mat3(m_viewMatrices[RightEye])));
}

void Z3DCamera::makeProjectionMatrices()
{
  if (m_projectionType == ProjectionType::Orthographic) {
    glm::mat4 pmat;
    if (m_coordinateSystem == Z3DCoordinateSystem::Vulkan) {
      // Vulkan-style depth in [0,1]
      pmat = glm::orthoRH_ZO(m_left, m_right, m_bottom, m_top, m_nearDist, m_farDist);
    } else {
      pmat = glm::ortho(m_left, m_right, m_bottom, m_top, m_nearDist, m_farDist);
    }
    m_projectionMatrices[LeftEye] = pmat;
    m_projectionMatrices[MonoEye] = pmat;
    m_projectionMatrices[RightEye] = pmat;
    pmat = glm::inverse(pmat);
    m_inverseProjectionMatrices[LeftEye] = pmat;
    m_inverseProjectionMatrices[MonoEye] = pmat;
    m_inverseProjectionMatrices[RightEye] = pmat;
  } else {
    // Create perspective projection matrices
    auto createFrustum = [this](float left, float right, float bottom, float top, float near, float far) -> glm::mat4 {
      if (m_coordinateSystem == Z3DCoordinateSystem::Vulkan) {
        return glm::frustumRH_ZO(left, right, bottom, top, near, far);
      }
      return glm::frustum(left, right, bottom, top, near, far);
    };
    
    // VLOG(1) << fmt::format("{}, {}, {}, {}", m_left, m_right, m_bottom, m_top);
    m_projectionMatrices[MonoEye] = createFrustum(m_left, m_right, m_bottom, m_top, m_nearDist, m_farDist);
    
    float frustumShift = 0.5f * m_eyeSeparation * m_nearDist / m_focusDistance;
    m_projectionMatrices[LeftEye] = 
      createFrustum(m_left + frustumShift, m_right + frustumShift, m_bottom, m_top, m_nearDist, m_farDist);
      
    m_projectionMatrices[RightEye] = 
      createFrustum(m_left - frustumShift, m_right - frustumShift, m_bottom, m_top, m_nearDist, m_farDist);

    m_inverseProjectionMatrices[LeftEye] = glm::inverse(m_projectionMatrices[LeftEye]);
    m_inverseProjectionMatrices[MonoEye] = glm::inverse(m_projectionMatrices[MonoEye]);
    m_inverseProjectionMatrices[RightEye] = glm::inverse(m_projectionMatrices[RightEye]);
  }

  m_projectionViewMatrices[LeftEye] = m_projectionMatrices[LeftEye] * m_viewMatrices[LeftEye];
  m_projectionViewMatrices[MonoEye] = m_projectionMatrices[MonoEye] * m_viewMatrices[MonoEye];
  m_projectionViewMatrices[RightEye] = m_projectionMatrices[RightEye] * m_viewMatrices[RightEye];
}

} // namespace nim
