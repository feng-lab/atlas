#include "z3dlinerenderer.h"

#include "z3dgl.h"
#include "z3dgpuinfo.h"
#include "z3dshaderprogram.h"
#include "z3dtexture.h"
#include "zlog.h"

#include <utility>

namespace nim {

Z3DLineRenderer::Z3DLineRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_hasExplicitColors(false)
  , m_useSmoothLine(true)
  , m_srcLineWidth(1)
  , m_lineWidth(1.f)
  , m_enableMultisample(true)
  , m_texture(nullptr)
  , m_dataChanged(false)
  , m_pickingDataChanged(false)
  , m_isLineStrip(false)
  , m_useTextureColor(false)
  , m_screenAligned(false)
  , m_roundCap(true)
  , m_oneBatchNumber(4e6)
  , m_useGeomLineShader(false)
{
  updateLineWidth();
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  setUseDisplayList(true);
#endif
  createResources(m_rendererBase.activeBackend());
}

void Z3DLineRenderer::createResources(RenderBackend backend)
{
  if (backend != RenderBackend::OpenGL) {
    return;
  }
  m_lineShaderGrp = std::make_unique<Z3DShaderGroup>(m_rendererBase);
  m_smoothLineShaderGrp = std::make_unique<Z3DShaderGroup>(m_rendererBase);
  m_smoothLineShaderGrp1 = std::make_unique<Z3DShaderGroup>(m_rendererBase);

  QStringList allshaders;
  allshaders << "line.vert"
             << "line_func.frag";
  QStringList normalShaders;
  normalShaders << "line.vert"
                << "line.frag";
  m_lineShaderGrp->init(allshaders, m_rendererBase.generateHeader() + generateHeader(), "", normalShaders);
  m_lineShaderGrp->addAllSupportedPostShaders();

  if (m_useGeomLineShader) {
    allshaders.clear();
    allshaders << "wideline.vert"
               << "wideline.geom"
               << "wideline_func.frag";
    m_smoothLineShaderGrp->init(allshaders,
                               m_rendererBase.generateHeader() + generateHeader(),
                               m_rendererBase.generateGeomHeader() + generateHeader());
    m_smoothLineShaderGrp->addAllSupportedPostShaders();
  } else {
    allshaders.clear();
    allshaders << "wideline1.vert"
               << "wideline_func1.frag";
    m_smoothLineShaderGrp1->init(allshaders, m_rendererBase.generateHeader() + generateHeader());
    m_smoothLineShaderGrp1->addAllSupportedPostShaders();
  }

  m_VAO = std::make_unique<Z3DVertexArrayObject>(1);
  m_pickingVAO = std::make_unique<Z3DVertexArrayObject>(1);
  m_VBOs = std::make_unique<Z3DVertexBufferObject>(2);
  m_pickingVBOs = std::make_unique<Z3DVertexBufferObject>(2);
  m_VAOs = std::make_unique<Z3DVertexArrayObject>(1);
  m_pickingVAOs = std::make_unique<Z3DVertexArrayObject>(1);

  m_batchVBOs.clear();
  m_batchPickingVBOs.clear();

  m_dataChanged = true;
  m_pickingDataChanged = true;
}

void Z3DLineRenderer::destroyResources()
{
  m_lineShaderGrp.reset();
  m_smoothLineShaderGrp.reset();
  m_smoothLineShaderGrp1.reset();
  m_VAO.reset();
  m_pickingVAO.reset();
  m_VBOs.reset();
  m_pickingVBOs.reset();
  m_VAOs.reset();
  m_pickingVAOs.reset();
  m_batchVBOs.clear();
  m_batchPickingVBOs.clear();
}

void Z3DLineRenderer::setData(std::span<const glm::vec3> lines)
{
  std::vector<glm::vec3> copy(lines.begin(), lines.end());
  setData(std::move(copy));
}

void Z3DLineRenderer::setData(std::vector<glm::vec3> lines)
{
  m_linePositions = std::move(lines);

  ensureLineColorStorage();
  syncPickingColorCount();
  refreshSmoothLinePayloads();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
  invalidateOpenglPickingRenderer();
#endif
  m_dataChanged = true;
  m_pickingDataChanged = true;
}

void Z3DLineRenderer::setDataColors(std::span<const glm::vec4> lineColorsInput)
{
  std::vector<glm::vec4> copy(lineColorsInput.begin(), lineColorsInput.end());
  setDataColors(std::move(copy));
}

void Z3DLineRenderer::setDataColors(std::vector<glm::vec4> lineColorsInput)
{
  if (m_useTextureColor) {
    m_useTextureColor = false;
    if (m_rendererBase.activeBackend() == RenderBackend::OpenGL) {
      compile();
    }
  }

  m_lineColors = std::move(lineColorsInput);
  m_hasExplicitColors = !m_lineColors.empty();

  ensureLineColorStorage();
  refreshSmoothLinePayloads();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
#endif
  m_dataChanged = true;
}

void Z3DLineRenderer::setTexture(Z3DTexture* tex)
{
  if (m_useSmoothLine) {
    if (!m_useTextureColor) {
      m_useTextureColor = true;
      if (m_rendererBase.activeBackend() == RenderBackend::OpenGL) {
        compile();
      }
      m_dataChanged = true;
    }
    m_texture = tex;
    CHECK(m_texture->textureTarget() == GL_TEXTURE_1D);
  }
}

void Z3DLineRenderer::setDataPickingColors(std::span<const glm::vec4> linePickingColorsInput)
{
  std::vector<glm::vec4> copy(linePickingColorsInput.begin(), linePickingColorsInput.end());
  setDataPickingColors(std::move(copy));
}

void Z3DLineRenderer::setDataPickingColors(std::vector<glm::vec4> linePickingColorsInput)
{
  m_linePickingColors = std::move(linePickingColorsInput);
  syncPickingColorCount();
  refreshSmoothLinePayloads();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglPickingRenderer();
#endif
  m_pickingDataChanged = true;
}

void Z3DLineRenderer::clearPickingColors()
{
  m_linePickingColors.clear();
  refreshSmoothLinePayloads();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglPickingRenderer();
#endif
  m_pickingDataChanged = true;
}

void Z3DLineRenderer::setRoundCap(bool v)
{
  m_roundCap = v;
  if (m_roundCap) {
    m_screenAligned = false;
  }
  if (m_rendererBase.activeBackend() == RenderBackend::OpenGL) {
    compile();
  }
}

void Z3DLineRenderer::setScreenAlign(bool v)
{
  m_screenAligned = v;
  if (m_screenAligned) {
    m_roundCap = false;
  }
  if (m_rendererBase.activeBackend() == RenderBackend::OpenGL) {
    compile();
  }
}

void Z3DLineRenderer::compile()
{
  m_lineShaderGrp->rebuild(m_rendererBase.generateHeader() + generateHeader());
  if (m_useGeomLineShader) {
    m_smoothLineShaderGrp->rebuild(m_rendererBase.generateHeader() + generateHeader(),
                                  m_rendererBase.generateGeomHeader() + generateHeader());
  } else {
    m_smoothLineShaderGrp1->rebuild(m_rendererBase.generateHeader() + generateHeader());
  }
}

std::string Z3DLineRenderer::generateHeader()
{
  std::string header;
  if (m_useTextureColor) {
    header += "#define USE_1DTEXTURE\n";
  }
  if (m_screenAligned) {
    header += "#define LINE_SCREEN_ALIGNED\n";
  }
  if (m_roundCap) {
    header += "#define ROUND_CAP\n";
  }
  return header;
}

float Z3DLineRenderer::lineWidth() const
{
  if (m_followSizeScale) {
    if (m_useSmoothLine) {
      return std::max(1.f, m_lineWidth * m_rendererBase.parameterState().sizeScale);
    } else {
      return std::min(Z3DGpuInfo::instance().maxAliasedLineWidth(),
                      std::max(m_rendererBase.parameterState().sizeScale * m_lineWidth, Z3DGpuInfo::instance().minAliasedLineWidth()));
    }
  } else {
    if (m_useSmoothLine) {
      return m_lineWidth;
    } else {
      return std::min(Z3DGpuInfo::instance().maxAliasedLineWidth(),
                      std::max(m_lineWidth, Z3DGpuInfo::instance().minAliasedLineWidth()));
    }
  }
}

void Z3DLineRenderer::ensureLineColorStorage()
{
  if (m_linePositions.empty()) {
    // Geometry has not been provided yet; keep explicit colors intact until we know the vertex count.
    return;
  }

  if (m_hasExplicitColors) {
    if (m_lineColors.size() > m_linePositions.size()) {
      m_lineColors.resize(m_linePositions.size());
    } else if (m_lineColors.size() < m_linePositions.size()) {
      m_lineColors.resize(m_linePositions.size(), glm::vec4(0.f, 0.f, 0.f, 1.f));
    }
  } else if (m_lineColors.size() != m_linePositions.size()) {
    m_lineColors.assign(m_linePositions.size(), glm::vec4(0.f, 0.f, 0.f, 1.f));
  }
}

void Z3DLineRenderer::syncPickingColorCount()
{
  if (m_linePickingColors.empty()) {
    return;
  }

  if (m_linePositions.empty()) {
    // Delay adjustment until geometry is available so we preserve explicit picking colors.
    return;
  }

  if (m_linePickingColors.size() > m_linePositions.size()) {
    m_linePickingColors.resize(m_linePositions.size());
  } else if (m_linePickingColors.size() < m_linePositions.size()) {
    m_linePickingColors.resize(m_linePositions.size(), glm::vec4(0.f));
  }
}

void Z3DLineRenderer::refreshSmoothLinePayloads()
{
  if (m_useGeomLineShader) {
    return;
  }

  m_smoothLineP0s.clear();
  m_smoothLineP1s.clear();
  m_smoothLineP0Colors.clear();
  m_smoothLineP1Colors.clear();
  m_smoothLinePickingColors.clear();
  m_indexs.clear();

  const size_t vertexCount = m_linePositions.size();
  if (vertexCount < 2) {
    m_allFlags.clear();
    return;
  }

  ensureLineColorStorage();
  syncPickingColorCount();

  constexpr GLuint quadIndices[6] = {0, 1, 2, 2, 1, 3};
  constexpr float cornerFlags[4] = {0 << 4 | 0, 2 << 4 | 0, 0 << 4 | 2, 2 << 4 | 2};

  const bool emitColors = m_hasExplicitColors;
  const bool emitPicking = !m_linePickingColors.empty();

  GLuint quadIdx = 0;
  auto pushSegment = [&](size_t idx0, size_t idx1) {
    const glm::vec3& p0 = m_linePositions[idx0];
    const glm::vec3& p1 = m_linePositions[idx1];

    m_smoothLineP0s.push_back(p0);
    m_smoothLineP0s.push_back(p0);
    m_smoothLineP0s.push_back(p0);
    m_smoothLineP0s.push_back(p0);

    m_smoothLineP1s.push_back(p1);
    m_smoothLineP1s.push_back(p1);
    m_smoothLineP1s.push_back(p1);
    m_smoothLineP1s.push_back(p1);

    for (GLuint idx : quadIndices) {
      m_indexs.push_back(idx + quadIdx * 4);
    }
    ++quadIdx;

    if (emitColors) {
      const glm::vec4& c0 = m_lineColors[idx0];
      const glm::vec4& c1 = m_lineColors[idx1];
      m_smoothLineP0Colors.push_back(c0);
      m_smoothLineP0Colors.push_back(c0);
      m_smoothLineP0Colors.push_back(c0);
      m_smoothLineP0Colors.push_back(c0);

      m_smoothLineP1Colors.push_back(c1);
      m_smoothLineP1Colors.push_back(c1);
      m_smoothLineP1Colors.push_back(c1);
      m_smoothLineP1Colors.push_back(c1);
    }

    if (emitPicking) {
      const glm::vec4& pick0 = m_linePickingColors[idx0];
      m_smoothLinePickingColors.push_back(pick0);
      m_smoothLinePickingColors.push_back(pick0);
      m_smoothLinePickingColors.push_back(pick0);
      m_smoothLinePickingColors.push_back(pick0);
    }
  };

  if (m_isLineStrip) {
    for (size_t i = 1; i < vertexCount; ++i) {
      pushSegment(i - 1, i);
    }
  } else {
    for (size_t i = 0; i + 1 < vertexCount; i += 2) {
      pushSegment(i, i + 1);
    }
  }

  const size_t targetSize = m_smoothLineP0s.size();
  const size_t previousSize = m_allFlags.size();
  m_allFlags.resize(targetSize);
  for (size_t i = previousSize; i < targetSize; i += 4) {
    m_allFlags[i] = cornerFlags[0];
    if (i + 1 < targetSize) {
      m_allFlags[i + 1] = cornerFlags[1];
    }
    if (i + 2 < targetSize) {
      m_allFlags[i + 2] = cornerFlags[2];
    }
    if (i + 3 < targetSize) {
      m_allFlags[i + 3] = cornerFlags[3];
    }
  }
}

std::vector<glm::vec4>& Z3DLineRenderer::lineColors()
{
  ensureLineColorStorage();
  return m_lineColors;
}

void Z3DLineRenderer::buildWideLineGeometry(std::vector<LineWideVertex>& outVertices,
                                            std::vector<uint32_t>& outIndices) const
{
  outVertices.clear();
  outIndices.clear();

  if (!m_useSmoothLine) {
    return;
  }

  const size_t vertexCount = m_smoothLineP0s.size();
  if (vertexCount == 0) {
    return;
  }

  const bool hasP0Colors = !m_smoothLineP0Colors.empty();
  const bool hasP1Colors = !m_smoothLineP1Colors.empty();
  const glm::vec4 defaultColor(0.f, 0.f, 0.f, 1.f);

  outVertices.reserve(vertexCount);
  for (size_t i = 0; i < vertexCount; ++i) {
    LineWideVertex vertex{};
    vertex.p0 = m_smoothLineP0s[i];
    vertex.p1 = m_smoothLineP1s[i];
    vertex.c0 = hasP0Colors ? m_smoothLineP0Colors[i] : defaultColor;
    vertex.c1 = hasP1Colors ? m_smoothLineP1Colors[i] : vertex.c0;
    vertex.flags = (i < m_allFlags.size()) ? m_allFlags[i] : 0.f;
    outVertices.push_back(vertex);
  }

  outIndices.assign(m_indexs.begin(), m_indexs.end());
}

LinePayload Z3DLineRenderer::buildLinePayload(bool picking) const
{
  LinePayload payload;

  payload.renderer = const_cast<Z3DLineRenderer*>(this);

  payload.positions = spanOrEmpty(m_linePositions);

  if (!m_linePositions.empty()) {
    auto& colors = const_cast<Z3DLineRenderer*>(this)->lineColors();
    payload.colors = spanOrEmpty(colors);
  }

  payload.pickingColors = spanOrEmpty(m_linePickingColors);
  payload.perSegmentWidths = spanOrEmpty(m_lineWidthArray);

  payload.smoothP0Positions = spanOrEmpty(m_smoothLineP0s);
  payload.smoothP1Positions = spanOrEmpty(m_smoothLineP1s);
  payload.smoothP0Colors = spanOrEmpty(m_smoothLineP0Colors);
  payload.smoothP1Colors = spanOrEmpty(m_smoothLineP1Colors);
  payload.smoothPickingColors = spanOrEmpty(m_smoothLinePickingColors);
  payload.smoothFlags = spanFromGLfloats(m_allFlags);
  payload.smoothIndices = spanFromGLuints(m_indexs);

  payload.texture = m_texture;
  payload.useSmoothLine = m_useSmoothLine;
  payload.useTextureColor = m_useTextureColor;
  payload.screenAligned = m_screenAligned;
  payload.roundCap = m_roundCap;
  payload.isLineStrip = m_isLineStrip;
  payload.enableMultisample = m_enableMultisample;
  payload.srcLineWidth = m_srcLineWidth;
  payload.resolvedLineWidth = m_lineWidth;
  payload.pickingPass = picking;

  return payload;
}

RenderBatch Z3DLineRenderer::buildRenderBatch(Z3DEye eye, bool picking) const
{
  RenderBatch batch;

  batch.eye = eye;

  batch.draw.topology = m_isLineStrip ? PrimitiveTopology::LineStrip : PrimitiveTopology::LineList;

  auto payload = buildLinePayload(picking);
  batch.draw.vertexCount = static_cast<uint32_t>(payload.positions.size());
  batch.draw.indexCount = static_cast<uint32_t>(payload.smoothIndices.size());
  batch.geometry = std::move(payload);

  return batch;
}

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
void Z3DLineRenderer::renderUsingOpengl()
{
  if (m_linePositions.empty()) {
    return;
  }

  auto& colors = lineColors();

  if (!colors.empty() && colors[0].a != opacity()) {
    for (auto& color : colors) {
      color.a = opacity();
    }
  }

  glLineWidth(lineWidth());
  glPointSize(lineWidth());

  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  // glScalef(getCoordTransform().x, getCoordTransform().y, getCoordTransform().z);
  glMultMatrixf(&coordTransform()[0][0]); // not sure, todo check

  GLuint bufObjects[2];
  glGenBuffers(2, bufObjects);

  glEnableClientState(GL_VERTEX_ARRAY);
  glBindBuffer(GL_ARRAY_BUFFER, bufObjects[0]);
  glBufferData(GL_ARRAY_BUFFER, m_linePositions.size() * 3 * sizeof(GLfloat), m_linePositions.data(), GL_STATIC_DRAW);
  glVertexPointer(3, GL_FLOAT, 0, 0);

  glEnableClientState(GL_COLOR_ARRAY);
  glBindBuffer(GL_ARRAY_BUFFER, bufObjects[1]);
  glBufferData(GL_ARRAY_BUFFER, colors.size() * 4 * sizeof(GLfloat), colors.data(), GL_STATIC_DRAW);
  glColorPointer(4, GL_FLOAT, 0, 0);

  glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m_linePositions.size()));
  glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(m_linePositions.size()));

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(2, bufObjects);
  glDisableClientState(GL_VERTEX_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);

  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();

  glLineWidth(1.0);
  glPointSize(1.0);
}

void Z3DLineRenderer::renderPickingUsingOpengl()
{
  if (m_linePositions.empty()) {
    return;
  }

  if (m_linePickingColors.empty() || m_linePickingColors.size() != m_linePositions.size()) {
    return;
  }

  glLineWidth(lineWidth());
  glPointSize(lineWidth());

  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  // glScalef(getCoordTransform().x, getCoordTransform().y, getCoordTransform().z);
  glMultMatrixf(&coordTransform()[0][0]); // not sure, todo check

  GLuint bufObjects[2];
  glGenBuffers(2, bufObjects);

  glEnableClientState(GL_VERTEX_ARRAY);
  glBindBuffer(GL_ARRAY_BUFFER, bufObjects[0]);
  glBufferData(GL_ARRAY_BUFFER, m_linePositions.size() * 3 * sizeof(GLfloat), m_linePositions.data(), GL_STATIC_DRAW);
  glVertexPointer(3, GL_FLOAT, 0, 0);

  glEnableClientState(GL_COLOR_ARRAY);
  glBindBuffer(GL_ARRAY_BUFFER, bufObjects[1]);
  glBufferData(GL_ARRAY_BUFFER,
               m_linePickingColors.size() * 4 * sizeof(GLfloat),
               m_linePickingColors.data(),
               GL_STATIC_DRAW);
  glColorPointer(4, GL_FLOAT, 0, 0);

  glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m_linePositions.size()));
  glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(m_linePositions.size()));

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(2, bufObjects);
  glDisableClientState(GL_VERTEX_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);

  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();

  glLineWidth(1.0);
  glPointSize(1.0);
}
#endif

void Z3DLineRenderer::render(Z3DEye eye)
{
  if (m_linePositions.empty()) {
    return;
  }

  updateLineWidth();

  if (!m_useGeomLineShader && m_useSmoothLine) {
    renderSmooth(eye);
    return;
  }

  auto& colors = lineColors();

  if (!m_useSmoothLine) {
    glLineWidth(lineWidth());
    glPointSize(lineWidth());
  }

  currentShaderGrp().bind();
  Z3DShaderProgram& shader = currentShaderGrp().get();
  m_rendererBase.setGlobalShaderParameters(shader, eye);
  setShaderParameters(shader);
  shader.setLineWidthUniform(m_lineWidth);
  if (m_useTextureColor) {
    shader.bindTexture("texture", m_texture);
  }

  if (m_useVAO) {
    if (m_dataChanged) {
      m_VAO->bind();
      auto attr_vertex = shader.vertexAttributeLocation();

      glEnableVertexAttribArray(attr_vertex);
      m_VBOs->bind(GL_ARRAY_BUFFER, 0);
      glBufferData(GL_ARRAY_BUFFER, m_linePositions.size() * 3 * sizeof(GLfloat), m_linePositions.data(), GL_STATIC_DRAW);
      glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

      if (!m_useTextureColor) {
        auto attr_color = shader.colorAttributeLocation();
        glEnableVertexAttribArray(attr_color);
        m_VBOs->bind(GL_ARRAY_BUFFER, 1);
        glBufferData(GL_ARRAY_BUFFER, colors.size() * 4 * sizeof(GLfloat), colors.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
      }

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      m_VAO->release();

      m_dataChanged = false;
    }

    m_VAO->bind();
    if (!m_lineWidthArray.empty()) {
      for (size_t i = 0; i < m_lineWidthArray.size(); ++i) {
        if (!m_useSmoothLine) {
          glLineWidth(m_lineWidthArray[i]);
        }
        if (m_isLineStrip) {
          glDrawArrays(GL_LINE_STRIP, i * 2, 2);
        } else {
          glDrawArrays(GL_LINES, i * 2, 2);
        }
      }
    } else {
      if (m_isLineStrip) {
        glDrawArrays(GL_LINE_STRIP, 0, static_cast<GLsizei>(m_linePositions.size()));
      } else {
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m_linePositions.size()));
      }
    }

#ifndef _FLYEM_
    if (!m_useSmoothLine) {
      glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(m_linePositions.size()));
    }
#endif
    m_VAO->release();

  } else {
    auto attr_vertex = shader.vertexAttributeLocation();
    glEnableVertexAttribArray(attr_vertex);
    m_VBOs->bind(GL_ARRAY_BUFFER, 0);
    if (m_dataChanged) {
      glBufferData(GL_ARRAY_BUFFER, m_linePositions.size() * 3 * sizeof(GLfloat), m_linePositions.data(), GL_STATIC_DRAW);
    }
    glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    GLint attr_color = -1;
    if (!m_useTextureColor) {
      attr_color = shader.colorAttributeLocation();
      glEnableVertexAttribArray(attr_color);
      m_VBOs->bind(GL_ARRAY_BUFFER, 1);
      if (m_dataChanged) {
        glBufferData(GL_ARRAY_BUFFER, colors.size() * 4 * sizeof(GLfloat), colors.data(), GL_STATIC_DRAW);
      }
      glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    if (!m_lineWidthArray.empty()) {
      for (size_t i = 0; i < m_lineWidthArray.size(); ++i) {
        if (!m_useSmoothLine) {
          glLineWidth(m_lineWidthArray[i]);
        }
        if (m_isLineStrip) {
          glDrawArrays(GL_LINE_STRIP, i * 2, 2);
        } else {
          glDrawArrays(GL_LINES, i * 2, 2);
        }
      }
    } else {
      if (m_isLineStrip) {
        glDrawArrays(GL_LINE_STRIP, 0, static_cast<GLsizei>(m_linePositions.size()));
      } else {
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m_linePositions.size()));
      }
    }

#ifndef _FLYEM_
    if (!m_useSmoothLine) {
      glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(m_linePositions.size()));
    }
#endif

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (!m_useTextureColor) {
      glDisableVertexAttribArray(attr_color);
    }
    glDisableVertexAttribArray(attr_vertex);

    m_dataChanged = false;
  }

  glLineWidth(1.0);
  glPointSize(1.0);

  currentShaderGrp().release();
}

void Z3DLineRenderer::renderPicking(Z3DEye eye)
{
  if (m_linePositions.empty()) {
    return;
  }

  if (m_linePickingColors.empty() || m_linePickingColors.size() != m_linePositions.size()) {
    return;
  }

  if (!m_useGeomLineShader && m_useSmoothLine) {
    renderSmoothPicking(eye);
    return;
  }

  if (!m_useSmoothLine) {
    glLineWidth(lineWidth());
    glPointSize(lineWidth());
  }

  currentShaderGrp().bind();
  Z3DShaderProgram& shader = currentShaderGrp().get();
  m_rendererBase.setGlobalShaderParameters(shader, eye);
  setPickingShaderParameters(shader);
  shader.setLineWidthUniform(m_lineWidth);

  if (m_useVAO) {
    if (m_pickingDataChanged) {
      m_pickingVAO->bind();
      auto attr_vertex = shader.vertexAttributeLocation();
      auto attr_color = shader.colorAttributeLocation();

      glEnableVertexAttribArray(attr_vertex);
      if (m_dataChanged) {
        m_pickingVBOs->bind(GL_ARRAY_BUFFER, 0);
        glBufferData(GL_ARRAY_BUFFER, m_linePositions.size() * 3 * sizeof(GLfloat), m_linePositions.data(), GL_STATIC_DRAW);
      } else {
        m_VBOs->bind(GL_ARRAY_BUFFER, 0);
      }
      glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_color);
      m_pickingVBOs->bind(GL_ARRAY_BUFFER, 1);
      glBufferData(GL_ARRAY_BUFFER,
                   m_linePickingColors.size() * 4 * sizeof(GLfloat),
                   m_linePickingColors.data(),
                   GL_STATIC_DRAW);
      glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      m_pickingVAO->release();

      m_pickingDataChanged = false;
    }

    m_pickingVAO->bind();
    if (m_isLineStrip) {
      glDrawArrays(GL_LINE_STRIP, 0, static_cast<GLsizei>(m_linePositions.size()));
    } else {
      glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m_linePositions.size()));
    }
    m_pickingVAO->release();

  } else {
    auto attr_vertex = shader.vertexAttributeLocation();
    auto attr_color = shader.colorAttributeLocation();

    glEnableVertexAttribArray(attr_vertex);
    if (m_dataChanged) {
      m_pickingVBOs->bind(GL_ARRAY_BUFFER, 0);
      if (m_pickingDataChanged) {
        glBufferData(GL_ARRAY_BUFFER, m_linePositions.size() * 3 * sizeof(GLfloat), m_linePositions.data(), GL_STATIC_DRAW);
      }
    } else {
      m_VBOs->bind(GL_ARRAY_BUFFER, 0);
    }
    glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_color);
    m_pickingVBOs->bind(GL_ARRAY_BUFFER, 1);
    if (m_pickingDataChanged) {
      glBufferData(GL_ARRAY_BUFFER,
                   m_linePickingColors.size() * 4 * sizeof(GLfloat),
                   m_linePickingColors.data(),
                   GL_STATIC_DRAW);
    }
    glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    if (m_isLineStrip) {
      glDrawArrays(GL_LINE_STRIP, 0, static_cast<GLsizei>(m_linePositions.size()));
    } else {
      glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m_linePositions.size()));
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(attr_color);
    glDisableVertexAttribArray(attr_vertex);

    m_pickingDataChanged = false;
  }

  glLineWidth(1.0);
  glPointSize(1.0);

  currentShaderGrp().release();
}

void Z3DLineRenderer::enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking)
{
  if (backend != RenderBackend::Vulkan) {
    return;
  }

  if (m_linePositions.empty()) {
    return;
  }

  if (picking) {
    if (m_linePickingColors.empty() || m_linePickingColors.size() != m_linePositions.size()) {
      return;
    }
  }

  updateLineWidth();

  auto batch = buildRenderBatch(eye, picking);
  m_rendererBase.appendBatch(std::move(batch));
}

void Z3DLineRenderer::renderSmooth(Z3DEye eye)
{
  updateLineWidth();

  if (m_smoothLineP0Colors.size() < m_smoothLineP0s.size()) {
    for (size_t i = m_smoothLineP0Colors.size(); i < m_smoothLineP0s.size(); ++i) {
      m_smoothLineP0Colors.emplace_back(0.f, 0.f, 0.f, 1.f);
      m_smoothLineP1Colors.emplace_back(0.f, 0.f, 0.f, 1.f);
    }
  }

  m_smoothLineShaderGrp1->bind();
  Z3DShaderProgram& shader = m_smoothLineShaderGrp1->get();
  m_rendererBase.setGlobalShaderParameters(shader, eye);
  setShaderParameters(shader);
  shader.setLineWidthUniform(m_lineWidth);
  if (m_useTextureColor) {
    shader.bindTexture("texture", m_texture);
  }

  size_t numBatch = std::ceil(m_smoothLineP0s.size() * 1.0 / m_oneBatchNumber);

  if (m_useVAO) {
    if (m_dataChanged) {
      m_VAOs->resize(numBatch);

      m_batchVBOs.resize(numBatch);
      for (auto& batchVBO : m_batchVBOs) {
        if (!batchVBO) {
          batchVBO = std::make_unique<Z3DVertexBufferObject>(6);
        } else {
          batchVBO->resize(6);
        }
      }

      // set vertex data
      auto attr_p0 = shader.p0AttributeLocation();
      auto attr_p1 = shader.p1AttributeLocation();
      GLint attr_p0color = -1;
      GLint attr_p1color = -1;
      if (!m_useTextureColor) {
        attr_p0color = shader.p0ColorAttributeLocation();
        attr_p1color = shader.p1ColorAttributeLocation();
      }
      auto attr_flags = shader.flagsAttributeLocation();

      for (size_t i = 0; i < numBatch; ++i) {
        m_VAOs->bind(i);
        size_t size = m_oneBatchNumber;
        if (i == numBatch - 1) {
          size = m_smoothLineP0s.size() - (numBatch - 1) * m_oneBatchNumber;
        }
        size_t start = m_oneBatchNumber * i;

        glEnableVertexAttribArray(attr_p0);
        m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 0);
        glBufferData(GL_ARRAY_BUFFER, size * 3 * sizeof(GLfloat), &(m_smoothLineP0s[start]), GL_STATIC_DRAW);
        glVertexAttribPointer(attr_p0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

        glEnableVertexAttribArray(attr_p1);
        m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 1);
        glBufferData(GL_ARRAY_BUFFER, size * 3 * sizeof(GLfloat), &(m_smoothLineP1s[start]), GL_STATIC_DRAW);
        glVertexAttribPointer(attr_p1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

        if (!m_useTextureColor) {
          glEnableVertexAttribArray(attr_p0color);
          m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 3);
          glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_smoothLineP0Colors[start]), GL_STATIC_DRAW);
          glVertexAttribPointer(attr_p0color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

          glEnableVertexAttribArray(attr_p1color);
          m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 4);
          glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_smoothLineP1Colors[start]), GL_STATIC_DRAW);
          glVertexAttribPointer(attr_p1color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        }

        glEnableVertexAttribArray(attr_flags);
        m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 2);
        glBufferData(GL_ARRAY_BUFFER, size * sizeof(GLfloat), &(m_allFlags[start]), GL_STATIC_DRAW);
        glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

        m_batchVBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, 5);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, size * 6 / 4 * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_VAOs->release();
      }

      m_dataChanged = false;
    }

    for (size_t i = 0; i < numBatch; ++i) {
      size_t size = m_oneBatchNumber;
      if (i == numBatch - 1) {
        size = m_smoothLineP0s.size() - (numBatch - 1) * m_oneBatchNumber;
      }
      m_VAOs->bind(i);
      glDrawElements(GL_TRIANGLES, size * 6 / 4, GL_UNSIGNED_INT, nullptr);
      m_VAOs->release();
    }

  } else {
    if (m_dataChanged) {
      m_batchVBOs.resize(numBatch);
      for (auto& batchVBO : m_batchVBOs) {
        if (!batchVBO) {
          batchVBO = std::make_unique<Z3DVertexBufferObject>(6);
        } else {
          batchVBO->resize(6);
        }
      }
    }
    // set vertex data
    auto attr_p0 = shader.p0AttributeLocation();
    auto attr_p1 = shader.p1AttributeLocation();
    GLint attr_p0color = -1;
    GLint attr_p1color = -1;
    if (!m_useTextureColor) {
      attr_p0color = shader.p0ColorAttributeLocation();
      attr_p1color = shader.p1ColorAttributeLocation();
    }
    auto attr_flags = shader.flagsAttributeLocation();

    for (size_t i = 0; i < numBatch; ++i) {
      size_t size = m_oneBatchNumber;
      if (i == numBatch - 1) {
        size = m_smoothLineP0s.size() - (numBatch - 1) * m_oneBatchNumber;
      }
      size_t start = m_oneBatchNumber * i;

      glEnableVertexAttribArray(attr_p0);
      m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 0);
      if (m_dataChanged) {
        glBufferData(GL_ARRAY_BUFFER, size * 3 * sizeof(GLfloat), &(m_smoothLineP0s[start]), GL_STATIC_DRAW);
      }
      glVertexAttribPointer(attr_p0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_p1);
      m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 1);
      if (m_dataChanged) {
        glBufferData(GL_ARRAY_BUFFER, size * 3 * sizeof(GLfloat), &(m_smoothLineP1s[start]), GL_STATIC_DRAW);
      }
      glVertexAttribPointer(attr_p1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

      if (!m_useTextureColor) {
        glEnableVertexAttribArray(attr_p0color);
        m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 3);
        if (m_dataChanged) {
          glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_smoothLineP0Colors[start]), GL_STATIC_DRAW);
        }
        glVertexAttribPointer(attr_p0color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

        glEnableVertexAttribArray(attr_p1color);
        m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 4);
        if (m_dataChanged) {
          glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_smoothLineP1Colors[start]), GL_STATIC_DRAW);
        }
        glVertexAttribPointer(attr_p1color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
      }

      glEnableVertexAttribArray(attr_flags);
      m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 2);
      if (m_dataChanged) {
        glBufferData(GL_ARRAY_BUFFER, size * sizeof(GLfloat), &(m_allFlags[start]), GL_STATIC_DRAW);
      }
      glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

      m_batchVBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, 5);
      if (m_dataChanged) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, size * 6 / 4 * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);
      }

      glDrawElements(GL_TRIANGLES, size * 6 / 4, GL_UNSIGNED_INT, nullptr);

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
      glDisableVertexAttribArray(attr_p0);
      glDisableVertexAttribArray(attr_p1);
      if (!m_useTextureColor) {
        glDisableVertexAttribArray(attr_p0color);
        glDisableVertexAttribArray(attr_p1color);
      }
      glDisableVertexAttribArray(attr_flags);
    }

    m_dataChanged = false;
  }

  m_smoothLineShaderGrp1->release();
}

void Z3DLineRenderer::renderSmoothPicking(Z3DEye eye)
{
  updateLineWidth();

  m_smoothLineShaderGrp1->bind();
  Z3DShaderProgram& shader = m_smoothLineShaderGrp1->get();
  m_rendererBase.setGlobalShaderParameters(shader, eye);
  setPickingShaderParameters(shader);
  shader.setLineWidthUniform(m_lineWidth);

  size_t numBatch = std::ceil(m_smoothLineP0s.size() * 1.0 / m_oneBatchNumber);

  if (m_useVAO) {
    if (m_pickingDataChanged) {
      m_pickingVAOs->resize(numBatch);

      m_batchPickingVBOs.resize(numBatch);
      for (auto& batchPickingVBO : m_batchPickingVBOs) {
        if (!batchPickingVBO) {
          batchPickingVBO = std::make_unique<Z3DVertexBufferObject>(5);
        } else {
          batchPickingVBO->resize(5);
        }
      }

      // set vertex data
      auto attr_p0 = shader.p0AttributeLocation();
      auto attr_p1 = shader.p1AttributeLocation();
      auto attr_p0color = shader.p0ColorAttributeLocation();
      auto attr_p1color = shader.p1ColorAttributeLocation();
      auto attr_flags = shader.flagsAttributeLocation();

      for (size_t i = 0; i < numBatch; ++i) {
        m_pickingVAOs->bind(i);
        size_t size = m_oneBatchNumber;
        if (i == numBatch - 1) {
          size = m_smoothLineP0s.size() - (numBatch - 1) * m_oneBatchNumber;
        }
        size_t start = m_oneBatchNumber * i;

        glEnableVertexAttribArray(attr_p0);
        if (m_dataChanged) {
          m_batchPickingVBOs[i]->bind(GL_ARRAY_BUFFER, 0);
          glBufferData(GL_ARRAY_BUFFER, size * 3 * sizeof(GLfloat), &(m_smoothLineP0s[start]), GL_STATIC_DRAW);
        } else {
          m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 0);
        }
        glVertexAttribPointer(attr_p0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

        glEnableVertexAttribArray(attr_p1);
        if (m_dataChanged) {
          m_batchPickingVBOs[i]->bind(GL_ARRAY_BUFFER, 1);
          glBufferData(GL_ARRAY_BUFFER, size * 3 * sizeof(GLfloat), &(m_smoothLineP1s[start]), GL_STATIC_DRAW);
        } else {
          m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 1);
        }
        glVertexAttribPointer(attr_p1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

        glEnableVertexAttribArray(attr_p0color);
        m_batchPickingVBOs[i]->bind(GL_ARRAY_BUFFER, 2);
        glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_smoothLinePickingColors[start]), GL_STATIC_DRAW);
        glVertexAttribPointer(attr_p0color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

        glEnableVertexAttribArray(attr_p1color);
        glVertexAttribPointer(attr_p1color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

        glEnableVertexAttribArray(attr_flags);
        if (m_dataChanged) {
          m_batchPickingVBOs[i]->bind(GL_ARRAY_BUFFER, 3);
          glBufferData(GL_ARRAY_BUFFER, size * sizeof(GLfloat), &(m_allFlags[start]), GL_STATIC_DRAW);
        } else {
          m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 2);
        }
        glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

        if (m_dataChanged) {
          m_batchPickingVBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, 4);
          glBufferData(GL_ELEMENT_ARRAY_BUFFER, size * 6 / 4 * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);
        } else {
          m_batchVBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, 5);
        }

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_pickingVAOs->release();
      }

      m_pickingDataChanged = false;
    }

    for (size_t i = 0; i < numBatch; ++i) {
      size_t size = m_oneBatchNumber;
      if (i == numBatch - 1) {
        size = m_smoothLineP0s.size() - (numBatch - 1) * m_oneBatchNumber;
      }
      m_pickingVAOs->bind(i);
      glDrawElements(GL_TRIANGLES, size * 6 / 4, GL_UNSIGNED_INT, nullptr);
      m_pickingVAOs->release();
    }

  } else {
    if (m_pickingDataChanged) {
      m_batchPickingVBOs.resize(numBatch);
      for (auto& batchPickingVBO : m_batchPickingVBOs) {
        if (!batchPickingVBO) {
          batchPickingVBO = std::make_unique<Z3DVertexBufferObject>(5);
        } else {
          batchPickingVBO->resize(5);
        }
      }
    }

    // set vertex data
    auto attr_p0 = shader.p0AttributeLocation();
    auto attr_p1 = shader.p1AttributeLocation();
    auto attr_p0color = shader.p0ColorAttributeLocation();
    auto attr_p1color = shader.p1ColorAttributeLocation();
    auto attr_flags = shader.flagsAttributeLocation();

    for (size_t i = 0; i < numBatch; ++i) {
      size_t size = m_oneBatchNumber;
      if (i == numBatch - 1) {
        size = m_smoothLineP0s.size() - (numBatch - 1) * m_oneBatchNumber;
      }
      size_t start = m_oneBatchNumber * i;

      glEnableVertexAttribArray(attr_p0);
      if (m_dataChanged) {
        m_batchPickingVBOs[i]->bind(GL_ARRAY_BUFFER, 0);
        if (m_pickingDataChanged) {
          glBufferData(GL_ARRAY_BUFFER, size * 3 * sizeof(GLfloat), &(m_smoothLineP0s[start]), GL_STATIC_DRAW);
        }
      } else {
        m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 0);
      }
      glVertexAttribPointer(attr_p0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_p1);
      if (m_dataChanged) {
        m_batchPickingVBOs[i]->bind(GL_ARRAY_BUFFER, 1);
        if (m_pickingDataChanged) {
          glBufferData(GL_ARRAY_BUFFER, size * 3 * sizeof(GLfloat), &(m_smoothLineP1s[start]), GL_STATIC_DRAW);
        }
      } else {
        m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 1);
      }
      glVertexAttribPointer(attr_p1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_p0color);
      m_batchPickingVBOs[i]->bind(GL_ARRAY_BUFFER, 2);
      glBufferData(GL_ARRAY_BUFFER, size * 4 * sizeof(GLfloat), &(m_smoothLinePickingColors[start]), GL_STATIC_DRAW);
      glVertexAttribPointer(attr_p0color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_p1color);
      glVertexAttribPointer(attr_p1color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_flags);
      if (m_dataChanged) {
        m_batchPickingVBOs[i]->bind(GL_ARRAY_BUFFER, 3);
        if (m_pickingDataChanged) {
          glBufferData(GL_ARRAY_BUFFER, size * sizeof(GLfloat), &(m_allFlags[start]), GL_STATIC_DRAW);
        }
      } else {
        m_batchVBOs[i]->bind(GL_ARRAY_BUFFER, 2);
      }
      glVertexAttribPointer(attr_flags, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

      if (m_dataChanged) {
        m_batchPickingVBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, 4);
        if (m_pickingDataChanged) {
          glBufferData(GL_ELEMENT_ARRAY_BUFFER, size * 6 / 4 * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);
        }
      } else {
        m_batchVBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, 5);
      }

      glDrawElements(GL_TRIANGLES, size * 6 / 4, GL_UNSIGNED_INT, nullptr);

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
      glDisableVertexAttribArray(attr_p0);
      glDisableVertexAttribArray(attr_p1);
      glDisableVertexAttribArray(attr_p0color);
      glDisableVertexAttribArray(attr_p1color);
      glDisableVertexAttribArray(attr_flags);
    }

    m_pickingDataChanged = false;
  }

  m_smoothLineShaderGrp1->release();
}

// void Z3DLineRenderer::enableLineSmooth()
//{
// #if defined(_WIN32) || defined(_WIN64)
//   if (Z3DGpuInfoInstance.getGpuVendor() == Z3DGpuInfo::GPU_VENDOR_ATI) {
//     return;
//   }
// #endif
//   return;
//   if (m_rendererBase->getShaderHookType() == Z3DRendererBase::Normal) {
//     glPushAttrib(GL_ALL_ATTRIB_BITS);
//     glDisable(GL_MULTISAMPLE);
//     glEnable(GL_LINE_SMOOTH);
//     glEnable(GL_BLEND);
//     glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
//   } else {
//     glPushAttrib(GL_LINE_BIT);
//     glDisable(GL_MULTISAMPLE);
//     glEnable(GL_LINE_SMOOTH);
//   }
// }

// void Z3DLineRenderer::disableLineSmooth()
//{
// #if defined(_WIN32) || defined(_WIN64)
//   if (Z3DGpuInfoInstance.getGpuVendor() == Z3DGpuInfo::GPU_VENDOR_ATI) {
//     return;
//   }
// #endif
//   return;
//   glPopAttrib();
// }

} // namespace nim
