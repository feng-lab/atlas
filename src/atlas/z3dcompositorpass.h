#pragma once

#include "z3drendercommands.h"
#include "z3drendererstates.h"

#include <vector>

namespace nim {

class Z3DBoundedFilter;
class Z3DTexture;
class Z3DScratchResourcePool;

struct Z3DCompositorImageLayer
{
  AttachmentDesc colorAttachment;
  AttachmentDesc depthAttachment;
  const Z3DTexture* glColorTexture = nullptr;
  const Z3DTexture* glDepthTexture = nullptr;
};

struct Z3DCompositorTransparentBatch
{
  Z3DBoundedFilter* filter = nullptr;
  bool glowEnabled = false;
};

struct Z3DCompositorPass
{
  enum class Kind
  {
    Geometry
  };

  Kind kind = Kind::Geometry;

  Z3DScratchResourcePool::RenderTargetLease* targetLease = nullptr;
  RendererFrameState::ActiveSurface surface;
  Z3DEye eye = MonoEye;
  TransparencyMode transparency = TransparencyMode::Unknown;
  GeometryMSAAMode msaaMode = GeometryMSAAMode::None;

  bool clearColor = true;
  bool clearDepth = true;
  bool clearStencil = false;
  ClearValue clearValue{};

  std::vector<Z3DBoundedFilter*> opaqueFilters;
  std::vector<Z3DCompositorTransparentBatch> transparentFilters;
  std::vector<Z3DCompositorImageLayer> imageLayers;
  const char* debugLabel = nullptr;
};

} // namespace nim
