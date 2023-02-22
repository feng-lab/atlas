#pragma once

#include "z3dconerenderer.h"

namespace nim {

class Z3DArrowRenderer : public Z3DConeRenderer
{
public:
  explicit Z3DArrowRenderer(Z3DRendererBase& rendererBase);

  // head length is in proportion to whole length
  void setArrowData(std::vector<glm::vec4>* tailPosAndTailRadius,
                    std::vector<glm::vec4>* headPosAndHeadRadius,
                    float headLengthProportion = 0.1);

  // head length is fixed
  void setFixedHeadLengthArrowData(std::vector<glm::vec4>* tailPosAndTailRadius,
                                   std::vector<glm::vec4>* headPosAndHeadRadius,
                                   float fixedHeadLength);

  void setArrowColors(std::vector<glm::vec4>* arrowColors);

  void setArrowColors(std::vector<glm::vec4>* arrowTailColors, std::vector<glm::vec4>* arrowHeadColors);

  void setArrowPickingColors(std::vector<glm::vec4>* arrowPickingColors = nullptr);

private:
  std::vector<glm::vec4> m_arrowConeBaseAndBaseRadius;
  std::vector<glm::vec4> m_arrowConeAxisAndTopRadius;
  std::vector<glm::vec4> m_arrowConeColors;
  std::vector<glm::vec4> m_arrowConePickingColors;
};

} // namespace nim
