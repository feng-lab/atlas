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
#include <glbinding/glbinding.h>
#include <glbinding-aux/Meta.h>
#include <QOffscreenSurface>
#include <memory>

DEFINE_bool(
  atlas_check_opengl_error_for_all_gl_calls,
  true,
  "Whether to check opengl error after all gl calls, default is true, can set to false for better performance");

DEFINE_bool(atlas_log_glbinding_context_switch,
            false,
            "Whether to log glbinding context switch event, default is false");

namespace nim {

Z3DRenderingEngine::Z3DRenderingEngine(ZDoc& doc, QObject* parent)
  : QObject(parent)
  , m_doc(doc)
  , m_numObjsBefore(m_doc.numObjs())
{
  m_eventTypes = std::set<QEvent::Type>{QEvent::ContextMenu,
                                        QEvent::Enter,
                                        QEvent::Leave,
                                        QEvent::MouseButtonPress,
                                        QEvent::MouseButtonRelease,
                                        QEvent::MouseMove,
                                        QEvent::MouseButtonDblClick,
                                        QEvent::Wheel,
                                        QEvent::KeyPress,
                                        QEvent::KeyRelease,
                                        QEvent::UpdateRequest};

  // need to be created in main gui thread
  // see https://bugreports.qt.io/browse/QTBUG-87115
  m_offscreenSurface = std::make_unique<QOffscreenSurface>();
  m_offscreenSurface->setFormat(QSurfaceFormat::defaultFormat());
  m_offscreenSurface->create();
  if (!m_offscreenSurface->isValid()) {
    LOG(ERROR) << "Can not create OpenGL Offscreen surface";
  }
}

Z3DRenderingEngine::~Z3DRenderingEngine()
{
  LOG(INFO) << "in engine destructor";
  detachCanvas();
  getGLFocus();
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
    return m_globalParas->widgetsGroup(false, *this);
  } else {
    for (auto& objView : m_3dObjViews) {
      std::shared_ptr<ZWidgetsGroup> wg = objView->viewSettingWidgetsGroupOf(id);
      if (wg) {
        return wg;
      }
    }
  }
  return {};
}

QWidget* Z3DRenderingEngine::globalParasWidget()
{
  return m_globalParas->widgetsGroup(true, *this)->createWidget(false);
}

QWidget* Z3DRenderingEngine::backgroundWidget()
{
  return m_compositor->backgroundWidgetsGroup()->createWidget(false);
}

QWidget* Z3DRenderingEngine::axisWidget()
{
  return m_compositor->axisWidgetsGroup()->createWidget(false);
}

void Z3DRenderingEngine::updateBoundBox()
{
  m_boundBox.reset();
  for (auto& objView : m_3dObjViews) {
    m_boundBox.expand(objView->boundBox());
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
  for (auto& objView : m_3dObjViews) {
    if (objView->hasObj(id)) {
      if (asQString(json.at("ViewObjType")) == objView->doc().typeName()) {
        objView->read(id, json);
      } else {
        LOG(WARNING) << "view object type " << asQString(json.at("ViewObjType")) << " dones't match object type "
                     << objView->doc().typeName() << ". abort.";
      }
      return;
    }
  }
}

void Z3DRenderingEngine::write(size_t id, json::object& json) const
{
  for (auto objView : m_3dObjViews) {
    if (objView->hasObj(id)) {
      json["ViewObjType"] = json::value_from(objView->doc().typeName());
      json["ViewVersion"] = 1.0;
      objView->write(id, json);
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
  try {
    takeFixedSizeScreenShotWithoutResetCanvasSize(filename, width, height, sst);
    resetCanvasSize();
  }
  catch (ZException const& e) {
    auto errorMsg = fmt::format("takeFixedSizeScreenShot error: {}", e.what());
    LOG(ERROR) << errorMsg;
    reportRenderingError(errorMsg);
  }
}

void Z3DRenderingEngine::takeFixedSizeScreenShotWithoutResetCanvasSize(const QString& filename,
                                                                       int width,
                                                                       int height,
                                                                       Z3DScreenShotType sst)
{
  try {
    getGLFocus();

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
          LOG(INFO) << "Saved stereo rendering (" << img.width() << " x 2, " << img.height()
                    << ") to file: " << filename;
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
  catch (ZException const& e) {
    auto errorMsg = fmt::format("takeFixedSizeScreenShotWithoutResetCanvasSize error: {}", e.what());
    LOG(ERROR) << errorMsg;
    reportRenderingError(errorMsg);
  }
}

void Z3DRenderingEngine::resetCanvasSize()
{
  if (m_canvas) {
    setOutputSize(m_canvas->physicalSize());
  }
}

void Z3DRenderingEngine::takeScreenShot(const QString& filename, Z3DScreenShotType sst)
{
  try {
    getGLFocus();
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
  catch (ZException const& e) {
    auto errorMsg = fmt::format("takeScreenShot error: {}", e.what());
    LOG(ERROR) << errorMsg;
    reportRenderingError(errorMsg);
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
  initGL();
  getGLFocus();

  m_globalParas = std::make_unique<Z3DGlobalParameters>();

  // filters
  m_compositor = std::make_unique<Z3DCompositor>(*m_globalParas);
  addEventListenerToBack(*m_compositor);
  connect(m_compositor.get(), &Z3DCompositor::sceneParaUpdated, this, &Z3DRenderingEngine::sceneParaUpdated);
  connect(m_compositor.get(), &Z3DCompositor::renderingFinished, this, &Z3DRenderingEngine::renderingFinished);

  // build network and connect to canvas
  m_networkEvaluator = std::make_unique<Z3DNetworkEvaluator>(*m_compositor);

  // packages
  connect(&m_doc,
          &ZDoc::requestToAdjustViewToPosition,
          this,
          qOverload<double, double, double, double>(&Z3DRenderingEngine::cameraFocusesOn));
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

  LOG(INFO) << "3D Renderer Inited.";

  Q_EMIT networkConstructed();
}

void Z3DRenderingEngine::initAndAttachToCanvas(Z3DCanvas* canvas)
{
  CHECK(canvas);
  m_canvas = canvas;
  init();

  m_globalParas->setDevicePixelRatio(m_canvas->devicePixelRatio());
  setOutputSize(m_canvas->physicalSize());

  m_canvas->setRenderingEngine(this);
  connect(m_canvas, &Z3DCanvas::canvasSizeChanged, this, &Z3DRenderingEngine::onCanvasResized);
  connect(m_canvas, &Z3DCanvas::rotateX, this, &Z3DRenderingEngine::rotateX);
  connect(m_canvas, &Z3DCanvas::rotateY, this, &Z3DRenderingEngine::rotateY);
  connect(m_canvas, &Z3DCanvas::rotateZ, this, &Z3DRenderingEngine::rotateZ);
  connect(m_canvas, &Z3DCanvas::rotateXM, this, &Z3DRenderingEngine::rotateXM);
  connect(m_canvas, &Z3DCanvas::rotateYM, this, &Z3DRenderingEngine::rotateYM);
  connect(m_canvas, &Z3DCanvas::rotateZM, this, &Z3DRenderingEngine::rotateZM);
  connect(this, &Z3DRenderingEngine::sceneParaUpdated, m_canvas, &Z3DCanvas::sceneParaUpdated);
  connect(this, &Z3DRenderingEngine::renderingFinished, m_canvas, &Z3DCanvas::renderingFinished);
}

void Z3DRenderingEngine::detachCanvas()
{
  m_globalParas->setDevicePixelRatio(1);

  if (!m_canvas) {
    return;
  }

  m_canvas->disconnect(this);
  disconnect(m_canvas);
  m_canvas->setRenderingEngine(nullptr);

  m_canvas.clear();
}

void Z3DRenderingEngine::setOutputSize(const glm::uvec2& size)
{
  getGLFocus();
  m_compositor->setOutputSize(size);
}

void Z3DRenderingEngine::makeOutputSizeEvenNumbers()
{
  getGLFocus();
  m_compositor->makeOutputSizeEvenNumbers();
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

void Z3DRenderingEngine::onCanvasResized(size_t w, size_t h)
{
  setOutputSize(glm::uvec2(w, h));
}

void Z3DRenderingEngine::initGL()
{
  if (m_canvas) {
    m_context = std::make_unique<Z3DContext>(*m_offscreenSurface, m_canvas->context());
  } else {
    m_context = std::make_unique<Z3DContext>(*m_offscreenSurface);
  }
  m_context->makeCurrent();

  glbinding::initialize(0, [this](const char* name) {
    return m_context->getProcAddress(name);
  });
  glbinding::useContext(0);
  Z3DGpuInfo::instance().logGpuInfo();
  if (FLAGS_atlas_check_opengl_error_for_all_gl_calls) {
    glbinding::setCallbackMaskExcept(glbinding::CallbackMask::After |
                                       glbinding::CallbackMask::ParametersAndReturnValue |
                                       glbinding::CallbackMask::Unresolved,
                                     {"glGetError"});
    glbinding::setAfterCallback([](const glbinding::FunctionCall& call) {
      GLenum error = glGetError();
      if (error != GL_NO_ERROR) {
        std::ostringstream os;

        os << call.function->name() << "(";
        for (size_t i = 0; i < call.parameters.size(); ++i) {
          os << call.parameters[i].get();
          if (i + 1 < call.parameters.size()) {
            os << ", ";
          }
        }
        os << ")";

        if (call.returnValue) {
          os << " -> " << call.returnValue.get();
        }

        LOG(ERROR) << "OpenGL error: " << glbinding::aux::Meta::getString(error) << " with " << os.str();
      }
    });
  } else {
    glbinding::setCallbackMask(glbinding::CallbackMask::Unresolved);
  }
  glbinding::setUnresolvedCallback([](const glbinding::AbstractFunction& call) {
    LOG(ERROR) << "OpenGL function " << call.name() << " can not be resolved.";
  });
  if (FLAGS_atlas_log_glbinding_context_switch) {
    glbinding::addContextSwitchCallback([](glbinding::ContextHandle handle) {
      LOG(INFO) << "Switching to OpenGL context " << handle;
    });
  }

  if (!Z3DGpuInfo::instance().isSupported()) {
    auto errMsg = QString("3D Rendering not supported: ") + Z3DGpuInfo::instance().notSupportedReason();
    LOG(ERROR) << errMsg;
    reportRenderingError(errMsg);
  }
}

void Z3DRenderingEngine::rotateX()
{
  for (auto listener : m_listeners) {
    listener->rotateX();
  }
}

void Z3DRenderingEngine::rotateY()
{
  for (auto listener : m_listeners) {
    listener->rotateY();
  }
}

void Z3DRenderingEngine::rotateZ()
{
  for (auto listener : m_listeners) {
    listener->rotateZ();
  }
}

void Z3DRenderingEngine::rotateXM()
{
  for (auto listener : m_listeners) {
    listener->rotateXM();
  }
}

void Z3DRenderingEngine::rotateYM()
{
  for (auto listener : m_listeners) {
    listener->rotateYM();
  }
}

void Z3DRenderingEngine::rotateZM()
{
  for (auto listener : m_listeners) {
    listener->rotateZM();
  }
}

bool Z3DRenderingEngine::event(QEvent* e)
{
  LOG(INFO) << e->type();
  if (contains(m_eventTypes, e->type())) {
    if (e->type() == QEvent::UpdateRequest) {
      render();
      e->accept();
      return true;
    }
    auto outputSize = m_compositor->outputSize();
    int w = outputSize.x;
    int h = outputSize.y;
    if (m_canvas) {
      w = m_canvas->width();
      h = m_canvas->height();
    }
    for (auto listener : m_listeners) {
      listener->onEvent(e, w, h);
      if (e->isAccepted()) {
        return true;
      }
    }
  }
  return QObject::event(e);
}

void Z3DRenderingEngine::getGLFocus()
{
  m_context->makeCurrent();
  // glbinding::useContext(0);
}

void Z3DRenderingEngine::render(bool stereo)
{
  LOG(INFO) << "render";
  getGLFocus();
  m_networkEvaluator->setFastRenderingMode(true, stereo);
  m_networkEvaluator->process(stereo);
  if (!m_globalParas->cancelLongRendering.load()) {
    try {
      m_networkEvaluator->setFastRenderingMode(false, stereo);
      double progress = 0.1;
      Q_EMIT progressChanged(std::clamp<int>(progress * 100., 0, 100));
      while (progress < 1.0) {
        progress = m_networkEvaluator->process(stereo);
        Q_EMIT progressChanged(std::clamp<int>(progress * 100., 0, 100));
      }
    }
    catch (ZException& e) {
      LOG(INFO) << e.what();
    }
  }
  m_globalParas->cancelLongRendering = false;
}

Z3DRenderTarget* Z3DRenderingEngine::monoReadyTarget() const
{
  return m_compositor->monoReadyTarget();
}

Z3DRenderTarget* Z3DRenderingEngine::leftReadyTarget() const
{
  return m_compositor->leftReadyTarget();
}

Z3DRenderTarget* Z3DRenderingEngine::rightReadyTarget() const
{
  return m_compositor->rightReadyTarget();
}

void Z3DRenderingEngine::reportRenderingError(const QString& error) const
{
  Q_EMIT renderingError(error);
}

void Z3DRenderingEngine::reportRenderingError(const std::string& error) const
{
  Q_EMIT renderingError(QString::fromStdString(error));
}

} // namespace nim
