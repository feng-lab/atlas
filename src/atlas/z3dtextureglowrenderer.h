#pragma once

#include "z3dprimitiverenderer.h"
#include "z3drendercommands.h"
#include "z3dshaderprogram.h"
#include <memory>
#include <string>

namespace nim {

class Z3DTextureGlowRenderer : public Z3DPrimitiveRenderer
{
public:
  explicit Z3DTextureGlowRenderer(Z3DRendererBase& rendererBase);

  void setColorTexture(const Z3DTexture* colorTex)
  {
    m_colorTexture = colorTex;
    m_colorAttachmentHandle = {};
  }

  void setDepthTexture(const Z3DTexture* depthTex)
  {
    m_depthTexture = depthTex;
    m_depthAttachmentHandle = {};
  }

  void setSourceAttachments(AttachmentHandle colorHandle, AttachmentHandle depthHandle)
  {
    m_colorAttachmentHandle = colorHandle;
    m_depthAttachmentHandle = depthHandle;
  }

  void setGlowMode(GlowMode mode);

  void setBlurRadius(int radius);

  void setBlurScale(float scale);

  void setBlurStrength(float strength);

  // Query current glow parameters (used by compositor when enqueuing batches directly)
  GlowMode glowMode() const { return m_glowMode; }
  int blurRadius() const { return m_blurRadius; }
  float blurScale() const { return m_blurScale; }
  float blurStrength() const { return m_blurStrength; }

protected:
  void compile() override;

  [[nodiscard]] std::string generateHeader();

  void render(Z3DEye eye) override;

  [[nodiscard]] TextureGlowPayload buildTextureGlowPayload() const;
  [[nodiscard]] TextureGlowPayload buildTextureGlowPayload(AttachmentHandle colorHandle,
                                                           AttachmentHandle depthHandle) const;
  [[nodiscard]] RenderBatch buildRenderBatch(Z3DEye eye) const;
  [[nodiscard]] RenderBatch buildRenderBatch(Z3DEye eye,
                                             AttachmentHandle colorHandle,
                                             AttachmentHandle depthHandle) const;

  void enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking) override;
  void renderVulkan(Z3DEye eye, AttachmentHandle colorHandle, AttachmentHandle depthHandle);

protected:
  const Z3DTexture* m_colorTexture = nullptr;
  const Z3DTexture* m_depthTexture = nullptr;
  AttachmentHandle m_colorAttachmentHandle;
  AttachmentHandle m_depthAttachmentHandle;

  void createResources(RenderBackend backend) override;

  void destroyResources() override;

  std::unique_ptr<Z3DShaderProgram> m_blurXTextureShader;
  std::unique_ptr<Z3DShaderProgram> m_blurYTextureShader;
  std::unique_ptr<Z3DShaderGroup> m_glowTextureShaderGrp;

  GlowMode m_glowMode = GlowMode::Screen;
  int m_blurRadius = 10;
  float m_blurScale = 1.f;
  float m_blurStrength = 0.5f;

  std::unique_ptr<Z3DVertexArrayObject> m_VAO;
};

} // namespace nim
