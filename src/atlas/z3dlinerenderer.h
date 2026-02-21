#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dgpuinfo.h"
#include "z3drendercommands.h"
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nim {

class Z3DLineRenderer : public Z3DPrimitiveRenderer
{
public:
  // default use display list but not lighing in opengl mode
  explicit Z3DLineRenderer(Z3DRendererBase& rendererBase);

  // default false, must call before setData and setDataColor and setDataPickingColors
  void setLineStrip(bool v)
  {
    m_isLineStrip = v;
  }

  void setData(std::span<const glm::vec3> lines);
  void setData(std::vector<glm::vec3> lines);

  void setLineWidth(const std::vector<float>& lineWidthArray)
  {
    m_lineWidthArray = lineWidthArray;
  }

  // use vertice color
  void setDataColors(std::span<const glm::vec4> lineColorsInput);
  void setDataColors(std::vector<glm::vec4> lineColorsInput);

  // use 1d texture color
  void setTexture(Z3DTexture* tex);

  void setDataPickingColors(std::span<const glm::vec4> linePickingColorsInput);
  void setDataPickingColors(std::vector<glm::vec4> linePickingColorsInput);
  void clearPickingColors();

  void buildWideLineGeometry(std::vector<LineWideVertex>& outVertices, std::vector<uint32_t>& outIndices) const;

  // default true since glLineWidth only support 1 pixel width line from now on
  void setUseSmoothLine(bool v)
  {
    m_useSmoothLine = v;
  }

  void setLineWidth(float v)
  {
    m_srcLineWidth = std::max(1.f, v);
    updateLineWidth();
  }

  // When the global geometry anti-aliasing mode is supersampling (2x2), this
  // flag controls whether line width is compensated to preserve apparent
  // thickness after the resolve/downsample step.
  void setFollowSupersampling(bool v)
  {
    m_followSupersampling = v;
    updateLineWidth();
  }

  // default true, will disable screen align
  void setRoundCap(bool v);

  // default false, will disable round cap
  void setScreenAlign(bool v);

protected:
  void compile() override;

  [[nodiscard]] std::string generateHeader();

  [[nodiscard]] virtual float lineWidth() const;

  std::vector<glm::vec4>& lineColors();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  void renderUsingOpengl() override;
  void renderPickingUsingOpengl() override;
#endif

  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye eye) override;

  void enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking) override;

  // void enableLineSmooth();
  // void disableLineSmooth();

private:
  void updateLineWidth()
  {
    if (m_followSupersampling && m_rendererBase.sceneState().geometryAAMode == GeometryAAMode::Supersample2x2) {
      m_lineWidth = (m_srcLineWidth - 0.9f) * 2.f;
    } else {
      m_lineWidth = m_srcLineWidth - 0.9f;
    }

    m_lineWidth *= m_rendererBase.sceneState().devicePixelRatio;
  }

  Z3DShaderGroup& currentShaderGrp()
  {
    if (m_useGeomLineShader && m_useSmoothLine) {
      return *m_smoothLineShaderGrp;
    } else if (m_useSmoothLine) {
      return *m_smoothLineShaderGrp1;
    } else {
      return *m_lineShaderGrp;
    }
  }

protected:
  void createResources(RenderBackend backend) override;

  void destroyResources() override;

  std::unique_ptr<Z3DShaderGroup> m_lineShaderGrp;
  std::unique_ptr<Z3DShaderGroup> m_smoothLineShaderGrp;
  std::unique_ptr<Z3DShaderGroup> m_smoothLineShaderGrp1;

  std::vector<glm::vec3> m_linePositions;
  std::vector<glm::vec4> m_lineColors;
  std::vector<glm::vec4> m_linePickingColors;
  bool m_hasExplicitColors = false;

  bool m_useSmoothLine = true;
  float m_srcLineWidth = 1.f;
  float m_lineWidth = 1.f;
  bool m_followSupersampling = true;
  std::vector<float> m_lineWidthArray;

  Z3DTexture* m_texture = nullptr;

private:
  std::unique_ptr<Z3DVertexArrayObject> m_VAO;
  std::unique_ptr<Z3DVertexArrayObject> m_pickingVAO;
  std::unique_ptr<Z3DVertexBufferObject> m_VBOs;
  std::unique_ptr<Z3DVertexBufferObject> m_pickingVBOs;
  bool m_dataChanged = false;
  bool m_pickingDataChanged = false;
  bool m_isLineStrip = false;

  bool m_useTextureColor = false;
  bool m_screenAligned = false;
  bool m_roundCap = true;

  void renderSmooth(Z3DEye eye);

  void renderSmoothPicking(Z3DEye eye);

  std::vector<glm::vec3> m_smoothLineP0s;
  std::vector<glm::vec3> m_smoothLineP1s;
  std::vector<glm::vec4> m_smoothLineP0Colors;
  std::vector<glm::vec4> m_smoothLineP1Colors;
  std::vector<glm::vec4> m_smoothLinePickingColors;
  std::vector<GLfloat> m_allFlags;
  std::vector<GLuint> m_indexs;

  std::unique_ptr<Z3DVertexArrayObject> m_VAOs;
  std::unique_ptr<Z3DVertexArrayObject> m_pickingVAOs;
  std::vector<std::unique_ptr<Z3DVertexBufferObject>> m_batchVBOs;
  std::vector<std::unique_ptr<Z3DVertexBufferObject>> m_batchPickingVBOs;
  size_t m_oneBatchNumber = 4e6;
  bool m_useGeomLineShader = false;

  [[nodiscard]] LinePayload buildLinePayload(bool picking) const;
  [[nodiscard]] RenderBatch buildRenderBatch(Z3DEye eye, bool picking) const;

  void refreshSmoothLinePayloads();
  void ensureLineColorStorage();
  void syncPickingColorCount();

  // Generation counters for Vulkan selective restaging
  uint32_t m_positionsGen = 0;
  uint32_t m_smoothGen = 0;
  uint32_t m_indicesGen = 0;
  uint32_t m_colorsGen = 0;
  uint32_t m_pickingColorsGen = 0;
  // Wide-line per-stream gens
  uint32_t m_smoothP0Gen = 0;
  uint32_t m_smoothP1Gen = 0;
  uint32_t m_smoothC0Gen = 0;
  uint32_t m_smoothC1Gen = 0;
  uint32_t m_smoothPickGen = 0;
  uint32_t m_smoothFlagsGen = 0;
};

} // namespace nim
