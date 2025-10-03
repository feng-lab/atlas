#pragma once

#include "z3dprimitiverenderer.h"
#include "z3drendercommands.h"
#include "z3dshaderprogram.h"
#include <memory>
#include <string>

namespace nim {

class Z3DTextureBlendRenderer : public Z3DPrimitiveRenderer
{
public:
  // supported modes:
  // "DepthTest", "FirstOnTop", "SecondOnTop"
  // "DepthTestBlending", "FirstOnTopBlending", "SecondOnTopBlending"
  // "MIPImageDepthTestBlending"
  explicit Z3DTextureBlendRenderer(Z3DRendererBase& rendererBase,
                                   TextureBlendMode mode = TextureBlendMode::DepthTestBlending);

  void setColorTexture1(const Z3DTexture* colorTex)
  {
    m_colorTexture1 = colorTex;
    m_colorAttachmentHandle0 = {};
  }

  void setDepthTexture1(const Z3DTexture* depthTex)
  {
    m_depthTexture1 = depthTex;
    m_depthAttachmentHandle0 = {};
  }

  void setColorTexture2(const Z3DTexture* colorTex)
  {
    m_colorTexture2 = colorTex;
    m_colorAttachmentHandle1 = {};
  }

  void setDepthTexture2(const Z3DTexture* depthTex)
  {
    m_depthTexture2 = depthTex;
    m_depthAttachmentHandle1 = {};
  }

  void setSourceAttachments0(AttachmentHandle colorHandle, AttachmentHandle depthHandle)
  {
    m_colorAttachmentHandle0 = colorHandle;
    m_depthAttachmentHandle0 = depthHandle;
  }

  void setSourceAttachments1(AttachmentHandle colorHandle, AttachmentHandle depthHandle)
  {
    m_colorAttachmentHandle1 = colorHandle;
    m_depthAttachmentHandle1 = depthHandle;
  }

  void setBlendMode(TextureBlendMode mode);

protected:
  void compile() override;

  [[nodiscard]] std::string generateHeader();

  void render(Z3DEye eye) override;

  [[nodiscard]] TextureBlendPayload buildTextureBlendPayload() const;
  [[nodiscard]] RenderBatch buildRenderBatch(Z3DEye eye) const;

  void enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking) override;
  void renderVulkan(Z3DEye eye,
                    AttachmentHandle colorHandle0,
                    AttachmentHandle depthHandle0,
                    AttachmentHandle colorHandle1,
                    AttachmentHandle depthHandle1);

protected:
  const Z3DTexture* m_colorTexture1 = nullptr;
  const Z3DTexture* m_depthTexture1 = nullptr;
  const Z3DTexture* m_colorTexture2 = nullptr;
  const Z3DTexture* m_depthTexture2 = nullptr;
  AttachmentHandle m_colorAttachmentHandle0;
  AttachmentHandle m_depthAttachmentHandle0;
  AttachmentHandle m_colorAttachmentHandle1;
  AttachmentHandle m_depthAttachmentHandle1;

  void createResources(RenderBackend backend) override;

  void destroyResources() override;

  std::unique_ptr<Z3DShaderProgram> m_blendTextureShader;

  TextureBlendMode m_blendMode;
  std::unique_ptr<Z3DVertexArrayObject> m_VAO;
};

} // namespace nim
