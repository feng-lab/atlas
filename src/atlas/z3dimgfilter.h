#pragma once

#include "z3dboundedfilter.h"
#include "znumericparameter.h"
#include "zoptionparameter.h"
#include "z3dimg.h"
#include "z3dtransferfunction.h"
#include "zwidgetsgroup.h"
#include "z3dimgraycasterrenderer.h"
#include "z3dimgslicerenderer.h"
#include "zeventlistenerparameter.h"
#include "z3dtexturecopyrenderer.h"
#include "zimgpack.h"
#include "z3dscratchresourcepool.h"

#include <array>
#include <vector>

namespace nim {

class ZImg;

class Z3DImgFilter : public Z3DBoundedFilter
{
  Q_OBJECT

  friend class Z3DCompositor;

public:
  explicit Z3DImgFilter(Z3DGlobalParameters& globalParas, QObject* parent = nullptr);

  void setData(const ZImgPack& imgPack);

  [[nodiscard]] virtual bool isStayOnTop() const
  {
    return m_stayOnTop.get();
  }

  virtual void setStayOnTop(bool s)
  {
    m_stayOnTop.set(s);
  }

  std::shared_ptr<ZWidgetsGroup> widgetsGroup();

  [[nodiscard]] bool isReady(Z3DEye eye) const override;

  [[nodiscard]] bool hasOpaque(Z3DEye eye) const override;

  void renderOpaque(Z3DEye eye) override;

  [[nodiscard]] bool hasTransparent(Z3DEye eye) const override;

  void renderTransparent(Z3DEye eye) override;

  // get salient 3d position hit by 2d point
  // check success before using the returned value
  // if first hit 3d position is in volume, success will be true,
  // otherwise don't use the returned value
  glm::vec3 get3DPosition(int x, int y, int width, int height, bool& success);

  void setProgressiveRenderingMode(bool v) override;

  void enterSubregionView(float x, float y, float z);

  void exitSubregionView();

  void invalidate(State inv) override;

Q_SIGNALS:
  void showImgContextMenu(QPoint globalPos, float x, float y, float z, bool enter, bool exit);

protected:
  void switchRendererBackend(RenderBackend backend) override
  {
    Z3DBoundedFilter::switchRendererBackend(backend);
    if (!m_3dImg) {
      return;
    }
    if (backend == RenderBackend::Vulkan) {
      m_3dImg->releaseGLResources();
    } else {
      // Prepare GL paging resources proactively to avoid first-use hazards
      m_3dImg->rebuildGLPagingResources();
    }
  }
  void updateSize(const glm::uvec2& targetSize) override;

  void changeCoordTransform();

  void adjustWidget();

  void fullResolutionRenderingToggled();

  void leftMouseButtonPressed(QMouseEvent* e, int w, int h);

  void contextMenuEvent(QContextMenuEvent* e, int w, int h);

  //  void invalidateFRVolumeZSlice();
  //  void invalidateFRVolumeYSlice();
  //  void invalidateFRVolumeXSlice();
  //  void invalidateFRVolumeZSlice2();
  //  void invalidateFRVolumeYSlice2();
  //  void invalidateFRVolumeXSlice2();

  void setClipPlanes() override {}

  //  void enterFastMode();
  //
  //  void exitFastMode();

  double process(Z3DEye eye) override;

  [[nodiscard]] bool hasSlices() const;

  double renderSlices(Z3DEye eye);

  [[nodiscard]] bool hasImage() const;

  double renderImage(Z3DEye eye);

  [[nodiscard]] bool onlyBoundBox() const;

  void renderOnlyBoundBox(Z3DEye eye);

  void updateNotTransformedBoundBoxImpl() override;

  void expandCutRange() override {}

private:
  // void invalidateAllFRVolumeSlices();
  void updateBlockIDTarget();

  void volumeChanged();

  void channelRangeChanged();

  void updateRaycasterCompositingMode();
  void updateRaycasterSamplingRate();
  void updateRaycasterIsoValue();
  void updateRaycasterLocalMIPThreshold();
  void onVisibilityChanged(bool visible);
  [[nodiscard]] Z3DRenderTarget& transparentTarget(Z3DEye eye);
  [[nodiscard]] const Z3DScratchResourcePool::RenderTargetLease& transparentLease(Z3DEye eye) const
  {
    return m_transparentTargets[eye];
  }

  [[nodiscard]] const Z3DScratchResourcePool::RenderTargetLease& opaqueLease(Z3DEye eye) const
  {
    return m_opaqueTargets[eye];
  }
  [[nodiscard]] Z3DRenderTarget& opaqueTarget(Z3DEye eye);
  [[nodiscard]] Z3DRenderTarget& ensureRenderTarget(Z3DScratchResourcePool::RenderTargetLease& lease);
  void releaseAllRenderTargets();
  void markTargetsInvalid();

  // check success before using the returned value
  // if first hit 3d position is in volume, success will be true,
  // otherwise don't use the returned value
  glm::vec3 getFirstHit3DPosition(int x, int y, int width, int height, bool& success);

  // use first channel intensity
  glm::vec3 getMaxInten3DPositionUnderScreenPoint(int x, int y, int width, int height, bool& success);

  // get 3D position from 2D screen position
  glm::vec3 get3DPosition(glm::ivec2 pos2D, int width, int height, Z3DRenderTarget& target);

  // get 3D position from 2D screen position and depth
  glm::vec3 get3DPosition(glm::ivec2 pos2D, double depth, int width, int height);

 private:
  Z3DImgRaycasterRenderer m_imgRaycasterRenderer;
  Z3DImgSliceRenderer m_imgSliceRenderer;
  Z3DTextureCopyRenderer m_textureCopyRenderer;

  // Optional adapter pack used when 3D rendering needs a different view of the source
  // (e.g. Neuroglancer uint64 segmentation shown as an RGB volume for visualization).
  // Must outlive m_3dImg since Z3DImg holds a reference to its ZImgPack.
  std::unique_ptr<ZImgPack> m_imgPackOverride;
  // Tracks whether we auto-initialized coordTransform.scale() from voxel-size aspect ratios.
  // If the user later changes the scale, we stop overriding it on subsequent dataset loads.
  bool m_hasAutoVoxelAspectScale = false;
  glm::vec3 m_autoVoxelAspectScale{1.f, 1.f, 1.f};
  std::unique_ptr<Z3DImg> m_3dImg;
  ZBoolParameter m_stayOnTop;
  ZBoolParameter m_fullResolutionRendering;
  ZStringIntOptionParameter m_raycasterCompositingMode;
  ZFloatParameter m_raycasterSamplingRate;
  ZFloatParameter m_raycasterIsoValue;
  ZFloatParameter m_raycasterLocalMIPThreshold;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  size_t m_numParas;

  // ZIntParameter m_interactionDownsample;      // screen space downsample during interaction
  // ZBoolParameter m_smoothInteraction;

  std::array<Z3DScratchResourcePool::RenderTargetLease, 3> m_transparentTargets;
  std::array<Z3DScratchResourcePool::RenderTargetLease, 3> m_opaqueTargets;
  std::array<bool, 3> m_transparentValid{};
  std::array<bool, 3> m_opaqueValid{};
  glm::uvec2 m_outputSize{32u, 32u};

  std::vector<std::unique_ptr<ZDoubleSpanParameter>> m_doubleChannelRangeParas;
  std::vector<std::unique_ptr<ZBoolParameter>> m_channelVisibleParas;
  std::vector<std::unique_ptr<Z3DTransferFunctionParameter>> m_transferFuncParas;

  ZBoolParameter m_showXSlice;
  ZIntParameter m_xSlicePosition;
  ZBoolParameter m_showYSlice;
  ZIntParameter m_ySlicePosition;
  ZBoolParameter m_showZSlice;
  ZIntParameter m_zSlicePosition;
  std::vector<std::unique_ptr<ZColorMapParameter>> m_sliceColormaps;

  ZBoolParameter m_showObliqueSlice;
  ZVec3Parameter m_obliqueSliceNormal;
  ZFloatParameter m_obliqueSliceDistanceToOrigin;

  ZBoolParameter m_showObliqueSlice2;
  ZVec3Parameter m_obliqueSlice2Normal;
  ZFloatParameter m_obliqueSlice2DistanceToOrigin;

  ZBoolParameter m_showXSlice2;
  ZIntParameter m_xSlice2Position;
  ZBoolParameter m_showYSlice2;
  ZIntParameter m_ySlice2Position;
  ZBoolParameter m_showZSlice2;
  ZIntParameter m_zSlice2Position;

  ZEventListenerParameter m_leftMouseButtonPressEvent;
  ZEventListenerParameter m_contextMenuEvent;
  glm::ivec2 m_startCoord{};

  bool m_channelRangeChanged = false;

  bool m_progressiveRendering = false;

  // Defer renderer progress reset to the start of process() to avoid
  // mutating renderer state mid-pass when invalidation arrives.
  bool m_resetProgressPending = false;

  // Cache effective global cuts intersected with this filter's world AABB to avoid redundant redraws
  bool m_cachedGlobalCutsInitialized = false;
  glm::vec2 m_cachedEffXCut{}; // [low, high] after clamping to axisAlignedBoundBox()
  glm::vec2 m_cachedEffYCut{};
  glm::vec2 m_cachedEffZCut{};
};

} // namespace nim
