#pragma once

#include "z3dboundedfilter.h"

#include "z3dcameraparameter.h"
#include "znumericparameter.h"
#include "z3dvolume.h"
#include "z3dtransformparameter.h"
#include "zmesh.h"
#include "zwidgetsgroup.h"
#include "zoptionparameter.h"
#include <array>
#include <vector>
#include "z3dvolumeraycasterrenderer.h"
#include "z3dtransferfunction.h"
#include "z3dvolumeslicerenderer.h"
#include "z3dtextureandeyecoordinaterenderer.h"
#include "z3dimage2drenderer.h"
#include "zeventlistenerparameter.h"
#include "z3dtexturecopyrenderer.h"
#include "zimgpack.h"
#include "z3dscratchresourcepool.h"

namespace nim {

class ZImg;

class ZMesh;

class Z3DVolumeFilter : public Z3DBoundedFilter
{
  Q_OBJECT

  friend class Z3DCompositor;

public:
  explicit Z3DVolumeFilter(Z3DGlobalParameters& globalParas, QObject* parent = nullptr);

  void setData(const ZImgPack& img);

  [[nodiscard]] virtual bool isStayOnTop() const
  {
    return m_stayOnTop.get();
  }

  virtual void setStayOnTop(bool s)
  {
    m_stayOnTop.set(s);
  }

  // input volPos should be in volume coordinate
  // means it is in range [0 width-1 0 height-1 0 depth-1]
  bool openZoomInView(const glm::ivec3& volPos);

  void exitZoomInView();

  [[nodiscard]] const ZBBox<glm::dvec3>& zoomInBound() const
  {
    return m_zoomInBound;
  }

  [[nodiscard]] bool volumeNeedDownsample() const;

  [[nodiscard]] bool isVolumeDownsampled() const;

  [[nodiscard]] bool isSubvolume() const
  {
    return m_isSubVolume.get();
  }

  std::shared_ptr<ZWidgetsGroup> widgetsGroup();

  void enterInteractionMode() override;

  void exitInteractionMode() override;

  [[nodiscard]] bool isReady(Z3DEye eye) const override;

  // get salient 3d position hit by 2d point
  // check success before using the returned value
  // if first hit 3d position is in volume, success will be true,
  // otherwise don't use the returned value
  glm::vec3 get3DPosition(int x, int y, int width, int height, bool& success);

  [[nodiscard]] bool hasOpaque(Z3DEye eye) const override;

  void renderOpaque(Z3DEye eye) override;

  [[nodiscard]] bool hasTransparent(Z3DEye eye) const override;

  void renderTransparent(Z3DEye eye) override;

Q_SIGNALS:
  void pointInVolumeLeftClicked(QPoint pt, glm::ivec3 pos3D);

  void pointInVolumeRightClicked(QPoint pt, glm::ivec3 pos3D);

protected:
  void changeCoordTransform();

  void changeZoomInViewSize();

  void adjustWidget();

  void leftMouseButtonPressed(QMouseEvent* e, int w, int h);

  void invalidateFRVolumeZSlice();

  void invalidateFRVolumeYSlice();

  void invalidateFRVolumeXSlice();

  void invalidateFRVolumeZSlice2();

  void invalidateFRVolumeYSlice2();

  void invalidateFRVolumeXSlice2();

  void updateCubeSerieSlices();

  void setClipPlanes() override {}

protected:
  void invalidate(State inv) override;
  void process(Z3DEye eye) override;

  [[nodiscard]] bool hasSlices() const;

  void renderSlices(Z3DEye eye);

  [[nodiscard]] const std::vector<std::unique_ptr<Z3DVolume>>& getVolumes() const;

  void updateNotTransformedBoundBoxImpl() override;

  void updateSize() override;
  void expandCutRange() override {}

private:
  void readVolumes();

  void readSubVolumes(int left, int right, int up, int down, int front, int back);

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

  // based on context, prepare minimum necessary data and send to raycasterrenderer
  void prepareDataForRaycaster(Z3DVolume* volume, Z3DEye eye);

  void invalidateAllFRVolumeSlices();

  void volumeChanged();

  void updateRaycasterCompositingMode();
  void updateRaycasterSamplingRate();
  void updateRaycasterIsoValue();
  void updateRaycasterLocalMIPThreshold();
  [[nodiscard]] static size_t eyeIndex(Z3DEye eye);
  [[nodiscard]] Z3DRenderTarget& transparentTarget(Z3DEye eye);
  [[nodiscard]] const Z3DRenderTarget& transparentTarget(Z3DEye eye) const;
  [[nodiscard]] Z3DRenderTarget& opaqueTarget(Z3DEye eye);
  [[nodiscard]] const Z3DRenderTarget& opaqueTarget(Z3DEye eye) const;
  [[nodiscard]] Z3DRenderTarget& ensureRenderTarget(Z3DScratchResourcePool::RenderTargetLease& lease);
  void releaseAllRenderTargets();
  void markTargetsInvalid();

  Z3DVolumeRaycasterRenderer m_volumeRaycasterRenderer;
  Z3DVolumeSliceRenderer m_volumeSliceRenderer;
  Z3DTextureAndEyeCoordinateRenderer m_textureAndEyeCoordinateRenderer;
  std::vector<std::unique_ptr<Z3DImage2DRenderer>> m_image2DRenderers;
  Z3DTextureCopyRenderer m_textureCopyRenderer;

  ZStringIntOptionParameter m_raycasterCompositingMode;
  ZFloatParameter m_raycasterSamplingRate;
  ZFloatParameter m_raycasterIsoValue;
  ZFloatParameter m_raycasterLocalMIPThreshold;

  const ZImgPack* m_imgPack;
  std::vector<std::unique_ptr<Z3DVolume>> m_volumes;
  std::vector<std::unique_ptr<Z3DVolume>> m_zoomInVolumes;
  ZBoolParameter m_stayOnTop;
  ZBoolParameter m_isVolumeDownsampled;
  ZBoolParameter m_isSubVolume;
  ZIntParameter m_zoomInViewSize;
  glm::ivec3 m_zoomInPos{};
  ZBBox<glm::dvec3> m_zoomInBound;

  size_t m_maxVoxelNumber;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  size_t m_numParas;

  ZIntParameter m_interactionDownsample; // screen space downsample during interaction

  Z3DRenderTarget m_entryTarget;
  Z3DRenderTarget m_exitTarget;
  Z3DRenderTarget m_layerTarget;
  Z3DTexture m_layerColorTexture;
  Z3DTexture m_layerDepthTexture;

  Z3DFilterOutputPort<Z3DVolumeFilter> m_vPPort;
  std::array<Z3DScratchResourcePool::RenderTargetLease, 3> m_transparentTargets;
  std::array<Z3DScratchResourcePool::RenderTargetLease, 3> m_opaqueTargets;
  std::array<bool, 3> m_transparentValid{};
  std::array<bool, 3> m_opaqueValid{};
  glm::uvec2 m_outputSize{32u, 32u};
  glm::uvec2 m_interactionBaseSize{0u, 0u};
  bool m_interactionDownsampleActive = false;

  static const size_t m_maxNumOfFullResolutionVolumeSlice;
  // each channel is represented by a Z3DVolume
  std::vector<std::vector<std::unique_ptr<Z3DVolume>>> m_FRVolumeSlices;
  std::vector<bool> m_FRVolumeSlicesValidState;
  ZBoolParameter m_useFRVolumeSlice;
  ZBoolParameter m_showXSlice;
  ZIntParameter m_xSlicePosition;
  ZBoolParameter m_showYSlice;
  ZIntParameter m_ySlicePosition;
  ZBoolParameter m_showZSlice;
  ZIntParameter m_zSlicePosition;
  std::vector<std::unique_ptr<ZBoolParameter>> m_channelVisibleParas;
  std::vector<std::unique_ptr<Z3DTransferFunctionParameter>> m_transferFuncParas;
  std::vector<std::unique_ptr<ZStringIntOptionParameter>> m_texFilterModeParas;
  std::vector<std::unique_ptr<ZColorMapParameter>> m_sliceColormaps;
  ZBoolParameter m_showXSlice2;
  ZIntParameter m_xSlice2Position;
  ZBoolParameter m_showYSlice2;
  ZIntParameter m_ySlice2Position;
  ZBoolParameter m_showZSlice2;
  ZIntParameter m_zSlice2Position;

  ZEventListenerParameter m_leftMouseButtonPressEvent;
  glm::ivec2 m_startCoord{};

  ZMesh m_2DImageQuad;

  std::map<std::string, ZMesh> m_cubeSerieSlices;

  double m_imgMinIntensity{};
  double m_imgMaxIntensity{};

  size_t m_nChannels;
};

} // namespace nim
