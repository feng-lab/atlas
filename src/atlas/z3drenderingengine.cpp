#include "z3drenderingengine.h"

#include "z3dcanvas.h"
#include "z3dcompositor.h"
#include "z3dcameraparameter.h"
#include "z3dinteractionhandler.h"
#include "z3dnetworkevaluator.h"
#include "zwidgetsgroup.h"
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
#include "zimgformat.h"
#include "ztheme.h"
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QApplication>
#include <memory>

namespace nim {

Z3DRenderingEngine::Z3DRenderingEngine(ZDoc& doc, bool stereo, QObject* parent)
  : QObject(parent)
  , m_doc(doc)
  , m_isStereoView(stereo)
  , m_numObjsBefore(m_doc.numObjs())
{
  createActions();

  connect(&m_doc,
          &ZDoc::requestToAdjustViewToPosition,
          this,
          qOverload<double, double, double, double>(&Z3DRenderingEngine::cameraFocusesOn));
}

Z3DRenderingEngine::~Z3DRenderingEngine()
{
  m_canvas->getGLFocus();
}

const ZDoc& Z3DRenderingEngine::doc() const
{
  return m_doc;
}

std::shared_ptr<ZWidgetsGroup> Z3DRenderingEngine::viewSettingWidgetsGroupOf(size_t id)
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

QWidget* Z3DRenderingEngine::globalParasWidget()
{
  return m_globalParas->widgetsGroup(true)->createWidget(false);
}

QWidget* Z3DRenderingEngine::backgroundWidget()
{
  return m_compositor->backgroundWidgetsGroup()->createWidget(false);
}

QWidget* Z3DRenderingEngine::axisWidget()
{
  return m_compositor->axisWidgetsGroup()->createWidget(false);
}

QWidget* Z3DRenderingEngine::helpWidget()
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

void Z3DRenderingEngine::updateBoundBox()
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

void Z3DRenderingEngine::read(size_t id, const json::object& json)
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

void Z3DRenderingEngine::write(size_t id, json::object& json) const
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

void Z3DRenderingEngine::read(const json::object& json)
{
  if (json.contains("Compositor") && json.at("Compositor").is_object()) {
    m_compositor->read(json.at("Compositor").as_object());
  }
  if (json.contains("Global") && json.at("Global").is_object()) {
    m_globalParas->read(json.at("Global").as_object());
  }
}

void Z3DRenderingEngine::write(json::object& json) const
{
  json::object compObj;
  m_compositor->write(compObj);
  json["Compositor"] = compObj;

  json::object globObj;
  m_globalParas->write(globObj);
  json["Global"] = globObj;
}

void Z3DRenderingEngine::zoomIn()
{
  camera().dolly(1.1);
  // resetCameraClippingRange();
}

void Z3DRenderingEngine::zoomOut()
{
  camera().dolly(0.9);
  // resetCameraClippingRange();
}

void Z3DRenderingEngine::resetCamera()
{
  camera().resetCamera(m_boundBox, Z3DCamera::ResetOption::ResetAll);
}

void Z3DRenderingEngine::resetCameraCenter()
{
  camera().resetCamera(m_boundBox, Z3DCamera::ResetOption::PreserveViewVector);
}

void Z3DRenderingEngine::resetCameraClippingRange()
{
  if (!m_mutex.try_lock()) {
    return;
  }
  camera().resetCameraNearFarPlane(m_boundBox);
  m_mutex.unlock();
}

void Z3DRenderingEngine::takeFixedSizeScreenShot(const QString& filename, int width, int height, Z3DScreenShotType sst)
{
  takeFixedSizeScreenShotWithoutResetCanvasSize(filename, width, height, sst);
  resetCanvasSize();
}

void Z3DRenderingEngine::takeFixedSizeScreenShotWithoutResetCanvasSize(const QString& filename,
                                                                       int width,
                                                                       int height,
                                                                       Z3DScreenShotType sst)
{
  const int tileSize = 7680; // 2048;
  const int tileBorder = 128;
  const auto tileInnerSize = tileSize - 2 * tileBorder;

  if (width <= tileSize && height <= tileSize) {
    // resize texture container to desired image dimensions and propagate change
    setOutputSize(glm::uvec2(width, height));

    takeScreenShot(filename, sst);
  } else {
    m_globalParas->camera.viewportChanged(glm::uvec2(width, height));
    setOutputSize(glm::uvec2(tileSize, tileSize));

    ZImg img(ZImgInfo(width, height, 1, 4));
    ZImg rightImg;
    if (sst != Z3DScreenShotType::MonoView) {
      rightImg = ZImg(ZImgInfo(width, height, 1, 4));
    }

    auto numCols = (width + tileInnerSize - 1) / tileInnerSize;
    auto numRows = (height + tileInnerSize - 1) / tileInnerSize;
    for (auto c = 0; c < numCols; ++c) {
      for (auto r = 0; r < numRows; ++r) {
        auto m_tileStartX = c * tileInnerSize - tileBorder;
        auto m_tileStartY = r * tileInnerSize - tileBorder;
        double left = m_tileStartX / 1.0 / width;
        double right = (m_tileStartX + tileSize) / 1.0 / width;
        double bottom = m_tileStartY / 1.0 / height;
        double top = (m_tileStartY + tileSize) / 1.0 / height;

        // set camera frustum
        m_globalParas->camera.setTileFrustum(left, right, bottom, top);
        m_compositor->setRenderingRegion(left, right, bottom, top);
        // LOG(INFO) << globalCameraPara().get().left() << globalCameraPara().get().right() <<
        // globalCameraPara().get().top() << globalCameraPara().get().bottom();

        m_networkEvaluator->process(sst != Z3DScreenShotType::MonoView);

        if (sst == Z3DScreenShotType::MonoView) {
          auto tmpImg = textureToRGBAImg(*m_compositor->monoReadyTarget()->colorTexture());
          img.pasteImg(tmpImg, ZVoxelCoordinate(m_tileStartX, m_tileStartY));
        } else {
          auto tmpImg = textureToRGBAImg(*m_compositor->leftReadyTarget()->colorTexture());
          img.pasteImg(tmpImg, ZVoxelCoordinate(m_tileStartX, m_tileStartY));
          tmpImg = textureToRGBAImg(*m_compositor->rightReadyTarget()->colorTexture());
          rightImg.pasteImg(tmpImg, ZVoxelCoordinate(m_tileStartX, m_tileStartY));
        }
      }
    }

    if (sst == Z3DScreenShotType::MonoView) {
      img.flip(Dimension::Y).save(filename);
      LOG(INFO) << "Saved rendering (" << img.width() << ", " << img.height() << ") to file: " << filename;
    } else {
      if (sst == Z3DScreenShotType::FullSideBySideStereoView) {
        ZImg::cat(std::vector<const ZImg*>{&img, &rightImg}, Dimension::X).flip(Dimension::Y).save(filename);
        LOG(INFO) << "Saved stereo rendering (" << img.width() << " x 2, " << img.height() << ") to file: " << filename;
      } else {
        ZImg::cat(std::vector<const ZImg*>{&img, &rightImg}, Dimension::X)
          .zoom(0.5, 1)
          .flip(Dimension::Y)
          .save(filename);
        LOG(INFO) << "Saved half sbs stereo rendering (" << img.width() << ", " << img.height()
                  << ") to file:" << filename;
      }
    }

    m_globalParas->camera.setTileFrustum();
    m_compositor->setRenderingRegion();
  }
}

void Z3DRenderingEngine::resetCanvasSize()
{
  if (m_canvas) {
    m_compositor->setOutputSize(m_canvas->physicalSize());
  }
}

void Z3DRenderingEngine::takeScreenShot(const QString& filename, Z3DScreenShotType sst)
{
  m_networkEvaluator->process(sst != Z3DScreenShotType::MonoView);

  if (sst == Z3DScreenShotType::MonoView) {
    auto img = textureToRGBAImg(*m_compositor->monoReadyTarget()->colorTexture());
    img.flip(Dimension::Y).save(filename);
    LOG(INFO) << "Saved rendering (" << img.width() << ", " << img.height() << ") to file: " << filename;
  } else {
    auto leftImg = textureToRGBAImg(*m_compositor->leftReadyTarget()->colorTexture());
    auto rightImg = textureToRGBAImg(*m_compositor->rightReadyTarget()->colorTexture());

    if (sst == Z3DScreenShotType::FullSideBySideStereoView) {
      ZImg::cat(std::vector<const ZImg*>{&leftImg, &rightImg}, Dimension::X).flip(Dimension::Y).save(filename);
      LOG(INFO) << "Saved stereo rendering (" << leftImg.width() << " x 2, " << leftImg.height()
                << ") to file: " << filename;
    } else {
      ZImg::cat(std::vector<const ZImg*>{&leftImg, &rightImg}, Dimension::X)
        .zoom(0.5, 1)
        .flip(Dimension::Y)
        .save(filename);
      LOG(INFO) << "Saved half sbs stereo rendering (" << leftImg.width() << ", " << leftImg.height()
                << ") to file:" << filename;
    }
  }
}

ZBBox<glm::dvec3> Z3DRenderingEngine::boundBoxOfObjs(const std::vector<size_t>& ids) const
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

ZBBox<glm::dvec3> Z3DRenderingEngine::boundBoxOfObjsAfterClipping(const std::vector<size_t>& ids) const
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

void Z3DRenderingEngine::flipView()
{
  camera().flipViewDirection();
}

void Z3DRenderingEngine::setXZView()
{
  resetCamera();
  camera().rotate90X();
  resetCameraClippingRange();
}

void Z3DRenderingEngine::setYZView()
{
  resetCamera();
  camera().rotate90XZ();
  resetCameraClippingRange();
}

// bool Z3DRenderingEngine::takeFixedSizeSeriesScreenShot(const QDir& dir, const QString& namePrefix, const glm::vec3&
// axis,
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
// bool Z3DRenderingEngine::takeSeriesScreenShot(const QDir& dir, const QString& namePrefix, const glm::vec3& axis,
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

void Z3DRenderingEngine::init()
{
  m_canvas->getGLFocus();
  m_globalParas = std::make_unique<Z3DGlobalParameters>(*this);

  // filters
  m_compositor = std::make_unique<Z3DCompositor>(*m_globalParas);

  // build network and connect to canvas
  m_networkEvaluator = std::make_unique<Z3DNetworkEvaluator>(*m_compositor);

  // packages
  for (auto objDoc : m_doc.objDocs()) {
    if (auto imgDoc = qobject_cast<ZImgDoc*>(objDoc)) {
      auto imgView = new Z3DImgView(*imgDoc, *this);
      connect(imgView, &Z3DImgView::objViewReady, this, &Z3DRenderingEngine::objViewReady);
      m_3dObjViews.push_back(imgView);
    } else if (auto punctaDoc = qobject_cast<ZPunctaDoc*>(objDoc)) {
      auto punctaView = new Z3DPunctaView(*punctaDoc, *this);
      connect(punctaView, &Z3DPunctaView::objViewReady, this, &Z3DRenderingEngine::objViewReady);
      m_3dObjViews.push_back(punctaView);
    } else if (auto swcDoc = qobject_cast<ZSwcDoc*>(objDoc)) {
      auto swcView = new Z3DSwcView(*swcDoc, *this);
      connect(swcView, &Z3DSwcView::objViewReady, this, &Z3DRenderingEngine::objViewReady);
      m_3dObjViews.push_back(swcView);
    } else if (auto meshDoc = qobject_cast<ZMeshDoc*>(objDoc)) {
      auto meshView = new Z3DMeshView(*meshDoc, *this);
      connect(meshView, &Z3DMeshView::objViewReady, this, &Z3DRenderingEngine::objViewReady);
      m_3dObjViews.push_back(meshView);
    } else if (auto aniDoc = qobject_cast<Z3DAnimationDoc*>(objDoc)) {
      auto aniView = new Z3DAnimationView(*aniDoc, *this);
      connect(aniView, &Z3DAnimationView::objViewReady, this, &Z3DRenderingEngine::objViewReady);
      m_3dObjViews.push_back(aniView);
    } else if (auto raDoc = qobject_cast<ZRegionAnnotationDoc*>(objDoc)) {
      auto aniView = new Z3DRegionAnnotationView(*raDoc, *this);
      connect(aniView, &Z3DRegionAnnotationView::objViewReady, this, &Z3DRenderingEngine::objViewReady);
      m_3dObjViews.push_back(aniView);
    }
  }

  updateBoundBox();

  // adjust camera
  resetCamera();

  connect(&camera(), &Z3DCameraParameter::valueChanged, this, &Z3DRenderingEngine::resetCameraClippingRange);

  Q_EMIT networkConstructed();
}

void Z3DRenderingEngine::attachToCanvas(Z3DCanvas& canvas)
{
  m_canvas = &canvas;
  m_globalParas->attachToCanvas(canvas);
  m_canvas->addEventListenerToBack(*m_compositor);
}

void Z3DRenderingEngine::setOutputSize(const glm::uvec2& size)
{
  m_compositor->setOutputSize(size);
}

void Z3DRenderingEngine::makeOutputSizeEvenNumbers()
{
  m_compositor->makeOutputSizeEvenNumbers();
}

void Z3DRenderingEngine::createActions()
{
  m_zoomInAction = new QAction(ZTheme::instance().icon(ZTheme::ZoomInIcon), tr("Zoom &In"), this);
  QList<QKeySequence> zoomInKey;
  zoomInKey << QKeySequence::ZoomIn << QKeySequence(Qt::Key_Plus) << QKeySequence(Qt::Key_Equal);
  m_zoomInAction->setShortcuts(zoomInKey);
  m_zoomInAction->setStatusTip(tr("Zoom in"));
  connect(m_zoomInAction, &QAction::triggered, this, &Z3DRenderingEngine::zoomIn);

  m_zoomOutAction = new QAction(ZTheme::instance().icon(ZTheme::ZoomOutIcon), tr("Zoom &Out"), this);
  QList<QKeySequence> zoomOutKey;
  zoomOutKey << QKeySequence::ZoomOut << QKeySequence(Qt::Key_Minus);
  m_zoomOutAction->setShortcuts(zoomOutKey);
  m_zoomOutAction->setStatusTip(tr("Zoom out"));
  connect(m_zoomOutAction, &QAction::triggered, this, &Z3DRenderingEngine::zoomOut);

  m_resetCameraAction = new QAction(tr("&Reset Camera"), this);
  m_resetCameraAction->setStatusTip(tr("Reset camera to show all objects in scene"));
  connect(m_resetCameraAction, &QAction::triggered, this, &Z3DRenderingEngine::resetCamera);
}

ZImg Z3DRenderingEngine::textureToRGBAImg(const Z3DTexture& tex)
{
  GLenum dataFormat = GL_BGRA;
  GLenum dataType = GL_UNSIGNED_INT_8_8_8_8_REV;
  std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 64>> colorBuffer(
    Z3DTexture::bypePerPixel(dataFormat, dataType) * tex.numPixels());
  tex.downloadTextureToBuffer(dataFormat, dataType, colorBuffer.data());
  ZImg bufImg;
  bufImg.wrapData(colorBuffer.data(), ZImgInfo(tex.width(), tex.height(), 1, 4));
  ZImg res(bufImg.info());
  ZImgFormat::CXYZtoXYZC(bufImg, res, true);
  res.infoRef().lastChannelIsAlphaChannel = true;
  res.correctPreMultipliedColor();
  return res;
}

} // namespace nim
