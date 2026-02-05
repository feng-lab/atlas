#pragma once

#include "z3drendercommands.h"

namespace nim {

class Z3DTexture;

struct Z3DCompositorImageLayer
{
  AttachmentDesc colorAttachment;
  AttachmentDesc depthAttachment;
  const Z3DTexture* glColorTexture = nullptr;
  const Z3DTexture* glDepthTexture = nullptr;
};

} // namespace nim
