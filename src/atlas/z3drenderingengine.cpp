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
#include <memory>

DEFINE_bool(
  atlas_check_opengl_error_for_all_gl_calls,
  true,
  "Whether to check opengl error after all gl calls, default is true, can set to false for better performance");

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
                                        QEvent::Timer};
}

Z3DRenderingEngine::~Z3DRenderingEngine()
{
  LOG(INFO) << "in engine destructor";
  m_context->makeCurrent();
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
    LOG(ERROR) << "Exception: " << e.what();
    throw;
  }
}

void Z3DRenderingEngine::takeFixedSizeScreenShotWithoutResetCanvasSize(const QString& filename,
                                                                       int width,
                                                                       int height,
                                                                       Z3DScreenShotType sst)
{
  try {
    m_context->makeCurrent();

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
    LOG(ERROR) << "Exception: " << e.what();
    throw;
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
    m_context->makeCurrent();
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
    LOG(ERROR) << "Exception: " << e.what();
    throw;
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
  m_context->makeCurrent();

  m_globalParas = std::make_unique<Z3DGlobalParameters>(*this);

  // filters
  m_compositor = std::make_unique<Z3DCompositor>(*m_globalParas);
  addEventListenerToBack(*m_compositor);

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

  m_inited = true;
  LOG(INFO) << "3D Renderer Inited.";

  Q_EMIT networkConstructed();
}

void Z3DRenderingEngine::attachToCanvas(Z3DCanvas* canvas)
{
  CHECK(canvas);
  m_canvas = canvas;
  m_canvas->setShareContext(m_context->context());
  m_canvas->setEventReceiver(this);

  m_globalParas->setDevicePixelRatio(m_canvas->devicePixelRatio());

  setOutputSize(m_canvas->physicalSize());
  connect(m_canvas, &Z3DCanvas::canvasSizeChanged, this, &Z3DRenderingEngine::onCanvasResized);
  connect(m_canvas, &Z3DCanvas::rotateX, this, &Z3DRenderingEngine::rotateX);
  connect(m_canvas, &Z3DCanvas::rotateY, this, &Z3DRenderingEngine::rotateY);
  connect(m_canvas, &Z3DCanvas::rotateZ, this, &Z3DRenderingEngine::rotateZ);
  connect(m_canvas, &Z3DCanvas::rotateXM, this, &Z3DRenderingEngine::rotateXM);
  connect(m_canvas, &Z3DCanvas::rotateYM, this, &Z3DRenderingEngine::rotateYM);
  connect(m_canvas, &Z3DCanvas::rotateZM, this, &Z3DRenderingEngine::rotateZM);
}

void Z3DRenderingEngine::setOutputSize(const glm::uvec2& size)
{
  m_context->makeCurrent();
  m_compositor->setOutputSize(size);
}

void Z3DRenderingEngine::makeOutputSizeEvenNumbers()
{
  m_context->makeCurrent();
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
  m_context = std::make_unique<Z3DContext>();
  m_context->makeCurrent();

  glbinding::initialize([](const char* name) {
    return Z3DContext().getProcAddress(name);
  });
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
  glbinding::addContextSwitchCallback([](glbinding::ContextHandle handle) {
    LOG(INFO) << "Switching to OpenGL context " << handle;
  });

  if (!Z3DGpuInfo::instance().isSupported()) {
    auto errMsg = Z3DGpuInfo::instance().notSupportedReason();
    LOG(ERROR) << errMsg;
    throw ZGLException(errMsg);
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
  if (m_inited && contains(m_eventTypes, e->type())) {
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

} // namespace nim
