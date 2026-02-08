#include "z3drenderingengine.h"

#include "z3dcanvas.h"
#include "z3dcompositor.h"
#include "z3dcameraparameter.h"
#include "zwidgetsgroup.h"
#include "zimgdoc.h"
#include "z3dimgview.h"
#include "zpunctadoc.h"
#include "z3dpunctaview.h"
#include "zswcdoc.h"
#include "z3dswcview.h"
#include "zmeshdoc.h"
#include "z3dmeshview.h"
#include "zskeletondoc.h"
#include "z3dskeletonview.h"
#include "z3danimationdoc.h"
#include "z3danimationview.h"
#include "z3dregionannotationview.h"
#include "zimgformat.h"
#include "zvideoencoder.h"
#include "z3drenderglobalstate.h"
#include "z3dscratchresourcepool.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkan.h"
#include "z3dshadermanager.h"
#include "z3dgpuinfo.h"
#include "z3dgl.h"
#include "z3dfilter.h"
#include "zcancellation.h"
#include "z3dperfcollector.h"
#include "zqtexecutor.h"
#include "zrenderthreadexecutor_tls.h"
#include <folly/OperationCancelled.h>
#include <glbinding/glbinding.h>
#include <glbinding-aux/Meta.h>
#include <QOffscreenSurface>
#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>
#include <QThread>
#include <memory>

DEFINE_bool(atlas_debug_opengl,
            false,
            "Whether to check openGL error aggressively, default is false, can set to true for debugging");

DEFINE_bool(atlas_log_glbinding_context_switch, false, "Whether to log openGL context switch event, default is false");

DECLARE_string(output_image_name_prefix);
DECLARE_int32(output_image_name_field_width);
DECLARE_int32(maximum_output_width);
DECLARE_int32(maximum_output_height);

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

void Z3DRenderingEngine::CheckOpenGLStateFilterWrapper::afterFilterProcess(const Z3DFilter* p)
{
  checkState(p);
}

void Z3DRenderingEngine::CheckOpenGLStateFilterWrapper::beforeNetworkProcess()
{
  checkState(nullptr);
}

void Z3DRenderingEngine::CheckOpenGLStateFilterWrapper::checkState(const Z3DFilter* p)
{
  if (!checkGLState(GL_BLEND, false)) {
    glDisable(GL_BLEND);
    warn(p, "GL_BLEND was enabled");
  }

  if (!checkGLState(GL_BLEND_SRC, GL_ONE) || !checkGLState(GL_BLEND_DST, GL_ZERO)) {
    glBlendFunc(GL_ONE, GL_ZERO);
    warn(p, "Modified BlendFunc");
  }

  if (!checkGLState(GL_DEPTH_TEST, false)) {
    glDisable(GL_DEPTH_TEST);
    warn(p, "GL_DEPTH_TEST was enabled");
  }

  if (!checkGLState(GL_CULL_FACE, false)) {
    glDisable(GL_CULL_FACE);
    warn(p, "GL_CULL_FACE was enabled");
  }

  if (!checkGLState(GL_COLOR_CLEAR_VALUE, glm::vec4(0.f))) {
    glClearColor(0.f, 0.f, 0.f, 0.f);
    warn(p, "glClearColor() was not set to all zeroes");
  }

  if (!checkGLState(GL_DEPTH_CLEAR_VALUE, 1.f)) {
    glClearDepth(1.0);
    warn(p, "glClearDepth() was not set to 1.0");
  }

  if (!checkGLState(GL_LINE_WIDTH, 1.f)) {
    glLineWidth(1.f);
    warn(p, "glLineWidth() was not set to 1.0");
  }

  if (!checkGLState(GL_ACTIVE_TEXTURE, GL_TEXTURE0)) {
    glActiveTexture(GL_TEXTURE0);
    warn(p, "glActiveTexture was not set to GL_TEXTURE0");
  }

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  if (!checkGLState(GL_MATRIX_MODE, GL_MODELVIEW)) {
    glMatrixMode(GL_MODELVIEW);
    warn(p, "glMatrixMode was not set to GL_MODELVIEW");
  }

  if (!checkGLState(GL_TEXTURE_1D, false)) {
    glDisable(GL_TEXTURE_1D);
    warn(p, "GL_TEXTURE_1D was enabled");
  }

  if (!checkGLState(GL_TEXTURE_2D, false)) {
    glDisable(GL_TEXTURE_2D);
    warn(p, "GL_TEXTURE_2D was enabled");
  }

  if (!checkGLState(GL_TEXTURE_3D, false)) {
    glDisable(GL_TEXTURE_3D);
    warn(p, "GL_TEXTURE_3D was enabled");
  }
#endif

  GLint id;
  glGetIntegerv(GL_CURRENT_PROGRAM, &id);
  if (id != 0) {
    glUseProgram(0);
    warn(p, "A shader was active");
  }

  // can not check this as we are drawing to QOpenglWidget's (Qt5) fbo which is not 0
#if 0
  if (Z3DRenderTarget::currentBoundDrawFBO() != 0) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    warn(p, "A render target was bound (releaseTarget() missing?)");
  }
#endif

  if (!checkGLState(GL_DEPTH_FUNC, GL_LESS)) {
    glDepthFunc(GL_LESS);
    warn(p, "glDepthFunc was not set to GL_LESS");
  }

  if (!checkGLState(GL_CULL_FACE_MODE, GL_BACK)) {
    glCullFace(GL_BACK);
    warn(p, "glCullFace was not set to GL_BACK");
  }
}

void Z3DRenderingEngine::CheckOpenGLStateFilterWrapper::warn(const Z3DFilter* p, const char* message)
{
  if (p) {
    LOG(WARNING) << "Invalid OpenGL state after processing " << p->className() << " : " << message;
  } else {
    LOG(WARNING) << "Invalid OpenGL state before network processing: " << message;
  }
}

void Z3DRenderingEngine::ProfileFilterWrapper::afterFilterProcess(const Z3DFilter* p)
{
  m_benchTimer.recordEvent(p->className().toStdString());
}

void Z3DRenderingEngine::ProfileFilterWrapper::beforeNetworkProcess()
{
  const uint64_t token = Z3DRenderGlobalState::instance().currentPerfFrameToken();
  m_benchTimer.resetAndStart(fmt::format("Network [frame#{}]", token));
}

void Z3DRenderingEngine::ProfileFilterWrapper::afterNetworkProcess()
{
  STOP_AND_LOG(m_benchTimer)
}

void Z3DRenderingEngine::ScratchPoolDeleter::operator()(Z3DScratchResourcePool* pool) const
{
  Z3DRenderGlobalState::instance().resetScratchPool();
  delete pool;
}

Z3DRenderingEngine::Z3DRenderingEngine(ZDoc& doc, QObject* parent)
  : QObject(parent)
  , m_doc(doc)
  , m_numObjsBefore(m_doc.numObjs())
{
  m_eventTypes = boost::unordered_flat_set<QEvent::Type>{QEvent::ContextMenu,
                                                         QEvent::MouseButtonPress,
                                                         QEvent::MouseButtonRelease,
                                                         QEvent::MouseMove,
                                                         QEvent::MouseButtonDblClick,
                                                         QEvent::Wheel,
                                                         QEvent::KeyPress,
                                                         QEvent::KeyRelease,
                                                         QEvent::UpdateRequest,
                                                         QEvent::LayoutRequest};

  // Render-thread executor: provides a dedicated folly executor bound to this
  // engine's thread for linear async flows (instead of ad-hoc callback chains).
  m_renderThreadExecutor = std::make_unique<ZQtExecutor>(this, "Z3DRenderingEngine");

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
  VLOG(1) << "in engine destructor";
  m_shuttingDown = true;
  stopVulkanCompletionPolling();
  setCurrentRenderThreadExecutor(nullptr);
  detachCanvas();
  VLOG(1) << "canvas detached";
  getGLFocus();

  // Proactively release any cached GL shaders while GL context is current,
  // to avoid deleting them later when no GL context is available.
  Z3DShaderManager::instance().clear();

  // Safe cleanup for Vulkan: ensure GPU idle and release scratch resources
  try {
    if (m_vkDevice) {
      m_vkDevice->context().device().waitIdle();
    }
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Vulkan waitIdle during engine shutdown failed: " << e.what();
  }
}

const ZDoc& Z3DRenderingEngine::doc() const
{
  return m_doc;
}

ZQtExecutor& Z3DRenderingEngine::renderThreadExecutor()
{
  CHECK(m_renderThreadExecutor != nullptr);
  return *m_renderThreadExecutor;
}

const ZQtExecutor& Z3DRenderingEngine::renderThreadExecutor() const
{
  CHECK(m_renderThreadExecutor != nullptr);
  return *m_renderThreadExecutor;
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

  VLOG(2) << json::value_from(m_boundBox);
  // Update global cuts using explicit binding modes
  m_globalParas->applyBoundsForCuts(m_boundBox);
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
  for (auto& objView : m_3dObjViews) {
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
  // VLOG(1) << "resetCameraClippingRange";
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
  catch (const ZException& e) {
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
  catch (const ZException& e) {
    auto errorMsg = fmt::format("takeScreenShot error: {}", e.what());
    LOG(ERROR) << errorMsg;
    reportRenderingError(errorMsg);
  }
}

void Z3DRenderingEngine::exportFixedSize3DAnimation(const ZAnimation* animation,
                                                    const QString& fn,
                                                    int framePerSecond,
                                                    int startFrame,
                                                    int endFrame,
                                                    int width,
                                                    int height,
                                                    bool overwriteFileIfExist,
                                                    Z3DScreenShotType sst,
                                                    std::atomic_bool* cancelFlag,
                                                    const QString* imageOuputFolder,
                                                    bool skipVideoCompression,
                                                    int tileSize,
                                                    int tileBorder)
{
  LOG(INFO) << "start exporting video";
  auto logGuard = folly::makeGuard([]() {
    LOG(INFO) << "end exporting video";
  });

  CHECK(animation);
  int totalNumFrames = std::max(1, static_cast<int>(std::ceil(animation->duration() * framePerSecond)));
  if (startFrame < 0 || startFrame >= totalNumFrames) {
    Q_EMIT renderingError(QString("Video start frame %1 is not correct").arg(startFrame));
    return;
  }
  if (endFrame >= 0 && endFrame <= startFrame) {
    Q_EMIT renderingError(QString("Video end frame %1 is not correct").arg(endFrame));
    return;
  }
  if (endFrame < 0 || endFrame > totalNumFrames) {
    endFrame = totalNumFrames;
  }
  if (width <= 0 || height <= 0) {
    reportRenderingError(fmt::format("Invalid width {} or height {}", width, height));
    return;
  }
  if (width > FLAGS_maximum_output_width || height > FLAGS_maximum_output_height) {
    reportRenderingError(fmt::format("Does not support output size larger than  {} x {}",
                                     FLAGS_maximum_output_width,
                                     FLAGS_maximum_output_height));
    return;
  }
  CHECK(tileSize >= 0 && tileBorder >= 0);
  LOG(INFO) << fmt::format(
    "fps: {} width: {} height: {} startFrame: {} endFrame: {} startTime: {} endTime: {} tileSize: {} tileBorder: {}",
    framePerSecond,
    width,
    height,
    startFrame,
    endFrame,
    static_cast<double>(startFrame) / framePerSecond,
    static_cast<double>(endFrame) / framePerSecond,
    tileSize,
    tileBorder);

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

    int numFrame = endFrame - startFrame;
    int fieldWidth = std::max(FLAGS_output_image_name_field_width, numDigits(totalNumFrames - 1));
    QString namePrefix = QString::fromStdString(FLAGS_output_image_name_prefix);
    auto tempdir = std::make_shared<QTemporaryDir>();
    QDir tmpdir(imageOuputFolder ? *imageOuputFolder : tempdir->path());
    if (tileSize == 0 || (tileSize >= width && tileSize >= height)) {
      for (int i = startFrame; i < endFrame; ++i) {
        Q_EMIT progressChanged(std::clamp<int>(std::floor((i - startFrame) * 1. / numFrame * 100.), 0, 99));
        if (cancelFlag && cancelFlag->load()) {
          reportCancelError();
          return;
        }

        animation->setCurrentTime(static_cast<double>(i) / framePerSecond);
        QString filename = QString("%1%2.png").arg(namePrefix).arg(i, fieldWidth, 10, QChar('0'));
        QString filepath = tmpdir.filePath(filename);

        takeFixedSizeScreenShotWithoutResetCanvasSizePrivate(filepath, width, height, sst);
      }
    } else {
      int numCols = (width + tileSize - 1) / tileSize;
      int numRows = (height + tileSize - 1) / tileSize;
      bool forwardFrame = false;
      bool forwardCol = false;
      for (int r = 0; r < numRows; ++r) {
        auto tileStartY = r * tileSize;
        forwardCol = !forwardCol;
        for (int c = forwardCol ? 0 : (numCols - 1); forwardCol ? (c < numCols) : (c >= 0); forwardCol ? ++c : --c) {
          auto tileStartX = c * tileSize;
          forwardFrame = !forwardFrame;
          for (int i = forwardFrame ? startFrame : (endFrame - 1); forwardFrame ? (i < endFrame) : (i >= startFrame);
               forwardFrame ? ++i : --i) {
            Q_EMIT progressChanged(std::clamp<int>(
              std::floor(((c * r + r) * numFrame + i - startFrame) * 1. / (numFrame * numCols * numRows) * 100.),
              0,
              98));
            if (cancelFlag && cancelFlag->load()) {
              reportCancelError();
              return;
            }

            animation->setCurrentTime(static_cast<double>(i) / framePerSecond);
            QString filename = QString("_%1%2_%3_%4.png")
                                 .arg(namePrefix)
                                 .arg(i, fieldWidth, 10, QChar('0'))
                                 .arg(tileStartX)
                                 .arg(tileStartY);
            QString filepath = tmpdir.filePath(filename);

            QString rightFilepath;

            if (sst != Z3DScreenShotType::MonoView) {
              filename = QString("_%1%2_%3_%4_left.png")
                           .arg(namePrefix)
                           .arg(i, fieldWidth, 10, QChar('0'))
                           .arg(tileStartX)
                           .arg(tileStartY);
              filepath = tmpdir.filePath(filename);
              filename = QString("_%1%2_%3_%4_right.png")
                           .arg(namePrefix)
                           .arg(i, fieldWidth, 10, QChar('0'))
                           .arg(tileStartX)
                           .arg(tileStartY);
              rightFilepath = tmpdir.filePath(filename);
            }

            takeFixedSizeScreenShotWithoutResetCanvasSizeByTilePrivate(filepath,
                                                                       rightFilepath,
                                                                       width,
                                                                       height,
                                                                       sst,
                                                                       tileSize,
                                                                       tileBorder,
                                                                       tileStartX,
                                                                       tileStartY);
          }
        }
      }

      // compose images
      for (int i = startFrame; i < endFrame; ++i) {
        Q_EMIT progressChanged(99);
        if (cancelFlag && cancelFlag->load()) {
          reportCancelError();
          return;
        }
        QString targetFilename = QString("%1%2.png").arg(namePrefix).arg(i, fieldWidth, 10, QChar('0'));
        auto targetFilepath = tmpdir.filePath(targetFilename);
        ZImg img(ZImgInfo(width, height, 1, 4));
        img.infoRef().lastChannelIsAlphaChannel = true;
        ZImg rightImg;
        if (sst != Z3DScreenShotType::MonoView) {
          rightImg = ZImg(ZImgInfo(width, height, 1, 4));
          rightImg.infoRef().lastChannelIsAlphaChannel = true;
        }
        for (auto c = 0; c < numCols; ++c) {
          for (auto r = 0; r < numRows; ++r) {
            auto tileStartX = c * tileSize;
            auto tileStartY = r * tileSize;
            if (sst == Z3DScreenShotType::MonoView) {
              QString filename = QString("_%1%2_%3_%4.png")
                                   .arg(namePrefix)
                                   .arg(i, fieldWidth, 10, QChar('0'))
                                   .arg(tileStartX)
                                   .arg(tileStartY);
              QString filepath = tmpdir.filePath(filename);
              if (tmpdir.exists(filename)) {
                img.pasteImg(ZImg(filepath), ZVoxelCoordinate(tileStartX, tileStartY));
              } else {
                LOG(ERROR) << "Could not find file: " << filepath;
              }
            } else {
              QString filename = QString("_%1%2_%3_%4_left.png")
                                   .arg(namePrefix)
                                   .arg(i, fieldWidth, 10, QChar('0'))
                                   .arg(tileStartX)
                                   .arg(tileStartY);
              QString filepath = tmpdir.filePath(filename);
              if (tmpdir.exists(filename)) {
                img.pasteImg(ZImg(filepath), ZVoxelCoordinate(tileStartX, tileStartY));
              } else {
                LOG(ERROR) << "Could not find left file: " << filepath;
              }
              filename = QString("_%1%2_%3_%4_right.png")
                           .arg(namePrefix)
                           .arg(i, fieldWidth, 10, QChar('0'))
                           .arg(tileStartX)
                           .arg(tileStartY);
              if (tmpdir.exists(filename)) {
                rightImg.pasteImg(ZImg(filepath), ZVoxelCoordinate(tileStartX, tileStartY));
              } else {
                LOG(ERROR) << "Could not find right file: " << filepath;
              }
            }
          }
        }

        const auto backend = static_cast<RenderBackend>(m_globalParas->renderBackend.associatedData());

        if (sst == Z3DScreenShotType::MonoView) {
          if (backend == RenderBackend::OpenGL) {
            img.flip(Dimension::Y).save(targetFilepath);
          } else {
            img.save(targetFilepath);
          }
          LOG(INFO) << fmt::format("Saved rendering ({}, {}) to file: {}", img.width(), img.height(), targetFilepath);
        } else {
          if (sst == Z3DScreenShotType::FullSideBySideStereoView) {
            if (backend == RenderBackend::OpenGL) {
              ZImg::cat(std::vector<const ZImg*>{&img, &rightImg}, Dimension::X)
                .flip(Dimension::Y)
                .save(targetFilepath);
            } else {
              ZImg::cat(std::vector<const ZImg*>{&img, &rightImg}, Dimension::X).save(targetFilepath);
            }
            LOG(INFO) << fmt::format("Saved stereo rendering ({} x 2, {}) to file: {}",
                                     img.width(),
                                     img.height(),
                                     targetFilepath);
          } else {
            if (backend == RenderBackend::OpenGL) {
              ZImg::cat(std::vector<const ZImg*>{&img, &rightImg}, Dimension::X)
                .zoom(0.5, 1)
                .flip(Dimension::Y)
                .save(targetFilepath);
            } else {
              ZImg::cat(std::vector<const ZImg*>{&img, &rightImg}, Dimension::X).zoom(0.5, 1).save(targetFilepath);
            }
            LOG(INFO) << fmt::format("Saved half sbs stereo rendering ({}, {}) to file: {}",
                                     img.width(),
                                     img.height(),
                                     targetFilepath);
          }
        }
      }
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
  catch (const ZException& e) {
    LOG(ERROR) << e.what();
    reportRenderingError(e.what());
  }
}

ZBBox<glm::dvec3> Z3DRenderingEngine::boundBoxOfObjs(const std::vector<size_t>& ids) const
{
  ZBBox<glm::dvec3> res;
  for (auto id : ids) {
    for (auto& objView : m_3dObjViews) {
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
    for (auto& objView : m_3dObjViews) {
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

  if (!m_vkCompletionPollTimer) {
    // Poll interval for Vulkan fence completion. This timer is only started
    // while the Vulkan frame executor has in-flight submissions, so it does
    // not run during steady-state idle.
    constexpr int kVulkanCompletionPollIntervalMs = 2;

    m_vkCompletionPollTimer = new QTimer(this);
    m_vkCompletionPollTimer->setTimerType(Qt::PreciseTimer);
    m_vkCompletionPollTimer->setInterval(kVulkanCompletionPollIntervalMs);
    connect(m_vkCompletionPollTimer, &QTimer::timeout, this, [this]() {
      if (m_shuttingDown.load(std::memory_order_relaxed)) {
        stopVulkanCompletionPolling();
        return;
      }
      if (!m_vkDevice || !m_globalParas || !m_compositor) {
        stopVulkanCompletionPolling();
        return;
      }

      const auto backend = static_cast<RenderBackend>(m_globalParas->renderBackend.associatedData());
      if (backend != RenderBackend::Vulkan) {
        stopVulkanCompletionPolling();
        return;
      }

      auto* backendImpl = m_compositor->rendererBase().backend();
      if (!backendImpl) {
        stopVulkanCompletionPolling();
        return;
      }

      try {
        backendImpl->pollCompletionsAndPumpSafePoints();
      }
      catch (const ZCancellationException&) {
        // Cancellation is expected during interactive aborts and shutdown.
        VLOG(1) << "Vulkan completion polling cancelled (ZCancellationException)";
      }
      catch (const folly::OperationCancelled&) {
        VLOG(1) << "Vulkan completion polling cancelled (folly::OperationCancelled)";
      }
      catch (const std::exception& e) {
        LOG(ERROR) << "Vulkan completion polling failed: " << e.what();
        CHECK(false) << "Vulkan completion polling failed (exception).";
      }
      catch (...) {
        LOG(ERROR) << "Vulkan completion polling failed (unknown exception)";
        CHECK(false) << "Vulkan completion polling failed (unknown exception).";
      }
      maybeKickDeferredRenderAfterVulkanPoll();
      if (!backendImpl->hasInFlightFrames()) {
        stopVulkanCompletionPolling();
      }
    });
  }

  m_globalParas = std::make_unique<Z3DGlobalParameters>();
  if (m_scratchPool) {
    m_scratchPool->setDefaultBackend(static_cast<RenderBackend>(m_globalParas->renderBackend.associatedData()));
  }
  connect(&m_globalParas->renderBackend, &ZParameter::valueChanged, this, [this]() {
    handleRenderBackendChanged();
  });

  // filters
  m_compositor = std::make_unique<Z3DCompositor>(*m_globalParas);
  // Vulkan bridge deferred
  addEventListenerToBack(*m_compositor);
  connect(m_compositor.get(), &Z3DCompositor::sceneParaUpdated, this, &Z3DRenderingEngine::sceneParaUpdated);
  connect(m_compositor.get(), &Z3DCompositor::renderingFinished, this, &Z3DRenderingEngine::renderingFinished);

  // Initialize pipeline helpers (GL state checker + profiler).
  m_filterWrappers.clear();
  m_filterWrappers.emplace_back(std::make_unique<CheckOpenGLStateFilterWrapper>());
  m_filterWrappers.emplace_back(std::make_unique<ProfileFilterWrapper>());

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
      m_3dObjViews.emplace_back(imgView);
    } else if (auto punctaDoc = qobject_cast<ZPunctaDoc*>(objDoc)) {
      auto punctaView = new Z3DPunctaView(*punctaDoc, *this);
      connect(punctaView, &Z3DPunctaView::objViewReady, this, &Z3DRenderingEngine::objViewReady);
      m_3dObjViews.emplace_back(punctaView);
    } else if (auto swcDoc = qobject_cast<ZSwcDoc*>(objDoc)) {
      auto swcView = new Z3DSwcView(*swcDoc, *this);
      connect(swcView, &Z3DSwcView::objViewReady, this, &Z3DRenderingEngine::objViewReady);
      m_3dObjViews.emplace_back(swcView);
    } else if (auto meshDoc = qobject_cast<ZMeshDoc*>(objDoc)) {
      auto meshView = new Z3DMeshView(*meshDoc, *this);
      connect(meshView, &Z3DMeshView::objViewReady, this, &Z3DRenderingEngine::objViewReady);
      m_3dObjViews.emplace_back(meshView);
    } else if (auto skeletonDoc = qobject_cast<ZSkeletonDoc*>(objDoc)) {
      auto skeletonView = new Z3DSkeletonView(*skeletonDoc, *this);
      connect(skeletonView, &Z3DSkeletonView::objViewReady, this, &Z3DRenderingEngine::objViewReady);
      m_3dObjViews.emplace_back(skeletonView);
    } else if (auto aniDoc = qobject_cast<Z3DAnimationDoc*>(objDoc)) {
      auto aniView = new Z3DAnimationView(*aniDoc, *this);
      connect(aniView, &Z3DAnimationView::objViewReady, this, &Z3DRenderingEngine::objViewReady);
      m_3dObjViews.emplace_back(aniView);
    } else if (auto raDoc = qobject_cast<ZRegionAnnotationDoc*>(objDoc)) {
      auto aniView = new Z3DRegionAnnotationView(*raDoc, *this);
      connect(aniView, &Z3DRegionAnnotationView::objViewReady, this, &Z3DRenderingEngine::objViewReady);
      m_3dObjViews.emplace_back(aniView);
    }
  }

  updateBoundBox();

  // Build initial filter pipeline now that views and compositor are wired.
  updatePipeline();

  Q_EMIT progressChanged(60);

  // adjust camera
  resetCamera();

  connect(&camera(), &Z3DCameraParameter::valueChanged, this, &Z3DRenderingEngine::resetCameraClippingRange);

  LOG(INFO) << "3D Renderer Inited.";

  Q_EMIT progressChanged(100);

  Q_EMIT initialized();

  // When object views become ready, apply any pending per-object View3D JSON
  connect(this, &Z3DRenderingEngine::objViewReady, this, [this](size_t id) {
    auto it = m_pendingObjViewJson.find(id);
    if (it != m_pendingObjViewJson.end()) {
      this->read(id, it->second);
      m_pendingObjViewJson.erase(it);
      if (m_sceneApplyOutstanding > 0) {
        --m_sceneApplyOutstanding;
        if (m_sceneApplyOutstanding == 0) {
          Q_EMIT scene3DApplyFinished();
        }
      }
    }
  });
}

void Z3DRenderingEngine::initAndAttachToCanvas(Z3DCanvas* canvas)
{
  CHECK(canvas);
  setCurrentRenderThreadExecutor(m_renderThreadExecutor.get());
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

void Z3DRenderingEngine::drainVulkanFrameExecutorForTeardown()
{
  // Must run on the rendering thread (engine thread affinity).
  CHECK(QThread::currentThread() == this->thread()) << "drainVulkanFrameExecutorForTeardown must run on engine thread";

  // Stop polling first to avoid re-entrancy while we synchronously drain.
  stopVulkanCompletionPolling();

  if (!m_vkDevice) {
    return;
  }

  // Prefer draining through the Vulkan backend so the backend's completion-safe-point
  // hooks (readback consumers, descriptor-pool resets, deferred scratch releases, etc.)
  // are executed while the compositor/backend objects are still alive.
  if (m_globalParas && m_compositor &&
      static_cast<RenderBackend>(m_globalParas->renderBackend.associatedData()) == RenderBackend::Vulkan) {
    if (auto* backendImpl = m_compositor->rendererBase().backend()) {
      try {
        backendImpl->flushForTeardown("engine_teardown_drain");
        if (backendImpl->supportsCommandLists()) {
          return;
        }
      }
      catch (const std::exception& e) {
        LOG(ERROR) << "Backend flushForTeardown failed during engine teardown drain: " << e.what();
      }
      catch (...) {
        LOG(ERROR) << "Backend flushForTeardown failed during engine teardown drain (unknown exception)";
      }
    }
  }

  // Fallback: at least ensure the VkDevice is idle so nothing outlives it.
  // Note: this does not execute backend safe-point hooks; it is a last resort
  // when the backend cannot be reached during teardown.
  try {
    m_vkDevice->context().device().waitIdle();
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Vulkan waitIdle failed during teardown: " << e.what();
  }
}

void Z3DRenderingEngine::detachCanvas()
{
  // Disconnect UI first to avoid any queued signal posting back to engine/canvas during teardown
  if (m_canvas) {
    m_canvas->disconnect(this);
    disconnect(m_canvas);
    m_canvas->setRenderingEngine(nullptr);
    m_canvas.clear();
  }

  // Now safe to update DPI etc. without waking the canvas
  if (m_globalParas) {
    m_globalParas->setDevicePixelRatio(1);
  }
}

glm::uvec2 Z3DRenderingEngine::outputSize() const
{
  return m_outputSize;
}

void Z3DRenderingEngine::setOutputSize(const glm::uvec2& size)
{
  if (m_outputSize == size) {
    return;
  }

  CHECK_GT(size.x, 0u);
  CHECK_GT(size.y, 0u);

  getGLFocus();
  m_outputSize = size;
  m_globalParas->camera.viewportChanged(size);
  updateAllFilterSizes();
}

void Z3DRenderingEngine::updatePipeline()
{
  m_pipeline.clear();
  std::vector<Z3DGeometryFilter*> geometryFilters;
  std::vector<Z3DImgFilter*> volumeFilters;

  // Collect filters from all object views.
  for (const auto& objView : m_3dObjViews) {
    const auto filters = objView->filters();
    for (auto* filter : filters) {
      if (!filter) {
        continue;
      }
      m_pipeline.push_back(filter);
      if (auto* img = dynamic_cast<Z3DImgFilter*>(filter)) {
        volumeFilters.push_back(img);
      } else if (auto* geom = dynamic_cast<Z3DGeometryFilter*>(filter)) {
        geometryFilters.push_back(geom);
      }
    }
  }
  // Compositor must be the last filter in the pipeline.
  CHECK(m_compositor);
  m_pipeline.push_back(m_compositor.get());

  // Keep compositor geometry/volume lists in sync with the current pipeline.
  m_compositor->setGeometryFilters(geometryFilters);
  m_compositor->setVolumeFilters(volumeFilters);

  updateAllFilterSizes();
}

void Z3DRenderingEngine::updateAllFilterSizes()
{
  const glm::uvec2 targetSize = m_outputSize;
  for (auto it = m_pipeline.rbegin(); it != m_pipeline.rend(); ++it) {
    (*it)->updateSize(targetSize);
  }
}

double Z3DRenderingEngine::processFrame(bool stereo,
                                        bool progressiveRendering,
                                        const folly::CancellationToken* cancellationToken)
{
  if (m_pipeline.empty()) {
    return 1.0;
  }

  const auto backend = static_cast<RenderBackend>(m_globalParas->renderBackend.associatedData());
  const bool glMode = (backend == RenderBackend::OpenGL);

  getGLFocus();

  // Mark the start of a new user-visible frame for perf aggregation.
  Z3DRenderGlobalState::instance().beginNewPerfFrameToken();
  auto& renderState = Z3DRenderGlobalState::instance();

  // Notify filter wrappers (now that token is available for tagging).
  for (auto& wrapper : m_filterWrappers) {
    if (!glMode && dynamic_cast<CheckOpenGLStateFilterWrapper*>(wrapper.get()) != nullptr) {
      continue;
    }
    wrapper->beforeNetworkProcess();
  }
  if (glMode) {
    CHECK_GL_ERROR
  }

  double currentProgress = 0.0;
  double totalProgress = 0.0;

  for (auto* filter : m_pipeline) {
    if (cancellationToken) {
      maybeCancel(*cancellationToken);
    }

    filter->setProgressiveRenderingMode(progressiveRendering);

    const Z3DEye primaryEye = stereo ? LeftEye : MonoEye;

    auto processEye = [&](Z3DEye eye) {
      if (filter->isValid(eye) || !filter->isReady(eye)) {
        return;
      }

      for (auto& wrapper : m_filterWrappers) {
        if (!glMode && dynamic_cast<CheckOpenGLStateFilterWrapper*>(wrapper.get()) != nullptr) {
          continue;
        }
        wrapper->beforeFilterProcess(filter);
      }
      if (glMode) {
        CHECK_GL_ERROR
      }

      double progress = filter->process(eye);
      if (progress == 1.0) {
        if (filter == m_compositor.get()) {
          if (totalProgress == currentProgress) {
            filter->setValid(eye);
          }
        } else {
          filter->setValid(eye);
        }
      }
      currentProgress += progress;
      totalProgress += 1.0;
      if (glMode) {
        CHECK_GL_ERROR
      }

      for (auto& wrapper : m_filterWrappers) {
        if (!glMode && dynamic_cast<CheckOpenGLStateFilterWrapper*>(wrapper.get()) != nullptr) {
          continue;
        }
        wrapper->afterFilterProcess(filter);
      }
      if (glMode) {
        CHECK_GL_ERROR
      }
    };

    processEye(primaryEye);
    if (stereo) {
      processEye(RightEye);
    }
  }

  for (auto& wrapper : m_filterWrappers) {
    wrapper->afterNetworkProcess();
  }
  if (glMode) {
    CHECK_GL_ERROR
  }

  // Mark the current perf frame token as closed for aggregation. Actual flush
  // occurs after submission results are ingested (typically on the next frame).
  nim::Z3DPerfCollector::instance().markClosed(renderState.currentPerfFrameToken());

  if (!progressiveRendering) {
    CHECK(currentProgress == totalProgress) << currentProgress << " " << totalProgress;
  }
  return totalProgress > 0.0 ? currentProgress / totalProgress : 1.0;
}

ZImg Z3DRenderingEngine::localColorBufferToRGBAImg(const Z3DLocalColorBuffer& buffer)
{
  if (buffer.width == 0 || buffer.height == 0) {
    return {};
  }

  const auto backend = static_cast<RenderBackend>(m_globalParas->renderBackend.associatedData());

  const size_t w = buffer.width;
  const size_t h = buffer.height;
  const uint8_t* srcPtr = nullptr;
  if (backend == RenderBackend::Vulkan) {
    const size_t expectedStride = w * 4ull;
    CHECK(buffer.external != nullptr) << "Vulkan path requires external mapped color buffer";
    CHECK(buffer.externalStride == 0 || buffer.externalStride == expectedStride)
      << "Unexpected external stride for LocalColorBuffer";
    srcPtr = buffer.external;
  } else { // OpenGL
    CHECK(!buffer.data.empty()) << "OpenGL path requires CPU localColorBuffer data";
    srcPtr = buffer.data.data();
  }

  ZImg bufImg;
  bufImg.wrapData(const_cast<uint8_t*>(srcPtr), ZImgInfo(static_cast<int>(w), static_cast<int>(h), 1, 4));
  ZImg res(bufImg.info());
  ZImgFormat::CXYZtoXYZC(bufImg, res, backend == RenderBackend::OpenGL);
  res.infoRef().lastChannelIsAlphaChannel = true;
  res.correctPreMultipliedColor();
  return res;
}

void Z3DRenderingEngine::beginScene3DApply()
{
  // Reset apply session
  m_pendingObjViewJson.clear();
  m_sceneApplyOutstanding = 0;
}

void Z3DRenderingEngine::applyView3DGeneral(const json::object& json)
{
  // One job starts
  ++m_sceneApplyOutstanding;
  // Apply immediately on engine thread
  this->read(json);
  // Job finished
  --m_sceneApplyOutstanding;
  if (m_sceneApplyOutstanding == 0) {
    LOG(INFO) << "3D scene parameters applied";
    Q_EMIT scene3DApplyFinished();
  }
}

void Z3DRenderingEngine::applyView3DForId(size_t id, json::object json)
{
  // One job starts
  ++m_sceneApplyOutstanding;
  bool found = false;
  for (auto& objView : m_3dObjViews) {
    if (objView->hasObj(id)) {
      found = true;
      break;
    }
  }
  if (found) {
    this->read(id, json);
    --m_sceneApplyOutstanding;
    if (m_sceneApplyOutstanding == 0) {
      LOG(INFO) << "3D scene parameters applied";
      Q_EMIT scene3DApplyFinished();
    }
  } else {
    // Queue it until objViewReady(id)
    m_pendingObjViewJson[id] = std::move(json);
  }
}

std::vector<ZParameter*> Z3DRenderingEngine::parametersOfViewSetting(size_t id)
{
  std::vector<ZParameter*> res;
  auto installWatcher = [this, id](const std::shared_ptr<ZWidgetsGroup>& wg) {
    if (!wg) {
      return;
    }
    const ZWidgetsGroup* wgPtr = wg.get();
    if (m_observedWGs.find(wgPtr) == m_observedWGs.end()) {
      m_observedWGs.insert(wgPtr);
      QObject::connect(wgPtr, &ZWidgetsGroup::widgetsGroupChanged, this, [this, id]() {
        if (!m_shuttingDown.load(std::memory_order_relaxed)) {
          Q_EMIT viewSettingWidgetsGroupChanged(id);
        }
      });
      QObject::connect(wgPtr, &QObject::destroyed, this, [this, wgPtr]() {
        if (!m_shuttingDown.load(std::memory_order_relaxed)) {
          m_observedWGs.erase(wgPtr);
        }
      });
    }
  };

  if (id == 0) {
    // Expose camera as a view-setting parameter group for id=0
    res.push_back(&m_globalParas->camera);
    return res;
  } else if (id == 1) {
    auto wg = m_compositor->backgroundWidgetsGroup();
    installWatcher(wg);
    return wg->getParameterList();
  } else if (id == 2) {
    auto wg = m_compositor->axisWidgetsGroup();
    installWatcher(wg);
    return wg->getParameterList();
  } else if (id == 3) {
    auto wg = m_globalParas->widgetsGroup(false, *this);
    installWatcher(wg);
    return wg->getParameterList();
  }
  for (auto& objView : m_3dObjViews) {
    if (objView->hasObj(id)) {
      auto wg = objView->viewSettingWidgetsGroupOf(id);
      installWatcher(wg);
      return wg ? wg->getParameterList() : res;
    }
  }
  return res;
}

void Z3DRenderingEngine::onCanvasResized(size_t w, size_t h)
{
  setOutputSize(glm::uvec2(w, h));
}

void Z3DRenderingEngine::initGL()
{
  if (!m_scratchPool) {
    m_scratchPool.reset(new Z3DScratchResourcePool());
    Z3DRenderGlobalState::instance().setScratchPool(m_scratchPool.get());
  }

  ensureGLContext();
}

void Z3DRenderingEngine::ensureGLContext()
{
  if (m_context) {
    m_context->makeCurrent();
    return;
  }

  if (m_canvas) {
#if defined(ATLAS_USE_OPENGLWIDGET)
    m_context = std::make_unique<Z3DContext>(*m_offscreenSurface, m_canvas->context());
#else
    m_context = std::make_unique<Z3DContext>(*m_offscreenSurface);
#endif
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

  // Populate GL-backed caps and log info
  Z3DGpuInfo::instance().initializeFromOpenGL();
  Z3DGpuInfo::instance().logGpuInfo();

  if (FLAGS_atlas_debug_opengl) {
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

  static bool contextSwitchCallbackRegistered = false;
  if (FLAGS_atlas_log_glbinding_context_switch && !contextSwitchCallbackRegistered) {
    glbinding::addContextSwitchCallback([](glbinding::ContextHandle handle) {
      LOG(INFO) << "Switching to OpenGL context " << handle;
    });
    contextSwitchCallbackRegistered = true;
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
  // VLOG(1) << e->type();
  if (m_shuttingDown.load(std::memory_order_relaxed)) {
    // Ignore any late events posted during shutdown
    e->ignore();
    return true;
  }
  if (m_eventTypes.contains(e->type())) {
    if (e->type() == QEvent::UpdateRequest) {
      renderFast();
      e->accept();
      return true;
    } else if (e->type() == QEvent::LayoutRequest) {
      render();
      e->accept();
      return true;
    }
    auto outputSize = m_outputSize;
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
  // In Vulkan/headless paths there may be no GL context; treat as no-op.
  if (!m_context) {
    return;
  }
  m_context->makeCurrent();
}

void Z3DRenderingEngine::renderFast(bool stereo)
{
  if (m_isRendering) {
    LOG(INFO) << "in fast rendering, schedule a update later";
    QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
    return;
  }
  if (Z3DRenderGlobalState::instance().hasCancellationSource()) {
    Z3DRenderGlobalState::instance().requestCancellation();
    LOG(INFO) << "cancel rendering, schedule a update later";
    QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
    return;
  }
  if (shouldDeferVulkanNetworkProcessing()) {
    // Vulkan async readback mode: avoid starting expensive CPU network processing
    // while the previous presentable frame is still in flight. This coalesces
    // bursty parameter changes into a single render once the GPU is ready.
    deferRenderUntilVulkanIdle(QEvent::UpdateRequest);
    return;
  }

  VLOG(1) << "renderFast";
  Q_EMIT progressChanged(10);
  m_isRendering = true;
  auto renderingGuard = folly::makeGuard([this]() {
    m_isRendering = false;
  });

  auto& renderState = Z3DRenderGlobalState::instance();
  auto cancellationSource = renderState.ensureCancellationSource();
  CHECK(cancellationSource);
  auto cancellationGuard = folly::makeGuard([&renderState]() {
    renderState.resetCancellationSource();
  });

  try {
    const auto token = cancellationSource->getToken();
    m_progress = processFrame(stereo, true, &token);
    maybeStartVulkanCompletionPolling();
  }
  catch (const ZCancellationException&) {
    LOG(INFO) << "cancelled, schedule a update later";
    QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
    Q_EMIT progressChanged(100);
    return;
  }
  catch (const folly::OperationCancelled&) {
    LOG(INFO) << "cancelled (folly), schedule a update later";
    QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
    Q_EMIT progressChanged(100);
    return;
  }
  catch (const ZException& e) {
    LOG(INFO) << e.what();
  }

  Q_EMIT progressChanged(std::clamp<int>(m_progress * 100., 0, 100));
  if (m_progress < 1.) {
    QCoreApplication::postEvent(this, new QEvent(QEvent::LayoutRequest), Qt::LowEventPriority - 1);
  } else {
    Q_EMIT progressChanged(100);
  }
}

void Z3DRenderingEngine::render(bool stereo)
{
  CHECK(!m_isRendering);
  CHECK(!Z3DRenderGlobalState::instance().hasCancellationSource());

  VLOG(1) << "render";
  auto completionPollGuard = folly::makeGuard([this]() {
    // Arm completion polling when returning to the event loop. The QTimer cannot
    // fire while we are inside this tight progressive loop.
    maybeStartVulkanCompletionPolling();
  });
  try {
    auto cancellationSource = Z3DRenderGlobalState::instance().ensureCancellationSource();
    CHECK(cancellationSource);
    while (m_progress < 1.0) {
      auto token = cancellationSource->getToken();
      m_progress = processFrame(stereo, true, &token);
      pollVulkanCompletionsOnce();
      Q_EMIT progressChanged(std::clamp<int>(m_progress * 100., 0, 100));
    }
  }
  catch (const ZCancellationException&) {
    LOG(INFO) << "cancelled, schedule a update later";
    QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
  }
  catch (const folly::OperationCancelled&) {
    LOG(INFO) << "cancelled (folly), schedule a update later";
    QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
  }
  catch (const ZException& e) {
    LOG(INFO) << e.what();
  }
  Q_EMIT progressChanged(100);
  Z3DRenderGlobalState::instance().resetCancellationSource();
}

Z3DLocalColorBuffer* Z3DRenderingEngine::monoReadyLocalBuffer() const
{
  return m_compositor->monoReadyLocalBuffer();
}

Z3DLocalColorBuffer* Z3DRenderingEngine::leftReadyLocalBuffer() const
{
  return m_compositor->leftReadyLocalBuffer();
}

Z3DLocalColorBuffer* Z3DRenderingEngine::rightReadyLocalBuffer() const
{
  return m_compositor->rightReadyLocalBuffer();
}

void Z3DRenderingEngine::takeFixedSizeScreenShotWithoutResetCanvasSizePrivate(const QString& filename,
                                                                              int width,
                                                                              int height,
                                                                              Z3DScreenShotType sst)
{
  m_isRendering = true;
  auto renderingGuard = folly::makeGuard([this]() {
    m_isRendering = false;
  });
  const int tileSize = 7680; // 2048;
  const int tileBorder = 128;

  if (width <= tileSize && height <= tileSize) {
    // resize texture container to desired image dimensions and propagate change
    setOutputSize(glm::uvec2(width, height));

    takeScreenShotPrivate(filename, sst);
  } else {
    ZImg img(ZImgInfo(width, height, 1, 4));
    img.infoRef().lastChannelIsAlphaChannel = true;
    ZImg rightImg;
    if (sst != Z3DScreenShotType::MonoView) {
      rightImg = ZImg(ZImgInfo(width, height, 1, 4));
      rightImg.infoRef().lastChannelIsAlphaChannel = true;
    }

    int numCols = (width + tileSize - 1) / tileSize;
    int numRows = (height + tileSize - 1) / tileSize;
    bool forwardCol = false;
    for (int r = 0; r < numRows; ++r) {
      auto tileStartY = r * tileSize;
      forwardCol = !forwardCol;
      for (int c = forwardCol ? 0 : (numCols - 1); forwardCol ? (c < numCols) : (c >= 0); forwardCol ? ++c : --c) {
        auto tileStartX = c * tileSize;

        int left = tileStartX;
        int right = std::min(tileStartX + tileSize, width);
        int bottom = tileStartY;
        int top = std::min(tileStartY + tileSize, height);
        double nLeft = static_cast<double>(left - tileBorder) / width;
        double nRight = static_cast<double>(right + tileBorder) / width;
        double nBottom = static_cast<double>(bottom - tileBorder) / height;
        double nTop = static_cast<double>(top + tileBorder) / height;
        ZImgRegion validRegion(tileBorder, tileBorder + right - left, tileBorder, tileBorder + top - bottom);

        setOutputSize(glm::uvec2(right - left + tileBorder * 2, top - bottom + tileBorder * 2));
        m_globalParas->camera.viewportChanged(glm::uvec2(width, height));

        // set camera frustum
        m_globalParas->camera.setTileFrustum(nLeft, nRight, nBottom, nTop);
        m_compositor->setRenderingRegion(nLeft, nRight, nBottom, nTop);

        // Evaluate the filter pipeline for this tile.
        processFrame(sst != Z3DScreenShotType::MonoView, false);

        if (sst == Z3DScreenShotType::MonoView) {
          img.pasteImg(localColorBufferToRGBAImg(*m_compositor->monoReadyLocalBuffer()).crop(validRegion),
                       ZVoxelCoordinate(tileStartX, tileStartY));
        } else {
          img.pasteImg(localColorBufferToRGBAImg(*m_compositor->leftReadyLocalBuffer()).crop(validRegion),
                       ZVoxelCoordinate(tileStartX, tileStartY));
          rightImg.pasteImg(localColorBufferToRGBAImg(*m_compositor->rightReadyLocalBuffer()).crop(validRegion),
                            ZVoxelCoordinate(tileStartX, tileStartY));
        }
      }
    }

    const auto backend = static_cast<RenderBackend>(m_globalParas->renderBackend.associatedData());

    if (sst == Z3DScreenShotType::MonoView) {
      if (backend == RenderBackend::OpenGL) {
        img.flip(Dimension::Y).save(filename);
      } else {
        img.save(filename);
      }
      LOG(INFO) << "Saved rendering (" << img.width() << ", " << img.height() << ") to file: " << filename;
    } else {
      if (sst == Z3DScreenShotType::FullSideBySideStereoView) {
        if (backend == RenderBackend::OpenGL) {
          ZImg::cat(std::vector<const ZImg*>{&img, &rightImg}, Dimension::X).flip(Dimension::Y).save(filename);
        } else {
          ZImg::cat(std::vector<const ZImg*>{&img, &rightImg}, Dimension::X).save(filename);
        }
        LOG(INFO) << "Saved stereo rendering (" << img.width() << " x 2, " << img.height() << ") to file: " << filename;
      } else {
        if (backend == RenderBackend::OpenGL) {
          ZImg::cat(std::vector<const ZImg*>{&img, &rightImg}, Dimension::X)
            .zoom(0.5, 1)
            .flip(Dimension::Y)
            .save(filename);
        } else {
          ZImg::cat(std::vector<const ZImg*>{&img, &rightImg}, Dimension::X).zoom(0.5, 1).save(filename);
        }
        LOG(INFO) << "Saved half sbs stereo rendering (" << img.width() << ", " << img.height()
                  << ") to file:" << filename;
      }
    }

    m_globalParas->camera.setTileFrustum();
    m_compositor->setRenderingRegion();
  }
}

void Z3DRenderingEngine::takeFixedSizeScreenShotWithoutResetCanvasSizeByTilePrivate(const QString& filename,
                                                                                    const QString& rightFilename,
                                                                                    int width,
                                                                                    int height,
                                                                                    Z3DScreenShotType sst,
                                                                                    int tileSize,
                                                                                    int tileBorder,
                                                                                    int tileStartX,
                                                                                    int tileStartY)
{
  m_isRendering = true;
  auto renderingGuard = folly::makeGuard([this]() {
    m_isRendering = false;
  });

  CHECK(tileSize > 0);
  CHECK(width > tileSize || height > tileSize);
  tileBorder = std::max(tileBorder, 16);

  int left = tileStartX;
  int right = std::min(tileStartX + tileSize, width);
  int bottom = tileStartY;
  int top = std::min(tileStartY + tileSize, height);
  double nLeft = static_cast<double>(left - tileBorder) / width;
  double nRight = static_cast<double>(right + tileBorder) / width;
  double nBottom = static_cast<double>(bottom - tileBorder) / height;
  double nTop = static_cast<double>(top + tileBorder) / height;
  ZImgRegion validRegion(tileBorder, tileBorder + right - left, tileBorder, tileBorder + top - bottom);

  setOutputSize(glm::uvec2(right - left + tileBorder * 2, top - bottom + tileBorder * 2));
  m_globalParas->camera.viewportChanged(glm::uvec2(width, height));

  // set camera frustum
  m_globalParas->camera.setTileFrustum(nLeft, nRight, nBottom, nTop);
  m_compositor->setRenderingRegion(nLeft, nRight, nBottom, nTop);
  auto regionGuard = folly::makeGuard([this]() {
    m_globalParas->camera.setTileFrustum();
    m_compositor->setRenderingRegion();
  });
  // Evaluate the filter pipeline for this tile.
  processFrame(sst != Z3DScreenShotType::MonoView, false);

  if (sst == Z3DScreenShotType::MonoView) {
    localColorBufferToRGBAImg(*m_compositor->monoReadyLocalBuffer()).crop(validRegion).save(filename);
    LOG(INFO) << fmt::format("Saved tiled rendering (total: {} x {}, tile: X {}, Y {}, size {}, border {}) to file: {}",
                             width,
                             height,
                             tileStartX,
                             tileStartY,
                             tileSize,
                             tileBorder,
                             filename);
  } else {
    localColorBufferToRGBAImg(*m_compositor->leftReadyLocalBuffer()).crop(validRegion).save(filename);
    LOG(INFO) << fmt::format(
      "Saved left tiled rendering (total: {} x {}, tile: X {}, Y {}, size {}, border {}) to file: {}",
      width,
      height,
      tileStartX,
      tileStartY,
      tileSize,
      tileBorder,
      filename);
    localColorBufferToRGBAImg(*m_compositor->rightReadyLocalBuffer()).crop(validRegion).save(rightFilename);
    LOG(INFO) << fmt::format(
      "Saved right tiled rendering (total: {} x {}, tile: X {}, Y {}, size {}, border {}) to file: {}",
      width,
      height,
      tileStartX,
      tileStartY,
      tileSize,
      tileBorder,
      rightFilename);
  }
}

void Z3DRenderingEngine::takeScreenShotPrivate(const QString& filename, Z3DScreenShotType sst)
{
  m_isRendering = true;
  auto renderingGuard = folly::makeGuard([this]() {
    m_isRendering = false;
  });

  const auto backend = static_cast<RenderBackend>(m_globalParas->renderBackend.associatedData());
  processFrame(sst != Z3DScreenShotType::MonoView, false);

  if (sst == Z3DScreenShotType::MonoView) {
    auto img = localColorBufferToRGBAImg(*m_compositor->monoReadyLocalBuffer());
    if (backend == RenderBackend::OpenGL) {
      img.flip(Dimension::Y).save(filename);
    } else {
      img.save(filename);
    }
    LOG(INFO) << "Saved rendering (" << img.width() << ", " << img.height() << ") to file: " << filename;
  } else {
    auto leftImg = localColorBufferToRGBAImg(*m_compositor->leftReadyLocalBuffer());
    auto rightImg = localColorBufferToRGBAImg(*m_compositor->rightReadyLocalBuffer());

    if (sst == Z3DScreenShotType::FullSideBySideStereoView) {
      if (backend == RenderBackend::OpenGL) {
        ZImg::cat(std::vector<const ZImg*>{&leftImg, &rightImg}, Dimension::X).flip(Dimension::Y).save(filename);
      } else {
        ZImg::cat(std::vector<const ZImg*>{&leftImg, &rightImg}, Dimension::X).save(filename);
      }
      LOG(INFO) << "Saved stereo rendering (" << leftImg.width() << " x 2, " << leftImg.height()
                << ") to file: " << filename;
    } else {
      if (backend == RenderBackend::OpenGL) {
        ZImg::cat(std::vector<const ZImg*>{&leftImg, &rightImg}, Dimension::X)
          .zoom(0.5, 1)
          .flip(Dimension::Y)
          .save(filename);
      } else {
        ZImg::cat(std::vector<const ZImg*>{&leftImg, &rightImg}, Dimension::X).zoom(0.5, 1).save(filename);
      }
      LOG(INFO) << "Saved half sbs stereo rendering (" << leftImg.width() << ", " << leftImg.height()
                << ") to file:" << filename;
    }
  }
}

void Z3DRenderingEngine::saveCurrentFrameColor(const QString& filename, Z3DEye eye)
{
  m_compositor->saveOutputColorToImage(filename, eye);
}

void Z3DRenderingEngine::saveCurrentFrameDepth(const QString& filename, Z3DEye eye)
{
  m_compositor->saveOutputDepthToImage(filename, eye);
}

void Z3DRenderingEngine::resetOutputSizeToMatchCanvasSize()
{
  if (m_canvas) {
    setOutputSize(m_canvas->physicalSize());
  }
}

void Z3DRenderingEngine::handleRenderBackendChanged()
{
  VLOG(1) << "Render backend configuration changed";
  if (Z3DRenderGlobalState::instance().hasCancellationSource()) {
    Z3DRenderGlobalState::instance().requestCancellation();
    VLOG(1) << "Active render detected; queuing backend switch after cancellation.";

    if (!m_backendSwitchScheduled) {
      m_backendSwitchScheduled = true;
      QMetaObject::invokeMethod(
        this,
        [this]() {
          applyBackendSwitch();
        },
        Qt::QueuedConnection);
    }
  } else {
    VLOG(1) << "No active render; applying backend switch immediately";
    applyBackendSwitch();
  }
}

void Z3DRenderingEngine::applyBackendSwitch()
{
  VLOG(1) << "Entering applyBackendSwitch";
  stopVulkanCompletionPolling();
  auto resetScheduleGuard = folly::makeGuard([this]() {
    m_backendSwitchScheduled = false;
  });

  const auto backend = static_cast<RenderBackend>(m_globalParas->renderBackend.associatedData());
  std::scoped_lock lock(targetSwitchMutex());
  VLOG(1) << fmt::format("Switching rendering backend to {}", enumToString(backend));

  if (backend == RenderBackend::OpenGL) {
    // If switching away from Vulkan, ensure the GPU is idle before destroying scratch resources
    VLOG(1) << "Waiting for Vulkan device to become idle before switching to OpenGL";
    try {
      m_vkDevice->context().device().waitIdle();
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Vulkan waitIdle before pool reset failed: " << e.what();
    }

    VLOG(1) << "Ensuring OpenGL context is current for backend switch";
    ensureGLContext();
    if (!m_context) {
      const auto errorMsg = QStringLiteral("Failed to create OpenGL context for backend switch");
      LOG(ERROR) << errorMsg;
      reportRenderingError(errorMsg);
      return;
    }
    VLOG(1) << "OpenGL context ready for backend switch";
  }

  // Prepare Vulkan device if targeting Vulkan; inject into scratch pool before any Vulkan allocations
  if (backend == RenderBackend::Vulkan) {
    // Make GL current to safely tear down any GL resources in release paths
    VLOG(1) << "Preparing to switch to Vulkan backend";
    getGLFocus();

    if (!m_vkContext) {
      VLOG(1) << "Creating Vulkan context";
      try {
        m_vkContext = std::make_unique<ZVulkanContext>();
      }
      catch (const std::exception& e) {
        const auto errorMsg = fmt::format("Failed to create Vulkan context: {}", e.what());
        LOG(ERROR) << errorMsg;
        reportRenderingError(errorMsg);
        return;
      }
      VLOG(1) << "Vulkan context created";
    } else {
      VLOG(1) << "Reusing existing Vulkan context";
    }
    if (!m_vkDevice) {
      VLOG(1) << "Creating Vulkan device";
      try {
        m_vkDevice = m_vkContext->createDevice();
      }
      catch (const std::exception& e) {
        const auto errorMsg = fmt::format("Failed to create Vulkan device: {}", e.what());
        LOG(ERROR) << errorMsg;
        reportRenderingError(errorMsg);
        return;
      }
      VLOG(1) << "Vulkan device created";
    } else {
      VLOG(1) << "Reusing existing Vulkan device";
    }

    // Populate generic GPU caps from the selected Vulkan physical device so
    // shared code (e.g., image scaling limits) uses device-accurate numbers.
    auto& phys = m_vkContext->physicalDevice();
    const auto props = phys.getProperties();
    const auto features = phys.getFeatures();
    const auto memProps = phys.getMemoryProperties();

    uint64_t vramBytes = 0;
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
      if (memProps.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
        vramBytes += memProps.memoryHeaps[i].size;
      }
    }
    Z3DGpuInfo::GenericCaps caps;
    caps.maxTextureSize = props.limits.maxImageDimension2D;
    caps.max3DTextureSize = props.limits.maxImageDimension3D;
    caps.maxArrayTextureLayers = static_cast<int>(props.limits.maxImageArrayLayers);
    caps.maxColorAttachments = static_cast<int>(props.limits.maxColorAttachments);
    caps.maxTextureAnisotropy = (features.samplerAnisotropy ? props.limits.maxSamplerAnisotropy : 1.0f);
    caps.dedicatedVideoMemoryMB = static_cast<uint64_t>(vramBytes / (1024ull * 1024ull));
    caps.maxViewportDim =
      static_cast<int>(std::min(props.limits.maxViewportDimensions[0], props.limits.maxViewportDimensions[1]));
    // Reasonable defaults for GL-only caps when running under Vulkan
    caps.maxCombinedTextureImageUnits = 48;
    caps.maxTextureImageUnits = 16;
    caps.maxVertexTextureImageUnits = 16;
    caps.maxGeometryTextureImageUnits = 16;
    caps.maxTextureBufferSize = static_cast<int>(props.limits.maxTexelBufferElements);
    caps.maxDrawBuffer = static_cast<int>(props.limits.maxColorAttachments);

    Z3DGpuInfo::instance().overrideGenericCaps(caps);
    VLOG(1) << "Updated Z3DGpuInfo caps from Vulkan device";

    // Log Vulkan device/capabilities in a consistent style
    m_vkContext->logGpuInfo();

    // The scratch pool outlives the backend(s) and simply stores a borrowed pointer. All
    // command buffers and attachments retrieved through the Vulkan backend assume the active
    // device matches this pointer, so refresh it immediately after (re-)creating the device.
    m_scratchPool->setVulkanDevice(m_vkDevice.get());
    VLOG(1) << "Updated scratch pool to use active Vulkan device";
  }

  // Ensure scratch-pool deferred-release scheduler is cleared before propagating
  // the backend switch to filters. This avoids callbacks into a soon-to-be-
  // destroyed Vulkan backend when filters release their persistent Vulkan leases
  // during the switch.
  if (m_scratchPool) {
    m_scratchPool->setVulkanReleaseScheduler(std::function<void(std::function<void()>)>());
  }

  // Switch renderer backends for compositor + connected filters (this will idle Vulkan via preBackendSwitch)
  VLOG(1) << "Switching compositor and pipeline filters to new backend";
  m_compositor->switchBackend(backend);

  // Propagate backend change to all filters in the current pipeline. The
  // compositor is already switched above, so skip it here.
  std::unordered_set<Z3DBoundedFilter*> seen;
  for (auto* filter : m_pipeline) {
    if (!filter || filter == m_compositor.get()) {
      continue;
    }
    if (auto* bounded = dynamic_cast<Z3DBoundedFilter*>(filter)) {
      if (seen.insert(bounded).second) {
        VLOG(1) << fmt::format("Propagating backend to filter {}", static_cast<const void*>(bounded));
        bounded->switchRendererBackend(backend);
      }
    }
  }

  // Set the pool default backend so subsequent allocations use the new API
  VLOG(1) << "Resetting scratch pool state after backend switch";
  m_scratchPool->reset();
  m_scratchPool->setDefaultBackend(backend);
  VLOG(1) << fmt::format("Scratch pool default backend set to {}", enumToString(backend));

  if (backend == RenderBackend::Vulkan) {
    // Drop any cached GL shaders tied to the current GL context before
    // destroying it. This ensures their glDeleteShader calls happen with
    // a valid context and prevents late deletions against a different or
    // nonexistent context.
    VLOG(1) << "Clearing GL shader cache before releasing GL context";
    Z3DShaderManager::instance().clear();

    // Drop GL context after transition to Vulkan to avoid accidental usage
    m_context.reset();
    VLOG(1) << "Released GL context after switching to Vulkan";
  }

  m_globalParas->camera.get().setBackend(backend);

  // After switching clip space conventions, recompute near/far to avoid
  // accidental clipping differences between backends.
  resetCameraClippingRange();

  m_progress = 0.0;
  m_isRendering = false;
  VLOG(1) << "Backend switch state reset; requesting update";

  QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
  VLOG(1) << fmt::format("Backend switch to {} complete; update event posted", enumToString(backend));
}

void Z3DRenderingEngine::pollVulkanCompletionsOnce()
{
  CHECK(QThread::currentThread() == this->thread()) << "pollVulkanCompletionsOnce must run on engine thread";

  if (!m_vkDevice || !m_globalParas || !m_compositor) {
    return;
  }
  const auto backend = static_cast<RenderBackend>(m_globalParas->renderBackend.associatedData());
  if (backend != RenderBackend::Vulkan) {
    return;
  }

  auto* backendImpl = m_compositor->rendererBase().backend();
  if (!backendImpl) {
    return;
  }

  // Pump completion callbacks without posting any deferred render events. This
  // is safe inside the tight progressive render loop and allows async readback
  // consumers (e.g. presentable RGBA8) to run as soon as fences signal.
  try {
    backendImpl->pollCompletionsAndPumpSafePoints();
  }
  catch (const ZCancellationException&) {
    VLOG(1) << "Vulkan completion poll cancelled (ZCancellationException)";
  }
  catch (const folly::OperationCancelled&) {
    VLOG(1) << "Vulkan completion poll cancelled (folly::OperationCancelled)";
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Vulkan completion poll failed: " << e.what();
    CHECK(false) << "Vulkan completion poll failed (exception).";
  }
  catch (...) {
    LOG(ERROR) << "Vulkan completion poll failed (unknown exception)";
    CHECK(false) << "Vulkan completion poll failed (unknown exception).";
  }
}

void Z3DRenderingEngine::maybeStartVulkanCompletionPolling()
{
  CHECK(QThread::currentThread() == this->thread()) << "maybeStartVulkanCompletionPolling must run on engine thread";

  if (!m_vkCompletionPollTimer || !m_vkDevice || !m_globalParas || !m_compositor) {
    return;
  }
  const auto backend = static_cast<RenderBackend>(m_globalParas->renderBackend.associatedData());
  if (backend != RenderBackend::Vulkan) {
    stopVulkanCompletionPolling();
    return;
  }

  auto* backendImpl = m_compositor->rendererBase().backend();
  if (!backendImpl) {
    stopVulkanCompletionPolling();
    return;
  }

  // Poll once immediately so frames that finish quickly are presented with
  // minimal latency, then arm a timer while submissions remain in flight.
  try {
    backendImpl->pollCompletionsAndPumpSafePoints();
  }
  catch (const ZCancellationException&) {
    VLOG(1) << "Vulkan completion polling cancelled (ZCancellationException)";
  }
  catch (const folly::OperationCancelled&) {
    VLOG(1) << "Vulkan completion polling cancelled (folly::OperationCancelled)";
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Vulkan completion polling failed: " << e.what();
    CHECK(false) << "Vulkan completion polling failed (exception).";
  }
  catch (...) {
    LOG(ERROR) << "Vulkan completion polling failed (unknown exception)";
    CHECK(false) << "Vulkan completion polling failed (unknown exception).";
  }
  maybeKickDeferredRenderAfterVulkanPoll();
  if (backendImpl->hasInFlightFrames()) {
    if (!m_vkCompletionPollTimer->isActive()) {
      m_vkCompletionPollTimer->start();
    }
  } else {
    stopVulkanCompletionPolling();
  }
}

void Z3DRenderingEngine::stopVulkanCompletionPolling()
{
  if (m_vkCompletionPollTimer && m_vkCompletionPollTimer->isActive()) {
    m_vkCompletionPollTimer->stop();
  }
}

bool Z3DRenderingEngine::shouldDeferVulkanNetworkProcessing() const
{
  CHECK(QThread::currentThread() == this->thread()) << "shouldDeferVulkanNetworkProcessing must run on engine thread";

  if (!m_vkDevice || !m_globalParas || !m_compositor) {
    return false;
  }
  const auto backend = static_cast<RenderBackend>(m_globalParas->renderBackend.associatedData());
  if (backend != RenderBackend::Vulkan) {
    return false;
  }
  auto* backendImpl = m_compositor->rendererBase().backend();
  if (!backendImpl) {
    return false;
  }

  const uint32_t inFlight = backendImpl->inFlightCount();
  const uint32_t maxInFlight = backendImpl->maxFramesInFlight();
  if (maxInFlight == 0u) {
    // Backend does not expose an in-flight frame limit; do not apply Vulkan-style backpressure.
    return false;
  }
  // Defer starting CPU network processing if we'd have to block in beginFrame()
  // waiting for a frame slot fence. Allow pipelining up to maxFramesInFlight.
  return inFlight >= maxInFlight;
}

void Z3DRenderingEngine::deferRenderUntilVulkanIdle(QEvent::Type deferredType)
{
  CHECK(QThread::currentThread() == this->thread()) << "deferRenderUntilVulkanIdle must run on engine thread";

  if (!m_vkDeferredRenderEventType.has_value()) {
    m_vkDeferredRenderEventType = deferredType;
  } else if (deferredType == QEvent::UpdateRequest) {
    // Interactive updates always win over progressive refinement.
    m_vkDeferredRenderEventType = QEvent::UpdateRequest;
  } else if (*m_vkDeferredRenderEventType != QEvent::UpdateRequest) {
    m_vkDeferredRenderEventType = deferredType;
  }

  maybeStartVulkanCompletionPolling();
}

void Z3DRenderingEngine::maybeKickDeferredRenderAfterVulkanPoll()
{
  CHECK(QThread::currentThread() == this->thread())
    << "maybeKickDeferredRenderAfterVulkanPoll must run on engine thread";

  if (m_shuttingDown.load(std::memory_order_relaxed)) {
    return;
  }
  if (!m_vkDeferredRenderEventType.has_value() || !m_vkDevice) {
    return;
  }
  if (!m_globalParas || !m_compositor ||
      static_cast<RenderBackend>(m_globalParas->renderBackend.associatedData()) != RenderBackend::Vulkan) {
    return;
  }
  auto* backendImpl = m_compositor->rendererBase().backend();
  if (!backendImpl) {
    return;
  }

  const uint32_t inFlight = backendImpl->inFlightCount();
  const uint32_t maxInFlight = backendImpl->maxFramesInFlight();
  if (maxInFlight == 0u) {
    // Backend does not expose an in-flight frame limit; treat as unconstrained.
    // (This should not happen for Vulkan, but keep the engine backend-agnostic.)
  } else if (inFlight >= maxInFlight) {
    return;
  }

  const QEvent::Type type = *m_vkDeferredRenderEventType;
  m_vkDeferredRenderEventType.reset();
  QCoreApplication::postEvent(this, new QEvent(type), Qt::LowEventPriority);
}

bool Z3DRenderingEngine::switchVulkanDeviceIndex(int index)
{
  if (!m_vkContext || !m_vkDevice) {
    LOG(ERROR) << "Vulkan not initialized; cannot switch device";
    return false;
  }
  // Ensure GPU idle
  try {
    m_vkContext->device().waitIdle();
  }
  catch (const std::exception& e) {
    LOG(WARNING) << "Vulkan waitIdle failed before device switch: " << e.what();
  }

  if (index < 0) {
    LOG(ERROR) << "Invalid Vulkan device index (<0)";
    return false;
  }

  const size_t idx = static_cast<size_t>(index);
  if (!m_vkContext->setSelectedDeviceIndex(idx)) {
    return false;
  }

  // Recreate wrapper and wire into scratch pool
  m_vkDevice.reset();
  try {
    m_vkDevice = m_vkContext->createDevice();
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Failed to recreate Vulkan device wrapper: " << e.what();
    return false;
  }
  if (m_scratchPool) {
    m_scratchPool->setVulkanDevice(m_vkDevice.get());
  }

  // Refresh GPU caps from selected device
  auto& phys = m_vkContext->physicalDevice();
  const auto props = phys.getProperties();
  const auto features = phys.getFeatures();
  const auto memProps = phys.getMemoryProperties();
  uint64_t vramBytes = 0;
  for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
    if (memProps.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
      vramBytes += memProps.memoryHeaps[i].size;
    }
  }
  Z3DGpuInfo::GenericCaps caps;
  caps.maxTextureSize = props.limits.maxImageDimension2D;
  caps.max3DTextureSize = props.limits.maxImageDimension3D;
  caps.maxArrayTextureLayers = static_cast<int>(props.limits.maxImageArrayLayers);
  caps.maxColorAttachments = static_cast<int>(props.limits.maxColorAttachments);
  caps.maxTextureAnisotropy = (features.samplerAnisotropy ? props.limits.maxSamplerAnisotropy : 1.0f);
  caps.dedicatedVideoMemoryMB = static_cast<uint64_t>(vramBytes / (1024ull * 1024ull));
  caps.maxViewportDim =
    static_cast<int>(std::min(props.limits.maxViewportDimensions[0], props.limits.maxViewportDimensions[1]));
  caps.maxCombinedTextureImageUnits = 48;
  caps.maxTextureImageUnits = 16;
  caps.maxVertexTextureImageUnits = 16;
  caps.maxGeometryTextureImageUnits = 16;
  caps.maxTextureBufferSize = static_cast<int>(props.limits.maxTexelBufferElements);
  caps.maxDrawBuffer = static_cast<int>(props.limits.maxColorAttachments);
  Z3DGpuInfo::instance().overrideGenericCaps(caps);

  m_vkContext->logGpuInfo();

  return true;
}

} // namespace nim
