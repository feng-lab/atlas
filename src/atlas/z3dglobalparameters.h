#ifndef Z3DGLOBALPARAMETERS_H
#define Z3DGLOBALPARAMETERS_H

#include "znumericparameter.h"
#include "z3dcameraparameter.h"
#include "zoptionparameter.h"
#include "z3dpickingmanager.h"
#include "z3dinteractionhandler.h"
#include <vector>

namespace nim {

class Z3DCanvas;
class ZWidgetsGroup;

class Z3DGlobalParameters : public QObject
{
  Q_OBJECT
public:
  Z3DGlobalParameters();
  ~Z3DGlobalParameters();
  const std::vector<ZParameter*>& parameters() const { return m_parameters; }
  void read(const QJsonObject &json);
  void write(QJsonObject &json) const;
  ZWidgetsGroup* widgetsGroup(bool includeCamera);

  // count is lightCount
  const glm::vec4* lightPositionArray() const { return &m_lightPositionArray[0]; }
  const glm::vec4* lightAmbientArray() const { return &m_lightAmbientArray[0]; }
  const glm::vec4* lightDiffuseArray() const { return &m_lightDiffuseArray[0]; }
  const glm::vec4* lightSpecularArray() const { return &m_lightSpecularArray[0]; }
  const glm::vec3* lightAttenuationArray() const { return &m_lightAttenuationArray[0]; }
  const float* lightSpotCutoffArray() const { return &m_lightSpotCutoffArray[0]; }
  const float* lightSpotExponentArray() const { return &m_lightSpotExponentArray[0]; }
  const glm::vec3* lightSpotDirectionArray() const { return &m_lightSpotDirectionArray[0]; }

  ZStringIntOptionParameter geometriesMultisampleMode;
  ZStringIntOptionParameter transparencyMethod;
  ZIntParameter lightCount;
  std::vector<ZVec4Parameter*> lightPositions;
  std::vector<ZVec4Parameter*> lightAmbients;
  std::vector<ZVec4Parameter*> lightDiffuses;
  std::vector<ZVec4Parameter*> lightSpeculars;
  // The light source's attenuation factors (x = constant, y = linear, z = quadratic)
  std::vector<ZVec3Parameter*> lightAttenuations;
  std::vector<ZFloatParameter*> lightSpotCutoff;
  std::vector<ZFloatParameter*> lightSpotExponent;
  std::vector<ZVec3Parameter*> lightSpotDirection;
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

  // must call
  void setCanvas(Z3DCanvas* canvas) { m_canvas = canvas; }
  void getGLFocus();

  // must call
  void setPickingTarget(Z3DRenderTarget& rt) { pickingManager.setRenderTarget(rt); }

private slots:
  void updateLightsArray();

private:
  void addParameter(ZParameter &para) { addParameter(&para); }
  void addParameter(ZParameter *para) { m_parameters.push_back(para); }

  std::vector<ZParameter*> m_parameters;

  ZWidgetsGroup* m_widgetsGrp;
  ZWidgetsGroup* m_widgetsGrpNoCamera;

  std::vector<glm::vec4> m_lightPositionArray;
  std::vector<glm::vec4> m_lightAmbientArray;
  std::vector<glm::vec4> m_lightDiffuseArray;
  std::vector<glm::vec4> m_lightSpecularArray;
  std::vector<glm::vec3> m_lightAttenuationArray;
  std::vector<float> m_lightSpotCutoffArray;
  std::vector<float> m_lightSpotExponentArray;
  std::vector<glm::vec3> m_lightSpotDirectionArray;

  Z3DCanvas* m_canvas;
};

} // namespace nim

#endif // Z3DGLOBALPARAMETERS_H
