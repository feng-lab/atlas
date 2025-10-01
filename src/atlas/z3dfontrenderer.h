#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dsdfont.h"
#include <QString>
#include <QStringList>
#include <memory>
#include <string>

namespace nim {

enum class FontOutlineMode
{
  Glow,
  Outline
};

class Z3DFontRenderer : public Z3DPrimitiveRenderer
{
public:
  explicit Z3DFontRenderer(Z3DRendererBase& rendererBase);

  void setData(std::vector<glm::vec3>* positions, const QStringList& texts);

  void setDataColors(std::vector<glm::vec4>* colors);

  void setDataPickingColors(std::vector<glm::vec4>* pickingColors = nullptr);

  const QStringList& fontNames() const
  {
    return m_fontNames;
  }

  QString selectedFontName() const
  {
    return m_selectedFontName;
  }

  void setFontName(const QString& fontName);

  void setFontSize(float size);

  void setFontSoftEdgeScale(float scale);

  void setShowFontOutline(bool show);

  void setFontOutlineMode(FontOutlineMode mode);

  void setFontOutlineColor(const glm::vec4& color);

  void setShowFontShadow(bool show);

  void setFontShadowColor(const glm::vec4& color);

  void compile() override;

  std::vector<glm::vec4>* getColors();

  [[nodiscard]] std::string generateHeader();

  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye) override;

  void prepareFontShaderData(Z3DEye eye);

protected:
  void createResources(RenderBackend backend) override;

  void destroyResources() override;

  std::unique_ptr<Z3DShaderGroup> m_fontShaderGrp;

  QStringList m_fontNames;
  QString m_selectedFontName;
  int m_selectedFontIndex = 0;
  float m_fontSize = 32.f;
  float m_fontSoftEdgeScale = 80.f;
  bool m_showFontOutline = false;
  FontOutlineMode m_fontOutlineMode = FontOutlineMode::Glow;
  glm::vec4 m_fontOutlineColor = glm::vec4(1.f);
  bool m_showFontShadow = false;
  glm::vec4 m_fontShadowColor = glm::vec4(0.f, 0.f, 0.f, 1.f);

  std::vector<std::unique_ptr<Z3DSDFont>> m_allFonts;

  std::vector<glm::vec3>* m_positionsPt;
  std::vector<glm::vec4>* m_colorsPt;
  std::vector<glm::vec4>* m_pickingColorsPt;
  QStringList m_texts;
  std::vector<glm::vec4> m_colors;
  std::vector<glm::vec4> m_fontColors;
  std::vector<glm::vec4> m_fontPickingColors;
  std::vector<glm::vec3> m_fontPositions;
  std::vector<glm::vec2> m_fontTextureCoords;
  std::vector<GLuint> m_indexs;

  std::unique_ptr<Z3DVertexArrayObject> m_VAO;
  std::unique_ptr<Z3DVertexBufferObject> m_VBOs;
  std::unique_ptr<Z3DVertexBufferObject> m_pickingVBOs;
  bool m_dataChanged;
  bool m_pickingDataChanged;
};

} // namespace nim
