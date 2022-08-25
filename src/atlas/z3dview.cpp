#include "z3dview.h"

#include "z3dcanvas.h"
#include "z3dcompositor.h"
#include "z3dcanvaspainter.h"
#include "z3dcameraparameter.h"
#include "z3dinteractionhandler.h"
#include "z3dnetworkevaluator.h"
#include "zwidgetsgroup.h"
#include "ztakescreenshotwidget.h"
#include "zimgdoc.h"
#include "z3dimgview.h"
#include "zpunctadoc.h"
#include "z3dpunctaview.h"
#include "zswcdoc.h"
#include "z3dswcview.h"
#include "zmeshdoc.h"
#include "z3dmeshview.h"
#include "z3danimationdoc.h"
#include "z3danimationview.h"
#include "z3dregionannotationview.h"
#include "z3dmainwindow.h"
#include "ztheme.h"
#include <QMessageBox>
#include <QPlainTextEdit>
#include <memory>

namespace nim {

Z3DView::Z3DView(ZDoc& doc, bool stereo, Z3DMainWindow* parent)
  : QObject(parent)
  , m_doc(doc)
  , m_isStereoView(stereo)
  , m_mainWin(parent)
  , m_numObjsBefore(m_doc.numObjs())
{
  m_canvas = new Z3DCanvas("", 512, 512, m_mainWin);

  createActions();

  connect(m_canvas, &Z3DCanvas::openGLContextInitialized, this, &Z3DView::init);

  connect(&m_doc,
          &ZDoc::requestToAdjustViewToPosition,
          this,
          qOverload<double, double, double, double>(&Z3DView::cameraFocusesOn));
}

Z3DView::~Z3DView()
{
  m_canvas->getGLFocus();
}

const ZDoc& Z3DView::doc() const
{
  return m_doc;
}

std::shared_ptr<ZWidgetsGroup> Z3DView::viewSettingWidgetsGroupOf(size_t id)
{
  if (id == 1) {
    return m_compositor->backgroundWidgetsGroup();
  } else if (id == 2) {
    return m_compositor->axisWidgetsGroup();
  } else if (id == 3) {
    return m_globalParas->widgetsGroup(false);
  } else {
    for (auto& m_3dObjView : m_3dObjViews) {
      std::shared_ptr<ZWidgetsGroup> wg = m_3dObjView->viewSettingWidgetsGroupOf(id);
      if (wg) {
        return wg;
      }
    }
  }
  return {};
}

QWidget* Z3DView::globalParasWidget()
{
  return m_globalParas->widgetsGroup(true)->createWidget(false);
}

QWidget* Z3DView::captureWidget() const
{
  // auto res = new QScrollArea();
  auto m_screenShotWidget = new ZTakeScreenShotWidget(false, false, nullptr);
  m_screenShotWidget->setCaptureStereoImage(m_isStereoView);
  connect(m_screenShotWidget, &ZTakeScreenShotWidget::take3DScreenShot, this, &Z3DView::takeScreenShot);
  connect(m_screenShotWidget,
          &ZTakeScreenShotWidget::takeFixedSize3DScreenShot,
          this,
          &Z3DView::takeFixedSizeScreenShot);
  //  connect(m_screenShotWidget, &ZTakeScreenShotWidget::takeSeries3DScreenShot,
  //          this, &Z3DView::takeSeriesScreenShot);
  //  connect(m_screenShotWidget, &ZTakeScreenShotWidget::takeSeriesFixedSize3DScreenShot,
  //          this, &Z3DView::takeFixedSizeSeriesScreenShot);
  // res->setWidget(m_screenShotWidget);

  return m_screenShotWidget;
}

QWidget* Z3DView::backgroundWidget()
{
  return m_compositor->backgroundWidgetsGroup()->createWidget(false);
}

QWidget* Z3DView::axisWidget()
{
  return m_compositor->axisWidgetsGroup()->createWidget(false);
}

QWidget* Z3DView::helpWidget()
{
  auto edt = new QPlainTextEdit();
  edt->setReadOnly(true);
  edt->appendPlainText("zoom/dolly:");
  edt->appendPlainText("    1) command/control key + mouse wheel scroll");
  edt->appendPlainText("    2) command/control key + =(+)/- key");
  // edt->appendPlainText("    3) mouse wheel scroll (might be slow if image is rendered in full-resolution)");
  // edt->appendPlainText("    4) =(+)/- key (might be slow if image is rendered in full-resolution)");
  edt->appendPlainText("rotate:");
  edt->appendPlainText("    1) [(optional) command/control key] + mouse drag");
  edt->appendPlainText("    2) command/control key + Left/Right/Up/Down key");
  edt->appendPlainText("shift:");
  edt->appendPlainText("    1) shift key + mouse drag");
  edt->appendPlainText("    2) shift key + Left/Right/Up/Down key");
  edt->appendPlainText("roll:");
  edt->appendPlainText("    1) alt key + mouse drag");
  edt->appendPlainText("    2) alt key + Left/Right key");
  edt->moveCursor(QTextCursor::Start);
  edt->ensureCursorVisible();
  return edt;
}

void Z3DView::updateBoundBox()
{
  m_boundBox.reset();
  for (auto& m_3dObjView : m_3dObjViews) {
    m_boundBox.expand(m_3dObjView->boundBox());
  }
  if (m_boundBox.empty()) {
    // nothing visible
    m_boundBox.setMinCorner(glm::dvec3(0.0));
    m_boundBox.setMaxCorner(glm::dvec3(.01));
  }
  m_boundBox.setMaxCorner(glm::max(m_boundBox.maxCorner, m_boundBox.minCorner + .01));
  if (m_numObjsBefore == 0 && m_doc.numObjs() > 0) {
    resetCamera();
  } else {
    resetCameraClippingRange();
  }
  m_numObjsBefore = m_doc.numObjs();

  LOG(INFO) << json::value_from(m_boundBox);
  // update global cut
  m_globalParas->xCut.setRangeKeepIfMinMax(std::floor(m_boundBox.minCorner.x) - 1,
                                           std::ceil(m_boundBox.maxCorner.x) + 1);

  m_globalParas->yCut.setRangeKeepIfMinMax(std::floor(m_boundBox.minCorner.y) - 1,
                                           std::ceil(m_boundBox.maxCorner.y) + 1);

  m_globalParas->zCut.setRangeKeepIfMinMax(std::floor(m_boundBox.minCorner.z) - 1,
                                           std::ceil(m_boundBox.maxCorner.z) + 1);
}

void Z3DView::read(size_t id, const json::object& json)
{
  for (auto& m_3dObjView : m_3dObjViews) {
    if (m_3dObjView->hasObj(id)) {
      if (asQString(json.at("ViewObjType")) == m_3dObjView->doc().typeName()) {
        m_3dObjView->read(id, json);
      } else {
        LOG(WARNING) << "view object type " << asQString(json.at("ViewObjType")) << " dones't match object type "
                     << m_3dObjView->doc().typeName() << ". abort.";
      }
      return;
    }
  }
}

void Z3DView::write(size_t id, json::object& json) const
{
  for (auto m_3dObjView : m_3dObjViews) {
    if (m_3dObjView->hasObj(id)) {
      json["ViewObjType"] = json::value_from(m_3dObjView->doc().typeName());
      json["ViewVersion"] = 1.0;
      m_3dObjView->write(id, json);
      return;
    }
  }
}

void Z3DView::read(const json::object& json)
{
  if (json.contains("Compositor") && json.at("Compositor").is_object()) {
    m_compositor->read(json.at("Compositor").as_object());
  }
  if (json.contains("Global") && json.at("Global").is_object()) {
    m_globalParas->read(json.at("Global").as_object());
  }
}

void Z3DView::write(json::object& json) const
{
  json::object compObj;
  m_compositor->write(compObj);
  json["Compositor"] = compObj;

  json::object globObj;
  m_globalParas->write(globObj);
  json["Global"] = globObj;
}

void Z3DView::zoomIn()
{
  camera().dolly(1.1);
  // resetCameraClippingRange();
}

void Z3DView::zoomOut()
{
  camera().dolly(0.9);
  // resetCameraClippingRange();
}

void Z3DView::resetCamera()
{
  camera().resetCamera(m_boundBox, Z3DCamera::ResetOption::ResetAll);
}

void Z3DView::resetCameraCenter()
{
  camera().resetCamera(m_boundBox, Z3DCamera::ResetOption::PreserveViewVector);
}

void Z3DView::resetCameraClippingRange()
{
  if (!m_mutex.try_lock()) {
    return;
  }
  camera().resetCameraNearFarPlane(m_boundBox);
  m_mutex.unlock();
}

bool Z3DView::takeFixedSizeScreenShot(const QString& filename, int width, int height, Z3DScreenShotType sst)
{
  QMutexLocker locker(&m_mutex);
  bool res = true;
  if (!m_canvasPainter->renderToImage(filename, width, height, sst, compositor())) {
    res = false;
    QMessageBox::critical(m_mainWin, QApplication::applicationName(), m_canvasPainter->renderToImageError());
  }
  m_canvasPainter->resetToMatchCanvasSize();
  return res;
}

bool Z3DView::takeFixedSizeScreenShotWithoutResetCanvasPainterSize(const QString& filename,
                                                                   int width,
                                                                   int height,
                                                                   Z3DScreenShotType sst)
{
  bool res = true;
  if (!m_canvasPainter->renderToImage(filename, width, height, sst, compositor())) {
    res = false;
    QMessageBox::critical(m_mainWin, QApplication::applicationName(), m_canvasPainter->renderToImageError());
  }
  return res;
}

void Z3DView::resetCanvasPainterSize()
{
  m_canvasPainter->resetToMatchCanvasSize();
}

bool Z3DView::takeScreenShot(const QString& filename, Z3DScreenShotType sst)
{
  auto h = m_canvas->height();
  if (h % 2 == 1) {
    ++h;
  }
  auto w = m_canvas->width();
  if (w % 2 == 1) {
    ++w;
  }
  if (m_canvas->width() % 2 == 1 || m_canvas->height() % 2 == 1) {
    LOG(INFO) << "Resize canvas size from (" << m_canvas->width() << ", " << m_canvas->height() << ") to (" << w << ", "
              << h << ").";
    m_canvas->resize(w, h);
  }
  bool res = true;
  if (!m_canvasPainter->renderToImage(filename, sst)) {
    res = false;
    QMessageBox::critical(m_mainWin, QApplication::applicationName(), m_canvasPainter->renderToImageError());
  }
  return res;
}

ZBBox<glm::dvec3> Z3DView::boundBoxOfObjs(const std::vector<size_t>& ids) const
{
  ZBBox<glm::dvec3> res;
  for (auto id : ids) {
    for (auto objView : m_3dObjViews) {
      if (objView->hasObj(id)) {
        res.expand(objView->boundBoxOfObj(id));
      }
    }
  }
  return res;
}

ZBBox<glm::dvec3> Z3DView::boundBoxOfObjsAfterClipping(const std::vector<size_t>& ids) const
{
  ZBBox<glm::dvec3> res;
  for (auto id : ids) {
    for (auto objView : m_3dObjViews) {
      if (objView->hasObj(id)) {
        res.expand(objView->boundBoxOfObjAfterClipping(id));
      }
    }
  }
  return res;
}

void Z3DView::flipView()
{
  camera().flipViewDirection();
}

void Z3DView::setXZView()
{
  resetCamera();
  camera().rotate90X();
  resetCameraClippingRange();
}

void Z3DView::setYZView()
{
  resetCamera();
  camera().rotate90XZ();
  resetCameraClippingRange();
}

// bool Z3DView::takeFixedSizeSeriesScreenShot(const QDir& dir, const QString& namePrefix, const glm::vec3& axis,
//                                             bool clockWise, int numFrame, int width, int height, Z3DScreenShotType
//                                             sst)
//{
//   using namespace boost::math::double_constants;
//   QString title = "Capturing Images...";
//   if (sst == Z3DScreenShotType::HalfSideBySideStereoView) {
//     title = "Capturing Half Side-By-Side Stereo Images...";
//   } else if (sst == Z3DScreenShotType::FullSideBySideStereoView) {
//     title = "Capturing Full Side-By-Side Stereo Images...";
//   }
//   QProgressDialog progress(title, "Cancel", 0, numFrame, m_mainWin);
//   progress.setWindowModality(Qt::WindowModal);
//   progress.show();
//   double rAngle = two_pi / numFrame;
//   bool res = true;
//   for (auto i = 0; i < numFrame; ++i) {
//     progress.setValue(i);
//     if (progress.wasCanceled())
//       break;
//
//     if (clockWise)
//       camera().rotate(rAngle, camera().get().vectorEyeToWorld(axis), camera().get().center());
//     else
//       camera().rotate(-rAngle, camera().get().vectorEyeToWorld(axis), camera().get().center());
//     //resetCameraClippingRange();
//     auto fieldWidth = numDigits(numFrame);
//     auto filename = QString("%1%2.png").arg(namePrefix).arg(i, fieldWidth, 10, QChar('0'));
//     auto filepath = dir.filePath(filename);
//     if (!takeFixedSizeScreenShot(filepath, width, height, sst)) {
//       res = false;
//       break;
//     }
//   }
//   progress.setValue(numFrame);
//   return res;
// }
//
// bool Z3DView::takeSeriesScreenShot(const QDir& dir, const QString& namePrefix, const glm::vec3& axis,
//                                    bool clockWise, int numFrame, Z3DScreenShotType sst)
//{
//   using namespace boost::math::double_constants;
//   QString title = "Capturing Images...";
//   if (sst == Z3DScreenShotType::HalfSideBySideStereoView) {
//     title = "Capturing Half Side-By-Side Stereo Images...";
//   } else if (sst == Z3DScreenShotType::FullSideBySideStereoView) {
//     title = "Capturing Full Side-By-Side Stereo Images...";
//   }
//   QProgressDialog progress(title, "Cancel", 0, numFrame, m_mainWin);
//   progress.setWindowModality(Qt::WindowModal);
//   progress.show();
//   double rAngle = two_pi / numFrame;
//   bool res = true;
//   for (auto i = 0; i < numFrame; ++i) {
//     progress.setValue(i);
//     if (progress.wasCanceled())
//       break;
//
//     if (clockWise)
//       camera().rotate(rAngle, camera().get().vectorEyeToWorld(axis), camera().get().center());
//     else
//       camera().rotate(-rAngle, camera().get().vectorEyeToWorld(axis), camera().get().center());
//     //resetCameraClippingRange();
//     auto fieldWidth = numDigits(numFrame);
//     auto filename = QString("%1%2.png").arg(namePrefix).arg(i, fieldWidth, 10, QChar('0'));
//     auto filepath = dir.filePath(filename);
//     if (!takeScreenShot(filepath, sst)) {
//       res = false;
//       break;
//     }
//   }
//   progress.setValue(numFrame);
//   return res;
// }

void Z3DView::init()
{
  m_canvas->getGLFocus();
  m_globalParas = std::make_unique<Z3DGlobalParameters>(*m_canvas, *this);

  // filters
  m_canvasPainter = std::make_unique<Z3DCanvasPainter>(*m_globalParas, *m_canvas);

  m_compositor = std::make_unique<Z3DCompositor>(*m_globalParas);
  m_compositor->outputPort("Image")->connect(m_canvasPainter->inputPort("Image"));
  m_compositor->outputPort("LeftEyeImage")->connect(m_canvasPainter->inputPort("LeftEyeImage"));
  m_compositor->outputPort("RightEyeImage")->connect(m_canvasPainter->inputPort("RightEyeImage"));
  m_canvas->addEventListenerToBack(*m_compositor);

  // build network and connect to canvas
  m_networkEvaluator = std::make_unique<Z3DNetworkEvaluator>(*m_canvasPainter);

  // packages
  for (auto objDoc : m_doc.objDocs()) {
    if (auto imgDoc = qobject_cast<ZImgDoc*>(objDoc)) {
      auto imgView = new Z3DImgView(*imgDoc, *this);
      connect(imgView, &Z3DImgView::objViewReady, this, &Z3DView::objViewReady);
      m_3dObjViews.push_back(imgView);
    } else if (auto punctaDoc = qobject_cast<ZPunctaDoc*>(objDoc)) {
      auto punctaView = new Z3DPunctaView(*punctaDoc, *this);
      connect(punctaView, &Z3DPunctaView::objViewReady, this, &Z3DView::objViewReady);
      m_3dObjViews.push_back(punctaView);
    } else if (auto swcDoc = qobject_cast<ZSwcDoc*>(objDoc)) {
      auto swcView = new Z3DSwcView(*swcDoc, *this);
      connect(swcView, &Z3DSwcView::objViewReady, this, &Z3DView::objViewReady);
      m_3dObjViews.push_back(swcView);
    } else if (auto meshDoc = qobject_cast<ZMeshDoc*>(objDoc)) {
      auto meshView = new Z3DMeshView(*meshDoc, *this);
      connect(meshView, &Z3DMeshView::objViewReady, this, &Z3DView::objViewReady);
      m_3dObjViews.push_back(meshView);
    } else if (auto aniDoc = qobject_cast<Z3DAnimationDoc*>(objDoc)) {
      auto aniView = new Z3DAnimationView(*aniDoc, *this);
      connect(aniView, &Z3DAnimationView::objViewReady, this, &Z3DView::objViewReady);
      m_3dObjViews.push_back(aniView);
    } else if (auto raDoc = qobject_cast<ZRegionAnnotationDoc*>(objDoc)) {
      auto aniView = new Z3DRegionAnnotationView(*raDoc, *this);
      connect(aniView, &Z3DRegionAnnotationView::objViewReady, this, &Z3DView::objViewReady);
      m_3dObjViews.push_back(aniView);
    }
  }

  updateBoundBox();

  // adjust camera
  resetCamera();

  connect(&camera(), &Z3DCameraParameter::valueChanged, this, &Z3DView::resetCameraClippingRange);

  Q_EMIT networkConstructed();
}

void Z3DView::createActions()
{
  m_zoomInAction = new QAction(ZTheme::instance().icon(ZTheme::ZoomInIcon), tr("Zoom &In"), this);
  QList<QKeySequence> zoomInKey;
  zoomInKey << QKeySequence::ZoomIn << QKeySequence(Qt::Key_Plus) << QKeySequence(Qt::Key_Equal);
  m_zoomInAction->setShortcuts(zoomInKey);
  m_zoomInAction->setStatusTip(tr("Zoom in"));
  connect(m_zoomInAction, &QAction::triggered, this, &Z3DView::zoomIn);

  m_zoomOutAction = new QAction(ZTheme::instance().icon(ZTheme::ZoomOutIcon), tr("Zoom &Out"), this);
  QList<QKeySequence> zoomOutKey;
  zoomOutKey << QKeySequence::ZoomOut << QKeySequence(Qt::Key_Minus);
  m_zoomOutAction->setShortcuts(zoomOutKey);
  m_zoomOutAction->setStatusTip(tr("Zoom out"));
  connect(m_zoomOutAction, &QAction::triggered, this, &Z3DView::zoomOut);

  m_resetCameraAction = new QAction(tr("&Reset Camera"), this);
  m_resetCameraAction->setStatusTip(tr("Reset camera to show all objects in scene"));
  connect(m_resetCameraAction, &QAction::triggered, this, &Z3DView::resetCamera);
}

} // namespace nim
