#pragma once

#include "zviewsettinginterface.h"
#include "zglmutils.h"
#include "zbbox.h"
#include <QWidget>
#include <QAction>
#include <array>

class QVBoxLayout;

class QToolButton;

class QLabel;

class QActionGroup;

class QGraphicsScene;

namespace nim {

class ZDoc;

class ZGraphicsScene;

class ZGraphicsView;

class ZObjView;

class ZROIPack;

class ZRegionAnnotationPack;

class ZIntParameter;

class ZBoolParameter;

class ZDVec4Parameter;

class ZStringIntOptionParameter;

class ZView : public QWidget, public ZViewSettingInterface
{
Q_OBJECT
public:
  enum class State
  {
    Normal, ROIRect, ROIEllipse, ROIPolygon, ROISpline, ROICut
  };

  enum class ViewStyle
  {
    Normal, MIP, Montage
  };

  explicit ZView(ZDoc& doc, QWidget* parent = nullptr, Qt::WindowFlags f = Qt::Widget);

  ~ZView() override;

  inline QAction* copyAction()
  { return m_copyAction; }

  inline QAction* pasteAction()
  { return m_pasteAction; }

  inline QAction* zoomInAction()
  { return m_zoomInAction; }

  inline QAction* zoomOutAction()
  { return m_zoomOutAction; }

  QWidget* createScaleWidget(QWidget* parent);

  inline QAction* normalViewAction()
  { return m_normalViewAction; }

  inline QAction* maxZProjViewAction()
  { return m_maxZProjViewAction; }

  inline QAction* montageViewAction()
  { return m_montageViewAction; }

  inline QAction* fitIntoWindowAction()
  { return m_fitIntoWindowAction; }

  inline QAction* scrollHandDragAction()
  { return m_scrollHandDragAction; }

  inline QAction* rubberBandDragAction()
  { return m_rubberBandDragAction; }

  QToolButton* createROIToolButton(QWidget* parent);

  QWidget* createROIModeWidget(QWidget* parent);

  inline bool isMaxZProjView() const
  { return currentViewStyle() == ViewStyle::MIP; }

  int currentSlice() const;

  int currentTime() const;

  double currentScale() const;

  QRectF currentViewport() const;

  // how many slices are showed in current view, for max z proj, this contains all slices
  // range end  = last slice + 1, for normal view, range will be [current slice, current slice + 1]
  std::pair<int, int> currentSliceRange() const;

  ZIntParameter& slicePara()
  { return *m_imgSlice; }

  ZIntParameter& timePara()
  { return *m_imgTime; }

  ZStringIntOptionParameter& viewStylePara()
  { return *m_viewStyle; }

  ZDVec4Parameter& viewportPara()
  {
    updateViewportPara();
    return *m_viewport;
  }

  inline ZGraphicsScene& scene()
  { return *m_scene; }

  inline const ZGraphicsScene& scene() const
  { return *m_scene; }

  inline ZGraphicsView& graphicsView()
  { return *m_view; }

  inline const ZGraphicsView& graphicsView() const
  { return *m_view; }

  ZROIPack& roiPack(size_t id);

  ZRegionAnnotationPack& regionAnnotationPack(size_t id);

  ZROIPack& currentROIPack();

  ZRegionAnnotationPack& currentRegionAnnotationPack();

  State state() const;

  QWidget* captureWidget() const;

  void updateViewSize();

  void updateBoundBox();

  // will show on label
  void setInfo(double x, double y);

  void registerObjView(std::unique_ptr<ZObjView>&& v);

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroupOf(size_t id) override;

  QWidget* globalParasWidget();

  void read(size_t id, const json::object& json);

  void write(size_t id, json::object& json) const;

  void read(const json::object& json);

  void write(json::object& json) const;

  void fitContentIntoWindow();

  void gotoPosition(double x, double y, double z, double radius = 128.);

  int minViewPrecedence() const;

  int maxViewPrecedence() const;

  void copy();

  void pasteHere(int slice, QPointF point, bool hFlip = false, bool vFlip = false);

  void paste();

  void checkViewport();

  bool isRegionAnnotationMode() const;

  bool isROIMode() const;

  void estimateMontageColumns() const;

signals:

  void objViewReady(size_t id);

  void viewportChanged();

protected:
  void keyPressEvent(QKeyEvent* e) override;

private:
  ViewStyle currentViewStyle() const;

  void sliceChanged();

  void zoomIn();

  void zoomOut();

  void triggerNormalView(bool v);

  void triggerMaxZProjView(bool v);

  void triggerMontageView(bool v);

  void changeViewStyle();

  void changeViewport();

  void takeFixedSizeScreenShot(const QString& filename, int width, int height);

  void takeScreenShot(const QString& filename);

  void mousePressed(QPointF scenePos);

  void mouseMoved(QPointF scenePos);

  void mouseReleased(QPointF scenePos);

  void selectionChanged();

  void setViewDragMode(QAction* act);

  void createActions();

  void updateViewportPara() const;

  void updateSceneRectFromBoundBox();

  void updateMontageScene();

  void emptyFun();

private:
  ZDoc& m_doc;
  ZGraphicsScene* m_scene = nullptr;
  QGraphicsScene* m_montageScene = nullptr;

  QVBoxLayout* m_layout = nullptr;
  QLabel* m_label = nullptr;
  ZGraphicsView* m_view = nullptr;
  ZIntParameter* m_imgSlice = nullptr;
  ZIntParameter* m_imgTime = nullptr;
  QWidget* m_imgSliceWidget = nullptr;
  QWidget* m_imgTimeWidget = nullptr;
  ZStringIntOptionParameter* m_viewStyle = nullptr;
  ZIntParameter* m_montageColumns = nullptr;
  mutable ZDVec4Parameter* m_viewport = nullptr;

  //
  QAction* m_copyAction = nullptr;
  QAction* m_pasteAction = nullptr;
  // QAction* m_deleteAction = nullptr;

  QAction* m_zoomInAction = nullptr;
  QAction* m_zoomOutAction = nullptr;
  QActionGroup* m_imgViewStyleActionGroup = nullptr;
  QAction* m_normalViewAction = nullptr;
  QAction* m_maxZProjViewAction = nullptr;
  QAction* m_montageViewAction = nullptr;
  QAction* m_fitIntoWindowAction = nullptr;
  //
  QActionGroup* m_dragModeActionGroup = nullptr;
  QAction* m_rubberBandDragAction = nullptr;
  QAction* m_scrollHandDragAction = nullptr;
  //
  QActionGroup* m_roiStyleActionGroup = nullptr;
  QAction* m_roiRectangleAction = nullptr;
  QAction* m_roiEllipseAction = nullptr;
  QAction* m_roiPolygonAction = nullptr;
  QAction* m_roiSplineAction = nullptr;
  QAction* m_roiCutLineAction = nullptr;

  ZStringIntOptionParameter* m_roiMode = nullptr;

  bool m_doNotReceiveSliceSignal = false;
  ZBBox<glm::ivec4> m_boundBox;

  std::vector<std::unique_ptr<ZObjView>> m_objViews;

  size_t m_numObjsBefore = 0;

  int m_montageZ = 0;
};

} // namespace nim

