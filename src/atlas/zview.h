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

namespace nim {

class ZDoc;

class ZGraphicsScene;

class ZGraphicsView;

class ZObjView;

class ZActionGroup;

class ZROIPack;

class ZRegionAnnotationPack;

class ZIntParameter;

class ZBoolParameter;

class ZDVec4Parameter;

class ZView : public QWidget, public ZViewSettingInterface
{
Q_OBJECT
public:
  enum class State
  {
    Normal, ROIRect, ROIEllipse, ROIPolygon, ROISpline, ROIFFPolygon
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

  inline QAction* fitIntoWindowAction()
  { return m_fitIntoWindowAction; }

  inline QAction* scrollHandDragAction()
  { return m_scrollHandDragAction; }

  inline QAction* rubberBandDragAction()
  { return m_rubberBandDragAction; }

  QToolButton* createROIToolButton(QWidget* parent);

  inline bool isNormalView() const
  { return m_normalViewAction->isChecked(); }

  inline bool isMaxZProjView() const
  { return m_maxZProjViewAction->isChecked(); }

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

  ZBoolParameter& mipPara()
  { return *m_mip; }

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

  ZROIPack& roiPack();

  ZRegionAnnotationPack& regionAnnotationPack();

  State state() const;

  QWidget* captureWidget();

  void updateViewSize();

  void updateBoundBox();

  // will show on label
  void setInfo(double x, double y);

  void registerObjView(std::unique_ptr<ZObjView>&& v);

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroupOf(size_t id) override;

  void read(size_t id, const QJsonObject& json);

  void write(size_t id, QJsonObject& json) const;

  void read(const QJsonObject& json);

  void write(QJsonObject& json) const;

  void fitContentIntoWindow();

  void gotoPosition(double x, double y, double z, double radius = 128.);

  int minViewPrecedence() const;

  int maxViewPrecedence() const;

  void copy();

  void pasteHere(int slice, QPointF point, bool hFlip = false, bool vFlip = false);

  void paste();

signals:

  void objViewReady(size_t id);

  void viewportChanged();

protected:
  void keyPressEvent(QKeyEvent* e) override;

private:
  void sliceChanged();

  void zoomIn();

  void zoomOut();

  void triggerNormalView(bool v);

  void triggerMaxZProjView(bool v);

  void changeViewStyle(bool mip);

  void changeViewport();

  void takeFixedSizeScreenShot(QString filename, int width, int height);

  void takeScreenShot(QString filename);

  void mousePressed(QPointF scenePos);

  void mouseMoved(QPointF scenePos);

  void mouseReleased(QPointF scenePos);

  void selectionChanged();

  void setViewDragMode(QAction* act);

  void createActions();

  void updateViewportPara() const;

private:
  ZDoc& m_doc;
  ZGraphicsScene* m_scene;

  QVBoxLayout* m_layout;
  QLabel* m_label;
  ZGraphicsView* m_view;
  ZIntParameter* m_imgSlice;
  ZIntParameter* m_imgTime;
  QWidget* m_imgSliceWidget;
  QWidget* m_imgTimeWidget;
  ZBoolParameter* m_mip;
  mutable ZDVec4Parameter* m_viewport;

  //
  QAction* m_copyAction;
  QAction* m_pasteAction;
  // QAction* m_deleteAction;

  QAction* m_zoomInAction;
  QAction* m_zoomOutAction;
  QActionGroup* m_imgViewStyleActionGroup;
  QAction* m_normalViewAction;
  QAction* m_maxZProjViewAction;
  QAction* m_fitIntoWindowAction;
  //
  QActionGroup* m_dragModeActionGroup;
  QAction* m_rubberBandDragAction;
  QAction* m_scrollHandDragAction;
  //
  ZActionGroup* m_roiStyleActionGroup;
  QAction* m_roiRectangleAction;
  QAction* m_roiEllipseAction;
  QAction* m_roiPolygonAction;
  QAction* m_roiSplineAction;
  QAction* m_roiFFPolygonAction;

  bool m_doNotReceiveSliceSignal;
  ZBBox<glm::ivec4> m_boundBox;

  std::vector<std::unique_ptr<ZObjView>> m_objViews;

  size_t m_numObjsBefore;
};

} // namespace nim

