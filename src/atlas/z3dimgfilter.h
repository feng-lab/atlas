#ifndef Z3DIMGFILTER_H
#define Z3DIMGFILTER_H

#include "z3dboundedfilter.h"

#include "z3dcameraparameter.h"
#include "znumericparameter.h"
#include "z3dvolume.h"
#include "z3dtransformparameter.h"
#include "zmesh.h"
#include "zwidgetsgroup.h"
#include <vector>
#include "z3dvolumeraycasterrenderer.h"
#include "z3dvolumeslicerenderer.h"
#include "z3dtextureandeyecoordinaterenderer.h"
#include "z3dimage2drenderer.h"
#include "zeventlistenerparameter.h"
#include "z3dtexturecopyrenderer.h"
#include "zimgpack.h"
#include "z3drenderport.h"

namespace nim {

class ZImg;
class ZMesh;

class Z3DImgFilter : public Z3DBoundedFilter
{
  Q_OBJECT
  friend class Z3DCompositor;
public:
  explicit Z3DImgFilter(Z3DGlobalParameters &globalParas, QObject *parent = nullptr);
  ~Z3DImgFilter();

  void setVisible(bool v) { m_visible.set(v); }
  void setOffset(double x, double y, double z);

  void setData(const ZImgPack &img);

  virtual bool isStayOnTop() const { return m_stayOnTop.get(); }
  virtual void setStayOnTop(bool s) { m_stayOnTop.set(s); }

  // input volPos should be in volume coordinate
  // means it is in range [0 width-1 0 height-1 0 depth-1]
  bool openZoomInView(const glm::ivec3 &volPos);
  void exitZoomInView();

  std::vector<double> zoomInBound() const { return m_zoomInBound; }

  bool volumeNeedDownsample() const;
  bool isVolumeDownsampled() const;
  bool isSubvolume() const { return m_isSubVolume.get(); }

  std::shared_ptr<ZWidgetsGroup> widgetsGroup();

  virtual void enterInteractionMode() override;
  virtual void exitInteractionMode() override;

  bool isReady(Z3DEye eye) const override;

  // get salient 3d position hit by 2d point
  // check success before using the returned value
  // if first hit 3d position is in volume, success will be true,
  // otherwise don't use the returned value
  glm::vec3 get3DPosition(int x, int y, int width, int height, bool &success);

  virtual bool hasOpaque(Z3DEye eye) const override;
  virtual void renderOpaque(Z3DEye eye) override;
  virtual bool hasTransparent(Z3DEye eye) const override;
  virtual void renderTransparent(Z3DEye eye) override;

signals:
  void pointInVolumeLeftClicked(QPoint pt, glm::ivec3 pos3D);
  void pointInVolumeRightClicked(QPoint pt, glm::ivec3 pos3D);

protected slots:
  void changeCoordTransform();
  void changeZoomInViewSize();

  void adjustWidget();
  void leftMouseButtonPressed(QMouseEvent *e, int w, int h);

  void invalidateFRVolumeZSlice();
  void invalidateFRVolumeYSlice();
  void invalidateFRVolumeXSlice();
  void invalidateFRVolumeZSlice2();
  void invalidateFRVolumeYSlice2();
  void invalidateFRVolumeXSlice2();

  void updateCubeSerieSlices();

  virtual void setClipPlanes() override {}

protected:
  virtual void process(Z3DEye eye) override;

  const std::vector<std::unique_ptr<Z3DVolume>>& getVolumes() const;

  virtual void updateNotTransformedBoundBoxImpl() override;
  virtual void expandCutRange() override {}

private:
  void readVolumes();
  void readSubVolumes(int left, int right, int up, int down, int front, int back);

  // check success before using the returned value
  // if first hit 3d position is in volume, success will be true,
  // otherwise don't use the returned value
  glm::vec3 getFirstHit3DPosition(int x, int y, int width, int height, bool &success);
  // use first channel intensity
  glm::vec3 getMaxInten3DPositionUnderScreenPoint(int x, int y, int width, int height, bool &success);
  //get 3D position from 2D screen position
  glm::vec3 get3DPosition(glm::ivec2 pos2D, int width, int height, Z3DRenderOutputPort &port);
  //get 3D position from 2D screen position and depth
  glm::vec3 get3DPosition(glm::ivec2 pos2D, double depth, int width, int height);

  // based on context, prepare minimum necessary data and send to raycasterrenderer
  void prepareDataForRaycaster(Z3DVolume *volume, Z3DEye eye);

  void invalidateAllFRVolumeSlices();

  void volumeChanged();

  Z3DVolumeRaycasterRenderer m_volumeRaycasterRenderer;
  Z3DVolumeSliceRenderer m_volumeSliceRenderer;
  Z3DTextureAndEyeCoordinateRenderer m_textureAndEyeCoordinateRenderer;
  std::vector<std::unique_ptr<Z3DImage2DRenderer>> m_image2DRenderers;
  Z3DTextureCopyRenderer m_textureCopyRenderer;

  const ZImgPack *m_imgPack;
  std::vector<std::unique_ptr<Z3DVolume>> m_volumes;
  std::vector<std::unique_ptr<Z3DVolume>> m_zoomInVolumes;
  ZBoolParameter m_visible;
  ZBoolParameter m_stayOnTop;
  ZBoolParameter m_isVolumeDownsampled;
  ZBoolParameter m_isSubVolume;
  ZIntParameter m_zoomInViewSize;
  glm::ivec3 m_zoomInPos;
  std::vector<double> m_zoomInBound;

  size_t m_maxVoxelNumber;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  size_t m_numParas;

  ZIntParameter m_interactionDownsample;      // screen space downsample during interaction

  Z3DRenderTarget m_entryTarget;
  Z3DRenderTarget m_exitTarget;

  Z3DRenderOutputPort m_outport;
  Z3DRenderOutputPort m_leftEyeOutport;
  Z3DRenderOutputPort m_rightEyeOutport;
  Z3DFilterOutputPort<Z3DImgFilter> m_vPPort;

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
  std::vector<std::unique_ptr<ZColorMapParameter>> m_sliceColormaps;
  ZBoolParameter m_showXSlice2;
  ZIntParameter m_xSlice2Position;
  ZBoolParameter m_showYSlice2;
  ZIntParameter m_ySlice2Position;
  ZBoolParameter m_showZSlice2;
  ZIntParameter m_zSlice2Position;

  ZEventListenerParameter m_leftMouseButtonPressEvent;
  glm::ivec2 m_startCoord;

  ZMesh m_2DImageQuad;

  std::map<std::string, ZMesh> m_cubeSerieSlices;

  double m_imgMinIntensity;
  double m_imgMaxIntensity;

  size_t m_nChannels;
};

} // namespace nim

#endif // Z3DIMGFILTER_H
