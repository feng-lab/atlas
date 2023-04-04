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
#include "zvideoencoder.h"
#include <glbinding/glbinding.h>
#include <glbinding-aux/Meta.h>
#include <QOffscreenSurface>
#include <QCoreApplication>
#include <memory>

DEFINE_bool(
  atlas_check_opengl_error_for_all_gl_calls,
  true,
  "Whether to check opengl error after all gl calls, default is true, can set to false for better performance");

DEFINE_bool(atlas_log_glbinding_context_switch,
            false,
            "Whether to log glbinding context switch event, default is false");

DEFINE_string(output_image_name_prefix, "video", "name prefix of the output images, default is video");

DEFINE_int32(output_image_name_field_width,
             8,
             "number of decimals used for the name of output images after name prefix, default is 8");

#if defined(__linux__)
DECLARE_bool(__use_EGL);
#endif

namespace {

// generic solution
template<class T>
int numDigits(T number)
{
  int digits = 0;
  if (number < 0) {
    digits = 1; // remove this line if '-' counts as a digit
  }
  while (number) {
    number /= 10;
    digits++;
  }
  return digits;
}

//// partial specialization optimization for 64-bit numbers
// template<>
// int numDigits(int64_t x)
//{
//   if (x == INT64_MIN) {
//     return 19 + 1;
//   }
//   if (x < 0) {
//     return numDigits(-x) + 1;
//   }
//
//   if (x >= 10000000000) {
//     if (x >= 100000000000000) {
//       if (x >= 10000000000000000) {
//         if (x >= 100000000000000000) {
//           if (x >= 1000000000000000000) {
//             return 19;
//           }
//           return 18;
//         }
//         return 17;
//       }
//       if (x >= 1000000000000000) {
//         return 16;
//       }
//       return 15;
//     }
//     if (x >= 1000000000000) {
//       if (x >= 10000000000000) {
//         return 14;
//       }
//       return 13;
//     }
//     if (x >= 100000000000) {
//       return 12;
//     }
//     return 11;
//   }
//   if (x >= 100000) {
//     if (x >= 10000000) {
//       if (x >= 100000000) {
//         if (x >= 1000000000) {
//           return 10;
//         }
//         return 9;
//       }
//       return 8;
//     }
//     if (x >= 1000000) {
//       return 7;
//     }
//     return 6;
//   }
//   if (x >= 100) {
//     if (x >= 1000) {
//       if (x >= 10000) {
//         return 5;
//       }
//       return 4;
//     }
//     return 3;
//   }
//   if (x >= 10) {
//     return 2;
//   }
//   return 1;
// }

// partial specialization optimization for 32-bit numbers
template<>
int numDigits(int32_t x)
{
  if (x == INT32_MIN) {
    return 10 + 1;
  }
  if (x < 0) {
    return numDigits(-x) + 1;
  }

  if (x >= 10000) {
    if (x >= 10000000) {
      if (x >= 100000000) {
        if (x >= 1000000000) {
          return 10;
        }
        return 9;
      }
      return 8;
    }
    if (x >= 100000) {
      if (x >= 1000000) {
        return 7;
      }
      return 6;
    }
    return 5;
  }
  if (x >= 100) {
    if (x >= 1000) {
      return 4;
    }
    return 3;
  }
  if (x >= 10) {
    return 2;
  }
  return 1;
}

} // namespace

namespace nim {

Z3DRenderingEngine::Z3DRenderingEngine(ZDoc& doc, QObject* parent)
  : QObject(parent)
  , m_doc(doc)
  , m_numObjsBefore(m_doc.numObjs())
{
  m_eventTypes = std::set<QEvent::Type>{QEvent::ContextMenu,
                                        QEvent::MouseButtonPress,
                                        QEvent::MouseButtonRelease,
                                        QEvent::MouseMove,
                                        QEvent::MouseButtonDblClick,
                                        QEvent::Wheel,
                                        QEvent::KeyPress,
                                        QEvent::KeyRelease,
                                        QEvent::UpdateRequest,
                                        QEvent::LayoutRequest};

#if defined(__linux__)
  if (FLAGS___use_EGL) {
    return;
  }
#endif
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
  // LOG(INFO) << "resetCameraClippingRange";
  if (!m_mutex.try_lock()) {
    return;
  }
  camera().resetCameraNearFarPlane(m_boundBox);
  m_mutex.unlock();
}

void Z3DRenderingEngine::takeFixedSizeScreenShot(const QString& filename, int width, int height, Z3DScreenShotType sst)
{
  try {
    takeFixedSizeScreenShotWithoutResetCanvasSizePrivate(filename, width, height, sst);
    resetOutputSizeToMatchCanvasSize();
  }
  catch (ZException const& e) {
    auto errorMsg = fmt::format("takeFixedSizeScreenShot error: {}", e.what());
    LOG(ERROR) << errorMsg;
    reportRenderingError(errorMsg);
  }
}

void Z3DRenderingEngine::takeScreenShot(const QString& filename, Z3DScreenShotType sst)
{
  try {
    takeScreenShotPrivate(filename, sst);
  }
  catch (ZException const& e) {
    auto errorMsg = fmt::format("takeScreenShot error: {}", e.what());
    LOG(ERROR) << errorMsg;
    reportRenderingError(errorMsg);
  }
}

void Z3DRenderingEngine::exportFixedSize3DAnimation(const ZAnimation* animation,
                                                    const QString& fn,
                                                    double framePerSecond,
                                                    double startTime,
                                                    double endTime,
                                                    int width,
                                                    int height,
                                                    bool overwriteFileIfExist,
                                                    Z3DScreenShotType sst,
                                                    std::atomic_bool* cancelFlag,
                                                    const QString* imageOuputFolder,
                                                    bool skipVideoCompression)
{
  LOG(INFO) << "start exporting video";
  auto logGuard = folly::makeGuard([]() {
    LOG(INFO) << "end exporting video";
  });

  CHECK(animation);
  if (startTime < 0 || startTime >= animation->duration()) {
    Q_EMIT renderingError(QString("Video start time %1 is not correct").arg(startTime));
    return;
  }
  if (endTime >= 0 && endTime <= startTime) {
    Q_EMIT renderingError(QString("Video end time %1 is not correct").arg(endTime));
    return;
  }
  if (endTime < 0 || endTime > animation->duration()) {
    endTime = animation->duration();
  }
  if (width > 7680 || height > 4320) {
    Q_EMIT renderingError("does not support output size larger than 7680x4320");
    return;
  }

  QDir dir(QFileInfo(fn).absolutePath());
  if (!dir.exists()) {
    if (!dir.mkpath(".")) {
      Q_EMIT renderingError(QString("Can not create folder %1").arg(dir.path()));
      return;
    }
  }
  if (dir.exists(fn)) {
    if (!overwriteFileIfExist) {
      Q_EMIT renderingError(QString("File %1 already exists").arg(dir.filePath(fn)));
      return;
    } else if (!QFile::remove(dir.filePath(fn))) {
      Q_EMIT renderingError(QString("Can not replace existed file %1").arg(dir.filePath(fn)));
      return;
    }
  }
  if (imageOuputFolder) {
    if (imageOuputFolder->trimmed().isEmpty()) {
      Q_EMIT renderingError(QString("Image output folder name %1 is empty, can not be used").arg(*imageOuputFolder));
      return;
    }
    QDir iofDir(*imageOuputFolder);
    if (!iofDir.exists()) {
      if (!iofDir.mkpath(".")) {
        Q_EMIT renderingError(QString("Can not create image output folder %1").arg(*imageOuputFolder));
        return;
      }
    }
    auto iof = QFileInfo(iofDir.absolutePath());
    if (!iof.isDir() || !iof.isWritable()) {
      Q_EMIT renderingError(QString("Image output folder %1 can not be used, not writable").arg(*imageOuputFolder));
      return;
    }
  }
  if (skipVideoCompression) {
    if (!imageOuputFolder) {
      Q_EMIT renderingError(QString("Image output folder must be specified if we are going to skip video compression"));
      return;
    }
  }

  try {
    if (width % 2 == 1) {
      ++width;
    }
    if (height % 2 == 1) {
      ++height;
    }
    m_doc.hideAnimation3DView();
    m_doc.deselectAllObjs();

    auto duration = endTime - startTime;
    int numFrame = std::ceil(duration * framePerSecond);
    int fieldWidth = std::max(FLAGS_output_image_name_field_width,
                              numDigits(static_cast<int>(std::ceil(animation->duration() * framePerSecond))));
    double time = startTime;
    int startFrame = static_cast<int>(std::round(startTime * framePerSecond));
    double timeIncrement = duration / numFrame;
    QString namePrefix = QString::fromStdString(FLAGS_output_image_name_prefix);
    auto tempdir = std::make_shared<QTemporaryDir>();
    QDir tmpdir(imageOuputFolder ? *imageOuputFolder : tempdir->path());
    for (int i = 0; i < numFrame; ++i) {
      Q_EMIT progressChanged(std::clamp<int>(std::floor(i * 1. / numFrame * 100.), 0, 100));
      if (cancelFlag && cancelFlag->load()) {
        reportCancelError();
        return;
      }

      animation->setCurrentTime(time);
      time += timeIncrement;
      QString filename = QString("%1%2.png").arg(namePrefix).arg(i + startFrame, fieldWidth, 10, QChar('0'));
      QString filepath = tmpdir.filePath(filename);

      takeFixedSizeScreenShotWithoutResetCanvasSizePrivate(filepath, width, height, sst);
    }
    resetOutputSizeToMatchCanvasSize();

    if (cancelFlag && cancelFlag->load()) {
      reportCancelError();
      return;
    }

    ZVideoEncoder videoEncoder;
    if (skipVideoCompression) {
      LOG(INFO) << "video compression skipped, you can run the following command to get video:";
      ZVideoEncoder::encodeDryRun(tmpdir, namePrefix, fieldWidth, framePerSecond, dir.filePath(fn));
    } else {
      connect(&videoEncoder, &ZVideoEncoder::error, this, &Z3DRenderingEngine::renderingError);
      connect(&videoEncoder, &ZVideoEncoder::finished, this, &Z3DRenderingEngine::videoEncoderFinished);
      connect(&videoEncoder, &ZVideoEncoder::canceled, this, &Z3DRenderingEngine::reportCancelError);
      videoEncoder.encode(tmpdir, namePrefix, fieldWidth, framePerSecond, dir.filePath(fn));
      while (!videoEncoder.waitForFinished(3000)) {
        if (cancelFlag && cancelFlag->load()) {
          videoEncoder.cancel();
          return;
        }
      }
      LOG(INFO) << dir.filePath(fn) << " saved";
    }
  }
  catch (ZException const& e) {
    LOG(ERROR) << e.what();
    reportRenderingError(e.what());
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

void Z3DRenderingEngine::init()
{
  Q_EMIT progressChanged(10);
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

  Q_EMIT progressChanged(20);

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

  Q_EMIT progressChanged(60);

  // adjust camera
  resetCamera();

  connect(&camera(), &Z3DCameraParameter::valueChanged, this, &Z3DRenderingEngine::resetCameraClippingRange);

  LOG(INFO) << "3D Renderer Inited.";

  Q_EMIT progressChanged(100);

  Q_EMIT initialized();
}

void Z3DRenderingEngine::initAndAttachToCanvas(Z3DCanvas* canvas)
{
  CHECK(canvas);
  m_canvas = canvas;
  init();

  m_globalParas->setDevicePixelRatio(m_canvas->devicePixelRatio());
  setOutputSize(m_canvas->physicalSize());

  connect(m_canvas, &Z3DCanvas::canvasSizeChanged, this, &Z3DRenderingEngine::onCanvasResized);
  connect(m_canvas, &Z3DCanvas::rotateX, this, &Z3DRenderingEngine::rotateX);
  connect(m_canvas, &Z3DCanvas::rotateY, this, &Z3DRenderingEngine::rotateY);
  connect(m_canvas, &Z3DCanvas::rotateZ, this, &Z3DRenderingEngine::rotateZ);
  connect(m_canvas, &Z3DCanvas::rotateXM, this, &Z3DRenderingEngine::rotateXM);
  connect(m_canvas, &Z3DCanvas::rotateYM, this, &Z3DRenderingEngine::rotateYM);
  connect(m_canvas, &Z3DCanvas::rotateZM, this, &Z3DRenderingEngine::rotateZM);
  connect(this, &Z3DRenderingEngine::sceneParaUpdated, m_canvas, &Z3DCanvas::sceneParaUpdated);
  connect(this, &Z3DRenderingEngine::renderingFinished, m_canvas, &Z3DCanvas::renderingFinished);
  m_canvas->setRenderingEngine(this);
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

glm::uvec2 Z3DRenderingEngine::outputSize() const
{
  return m_compositor->outputSize();
}

void Z3DRenderingEngine::setOutputSize(const glm::uvec2& size)
{
  getGLFocus();
  m_compositor->setOutputSize(size);
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
#if defined(__linux__)
    if (FLAGS___use_EGL) {
      try {
        m_context = std::make_unique<Z3DContext>();
      }
      catch (const ZException& e) {
        auto errMsg = fmt::format("Can not initialize 3d context: {}", e.what());
        LOG(ERROR) << errMsg;
        reportRenderingError(errMsg);
      }
    } else {
      m_context = std::make_unique<Z3DContext>(*m_offscreenSurface);
    }
#else
    m_context = std::make_unique<Z3DContext>(*m_offscreenSurface);
#endif
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
  // LOG(INFO) << e->type();
  if (contains(m_eventTypes, e->type())) {
    if (e->type() == QEvent::UpdateRequest) {
      renderFast();
      e->accept();
      return true;
    } else if (e->type() == QEvent::LayoutRequest) {
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

void Z3DRenderingEngine::renderFast(bool stereo)
{
  if (m_isRendering) {
    LOG(INFO) << "in fast rendering, schedule a update later";
    QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
    return;
  }

  LOG(INFO) << "renderFast";
  m_isRendering = true;
  getGLFocus();
  Q_EMIT progressChanged(0);
  m_networkEvaluator->process(stereo, true);
  Q_EMIT progressChanged(100);
  QCoreApplication::postEvent(this, new QEvent(QEvent::LayoutRequest), Qt::LowEventPriority - 1);

  m_isRendering = false;
}

void Z3DRenderingEngine::render(bool stereo)
{
  if (m_isRendering) {
    LOG(INFO) << "in rendering, schedule a update later";
    QCoreApplication::postEvent(this, new QEvent(QEvent::LayoutRequest), Qt::LowEventPriority - 1);
    return;
  }

  LOG(INFO) << "render";
  m_isRendering = true;
  getGLFocus();
    try {
      m_globalParas->cancellationSource = std::make_unique<folly::CancellationSource>();
      double progress = 0.1;
      Q_EMIT progressChanged(std::clamp<int>(progress * 100., 0, 100));
      while (progress < 1.0) {
        progress = m_networkEvaluator->process(stereo, false, m_globalParas->cancellationSource->getToken());
        Q_EMIT progressChanged(std::clamp<int>(progress * 100., 0, 100));
      }
    }
    catch (ZException& e) {
      LOG(INFO) << e.what();
    }
  m_globalParas->cancellationSource.reset();
  m_isRendering = false;
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

void Z3DRenderingEngine::takeFixedSizeScreenShotWithoutResetCanvasSizePrivate(const QString& filename,
                                                                              int width,
                                                                              int height,
                                                                              Z3DScreenShotType sst)
{
  getGLFocus();

  const int tileSize = 7680; // 2048;
  const int tileBorder = 128;
  const auto tileInnerSize = tileSize - 2 * tileBorder;

  if (width <= tileSize && height <= tileSize) {
    // resize texture container to desired image dimensions and propagate change
    setOutputSize(glm::uvec2(width, height));

    takeScreenShotPrivate(filename, sst);
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

void Z3DRenderingEngine::takeScreenShotPrivate(const QString& filename, Z3DScreenShotType sst)
{
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

void Z3DRenderingEngine::resetOutputSizeToMatchCanvasSize()
{
  if (m_canvas) {
    setOutputSize(m_canvas->physicalSize());
  }
}

} // namespace nim
