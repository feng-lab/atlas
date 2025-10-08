#pragma once

#include "z3dprimitiverenderer.h"
#include "z3drendercommands.h"
#include <memory>
#include <string>

namespace nim {

class Z3DTextureCopyRenderer : public Z3DPrimitiveRenderer
{
public:
  // Multiply_Alpha : output color will be multiplied by alpha value (convert to premultiplied format)
  // None (default): just copy
  // Divide_By_Alpha : output color will be divided by alpha value (input is premultiplied format)
  enum class OutputColorOption
  {
    NoChange,
    DivideByAlpha,
    MultiplyAlpha
  };

  explicit Z3DTextureCopyRenderer(Z3DRendererBase& rendererBase, OutputColorOption mode = OutputColorOption::NoChange);

  // colorTex may be null to clear
  void setColorTexture(/*nullable*/ const Z3DTexture* colorTex)
  {
    m_colorTexture = colorTex;
    m_colorAttachmentHandle = {};
  }

  // depthTex may be null to clear
  void setDepthTexture(/*nullable*/ const Z3DTexture* depthTex)
  {
    m_depthTexture = depthTex;
    m_depthAttachmentHandle = {};
  }

  void setSourceAttachments(AttachmentHandle colorHandle, AttachmentHandle depthHandle)
  {
    m_colorAttachmentHandle = colorHandle;
    m_depthAttachmentHandle = depthHandle;
  }

  // if true, color with zero alpha value should be discarded, which might save many depth texture lookup. default is
  // true Make sure your color and depth buffer are cleared before if set to true glClear + discard transparent  is
  // usually faster than   not discard transparent if many pixels are empty
  void setDiscardTransparent(bool v)
  {
    m_discardTransparent = v;
  }

  void setFlipY(bool v)
  {
    m_flipY = v;
  }

protected:
  void compile() override;

  [[nodiscard]] std::string generateHeader() const;

  void render(Z3DEye eye) override;

  [[nodiscard]] TextureCopyPayload buildTextureCopyPayload() const;
  [[nodiscard]] RenderBatch buildRenderBatch(Z3DEye eye) const;

  void enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking) override;
  void renderVulkan(Z3DEye eye, AttachmentHandle colorHandle, AttachmentHandle depthHandle);

protected:
  const Z3DTexture* m_colorTexture = nullptr;
  const Z3DTexture* m_depthTexture = nullptr;
  AttachmentHandle m_colorAttachmentHandle;
  AttachmentHandle m_depthAttachmentHandle;

  void createResources(RenderBackend backend) override;

  void destroyResources() override;

  std::unique_ptr<Z3DShaderGroup> m_copyTextureShaderGrp;
  bool m_discardTransparent = true;
  bool m_flipY = false;

  OutputColorOption m_mode;
  std::unique_ptr<Z3DVertexArrayObject> m_VAO;
};

} // namespace nim
