#include "zrpcuidispatcher.h"

#include "z3danimation.h"
#include "z3danimationdoc.h"
#include "z3dcameraparameter.h"
#include "z3dmainwindow.h"
#include "z3dobjview.h"
#include "z3drenderingengine.h"
#include "zcameraparameteranimation.h"
#include "zdoc.h"
#include "zimgdoc.h"
#include "zlog.h"
#include "zmainwindow.h"
#include "zobjdoc.h"
#include "zneuroglancerprecomputed.h"
#include "zqobjectthreadinvoker.h"
#include "zscenejsonio.h"
#include "zview.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QThread>
#include <QUuid>
#include <QUrl>
#include <cmath>
#include <sstream>

namespace nim {

namespace {

[[nodiscard]] std::vector<size_t> filterVisualIds(ZDoc* doc, const std::vector<size_t>& in)
{
  std::vector<size_t> out;
  out.reserve(in.size());
  for (auto id : in) {
    if (doc) {
      auto* od = doc->idToDoc(id);
      if (od && od->typeName() == QStringLiteral("Animation3D")) {
        continue;
      }
    }
    out.push_back(id);
  }
  return out;
}

struct EngineCameraAndBBoxSnapshot
{
  json::value cameraJson;
  ZBBox<glm::dvec3> bbox;
};

[[nodiscard]] ZQObjectThreadInvokeResult<json::value> snapshotEngineCameraJson(Z3DRenderingEngine* engine,
                                                                              std::string_view what)
{
  return invokeOnObjectThreadWait(
    engine,
    [engine]() {
      return engine->camera().jsonValue();
    },
    what);
}

[[nodiscard]] ZQObjectThreadInvokeResult<EngineCameraAndBBoxSnapshot> snapshotEngineCameraAndBBox(Z3DRenderingEngine* engine,
                                                                                                  const std::vector<size_t>& ids,
                                                                                                  bool afterClipping,
                                                                                                  std::string_view what)
{
  return invokeOnObjectThreadWait(
    engine,
    [engine, &ids, afterClipping]() {
      EngineCameraAndBBoxSnapshot s;
      s.cameraJson = engine->camera().jsonValue();
      s.bbox = afterClipping ? engine->boundBoxOfObjsAfterClipping(ids) : engine->boundBoxOfObjs(ids);
      return s;
    },
    what);
}

[[nodiscard]] ZQObjectThreadInvokeResult<ZBBox<glm::dvec3>> snapshotEngineBBox(Z3DRenderingEngine* engine,
                                                                               const std::vector<size_t>& ids,
                                                                               bool afterClipping,
                                                                               std::string_view what)
{
  return invokeOnObjectThreadWait(
    engine,
    [engine, &ids, afterClipping]() {
      return afterClipping ? engine->boundBoxOfObjsAfterClipping(ids) : engine->boundBoxOfObjs(ids);
    },
    what);
}

[[nodiscard]] ZRpcUiDispatcher::ErrorKind mapAnimErrorKind(Z3DAnimationDoc::KeyOpResult::ErrorKind k)
{
  switch (k) {
    case Z3DAnimationDoc::KeyOpResult::ErrorKind::InvalidArgument:
      return ZRpcUiDispatcher::ErrorKind::InvalidArgument;
    case Z3DAnimationDoc::KeyOpResult::ErrorKind::FailedPrecondition:
      return ZRpcUiDispatcher::ErrorKind::FailedPrecondition;
  }
  return ZRpcUiDispatcher::ErrorKind::FailedPrecondition;
}

} // namespace

ZRpcUiDispatcher::ZRpcUiDispatcher(QObject* parent)
  : QObject(parent)
{
  // Must live on UI thread.
  CHECK(QCoreApplication::instance());
  CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());
}

void ZRpcUiDispatcher::setMainWindow(ZMainWindow* mainWindow)
{
  CHECK(QCoreApplication::instance());
  CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());
  m_mainWindow = mainWindow;
}

ZRpcUiDispatcher::StringResult ZRpcUiDispatcher::appLocation() const
{
  StringResult out;

  CHECK(QCoreApplication::instance());
  CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());

  // applicationDirPath:
  // - macOS:   .../Atlas.app/Contents/MacOS
  // - Windows: <install>/ (contains Atlas.exe)
  // - Linux:   <install>/ (contains Atlas)
  const QString appDir = QCoreApplication::applicationDirPath();
  if (appDir.isEmpty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "get_app_location: applicationDirPath is empty";
    return out;
  }

#if defined(Q_OS_MAC)
  // Convert .../Atlas.app/Contents/MacOS -> .../Atlas.app
  QDir d(appDir);
  if (!d.cdUp()) { // Contents
    out.ok = true;
    out.value = appDir;
    return out;
  }
  if (!d.cdUp()) { // .app
    out.ok = true;
    out.value = d.absolutePath();
    return out;
  }
  out.ok = true;
  out.value = d.absolutePath();
  return out;
#else
  out.ok = true;
  out.value = QDir(appDir).absolutePath();
  return out;
#endif
}

ZMainWindow* ZRpcUiDispatcher::mainWindowUi() const
{
  CHECK(QCoreApplication::instance());
  CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());

  if (m_mainWindow) {
    return m_mainWindow.data();
  }
  // Fallback for early startup or when the main window wasn't registered.
  for (QWidget* w : QApplication::topLevelWidgets()) {
    if (auto mw = qobject_cast<ZMainWindow*>(w)) {
      return mw;
    }
  }
  return nullptr;
}

ZRpcUiDispatcher::BoolResult ZRpcUiDispatcher::ensure3DWindow()
{
  BoolResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }

  if (!mainWin->get3DWindow()) {
    // Must run synchronously on the UI thread: this matches the GUI action
    // and avoids races for clients that poll EngineReady immediately after.
    mainWin->ensure3DWindow();
  }

  if (!mainWin->get3DWindow()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "failed to open 3D window";
    return out;
  }

  out.ok = true;
  return out;
}

bool ZRpcUiDispatcher::engineReady() const
{
  CHECK(QCoreApplication::instance());
  CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    return false;
  }
  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    return false;
  }
  auto* engine = w3d->engine();
  return engine && engine->thread() && engine->thread()->isRunning() && !engine->thread()->isFinished();
}

ZRpcUiDispatcher::BBoxValuesResult ZRpcUiDispatcher::bboxOfObjects(const std::vector<size_t>& requestedIds,
                                                                   bool afterClipping)
{
  BBoxValuesResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "bbox: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "bbox: doc not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "bbox: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "bbox: engine not ready";
    return out;
  }

  std::vector<size_t> invalidIds;
  std::vector<size_t> idsToUse;

  if (requestedIds.empty()) {
    idsToUse = filterVisualIds(doc, doc->objs());
  } else {
    invalidIds.reserve(requestedIds.size());
    for (auto id : requestedIds) {
      if (id <= kZRpcScopeGlobal) {
        invalidIds.push_back(id);
        continue;
      }
      if (!doc->idToDoc(id)) {
        invalidIds.push_back(id);
      }
    }
    if (!invalidIds.empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      std::ostringstream oss;
      oss << "bbox: invalid object ids: [";
      for (size_t i = 0; i < invalidIds.size(); ++i) {
        if (i) {
          oss << ", ";
        }
        oss << invalidIds[i];
      }
      oss << "]";
      out.error = oss.str();
      return out;
    }

    idsToUse = filterVisualIds(doc, requestedIds);
    if (idsToUse.empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      out.error = "bbox: ids contain no visual objects";
      return out;
    }
  }

  auto inv = invokeOnObjectThreadWait(
    engine,
    [engine, afterClipping, idsToUse = std::move(idsToUse)]() {
      return afterClipping ? engine->boundBoxOfObjsAfterClipping(idsToUse) : engine->boundBoxOfObjs(idsToUse);
    },
    "bbox:snapshot_engine");

  if (!inv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = inv.error;
    return out;
  }

  const auto& bb = inv.value;
  if (bb.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "bbox: empty bounding box (objects may not be ready)";
    return out;
  }

  out.minCorner = bb.minCorner;
  out.maxCorner = bb.maxCorner;
  out.size = bb.maxCorner - bb.minCorner;
  out.center = (bb.maxCorner + bb.minCorner) * 0.5;
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::BoolResult ZRpcUiDispatcher::cutSet(const CutSetRequest& req)
{
  BoolResult out;

  if (req.box.has_value() && !req.planes.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "cut_set: specify exactly one of box or planes";
    return out;
  }
  if (!req.box.has_value() && req.planes.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "cut_set: cut is required";
    return out;
  }

  if (req.box.has_value()) {
    const auto& b = *req.box;
    if (!std::isfinite(b.minCorner.x) || !std::isfinite(b.minCorner.y) || !std::isfinite(b.minCorner.z) ||
        !std::isfinite(b.maxCorner.x) || !std::isfinite(b.maxCorner.y) || !std::isfinite(b.maxCorner.z)) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      out.error = "cut_set: box corners must be finite numbers";
      return out;
    }
    if (b.minCorner.x > b.maxCorner.x || b.minCorner.y > b.maxCorner.y || b.minCorner.z > b.maxCorner.z) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      out.error = "cut_set: box min must be <= box max (component-wise)";
      return out;
    }
  }

  if (!req.planes.empty()) {
    for (const auto& p : req.planes) {
      if (!std::isfinite(p.a) || !std::isfinite(p.b) || !std::isfinite(p.c) || !std::isfinite(p.d)) {
        out.ok = false;
        out.errorKind = ErrorKind::InvalidArgument;
        out.error = "cut_set: plane coefficients must be finite numbers";
        return out;
      }
      // Support only axis-aligned planes: (1,0,0,-lower), (-1,0,0,upper), etc.
      const bool ok = (p.a == 1.0 && p.b == 0.0 && p.c == 0.0) || (p.a == -1.0 && p.b == 0.0 && p.c == 0.0) ||
                      (p.a == 0.0 && p.b == 1.0 && p.c == 0.0) || (p.a == 0.0 && p.b == -1.0 && p.c == 0.0) ||
                      (p.a == 0.0 && p.b == 0.0 && p.c == 1.0) || (p.a == 0.0 && p.b == 0.0 && p.c == -1.0);
      if (!ok) {
        out.ok = false;
        out.errorKind = ErrorKind::InvalidArgument;
        out.error = "cut_set: unsupported plane orientation (only axis-aligned planes are supported)";
        return out;
      }
    }
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "cut_set: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "cut_set: doc not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "cut_set: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "cut_set: engine not ready";
    return out;
  }

  std::vector<size_t> refitIds;
  if (req.refitCamera) {
    refitIds = filterVisualIds(doc, doc->objs());
  }

  auto inv = invokeOnObjectThreadWait(
    engine,
    [engine, req, refitIds = std::move(refitIds)]() -> bool {
      auto& gp = engine->globalParas();
      bool applied = false;
      if (req.box.has_value()) {
        gp.globalXCut.setLowerValue(req.box->minCorner.x);
        gp.globalXCut.setUpperValue(req.box->maxCorner.x);
        gp.globalYCut.setLowerValue(req.box->minCorner.y);
        gp.globalYCut.setUpperValue(req.box->maxCorner.y);
        gp.globalZCut.setLowerValue(req.box->minCorner.z);
        gp.globalZCut.setUpperValue(req.box->maxCorner.z);
        applied = true;
      } else if (!req.planes.empty()) {
        double lx = gp.globalXCut.lowerValue(), ux = gp.globalXCut.upperValue();
        double ly = gp.globalYCut.lowerValue(), uy = gp.globalYCut.upperValue();
        double lz = gp.globalZCut.lowerValue(), uz = gp.globalZCut.upperValue();
        for (const auto& p : req.planes) {
          if (p.a == 1.0 && p.b == 0.0 && p.c == 0.0) {
            lx = -p.d;
            applied = true;
          } else if (p.a == -1.0 && p.b == 0.0 && p.c == 0.0) {
            ux = p.d;
            applied = true;
          } else if (p.a == 0.0 && p.b == 1.0 && p.c == 0.0) {
            ly = -p.d;
            applied = true;
          } else if (p.a == 0.0 && p.b == -1.0 && p.c == 0.0) {
            uy = p.d;
            applied = true;
          } else if (p.a == 0.0 && p.b == 0.0 && p.c == 1.0) {
            lz = -p.d;
            applied = true;
          } else if (p.a == 0.0 && p.b == 0.0 && p.c == -1.0) {
            uz = p.d;
            applied = true;
          }
        }
        gp.globalXCut.setLowerValue(lx);
        gp.globalXCut.setUpperValue(ux);
        gp.globalYCut.setLowerValue(ly);
        gp.globalYCut.setUpperValue(uy);
        gp.globalZCut.setLowerValue(lz);
        gp.globalZCut.setUpperValue(uz);
      }

      if (!applied) {
        return false;
      }

      if (req.refitCamera && !refitIds.empty()) {
        const auto bb = engine->boundBoxOfObjsAfterClipping(refitIds);
        if (!bb.empty()) {
          engine->camera().resetCamera(bb, Z3DCamera::ResetOption::PreserveViewVector);
        }
      }
      return true;
    },
    "cut_set");

  if (!inv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = inv.error;
    return out;
  }
  if (!inv.value) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "cut_set failed";
    return out;
  }

  out.ok = true;
  return out;
}

ZRpcUiDispatcher::BoolResult ZRpcUiDispatcher::cutClear()
{
  BoolResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "cut_clear: main window not ready";
    return out;
  }
  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "cut_clear: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "cut_clear: engine not ready";
    return out;
  }

  auto inv = invokeOnObjectThreadWait(
    engine,
    [engine]() -> bool {
      auto& gp = engine->globalParas();
      gp.globalXCut.setLowerValue(gp.globalXCut.minimum());
      gp.globalXCut.setUpperValue(gp.globalXCut.maximum());
      gp.globalYCut.setLowerValue(gp.globalYCut.minimum());
      gp.globalYCut.setUpperValue(gp.globalYCut.maximum());
      gp.globalZCut.setLowerValue(gp.globalZCut.minimum());
      gp.globalZCut.setUpperValue(gp.globalZCut.maximum());
      return true;
    },
    "cut_clear");

  if (!inv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = inv.error;
    return out;
  }
  if (!inv.value) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "cut_clear failed";
    return out;
  }

  out.ok = true;
  return out;
}

ZRpcUiDispatcher::BBoxValuesResult ZRpcUiDispatcher::cutSuggestBox(const CutSuggestRequest& req)
{
  BBoxValuesResult out;

  const std::string modeStr = req.mode;
  if (!modeStr.empty() && modeStr != std::string("box")) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "cut_suggest: unsupported mode (supported: \"box\")";
    return out;
  }
  if (!std::isfinite(req.margin) || req.margin < 0.0) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "cut_suggest: margin must be a finite number >= 0";
    return out;
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "cut_suggest: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "cut_suggest: doc not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "cut_suggest: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "cut_suggest: engine not ready";
    return out;
  }

  std::vector<size_t> invalidIds;
  std::vector<size_t> ids = req.ids;
  if (ids.empty()) {
    ids = filterVisualIds(doc, doc->objs());
    if (ids.empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      out.error = "cut_suggest: no visual objects available";
      return out;
    }
  } else {
    invalidIds.reserve(ids.size());
    for (auto id : ids) {
      if (!doc->idToDoc(id)) {
        invalidIds.push_back(id);
      }
    }
    if (!invalidIds.empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      std::ostringstream oss;
      oss << "cut_suggest: unknown object ids: [";
      for (size_t i = 0; i < invalidIds.size(); ++i) {
        if (i) {
          oss << ", ";
        }
        oss << invalidIds[i];
      }
      oss << "]";
      out.error = oss.str();
      return out;
    }
    ids = filterVisualIds(doc, ids);
    if (ids.empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      out.error = "cut_suggest: ids contain no visual objects";
      return out;
    }
  }

  auto bboxInv = invokeOnObjectThreadWait(
    engine,
    [engine, after = req.afterClipping, ids = std::move(ids)]() {
      return after ? engine->boundBoxOfObjsAfterClipping(ids) : engine->boundBoxOfObjs(ids);
    },
    "cut_suggest:bbox");

  if (!bboxInv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = bboxInv.error;
    return out;
  }

  auto bb = std::move(bboxInv.value);
  if (!bb.empty() && req.margin > 0.0) {
    bb.expand(bb.minCorner - glm::dvec3(req.margin));
    bb.expand(bb.maxCorner + glm::dvec3(req.margin));
  }
  if (bb.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "cut_suggest: bbox empty";
    return out;
  }

  out.minCorner = bb.minCorner;
  out.maxCorner = bb.maxCorner;
  out.size = bb.maxCorner - bb.minCorner;
  out.center = (bb.maxCorner + bb.minCorner) * 0.5;
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::Screenshot3DResult ZRpcUiDispatcher::takeScreenshot3D(const Screenshot3DRequest& req)
{
  Screenshot3DResult out;

  if (req.width <= 0 || req.height <= 0) {
    out.ok = false;
    out.error = "take_screenshot_3d: width and height must be > 0";
    return out;
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.error = "take_screenshot_3d: main window not ready";
    return out;
  }
  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.error = "take_screenshot_3d: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.error = "take_screenshot_3d: engine not ready";
    return out;
  }

  QString outPath = req.path.trimmed();
  if (outPath.isEmpty()) {
    const QString ts = QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz");
    const QString uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString fn = QString("atlas_scene_screenshot3d_%1_%2.png").arg(ts, uid);
    outPath = QDir(QDir::tempPath()).filePath(fn);
  } else {
    QFileInfo fi(outPath);
    if (fi.isRelative()) {
      outPath = QDir::current().filePath(outPath);
      fi = QFileInfo(outPath);
    }
    if (fi.exists() && !req.overwrite) {
      out.ok = false;
      out.path = outPath;
      out.error = QString("take_screenshot_3d: file already exists (overwrite=false): %1").arg(outPath).toStdString();
      return out;
    }
  }

  // Ensure output directory exists.
  QDir dir(QFileInfo(outPath).absolutePath());
  if (!dir.exists()) {
    if (!dir.mkpath(".")) {
      out.ok = false;
      out.path = outPath;
      out.error = QString("take_screenshot_3d: failed to create output folder: %1").arg(dir.absolutePath()).toStdString();
      return out;
    }
  }

  if (QFileInfo(outPath).exists() && req.overwrite) {
    if (!QFile::remove(outPath)) {
      out.ok = false;
      out.path = outPath;
      out.error = QString("take_screenshot_3d: failed to replace existing file: %1").arg(outPath).toStdString();
      return out;
    }
  }

  out.path = outPath;

  auto inv = invokeOnObjectThreadWait(
    engine,
    [engine, outPath, width = req.width, height = req.height]() -> Screenshot3DResult {
      Screenshot3DResult r;
      r.path = outPath;

      QString err;
      auto conn = QObject::connect(
        engine,
        &Z3DRenderingEngine::renderingError,
        engine,
        [&](const QString& e) {
          if (err.isEmpty()) {
            err = e;
          }
        },
        Qt::DirectConnection);

      engine->takeFixedSizeScreenShot(outPath, width, height, Z3DScreenShotType::MonoView);

      QObject::disconnect(conn);

      if (QFileInfo(outPath).exists()) {
        r.ok = true;
        return r;
      }

      r.ok = false;
      r.error = err.isEmpty() ? QString("take_screenshot_3d: screenshot failed (no output file): %1").arg(outPath).toStdString()
                              : err.toStdString();
      return r;
    },
    "take_screenshot_3d");

  if (!inv.ok) {
    out.ok = false;
    out.error = inv.error;
    return out;
  }

  return inv.value;
}

ZRpcUiDispatcher::GetParamValuesResult ZRpcUiDispatcher::getParamValues(size_t id,
                                                                        const std::vector<std::string>& jsonKeys)
{
  GetParamValuesResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "get_param_values: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "get_param_values: doc not ready";
    return out;
  }

  if (id > kZRpcScopeGlobal && !doc->idToDoc(id)) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "get_param_values: object id not found";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "get_param_values: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "get_param_values: engine not ready";
    return out;
  }

  auto inv = invokeOnObjectThreadWait(
    engine,
    [engine, id, jsonKeys]() {
      GetParamValuesResult r;
      const auto params = engine->parametersOfViewSetting(id);
      if (params.empty()) {
        r.ok = false;
        r.errorKind = ErrorKind::FailedPrecondition;
        std::ostringstream oss;
        oss << "get_param_values: target_not_ready: id=" << id;
        r.error = oss.str();
        return r;
      }

      json::object values = Z3DViewSettingParamOps::getParamValues(*engine, id);
      if (jsonKeys.empty()) {
        r.ok = true;
        r.values = std::move(values);
        return r;
      }

      json::object filtered;
      for (const auto& k : jsonKeys) {
        if (auto* pv = values.if_contains(k)) {
          filtered[k] = *pv;
        }
      }
      r.ok = true;
      r.values = std::move(filtered);
      return r;
    },
    "get_param_values");

  if (!inv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = inv.error;
    return out;
  }

  return inv.value;
}

ZRpcUiDispatcher::ListParamsResult ZRpcUiDispatcher::listParams(size_t id)
{
  ListParamsResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "list_params: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "list_params: doc not ready";
    return out;
  }

  if (id > kZRpcScopeGlobal && !doc->idToDoc(id)) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "list_params: object id not found";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "list_params: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "list_params: engine not ready";
    return out;
  }

  auto inv = invokeOnObjectThreadWait(
    engine,
    [engine, id]() {
      ListParamsResult r;
      const auto params = engine->parametersOfViewSetting(id);
      if (params.empty()) {
        r.ok = false;
        r.errorKind = ErrorKind::FailedPrecondition;
        std::ostringstream oss;
        oss << "list_params: target_not_ready: id=" << id;
        r.error = oss.str();
        return r;
      }
      r.ok = true;
      r.params = Z3DViewSettingParamOps::listParams(*engine, id);
      return r;
    },
    "list_params");

  if (!inv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = inv.error;
    return out;
  }

  return inv.value;
}

ZRpcUiDispatcher::CapabilitiesResult ZRpcUiDispatcher::capabilities(const std::vector<uint64_t>& ids)
{
  CapabilitiesResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "capabilities: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "capabilities: doc not ready";
    return out;
  }

  const bool useAll = ids.empty();

  std::vector<uint64_t> invalidIds;
  if (!useAll) {
    for (auto id64 : ids) {
      const size_t id = static_cast<size_t>(id64);
      if (id <= kZRpcScopeGlobal) {
        invalidIds.push_back(id64);
        continue;
      }
      if (!doc->idToDoc(id)) {
        invalidIds.push_back(id64);
      }
    }
    if (!invalidIds.empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      std::ostringstream oss;
      oss << "capabilities: invalid ids: [";
      for (size_t i = 0; i < invalidIds.size(); ++i) {
        if (i) {
          oss << ", ";
        }
        oss << invalidIds[i];
      }
      oss << "]";
      out.error = oss.str();
      return out;
    }
  }

  // Collect ids by type on the UI thread (doc is authoritative for type names).
  std::map<QString, std::vector<size_t>> typeToIds;
  if (useAll) {
    for (auto id : doc->objs()) {
      if (auto* od = doc->idToDoc(id)) {
        typeToIds[od->typeName()].push_back(id);
      }
    }
  } else {
    for (auto id64 : ids) {
      const size_t id = static_cast<size_t>(id64);
      if (auto* od = doc->idToDoc(id)) {
        typeToIds[od->typeName()].push_back(id);
      }
    }
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "capabilities: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "capabilities: engine not ready";
    return out;
  }

  auto inv = invokeOnObjectThreadWait(
    engine,
    [engine, typeToIds = std::move(typeToIds)]() mutable {
      CapabilitiesResult r;

      // Always include camera/background/axis/global schemas.
      for (size_t scopeId : {kZRpcScopeCamera, kZRpcScopeBackground, kZRpcScopeAxis, kZRpcScopeGlobal}) {
        if (engine->parametersOfViewSetting(scopeId).empty()) {
          r.ok = false;
          r.errorKind = ErrorKind::FailedPrecondition;
          std::ostringstream oss;
          oss << "capabilities: target_not_ready: id=" << scopeId;
          r.error = oss.str();
          return r;
        }
      }

      r.camera = Z3DViewSettingParamOps::listParams(*engine, kZRpcScopeCamera);
      r.background = Z3DViewSettingParamOps::listParams(*engine, kZRpcScopeBackground);
      r.axis = Z3DViewSettingParamOps::listParams(*engine, kZRpcScopeAxis);
      r.global = Z3DViewSettingParamOps::listParams(*engine, kZRpcScopeGlobal);

      for (auto& [typeName, objectIds] : typeToIds) {
        std::vector<Z3DViewSettingParamOps::ParameterMeta> metas;
        for (auto id : objectIds) {
          if (!engine->parametersOfViewSetting(id).empty()) {
            metas = Z3DViewSettingParamOps::listParams(*engine, id);
            break;
          }
        }
        r.objects[typeName] = std::move(metas);
      }

      r.ok = true;
      return r;
    },
    "capabilities");

  if (!inv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = inv.error;
    return out;
  }

  return inv.value;
}

ZRpcUiDispatcher::ValidateSceneParamsResult ZRpcUiDispatcher::validateSceneParams(
  const std::vector<Z3DViewSettingParamOps::SetParamData>& setParams)
{
  ValidateSceneParamsResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.error = "validate_scene_params: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.error = "validate_scene_params: doc not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.error = "validate_scene_params: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.error = "validate_scene_params: engine not ready";
    return out;
  }

  // Pre-classify ids that are not present in the document so we can return a
  // stable "not_found" reason instead of a generic engine "target_not_ready".
  out.results.resize(setParams.size());
  std::vector<Z3DViewSettingParamOps::SetParamData> engineParams;
  std::vector<size_t> engineIndices;
  engineParams.reserve(setParams.size());
  engineIndices.reserve(setParams.size());

  for (size_t i = 0; i < setParams.size(); ++i) {
    const auto& sp = setParams[i];
    if (sp.id > kZRpcScopeGlobal && !doc->idToDoc(sp.id)) {
      Z3DViewSettingParamOps::ValidateResult r;
      r.jsonKey = sp.jsonKey.trimmed();
      r.ok = false;
      r.reason = QString("not_found: id=%1").arg(sp.id);
      out.results[i] = std::move(r);
      continue;
    }
    engineParams.push_back(sp);
    engineIndices.push_back(i);
  }

  auto inv = invokeOnObjectThreadWait(
    engine,
    [engine, engineParams = std::move(engineParams)]() mutable {
      return Z3DViewSettingParamOps::validate(*engine, engineParams);
    },
    "validate_scene_params");

  if (!inv.ok) {
    out.ok = false;
    out.error = inv.error;
    return out;
  }

  const auto& engineResults = inv.value;
  CHECK(engineResults.size() == engineIndices.size());
  for (size_t j = 0; j < engineIndices.size(); ++j) {
    out.results[engineIndices[j]] = engineResults[j];
  }

  bool allOk = true;
  for (const auto& r : out.results) {
    if (!r.ok) {
      allOk = false;
      break;
    }
  }

  out.ok = true;
  out.allOk = allOk;
  return out;
}

ZRpcUiDispatcher::BoolResult ZRpcUiDispatcher::applySceneParams(
  const std::vector<Z3DViewSettingParamOps::SetParamData>& setParams)
{
  BoolResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "apply_scene_params: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "apply_scene_params: doc not ready";
    return out;
  }

  std::vector<size_t> invalidIds;
  for (const auto& sp : setParams) {
    if (sp.id > kZRpcScopeGlobal && !doc->idToDoc(sp.id)) {
      invalidIds.push_back(sp.id);
    }
  }
  if (!invalidIds.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    std::ostringstream oss;
    oss << "apply_scene_params: object id not found: [";
    for (size_t i = 0; i < invalidIds.size(); ++i) {
      if (i) {
        oss << ", ";
      }
      oss << invalidIds[i];
    }
    oss << "]";
    out.error = oss.str();
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "apply_scene_params: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "apply_scene_params: engine not ready";
    return out;
  }

  auto inv = invokeOnObjectThreadWait(
    engine,
    [engine, setParams]() {
      BoolResult r;

      const auto vr = Z3DViewSettingParamOps::validate(*engine, setParams);
      CHECK(vr.size() == setParams.size());

      bool anyInvalidArg = false;
      std::vector<size_t> badIndices;
      for (size_t i = 0; i < vr.size(); ++i) {
        if (vr[i].ok) {
          continue;
        }
        badIndices.push_back(i);
        const std::string reason = vr[i].reason.toStdString();
        if (reason.rfind("target_not_ready", 0) != 0) {
          anyInvalidArg = true;
        }
      }

      if (!badIndices.empty()) {
        r.ok = false;
        r.errorKind = anyInvalidArg ? ErrorKind::InvalidArgument : ErrorKind::FailedPrecondition;
        std::ostringstream oss;
        oss << "apply_scene_params: validation failed: ";
        for (size_t j = 0; j < badIndices.size(); ++j) {
          const size_t i = badIndices[j];
          const auto& sp = setParams[i];
          const auto& rr = vr[i];
          if (j) {
            oss << "; ";
          }
          oss << "id=" << sp.id << " json_key=" << rr.jsonKey.toStdString() << " reason=" << rr.reason.toStdString();
        }
        r.error = oss.str();
        return r;
      }

      std::vector<Z3DViewSettingParamOps::SetParamData> normalized;
      normalized.reserve(setParams.size());
      for (size_t i = 0; i < setParams.size(); ++i) {
        Z3DViewSettingParamOps::SetParamData sp = setParams[i];
        if (vr[i].ok && vr[i].hasNormalizedValue) {
          sp.value = vr[i].normalizedValue;
        }
        normalized.push_back(std::move(sp));
      }

      const auto [ok, err] = Z3DViewSettingParamOps::apply(*engine, normalized);
      if (!ok) {
        r.ok = false;
        r.errorKind = ErrorKind::FailedPrecondition;
        r.error = err.empty() ? "apply_scene_params failed" : err;
        return r;
      }

      r.ok = true;
      return r;
    },
    "apply_scene_params");

  if (!inv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = inv.error;
    return out;
  }

  return inv.value;
}

ZRpcUiDispatcher::ListObjectsResult ZRpcUiDispatcher::listObjects()
{
  ListObjectsResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  const auto ids = doc->objs();
  out.objects.reserve(ids.size());
  for (auto id : ids) {
    auto* od = doc->idToDoc(id);
    if (!od) {
      continue;
    }
    ListedObject oi;
    oi.id = static_cast<uint64_t>(id);
    oi.type = od->typeName().toStdString();
    oi.name = doc->objName(id).toStdString();
    oi.path = od->objPath(id).toStdString();
    oi.visible = doc->isObjVisible(id);
    out.objects.push_back(std::move(oi));
  }

  out.ok = true;
  return out;
}

ZRpcUiDispatcher::ListObjectsResult ZRpcUiDispatcher::loadFilesAndListObjects(const QStringList& filePaths)
{
  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    ListObjectsResult out;
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "load_files: main window not ready";
    return out;
  }

  if (filePaths.isEmpty()) {
    return listObjects();
  }

  QStringList problems;
  QList<QUrl> urls;
  urls.reserve(filePaths.size());
  for (int i = 0; i < filePaths.size(); ++i) {
    const QString raw = filePaths.at(i).trimmed();
    if (raw.isEmpty()) {
      problems.push_back(QString("files[%1]: empty path").arg(i));
      continue;
    }

    QString localPath = raw;
    if (raw.contains("://")) {
      const QUrl url(raw);
      if (!url.isLocalFile()) {
        problems.push_back(QString("files[%1]: only local paths are supported (got url=%2)").arg(i).arg(raw));
        continue;
      }
      localPath = url.toLocalFile();
    }

    const QFileInfo fi(localPath);
    if (!fi.exists()) {
      problems.push_back(QString("files[%1]: not found: %2").arg(i).arg(localPath));
      continue;
    }
    urls.push_back(QUrl::fromLocalFile(fi.canonicalFilePath()));
  }

  if (!problems.isEmpty()) {
    ListObjectsResult out;
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = QString("load_files: invalid input: %1").arg(problems.join("; ")).toStdString();
    return out;
  }

  mainWin->loadUrls(urls);
  return listObjects();
}

ZRpcUiDispatcher::AddNeuroglancerPrecomputedResult
ZRpcUiDispatcher::addNeuroglancerPrecomputedVolume(std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol, bool setVisible)
{
  AddNeuroglancerPrecomputedResult out;

  if (!vol) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "add_neuroglancer_precomputed_volume: volume is null";
    return out;
  }

  const QString rootUrl = vol->rootUrl();

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "add_neuroglancer_precomputed_volume: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "add_neuroglancer_precomputed_volume: doc not ready";
    return out;
  }

  QString err;
  const size_t id = doc->imgDoc().addNeuroglancerPrecomputedVolume(std::move(vol), err);
  if (id == 0) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = err.isEmpty() ? "add_neuroglancer_precomputed_volume: failed to add dataset" : err.toStdString();
    return out;
  }

  if (setVisible) {
    doc->setObjVisible(id, true);
  }

  out.ok = true;
  out.id = static_cast<uint64_t>(id);
  out.rootUrl = rootUrl;
  return out;
}

ZRpcUiDispatcher::BoolResult ZRpcUiDispatcher::setVisibility(const std::vector<size_t>& ids, bool on)
{
  BoolResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "set_visibility: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "set_visibility: doc not ready";
    return out;
  }

  std::vector<size_t> invalidIds;
  invalidIds.reserve(ids.size());
  for (auto id : ids) {
    if (!doc->idToDoc(id)) {
      invalidIds.push_back(id);
    }
  }
  if (!invalidIds.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    std::ostringstream oss;
    oss << "set_visibility: unknown object ids: [";
    for (size_t i = 0; i < invalidIds.size(); ++i) {
      if (i) {
        oss << ", ";
      }
      oss << invalidIds[i];
    }
    oss << "]";
    out.error = oss.str();
    return out;
  }

  for (auto id : ids) {
    doc->setObjVisible(id, on);
  }

  out.ok = true;
  return out;
}

ZRpcUiDispatcher::BoolResult ZRpcUiDispatcher::removeObjects(const std::vector<size_t>& ids, bool allowUnsaved)
{
  BoolResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "remove_objects: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "remove_objects: doc not ready";
    return out;
  }
  if (ids.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "remove_objects: ids must be non-empty";
    return out;
  }

  std::vector<size_t> invalidIds;
  invalidIds.reserve(ids.size());
  std::vector<size_t> unsavedIds;
  unsavedIds.reserve(ids.size());

  for (auto id : ids) {
    ZObjDoc* od = doc->idToDoc(id);
    if (!od) {
      invalidIds.push_back(id);
      continue;
    }
    if (!allowUnsaved && od->objHasUnsavedChange(id)) {
      unsavedIds.push_back(id);
    }
  }

  if (!invalidIds.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    std::ostringstream oss;
    oss << "remove_objects: unknown object ids: [";
    for (size_t i = 0; i < invalidIds.size(); ++i) {
      if (i) {
        oss << ", ";
      }
      oss << invalidIds[i];
    }
    oss << "]";
    out.error = oss.str();
    return out;
  }

  if (!unsavedIds.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    std::ostringstream oss;
    oss << "remove_objects: refusing to remove objects with unsaved changes: [";
    for (size_t i = 0; i < unsavedIds.size(); ++i) {
      if (i) {
        oss << ", ";
      }
      oss << unsavedIds[i];
    }
    oss << "]. To discard changes, set allow_unsaved=true.";
    out.error = oss.str();
    return out;
  }

  doc->removeObjsNoPrompt(ids);
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::MakeAliasResult ZRpcUiDispatcher::makeAliases(const std::vector<uint64_t>& srcIds)
{
  MakeAliasResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "make_alias: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "make_alias: doc not ready";
    return out;
  }

  if (srcIds.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "make_alias: ids must be non-empty";
    return out;
  }

  out.aliases.reserve(srcIds.size());
  std::vector<uint64_t> invalidIds;
  std::vector<uint64_t> unsupportedIds;
  for (auto srcId64 : srcIds) {
    const size_t srcId = static_cast<size_t>(srcId64);
    ZObjDoc* od = doc->idToDoc(srcId);
    if (!od) {
      // System boundary: record invalid id and continue.
      out.hadInvalidId = true;
      invalidIds.push_back(srcId64);
      continue;
    }
    const size_t aliasId = od->makeAlias(srcId);
    if (aliasId == 0) {
      // Type does not support aliasing or alias creation failed.
      out.hadUnsupported = true;
      unsupportedIds.push_back(srcId64);
      continue;
    }
    out.aliases.push_back(MakeAliasPair{srcId64, static_cast<uint64_t>(aliasId)});
  }

  const bool ok = !out.aliases.empty() && !out.hadInvalidId && !out.hadUnsupported;
  out.ok = ok;

  if (!ok) {
    out.errorKind = ErrorKind::InvalidArgument;
    std::ostringstream oss;
    oss << "make_alias: ";
    if (out.aliases.empty()) {
      oss << "no aliases were created";
    } else {
      oss << "partial success";
    }
    if (!invalidIds.empty()) {
      oss << "; invalid_ids=[";
      for (size_t i = 0; i < invalidIds.size(); ++i) {
        if (i) {
          oss << ", ";
        }
        oss << invalidIds[i];
      }
      oss << "]";
    }
    if (!unsupportedIds.empty()) {
      oss << "; unsupported_ids=[";
      for (size_t i = 0; i < unsupportedIds.size(); ++i) {
        if (i) {
          oss << ", ";
        }
        oss << unsupportedIds[i];
      }
      oss << "]";
    }
    out.error = oss.str();
  }
  return out;
}

ZRpcUiDispatcher::StatusSnapshot ZRpcUiDispatcher::statusSnapshot(const std::vector<uint64_t>& ids,
                                                                  bool includeAllObjects)
{
  StatusSnapshot out;

  ZMainWindow* mainWin = mainWindowUi();
  ZDoc* doc = mainWin ? mainWin->doc() : nullptr;
  out.docReady = (doc != nullptr);

  auto* w3d = mainWin ? mainWin->get3DWindow() : nullptr;
  out.has3DWindow = (w3d != nullptr);

  Z3DRenderingEngine* engine = w3d ? w3d->engine() : nullptr;

  // Resolve which ids to report.
  std::vector<uint64_t> idsToReport;
  if (includeAllObjects) {
    if (doc) {
      const auto docIds = doc->objs();
      idsToReport.reserve(docIds.size());
      for (auto id : docIds) {
        idsToReport.push_back(static_cast<uint64_t>(id));
      }
    }
  } else {
    idsToReport = ids;
  }

  out.objects.reserve(idsToReport.size());
  for (auto id64 : idsToReport) {
    StatusObject st;
    st.id = id64;
    const size_t id = static_cast<size_t>(id64);
    if (doc) {
      if (auto* od = doc->idToDoc(id)) {
        st.type = od->typeName().toStdString();
        st.name = doc->objName(id).toStdString();
        st.path = od->objPath(id).toStdString();
        st.visible = doc->isObjVisible(id);
      }
    }
    out.objects.push_back(std::move(st));
  }

  // Determine per-object view readiness on the engine's thread (single-GL-context assumption).
  bool engineReady = engine && engine->thread() && engine->thread()->isRunning() && !engine->thread()->isFinished();
  std::vector<bool> viewReady(out.objects.size(), false);
  if (engineReady) {
    auto viewInv = invokeOnObjectThreadWait(
      engine,
      [engine, objs = out.objects]() {
        std::vector<bool> r;
        r.reserve(objs.size());
        for (const auto& info : objs) {
          const size_t id = static_cast<size_t>(info.id);
          if (id <= kZRpcScopeGlobal) {
            r.push_back(true);
            continue;
          }
          bool found = false;
          for (const auto& ov : engine->objViews()) {
            if (ov && ov->hasObj(id)) {
              found = true;
              break;
            }
          }
          r.push_back(found);
        }
        return r;
      },
      "get_status:view_ready");

    if (viewInv.ok) {
      viewReady = std::move(viewInv.value);
    } else {
      engineReady = false;
    }
  }

  out.engineReady = engineReady;

  for (size_t i = 0; i < out.objects.size(); ++i) {
    auto& st = out.objects[i];
    const size_t id = static_cast<size_t>(st.id);

    if (!out.docReady) {
      st.loadState = ObjectLoadState::DocNotReady;
    } else if (id > kZRpcScopeGlobal && doc && !doc->idToDoc(id)) {
      st.loadState = ObjectLoadState::NotFound;
    } else if (!engineReady) {
      st.loadState = ObjectLoadState::EngineNotReady;
    } else if (id <= kZRpcScopeGlobal) {
      st.loadState = ObjectLoadState::Ready;
    } else if (i < viewReady.size() && viewReady[i]) {
      st.loadState = ObjectLoadState::Ready;
    } else {
      st.loadState = ObjectLoadState::ViewNotReady;
    }
  }

  out.ok = true;
  return out;
}

std::vector<uint64_t> ZRpcUiDispatcher::fitCandidates()
{
  std::vector<uint64_t> out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    return out;
  }

  auto filtered = filterVisualIds(doc, doc->objs());
  out.reserve(filtered.size());
  for (auto id : filtered) {
    out.push_back(static_cast<uint64_t>(id));
  }
  return out;
}

ZRpcUiDispatcher::SaveSceneResult ZRpcUiDispatcher::saveSceneToPath(const QString& path)
{
  SaveSceneResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }
  ZView* view = mainWin->view();
  if (!view) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "view not ready";
    return out;
  }

  Z3DRenderingEngine* engineOrNull = nullptr;
  if (auto* w3d = mainWin->get3DWindow()) {
    engineOrNull = w3d->engine();
  }

  QString err;
  const bool ok = ZSceneJsonIO::saveToPath(doc, view, engineOrNull, path, err);
  out.ok = ok;
  if (!ok) {
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = err.isEmpty() ? "save_scene failed" : err.toStdString();
  }
  return out;
}

ZRpcUiDispatcher::EnsureAnimationResult ZRpcUiDispatcher::ensureAnimation3D(bool createNew, const QString& name)
{
  EnsureAnimationResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  auto& ad = doc->animation3DDoc();
  const auto ids = ad.animationIds();
  if (!createNew && !ids.empty()) {
    out.ok = true;
    out.animationId = static_cast<uint64_t>(ids.front());
    out.created = false;
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    mainWin->ensure3DWindow();
    w3d = mainWin->get3DWindow();
  }
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "engine not ready";
    return out;
  }

  // Ensure the animation doc is bound to the current 3D engine before creating
  // a new animation. This guarantees UI parity: a newly created Animation3D
  // captures a full default keyframe at t=0 (no scene fallback).
  ad.bindView(engine);

  const QString rawName = name.trimmed();
  const QString resolvedName = rawName.isEmpty() ? QStringLiteral("LLM Animation") : rawName;
  const size_t id = ad.createNewAnimationAndReturnId(resolvedName);
  if (id == 0) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "failed to create animation";
    return out;
  }
  out.ok = true;
  out.animationId = static_cast<uint64_t>(id);
  out.created = true;
  return out;
}

ZRpcUiDispatcher::BoolResult ZRpcUiDispatcher::setAnimationDuration(uint64_t animationId, double duration)
{
  BoolResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  auto& ad = doc->animation3DDoc();
  auto* anim = ad.animationPtr(static_cast<size_t>(animationId));
  if (!anim) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "animation_id not found";
    return out;
  }
  anim->setDuration(duration);
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::BoolResult ZRpcUiDispatcher::setAnimationKey(const SetKeyRequest& req)
{
  BoolResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  auto& ad = doc->animation3DDoc();
  auto r =
    ad.setKey(static_cast<size_t>(req.animationId),
              static_cast<size_t>(req.targetId),
              req.jsonKey,
              req.timeSec,
              req.easing,
              req.value);
  if (!r.ok) {
    out.ok = false;
    out.errorKind = mapAnimErrorKind(r.errorKind);
    out.error = r.error.isEmpty() ? "set_key failed" : r.error.toStdString();
    return out;
  }
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::BoolResult ZRpcUiDispatcher::removeAnimationKey(const RemoveKeyRequest& req)
{
  BoolResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  auto& ad = doc->animation3DDoc();
  auto r = ad.removeKey(static_cast<size_t>(req.animationId),
                        static_cast<size_t>(req.targetId),
                        req.jsonKey,
                        req.timeSec);
  if (!r.ok) {
    out.ok = false;
    out.errorKind = mapAnimErrorKind(r.errorKind);
    out.error = r.error.isEmpty() ? "remove_key failed" : r.error.toStdString();
    return out;
  }
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::BoolResult ZRpcUiDispatcher::clearAnimationKeys(const ClearKeysRequest& req)
{
  BoolResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  auto& ad = doc->animation3DDoc();
  auto r =
    ad.clearKeys(static_cast<size_t>(req.animationId), static_cast<size_t>(req.targetId), req.jsonKey);
  if (!r.ok) {
    out.ok = false;
    out.errorKind = mapAnimErrorKind(r.errorKind);
    out.error = r.error.isEmpty() ? "clear_keys failed" : r.error.toStdString();
    return out;
  }
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::BoolResult ZRpcUiDispatcher::batchAnimationKeys(const BatchKeysRequest& req)
{
  BoolResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  std::vector<Z3DAnimationDoc::BatchRemoveKeyOp> removes;
  removes.reserve(req.removeKeys.size());
  for (const auto& r : req.removeKeys) {
    Z3DAnimationDoc::BatchRemoveKeyOp o;
    o.targetId = static_cast<size_t>(r.targetId);
    o.jsonKey = r.jsonKey;
    o.timeSec = r.timeSec;
    removes.push_back(std::move(o));
  }
  std::vector<Z3DAnimationDoc::BatchSetKeyOp> sets;
  sets.reserve(req.setKeys.size());
  for (const auto& s : req.setKeys) {
    Z3DAnimationDoc::BatchSetKeyOp o;
    o.targetId = static_cast<size_t>(s.targetId);
    o.jsonKey = s.jsonKey;
    o.timeSec = s.timeSec;
    o.easing = s.easing;
    o.value = s.value;
    sets.push_back(std::move(o));
  }

  auto& ad = doc->animation3DDoc();
  auto r = ad.batchKeys(static_cast<size_t>(req.animationId), removes, sets, req.commit);
  if (!r.ok) {
    out.ok = false;
    out.errorKind = mapAnimErrorKind(r.errorKind);
    out.error = r.error.isEmpty() ? "batch failed" : r.error.toStdString();
    return out;
  }
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::ListKeysResult ZRpcUiDispatcher::listAnimationKeys(const ListKeysRequest& req)
{
  ListKeysResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  auto& ad = doc->animation3DDoc();
  auto r = ad.listKeys(static_cast<size_t>(req.animationId),
                       static_cast<size_t>(req.targetId),
                       req.jsonKey,
                       req.includeValues);
  if (!r.ok) {
    out.ok = false;
    out.errorKind = mapAnimErrorKind(r.errorKind);
    out.error = r.error.isEmpty() ? "list_keys failed" : r.error.toStdString();
    return out;
  }
  out.keys.reserve(r.keys.size());
  for (const auto& k : r.keys) {
    ListedKey dk;
    dk.timeSec = k.timeSec;
    dk.parameterType = k.parameterType.toStdString();
    dk.valueJson = k.keyJson.toStdString();
    out.keys.push_back(std::move(dk));
  }
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::TimeStatusResult ZRpcUiDispatcher::animationTimeStatus(uint64_t animationId)
{
  TimeStatusResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  auto& ad = doc->animation3DDoc();
  auto r = ad.timeStatus(static_cast<size_t>(animationId));
  if (!r.ok) {
    out.ok = false;
    out.errorKind = mapAnimErrorKind(r.errorKind);
    out.error = r.error.isEmpty() ? "get_time failed" : r.error.toStdString();
    return out;
  }
  out.ok = true;
  out.duration = r.duration;
  out.seconds = r.seconds;
  return out;
}

ZRpcUiDispatcher::BoolResult ZRpcUiDispatcher::setAnimationTime(const SetTimeRequest& req)
{
  BoolResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  auto& ad = doc->animation3DDoc();
  auto r = ad.setTime(static_cast<size_t>(req.animationId), req.seconds, req.cancelRendering);
  if (!r.ok) {
    out.ok = false;
    out.errorKind = mapAnimErrorKind(r.errorKind);
    out.error = r.error.isEmpty() ? "set_time failed" : r.error.toStdString();
    return out;
  }
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::BoolResult ZRpcUiDispatcher::addAnimationKeyFrame(const AddKeyFrameRequest& req)
{
  BoolResult out;

  if (req.animationId == 0) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "animation_id is required";
    return out;
  }
  if (!(req.timeSec >= 0.0) || !std::isfinite(req.timeSec)) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "time must be finite and >= 0";
    return out;
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "engine not ready";
    return out;
  }

  auto& ad = doc->animation3DDoc();
  auto* anim = ad.animationPtr(static_cast<size_t>(req.animationId));
  if (!anim) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "animation_id not found";
    return out;
  }

  // Ensure the animation doc is bound to the current 3D engine (UI parity).
  // This guarantees anim->addKeyFrame() has a valid engine binding.
  ad.bindView(engine);

  // Mirror the UI flow: scrub to time (optionally cancelling long rendering),
  // then capture a full keyframe snapshot at that time.
  if (req.cancelRendering) {
    anim->cancelRenderingAndSetCurrentTime(req.timeSec);
  } else {
    anim->setCurrentTime(req.timeSec);
  }
  anim->addKeyFrame(req.timeSec);

  out.ok = true;
  return out;
}

ZRpcUiDispatcher::SaveAnimationResult ZRpcUiDispatcher::saveAnimationToPath(uint64_t animationId, const QString& path)
{
  SaveAnimationResult out;

  if (path.trimmed().isEmpty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "path is required";
    return out;
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  auto& ad = doc->animation3DDoc();
  auto* anim = ad.animationPtr(static_cast<size_t>(animationId));
  if (!anim) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "animation_id not found";
    return out;
  }

  const bool ok = ad.saveToPath(static_cast<size_t>(animationId), path.trimmed());
  if (!ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "save_animation failed";
    return out;
  }

  out.ok = true;
  return out;
}

ZRpcUiDispatcher::BoolResult ZRpcUiDispatcher::setCameraInterpolationMethod(uint64_t animationId, const QString& method)
{
  BoolResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  auto& ad = doc->animation3DDoc();
  auto r = ad.setCameraInterpolationMethod(static_cast<size_t>(animationId), method);
  if (!r.ok) {
    out.ok = false;
    out.errorKind = mapAnimErrorKind(r.errorKind);
    out.error =
      r.error.isEmpty() ? "set_camera_interpolation_method failed" : r.error.toStdString();
    return out;
  }

  out.ok = true;
  return out;
}

ZRpcUiDispatcher::CameraInterpolationMethodResult ZRpcUiDispatcher::cameraInterpolationMethod(uint64_t animationId)
{
  CameraInterpolationMethodResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  auto& ad = doc->animation3DDoc();
  auto r = ad.cameraInterpolationMethod(static_cast<size_t>(animationId));
  if (!r.ok) {
    out.ok = false;
    out.errorKind = mapAnimErrorKind(r.errorKind);
    out.error =
      r.error.isEmpty() ? "get_camera_interpolation_method failed" : r.error.toStdString();
    return out;
  }

  out.ok = true;
  out.method = r.method.toStdString();
  return out;
}

ZRpcUiDispatcher::CameraValuesResult ZRpcUiDispatcher::cameraGet()
{
  CameraValuesResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_get: main window not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_get: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_get: engine not ready";
    return out;
  }

  auto snapInv = snapshotEngineCameraJson(engine, "camera_get:snapshot_engine");
  if (!snapInv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = snapInv.error;
    return out;
  }
  if (!snapInv.value.is_object()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_get: engine camera invalid";
    return out;
  }

  out.values.push_back(std::move(snapInv.value));
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::CameraValuesResult ZRpcUiDispatcher::cameraFit(const CameraFitRequest& req)
{
  CameraValuesResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_fit: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_fit: doc not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_fit: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_fit: engine not ready";
    return out;
  }

  const bool useAll = req.all || req.ids.empty();
  std::vector<size_t> ids = useAll ? doc->objs() : req.ids;

  if (!useAll) {
    std::vector<size_t> invalidIds;
    for (auto id : ids) {
      if (!doc->idToDoc(id)) {
        invalidIds.push_back(id);
      }
    }
    if (!invalidIds.empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      std::ostringstream oss;
      oss << "camera_fit: unknown object ids: [";
      for (size_t i = 0; i < invalidIds.size(); ++i) {
        if (i) {
          oss << ", ";
        }
        oss << invalidIds[i];
      }
      oss << "]";
      out.error = oss.str();
      return out;
    }
  }

  ids = filterVisualIds(doc, ids);
  if (ids.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = useAll ? "camera_fit: no visual objects available" : "camera_fit: ids contain no visual objects";
    return out;
  }

  auto snapInv = snapshotEngineCameraAndBBox(engine, ids, req.afterClipping, "camera_fit:snapshot_engine");
  if (!snapInv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = snapInv.error;
    return out;
  }
  EngineCameraAndBBoxSnapshot snap = std::move(snapInv.value);
  if (!snap.cameraJson.is_object()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_fit: engine camera invalid";
    return out;
  }

  ZBBox<glm::dvec3> bb = snap.bbox;
  if (bb.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_fit: bbox empty";
    return out;
  }

  if (req.minRadius > 0.0) {
    const auto cent = (bb.minCorner + bb.maxCorner) * 0.5;
    bb.expand(ZBBox<glm::dvec3>{cent - glm::dvec3(req.minRadius), cent + glm::dvec3(req.minRadius)});
  }

  Z3DCameraParameter base("Camera");
  base.readValue(snap.cameraJson);

  Z3DCameraPlanner::SolveRequest solveReq;
  solveReq.mode = Z3DCameraPlanner::SolveMode::Fit;
  solveReq.t0 = 0.0;
  solveReq.bbox = bb;

  std::string planErr;
  auto keys = Z3DCameraPlanner::solve(base, solveReq, planErr);
  if (!planErr.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = planErr;
    return out;
  }
  if (keys.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_fit: no camera key produced";
    return out;
  }

  out.values.push_back(std::move(keys.front().value));
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::CameraValuesResult ZRpcUiDispatcher::cameraOrbitSuggest(const CameraOrbitSuggestRequest& req)
{
  CameraValuesResult out;

  const QString axis = QString::fromStdString(req.axis).toLower().trimmed();
  if (!(axis == "x" || axis == "y" || axis == "z")) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_orbit_suggest: axis must be one of: x, y, z";
    return out;
  }

  const double degrees = (req.degrees == 0.0) ? 360.0 : req.degrees;
  if (!std::isfinite(degrees)) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_orbit_suggest: degrees must be finite";
    return out;
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_orbit_suggest: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_orbit_suggest: doc not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_orbit_suggest: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_orbit_suggest: engine not ready";
    return out;
  }

  std::vector<size_t> ids = req.ids.empty() ? doc->objs() : req.ids;
  if (!req.ids.empty()) {
    std::vector<size_t> invalidIds;
    for (auto id : ids) {
      if (!doc->idToDoc(id)) {
        invalidIds.push_back(id);
      }
    }
    if (!invalidIds.empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      std::ostringstream oss;
      oss << "camera_orbit_suggest: unknown object ids: [";
      for (size_t i = 0; i < invalidIds.size(); ++i) {
        if (i) {
          oss << ", ";
        }
        oss << invalidIds[i];
      }
      oss << "]";
      out.error = oss.str();
      return out;
    }
  }
  ids = filterVisualIds(doc, ids);
  if (ids.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = req.ids.empty() ? "camera_orbit_suggest: no visual objects available"
                                : "camera_orbit_suggest: ids contain no visual objects";
    return out;
  }

  auto snapInv = snapshotEngineCameraAndBBox(engine, ids, /*afterClipping=*/false, "camera_orbit_suggest:snapshot_engine");
  if (!snapInv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = snapInv.error;
    return out;
  }
  EngineCameraAndBBoxSnapshot snap = std::move(snapInv.value);
  if (!snap.cameraJson.is_object()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_orbit_suggest: engine camera invalid";
    return out;
  }
  if (snap.bbox.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_orbit_suggest: bbox empty";
    return out;
  }

  Z3DCameraParameter base("Camera");
  base.readValue(snap.cameraJson);

  Z3DCameraPlanner::SolveRequest solveReq;
  solveReq.mode = Z3DCameraPlanner::SolveMode::Orbit;
  solveReq.t0 = 0.0;
  solveReq.t1 = 1.0;
  solveReq.bbox = snap.bbox;
  solveReq.orbit.axis = axis.toStdString().front();
  solveReq.orbit.degrees = degrees;
  solveReq.orbit.maxStepDegrees = std::abs(degrees);

  std::string planErr;
  auto keys = Z3DCameraPlanner::solve(base, solveReq, planErr);
  if (!planErr.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = planErr;
    return out;
  }

  out.values.reserve(keys.size());
  for (auto& k : keys) {
    out.values.push_back(std::move(k.value));
  }
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::CameraValuesResult ZRpcUiDispatcher::cameraDollySuggest(const CameraDollySuggestRequest& req)
{
  CameraValuesResult out;

  if (!std::isfinite(req.startDist) || !std::isfinite(req.endDist)) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_dolly_suggest: start_dist and end_dist must be finite";
    return out;
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_dolly_suggest: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_dolly_suggest: doc not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_dolly_suggest: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_dolly_suggest: engine not ready";
    return out;
  }

  std::vector<size_t> ids = req.ids.empty() ? doc->objs() : req.ids;
  if (!req.ids.empty()) {
    std::vector<size_t> invalidIds;
    for (auto id : ids) {
      if (!doc->idToDoc(id)) {
        invalidIds.push_back(id);
      }
    }
    if (!invalidIds.empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      std::ostringstream oss;
      oss << "camera_dolly_suggest: unknown object ids: [";
      for (size_t i = 0; i < invalidIds.size(); ++i) {
        if (i) {
          oss << ", ";
        }
        oss << invalidIds[i];
      }
      oss << "]";
      out.error = oss.str();
      return out;
    }
  }
  ids = filterVisualIds(doc, ids);
  if (ids.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = req.ids.empty() ? "camera_dolly_suggest: no visual objects available"
                                : "camera_dolly_suggest: ids contain no visual objects";
    return out;
  }

  auto snapInv = snapshotEngineCameraAndBBox(engine, ids, /*afterClipping=*/false, "camera_dolly_suggest:snapshot_engine");
  if (!snapInv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = snapInv.error;
    return out;
  }
  EngineCameraAndBBoxSnapshot snap = std::move(snapInv.value);
  if (!snap.cameraJson.is_object()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_dolly_suggest: engine camera invalid";
    return out;
  }
  if (snap.bbox.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_dolly_suggest: bbox empty";
    return out;
  }

  Z3DCameraParameter base("Camera");
  base.readValue(snap.cameraJson);

  Z3DCameraPlanner::SolveRequest solveReq;
  solveReq.mode = Z3DCameraPlanner::SolveMode::Dolly;
  solveReq.t0 = 0.0;
  solveReq.t1 = 1.0;
  solveReq.bbox = snap.bbox;
  solveReq.dolly.startDist = req.startDist;
  solveReq.dolly.endDist = req.endDist;

  std::string planErr;
  auto keys = Z3DCameraPlanner::solve(base, solveReq, planErr);
  if (!planErr.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = planErr;
    return out;
  }

  out.values.reserve(keys.size());
  for (auto& k : keys) {
    out.values.push_back(std::move(k.value));
  }
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::CameraValuesResult ZRpcUiDispatcher::cameraFocus(const CameraFocusRequest& req)
{
  CameraValuesResult out;

  if (!std::isfinite(req.minRadius) || req.minRadius < 0.0) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_focus: min_radius must be a finite number >= 0";
    return out;
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_focus: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_focus: doc not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_focus: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_focus: engine not ready";
    return out;
  }

  std::vector<size_t> ids = req.ids.empty() ? doc->objs() : req.ids;
  if (!req.ids.empty()) {
    // Validate ids and filter non-visual objects (Animation3D).
    std::vector<size_t> invalidIds;
    for (auto id : ids) {
      if (!doc->idToDoc(id)) {
        invalidIds.push_back(id);
      }
    }
    if (!invalidIds.empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      std::ostringstream oss;
      oss << "camera_focus: unknown object ids: [";
      for (size_t i = 0; i < invalidIds.size(); ++i) {
        if (i) {
          oss << ", ";
        }
        oss << invalidIds[i];
      }
      oss << "]";
      out.error = oss.str();
      return out;
    }
  }

  ids = filterVisualIds(doc, ids);
  if (ids.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error =
      req.ids.empty() ? "camera_focus: no visual objects available" : "camera_focus: ids contain no visual objects";
    return out;
  }

  auto snapInv = snapshotEngineCameraAndBBox(engine, ids, req.afterClipping, "camera_focus:snapshot_engine");
  if (!snapInv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = snapInv.error;
    return out;
  }
  EngineCameraAndBBoxSnapshot snap = std::move(snapInv.value);
  if (!snap.cameraJson.is_object()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_focus: engine camera invalid";
    return out;
  }

  ZBBox<glm::dvec3> bb = snap.bbox;
  if (bb.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_focus: bbox empty";
    return out;
  }

  if (req.minRadius > 0.0) {
    const auto cent = (bb.minCorner + bb.maxCorner) * 0.5;
    bb.expand(ZBBox<glm::dvec3>{cent - glm::dvec3(req.minRadius), cent + glm::dvec3(req.minRadius)});
  }

  Z3DCameraParameter base("Camera");
  base.readValue(snap.cameraJson);

  Z3DCameraPlanner::SolveRequest solveReq;
  solveReq.mode = Z3DCameraPlanner::SolveMode::Fit;
  solveReq.t0 = 0.0;
  solveReq.bbox = bb;

  std::string planErr;
  auto keys = Z3DCameraPlanner::solve(base, solveReq, planErr);
  if (!planErr.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = planErr;
    return out;
  }
  if (keys.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_focus: no camera key produced";
    return out;
  }

  out.values.push_back(std::move(keys.front().value));
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::CameraValuesResult ZRpcUiDispatcher::cameraPointTo(const CameraPointToRequest& req)
{
  CameraValuesResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_point_to: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_point_to: doc not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_point_to: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_point_to: engine not ready";
    return out;
  }

  std::vector<size_t> ids = req.ids.empty() ? doc->objs() : req.ids;
  std::vector<size_t> invalidIds;
  if (!req.ids.empty()) {
    for (auto id : ids) {
      if (!doc->idToDoc(id)) {
        invalidIds.push_back(id);
      }
    }
  }
  if (!invalidIds.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    std::ostringstream oss;
    oss << "camera_point_to: unknown object ids: [";
    for (size_t i = 0; i < invalidIds.size(); ++i) {
      if (i) {
        oss << ", ";
      }
      oss << invalidIds[i];
    }
    oss << "]";
    out.error = oss.str();
    return out;
  }

  ids = filterVisualIds(doc, ids);
  if (ids.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = req.ids.empty() ? "camera_point_to: no visual objects available"
                                : "camera_point_to: ids contain no visual objects";
    return out;
  }

  auto snapInv = snapshotEngineCameraAndBBox(engine, ids, req.afterClipping, "camera_point_to:snapshot_engine");
  if (!snapInv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = snapInv.error;
    return out;
  }
  EngineCameraAndBBoxSnapshot snap = std::move(snapInv.value);
  if (!snap.cameraJson.is_object()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_point_to: engine camera invalid";
    return out;
  }
  if (snap.bbox.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_point_to: bbox empty";
    return out;
  }

  Z3DCameraParameter cam("Camera");
  cam.readValue(snap.cameraJson);
  cam.setCenter(glm::vec3((snap.bbox.minCorner + snap.bbox.maxCorner) * 0.5));

  out.values.push_back(cam.jsonValue());
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::CameraValuesResult ZRpcUiDispatcher::cameraRotate(const CameraRotateRequest& req)
{
  CameraValuesResult out;

  if (!req.baseValueOverride.has_value()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_rotate: base_value is required";
    return out;
  }

  const QString op = QString::fromStdString(req.op).toUpper().trimmed();
  if (op.isEmpty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_rotate: op is required";
    return out;
  }
  if (!(op == "AZIMUTH" || op == "ELEVATION" || op == "ROLL" || op == "YAW" || op == "PITCH" || op == "FLIP")) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_rotate: op must be one of: AZIMUTH, ELEVATION, ROLL, YAW, PITCH, FLIP";
    return out;
  }
  if (op != "FLIP" && !std::isfinite(req.degrees)) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_rotate: degrees must be finite";
    return out;
  }
  if (!req.baseValueOverride->is_object()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_rotate: base_value must be an object";
    return out;
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_rotate: main window not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_rotate: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_rotate: engine not ready";
    return out;
  }

  Z3DCameraParameter cam("Camera");
  cam.readValue(*req.baseValueOverride);

  const float angleRad = glm::radians(static_cast<float>(req.degrees));
  if (op == "AZIMUTH") {
    cam.azimuth(angleRad);
  } else if (op == "ELEVATION") {
    cam.elevation(angleRad);
  } else if (op == "ROLL") {
    cam.roll(angleRad);
  } else if (op == "YAW") {
    cam.yaw(angleRad);
  } else if (op == "PITCH") {
    cam.pitch(angleRad);
  } else if (op == "FLIP") {
    cam.flipViewDirection();
  }

  out.values.push_back(cam.jsonValue());
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::CameraValuesResult ZRpcUiDispatcher::cameraResetView(const CameraResetViewRequest& req)
{
  CameraValuesResult out;

  const QString mode = QString::fromStdString(req.mode).toUpper().trimmed();
  if (mode.isEmpty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_reset_view: mode is required (XY|XZ|YZ|RESET)";
    return out;
  }
  if (!(mode == "XY" || mode == "XZ" || mode == "YZ" || mode == "RESET")) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_reset_view: mode must be one of: XY, XZ, YZ, RESET";
    return out;
  }
  if (!std::isfinite(req.minRadius) || req.minRadius < 0.0) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_reset_view: min_radius must be a finite number >= 0";
    return out;
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_reset_view: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_reset_view: doc not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_reset_view: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_reset_view: engine not ready";
    return out;
  }

  std::vector<size_t> ids = req.ids.empty() ? doc->objs() : req.ids;
  if (!req.ids.empty()) {
    std::vector<size_t> invalidIds;
    for (auto id : ids) {
      if (!doc->idToDoc(id)) {
        invalidIds.push_back(id);
      }
    }
    if (!invalidIds.empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      std::ostringstream oss;
      oss << "camera_reset_view: unknown object ids: [";
      for (size_t i = 0; i < invalidIds.size(); ++i) {
        if (i) {
          oss << ", ";
        }
        oss << invalidIds[i];
      }
      oss << "]";
      out.error = oss.str();
      return out;
    }
  }
  ids = filterVisualIds(doc, ids);
  if (ids.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = req.ids.empty() ? "camera_reset_view: no visual objects available"
                                : "camera_reset_view: ids contain no visual objects";
    return out;
  }

  auto snapInv = snapshotEngineCameraAndBBox(engine, ids, req.afterClipping, "camera_reset_view:snapshot_engine");
  if (!snapInv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = snapInv.error;
    return out;
  }
  EngineCameraAndBBoxSnapshot snap = std::move(snapInv.value);
  if (!snap.cameraJson.is_object()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_reset_view: engine camera invalid";
    return out;
  }

  ZBBox<glm::dvec3> bb = snap.bbox;
  if (bb.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_reset_view: bbox empty";
    return out;
  }

  if (req.minRadius > 0.0 && (mode == "XY" || mode == "RESET")) {
    const auto cent = (bb.minCorner + bb.maxCorner) * 0.5;
    bb.expand(ZBBox<glm::dvec3>{cent - glm::dvec3(req.minRadius), cent + glm::dvec3(req.minRadius)});
  }

  Z3DCameraParameter cam("Camera");
  cam.readValue(snap.cameraJson);

  cam.resetCamera(bb, Z3DCamera::ResetOption::ResetAll);
  if (mode == "XZ") {
    cam.rotate90X();
  } else if (mode == "YZ") {
    cam.rotate90XZ();
  }

  out.values.push_back(cam.jsonValue());
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::CameraValuesResult ZRpcUiDispatcher::cameraMoveLocal(const CameraMoveLocalRequest& req)
{
  CameraValuesResult out;

  if (!req.baseValueOverride.has_value()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_move_local: base_value is required";
    return out;
  }

  const QString op = QString::fromStdString(req.op).toUpper().trimmed();
  if (op.isEmpty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_move_local: op is required";
    return out;
  }
  if (!(op == "FORWARD" || op == "BACK" || op == "RIGHT" || op == "LEFT" || op == "UP" || op == "DOWN")) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_move_local: invalid op (expected FORWARD|BACK|RIGHT|LEFT|UP|DOWN)";
    return out;
  }
  if (!std::isfinite(req.distance)) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_move_local: distance must be finite";
    return out;
  }
  if (!req.baseValueOverride->is_object()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_move_local: base_value must be an object";
    return out;
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_move_local: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_move_local: doc not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_move_local: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_move_local: engine not ready";
    return out;
  }

  std::optional<ZBBox<glm::dvec3>> bboxOpt;
  if (req.distanceIsFractionOfBBoxRadius) {
    std::vector<size_t> ids = req.ids.empty() ? doc->objs() : req.ids;
    if (!req.ids.empty()) {
      std::vector<size_t> invalidIds;
      for (auto id : ids) {
        if (!doc->idToDoc(id)) {
          invalidIds.push_back(id);
        }
      }
      if (!invalidIds.empty()) {
        out.ok = false;
        out.errorKind = ErrorKind::InvalidArgument;
        std::ostringstream oss;
        oss << "camera_move_local: unknown object ids: [";
        for (size_t i = 0; i < invalidIds.size(); ++i) {
          if (i) {
            oss << ", ";
          }
          oss << invalidIds[i];
        }
        oss << "]";
        out.error = oss.str();
        return out;
      }
    }
    ids = filterVisualIds(doc, ids);

    auto bboxInv = snapshotEngineBBox(engine, ids, req.afterClipping, "camera_move_local:bbox");
    if (!bboxInv.ok) {
      out.ok = false;
      out.errorKind = ErrorKind::FailedPrecondition;
      out.error = bboxInv.error;
      return out;
    }
    bboxOpt = std::move(bboxInv.value);
  }

  Z3DCameraParameter cam("Camera");
  cam.readValue(*req.baseValueOverride);

  double distWorld = req.distance;
  if (req.distanceIsFractionOfBBoxRadius) {
    if (!bboxOpt.has_value() || bboxOpt->empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::FailedPrecondition;
      out.error = "camera_move_local: bbox empty";
      return out;
    }
    const double r = Z3DCameraPlanner::bboxEnclosingSphereRadius(*bboxOpt);
    if (!(r > 0.0)) {
      out.ok = false;
      out.errorKind = ErrorKind::FailedPrecondition;
      out.error = "camera_move_local: bbox radius is zero";
      return out;
    }
    distWorld *= r;
  }

  glm::vec3 dir(0.f, 0.f, 0.f);
  if (op == "FORWARD") {
    dir = cam.get().viewVector();
  } else if (op == "BACK") {
    dir = -cam.get().viewVector();
  } else if (op == "RIGHT") {
    dir = cam.get().strafeVector();
  } else if (op == "LEFT") {
    dir = -cam.get().strafeVector();
  } else if (op == "UP") {
    dir = cam.get().upVector();
  } else if (op == "DOWN") {
    dir = -cam.get().upVector();
  }

  const glm::vec3 delta = static_cast<float>(distWorld) * dir;
  glm::vec3 eye = cam.get().eye();
  glm::vec3 center = cam.get().center();
  const glm::vec3 up = cam.get().upVector();
  eye += delta;
  if (req.moveCenter) {
    center += delta;
  }
  cam.setCamera(eye, center, up);

  out.values.push_back(cam.jsonValue());
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::CameraValuesResult ZRpcUiDispatcher::cameraLookAt(const CameraLookAtRequest& req)
{
  CameraValuesResult out;

  if (!req.baseValueOverride.has_value()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_look_at: base_value is required";
    return out;
  }
  if (!req.baseValueOverride->is_object()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_look_at: base_value must be an object";
    return out;
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_look_at: main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_look_at: doc not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_look_at: 3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera_look_at: engine not ready";
    return out;
  }

  std::optional<ZBBox<glm::dvec3>> bboxOpt;
  const bool needsBBox =
    (req.target == CameraLookAtRequest::Target::TargetBBoxCenter ||
     req.target == CameraLookAtRequest::Target::BBoxFractionPoint);
  if (needsBBox) {
    std::vector<size_t> ids = req.ids.empty() ? doc->objs() : req.ids;
    if (!req.ids.empty()) {
      std::vector<size_t> invalidIds;
      for (auto id : ids) {
        if (!doc->idToDoc(id)) {
          invalidIds.push_back(id);
        }
      }
      if (!invalidIds.empty()) {
        out.ok = false;
        out.errorKind = ErrorKind::InvalidArgument;
        std::ostringstream oss;
        oss << "camera_look_at: unknown object ids: [";
        for (size_t i = 0; i < invalidIds.size(); ++i) {
          if (i) {
            oss << ", ";
          }
          oss << invalidIds[i];
        }
        oss << "]";
        out.error = oss.str();
        return out;
      }
    }
    ids = filterVisualIds(doc, ids);

    auto bboxInv = snapshotEngineBBox(engine, ids, req.afterClipping, "camera_look_at:bbox");
    if (!bboxInv.ok) {
      out.ok = false;
      out.errorKind = ErrorKind::FailedPrecondition;
      out.error = bboxInv.error;
      return out;
    }
    bboxOpt = std::move(bboxInv.value);
  }

  Z3DCameraParameter cam("Camera");
  cam.readValue(*req.baseValueOverride);

  glm::vec3 target(0.f, 0.f, 0.f);
  if (req.target == CameraLookAtRequest::Target::WorldPoint) {
    target = req.worldPoint;
  } else if (req.target == CameraLookAtRequest::Target::TargetBBoxCenter) {
    if (!bboxOpt.has_value() || bboxOpt->empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::FailedPrecondition;
      out.error = "camera_look_at: bbox empty";
      return out;
    }
    const glm::dvec3 c = (bboxOpt->minCorner + bboxOpt->maxCorner) * 0.5;
    target = glm::vec3(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z));
  } else if (req.target == CameraLookAtRequest::Target::BBoxFractionPoint) {
    if (!bboxOpt.has_value() || bboxOpt->empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::FailedPrecondition;
      out.error = "camera_look_at: bbox empty";
      return out;
    }
    target = Z3DCameraPlanner::bboxFractionToWorld(*bboxOpt, req.bboxFractionPoint);
  } else {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_look_at: unsupported target";
    return out;
  }

  cam.setCamera(cam.get().eye(), target, cam.get().upVector());
  out.values.push_back(cam.jsonValue());
  out.ok = true;
  return out;
}

ZRpcUiDispatcher::CameraSolveResult ZRpcUiDispatcher::cameraSolve(const CameraSolveRequest& req)
{
  CameraSolveResult out;

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "engine not ready";
    return out;
  }

  std::vector<size_t> ids;
  if (req.ids.empty()) {
    ids = filterVisualIds(doc, doc->objs());
    if (ids.empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      out.error = "camera_solve: no visual objects available (provide ids or load visual objects first)";
      return out;
    }
  } else {
    std::vector<size_t> invalidIds;
    invalidIds.reserve(req.ids.size());
    for (auto id : req.ids) {
      if (!doc->idToDoc(id)) {
        invalidIds.push_back(id);
      }
    }
    if (!invalidIds.empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      std::ostringstream oss;
      oss << "camera_solve: unknown object ids: [";
      for (size_t i = 0; i < invalidIds.size(); ++i) {
        if (i) {
          oss << ", ";
        }
        oss << invalidIds[i];
      }
      oss << "]";
      out.error = oss.str();
      return out;
    }
    ids = filterVisualIds(doc, req.ids);
    if (ids.empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      out.error = "camera_solve: ids contain no visual objects";
      return out;
    }
  }
  auto snapInv = invokeOnObjectThreadWait(
    engine,
    [engine, &ids]() {
      EngineCameraAndBBoxSnapshot s;
      s.cameraJson = engine->camera().jsonValue();
      s.bbox = engine->boundBoxOfObjs(ids);
      return s;
    },
    "camera_solve:snapshot_engine");
  if (!snapInv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = snapInv.error;
    return out;
  }
  const EngineCameraAndBBoxSnapshot& snap = snapInv.value;
  if (!snap.cameraJson.is_object()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "engine camera invalid";
    return out;
  }

  Z3DCameraParameter base("Camera");
  base.readValue(snap.cameraJson);

  Z3DCameraPlanner::SolveRequest solveReq;
  solveReq.t0 = req.t0;
  solveReq.t1 = req.t1;
  solveReq.bbox = snap.bbox;
  solveReq.margin = req.margin;

  const auto mode = QString::fromStdString(req.mode).toUpper();
  if (mode == "FIT") {
    solveReq.mode = Z3DCameraPlanner::SolveMode::Fit;
  } else if (mode == "STATIC") {
    solveReq.mode = Z3DCameraPlanner::SolveMode::Static;
  } else if (mode == "ORBIT") {
    solveReq.mode = Z3DCameraPlanner::SolveMode::Orbit;
    solveReq.orbit.degrees = req.orbitDegrees;
    solveReq.orbit.maxStepDegrees = req.orbitMaxStepDegrees;
    const auto axq = QString::fromStdString(req.orbitAxis).toLower().trimmed();
    if (axq == "x") {
      solveReq.orbit.axis = 'x';
    } else if (axq == "y") {
      solveReq.orbit.axis = 'y';
    } else if (axq == "z") {
      solveReq.orbit.axis = 'z';
    } else {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      out.error = "camera_solve: orbit_axis must be one of: x, y, z";
      return out;
    }
  } else if (mode == "DOLLY") {
    solveReq.mode = Z3DCameraPlanner::SolveMode::Dolly;
    solveReq.dolly.startDist = req.dollyStartDist;
    solveReq.dolly.endDist = req.dollyEndDist;
  } else {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "camera_solve: mode must be one of: FIT, STATIC, ORBIT, DOLLY";
    return out;
  }

  std::string planErr;
  out.keys = Z3DCameraPlanner::solve(base, solveReq, planErr);
  if (!planErr.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = planErr;
    out.keys.clear();
    return out;
  }

  out.ok = true;
  return out;
}

ZRpcUiDispatcher::CameraPathSolveResult ZRpcUiDispatcher::cameraPathSolve(const CameraPathSolveRequest& req)
{
  CameraPathSolveResult out;

  if (req.waypoints.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "waypoints must be non-empty";
    return out;
  }
  if (!req.baseValueOverride.has_value()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "base_value is required";
    return out;
  }
  if (!req.baseValueOverride->is_object()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "base_value must be an object";
    return out;
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "engine not ready";
    return out;
  }

  Z3DCameraParameter base("Camera");
  base.readValue(*req.baseValueOverride);

  bool needBBox = false;
  for (const auto& w : req.waypoints) {
    if (w.eyeMode == Z3DCameraPlannerPathWaypoint::EyeMode::BBoxFraction ||
        w.lookAtMode == Z3DCameraPlannerPathWaypoint::LookAtMode::BBoxCenter ||
        w.lookAtMode == Z3DCameraPlannerPathWaypoint::LookAtMode::BBoxFraction) {
      needBBox = true;
      break;
    }
  }

  std::optional<ZBBox<glm::dvec3>> bbox;
  if (needBBox) {
    std::vector<size_t> ids = req.ids;
    if (ids.empty()) {
      if (doc) {
        ids = filterVisualIds(doc, doc->objs());
      }
    } else {
      ids = filterVisualIds(doc, ids);
    }

    auto bbInv = snapshotEngineBBox(engine, ids, req.afterClipping, "camera_path_solve:bbox");
    if (!bbInv.ok) {
      out.ok = false;
      out.errorKind = ErrorKind::FailedPrecondition;
      out.error = bbInv.error;
      return out;
    }
    bbox = std::move(bbInv.value);
    if (bbox->empty()) {
      out.ok = false;
      out.errorKind = ErrorKind::FailedPrecondition;
      out.error = "bbox empty";
      return out;
    }
  }

  std::string planErr;
  out.keys = Z3DCameraPlanner::solvePath(base, req.waypoints, bbox, planErr);
  if (!planErr.empty()) {
    out.ok = false;
    out.errorKind =
      planErr.rfind("bbox required", 0) == 0 ? ErrorKind::FailedPrecondition : ErrorKind::InvalidArgument;
    out.error = planErr;
    out.keys.clear();
    return out;
  }

  out.ok = true;
  return out;
}

ZRpcUiDispatcher::CameraValidateResult ZRpcUiDispatcher::cameraValidate(const CameraValidateRequest& req)
{
  CameraValidateResult out;

  if (req.times.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "times must be non-empty";
    return out;
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();

  auto* w3d = mainWin->get3DWindow();
  if (!w3d) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "3D window not ready";
    return out;
  }
  auto* engine = w3d->engine();
  if (!engine || !engine->thread() || !engine->thread()->isRunning() || engine->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "engine not ready";
    return out;
  }

  if (req.values.size() < req.times.size() && req.animationId == 0) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "animation_id is required when values are omitted (values_size < times_size)";
    return out;
  }

  if (req.animationId != 0) {
    if (!doc) {
      out.ok = false;
      out.errorKind = ErrorKind::FailedPrecondition;
      out.error = "doc not ready";
      return out;
    }
    auto& ad = doc->animation3DDoc();
    const size_t animId = static_cast<size_t>(req.animationId);
    if (!ad.animationPtr(animId)) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      out.error = "animation_id not found";
      return out;
    }
  }

  // Resolve ids: empty => all visual objects.
  std::vector<size_t> ids = req.ids;
  if (ids.empty()) {
    if (doc) {
      ids = filterVisualIds(doc, doc->objs());
    }
  } else {
    ids = filterVisualIds(doc, ids);
  }

  auto snapInv = invokeOnObjectThreadWait(
    engine,
    [engine, &ids, after = req.afterClipping]() {
      EngineCameraAndBBoxSnapshot s;
      s.cameraJson = engine->camera().jsonValue();
      s.bbox = after ? engine->boundBoxOfObjsAfterClipping(ids) : engine->boundBoxOfObjs(ids);
      return s;
    },
    "camera_validate:snapshot_engine");
  if (!snapInv.ok) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = snapInv.error;
    return out;
  }
  EngineCameraAndBBoxSnapshot snap = std::move(snapInv.value);
  if (!snap.cameraJson.is_object()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "engine camera invalid";
    return out;
  }

  Z3DCameraParameter base("Camera");
  base.readValue(snap.cameraJson);

  std::vector<double> times = req.times;
  std::vector<json::value> values = req.values;

  const size_t provided = values.size();
  if (provided < times.size()) {
    std::vector<json::value> sampled;
    sampled.reserve(times.size() - provided);

    // Default to the current engine camera when no animation/doc is available.
    if (!doc || req.animationId == 0) {
      for (size_t i = provided; i < times.size(); ++i) {
        sampled.push_back(snap.cameraJson);
      }
    } else {
      auto& ad = doc->animation3DDoc();
      const size_t animId = static_cast<size_t>(req.animationId);
      auto* anim = ad.animationPtr(animId);
      if (!anim) {
        for (size_t i = provided; i < times.size(); ++i) {
          sampled.push_back(snap.cameraJson);
        }
      } else {
        ZCameraParameterAnimation* cpa = anim->cameraParameterAnimation();
        if (!cpa) {
          for (size_t i = provided; i < times.size(); ++i) {
            sampled.push_back(snap.cameraJson);
          }
        } else {
          for (size_t i = provided; i < times.size(); ++i) {
            Z3DCameraParameter tmp("Camera");
            tmp.readValue(snap.cameraJson);
            cpa->updateParaToTime(times[i], &tmp);
            sampled.push_back(tmp.jsonValue());
          }
        }
      }
    }

    values.insert(values.end(), sampled.begin(), sampled.end());
  }

  Z3DCameraPlanner::ValidatePolicies policies;
  policies.adjustFov = req.adjustFov;
  policies.adjustDistance = req.adjustDistance;

  Z3DCameraPlanner::ValidateConstraints constraints;
  constraints.keepVisible = req.keepVisible;
  constraints.minCoverage = req.minCoverage;
  constraints.margin = req.margin;

  std::string planErr;
  out.results = Z3DCameraPlanner::validate(base, snap.bbox, times, values, constraints, policies, planErr);
  if (!planErr.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = planErr;
    out.results.clear();
    return out;
  }

  bool allOk = true;
  const bool keepVisible = req.keepVisible;
  const double minCov = keepVisible ? (req.minCoverage > 0.0 ? req.minCoverage : 0.95) : 0.0;
  for (const auto& r : out.results) {
    if (!r.withinFrame || (keepVisible && (r.coverage + 1e-6) < minCov)) {
      allOk = false;
      break;
    }
  }

  out.ok = true;
  out.allOk = allOk;
  return out;
}

ZRpcUiDispatcher::CameraSampleResult ZRpcUiDispatcher::cameraSample(const CameraSampleRequest& req)
{
  CameraSampleResult out;

  if (req.times.empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "times must be non-empty";
    return out;
  }
  if (req.animationId == 0) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "animation_id is required";
    return out;
  }
  for (double t : req.times) {
    if (!std::isfinite(t) || t < 0.0) {
      out.ok = false;
      out.errorKind = ErrorKind::InvalidArgument;
      out.error = "times must be finite and >= 0";
      return out;
    }
  }

  ZMainWindow* mainWin = mainWindowUi();
  if (!mainWin) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "main window not ready";
    return out;
  }
  ZDoc* doc = mainWin->doc();
  if (!doc) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "doc not ready";
    return out;
  }

  auto& ad = doc->animation3DDoc();
  const size_t animId = static_cast<size_t>(req.animationId);
  auto* anim = ad.animationPtr(animId);
  if (!anim) {
    out.ok = false;
    out.errorKind = ErrorKind::InvalidArgument;
    out.error = "animation_id not found";
    return out;
  }

  ZCameraParameterAnimation* cpa = anim->cameraParameterAnimation();
  if (!cpa) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera track not ready";
    return out;
  }
  if (cpa->keys().empty()) {
    out.ok = false;
    out.errorKind = ErrorKind::FailedPrecondition;
    out.error = "camera track has no keys";
    return out;
  }

  // Seed the temporary camera from an existing key to ensure the parameter is fully initialized.
  const json::value seed = cpa->keys().front()->value().jsonValue();

  out.samples.reserve(req.times.size());
  for (double t : req.times) {
    Z3DCameraParameter tmp("Camera");
    tmp.readValue(seed);
    cpa->updateParaToTime(t, &tmp);

    Z3DCameraPlannerSolveKey k;
    k.time = t;
    k.value = tmp.jsonValue();
    out.samples.push_back(std::move(k));
  }

  out.ok = true;
  return out;
}

} // namespace nim
