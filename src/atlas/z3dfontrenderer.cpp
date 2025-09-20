#include "z3dfontrenderer.h"

#include "z3dgl.h"
#include "zsysteminfo.h"
#include "z3dgpuinfo.h"
#include "z3dsdfont.h"
#include "z3dshaderprogram.h"
#include "zlog.h"
#include <QDir>
#include <algorithm>

namespace nim {

Z3DFontRenderer::Z3DFontRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_fontShaderGrp(rendererBase)
  , m_positionsPt(nullptr)
  , m_colorsPt(nullptr)
  , m_pickingColorsPt(nullptr)
  , m_VAO(1)
  , m_VBOs(4)
  , m_pickingVBOs(4)
  , m_dataChanged(false)
  , m_pickingDataChanged(false)
{
  QStringList allshaders;
  allshaders << "almag.vert"
             << "almag_func.frag";
  QStringList normalShaders;
  normalShaders << "almag.vert"
                << "almag.frag";
  m_fontShaderGrp.init(allshaders, m_rendererBase.generateHeader() + generateHeader(), "", normalShaders);
  m_fontShaderGrp.addAllSupportedPostShaders();

  // search for available fonts
  QDir fontDir(ZSystemInfo::instance().fontPath());
  QStringList filters;
  filters << "*.png";
  QFileInfoList list = fontDir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (const auto& fileInfo : list) {
    QFileInfo txtFileInfo(fontDir, fileInfo.completeBaseName() + ".txt");
    if (!txtFileInfo.exists()) {
      continue;
    }
    auto sdFont = std::make_unique<Z3DSDFont>(fileInfo.absoluteFilePath(), txtFileInfo.absoluteFilePath());
    if (!sdFont->isEmpty()) {
      m_fontNames.push_back(sdFont->fontName());
      m_allFonts.emplace_back(std::move(sdFont));
    }
  }
  if (!m_fontNames.isEmpty()) {
    m_selectedFontName = m_fontNames.front();
    m_selectedFontIndex = 0;
  }
}

void Z3DFontRenderer::setData(std::vector<glm::vec3>* positions, const QStringList& texts)
{
  m_positionsPt = positions;
  m_texts = texts;
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
  invalidateOpenglPickingRenderer();
#endif
  m_dataChanged = true;
  m_pickingDataChanged = true;
}

void Z3DFontRenderer::setDataColors(std::vector<glm::vec4>* colors)
{
  m_colorsPt = colors;
  m_colors.clear();
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
#endif
  m_dataChanged = true;
}

void Z3DFontRenderer::setDataPickingColors(std::vector<glm::vec4>* pickingColors)
{
  m_pickingColorsPt = pickingColors;
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglPickingRenderer();
#endif
  m_pickingDataChanged = true;
}

void Z3DFontRenderer::compile()
{
  m_fontShaderGrp.rebuild(m_rendererBase.generateHeader() + generateHeader());
}

std::vector<glm::vec4>* Z3DFontRenderer::getColors()
{
  if (!m_colorsPt) {
    m_colors.assign(m_positionsPt->size(), glm::vec4(0.f, 0.f, 0.f, 1.f));
    return &m_colors;
  } else if (m_colorsPt->size() < m_positionsPt->size()) {
    m_colors.clear();
    for (const auto& color : *m_colorsPt) {
      m_colors.push_back(color);
    }
    for (size_t i = m_colorsPt->size(); i < m_positionsPt->size(); ++i) {
      m_colors.emplace_back(0.f, 0.f, 0.f, 1.f);
    }
    return &m_colors;
  }

  return m_colorsPt;
}

QString Z3DFontRenderer::generateHeader()
{
  // if (m_fontUseSoftEdge.get())
  QString headerSource = "#define USE_SOFTEDGE\n";
  if (m_showFontOutline) {
    if (m_fontOutlineMode == FontOutlineMode::Glow) {
      headerSource += "#define SHOW_GLOW\n";
    } else {
      headerSource += "#define SHOW_OUTLINE\n";
    }
  }
  if (m_showFontShadow) {
    headerSource += "#define SHOW_SHADOW\n";
  }
  return headerSource;
}

void Z3DFontRenderer::render(Z3DEye eye)
{
  if (m_allFonts.empty()) {
    LOG(ERROR) << "Can not find any font.";
    return;
  }
  if (!m_positionsPt || m_positionsPt->empty() || m_positionsPt->size() != static_cast<size_t>(m_texts.size())) {
    return;
  }

  prepareFontShaderData(eye);

  m_selectedFontIndex = std::clamp(m_selectedFontIndex, 0, static_cast<int>(m_allFonts.size()) - 1);
  Z3DSDFont* font = m_allFonts[m_selectedFontIndex].get();
  if (fontNames().size() > m_selectedFontIndex) {
    m_selectedFontName = m_fontNames[m_selectedFontIndex];
  }

  if (m_rendererBase.shaderHookType() == Z3DRendererBase::ShaderHookType::Normal) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  }

  m_fontShaderGrp.bind();
  Z3DShaderProgram& shader = m_fontShaderGrp.get();

  m_rendererBase.setGlobalShaderParameters(shader, eye);
  shader.bindTexture("tex", font->texture());
  shader.setUniform("softedge_scale", m_fontSoftEdgeScale);
  if (m_showFontOutline) {
    shader.setUniform("outline_color", m_fontOutlineColor);
  }
  if (m_showFontShadow) {
    shader.setUniform("shadow_color", m_fontShadowColor);
  }

  if (m_useVAO) {
    if (m_dataChanged) {
      m_VAO.bind();
      // set vertex data
      auto attr_vertex = shader.vertexAttributeLocation();
      auto attr_2dTexCoord0 = shader.tex2dCoord0AttributeLocation();
      auto attr_color = shader.colorAttributeLocation();

      glEnableVertexAttribArray(attr_vertex);
      m_VBOs.bind(GL_ARRAY_BUFFER, 0);
      glBufferData(GL_ARRAY_BUFFER,
                   m_fontPositions.size() * 3 * sizeof(GLfloat),
                   m_fontPositions.data(),
                   GL_STATIC_DRAW);
      glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_2dTexCoord0);
      m_VBOs.bind(GL_ARRAY_BUFFER, 1);
      glBufferData(GL_ARRAY_BUFFER,
                   m_fontTextureCoords.size() * 2 * sizeof(GLfloat),
                   m_fontTextureCoords.data(),
                   GL_STATIC_DRAW);
      glVertexAttribPointer(attr_2dTexCoord0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_color);
      m_VBOs.bind(GL_ARRAY_BUFFER, 2);
      glBufferData(GL_ARRAY_BUFFER, m_fontColors.size() * 4 * sizeof(GLfloat), m_fontColors.data(), GL_STATIC_DRAW);
      glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      m_VBOs.bind(GL_ELEMENT_ARRAY_BUFFER, 3);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indexs.size() * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      m_VAO.release();

      m_dataChanged = false;
    }

    m_VAO.bind();
    glDrawElements(GL_TRIANGLES, m_indexs.size(), GL_UNSIGNED_INT, nullptr);
    m_VAO.release();

  } else {
    // set vertex data
    auto attr_vertex = shader.vertexAttributeLocation();
    auto attr_2dTexCoord0 = shader.tex2dCoord0AttributeLocation();
    auto attr_color = shader.colorAttributeLocation();

    glEnableVertexAttribArray(attr_vertex);
    m_VBOs.bind(GL_ARRAY_BUFFER, 0);
    glBufferData(GL_ARRAY_BUFFER, m_fontPositions.size() * 3 * sizeof(GLfloat), m_fontPositions.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_2dTexCoord0);
    m_VBOs.bind(GL_ARRAY_BUFFER, 1);
    glBufferData(GL_ARRAY_BUFFER,
                 m_fontTextureCoords.size() * 2 * sizeof(GLfloat),
                 m_fontTextureCoords.data(),
                 GL_STATIC_DRAW);
    glVertexAttribPointer(attr_2dTexCoord0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_color);
    m_VBOs.bind(GL_ARRAY_BUFFER, 2);
    glBufferData(GL_ARRAY_BUFFER, m_fontColors.size() * 4 * sizeof(GLfloat), m_fontColors.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    m_VBOs.bind(GL_ELEMENT_ARRAY_BUFFER, 3);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indexs.size() * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);

    glDrawElements(GL_TRIANGLES, m_indexs.size(), GL_UNSIGNED_INT, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(attr_vertex);
    glDisableVertexAttribArray(attr_2dTexCoord0);
    glDisableVertexAttribArray(attr_color);
  }

  m_fontShaderGrp.release();

  if (m_rendererBase.shaderHookType() == Z3DRendererBase::ShaderHookType::Normal) {
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
  }
}

void Z3DFontRenderer::renderPicking(Z3DEye)
{
  if (m_allFonts.empty()) {
    LOG(ERROR) << "Can not find any font.";
    return;
  }
  if (!m_pickingColorsPt || m_pickingColorsPt->empty() || m_pickingColorsPt->size() != m_positionsPt->size()) {
    return;
  }
  if (!m_positionsPt || m_positionsPt->empty() || m_positionsPt->size() != static_cast<size_t>(m_texts.size())) {
    return;
  }
}

void Z3DFontRenderer::prepareFontShaderData(Z3DEye eye)
{
  m_fontPositions.clear();
  m_fontTextureCoords.clear();
  m_fontColors.clear();
  m_fontPickingColors.clear();
  m_indexs.clear();
  glm::mat4 viewMatrix = m_rendererBase.camera().viewMatrix(eye);
  glm::vec3 rightVector(viewMatrix[0][0], viewMatrix[0][1], viewMatrix[0][2]);
  glm::vec3 upVector(viewMatrix[1][0], viewMatrix[1][1], viewMatrix[1][2]);
  m_selectedFontIndex = std::clamp(m_selectedFontIndex, 0, static_cast<int>(m_allFonts.size()) - 1);
  Z3DSDFont* font = m_allFonts[m_selectedFontIndex].get();
  float scale = m_fontSize / static_cast<float>(font->maxFontHeight());
  GLuint indices[6] = {0, 1, 2, 2, 1, 3};
  GLuint quadIdx = 0;
  for (index_t strIdx = 0; strIdx < m_texts.size(); strIdx++) {
    QString str = m_texts[strIdx];
    if (str.isEmpty()) {
      continue;
    }
    glm::vec4 color;
    if (!m_colorsPt || static_cast<size_t>(strIdx) >= m_colorsPt->size()) {
      color = glm::vec4(0.f, 0.f, 0.f, 1.f);
    } else {
      color = (*m_colorsPt)[strIdx];
    }
    glm::vec3 loc = (*m_positionsPt)[strIdx];
    for (auto charIdx : str) {
      Z3DSDFont::CharInfo charInfo = font->charInfo(charIdx.toLatin1());
      glm::vec3 leftUp = loc + rightVector * charInfo.xoffset * scale + upVector * charInfo.yoffset * scale;
      glm::vec3 leftDown = leftUp - upVector * static_cast<float>(charInfo.height) * scale;
      glm::vec3 rightUp = leftUp + rightVector * static_cast<float>(charInfo.width) * scale;
      glm::vec3 rightDown = leftDown + rightVector * static_cast<float>(charInfo.width) * scale;
      m_fontPositions.push_back(leftDown);
      m_fontPositions.push_back(rightDown);
      m_fontPositions.push_back(leftUp);
      m_fontPositions.push_back(rightUp);
      m_fontTextureCoords.emplace_back(charInfo.sMin, charInfo.tMin);
      m_fontTextureCoords.emplace_back(charInfo.sMax, charInfo.tMin);
      m_fontTextureCoords.emplace_back(charInfo.sMin, charInfo.tMax);
      m_fontTextureCoords.emplace_back(charInfo.sMax, charInfo.tMax);
      m_fontColors.push_back(color);
      m_fontColors.push_back(color);
      m_fontColors.push_back(color);
      m_fontColors.push_back(color);
      if (m_pickingColorsPt && m_pickingColorsPt->size() == m_positionsPt->size()) {
        m_fontPickingColors.push_back((*m_pickingColorsPt)[strIdx]);
        m_fontPickingColors.push_back((*m_pickingColorsPt)[strIdx]);
        m_fontPickingColors.push_back((*m_pickingColorsPt)[strIdx]);
        m_fontPickingColors.push_back((*m_pickingColorsPt)[strIdx]);
      }
      for (auto index : indices) {
        m_indexs.push_back(index + 4 * quadIdx);
      }
      quadIdx++;
      loc += rightVector * charInfo.xadvance * scale;
    }
  }
}

void Z3DFontRenderer::setFontName(const QString& fontName)
{
  if (m_fontNames.isEmpty()) {
    return;
  }
  const QString normalized = fontName.isEmpty() ? m_fontNames.front() : fontName;
  int index = m_fontNames.indexOf(normalized);
  if (index < 0) {
    index = 0;
  }
  if (index == m_selectedFontIndex) {
    return;
  }
  m_selectedFontIndex = index;
  m_selectedFontName = m_fontNames[m_selectedFontIndex];
  m_dataChanged = true;
  m_pickingDataChanged = true;
}

void Z3DFontRenderer::setFontSize(float size)
{
  float clamped = std::clamp(size, 0.1f, 5000.f);
  if (m_fontSize == clamped) {
    return;
  }
  m_fontSize = clamped;
  m_dataChanged = true;
  m_pickingDataChanged = true;
}

void Z3DFontRenderer::setFontSoftEdgeScale(float scale)
{
  float clamped = std::clamp(scale, 0.f, 200.f);
  if (m_fontSoftEdgeScale == clamped) {
    return;
  }
  m_fontSoftEdgeScale = clamped;
}

void Z3DFontRenderer::setShowFontOutline(bool show)
{
  if (m_showFontOutline == show) {
    return;
  }
  m_showFontOutline = show;
  m_dataChanged = true;
  compile();
}

void Z3DFontRenderer::setFontOutlineMode(FontOutlineMode mode)
{
  if (m_fontOutlineMode == mode) {
    return;
  }
  m_fontOutlineMode = mode;
  compile();
}

void Z3DFontRenderer::setFontOutlineColor(const glm::vec4& color)
{
  if (m_fontOutlineColor == color) {
    return;
  }
  m_fontOutlineColor = color;
}

void Z3DFontRenderer::setShowFontShadow(bool show)
{
  if (m_showFontShadow == show) {
    return;
  }
  m_showFontShadow = show;
  m_dataChanged = true;
  compile();
}

void Z3DFontRenderer::setFontShadowColor(const glm::vec4& color)
{
  if (m_fontShadowColor == color) {
    return;
  }
  m_fontShadowColor = color;
}

} // namespace nim
