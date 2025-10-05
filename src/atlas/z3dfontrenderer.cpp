#include "z3dfontrenderer.h"

#include "z3dgl.h"
#include "zsysteminfo.h"
#include "z3dgpuinfo.h"
#include "z3dsdfont.h"
#include "z3dshaderprogram.h"
#include "zlog.h"
#include "z3drendercommands.h"
#include <QDir>
#include <absl/strings/str_cat.h>
#include <algorithm>

namespace nim {

Z3DFontRenderer::Z3DFontRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_positionsPt(nullptr)
  , m_colorsPt(nullptr)
  , m_pickingColorsPt(nullptr)
  , m_dataChanged(false)
  , m_pickingDataChanged(false)
{
  createResources(m_rendererBase.activeBackend());

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
  if (m_rendererBase.activeBackend() != RenderBackend::OpenGL) {
    return;
  }
  DCHECK(m_fontShaderGrp != nullptr);
  m_fontShaderGrp->rebuild(m_rendererBase.generateHeader() + generateHeader());
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

std::string Z3DFontRenderer::generateHeader()
{
  std::string header = "#define USE_SOFTEDGE\n";
  if (m_showFontOutline) {
    if (m_fontOutlineMode == FontOutlineMode::Glow) {
      absl::StrAppend(&header, "#define SHOW_GLOW\n");
    } else {
      absl::StrAppend(&header, "#define SHOW_OUTLINE\n");
    }
  }
  if (m_showFontShadow) {
    absl::StrAppend(&header, "#define SHOW_SHADOW\n");
  }
  return header;
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

  m_fontShaderGrp->bind();
  Z3DShaderProgram& shader = m_fontShaderGrp->get();

  m_rendererBase.setGlobalShaderParameters(shader, eye);
  if (auto* tex = fontAtlasTextureGL(*font)) {
    shader.bindTexture("tex", tex);
  }
  shader.setUniform("softedge_scale", m_fontSoftEdgeScale);
  if (m_showFontOutline) {
    shader.setUniform("outline_color", m_fontOutlineColor);
  }
  if (m_showFontShadow) {
    shader.setUniform("shadow_color", m_fontShadowColor);
  }

  if (m_useVAO) {
    if (m_dataChanged) {
      m_VAO->bind();
      // set vertex data
      auto attr_vertex = shader.vertexAttributeLocation();
      auto attr_2dTexCoord0 = shader.tex2dCoord0AttributeLocation();
      auto attr_color = shader.colorAttributeLocation();

      glEnableVertexAttribArray(attr_vertex);
      m_VBOs->bind(GL_ARRAY_BUFFER, 0);
      glBufferData(GL_ARRAY_BUFFER,
                   m_fontPositions.size() * 3 * sizeof(GLfloat),
                   m_fontPositions.data(),
                   GL_STATIC_DRAW);
      glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_2dTexCoord0);
      m_VBOs->bind(GL_ARRAY_BUFFER, 1);
      glBufferData(GL_ARRAY_BUFFER,
                   m_fontTextureCoords.size() * 2 * sizeof(GLfloat),
                   m_fontTextureCoords.data(),
                   GL_STATIC_DRAW);
      glVertexAttribPointer(attr_2dTexCoord0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

      glEnableVertexAttribArray(attr_color);
      m_VBOs->bind(GL_ARRAY_BUFFER, 2);
      glBufferData(GL_ARRAY_BUFFER, m_fontColors.size() * 4 * sizeof(GLfloat), m_fontColors.data(), GL_STATIC_DRAW);
      glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

      m_VBOs->bind(GL_ELEMENT_ARRAY_BUFFER, 3);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indexs.size() * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      m_VAO->release();

      m_dataChanged = false;
    }

    m_VAO->bind();
    glDrawElements(GL_TRIANGLES, m_indexs.size(), GL_UNSIGNED_INT, nullptr);
    m_VAO->release();

  } else {
    // set vertex data
    auto attr_vertex = shader.vertexAttributeLocation();
    auto attr_2dTexCoord0 = shader.tex2dCoord0AttributeLocation();
    auto attr_color = shader.colorAttributeLocation();

    glEnableVertexAttribArray(attr_vertex);
    m_VBOs->bind(GL_ARRAY_BUFFER, 0);
    glBufferData(GL_ARRAY_BUFFER, m_fontPositions.size() * 3 * sizeof(GLfloat), m_fontPositions.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_2dTexCoord0);
    m_VBOs->bind(GL_ARRAY_BUFFER, 1);
    glBufferData(GL_ARRAY_BUFFER,
                 m_fontTextureCoords.size() * 2 * sizeof(GLfloat),
                 m_fontTextureCoords.data(),
                 GL_STATIC_DRAW);
    glVertexAttribPointer(attr_2dTexCoord0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray(attr_color);
    m_VBOs->bind(GL_ARRAY_BUFFER, 2);
    glBufferData(GL_ARRAY_BUFFER, m_fontColors.size() * 4 * sizeof(GLfloat), m_fontColors.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    m_VBOs->bind(GL_ELEMENT_ARRAY_BUFFER, 3);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indexs.size() * sizeof(GLuint), m_indexs.data(), GL_STATIC_DRAW);

    glDrawElements(GL_TRIANGLES, m_indexs.size(), GL_UNSIGNED_INT, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(attr_vertex);
    glDisableVertexAttribArray(attr_2dTexCoord0);
    glDisableVertexAttribArray(attr_color);
  }

  m_fontShaderGrp->release();

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

void Z3DFontRenderer::enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking)
{
  if (backend != RenderBackend::Vulkan) {
    return;
  }

  if (m_allFonts.empty()) {
    return;
  }

  if (!m_positionsPt || m_positionsPt->empty() || m_positionsPt->size() != static_cast<size_t>(m_texts.size())) {
    return;
  }

  if (picking) {
    if (!m_pickingColorsPt || m_pickingColorsPt->empty() || m_pickingColorsPt->size() != m_positionsPt->size()) {
      return;
    }
  }

  // Build CPU-side quads and attributes
  prepareFontShaderData(eye);

  m_selectedFontIndex = std::clamp(m_selectedFontIndex, 0, static_cast<int>(m_allFonts.size()) - 1);
  Z3DSDFont* font = m_allFonts[m_selectedFontIndex].get();
  if (!font) {
    return;
  }

  FontPayload payload;
  payload.renderer = this;
  payload.positions = spanOrEmpty(m_fontPositions);
  payload.texcoords = spanOrEmpty(m_fontTextureCoords);
  payload.colors = spanOrEmpty(m_fontColors);
  payload.pickingColors = spanOrEmpty(m_fontPickingColors);
  payload.indices = spanFromGLuints(m_indexs);
  // Avoid GL bridging for Vulkan: provide CPU atlas
  payload.atlasTexture = nullptr;
  payload.atlasPixels = font->atlasPixelsBGRA8();
  payload.atlasWidth = font->atlasWidth();
  payload.atlasHeight = font->atlasHeight();
  payload.softedgeScale = m_fontSoftEdgeScale;
  payload.showOutline = m_showFontOutline;
  payload.showShadow = m_showFontShadow;
  payload.outlineMode = (m_fontOutlineMode == FontOutlineMode::Glow) ? 0 : 1;
  payload.outlineColor = m_fontOutlineColor;
  payload.shadowColor = m_fontShadowColor;
  payload.pickingPass = picking;

  if (payload.positions.empty() || payload.texcoords.empty() || payload.indices.empty()) {
    return;
  }

  if (picking) {
    if (payload.pickingColors.empty()) {
      return;
    }
  } else {
    if (payload.colors.empty()) {
      return;
    }
  }

  RenderBatch batch;
  batch.eye = eye;
  batch.draw.topology = PrimitiveTopology::TriangleList;
  batch.draw.vertexCount = static_cast<uint32_t>(payload.positions.size());
  batch.draw.indexCount = static_cast<uint32_t>(payload.indices.size());
  batch.geometry = std::move(payload);

  m_rendererBase.appendBatch(std::move(batch));
}

void Z3DFontRenderer::prepareFontShaderData(Z3DEye eye)
{
  m_fontPositions.clear();
  m_fontTextureCoords.clear();
  m_fontColors.clear();
  m_fontPickingColors.clear();
  m_indexs.clear();
  const auto& eyeState = m_rendererBase.viewState().eyes[static_cast<size_t>(eye)];
  glm::mat4 viewMatrix = eyeState.viewMatrix;
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

void Z3DFontRenderer::createResources(RenderBackend backend)
{
  if (backend != RenderBackend::OpenGL) {
    return;
  }
  m_fontShaderGrp = std::make_unique<Z3DShaderGroup>(m_rendererBase);
  m_VAO = std::make_unique<Z3DVertexArrayObject>(1);
  m_VBOs = std::make_unique<Z3DVertexBufferObject>(4);
  m_pickingVBOs = std::make_unique<Z3DVertexBufferObject>(4);

  QStringList allshaders;
  allshaders << "almag.vert"
             << "almag_func.frag";
  QStringList normalShaders;
  normalShaders << "almag.vert"
                << "almag.frag";
  m_fontShaderGrp->init(allshaders, m_rendererBase.generateHeader() + generateHeader(), "", normalShaders);
  m_fontShaderGrp->addAllSupportedPostShaders();

  m_dataChanged = true;
  m_pickingDataChanged = true;
}

void Z3DFontRenderer::destroyResources()
{
  m_fontShaderGrp.reset();
  m_VAO.reset();
  m_VBOs.reset();
  m_pickingVBOs.reset();
}

Z3DTexture* Z3DFontRenderer::fontAtlasTextureGL(const Z3DSDFont& font) const
{
  const auto* key = &font;
  const uint32_t w = font.atlasWidth();
  const uint32_t h = font.atlasHeight();
  auto itMeta = m_fontCache.meta.find(key);
  auto itTex = m_fontCache.textures.find(key);
  const bool needCreate = itMeta == m_fontCache.meta.end() ||
                          itTex == m_fontCache.textures.end() ||
                          itMeta->second.first != w || itMeta->second.second != h;
  if (needCreate) {
    const uint8_t* pixels = font.atlasPixelsBGRA8();
    if (!pixels || w == 0u || h == 0u) {
      return nullptr;
    }
    auto tex = std::make_unique<Z3DTexture>(GLint(GL_RGBA8), glm::uvec3(w, h, 1), GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV);
    tex->updateImage(pixels);
    m_fontCache.textures[key] = std::move(tex);
    m_fontCache.meta[key] = std::make_pair(w, h);
  }
  return m_fontCache.textures[key].get();
}

void Z3DFontRenderer::releaseBackendResources()
{
  // Clear GL cache; textures will be deleted with unique_ptr
  m_fontCache.textures.clear();
  m_fontCache.meta.clear();
  Z3DPrimitiveRenderer::releaseBackendResources();
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
