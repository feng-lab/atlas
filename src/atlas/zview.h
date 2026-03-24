#pragma once

#include "zviewsettinginterface.h"
#include "zglmutils.h"
#include "zbbox.h"
#include "zjson.h"
#include <QWidget>
#include <QAction>
#include <array>

class QVBoxLayout;

class QToolButton;

class QLabel;

class QActionGroup;

class QGraphicsScene;

class QProgressDialog;

class QMenu;

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

class ZView
  : public QWidget
  , public ZViewSettingInterface
{
  Q_OBJECT

public:
  enum class State
  {
    Normal,
    ROIRect,
    ROIEllipse,
    ROIPolygon,
    ROISpline,
    ROICut
  };

  enum class ViewStyle
  {
    Normal,
    MIP,
    Montage
  };

  explicit ZView(ZDoc& doc, QWidget* parent = nullptr, Qt::WindowFlags f = Qt::Widget);

  ~ZView() override;

  QAction* copyAction()
  {
    return m_copyAction;
  }

  QAction* pasteAction()
  {
    return m_pasteAction;
  }

  QAction* zoomInAction()
  {
    return m_zoomInAction;
  }

  QAction* zoomOutAction()
  {
    return m_zoomOutAction;
  }

  QWidget* createScaleWidget(QWidget* parent);

  QAction* normalViewAction()
  {
    return m_normalViewAction;
  }

  QAction* maxZProjViewAction()
  {
    return m_maxZProjViewAction;
  }

  QAction* montageViewAction()
  {
    return m_montageViewAction;
  }

  QAction* fitIntoWindowAction()
  {
    return m_fitIntoWindowAction;
  }

  QAction* scrollHandDragAction()
  {
    return m_scrollHandDragAction;
  }

  QAction* rubberBandDragAction()
  {
    return m_rubberBandDragAction;
  }

  QToolButton* createROIToolButton(QWidget* parent);

  QWidget* createROIModeWidget(QWidget* parent);

  bool isMaxZProjView() const
  {
    return currentViewStyle() == ViewStyle::MIP;
  }

  ZDoc& doc()
  {
    return m_doc;
  }

  [[nodiscard]] const ZDoc& doc() const
  {
    return m_doc;
  }

  int currentSlice() const;

  int currentTime() const;

  double currentScale() const;

  QRectF currentViewport() const;

  // how many slices are showed in current view, for max z proj, this contains all slices
  // range end  = last slice + 1, for normal view, range will be [current slice, current slice + 1]
  std::pair<int, int> currentSliceRange() const;

  ZIntParameter& slicePara()
  {
    return *m_imgSlice;
  }

  ZIntParameter& timePara()
  {
    return *m_imgTime;
  }

  ZStringIntOptionParameter& viewStylePara()
  {
    return *m_viewStyle;
  }

  ZDVec4Parameter& viewportPara()
  {
    updateViewportPara();
    return *m_viewport;
  }

  ZGraphicsScene& scene()
  {
    return *m_scene;
  }

  const ZGraphicsScene& scene() const
  {
    return *m_scene;
  }

  ZGraphicsView& graphicsView()
  {
    return *m_view;
  }

  const ZGraphicsView& graphicsView() const
  {
    return *m_view;
  }

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

  // Allows registered object views to contribute actions to the 2D right-click context menu at a scene position.
  void appendContextMenuActions(QMenu& menu, QPointF scenePos, Qt::KeyboardModifiers modifiers);

  void checkViewport();

  bool isRegionAnnotationMode() const;

  bool isROIMode() const;

  [[nodiscard]] bool isTraceToolEnabled() const;

  void estimateMontageColumns() const;

  void takeFixedSizeScreenShot(const QString& filename, int width, int height);

  void takeScreenShot(const QString& filename);

  // For deterministic 2D animation export: waits (processing events) until all object views report
  // their async work complete for the current frame. If progress is non-null, supports cancellation.
  [[nodiscard]] bool waitFor2DExportFrameReady(QProgressDialog* progress, QString* errorMsg);

Q_SIGNALS:
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

  void mousePressed(QPointF scenePos, Qt::KeyboardModifiers modifiers);

  void mouseMoved(QPointF scenePos, Qt::KeyboardModifiers modifiers);

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
