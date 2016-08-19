#include "z3dview.h"

#include "z3dcanvas.h"
#include "z3daxisfilter.h"
#include "z3dcompositor.h"
#include "z3dcanvaspainter.h"
#include "z3dcameraparameter.h"
#include "z3dinteractionhandler.h"
#include "z3dnetworkevaluator.h"
#include "zwidgetsgroup.h"
#include "zsysteminfo.h"
#include <QMessageBox>
#include <QProgressDialog>
#include <QMainWindow>
#include <QScrollArea>
#include "ztakescreenshotwidget.h"
#include "z3dgpuinfo.h"

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

namespace {
// generic solution
template<class T>
int numDigits(T number)
{
  int digits = 0;
  if (number < 0) digits = 1; // remove this line if '-' counts as a digit
  while (number) {
    number /= 10;
    digits++;
  }
  return digits;
}

// partial specialization optimization for 32-bit numbers
template<>
int numDigits(int32_t x)
{
  if (x == std::numeric_limits<int>::min()) return 10 + 1;
  if (x < 0) return numDigits(-x) + 1;

  if (x >= 10000) {
    if (x >= 10000000) {
      if (x >= 100000000) {
        if (x >= 1000000000)
          return 10;
        return 9;
      }
      return 8;
    }
    if (x >= 100000) {
      if (x >= 1000000)
        return 7;
      return 6;
    }
    return 5;
  }
  if (x >= 100) {
    if (x >= 1000)
      return 4;
    return 3;
  }
  if (x >= 10)
    return 2;
  return 1;
}

}

namespace nim {

Z3DView::Z3DView(ZDoc* doc, bool stereo, Z3DMainWindow* parent)
  : QObject(parent)
  , m_doc(doc)
  , m_isStereoView(stereo)
  , m_mainWin(parent)
  , m_boundBox(6)
  , m_numObjsBefore(m_doc->numObjs())
  , m_lock(false)
{
  CHECK(m_doc);
  m_canvas = new Z3DCanvas("", 512, 512);
  init();

  createActions();
}

Z3DView::~Z3DView()
{
  m_canvas->setNetworkEvaluator(nullptr);
}

std::shared_ptr<ZWidgetsGroup> Z3DView::viewSettingWidgetsGroupOf(size_t id)
{
  if (id == 1) {
    return m_compositor->backgroundWidgetsGroup();
  } else if (id == 2) {
    return m_compositor->axisWidgetsGroup();
  } else if (id == 3) {
    return m_globalParas.widgetsGroup(false);
  } else {
    for (int i = 0; i < m_3dObjViews.size(); ++i) {
      std::shared_ptr<ZWidgetsGroup> wg = m_3dObjViews[i]->viewSettingWidgetsGroupOf(id);
      if (wg)
        return wg;
    }
  }
  return std::shared_ptr<ZWidgetsGroup>();
}

QWidget* Z3DView::globalParasWidget()
{
  return m_globalParas.widgetsGroup(true)->createWidget(false);
}

QWidget* Z3DView::captureWidget()
{
  QScrollArea* res = new QScrollArea();
  ZTakeScreenShotWidget* m_screenShotWidget = new ZTakeScreenShotWidget(false, false, nullptr);
  m_screenShotWidget->setCaptureStereoImage(m_isStereoView);
  connect(m_screenShotWidget, &ZTakeScreenShotWidget::take3DScreenShot,
          this, &Z3DView::takeScreenShot);
  connect(m_screenShotWidget, &ZTakeScreenShotWidget::takeFixedSize3DScreenShot,
          this, &Z3DView::takeFixedSizeScreenShot);
  connect(m_screenShotWidget, &ZTakeScreenShotWidget::takeSeries3DScreenShot,
          this, &Z3DView::takeSeriesScreenShot);
  connect(m_screenShotWidget, &ZTakeScreenShotWidget::takeSeriesFixedSize3DScreenShot,
          this, &Z3DView::takeFixedSizeSeriesScreenShot);
  res->setWidget(m_screenShotWidget);
  return res;
}

QWidget* Z3DView::backgroundWidget()
{
  return m_compositor->backgroundWidgetsGroup()->createWidget(false);
}

QWidget* Z3DView::axisWidget()
{
  return m_compositor->axisWidgetsGroup()->createWidget(false);
}

void Z3DView::updateBoundBox()
{
  m_boundBox[0] = m_boundBox[2] = m_boundBox[4] = std::numeric_limits<double>::max();
  m_boundBox[1] = m_boundBox[3] = m_boundBox[5] = std::numeric_limits<double>::lowest();
  for (int i = 0; i < m_3dObjViews.size(); ++i) {
    std::vector<double> boundBox = m_3dObjViews[i]->boundBox();
    m_boundBox[0] = std::min(boundBox[0], m_boundBox[0]);
    m_boundBox[1] = std::max(boundBox[1], m_boundBox[1]);
    m_boundBox[2] = std::min(boundBox[2], m_boundBox[2]);
    m_boundBox[3] = std::max(boundBox[3], m_boundBox[3]);
    m_boundBox[4] = std::min(boundBox[4], m_boundBox[4]);
    m_boundBox[5] = std::max(boundBox[5], m_boundBox[5]);
  }
  if (m_boundBox[0] > m_boundBox[1] || m_boundBox[2] > m_boundBox[3] || m_boundBox[4] > m_boundBox[5]) {
    // nothing visible
    m_boundBox[0] = m_boundBox[2] = m_boundBox[4] = 0.0;
    m_boundBox[1] = m_boundBox[3] = m_boundBox[5] = 1.0;
  }
  m_boundBox[1] = std::max(m_boundBox[1], m_boundBox[0] + 1.0);
  m_boundBox[3] = std::max(m_boundBox[3], m_boundBox[2] + 1.0);
  m_boundBox[5] = std::max(m_boundBox[5], m_boundBox[4] + 1.0);
  if (m_numObjsBefore == 0 && m_doc->numObjs() > 0) {
    resetCamera();
  } else {
    resetCameraClippingRange();
  }
  m_numObjsBefore = m_doc->numObjs();
}

void Z3DView::read(size_t id, const QJsonObject& json)
{
  for (int i = 0; i < m_3dObjViews.size(); ++i) {
    if (m_3dObjViews[i]->hasObj(id)) {
      if (json.value("ViewObjType").toString() == m_3dObjViews[i]->doc().typeName()) {
        m_3dObjViews[i]->read(id, json);
      } else {
        LOG(WARNING) << "view object type " << json.value("ViewObjType").toString()
                     << " dones't match object type " << m_3dObjViews[i]->doc().typeName() << ". abort.";
      }
      return;
    }
  }
}

void Z3DView::write(size_t id, QJsonObject& json) const
{
  for (int i = 0; i < m_3dObjViews.size(); ++i) {
    if (m_3dObjViews[i]->hasObj(id)) {
      json.insert("ViewObjType", m_3dObjViews[i]->doc().typeName());
      json.insert("ViewVersion", QJsonValue(1.0));
      m_3dObjViews[i]->write(id, json);
      return;
    }
  }
}

void Z3DView::read(const QJsonObject& json)
{
  if (json.contains("Compositor") && json.value("Compositor").isObject()) {
    m_compositor->read(json.value("Compositor").toObject());
  }
  if (json.contains("Global") && json.value("Global").isObject()) {
    m_globalParas.read(json.value("Global").toObject());
  }
}

void Z3DView::write(QJsonObject& json) const
{
  QJsonObject compObj;
  m_compositor->write(compObj);
  json.insert("Compositor", compObj);

  QJsonObject globObj;
  m_globalParas.write(globObj);
  json.insert("Global", globObj);
}

void Z3DView::zoomIn()
{
  camera().dolly(1.1);
  //resetCameraClippingRange();
}

void Z3DView::zoomOut()
{
  camera().dolly(0.9);
  //resetCameraClippingRange();
}

void Z3DView::resetCamera()
{
  camera().resetCamera(m_boundBox, Z3DCamera::ResetOption::ResetAll);
}

void Z3DView::resetCameraClippingRange()
{
  if (m_lock)
    return;
  m_lock = true;
  camera().resetCameraNearFarPlane(m_boundBox);
  m_lock = false;
}

bool Z3DView::takeFixedSizeScreenShot(QString filename, int width, int height, Z3DScreenShotType sst)
{
  bool res = true;
  m_lock = true;
  if (!m_canvasPainter->renderToImage(filename, width, height, sst, compositor())) {
    res = false;
    QMessageBox::critical(m_mainWin, qApp->applicationName(), m_canvasPainter->renderToImageError());
  }
  m_lock = false;
  return res;
}

bool Z3DView::takeScreenShot(QString filename, Z3DScreenShotType sst)
{
  int h = m_canvas->height();
  if (h % 2 == 1) {
    ++h;
  }
  int w = m_canvas->width();
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
    QMessageBox::critical(m_mainWin, qApp->applicationName(), m_canvasPainter->renderToImageError());
  }
  return res;
}

bool Z3DView::takeFixedSizeSeriesScreenShot(const QDir& dir, const QString& namePrefix, glm::vec3 axis,
                                            bool clockWise, int numFrame, int width, int height, Z3DScreenShotType sst)
{
  using namespace boost::math::double_constants;
  QString title = "Capturing Images...";
  if (sst == Z3DScreenShotType::HalfSideBySideStereoView)
    title = "Capturing Half Side-By-Side Stereo Images...";
  else if (sst == Z3DScreenShotType::FullSideBySideStereoView)
    title = "Capturing Full Side-By-Side Stereo Images...";
  QProgressDialog progress(title, "Cancel", 0, numFrame, m_mainWin);
  progress.setWindowModality(Qt::WindowModal);
  progress.show();
  double rAngle = two_pi / numFrame;
  bool res = true;
  for (int i = 0; i < numFrame; ++i) {
    progress.setValue(i);
    if (progress.wasCanceled())
      break;

    if (clockWise)
      camera().rotate(rAngle, camera().get().vectorEyeToWorld(axis), camera().get().center());
    else
      camera().rotate(-rAngle, camera().get().vectorEyeToWorld(axis), camera().get().center());
    //resetCameraClippingRange();
    int fieldWidth = numDigits(numFrame);
    QString filename = QString("%1%2.tif").arg(namePrefix).arg(i, fieldWidth, 10, QChar('0'));
    QString filepath = dir.filePath(filename);
    if (!takeFixedSizeScreenShot(filepath, width, height, sst)) {
      res = false;
      break;
    }
  }
  progress.setValue(numFrame);
  return res;
}

bool Z3DView::takeSeriesScreenShot(const QDir& dir, const QString& namePrefix, glm::vec3 axis,
                                   bool clockWise, int numFrame, Z3DScreenShotType sst)
{
  using namespace boost::math::double_constants;
  QString title = "Capturing Images...";
  if (sst == Z3DScreenShotType::HalfSideBySideStereoView)
    title = "Capturing Half Side-By-Side Stereo Images...";
  else if (sst == Z3DScreenShotType::FullSideBySideStereoView)
    title = "Capturing Full Side-By-Side Stereo Images...";
  QProgressDialog progress(title, "Cancel", 0, numFrame, m_mainWin);
  progress.setWindowModality(Qt::WindowModal);
  progress.show();
  double rAngle = two_pi / numFrame;
  bool res = true;
  for (int i = 0; i < numFrame; ++i) {
    progress.setValue(i);
    if (progress.wasCanceled())
      break;

    if (clockWise)
      camera().rotate(rAngle, camera().get().vectorEyeToWorld(axis), camera().get().center());
    else
      camera().rotate(-rAngle, camera().get().vectorEyeToWorld(axis), camera().get().center());
    //resetCameraClippingRange();
    int fieldWidth = numDigits(numFrame);
    QString filename = QString("%1%2.tif").arg(namePrefix).arg(i, fieldWidth, 10, QChar('0'));
    QString filepath = dir.filePath(filename);
    if (!takeScreenShot(filepath, sst)) {
      res = false;
      break;
    }
  }
  progress.setValue(numFrame);
  return res;
}

void Z3DView::init()
{
  m_globalParas.setCanvas(m_canvas);

  // filters
  m_compositor.reset(new Z3DCompositor(m_globalParas));
  //ZStringIntOptionParameter* transparentMethod = dynamic_cast<ZStringIntOptionParameter*>(m_compositor->getParameter("Transparency"));
  //if (Z3DGpuInfoInstance.isWeightedAverageSupported())
  //transparentMethod->select("Weighted Average");

  m_canvasPainter.reset(new Z3DCanvasPainter(m_globalParas));
  m_canvasPainter->setCanvas(m_canvas);

  m_canvas->addEventListenerToBack(m_compositor.get());  // for interaction

  m_compositor->outputPort("Image")->connect(m_canvasPainter->inputPort("Image"));
  m_compositor->outputPort("LeftEyeImage")->connect(m_canvasPainter->inputPort("LeftEyeImage"));
  m_compositor->outputPort("RightEyeImage")->connect(m_canvasPainter->inputPort("RightEyeImage"));

  // connection: canvas <-----> networkevaluator <-----> canvasrender
  m_networkEvaluator.reset(new Z3DNetworkEvaluator());
  m_canvas->setNetworkEvaluator(m_networkEvaluator.get());

  // pass the canvasrender to the network evaluator
  m_networkEvaluator->setNetworkSink(m_canvasPainter.get());

  // initializes all connected filters
  m_networkEvaluator->initializeNetwork();

  //packages
  QList<ZObjDoc*> objDocs = m_doc->objDocs();
  for (int i = 0; i < objDocs.size(); ++i) {
    if (objDocs[i]->typeName() == "Image") {
      ZImgDoc* imgDoc = qobject_cast<ZImgDoc*>(objDocs[i]);
      Z3DImgView* imgView = new Z3DImgView(*imgDoc, *this);
      connect(imgView, &Z3DImgView::objViewReady, this, &Z3DView::objViewReady);
      m_3dObjViews.push_back(imgView);
    } else if (objDocs[i]->typeName() == "Puncta") {
      ZPunctaDoc* punctaDoc = qobject_cast<ZPunctaDoc*>(objDocs[i]);
      Z3DPunctaView* punctaView = new Z3DPunctaView(*punctaDoc, *this);
      connect(punctaView, &Z3DPunctaView::objViewReady, this, &Z3DView::objViewReady);
      m_3dObjViews.push_back(punctaView);
    } else if (objDocs[i]->typeName() == "Swc") {
      ZSwcDoc* swcDoc = qobject_cast<ZSwcDoc*>(objDocs[i]);
      Z3DSwcView* swcView = new Z3DSwcView(*swcDoc, *this);
      connect(swcView, &Z3DSwcView::objViewReady, this, &Z3DView::objViewReady);
      m_3dObjViews.push_back(swcView);
    } else if (objDocs[i]->typeName() == "Mesh") {
      ZMeshDoc* meshDoc = qobject_cast<ZMeshDoc*>(objDocs[i]);
      Z3DMeshView* meshView = new Z3DMeshView(*meshDoc, *this);
      connect(meshView, &Z3DMeshView::objViewReady, this, &Z3DView::objViewReady);
      m_3dObjViews.push_back(meshView);
    } else if (objDocs[i]->typeName() == "Animation3D") {
      Z3DAnimationDoc* aniDoc = qobject_cast<Z3DAnimationDoc*>(objDocs[i]);
      Z3DAnimationView* aniView = new Z3DAnimationView(*aniDoc, *this);
      connect(aniView, &Z3DAnimationView::objViewReady, this, &Z3DView::objViewReady);
      m_3dObjViews.push_back(aniView);
    } else if (objDocs[i]->typeName() == "RegionAnnotation") {
      ZRegionAnnotationDoc* aniDoc = qobject_cast<ZRegionAnnotationDoc*>(objDocs[i]);
      Z3DRegionAnnotationView* aniView = new Z3DRegionAnnotationView(*aniDoc, *this);
      connect(aniView, &Z3DRegionAnnotationView::objViewReady, this, &Z3DView::objViewReady);
      m_3dObjViews.push_back(aniView);
    }
  }

  updateBoundBox();

  // adjust camera
  resetCamera();

  connect(&camera(), &Z3DCameraParameter::valueChanged, this, &Z3DView::resetCameraClippingRange);
}

void Z3DView::createActions()
{
  m_zoomInAction = new QAction(QIcon(":/icons/zoom_in-512.png"), tr("Zoom &In"), this);
  QList<QKeySequence> zoomInKey;
  zoomInKey << QKeySequence::ZoomIn << QKeySequence(Qt::Key_Plus) << QKeySequence(Qt::Key_Equal);
  m_zoomInAction->setShortcuts(zoomInKey);
  m_zoomInAction->setStatusTip(tr("Zoom in"));
  connect(m_zoomInAction, &QAction::triggered, this, &Z3DView::zoomIn);

  m_zoomOutAction = new QAction(QIcon(":/icons/zoom_out-512.png"), tr("Zoom &Out"), this);
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
