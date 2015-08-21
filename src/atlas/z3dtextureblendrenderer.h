#ifndef Z3DTEXTUREBLENDRENDERER_H
#define Z3DTEXTUREBLENDRENDERER_H

#include "z3dprimitiverenderer.h"
#include "z3dshaderprogram.h"

class Z3DTexture;

namespace nim {

class Z3DTextureBlendRenderer : public Z3DPrimitiveRenderer
{
  Q_OBJECT
public:
  // supported modes:
  // "DepthTest", "FirstOnTop", "SecondOnTop"
  // "DepthTestBlending", "FirstOnTopBlending", "SecondOnTopBlending"
  explicit Z3DTextureBlendRenderer(Z3DRendererBase &rendererBase, const QString &mode = "DepthTestBlending");

  void setColorTexture1(const Z3DTexture *colorTex) { m_colorTexture1 = colorTex; }
  void setDepthTexture1(const Z3DTexture *depthTex) { m_depthTexture1 = depthTex; }
  void setColorTexture2(const Z3DTexture *colorTex) { m_colorTexture2 = colorTex; }
  void setDepthTexture2(const Z3DTexture *depthTex) { m_depthTexture2 = depthTex; }

  ZStringStringOptionParameter& blendModePara() { return m_blendMode; }

signals:

public slots:

protected:
  virtual void compile() override;
  QString generateHeader();

  virtual void render(Z3DEye eye) override;

  const Z3DTexture *m_colorTexture1;
  const Z3DTexture *m_depthTexture1;
  const Z3DTexture *m_colorTexture2;
  const Z3DTexture *m_depthTexture2;

  Z3DShaderProgram m_blendTextureShader;

  ZStringStringOptionParameter m_blendMode;
  ZVertexArrayObject m_VAO;
};

} // namespace nim

#endif // Z3DTEXTUREBLENDRENDERER_H
