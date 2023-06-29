#pragma once

#include "zbbox.h"
#include "znumericparameter.h"
#include "z3dcameraparameter.h"
#include "z3dpickingmanager.h"
#include "zoptionparameter.h"
#include "z3dinteractionhandler.h"
#include <folly/CancellationToken.h>
#include <vector>
#include <deque>
#include <mutex>

namespace nim {

class ZWidgetsGroup;

class Z3DRenderingEngine;

class Z3DGlobalParameters : public QObject
{
  Q_OBJECT

public:
  Z3DGlobalParameters();

  void setDevicePixelRatio(float f);

  [[nodiscard]] const std::vector<ZParameter*>& parameters() const
  {
    return m_parameters;
  }

  void read(const json::object& json);

  void write(json::object& json) const;

  std::shared_ptr<ZWidgetsGroup> widgetsGroup(bool includeCamera, Z3DRenderingEngine& engine);

  // count is lightCount
  [[nodiscard]] const glm::vec4* lightPositionArray() const
  {
    return m_lightPositionArray.data();
  }

  [[nodiscard]] const glm::vec4* lightAmbientArray() const
  {
    return m_lightAmbientArray.data();
  }

  [[nodiscard]] const glm::vec4* lightDiffuseArray() const
  {
    return m_lightDiffuseArray.data();
  }

  [[nodiscard]] const glm::vec4* lightSpecularArray() const
  {
    return m_lightSpecularArray.data();
  }

  [[nodiscard]] const glm::vec3* lightAttenuationArray() const
  {
    return m_lightAttenuationArray.data();
  }

  [[nodiscard]] const float* lightSpotCutoffArray() const
  {
    return m_lightSpotCutoffArray.data();
  }

  [[nodiscard]] const float* lightSpotExponentArray() const
  {
    return m_lightSpotExponentArray.data();
  }

  [[nodiscard]] const glm::vec3* lightSpotDirectionArray() const
  {
    return m_lightSpotDirectionArray.data();
  }

  // must call
  void setPickingTarget(Z3DRenderTarget& rt)
  {
    pickingManager.setRenderTarget(rt);
  }

  void cameraFocusesOn(double x, double y, double z, double radius = 64);

  void cameraFocusesOn(const ZBBox<glm::dvec3>& bound, double minRadius = 64);

  void cameraPointsTo(double x, double y, double z);

  void cameraPointsTo(const ZBBox<glm::dvec3>& bound);

private:
  void updateLightsArray();

  void addParameter(ZParameter& para)
  {
    m_parameters.push_back(&para);
  }

public:
  ZStringIntOptionParameter geometriesMultisampleMode;
  ZStringIntOptionParameter transparencyMethod;
  ZFloatParameter weightedBlendedDepthScale;
  ZIntParameter lightCount;
  std::vector<std::unique_ptr<ZVec4Parameter>> lightPositions;
  std::vector<std::unique_ptr<ZVec4Parameter>> lightAmbients;
  std::vector<std::unique_ptr<ZVec4Parameter>> lightDiffuses;
  std::vector<std::unique_ptr<ZVec4Parameter>> lightSpeculars;
  // The light source's attenuation factors (x = constant, y = linear, z = quadratic)
  std::vector<std::unique_ptr<ZVec3Parameter>> lightAttenuations;
  std::vector<std::unique_ptr<ZFloatParameter>> lightSpotCutoff;
  std::vector<std::unique_ptr<ZFloatParameter>> lightSpotExponent;
  std::vector<std::unique_ptr<ZVec3Parameter>> lightSpotDirection;
  ZVec4Parameter sceneAmbient;

  // fog
  ZStringIntOptionParameter fogMode;
  ZVec3Parameter fogTopColor;
  ZVec3Parameter fogBottomColor;
  ZIntSpanParameter fogRange;
  ZFloatParameter fogDensity;

  Z3DCameraParameter camera;
  Z3DPickingManager pickingManager;
  // must add to network
  Z3DTrackballInteractionHandler interactionHandler;

  ZFloatSpanParameter globalXCut;
  ZFloatSpanParameter globalYCut;
  ZFloatSpanParameter globalZCut;

  ZFloatParameter devicePixelRatio;

  std::mutex targetSwitchMutex;
  std::atomic_bool hasNewRendering = false;
  std::unique_ptr<folly::CancellationSource> cancellationSource;

private:
  std::vector<ZParameter*> m_parameters;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGrp;
  std::shared_ptr<ZWidgetsGroup> m_widgetsGrpNoCamera;

  std::vector<glm::vec4> m_lightPositionArray;
  std::vector<glm::vec4> m_lightAmbientArray;
  std::vector<glm::vec4> m_lightDiffuseArray;
  std::vector<glm::vec4> m_lightSpecularArray;
  std::vector<glm::vec3> m_lightAttenuationArray;
  std::vector<float> m_lightSpotCutoffArray;
  std::vector<float> m_lightSpotExponentArray;
  std::vector<glm::vec3> m_lightSpotDirectionArray;

  size_t m_cameraParameterIndex = 0;
};

} // namespace nim
