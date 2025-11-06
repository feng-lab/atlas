#include "z3drenderingengine.h"

#include "z3dcanvas.h"
#include "z3dcompositor.h"
#include "z3dcameraparameter.h"
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
#include "z3drenderglobalstate.h"
#include "z3dscratchresourcepool.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkan.h"
#include "z3dshadermanager.h"
#include "z3dgpuinfo.h"
#include <glbinding/glbinding.h>
#include <glbinding-aux/Meta.h>
#include <QOffscreenSurface>
#include <QCoreApplication>
#include <QMetaObject>
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
    }
    // else if (auto aniDoc = qobject_cast<Z3DAnimationDoc*>(objDoc)) {
    //   auto aniView = new Z3DAnimationView(*aniDoc, *this);
    //   connect(aniView, &Z3DAnimationView::objViewReady, this, &Z3DRenderingEngine::objViewReady);
    //   m_3dObjViews.emplace_back(aniView);
    // } 
    else if (auto raDoc = qobject_cast<ZRegionAnnotationDoc*>(objDoc)) {
      auto aniView = new Z3DRegionAnnotationView(*raDoc, *this);
      connect(aniView, &Z3DRegionAnnotationView::objViewReady, this, &Z3DRenderingEngine::objViewReady);
      m_3dObjViews.emplace_back(aniView);
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
  return m_compositor->outputSize();
}

void Z3DRenderingEngine::setOutputSize(const glm::uvec2& size)
{
  getGLFocus();
  m_compositor->setOutputSize(size);
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

  if (id == 1) {
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
  // In Vulkan/headless paths there may be no GL context; treat as no-op.
  if (!m_context) {
    return;
  }
  m_context->makeCurrent();
}

void Z3DRenderingEngine::renderFast(bool stereo)
{
  if (Z3DRenderGlobalState::instance().hasCancellationSource()) {
    Z3DRenderGlobalState::instance().requestCancellation();
    LOG(INFO) << "cancel rendering, schedule a update later";
    QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
    return;
  }
  if (m_isRendering) {
    LOG(INFO) << "in fast rendering, schedule a update later";
    QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
    return;
  }

  VLOG(1) << "renderFast";
  Q_EMIT progressChanged(10);
  m_isRendering = true;
  auto renderingGuard = folly::makeGuard([this]() {
    m_isRendering = false;
  });
  getGLFocus();
  m_progress = m_networkEvaluator->process(stereo, true);
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
  getGLFocus();
  try {
    auto& cancellationSource = Z3DRenderGlobalState::instance().ensureCancellationSource();
    while (m_progress < 1.0) {
      m_progress = m_networkEvaluator->process(stereo, true, cancellationSource.getToken());
      Q_EMIT progressChanged(std::clamp<int>(m_progress * 100., 0, 100));
    }
  }
  catch (const ZCancellationException&) {
    LOG(INFO) << "cancelled, schedule a update later";
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

  getGLFocus();

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
        // VLOG(1) << globalCameraPara().get().left() << globalCameraPara().get().right() <<
        // globalCameraPara().get().top() << globalCameraPara().get().bottom();

        m_networkEvaluator->process(sst != Z3DScreenShotType::MonoView);

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
  getGLFocus();

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
  // VLOG(1) << globalCameraPara().get().nLeft() << globalCameraPara().get().nRight() <<
  // globalCameraPara().get().nTop() << globalCameraPara().get().nBottom();

  m_networkEvaluator->process(sst != Z3DScreenShotType::MonoView);

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

  getGLFocus();
  m_networkEvaluator->process(sst != Z3DScreenShotType::MonoView);

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
  VLOG(1) << "Switching compositor and connected filters to new backend";
  m_compositor->switchBackend(backend);

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
