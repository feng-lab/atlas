#include "zview.h"

#include "zdoc.h"
#include "zgraphicsscene.h"
#include "zgraphicsview.h"
#include "zobjview.h"
#include "zactiongroup.h"
#include "znumericparameter.h"
#include "zroidoc.h"
#include "ztakescreenshotwidget.h"
#include "zlog.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QActionGroup>
#include <QToolButton>
#include <QMessageBox>
#include <QApplication>

namespace nim {

ZView::ZView(ZDoc& doc, QWidget* parent, Qt::WindowFlags f)
  : QWidget(parent, f)
  , m_doc(doc)
  , m_doNotReceiveSliceSignal(false)
  , m_numObjsBefore(m_doc.numObjs())
{
  m_imgSlice = new ZIntParameter("slice", 0, 0, 0, this);
  m_imgTime = new ZIntParameter("time", 0, 0, 0, this);

  m_mip = new ZBoolParameter("MIP", false, this);
  connect(m_mip, &ZBoolParameter::boolChanged, this, &ZView::changeViewStyle);
  m_viewport = new ZDVec4Parameter("Viewport", this);
  connect(m_viewport, &ZDVec4Parameter::valueChanged, this, &ZView::changeViewport);

  setFocusPolicy(Qt::StrongFocus);

  m_label = new QLabel(this);
  m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_label->setWordWrap(true);

  m_layout = new QVBoxLayout;
  m_layout->addWidget(m_label);

  m_scene = new ZGraphicsScene(this);
  connect(m_scene, &ZGraphicsScene::mousePressed,
          this, &ZView::mousePressed);
  connect(m_scene, &ZGraphicsScene::mouseReleased,
          this, &ZView::mouseReleased);
  connect(m_scene, &ZGraphicsScene::selectionChanged,
          this, &ZView::selectionChanged);
  m_view = new ZGraphicsView(m_scene, this);
  connect(m_view, &ZGraphicsView::viewportChanged,
          this, &ZView::viewportChanged);

  m_layout->addWidget(m_view);
  m_layout->addSpacing(15);

  auto hly = new QHBoxLayout;
  m_imgSlice->setStyle("SPINBOXWITHSCROLLBAR");
  m_imgSliceWidget = m_imgSlice->createWidget(this);
  hly->addWidget(m_imgSlice->createNameLabel(this));
  hly->addWidget(m_imgSliceWidget);
  m_layout->addLayout(hly);

  hly = new QHBoxLayout;
  m_imgTime->setStyle("SPINBOXWITHSCROLLBAR");
  m_imgTimeWidget = m_imgTime->createWidget(this);
  hly->addWidget(m_imgTime->createNameLabel(this));
  hly->addWidget(m_imgTimeWidget);
  m_layout->addLayout(hly);

  createActions();

  m_imgSlice->setVisible(false);
  m_imgTime->setVisible(false);

  connect(m_imgSlice, &ZIntParameter::valueChanged, this, &ZView::sliceChanged);
  connect(m_imgTime, &ZIntParameter::valueChanged, this, &ZView::sliceChanged);

  setLayout(m_layout);
}

ZView::~ZView()
{
  m_objViews.clear();  // objviews hold some reference to view's member, so they should die first
}

QWidget* ZView::createScaleWidget(QWidget* parent)
{
  return m_view->createScaleWidget(parent);
}

QToolButton* ZView::createROIToolButton(QWidget* parent)
{
  auto res = new QToolButton(parent);
  //res->setCheckable(true);
  res->addAction(m_roiSplineAction);
  res->addAction(m_roiPolygonAction);
  res->addAction(m_roiRectangleAction);
  res->addAction(m_roiEllipseAction);
  connect(res, &QToolButton::triggered, res, &QToolButton::setDefaultAction);
  res->setDefaultAction(m_roiSplineAction);
  res->setPopupMode(QToolButton::MenuButtonPopup);
  return res;
}

int ZView::currentSlice() const
{
  return m_imgSlice->get();
}

int ZView::currentTime() const
{
  return m_imgTime->get();
}

double ZView::currentScale() const
{
  return m_view->currentScale();
}

QRectF ZView::currentViewport() const
{
  return m_view->getCurrrentlyVisibleRegion().intersected(m_scene->sceneRect());
}

std::pair<int, int> ZView::currentSliceRange() const
{
  if (isMaxZProjView()) {
    return std::make_pair(m_imgSlice->rangeMin(), m_imgSlice->rangeMax() + 1);
  } else {
    return std::make_pair(m_imgSlice->get(), m_imgSlice->get() + 1);
  }
}

ZROI& ZView::roi()
{
  return m_doc.roiDoc().currentROI();
}

ZView::State ZView::state() const
{
  if (m_roiEllipseAction->isChecked())
    return State::ROIEllipse;
  if (m_roiRectangleAction->isChecked())
    return State::ROIRect;
  if (m_roiPolygonAction->isChecked())
    return State::ROIPolygon;
  if (m_roiSplineAction->isChecked())
    return State::ROISpline;

  return State::Normal;
}

QWidget* ZView::captureWidget()
{
  auto res = new QScrollArea();
  auto m_screenShotWidget = new ZTakeScreenShotWidget(true, false, nullptr);
  connect(m_screenShotWidget, &ZTakeScreenShotWidget::take2DScreenShot,
          this, &ZView::takeScreenShot);
  connect(m_screenShotWidget, &ZTakeScreenShotWidget::takeFixedSize2DScreenShot,
          this, &ZView::takeFixedSizeScreenShot);
  res->setWidget(m_screenShotWidget);
  return res;
}

void ZView::updateBoundBox()
{
  m_doNotReceiveSliceSignal = true;
  std::array<int, 8> oldBound = m_boundBox;
  m_boundBox[0] = m_boundBox[2] = m_boundBox[4] = m_boundBox[6] = std::numeric_limits<int>::max();
  m_boundBox[1] = m_boundBox[3] = m_boundBox[5] = m_boundBox[7] = std::numeric_limits<int>::min();
  for (const auto& view : m_objViews) {
    const std::array<int, 8>& boundBox = view->boundBox();
    m_boundBox[0] = std::min(boundBox[0], m_boundBox[0]);
    m_boundBox[1] = std::max(boundBox[1], m_boundBox[1]);
    m_boundBox[2] = std::min(boundBox[2], m_boundBox[2]);
    m_boundBox[3] = std::max(boundBox[3], m_boundBox[3]);
    m_boundBox[4] = std::min(boundBox[4], m_boundBox[4]);
    m_boundBox[5] = std::max(boundBox[5], m_boundBox[5]);
    m_boundBox[6] = std::min(boundBox[6], m_boundBox[6]);
    m_boundBox[7] = std::max(boundBox[7], m_boundBox[7]);
  }
  if (m_boundBox[0] > m_boundBox[1] || m_boundBox[2] > m_boundBox[3] || m_boundBox[4] > m_boundBox[5] ||
      m_boundBox[6] > m_boundBox[7]) {
    // nothing visible
    m_boundBox[0] = m_boundBox[2] = m_boundBox[4] = m_boundBox[6] = 0;
    m_boundBox[1] = m_boundBox[3] = 100;
    m_boundBox[5] = m_boundBox[7] = 0;
  }
  if (oldBound == m_boundBox) {
    m_doNotReceiveSliceSignal = false;
    return;
  }
  QRectF sceneRect(m_boundBox[0], m_boundBox[2],
                   m_boundBox[1] - m_boundBox[0] + 1, m_boundBox[3] - m_boundBox[2] + 1);
  m_scene->setSceneRect(sceneRect);
  m_view->updateScaleFactorRange();
  int sliceBefore = m_imgSlice->get();
  int timeBefore = m_imgTime->get();
  m_imgSlice->setRange(m_boundBox[4], m_boundBox[5]);
  m_imgSlice->setVisible(m_boundBox[5] > m_boundBox[4] || m_maxZProjViewAction->isChecked());
  m_imgTime->setRange(m_boundBox[6], m_boundBox[7]);
  m_imgTime->setVisible(m_boundBox[7] > m_boundBox[6]);
  if (m_numObjsBefore == 0 && m_doc.numObjs() > 0) {
    m_imgSlice->set((m_boundBox[4] + m_boundBox[5]) / 2);
    fitContentIntoWindow();
  }
  m_numObjsBefore = m_doc.numObjs();
  m_doNotReceiveSliceSignal = false;

  m_zoomInAction->setEnabled(m_doc.hasObj());
  m_zoomOutAction->setEnabled(m_doc.hasObj());
  m_imgViewStyleActionGroup->setEnabled(m_doc.hasObj() && m_boundBox[5] > m_boundBox[4]);

  if (m_imgSlice->get() != sliceBefore || m_imgTime->get() != timeBefore)
    sliceChanged();
}

void ZView::setInfo(double x, double y)
{
  QString info;
  if (m_maxZProjViewAction->isChecked()) {
    info += "**Maximum Z Projection**      ";
  }
  for (const auto& view : m_objViews) {
    info += view->infoOfPos(x, y);
  }
  m_label->setText(info);
}

void ZView::registerObjView(std::unique_ptr<ZObjView>&& v)
{
  connect(v.get(), &ZObjView::objViewReady, this, &ZView::objViewReady);
  m_objViews.push_back(std::move(v));
}

std::shared_ptr<ZWidgetsGroup> ZView::viewSettingWidgetsGroupOf(size_t id)
{
  if (id == 1 || id == 2 || id == 3)
    return std::shared_ptr<ZWidgetsGroup>();
  for (const auto& view : m_objViews) {
    std::shared_ptr<ZWidgetsGroup> wg = view->viewSettingWidgetsGroupOf(id);
    if (wg)
      return wg;
  }
  return std::shared_ptr<ZWidgetsGroup>();
}

void ZView::read(size_t id, const QJsonObject& json)
{
  for (const auto& view : m_objViews) {
    if (view->hasObj(id)) {
      if (json.value("ViewObjType").toString() == view->doc().typeName()) {
        view->read(id, json);
      } else {
        LOG(WARNING) << "view object type " << json.value("ViewObjType").toString()
                     << " does not match object type " << view->doc().typeName() << ". abort.";
      }
      return;
    }
  }
}

void ZView::write(size_t id, QJsonObject& json) const
{
  for (const auto& view : m_objViews) {
    if (view->hasObj(id)) {
      json.insert("ViewObjType", view->doc().typeName());
      json.insert("ViewVersion", QJsonValue(1.0));
      view->write(id, json);
      return;
    }
  }
}

void ZView::read(const QJsonObject& json)
{
  m_imgSlice->read(json);
  m_imgTime->read(json);
  updateViewportPara();
  m_viewport->read(json);
  m_mip->read(json);
}

void ZView::write(QJsonObject& json) const
{
  m_imgSlice->write(json);
  m_imgTime->write(json);
  updateViewportPara();
  m_viewport->write(json);
  m_mip->write(json);
}

void ZView::fitContentIntoWindow()
{
  QRectF sceneRect(m_boundBox[0], m_boundBox[2], m_boundBox[1] - m_boundBox[0] + 1, m_boundBox[3] - m_boundBox[2] + 1);
  m_view->fitRect(sceneRect);
}

int ZView::minViewPrecedence() const
{
  int res = std::numeric_limits<int>::max();
  for (const auto& view : m_objViews) {
    res = std::min(res, view->minViewPrecedence());
  }
  return res;
}

int ZView::maxViewPrecedence() const
{
  int res = std::numeric_limits<int>::min();
  for (const auto& view : m_objViews) {
    res = std::max(res, view->maxViewPrecedence());
  }
  return res;
}

void ZView::sliceChanged()
{
  if (m_doNotReceiveSliceSignal)
    return;

  if (isMaxZProjView()) {
    for (const auto& view : m_objViews) {
      view->setMaxZProjView(m_imgTime->get());
    }
  } else {
    for (const auto& view : m_objViews) {
      view->setNormalView(m_imgSlice->get(), m_imgTime->get());
    }
  }
}

void ZView::zoomIn()
{
  m_view->setScale(1.1 * m_view->currentScale());
}

void ZView::zoomOut()
{
  m_view->setScale(1 / 1.1 * m_view->currentScale());
}

void ZView::triggerNormalView(bool v)
{
  m_mip->set(!v);
}

void ZView::triggerMaxZProjView(bool v)
{
  m_mip->set(v);
}

void ZView::changeViewStyle(bool mip)
{
  if (!m_doc.hasObj())
    return;

  if (mip) {
    if (!m_maxZProjViewAction->isChecked())
      m_maxZProjViewAction->setChecked(true);

    for (const auto& view : m_objViews) {
      view->setMaxZProjView(m_imgTime->get());
    }

    m_imgSlice->setEnabled(false);
  } else {
    if (!m_normalViewAction->isChecked())
      m_normalViewAction->setChecked(true);

    for (const auto& view : m_objViews) {
      view->setNormalView(m_imgSlice->get(), m_imgTime->get());
    }

    m_imgSlice->setEnabled(true);
  }
  setInfo(-1, -1);
}

void ZView::changeViewport()
{
  QRectF rect = QRectF(m_viewport->get().x,
                       m_viewport->get().y,
                       m_viewport->get().z,
                       m_viewport->get().w);
  if (rect != m_view->getCurrrentlyVisibleRegion() && rect.isValid()) {
    m_view->fitRect(rect);
  }
}

void ZView::takeFixedSizeScreenShot(QString filename, int width, int height)
{
  QString err;
  if (!m_view->renderToImage(filename, width, height, &err)) {
    QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), err);
  }
}

void ZView::takeScreenShot(QString filename)
{
  QString err;
  if (!m_view->renderToImage(filename, &err)) {
    QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), err);
  }
}

void ZView::mousePressed(QPointF scenePos)
{
  for (const auto& view : m_objViews) {
    view->mousePressed(scenePos);
  }
}

void ZView::mouseReleased(QPointF scenePos)
{
  for (const auto& view : m_objViews) {
    view->mouseReleased(scenePos);
  }
}

void ZView::selectionChanged()
{
  QList<QGraphicsItem*> items = m_scene->selectedItems();
  for (const auto& view : m_objViews) {
    view->selectionChanged(items);
  }
}

void ZView::setViewDragMode(QAction* act)
{
  if (act == m_scrollHandDragAction) {
    m_view->setScrollHandDragMode();
  } else if (act == m_rubberBandDragAction) {
    m_view->setRubberBandDragMode();
  }
}

void ZView::keyPressEvent(QKeyEvent* event)
{
  switch (event->key()) {
    case Qt::Key_Left:
      if (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier) {
        if (m_normalViewAction->isChecked() && m_imgSlice->get() > 0) {
          m_imgSlice->set(m_imgSlice->get() - 1);
        }
      }
      break;
    case Qt::Key_Right:
      if (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier) {
        if (m_normalViewAction->isChecked() && m_imgSlice->get() < m_imgSlice->rangeMax())
          m_imgSlice->set(m_imgSlice->get() + 1);
      }
      break;
    case Qt::Key_M:
      if (event->modifiers() == Qt::ControlModifier) {
        if (m_normalViewAction->isChecked()) {
          m_maxZProjViewAction->setChecked(true);
        } else {
          m_normalViewAction->setChecked(true);
        }
      }
      break;
    case Qt::Key_Backspace:
    case Qt::Key_Delete:
      if (event->modifiers() == Qt::NoModifier) {
        for (const auto& view : m_objViews) {
          view->deleteKeyPressed();
        }
      }
      break;
    case Qt::Key_R:
      if (event->modifiers() == Qt::ControlModifier) {
        for (const auto& view : m_objViews) {
          view->rotateClockwise();
        }
      } else if (event->modifiers() == (Qt::ControlModifier | Qt::AltModifier)) {
        for (const auto& view : m_objViews) {
          view->rotateCounterclockwise();
        }
      }
      break;
    default:
      break;
  }
}

void ZView::createActions()
{
  m_zoomInAction = new QAction(QIcon(":/icons/zoom_in-512.png"), tr("Zoom &In"), this);
  QList<QKeySequence> zoomInKey;
  zoomInKey << QKeySequence::ZoomIn << QKeySequence(Qt::Key_Plus) << QKeySequence(Qt::Key_Equal);
  m_zoomInAction->setShortcuts(zoomInKey);
  m_zoomInAction->setStatusTip(tr("Zoom In"));
  connect(m_zoomInAction, &QAction::triggered, this, &ZView::zoomIn);

  m_zoomOutAction = new QAction(QIcon(":/icons/zoom_out-512.png"), tr("Zoom &Out"), this);
  QList<QKeySequence> zoomOutKey;
  zoomOutKey << QKeySequence::ZoomOut << QKeySequence(Qt::Key_Minus);
  m_zoomOutAction->setShortcuts(zoomOutKey);
  m_zoomOutAction->setStatusTip(tr("Zoom Out"));
  connect(m_zoomOutAction, &QAction::triggered, this, &ZView::zoomOut);

  m_normalViewAction = new QAction(QIcon(":/icons/gallery-512.png"), tr("&Normal View"), this);
  m_normalViewAction->setCheckable(true);
  m_normalViewAction->setStatusTip(tr("Default Image View"));
  connect(m_normalViewAction, &QAction::toggled, this, &ZView::triggerNormalView);

  m_maxZProjViewAction = new QAction(QIcon(":/icons/frame-512.png"), tr("&Maximum Z Projection"), this);
  m_maxZProjViewAction->setCheckable(true);
  m_maxZProjViewAction->setStatusTip(tr("Maximum Project Image Along Dimension Z"));
  connect(m_maxZProjViewAction, &QAction::toggled, this, &ZView::triggerMaxZProjView);

  m_imgViewStyleActionGroup = new QActionGroup(this);
  m_imgViewStyleActionGroup->setExclusive(true);
  m_imgViewStyleActionGroup->addAction(m_normalViewAction);
  m_imgViewStyleActionGroup->addAction(m_maxZProjViewAction);
  if (m_mip->get())
    m_maxZProjViewAction->setChecked(true);
  else
    m_normalViewAction->setChecked(true);

  m_fitIntoWindowAction = new QAction(QIcon(":/icons/collapse-512.png"), tr("&Fit Into Window"), this);
  m_fitIntoWindowAction->setStatusTip(tr("Fit everything inside window"));
  connect(m_fitIntoWindowAction, &QAction::triggered, this, &ZView::fitContentIntoWindow);

  m_zoomInAction->setEnabled(false);
  m_zoomOutAction->setEnabled(false);
  m_imgViewStyleActionGroup->setEnabled(false);

  m_scrollHandDragAction = new QAction(QIcon(":/icons/hand_cursor-512.png"), tr("&Scroll Hand Drag"), this);
  m_scrollHandDragAction->setCheckable(true);
  m_scrollHandDragAction->setStatusTip(tr("Scroll Hand Drag"));

  m_rubberBandDragAction = new QAction(QIcon(":/icons/rubber_band-2048.png"), tr("&Rubber Band Drag"), this);
  m_rubberBandDragAction->setCheckable(true);
  m_rubberBandDragAction->setStatusTip(tr("Rubber Band Drag"));

  m_dragModeActionGroup = new QActionGroup(this);
  m_dragModeActionGroup->addAction(m_scrollHandDragAction);
  m_dragModeActionGroup->addAction(m_rubberBandDragAction);
  m_rubberBandDragAction->setChecked(true);
  setViewDragMode(m_rubberBandDragAction);
  connect(m_dragModeActionGroup, &QActionGroup::triggered, this, &ZView::setViewDragMode);

  m_roiRectangleAction = new QAction(QIcon(":/icons/rectangle_stroked-512.png"), tr("&Rectangular Selections"), this);
  m_roiRectangleAction->setCheckable(true);
  m_roiRectangleAction->setStatusTip(tr("Make Rectangular Selections"));

  m_roiEllipseAction = new QAction(QIcon(":/icons/ellipse_stroked-512.png"), tr("&Elliptical Selections"), this);
  m_roiEllipseAction->setCheckable(true);
  m_roiEllipseAction->setStatusTip(tr("Make Elliptical Selections"));

  m_roiPolygonAction = new QAction(QIcon(":/icons/polygon-512.png"), tr("&Polygon Selections"), this);
  m_roiPolygonAction->setCheckable(true);
  m_roiPolygonAction->setStatusTip(tr("Make Polygon Selections"));

  m_roiSplineAction = new QAction(QIcon(":/icons/spline-512.png"), tr("&Spline Selections"), this);
  m_roiSplineAction->setCheckable(true);
  m_roiSplineAction->setStatusTip(tr("Make Spline Selections"));

  //m_roiLineAction = new QAction(QIcon(":/icons/line-512.png"), tr("&Line Selections"), this);
  //m_roiLineAction->setCheckable(true);
  //m_roiLineAction->setStatusTip(tr("Make Line Selections"));

  m_roiStyleActionGroup = new ZActionGroup(this);
  m_roiStyleActionGroup->addAction(m_roiSplineAction);
  m_roiStyleActionGroup->addAction(m_roiPolygonAction);
  m_roiStyleActionGroup->addAction(m_roiRectangleAction);
  m_roiStyleActionGroup->addAction(m_roiEllipseAction);
  //m_roiStyleActionGroup->addAction(m_roiLineAction);
}

void ZView::updateViewportPara() const
{
  QRectF rect = m_view->getCurrrentlyVisibleRegion();
  m_viewport->blockSignals(true);
  m_viewport->set(glm::dvec4(rect.left(), rect.top(), rect.width(), rect.height()));
  m_viewport->blockSignals(false);
}

} // namespace nim
