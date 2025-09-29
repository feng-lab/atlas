#pragma once

#include "zbbox.h"
#include "znumericparameter.h"
#include "z3dcameraparameter.h"
#include "z3drenderglobalstate.h"
#include "z3dpickingmanager.h"
#include "z3dtypes.h"
#include "zoptionparameter.h"
#include "z3dinteractionhandler.h"
#include <vector>
#include <mutex>

namespace nim {

class ZWidgetsGroup;

class Z3DRenderingEngine;

struct Z3DLocalColorBuffer
{
  std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 64>> data;
  size_t width;
  size_t height;
};

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

  void markGlobalSceneStateDirty()
  {
    Z3DRenderGlobalState::instance().markSceneStateDirty();
  }

  void markGlobalViewStateDirty()
  {
    Z3DRenderGlobalState::instance().markViewStateDirty();
  }

  void addParameter(ZParameter& para)
  {
    m_parameters.push_back(&para);
  }

public:
  ZStringIntOptionParameter renderBackend;
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

private:
  std::vector<ZParameter*> m_parameters;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGrp;
  std::shared_ptr<ZWidgetsGroup> m_widgetsGrpNoCamera;

  size_t m_cameraParameterIndex = 0;
};

} // namespace nim
