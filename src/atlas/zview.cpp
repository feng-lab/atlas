#include "zview.h"

#include "zdoc.h"
#include "zgraphicsscene.h"
#include "zgraphicsview.h"
#include "zobjview.h"
#include "znumericparameter.h"
#include "zoptionparameter.h"
#include "zroidoc.h"
#include "zregionannotationdoc.h"
#include "ztakescreenshotwidget.h"
#include "zlog.h"
#include "ztheme.h"
#include "zmessageboxhelpers.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QActionGroup>
#include <QToolButton>
#include <QGraphicsPixmapItem>
#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QProgressDialog>
#include <QMenu>

namespace nim {

ZView::ZView(ZDoc& doc, QWidget* parent, Qt::WindowFlags f)
  : QWidget(parent, f)
  , m_doc(doc)
  , m_doNotReceiveSliceSignal(false)
  , m_numObjsBefore(m_doc.numObjs())
{
  m_imgSlice = new ZIntParameter("slice", 0, 0, 0, this);
  m_imgTime = new ZIntParameter("time", 0, 0, 0, this);

  m_viewStyle = new ZStringIntOptionParameter("View Style", this);
  m_viewStyle->addOptions(QString("Normal"), QString("MIP"), QString("Montage"));
  connect(m_viewStyle, &ZStringIntOptionParameter::valueChanged, this, &ZView::changeViewStyle);
  m_montageColumns = new ZIntParameter("Montage Columns", 0, 0, 1000, this);
  connect(m_montageColumns, &ZIntParameter::valueChanged, this, &ZView::updateSceneRectFromBoundBox);
  connect(m_montageColumns, &ZIntParameter::valueChanged, this, &ZView::updateMontageScene);
  m_viewport = new ZDVec4Parameter("Viewport", this);
  connect(m_viewport, &ZDVec4Parameter::valueChanged, this, &ZView::changeViewport);

  setFocusPolicy(Qt::StrongFocus);

  m_label = new QLabel(this);
  m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_label->setWordWrap(true);

  m_layout = new QVBoxLayout;
  m_layout->addWidget(m_label);

  m_scene = new ZGraphicsScene(this);
  connect(m_scene, &ZGraphicsScene::mousePressed, this, &ZView::mousePressed);
  connect(m_scene, &ZGraphicsScene::mouseMoved, this, &ZView::mouseMoved);
  connect(m_scene, &ZGraphicsScene::mouseReleased, this, &ZView::mouseReleased);
  connect(m_scene, &ZGraphicsScene::selectionChanged, this, &ZView::selectionChanged);
  m_view = new ZGraphicsView(m_scene, this);
  connect(m_view, &ZGraphicsView::viewportChanged, this, &ZView::viewportChanged);
  connect(m_view, &ZGraphicsView::viewportChanged, this, &ZView::updateMontageScene);

  m_montageScene = new QGraphicsScene(this);

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

  connect(&m_doc, &ZDoc::requestToAdjustViewToPosition, this, &ZView::gotoPosition);

  m_roiMode = new ZStringIntOptionParameter("ROI Mode", this);
  m_roiMode->addOptions("RegionAnnotation", "ROI");
  // m_roiMode->select("RegionAnnotation");
  m_roiMode->select("ROI");
}

ZView::~ZView()
{
  m_objViews.clear(); // objviews hold some reference to view's member, so they should die first
}

QWidget* ZView::createScaleWidget(QWidget* parent)
{
  return m_view->createScaleWidget(parent);
}

QToolButton* ZView::createROIToolButton(QWidget* parent)
{
  auto res = new QToolButton(parent);
  // res->setCheckable(true);
  res->addAction(m_roiSplineAction);
  res->addAction(m_roiPolygonAction);
  res->addAction(m_roiRectangleAction);
  res->addAction(m_roiEllipseAction);
  res->addAction(m_roiCutLineAction);
  connect(res, &QToolButton::triggered, res, &QToolButton::setDefaultAction);
  res->setDefaultAction(m_roiSplineAction);
  res->setPopupMode(QToolButton::MenuButtonPopup);
  return res;
}

QWidget* ZView::createROIModeWidget(QWidget* parent)
{
  return m_roiMode->createWidget(parent);
}

int ZView::currentSlice() const
{
  if (currentViewStyle() == ViewStyle::Montage) {
    return m_montageZ;
  }
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
  if (currentViewStyle() == ViewStyle::Montage) {
    return m_scene->sceneRect();
  }
  return m_view->getCurrrentlyVisibleRegion().intersected(m_view->scene()->sceneRect());
}

std::pair<int, int> ZView::currentSliceRange() const
{
  if (currentViewStyle() == ViewStyle::MIP) {
    return std::make_pair(m_imgSlice->rangeMin(), m_imgSlice->rangeMax() + 1);
  } else {
    return std::make_pair(m_imgSlice->get(), m_imgSlice->get() + 1);
  }
}

ZROIPack& ZView::roiPack(size_t id)
{
  return m_doc.roiDoc().roiPack(id);
}

ZRegionAnnotationPack& ZView::regionAnnotationPack(size_t id)
{
  return m_doc.regionAnnotationDoc().regionAnnotationPack(id);
}

ZROIPack& ZView::currentROIPack()
{
  return m_doc.roiDoc().currentROIPack();
}

ZRegionAnnotationPack& ZView::currentRegionAnnotationPack()
{
  return m_doc.regionAnnotationDoc().currentRegionAnnotationPack();
}

ZView::State ZView::state() const
{
  if (m_roiEllipseAction->isChecked()) {
    return State::ROIEllipse;
  }
  if (m_roiRectangleAction->isChecked()) {
    return State::ROIRect;
  }
  if (m_roiPolygonAction->isChecked()) {
    return State::ROIPolygon;
  }
  if (m_roiSplineAction->isChecked()) {
    return State::ROISpline;
  }
  if (m_roiCutLineAction->isChecked()) {
    return State::ROICut;
  }

  return State::Normal;
}

QWidget* ZView::captureWidget() const
{
  // auto res = new QScrollArea();
  auto m_screenShotWidget = new ZTakeScreenShotWidget(true, false, nullptr);
  connect(m_screenShotWidget, &ZTakeScreenShotWidget::take2DScreenShot, this, &ZView::takeScreenShot);
  connect(m_screenShotWidget, &ZTakeScreenShotWidget::takeFixedSize2DScreenShot, this, &ZView::takeFixedSizeScreenShot);
  // res->setWidget(m_screenShotWidget);
  return m_screenShotWidget;
}

void ZView::updateBoundBox()
{
  m_doNotReceiveSliceSignal = true;
  ZBBox<glm::ivec4> oldBound = m_boundBox;
  m_boundBox.reset();
  for (const auto& view : m_objViews) {
    m_boundBox.expand(view->boundBox());
  }
  if (m_boundBox.empty()) {
    // nothing visible
    m_boundBox.setMinCorner(glm::ivec4(0));
    m_boundBox.setMaxCorner(glm::ivec4(100, 100, 0, 0));
  }
  if (oldBound == m_boundBox) {
    m_doNotReceiveSliceSignal = false;
    return;
  }
  updateSceneRectFromBoundBox();
  m_view->updateScaleFactorRange();
  int sliceBefore = m_imgSlice->get();
  int timeBefore = m_imgTime->get();
  m_imgSlice->setRange(m_boundBox.minCorner.z, m_boundBox.maxCorner.z);
  m_imgSlice->setVisible(m_boundBox.maxCorner.z > m_boundBox.minCorner.z);
  m_imgTime->setRange(m_boundBox.minCorner.w, m_boundBox.maxCorner.w);
  m_imgTime->setVisible(m_boundBox.maxCorner.w > m_boundBox.minCorner.w);
  if (m_numObjsBefore == 0 && m_doc.numObjs() > 0) {
    m_imgSlice->set((m_boundBox.minCorner.z + m_boundBox.maxCorner.z) / 2);
    fitContentIntoWindow();
  }
  m_numObjsBefore = m_doc.numObjs();
  m_doNotReceiveSliceSignal = false;

  m_zoomInAction->setEnabled(m_doc.hasObj());
  m_zoomOutAction->setEnabled(m_doc.hasObj());
  m_imgViewStyleActionGroup->setEnabled(m_doc.hasObj() && m_boundBox.maxCorner.z > m_boundBox.minCorner.z);

  if (m_imgSlice->get() != sliceBefore || m_imgTime->get() != timeBefore) {
    sliceChanged();
  }
}

void ZView::setInfo(double x, double y)
{
  QString info;
  if (currentViewStyle() == ViewStyle::MIP) {
    info += "**Maximum Z Projection**      ";
  } else if (currentViewStyle() == ViewStyle::Montage) {
    info += "**Montage**      ";
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
  if (id == 1 || id == 2 || id == 3) {
    return {};
  }
  for (const auto& view : m_objViews) {
    std::shared_ptr<ZWidgetsGroup> wg = view->viewSettingWidgetsGroupOf(id);
    if (wg) {
      return wg;
    }
  }
  return {};
}

QWidget* ZView::globalParasWidget()
{
  auto widgetsGrp = std::make_shared<ZWidgetsGroup>("Global", 1);
  widgetsGrp->addChild(*m_viewStyle, 1);
  widgetsGrp->addChild(*m_montageColumns, 1);
  return widgetsGrp->createWidget(false);
}

void ZView::read(size_t id, const json::object& json)
{
  for (const auto& view : m_objViews) {
    if (view->hasObj(id)) {
      if (asQString(json.at("ViewObjType")) == view->doc().typeName()) {
        view->read(id, json);
      } else {
        LOG(WARNING) << "view object type " << asQString(json.at("ViewObjType")) << " does not match object type "
                     << view->doc().typeName() << ". abort.";
      }
      return;
    }
  }
}

void ZView::write(size_t id, json::object& json) const
{
  for (const auto& view : m_objViews) {
    if (view->hasObj(id)) {
      json["ViewObjType"] = json::value_from(view->doc().typeName());
      json["ViewVersion"] = 1.0;
      view->write(id, json);
      return;
    }
  }
}

void ZView::read(const json::object& json)
{
  m_imgSlice->read(json);
  m_imgTime->read(json);
  updateViewportPara();
  m_viewport->read(json);
  m_viewStyle->read(json);
  m_montageColumns->read(json);
}

void ZView::write(json::object& json) const
{
  m_imgSlice->write(json);
  m_imgTime->write(json);
  updateViewportPara();
  m_viewport->write(json);
  m_viewStyle->write(json);
  m_montageColumns->write(json);
}

void ZView::fitContentIntoWindow()
{
  m_view->fitRect(m_view->scene()->sceneRect());
}

void ZView::gotoPosition(double x, double y, double z, double radius)
{
  if (currentViewStyle() == ViewStyle::Normal) {
    m_imgSlice->set(std::round(z));
  }
  QRectF sceneRect(x - radius, y - radius, radius * 2 + 1, radius * 2 + 1);
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

void ZView::copy()
{
  VLOG(1) << "copy";
  for (const auto& view : m_objViews) {
    view->copyKeyPressed();
  }
}

void ZView::pasteHere(int slice, QPointF point, bool hFlip, bool vFlip)
{
  VLOG(1) << "paste here";
  for (const auto& view : m_objViews) {
    view->pasteKeyPressed(slice, point, hFlip, vFlip);
  }
}

void ZView::paste()
{
  VLOG(1) << "paste";
  for (const auto& view : m_objViews) {
    view->pasteKeyPressed(currentSlice(), m_scene->lastPressedPoint(), false, false);
  }
}

void ZView::appendContextMenuActions(QMenu& menu, QPointF scenePos, Qt::KeyboardModifiers modifiers)
{
  const size_t activeId = m_doc.viewSettingId();
  for (const auto& view : m_objViews) {
    view->appendContextMenuActions(menu, activeId, scenePos, modifiers);
  }
}

void ZView::checkViewport()
{
  m_view->checkViewport();
}

bool ZView::isRegionAnnotationMode() const
{
  return m_roiMode->isSelected("RegionAnnotation");
}

bool ZView::isROIMode() const
{
  return m_roiMode->isSelected("ROI");
}

void ZView::estimateMontageColumns() const
{
  auto currentNCols = m_montageColumns->get();
  if (currentNCols == 0) {
    m_montageColumns->blockSignals(true);
  }
  auto width = m_boundBox.maxCorner.x - m_boundBox.minCorner.x + 1;
  auto height = m_boundBox.maxCorner.y - m_boundBox.minCorner.y + 1;
  auto depth = m_boundBox.maxCorner.z - m_boundBox.minCorner.z + 1;
  if (depth >= 1) {
    int bestNCols = 0;
    double bestDist = std::numeric_limits<double>::max();
    for (int nCols = 1; nCols <= depth; ++nCols) {
      int nRows = (depth + nCols - 1) / nCols;
      double dist = std::abs(16.0 / 9.0 - (nCols * width * 1.0) / (nRows * height * 1.0));
      if (dist < bestDist) {
        bestNCols = nCols;
        bestDist = dist;
        // VLOG(1) << bestDist << " " << bestNCols;
      } else {
        break;
      }
    }
    m_montageColumns->set(bestNCols);
  }
  if (currentNCols == 0) {
    m_montageColumns->blockSignals(false);
  }
}

ZView::ViewStyle ZView::currentViewStyle() const
{
  if (m_viewStyle->isSelected("Normal")) {
    return ViewStyle::Normal;
  } else if (m_viewStyle->isSelected("MIP")) {
    return ViewStyle::MIP;
  } else {
    return ViewStyle::Montage;
  }
}

void ZView::sliceChanged()
{
  if (m_doNotReceiveSliceSignal) {
    return;
  }

  if (currentViewStyle() == ViewStyle::MIP) {
    for (const auto& view : m_objViews) {
      view->setMaxZProjView(m_imgTime->get());
    }
  } else if (currentViewStyle() == ViewStyle::Normal) {
    for (const auto& view : m_objViews) {
      view->setNormalView(m_imgSlice->get(), m_imgTime->get());
    }
  } else {
    updateMontageScene();
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
  if (v) {
    m_viewStyle->select("Normal");
  }
}

void ZView::triggerMaxZProjView(bool v)
{
  if (v) {
    m_viewStyle->select("MIP");
  }
}

void ZView::triggerMontageView(bool v)
{
  if (v) {
    m_viewStyle->select("Montage");
  }
}

void ZView::changeViewStyle()
{
  if (!m_doc.hasObj()) {
    return;
  }

  if (currentViewStyle() == ViewStyle::MIP) {
    if (!m_maxZProjViewAction->isChecked()) {
      m_maxZProjViewAction->setChecked(true);
    }

    if (m_view->scene() != m_scene) {
      m_scene->disconnect(m_montageScene);
      m_view->setScene(m_scene);
      m_view->updateScaleFactorRange();
      fitContentIntoWindow();
    }

    for (const auto& view : m_objViews) {
      view->setMaxZProjView(m_imgTime->get());
    }

    m_imgSlice->setEnabled(false);
  } else if (currentViewStyle() == ViewStyle::Normal) {
    if (!m_normalViewAction->isChecked()) {
      m_normalViewAction->setChecked(true);
    }

    if (m_view->scene() != m_scene) {
      m_scene->disconnect(m_montageScene);
      m_view->setScene(m_scene);
      m_view->updateScaleFactorRange();
      fitContentIntoWindow();
    }

    for (const auto& view : m_objViews) {
      view->setNormalView(m_imgSlice->get(), m_imgTime->get());
    }

    m_imgSlice->setEnabled(true);
  } else {
    if (!m_montageViewAction->isChecked()) {
      m_montageViewAction->setChecked(true);
    }

    if (m_view->scene() != m_montageScene) {
      m_view->setScene(m_montageScene);
      m_view->updateScaleFactorRange();
      fitContentIntoWindow();
    }

    updateMontageScene();
    //    connect(m_scene, &ZGraphicsScene::changed, this, &ZView::emptyFun);
    connect(m_scene, &ZGraphicsScene::changed, this, &ZView::updateMontageScene);

    m_imgSlice->setEnabled(false);
  }
  setInfo(-1, -1);
}

void ZView::changeViewport()
{
  QRectF rect = QRectF(m_viewport->get().x, m_viewport->get().y, m_viewport->get().z, m_viewport->get().w);
  if (rect != m_view->getCurrrentlyVisibleRegion() && rect.isValid()) {
    m_view->fitRect(rect);
  }
}

void ZView::takeFixedSizeScreenShot(const QString& filename, int width, int height)
{
  QString prepErr;
  for (const auto& view : m_objViews) {
    view->prepare2DExportFrame();
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents);

  bool ready = true;
  for (const auto& view : m_objViews) {
    QString viewErr;
    if (!view->is2DExportFrameReady(&viewErr)) {
      ready = false;
      if (!viewErr.isEmpty()) {
        showCriticalWithDetails(QApplication::activeWindow(), tr("Can not take screenshot %1").arg(filename), viewErr);
        return;
      }
      break;
    }
  }

  if (!ready) {
    QProgressDialog progress(tr("Preparing screenshot..."), tr("Cancel"), 0, 0, QApplication::activeWindow());
    progress.setWindowModality(Qt::WindowModal);
    progress.show();
    if (!waitFor2DExportFrameReady(&progress, &prepErr)) {
      if (!progress.wasCanceled() && !prepErr.isEmpty()) {
        showCriticalWithDetails(QApplication::activeWindow(), tr("Can not take screenshot %1").arg(filename), prepErr);
      }
      return;
    }
  }

  QString err;
  if (!m_view->renderToImage(filename, width, height, &err)) {
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not save screenshot %1").arg(filename), err);
  }
}

void ZView::takeScreenShot(const QString& filename)
{
  QString prepErr;
  for (const auto& view : m_objViews) {
    view->prepare2DExportFrame();
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents);

  bool ready = true;
  for (const auto& view : m_objViews) {
    QString viewErr;
    if (!view->is2DExportFrameReady(&viewErr)) {
      ready = false;
      if (!viewErr.isEmpty()) {
        showCriticalWithDetails(QApplication::activeWindow(), tr("Can not take screenshot %1").arg(filename), viewErr);
        return;
      }
      break;
    }
  }

  if (!ready) {
    QProgressDialog progress(tr("Preparing screenshot..."), tr("Cancel"), 0, 0, QApplication::activeWindow());
    progress.setWindowModality(Qt::WindowModal);
    progress.show();
    if (!waitFor2DExportFrameReady(&progress, &prepErr)) {
      if (!progress.wasCanceled() && !prepErr.isEmpty()) {
        showCriticalWithDetails(QApplication::activeWindow(), tr("Can not take screenshot %1").arg(filename), prepErr);
      }
      return;
    }
  }

  QString err;
  if (!m_view->renderToImage(filename, &err)) {
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not save screenshot %1").arg(filename), err);
  }
}

bool ZView::waitFor2DExportFrameReady(QProgressDialog* progress, QString* errorMsg)
{
  if (errorMsg) {
    errorMsg->clear();
  }

  for (const auto& view : m_objViews) {
    view->prepare2DExportFrame();
  }

  // Let any queued signals kick off async work before we start waiting.
  QCoreApplication::processEvents(QEventLoop::AllEvents);

  while (true) {
    if (progress && progress->wasCanceled()) {
      return false;
    }

    bool allReady = true;
    for (const auto& view : m_objViews) {
      QString viewErr;
      if (!view->is2DExportFrameReady(&viewErr)) {
        allReady = false;
        if (!viewErr.isEmpty()) {
          if (errorMsg) {
            *errorMsg = viewErr;
          }
          return false;
        }
      }
    }

    if (allReady) {
      return true;
    }

    QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
  }
}

void ZView::mousePressed(QPointF scenePos, Qt::KeyboardModifiers modifiers)
{
  for (const auto& view : m_objViews) {
    view->mousePressed(scenePos, modifiers);
  }
}

void ZView::mouseMoved(QPointF scenePos, Qt::KeyboardModifiers modifiers)
{
  for (const auto& view : m_objViews) {
    view->mouseMoved(scenePos, modifiers);
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
    m_view->setDragMode(QGraphicsView::ScrollHandDrag);
    m_view->setInteractive(false);
  } else if (act == m_rubberBandDragAction) {
    m_view->setDragMode(QGraphicsView::RubberBandDrag);
    m_view->setInteractive(true);
  } else {
    m_view->setDragMode(QGraphicsView::NoDrag);
    m_view->setInteractive(true);
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
        if (m_normalViewAction->isChecked() && m_imgSlice->get() < m_imgSlice->rangeMax()) {
          m_imgSlice->set(m_imgSlice->get() + 1);
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
          view->rotateClockwise(m_scene->lastPressedPoint().x(), m_scene->lastPressedPoint().y());
        }
      } else if (event->modifiers() == (Qt::ControlModifier | Qt::AltModifier)) {
        for (const auto& view : m_objViews) {
          view->rotateCounterclockwise(m_scene->lastPressedPoint().x(), m_scene->lastPressedPoint().y());
        }
      }
      break;
    case Qt::Key_Escape:
      m_scene->escKeyPressed();
      break;
    default:
      break;
  }
}

void ZView::createActions()
{
  m_copyAction = new QAction(tr("&Copy"), this);
  m_copyAction->setStatusTip(tr("Copy Selected Items"));
  m_copyAction->setShortcut(QKeySequence::Copy);
  connect(m_copyAction, &QAction::triggered, this, &ZView::copy);

  m_pasteAction = new QAction(tr("&Paste"), this);
  m_pasteAction->setStatusTip(tr("Paste Selected Items"));
  m_pasteAction->setShortcut(QKeySequence::Paste);
  connect(m_pasteAction, &QAction::triggered, this, &ZView::paste);

  //  m_deleteAction = new QAction(tr("&Delete"), this);
  //  m_deleteAction->setStatusTip(tr("Delete Selected Items"));
  //  QList<QKeySequence> deleteKey;
  //  deleteKey << QKeySequence::Delete << QKeySequence(Qt::Key_Backspace);
  //  m_deleteAction->setShortcuts(deleteKey);
  //  connect(m_deleteAction, &QAction::triggered, this, &ZView::deleteKeyPressed);

  m_zoomInAction = new QAction(ZTheme::instance().icon(ZTheme::ZoomInIcon), tr("Zoom &In"), this);
  QList<QKeySequence> zoomInKey;
  zoomInKey << QKeySequence::ZoomIn << QKeySequence(Qt::Key_Plus) << QKeySequence(Qt::Key_Equal);
  m_zoomInAction->setShortcuts(zoomInKey);
  m_zoomInAction->setStatusTip(tr("Zoom In"));
  connect(m_zoomInAction, &QAction::triggered, this, &ZView::zoomIn);

  m_zoomOutAction = new QAction(ZTheme::instance().icon(ZTheme::ZoomOutIcon), tr("Zoom &Out"), this);
  QList<QKeySequence> zoomOutKey;
  zoomOutKey << QKeySequence::ZoomOut << QKeySequence(Qt::Key_Minus);
  m_zoomOutAction->setShortcuts(zoomOutKey);
  m_zoomOutAction->setStatusTip(tr("Zoom Out"));
  connect(m_zoomOutAction, &QAction::triggered, this, &ZView::zoomOut);

  m_normalViewAction = new QAction(ZTheme::instance().icon(ZTheme::NormalViewIcon), tr("&Normal View"), this);
  m_normalViewAction->setCheckable(true);
  m_normalViewAction->setStatusTip(tr("Default Image View"));
  connect(m_normalViewAction, &QAction::toggled, this, &ZView::triggerNormalView);

  m_maxZProjViewAction =
    new QAction(ZTheme::instance().icon(ZTheme::ProjectionViewIcon), tr("&Maximum Z Projection"), this);
  m_maxZProjViewAction->setCheckable(true);
  m_maxZProjViewAction->setStatusTip(tr("Maximum Project Image Along Dimension Z"));
  connect(m_maxZProjViewAction, &QAction::toggled, this, &ZView::triggerMaxZProjView);

  m_montageViewAction = new QAction(ZTheme::instance().icon(ZTheme::MontageViewIcon), tr("&Montage View"), this);
  m_montageViewAction->setCheckable(true);
  m_montageViewAction->setStatusTip(tr("Montage View of Z Slices"));
  connect(m_montageViewAction, &QAction::toggled, this, &ZView::triggerMontageView);

  m_imgViewStyleActionGroup = new QActionGroup(this);
  m_imgViewStyleActionGroup->setExclusive(true);
  m_imgViewStyleActionGroup->addAction(m_normalViewAction);
  m_imgViewStyleActionGroup->addAction(m_maxZProjViewAction);
  m_imgViewStyleActionGroup->addAction(m_montageViewAction);
  m_normalViewAction->setChecked(currentViewStyle() == ViewStyle::Normal);
  m_maxZProjViewAction->setChecked(currentViewStyle() == ViewStyle::MIP);
  m_montageViewAction->setChecked(currentViewStyle() == ViewStyle::Montage);

  m_fitIntoWindowAction = new QAction(ZTheme::instance().icon(ZTheme::CollapseIcon), tr("&Fit Into Window"), this);
  m_fitIntoWindowAction->setStatusTip(tr("Fit everything inside window"));
  connect(m_fitIntoWindowAction, &QAction::triggered, this, &ZView::fitContentIntoWindow);

  m_zoomInAction->setEnabled(false);
  m_zoomOutAction->setEnabled(false);
  m_imgViewStyleActionGroup->setEnabled(false);

  m_scrollHandDragAction = new QAction(ZTheme::instance().icon(ZTheme::DragIcon), tr("&Scroll Hand Drag"), this);
  m_scrollHandDragAction->setCheckable(true);
  m_scrollHandDragAction->setStatusTip(tr("Scroll Hand Drag"));

  m_rubberBandDragAction = new QAction(ZTheme::instance().icon(ZTheme::SelectionIcon), tr("&Rubber Band Drag"), this);
  m_rubberBandDragAction->setCheckable(true);
  m_rubberBandDragAction->setStatusTip(tr("Rubber Band Drag"));

  m_dragModeActionGroup = new QActionGroup(this);
  m_dragModeActionGroup->addAction(m_scrollHandDragAction);
  m_dragModeActionGroup->addAction(m_rubberBandDragAction);
  m_rubberBandDragAction->setChecked(true);
  setViewDragMode(m_rubberBandDragAction);
  connect(m_dragModeActionGroup, &QActionGroup::triggered, this, &ZView::setViewDragMode);

  m_roiRectangleAction =
    new QAction(ZTheme::instance().icon(ZTheme::RectangleIcon), tr("&Rectangular Selections"), this);
  m_roiRectangleAction->setCheckable(true);
  m_roiRectangleAction->setStatusTip(tr("Make Rectangular Selections"));

  m_roiEllipseAction = new QAction(ZTheme::instance().icon(ZTheme::EllipseIcon), tr("&Elliptical Selections"), this);
  m_roiEllipseAction->setCheckable(true);
  m_roiEllipseAction->setStatusTip(tr("Make Elliptical Selections"));

  m_roiPolygonAction = new QAction(ZTheme::instance().icon(ZTheme::PolygonIcon), tr("&Polygon Selections"), this);
  m_roiPolygonAction->setCheckable(true);
  m_roiPolygonAction->setStatusTip(tr("Make Polygon Selections"));

  m_roiSplineAction = new QAction(ZTheme::instance().icon(ZTheme::SplineIcon), tr("&Spline Selections"), this);
  m_roiSplineAction->setCheckable(true);
  m_roiSplineAction->setStatusTip(tr("Make Spline Selections"));

  m_roiCutLineAction = new QAction(ZTheme::instance().icon(ZTheme::SplineCutIcon), tr("&Cut ROI"), this);
  m_roiCutLineAction->setCheckable(true);
  m_roiCutLineAction->setStatusTip(tr("Draw Line to Cut ROI"));

  m_dragModeActionGroup->addAction(m_roiSplineAction);
  m_dragModeActionGroup->addAction(m_roiPolygonAction);
  m_dragModeActionGroup->addAction(m_roiRectangleAction);
  m_dragModeActionGroup->addAction(m_roiEllipseAction);
  m_dragModeActionGroup->addAction(m_roiCutLineAction);
}

void ZView::updateViewportPara() const
{
  QRectF rect = m_view->getCurrrentlyVisibleRegion();
  m_viewport->blockSignals(true);
  m_viewport->set(glm::dvec4(rect.left(), rect.top(), rect.width(), rect.height()));
  m_viewport->blockSignals(false);
}

void ZView::updateSceneRectFromBoundBox()
{
  QRectF sceneRect(m_boundBox.minCorner.x,
                   m_boundBox.minCorner.y,
                   m_boundBox.maxCorner.x - m_boundBox.minCorner.x + 1,
                   m_boundBox.maxCorner.y - m_boundBox.minCorner.y + 1);
  if (sceneRect != m_scene->sceneRect()) {
    m_scene->setSceneRect(sceneRect);
  }

  if (m_montageColumns->get() == 0) {
    estimateMontageColumns();
  }
  auto nCols = m_montageColumns->get();
  auto nRows = (m_boundBox.maxCorner.z - m_boundBox.minCorner.z + nCols) / nCols;
  sceneRect = QRectF(sceneRect.x(), sceneRect.y(), sceneRect.width() * nCols, sceneRect.height() * nRows);
  if (sceneRect != m_montageScene->sceneRect()) {
    m_montageScene->setSceneRect(sceneRect);
  }
}

void ZView::updateMontageScene()
{
  if (currentViewStyle() != ViewStyle::Montage) {
    return;
  }

  m_scene->blockSignals(true);

  m_montageScene->clear();

  auto rgn = m_view->getCurrrentlyVisibleRegion();

  for (auto z = 0; z <= m_boundBox.maxCorner.z - m_boundBox.minCorner.z; ++z) {
    auto r = z / m_montageColumns->get();
    auto c = z % m_montageColumns->get();
    QRectF sceneRgn(c * m_scene->sceneRect().width(),
                    r * m_scene->sceneRect().height(),
                    m_scene->sceneRect().width(),
                    m_scene->sceneRect().height());
    if (!sceneRgn.intersects(rgn)) {
      continue;
    }
    int width = std::ceil(sceneRgn.width() * m_view->currentScale());
    int height = std::ceil(sceneRgn.height() * m_view->currentScale());
    QPixmap img(width, height);
    img.fill(Qt::black);
    QPainter painter(&img);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    for (const auto& view : m_objViews) {
      m_montageZ = z + m_boundBox.minCorner.z;
      view->setNormalView(m_montageZ, m_imgTime->get());
    }
    m_scene->render(&painter, QRectF(), QRect(), Qt::KeepAspectRatioByExpanding);
    auto item = new QGraphicsPixmapItem(img);
    item->setScale(1.0 / m_view->currentScale());
    item->setPos(sceneRgn.x(), sceneRgn.y());
    m_montageScene->addItem(item);
  }

  QApplication::processEvents();
  m_scene->blockSignals(false);
}

void ZView::emptyFun()
{
  VLOG(1) << "here";
}

} // namespace nim
