#pragma once

#include "z3dboundedfilter.h"
#include "z3dcameraparameter.h"
#include "znumericparameter.h"
#include "z3dimg.h"
#include "z3dtransformparameter.h"
#include "zwidgetsgroup.h"
#include "z3dimgraycasterrenderer.h"
#include "z3dimgslicerenderer.h"
#include "z3dtextureandeyecoordinaterenderer.h"
#include "z3dimage2drenderer.h"
#include "zeventlistenerparameter.h"
#include "z3dtexturecopyrenderer.h"
#include "zimgpack.h"
#include "z3drenderport.h"
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
  { return m_stayOnTop.get(); }

  virtual void setStayOnTop(bool s)
  { m_stayOnTop.set(s); }

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

protected:
  void updateSize() override;

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

  void setClipPlanes() override
  {}

  void mousePressed();

  void mouseReleased();

  void process(Z3DEye eye) override;

  [[nodiscard]] bool hasSlices() const;

  void renderSlices(Z3DEye eye);

  [[nodiscard]] bool hasImage() const;

  void renderImage(Z3DEye eye);

  [[nodiscard]] bool onlyBoundBox() const;

  void renderOnlyBoundBox(Z3DEye eye);

  void updateNotTransformedBoundBoxImpl() override;

  void expandCutRange() override
  {}

private:
  //void invalidateAllFRVolumeSlices();
  void updateBlockIDTarget();

  void volumeChanged();

  // check success before using the returned value
  // if first hit 3d position is in volume, success will be true,
  // otherwise don't use the returned value
  glm::vec3 getFirstHit3DPosition(int x, int y, int width, int height, bool& success);

  // use first channel intensity
  glm::vec3 getMaxInten3DPositionUnderScreenPoint(int x, int y, int width, int height, bool& success);

  //get 3D position from 2D screen position
  glm::vec3 get3DPosition(glm::ivec2 pos2D, int width, int height, Z3DRenderOutputPort& port);

  //get 3D position from 2D screen position and depth
  glm::vec3 get3DPosition(glm::ivec2 pos2D, double depth, int width, int height);

private:
  Z3DImgRaycasterRenderer m_imgRaycasterRenderer;
  Z3DImgSliceRenderer m_imgSliceRenderer;
  Z3DTextureAndEyeCoordinateRenderer m_textureAndEyeCoordinateRenderer;
  //std::vector<std::unique_ptr<Z3DImage2DRenderer>> m_image2DRenderers;
  Z3DTextureCopyRenderer m_textureCopyRenderer;

  std::unique_ptr<Z3DImg> m_3dImg;
  ZBoolParameter m_stayOnTop;
  ZBoolParameter m_fullResolutionRendering;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  size_t m_numParas;

  //ZIntParameter m_interactionDownsample;      // screen space downsample during interaction
  ZBoolParameter m_smoothInteraction;

  Z3DRenderTarget m_entryTarget;
  Z3DRenderTarget m_exitTarget;
  Z3DRenderTarget m_layerTarget;
  Z3DTexture m_layerColorTexture;
  Z3DTexture m_layerDepthTexture;
  Z3DTexture m_missBlocksTexture1;
  Z3DTexture m_missBlocksTexture2;
  Z3DTexture m_missBlocksTexture3;
  Z3DTexture m_missBlocksTexture4;
  Z3DTexture m_usedBlocksTexture1;
  Z3DTexture m_usedBlocksTexture2;
  Z3DTexture m_usedBlocksTexture3;
  Z3DTexture m_usedBlocksTexture4;
  Z3DRenderTarget m_blockIDsRenderTarget;

  Z3DRenderOutputPort m_outport;
  Z3DRenderOutputPort m_leftEyeOutport;
  Z3DRenderOutputPort m_rightEyeOutport;
  Z3DFilterOutputPort<Z3DImgFilter> m_vPPort;
  Z3DRenderOutputPort m_opaqueOutport;
  Z3DRenderOutputPort m_opaqueLeftEyeOutport;
  Z3DRenderOutputPort m_opaqueRightEyeOutport;

  //static const size_t m_maxNumOfFullResolutionVolumeSlice;
  // each channel is represented by a Z3DVolume
  //std::vector<std::vector<std::unique_ptr<Z3DVolume>>> m_FRVolumeSlices;
  //std::vector<bool> m_FRVolumeSlicesValidState;
  //ZBoolParameter m_useFRVolumeSlice;
  ZBoolParameter m_showXSlice;
  ZIntParameter m_xSlicePosition;
  ZBoolParameter m_showYSlice;
  ZIntParameter m_ySlicePosition;
  ZBoolParameter m_showZSlice;
  ZIntParameter m_zSlicePosition;
  std::vector<std::unique_ptr<ZColorMapParameter>> m_sliceColormaps;
  ZBoolParameter m_showXSlice2;
  ZIntParameter m_xSlice2Position;
  ZBoolParameter m_showYSlice2;
  ZIntParameter m_ySlice2Position;
  ZBoolParameter m_showZSlice2;
  ZIntParameter m_zSlice2Position;

  ZEventListenerParameter m_leftMouseButtonPressEvent;
  ZEventListenerParameter m_contextMenuEvent;
  glm::ivec2 m_startCoord{};
};

} // namespace nim

