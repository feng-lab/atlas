#include "z3dcanvas.h"

#include "zdoc.h"
#include "zimgdoc.h"
#include "zlog.h"
#include "z3drenderingengine.h"
#include "zneuroglancerprecomputed.h"
#include "zseedtrace.h"
#include "zswcdoc.h"
#include "zswcpack.h"
#include "zswctypedialog.h"
#include "zsysteminfo.h"
#include "ztracesettings.h"
#include "z3dmainwindow.h"
#include "zmainwindow.h"
#include "zview.h"
#include "zgraphicsview.h"
#include "z3dswcview.h"
#include "z3dswcfilter.h"
#include "z3dinteractionhandler.h"
#if defined(ATLAS_USE_OPENGLWIDGET)
#include "z3dscene.h"
#include "z3dopenglwidget.h"
#endif
#include <QCoreApplication>
#include <QAction>
#include <QDir>
#include <QMenu>
#include <QPointer>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

DECLARE_bool(atlas_vk_copy_yflip_in_shader);

namespace nim {

namespace {

[[nodiscard]] std::optional<glm::dmat3> tryExtractCoordLinearInvFromView3DJson(const json::object& view3dJson)
{
  const std::string coordKey = "Coord Transform 3DTransform";
  const auto it = view3dJson.find(coordKey);
  if (it == view3dJson.end() || !it->value().is_object()) {
    return {};
  }

  const json::object& transformObj = it->value().as_object();

  glm::dvec3 scale(1.0, 1.0, 1.0);
  if (const auto sIt = transformObj.find("Scale Vec3"); sIt != transformObj.end()) {
    scale = json::value_to<glm::dvec3>(sIt->value());
  }

  // Stored as axis-angle: (angle_degrees, axis_x, axis_y, axis_z)
  glm::dvec4 rot(0.0, 0.0, 1.0, 0.0);
  if (const auto rIt = transformObj.find("Rotation Vec4"); rIt != transformObj.end()) {
    rot = json::value_to<glm::dvec4>(rIt->value());
  }

  glm::dvec3 axis(rot.y, rot.z, rot.w);
  const double angleDeg = rot.x;
  glm::dmat3 rotMat(1.0);
  if (angleDeg != 0.0) {
    const double axisLen = glm::length(axis);
    if (axisLen > 0.0) {
      rotMat = glm::mat3_cast(glm::angleAxis(glm::radians(angleDeg), axis / axisLen));
    }
  }

  glm::dmat3 scaleMat(1.0);
  scaleMat[0][0] = scale.x;
  scaleMat[1][1] = scale.y;
  scaleMat[2][2] = scale.z;

  const glm::dmat3 linear = rotMat * scaleMat;
  return glm::inverse(linear);
}

[[nodiscard]] Z3DSwcFilter* findSwcFilterForObjId(Z3DRenderingEngine& engine, size_t swcObjId)
{
  for (const auto& viewPtr : engine.objViews()) {
    auto* swcView = dynamic_cast<Z3DSwcView*>(viewPtr.get());
    if (!swcView) {
      continue;
    }

    auto& idToFilter = swcView->idToFilter();
    auto it = idToFilter.find(swcObjId);
    if (it == idToFilter.end()) {
      continue;
    }
    return it->second.get();
  }
  return nullptr;
}

void setSwcFilterInteractionMode(Z3DRenderingEngine& engine, size_t swcObjId, Z3DSwcFilter::InteractionMode mode)
{
  Z3DSwcFilter* filter = findSwcFilterForObjId(engine, swcObjId);
  if (!filter) {
    return;
  }
  filter->setInteractionMode(mode);
}

} // namespace

Z3DCanvas::Z3DCanvas(const QString& title, int width, int height, QWidget* parent, Qt::WindowFlags f)
  : QGraphicsView(parent)
{
  setAlignment(Qt::AlignLeft | Qt::AlignTop);
  resize(width, height);

#if defined(ATLAS_USE_OPENGLWIDGET)
  m_glWidget = new ZOpenGLWidget(this, f);
  m_3dScene = std::make_unique<Z3DScene>(width, height, m_glWidget->format().stereo(), *this);
  setViewport(m_glWidget);
  setScene(m_3dScene.get());
#else
  Q_UNUSED(f)
  m_scene = std::make_unique<QGraphicsScene>(0, 0, width, height);
  m_pixmapItem = new QGraphicsPixmapItem();
  // m_pixmapItem->setTransformationMode(Qt::SmoothTransformation);
  m_scene->addItem(m_pixmapItem);
  setScene(m_scene.get());
#endif

  setViewportUpdateMode(FullViewportUpdate);

  setWindowTitle(title);
  setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
  setOptimizationFlags(QGraphicsView::DontSavePainterState | QGraphicsView::DontAdjustForAntialiasing);

  setAcceptDrops(true);
  setFocusPolicy(Qt::StrongFocus);

#if !defined(__APPLE__) && !defined(_WIN32)
  setStyleSheet("border-style: none;");
#endif

  m_rotateXShortCut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_X), this);
  connect(m_rotateXShortCut, &QShortcut::activated, this, &Z3DCanvas::rotateX);
  m_rotateYShortCut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Y), this);
  connect(m_rotateYShortCut, &QShortcut::activated, this, &Z3DCanvas::rotateY);
  m_rotateZShortCut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Z), this);
  connect(m_rotateZShortCut, &QShortcut::activated, this, &Z3DCanvas::rotateZ);
  m_rotateXMShortCut = new QShortcut(QKeySequence(QKeyCombination(Qt::ALT | Qt::SHIFT, Qt::Key_X)), this);
  connect(m_rotateXMShortCut, &QShortcut::activated, this, &Z3DCanvas::rotateXM);
  m_rotateYMShortCut = new QShortcut(QKeySequence(QKeyCombination(Qt::ALT | Qt::SHIFT, Qt::Key_Y)), this);
  connect(m_rotateYMShortCut, &QShortcut::activated, this, &Z3DCanvas::rotateYM);
  m_rotateZMShortCut = new QShortcut(QKeySequence(QKeyCombination(Qt::ALT | Qt::SHIFT, Qt::Key_Z)), this);
  connect(m_rotateZMShortCut, &QShortcut::activated, this, &Z3DCanvas::rotateZM);

#if defined(ATLAS_USE_OPENGLWIDGET)
  connect(m_glWidget, &ZOpenGLWidget::openGLContextInitialized, this, &Z3DCanvas::openGLContextInitialized);
  connect(m_glWidget, &ZOpenGLWidget::openGLContextInitialized, m_3dScene.get(), &Z3DScene::initPainter);
#endif
}

#ifdef ATLAS_USE_OPENGLWIDGET
Z3DCanvas::~Z3DCanvas()
{
  VLOG(1) << "in canvas destructor";
  getGLFocus();
}

QOpenGLContext* Z3DCanvas::context() const
{
  return m_glWidget->context();
}

void Z3DCanvas::getGLFocus()
{
  m_glWidget->makeCurrent();
}
#endif

void Z3DCanvas::setRenderingEngine(Z3DRenderingEngine* engine)
{
  m_engine = engine;
#if defined(ATLAS_USE_OPENGLWIDGET)
  m_3dScene->setRenderingEngine(engine);
#endif
  sceneParaUpdated();
}

void Z3DCanvas::toggleFullScreen()
{
  if (m_fullscreen) {
    m_fullscreen = false;
    showNormal();
  } else {
    showFullScreen();
    m_fullscreen = !m_fullscreen;
  }
}

void Z3DCanvas::sceneParaUpdated()
{
  VLOG(1) << "sceneParaUpdated";
  if (m_engine) {
    QCoreApplication::postEvent(m_engine, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
  }
}

void Z3DCanvas::renderingFinished()
{
  // Engine may have been detached/destroyed while a queued signal is in flight
  if (m_engine && m_engine->hasNewRenderingFlag()) {
    VLOG(1) << "update";

#if defined(ATLAS_USE_OPENGLWIDGET)
    m_glWidget->update();
#else
    const std::scoped_lock lock(m_engine->targetSwitchMutex());
    auto localBuffer = m_engine->monoReadyLocalBuffer();
    QImage image;
    const bool vulkanZeroCopy = localBuffer->external && localBuffer->width > 0 && localBuffer->height > 0;
    if (vulkanZeroCopy) {
      // Vulkan zero-copy path: mapped RGBA8 staging
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
      image =
        QImage(localBuffer->external,
               static_cast<int>(localBuffer->width),
               static_cast<int>(localBuffer->height),
               static_cast<int>(localBuffer->externalStride ? localBuffer->externalStride : localBuffer->width * 4),
               QImage::Format_RGBA8888_Premultiplied);
#else
      image =
        QImage(localBuffer->external,
               static_cast<int>(localBuffer->width),
               static_cast<int>(localBuffer->height),
               static_cast<int>(localBuffer->externalStride ? localBuffer->externalStride : localBuffer->width * 4),
               QImage::Format_RGB32); // Fallback; older Qt may lack RGBA8888
#endif
    } else {
      image = QImage(localBuffer->data.data(),
                     static_cast<int>(localBuffer->width),
                     static_cast<int>(localBuffer->height),
                     QImage::Format_ARGB32_Premultiplied);
    }
    // If shader y-flip is enabled for Vulkan path, present directly; otherwise flip vertically.
    auto pixmap =
      QPixmap::fromImage((vulkanZeroCopy && FLAGS_atlas_vk_copy_yflip_in_shader) ? image : image.flipped(Qt::Vertical));
    pixmap.setDevicePixelRatio(devicePixelRatio());
    m_pixmapItem->setPixmap(pixmap);
    if (m_engine) {
      m_engine->clearNewRenderingFlag();
    }
    VLOG(1) << localBuffer << " " << localBuffer->width << " " << localBuffer->height;
#endif
  }
}

void Z3DCanvas::contextMenuEvent(QContextMenuEvent* e)
{
  // neuTube parity: in interactive connect-to mode, right click exits the mode (and does not show menus).
  if (m_connectTo3dSwcModeActive) {
    m_connectTo3dSwcModeActive = false;
    const std::optional<size_t> swcObjIdOpt = m_connectTo3dSwcObjId;
    m_connectTo3dSwcObjId.reset();

    if (m_engine && swcObjIdOpt.has_value()) {
      m_engine->cancelActiveRender();
      const size_t swcObjId = *swcObjIdOpt;
      QMetaObject::invokeMethod(
        m_engine,
        [enginePtr = QPointer<Z3DRenderingEngine>(m_engine), swcObjId]() {
          if (!enginePtr) {
            return;
          }
          setSwcFilterInteractionMode(*enginePtr, swcObjId, Z3DSwcFilter::InteractionMode::Select);
        },
        Qt::QueuedConnection);
    }

    e->accept();
    return;
  }

  if (m_engine) {
    m_engine->cancelActiveRender();
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::showSeedTraceContextMenu(QPoint globalPos, size_t imgObjId, size_t sc, float x, float y, float z)
{
  if (m_doc == nullptr) {
    return;
  }

  const ZTraceSettings& settings = m_doc->traceSettings();
  if (!settings.traceToolEnabled()) {
    return;
  }

  if (settings.traceInProgress()) {
    return;
  }

  // neuTube parity: do not show the trace menu while SWC node edit modes are active.
  if ((m_toggle3dAddNeuronNodeAction && m_toggle3dAddNeuronNodeAction->isChecked()) ||
      (m_toggle3dMoveSelectedAction && m_toggle3dMoveSelectedAction->isChecked()) ||
      (m_toggle3dExtendAction && m_toggle3dExtendAction->isChecked()) || m_connectTo3dSwcModeActive) {
    return;
  }

  const std::optional<size_t> storedImgId = settings.sourceImageId();
  if (!storedImgId.has_value() || *storedImgId != imgObjId) {
    return;
  }

  ZImgDoc& imgDoc = m_doc->imgDoc();
  if (!imgDoc.hasObjWithID(imgObjId)) {
    return;
  }

  std::shared_ptr<ZImgPack> imgPack = imgDoc.imgPackShared(imgObjId);
  if (!imgPack) {
    return;
  }

  // Skip segmentation datasets (not traceable).
  if (imgPack->isNeuroglancerPrecomputed()) {
    const std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol = imgPack->neuroglancerVolumeShared();
    if (vol && vol->isSegmentation()) {
      return;
    }
  }

  const ZImgInfo info = imgPack->imgInfo();
  if (settings.sourceChannel() != sc) {
    return;
  }
  if (sc >= info.numChannels) {
    return;
  }

  std::optional<std::pair<size_t, ZSwc>> hostSwcOpt;
  QString actionName = QStringLiteral("Trace");

  if (settings.swcTargetMode() == ZTraceSettings::SwcTargetMode::ExistingSwc) {
    const std::optional<size_t> storedSwcId = settings.targetSwcId();
    if (!storedSwcId.has_value()) {
      return;
    }

    ZSwcDoc& swcDoc = m_doc->swcDoc();
    if (!swcDoc.hasObjWithID(*storedSwcId)) {
      return;
    }

    hostSwcOpt = std::make_optional<std::pair<size_t, ZSwc>>(*storedSwcId, swcDoc.swcPack(*storedSwcId).swc());
    actionName = QStringLiteral("Trace (attach)");
  }

  if (settings.swcTargetMode() != ZTraceSettings::SwcTargetMode::NewSwc && !hostSwcOpt.has_value()) {
    return;
  }

  const bool promoteNewSwcToExistingTarget = (settings.swcTargetMode() == ZTraceSettings::SwcTargetMode::NewSwc);
  const std::array<double, 3> seed = {static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)};
  const QString traceCfgPath = QDir(ZSystemInfo::jsonDirPath()).absoluteFilePath("trace_config.json");

  QMenu traceMenu(this);
  QAction* traceAct = traceMenu.addAction(QStringLiteral("Trace"));
  connect(traceAct,
          &QAction::triggered,
          this,
          [docPtr = m_doc,
           actionName,
           imgObjId,
           imgPack,
           sc,
           seed,
           traceCfgPath,
           promoteNewSwcToExistingTarget,
           hostSwcOpt = std::move(hostSwcOpt),
           enginePtr = QPointer<Z3DRenderingEngine>(m_engine),
           swcDocTypeName = (m_doc != nullptr) ? m_doc->swcDoc().typeName() : QString()]() mutable {
            if (docPtr == nullptr) {
              return;
            }
            std::function<void(size_t)> onNewSwcCreated = [enginePtr, imgObjId, swcDocTypeName](size_t newSwcId) {
              if (!enginePtr) {
                return;
              }

              QMetaObject::invokeMethod(
                enginePtr,
                [enginePtr, imgObjId, newSwcId, swcDocTypeName]() {
                  if (!enginePtr) {
                    return;
                  }

                  json::object srcViewJson;
                  enginePtr->write(imgObjId, srcViewJson);
                  const std::string coordKey = "Coord Transform 3DTransform";
                  const auto it = srcViewJson.find(coordKey);
                  if (it == srcViewJson.end()) {
                    return;
                  }

                  json::object dstViewJson;
                  dstViewJson["ViewObjType"] = json::value_from(swcDocTypeName);
                  dstViewJson["ViewVersion"] = 1.0;
                  dstViewJson[coordKey] = it->value();
                  dstViewJson["Rendering Mode StringIntOption"] = "Sphere";
                  enginePtr->applyView3DForId(newSwcId, dstViewJson);
                },
                Qt::QueuedConnection);
            };
            startSeedTraceInteractive(*docPtr,
                                      actionName,
                                      imgObjId,
                                      imgPack,
                                      sc,
                                      /*t=*/0,
                                      seed,
                                      traceCfgPath,
                                      std::move(hostSwcOpt),
                                      promoteNewSwcToExistingTarget,
                                      std::move(onNewSwcCreated));
          });
  traceMenu.exec(globalPos);
}

void Z3DCanvas::ensure3dSwcNodeActions()
{
  if (m_toggle3dExtendAction != nullptr) {
    return;
  }

  m_toggle3dExtendAction = new QAction(tr("Extend"), this);
  m_toggle3dExtendAction->setCheckable(true);
  m_toggle3dExtendAction->setShortcut(Qt::Key_Space);
  connect(m_toggle3dExtendAction, &QAction::toggled, this, &Z3DCanvas::toggle3dSwcExtendMode);

  m_connectTo3dSwcNodeAction = new QAction(tr("Connect to"), this);
  m_connectTo3dSwcNodeAction->setShortcut(Qt::Key_C);
  connect(m_connectTo3dSwcNodeAction, &QAction::triggered, this, &Z3DCanvas::start3dSwcConnectToMode);

  m_toggle3dMoveSelectedAction = new QAction(tr("Move Selected (Shift+Mouse)"), this);
  m_toggle3dMoveSelectedAction->setCheckable(true);
  m_toggle3dMoveSelectedAction->setShortcut(Qt::Key_V);
  connect(m_toggle3dMoveSelectedAction, &QAction::toggled, this, &Z3DCanvas::toggle3dSwcMoveSelectedMode);

  m_locate3dNodesIn2DAction = new QAction(tr("Locate node(s) in 2D"), this);
  connect(m_locate3dNodesIn2DAction, &QAction::triggered, this, &Z3DCanvas::locate3dSwcNodesIn2D);

  m_change3dSwcNodeTypeAction = new QAction(tr("Change type"), this);
  connect(m_change3dSwcNodeTypeAction, &QAction::triggered, this, &Z3DCanvas::change3dSwcNodeType);

  m_toggle3dAddNeuronNodeAction = new QAction(tr("Add neuron node"), this);
  m_toggle3dAddNeuronNodeAction->setCheckable(true);
  connect(m_toggle3dAddNeuronNodeAction, &QAction::toggled, this, &Z3DCanvas::toggle3dAddNeuronNodeMode);
}

void Z3DCanvas::setActive3dSwcPackForEditing(ZSwcPack* swcPack, int64_t clickedNodeId)
{
  if (swcPack == nullptr) {
    m_active3dSwcPack = nullptr;
    m_active3dClickedNodeId = -1;
    return;
  }

  m_active3dSwcPack = swcPack;
  m_active3dClickedNodeId = clickedNodeId;
}

void Z3DCanvas::update3dSwcNodeActionEnabledState()
{
  const bool havePack = (m_active3dSwcPack != nullptr);
  const bool haveSelection = havePack && !m_active3dSwcPack->selectedNodes().empty();
  const bool haveSingleNode = havePack && (m_active3dSwcPack->selectedNodes().size() == 1);

  if (m_toggle3dExtendAction) {
    m_toggle3dExtendAction->setEnabled(haveSingleNode);
  }
  if (m_connectTo3dSwcNodeAction) {
    m_connectTo3dSwcNodeAction->setEnabled(haveSingleNode);
  }
  if (m_toggle3dMoveSelectedAction) {
    m_toggle3dMoveSelectedAction->setEnabled(haveSelection);
  }
  if (m_locate3dNodesIn2DAction) {
    m_locate3dNodesIn2DAction->setEnabled(haveSelection);
  }
  if (m_change3dSwcNodeTypeAction) {
    m_change3dSwcNodeTypeAction->setEnabled(haveSelection);
  }
  if (m_toggle3dAddNeuronNodeAction) {
    m_toggle3dAddNeuronNodeAction->setEnabled(havePack);
  }
}

void Z3DCanvas::toggle3dSwcExtendMode(bool)
{
  if (m_active3dSwcPack == nullptr) {
    if (m_toggle3dExtendAction) {
      const QSignalBlocker blocker(*m_toggle3dExtendAction);
      m_toggle3dExtendAction->setChecked(false);
    }
    return;
  }

  const bool on = m_toggle3dExtendAction && m_toggle3dExtendAction->isChecked();
  if (on && m_toggle3dAddNeuronNodeAction && m_toggle3dAddNeuronNodeAction->isChecked()) {
    const QSignalBlocker blocker(*m_toggle3dAddNeuronNodeAction);
    m_toggle3dAddNeuronNodeAction->setChecked(false);
  }

  if (m_engine == nullptr) {
    return;
  }

  const ZTraceSettings& settings = m_doc->traceSettings();
  const std::optional<size_t> imgIdOpt = settings.sourceImageId();
  const bool haveStackData = imgIdOpt.has_value() && m_doc->imgDoc().hasObjWithID(*imgIdOpt);

  const Z3DSwcFilter::InteractionMode mode = on ? (haveStackData ? Z3DSwcFilter::InteractionMode::SmartExtendSwcNode
                                                                 : Z3DSwcFilter::InteractionMode::PlainExtendSwcNode)
                                                : Z3DSwcFilter::InteractionMode::Select;

  const size_t swcObjId = m_active3dSwcPack->id();
  m_engine->cancelActiveRender();
  QMetaObject::invokeMethod(
    m_engine,
    [enginePtr = QPointer<Z3DRenderingEngine>(m_engine), swcObjId, mode]() {
      if (!enginePtr) {
        return;
      }
      setSwcFilterInteractionMode(*enginePtr, swcObjId, mode);
    },
    Qt::QueuedConnection);
}

void Z3DCanvas::start3dSwcConnectToMode()
{
  if (m_active3dSwcPack == nullptr || m_engine == nullptr) {
    return;
  }
  if (m_active3dSwcPack->selectedNodes().size() != 1) {
    return;
  }

  m_connectTo3dSwcModeActive = true;
  m_connectTo3dSwcObjId = m_active3dSwcPack->id();

  const size_t swcObjId = m_active3dSwcPack->id();
  m_engine->cancelActiveRender();
  QMetaObject::invokeMethod(
    m_engine,
    [enginePtr = QPointer<Z3DRenderingEngine>(m_engine), swcObjId]() {
      if (!enginePtr) {
        return;
      }
      setSwcFilterInteractionMode(*enginePtr, swcObjId, Z3DSwcFilter::InteractionMode::ConnectSwcNode);
    },
    Qt::QueuedConnection);
}

void Z3DCanvas::toggle3dSwcMoveSelectedMode(bool)
{
  const bool on = m_toggle3dMoveSelectedAction && m_toggle3dMoveSelectedAction->isChecked();

  m_active3dSwcLinearInv.reset();
  if (m_engine == nullptr) {
    return;
  }

  m_engine->cancelActiveRender();
  if (on && m_active3dSwcPack != nullptr) {
    json::object viewJson;
    const size_t swcObjId = m_active3dSwcPack->id();

    QMetaObject::invokeMethod(
      m_engine,
      [&viewJson, enginePtr = QPointer<Z3DRenderingEngine>(m_engine), swcObjId]() {
        if (!enginePtr) {
          return;
        }
        enginePtr->write(swcObjId, viewJson);
      },
      Qt::BlockingQueuedConnection);

    m_active3dSwcLinearInv = tryExtractCoordLinearInvFromView3DJson(viewJson);
  }

  QMetaObject::invokeMethod(
    m_engine,
    [enginePtr = QPointer<Z3DRenderingEngine>(m_engine), on]() {
      if (!enginePtr) {
        return;
      }
      enginePtr->globalParas().interactionHandler.setMoveObjects(on);
    },
    Qt::QueuedConnection);
}

void Z3DCanvas::locate3dSwcNodesIn2D()
{
  if (m_active3dSwcPack == nullptr) {
    return;
  }
  if (m_active3dSwcPack->isLocked()) {
    return;
  }
  if (m_active3dSwcPack->selectedNodes().empty()) {
    return;
  }

  double minX = std::numeric_limits<double>::infinity();
  double minY = std::numeric_limits<double>::infinity();
  double minZ = std::numeric_limits<double>::infinity();
  double maxX = -std::numeric_limits<double>::infinity();
  double maxY = -std::numeric_limits<double>::infinity();
  double maxZ = -std::numeric_limits<double>::infinity();

  for (const auto& n : m_active3dSwcPack->selectedNodes()) {
    minX = std::min(minX, n->x);
    minY = std::min(minY, n->y);
    minZ = std::min(minZ, n->z);
    maxX = std::max(maxX, n->x);
    maxY = std::max(maxY, n->y);
    maxZ = std::max(maxZ, n->z);
  }

  const double cx = (minX + maxX) * 0.5;
  const double cy = (minY + maxY) * 0.5;
  const double cz = (minZ + maxZ) * 0.5;

  double width = std::max(maxX - minX, maxY - minY);
  width = width + 1.0;
  const double kMinWidth = 800.0;
  if (width < kMinWidth) {
    width = kMinWidth;
  }

  auto* win3d = qobject_cast<Z3DMainWindow*>(window());
  if (win3d == nullptr) {
    return;
  }

  ZMainWindow& win2d = win3d->window2d();
  win2d.raise();
  win2d.activateWindow();

  ZView* view = win2d.view();
  if (view == nullptr) {
    return;
  }

  view->slicePara().set(static_cast<int>(std::llround(cz)));
  view->graphicsView().fitRect(QRectF(cx - width * 0.5, cy - width * 0.5, width, width));
}

void Z3DCanvas::change3dSwcNodeType()
{
  if (m_active3dSwcPack == nullptr) {
    return;
  }
  if (m_active3dSwcPack->isLocked()) {
    return;
  }
  if (m_active3dSwcPack->selectedNodes().empty()) {
    return;
  }

  ZSwcTypeDialog dlg(ZSwcTypeDialog::SelectionMode::SwcNode, this);
  if (dlg.exec() != QDialog::Accepted) {
    return;
  }

  // Apply semantics match neuTube `Z3DWindow::changeSelectedSwcNodeType()`:
  // - Individual: selected nodes
  // - Downstream: subtree of each selected node
  // - Connection: upstream until common ancestor across the selection
  // - Longest leaf: path from the first selected node to its furthest leaf (geodesic)
  const int newType = dlg.type();

  std::vector<int64_t> selectedIds;
  selectedIds.reserve(m_active3dSwcPack->selectedNodes().size());
  for (const auto& n : m_active3dSwcPack->selectedNodes()) {
    selectedIds.push_back(n->id);
  }

  ZSwc newSwc = m_active3dSwcPack->swc();
  bool anyChange = false;

  std::unordered_map<int64_t, ZSwc::SwcTreeNode> idToNode;
  idToNode.reserve(newSwc.size());
  for (auto it = newSwc.begin(); it != newSwc.end(); ++it) {
    idToNode[it->id] = it;
  }

  const auto setTypeIfDiff = [&](const ZSwc::SwcTreeNode& n) {
    if (ZSwc::isNull(n)) {
      return;
    }
    if (n->type != newType) {
      n->type = newType;
      anyChange = true;
    }
  };

  switch (dlg.pickingMode()) {
    case ZSwcTypeDialog::PickingMode::Individual: {
      for (const int64_t id : selectedIds) {
        const auto it = idToNode.find(id);
        if (it != idToNode.end()) {
          setTypeIfDiff(it->second);
        }
      }
      break;
    }
    case ZSwcTypeDialog::PickingMode::Downstream: {
      for (const int64_t id : selectedIds) {
        const auto rootIt = idToNode.find(id);
        if (rootIt == idToNode.end()) {
          continue;
        }
        for (auto it = newSwc.begin(rootIt->second); it != newSwc.end(rootIt->second); ++it) {
          setTypeIfDiff(it);
        }
      }
      break;
    }
    case ZSwcTypeDialog::PickingMode::Connection: {
      if (selectedIds.empty()) {
        break;
      }

      // Find common ancestor across the entire selection (neuTube: SwcTreeNode::commonAncestor).
      ZSwc::SwcTreeNode ancestor;
      {
        const auto firstIt = idToNode.find(selectedIds.front());
        if (firstIt == idToNode.end()) {
          break;
        }
        ancestor = firstIt->second;
      }

      for (size_t i = 1; i < selectedIds.size(); ++i) {
        const auto it = idToNode.find(selectedIds[i]);
        if (it == idToNode.end()) {
          ancestor = ZSwc::SwcTreeNode{};
          break;
        }
        ancestor = newSwc.lowestCommonAncestor(ancestor, it->second);
        if (ZSwc::isNull(ancestor)) {
          break;
        }
      }

      if (ZSwc::isNull(ancestor)) {
        break;
      }

      for (const int64_t id : selectedIds) {
        const auto it = idToNode.find(id);
        if (it == idToNode.end()) {
          continue;
        }
        for (auto up = newSwc.beginAncestor(it->second); up != newSwc.endAncestor(it->second); ++up) {
          setTypeIfDiff(up);
          if (up == ancestor) {
            break;
          }
        }
      }
      break;
    }
    case ZSwcTypeDialog::PickingMode::LongestLeaf: {
      if (selectedIds.empty()) {
        break;
      }

      // neuTube uses only the first selected node for LONGEST_LEAF.
      const auto startIt = idToNode.find(selectedIds.front());
      if (startIt == idToNode.end()) {
        break;
      }
      const ZSwc::SwcTreeNode start = startIt->second;

      // Find furthest downstream node by geodesic distance.
      double bestDist = -1.0;
      ZSwc::SwcTreeNode bestNode;
      for (auto it = newSwc.begin(start); it != newSwc.end(start); ++it) {
        double dist = 0.0;
        auto cur = it;
        while (!ZSwc::isNull(cur) && cur != start) {
          const auto par = ZSwc::parent(cur);
          if (ZSwc::isNull(par)) {
            break;
          }
          dist += glm::length(glm::dvec3(cur->x - par->x, cur->y - par->y, cur->z - par->z));
          cur = par;
        }
        if (dist > bestDist) {
          bestDist = dist;
          bestNode = it;
        }
      }

      if (!ZSwc::isNull(bestNode)) {
        // Mark the path start -> bestNode.
        auto cur = bestNode;
        while (!ZSwc::isNull(cur)) {
          setTypeIfDiff(cur);
          if (cur == start) {
            break;
          }
          cur = ZSwc::parent(cur);
        }
      }
      break;
    }
    default:
      break;
  }

  if (!anyChange) {
    return;
  }

  m_active3dSwcPack->replaceSwcWithUndo(QStringLiteral("Change SWC Node Type"), std::move(newSwc));

  std::set<ZSwc::SwcTreeNode> restored;
  for (const int64_t id : selectedIds) {
    const ZSwc::SwcTreeNode it = m_active3dSwcPack->findNodeByIdOrNull(id);
    if (!ZSwc::isNull(it)) {
      restored.insert(it);
    }
  }
  if (!restored.empty()) {
    m_active3dSwcPack->setSelectedNodes(restored);
  }
}

void Z3DCanvas::toggle3dAddNeuronNodeMode(bool)
{
  if (m_active3dSwcPack == nullptr) {
    if (m_toggle3dAddNeuronNodeAction) {
      const QSignalBlocker blocker(*m_toggle3dAddNeuronNodeAction);
      m_toggle3dAddNeuronNodeAction->setChecked(false);
    }
    return;
  }

  const bool on = m_toggle3dAddNeuronNodeAction && m_toggle3dAddNeuronNodeAction->isChecked();
  if (on && m_toggle3dExtendAction && m_toggle3dExtendAction->isChecked()) {
    const QSignalBlocker blocker(*m_toggle3dExtendAction);
    m_toggle3dExtendAction->setChecked(false);
  }

  if (m_engine == nullptr) {
    return;
  }
  const size_t swcObjId = m_active3dSwcPack->id();
  const Z3DSwcFilter::InteractionMode mode =
    on ? Z3DSwcFilter::InteractionMode::AddSwcNode : Z3DSwcFilter::InteractionMode::Select;

  m_engine->cancelActiveRender();
  QMetaObject::invokeMethod(
    m_engine,
    [enginePtr = QPointer<Z3DRenderingEngine>(m_engine), swcObjId, mode]() {
      if (!enginePtr) {
        return;
      }
      setSwcFilterInteractionMode(*enginePtr, swcObjId, mode);
    },
    Qt::QueuedConnection);
}

namespace {

void appendClonedMenuActions3dSwcDocParity(QMenu& dst, const QMenu& src)
{
  for (QAction* action : src.actions()) {
    if (action == nullptr) {
      continue;
    }
    if (action->isSeparator()) {
      dst.addSeparator();
      continue;
    }

    if (const QMenu* sub = action->menu()) {
      const QString title =
        (sub->title() == QStringLiteral("Interpolate")) ? QStringLiteral("Intepolate") : sub->title();
      auto* newSub = dst.addMenu(title);
      appendClonedMenuActions3dSwcDocParity(*newSub, *sub);
      continue;
    }

    dst.addAction(action);
  }
}

} // namespace

void Z3DCanvas::showSwcNodeContextMenu(QPoint globalPos, ZSwcPack* swcPack, int64_t clickedNodeId)
{
  if (swcPack == nullptr || swcPack->isLocked()) {
    return;
  }
  if (m_doc == nullptr) {
    return;
  }

  ensure3dSwcNodeActions();
  setActive3dSwcPackForEditing(swcPack, clickedNodeId);

  // neuTube selection semantics: right-clicking an unselected node should make it the active selection
  // so actions apply to that node.
  const ZSwc::SwcTreeNode clickedNodeIt = swcPack->findNodeByIdOrNull(clickedNodeId);
  const bool clickedNodeFound = !ZSwc::isNull(clickedNodeIt);
  const bool clickedNodeIsSelected = clickedNodeFound && swcPack->selectedNodes().contains(clickedNodeIt);

  if (clickedNodeFound && !clickedNodeIsSelected) {
    std::set<ZSwc::SwcTreeNode> selection;
    selection.insert(clickedNodeIt);
    swcPack->setSelectedNodes(selection);
  }

  update3dSwcNodeActionEnabledState();

  auto* menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose);

  menu->addAction(m_toggle3dExtendAction);
  menu->addAction(m_connectTo3dSwcNodeAction);
  menu->addAction(m_toggle3dMoveSelectedAction);

  appendClonedMenuActions3dSwcDocParity(*menu, swcPack->contextMenu());

  menu->addSeparator();
  menu->addAction(m_locate3dNodesIn2DAction);
  menu->addAction(m_change3dSwcNodeTypeAction);
  menu->addAction(m_toggle3dAddNeuronNodeAction);

  menu->popup(globalPos);
}

void Z3DCanvas::request3dSwcAddNeuronNode(ZSwcPack* swcPack, double x, double y, double z, double r)
{
  if (swcPack == nullptr || swcPack->isLocked()) {
    return;
  }
  swcPack->addIsolatedNodeLegacyLike(glm::dvec3(x, y, z), r);
}

void Z3DCanvas::request3dSwcPlainExtend(ZSwcPack* swcPack, double x, double y, double z, double r)
{
  if (swcPack == nullptr || swcPack->isLocked()) {
    return;
  }
  swcPack->extendSelectedNodePlain(glm::dvec3(x, y, z), r);
}

void Z3DCanvas::request3dSwcConnectToTarget(ZSwcPack* swcPack, int64_t targetNodeId)
{
  if (swcPack == nullptr || swcPack->isLocked()) {
    return;
  }

  if (!m_connectTo3dSwcModeActive) {
    return;
  }

  const ZSwc::SwcTreeNode target = swcPack->findNodeByIdOrNull(targetNodeId);
  if (!ZSwc::isNull(target)) {
    const bool connected = swcPack->connectSelectedNodeToLegacyLike(target);
    (void)connected;
  }

  m_connectTo3dSwcModeActive = false;
  m_connectTo3dSwcObjId.reset();
  if (m_engine) {
    const size_t swcObjId = swcPack->id();
    QMetaObject::invokeMethod(
      m_engine,
      [enginePtr = QPointer<Z3DRenderingEngine>(m_engine), swcObjId]() {
        if (!enginePtr) {
          return;
        }
        setSwcFilterInteractionMode(*enginePtr, swcObjId, Z3DSwcFilter::InteractionMode::Select);
      },
      Qt::QueuedConnection);
  }
}

void Z3DCanvas::on3dObjectsMoved(double x, double y, double z)
{
  if (!m_toggle3dMoveSelectedAction || !m_toggle3dMoveSelectedAction->isChecked()) {
    return;
  }
  if (m_active3dSwcPack == nullptr || m_active3dSwcPack->isLocked()) {
    return;
  }
  if (m_active3dSwcPack->selectedNodes().empty()) {
    return;
  }

  glm::dvec3 delta(x, y, z);
  if (m_active3dSwcLinearInv.has_value()) {
    delta = (*m_active3dSwcLinearInv) * delta;
  }
  m_active3dSwcPack->translateSelectedNodesLegacyLike(delta.x, delta.y, delta.z);
}

void Z3DCanvas::pointInVolumeLeftClicked(QPoint,
                                         size_t imgObjId,
                                         size_t sc,
                                         float x,
                                         float y,
                                         float z,
                                         Qt::KeyboardModifiers modifiers)
{
  if (!m_toggle3dExtendAction || !m_toggle3dExtendAction->isChecked()) {
    return;
  }
  if (m_doc == nullptr) {
    return;
  }

  const ZTraceSettings& settings = m_doc->traceSettings();
  const std::optional<size_t> srcImgId = settings.sourceImageId();
  if (!srcImgId.has_value() || *srcImgId != imgObjId) {
    return;
  }
  if (settings.sourceChannel() != sc) {
    return;
  }

  // neuTube parity: extend is enabled when there is exactly one SWC node selected globally.
  // In Atlas we may have multiple SWC objects, so resolve the selected node by scanning all SWC packs.
  ZSwcPack* pack = nullptr;
  ZSwc::SwcTreeNode parentNode;
  size_t numSelected = 0;
  for (const size_t swcId : m_doc->swcDoc().objs()) {
    ZSwcPack& sp = m_doc->swcDoc().swcPack(swcId);
    numSelected += sp.selectedNodes().size();
    if (sp.selectedNodes().size() == 1) {
      pack = &sp;
      parentNode = *sp.selectedNodes().begin();
    }
    if (numSelected > 1) {
      break;
    }
  }
  if (numSelected != 1 || pack == nullptr || ZSwc::isNull(parentNode)) {
    return;
  }
  if (pack->isLocked()) {
    return;
  }

  const glm::dvec3 center(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z));
  const double radius = parentNode->radius;

  if (modifiers == Qt::ControlModifier) {
    pack->extendSelectedNodePlain(center, radius);
  } else {
    pack->extendSelectedNodeSmartLegacyLike(center, radius, /*t=*/0);
  }
}

// void Z3DCanvas::enterEvent(QEnterEvent* e)
//{
//   if (m_engine) {
//     QCoreApplication::postEvent(m_engine, e->clone());
//   }
// }
//
// void Z3DCanvas::leaveEvent(QEvent* e)
//{
//   if (m_engine) {
//     QCoreApplication::postEvent(m_engine, e->clone());
//   }
// }

void Z3DCanvas::mousePressEvent(QMouseEvent* e)
{
  if (m_engine) {
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::mouseReleaseEvent(QMouseEvent* e)
{
  if (m_engine) {
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::mouseMoveEvent(QMouseEvent* e)
{
  // VLOG(1) << "mousemoveevent";
  if (m_engine) {
    m_engine->cancelLongRendering();
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::mouseDoubleClickEvent(QMouseEvent* e)
{
  if (m_engine) {
    m_engine->cancelActiveRender();
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::wheelEvent(QWheelEvent* e)
{
  // VLOG(1) << "wheelevent";
  if (m_engine && e->angleDelta().y() != 0) {
    m_engine->cancelLongRendering();
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::keyPressEvent(QKeyEvent* e)
{
  QGraphicsView::keyPressEvent(e);
  if (m_engine) {
    if (Z3DTrackballInteractionHandler::isTrackballNavigationKeyPress(*e)) {
      m_engine->cancelLongRendering();
    }
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::keyReleaseEvent(QKeyEvent* e)
{
  if (m_engine) {
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::resizeEvent(QResizeEvent* event)
{
  QGraphicsView::resizeEvent(event);
#if defined(ATLAS_USE_OPENGLWIDGET)
  if (m_3dScene) {
    m_3dScene->setSceneRect(QRect(QPoint(0, 0), event->size()));
  }
#else
  CHECK(m_scene);
  m_scene->setSceneRect(QRect(QPoint(0, 0), event->size()));
#endif

  // VLOG(1) << devicePixelRatio() << " " << event->size() << " " << logicalDpiX() << " " << physicalDpiX();
  if (m_engine) {
    m_engine->cancelLongRendering();
  }
  Q_EMIT canvasSizeChanged(event->size().width() * devicePixelRatio(), event->size().height() * devicePixelRatio());
}

void Z3DCanvas::dragEnterEvent(QDragEnterEvent* event)
{
  event->ignore();
}

void Z3DCanvas::dropEvent(QDropEvent* event)
{
  event->ignore();
}

void Z3DCanvas::timerEvent(QTimerEvent* e)
{
  //  if (m_engine) {
  //    QCoreApplication::postEvent(m_engine, e->clone());
  //  }
  QGraphicsView::timerEvent(e);
}

} // namespace nim
