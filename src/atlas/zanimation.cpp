#include "zanimation.h"

#include "zvideoencoder.h"
#include "zdoc.h"
#include "zobjdoc.h"
#include "zparameteranimation.h"
#include "zcameraparameterkey.h"
#include "zbenchtimer.h"
#include "zexception.h"
#include "zserializationutils.h"
#include "zview.h"
#include "z3drenderingengine.h"
#include "zgraphicsview.h"
#include <QLabel>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include "zmessageboxhelpers.h"
#include <QApplication>
#include <QDir>
#include <QProgressDialog>
#include <QThread>
#include <utility>
#include "zlog.h"

DEFINE_string(output_image_name_prefix, "video", "Prefix for naming output images. Default is video");

DEFINE_int32(output_image_name_field_width,
             8,
             "Length of the numeric part in output image names following the prefix. Default: 8");

namespace {
// generic solution
template<class T>
int numDigits(T number)
{
  int digits = 0;
  if (number < 0) {
    digits = 1;
  } // remove this line if '-' counts as a digit
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
  if (x == std::numeric_limits<int>::min()) {
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

namespace {

struct ParsedJsonKey
{
  bool ok = false;
  QString name;
  QString type;
};

[[nodiscard]] ParsedJsonKey parseJsonKey(const QString& jsonKey)
{
  ParsedJsonKey out;
  const int spaceIdx = jsonKey.lastIndexOf(QChar(' '));
  if (spaceIdx <= 0 || spaceIdx + 1 >= jsonKey.size()) {
    return out;
  }
  out.name = jsonKey.left(spaceIdx);
  out.type = jsonKey.mid(spaceIdx + 1);
  if (out.name.trimmed().isEmpty() || out.type.trimmed().isEmpty()) {
    return out;
  }
  out.ok = true;
  return out;
}

} // namespace

namespace {

static constexpr const char* kUndoDuration = "duration";
static constexpr const char* kUndoNextUniqueId = "next_unique_id";
static constexpr const char* kUndoGlobalTracks = "global_tracks";
static constexpr const char* kUndoObjects = "objects";

static constexpr const char* kUndoObjType = "obj_type";
static constexpr const char* kUndoObjJsonValue = "obj_json";
static constexpr const char* kUndoObjExpanded = "expanded";
static constexpr const char* kUndoObjShowAll = "show_all";
static constexpr const char* kUndoObjBoundId = "bound_id";
static constexpr const char* kUndoObjUniqueId = "unique_id";
static constexpr const char* kUndoObjTracks = "tracks";

static constexpr const char* kUndoTrackColor = "color";
static constexpr const char* kUndoTrackKeys = "keys";

[[nodiscard]] json::object snapshotTrackValue(const ZParameterAnimation& pa)
{
  json::object obj;
  obj[kUndoTrackColor] = json::value_from(pa.color());

  json::array keys;
  keys.reserve(pa.keys().size());
  for (const auto& k : pa.keys()) {
    keys.push_back(k->jsonValue());
  }
  obj[kUndoTrackKeys] = std::move(keys);
  return obj;
}

[[nodiscard]] bool restoreTrackFromSnapshot(ZParameterAnimation& pa, const json::value& v, QString* error)
{
  if (!v.is_object()) {
    if (error) {
      *error = "track snapshot is not an object";
    }
    return false;
  }

  const auto& obj = v.as_object();

  if (auto it = obj.find(kUndoTrackColor); it != obj.end()) {
    QColor color(0, 0, 0, 255);
    const json::value& cv = it->value();
    try {
      color = json::value_to<QColor>(cv);
    }
    catch (...) {
      if (cv.is_string()) {
        toVal(cv.get_string(), color);
      } else {
        if (error) {
          *error = "invalid track color";
        }
        return false;
      }
    }
    pa.setColor(color);
  }

  std::vector<std::unique_ptr<ZParameterKey>> keys;
  if (auto it = obj.find(kUndoTrackKeys); it != obj.end()) {
    if (!it->value().is_array()) {
      if (error) {
        *error = "track keys must be an array";
      }
      return false;
    }
    const auto& arr = it->value().as_array();
    keys.reserve(arr.size());
    for (const auto& kv : arr) {
      if (pa.type() == "3DCamera") {
        auto ck = std::make_unique<ZCameraParameterKey>();
        if (!ck->readValue(kv)) {
          if (error) {
            *error = "failed to read 3DCamera key";
          }
          return false;
        }
        keys.push_back(std::unique_ptr<ZParameterKey>(std::move(ck)));
      } else {
        auto pk = std::make_unique<ZParameterKey>(pa.type());
        if (!pk->readValue(kv)) {
          if (error) {
            *error = QString("failed to read key for type %1").arg(pa.type());
          }
          return false;
        }
        keys.push_back(std::move(pk));
      }
    }
  }

  pa.replaceKeys(std::move(keys));
  return true;
}

class ZAnimationSnapshotCommand final : public QUndoCommand
{
public:
  ZAnimationSnapshotCommand(ZAnimation* animation, QString text, ZAnimation::UndoSnapshot snapshot)
    : QUndoCommand(std::move(text))
    , m_animation(animation)
    , m_snapshot(std::move(snapshot))
  {
    CHECK(m_animation);
  }

  void undo() override
  {
    swapSnapshot();
  }

  void redo() override
  {
    if (m_firstRedo) {
      m_firstRedo = false;
      return;
    }
    swapSnapshot();
  }

private:
  void swapSnapshot()
  {
    CHECK(m_animation);
    ZAnimation::UndoSnapshot current = m_animation->captureUndoSnapshot();
    m_animation->restoreFromUndoSnapshot(m_snapshot);
    m_snapshot = std::move(current);
  }

  ZAnimation* m_animation = nullptr; // non-owning
  ZAnimation::UndoSnapshot m_snapshot;
  bool m_firstRedo = true;
};

} // namespace

void ZAnimationChangeDurationCommand::undo()
{
  m_ani->setDurationImpl(m_oldDuration);
}

void ZAnimationChangeDurationCommand::redo()
{
  m_ani->setDurationImpl(m_newDuration);
}

ZAnimation::ZAnimation(ZDoc& doc, QObject* parent)
  : QObject(parent)
  , m_doc(doc)
  , m_engine(nullptr)
  , m_duration(10.0)
  , m_nextUniqueId(100)
{
  connect(&m_doc, &ZDoc::objAboutToBeRemoved, this, &ZAnimation::disableAnimationOf);
  m_videoEncoder = new ZVideoEncoder(this);
  connect(m_videoEncoder, &ZVideoEncoder::error, this, &ZAnimation::videoEncoderError);
  connect(m_videoEncoder, &ZVideoEncoder::finished, this, &ZAnimation::videoEncoderFinished);
  connect(m_videoEncoder, &ZVideoEncoder::canceled, this, &ZAnimation::videoEncoderCanceled);
}

ZAnimation::~ZAnimation()
{
  releaseParameters();
}

void ZAnimation::addKeyFrame(double time)
{
  CHECK(time >= 0.0);
  CHECK(m_engine);

  UndoSnapshot before = captureUndoSnapshot();

  bool objChange = false;
  bool sorted = false;

  {
    const QSignalBlocker blocker(this);

    addGlobalKey(time);

    auto objs = m_doc.objs();
    if (!is2DAnimation()) {
      objs.push_back(1); // background
      objs.push_back(2); // axis
      objs.push_back(3); // lighting
    }
    for (auto id : objs) {
      // VLOG(2) << "Processing obj id " << id;
      QString objTypeName;
      json::value objJsonValue;
      if (id == 1) {
        objTypeName = "Background";
      } else if (id == 2) {
        objTypeName = "Axis";
      } else if (id == 3) {
        objTypeName = "Lighting";
      } else {
        ZObjDoc* objDoc = m_doc.idToDoc(id);
        objTypeName = objDoc->typeName();
        objJsonValue = objDoc->jsonValue(id);
      }
      if (objTypeName.contains("Animation", Qt::CaseInsensitive)) {
        continue;
      }
      std::shared_ptr<ZWidgetsGroup> wg = m_engine->viewSettingWidgetsGroupOf(id);
      CHECK(wg) << "Can not find view setting widgets group for obj id " << id;
      const std::vector<ZParameter*>& paraList = wg->getParameterList();

      AnimationObj* aniObj = findBoundId(id);
      if (!aniObj) {
        auto aO = std::make_unique<AnimationObj>(objTypeName, objJsonValue);
        aO->uniqueId = m_nextUniqueId++;
        aO->boundId = id;
        objChange = true;
        m_objList.push_back(std::move(aO));
        aniObj = m_objList.back().get();
      }
      auto& paraAnimationList = aniObj->objParaAnimations;

      for (size_t i = 0; i < paraList.size(); ++i) {
        bool found = false;
        // VLOG(2) << paraList[i]->name();
        for (size_t j = 0; j < paraAnimationList.size(); ++j) {
          if (paraList[i] == paraAnimationList[j]->boundParameter()) {
            found = true;
            // VLOG(2) << "Adding key to existing parameter animation " << paraList[i]->name();
            paraAnimationList[j]->addKey(std::make_unique<ZParameterKey>(time, *paraList[i]), false);
            if (j != i) {
              std::swap(paraAnimationList[i], paraAnimationList[j]);
              sorted = true;
            }
            break;
          }
        }
        if (!found) {
          // VLOG(2) << "Adding new parameter animation for " << paraList[i]->name();
          objChange = true;
          auto paraAnimation = std::make_unique<ZParameterAnimation>(paraList[i]->name(), paraList[i]->type());
          paraAnimation->setParent(this);
          paraAnimation->bindParameter(*paraList[i]);
          paraAnimation->addKey(std::make_unique<ZParameterKey>(time, *paraList[i]), false);
          paraAnimationList.insert(paraAnimationList.begin() + i, std::move(paraAnimation));
        }
        // VLOG(2) << "Finished parameter " << paraList[i]->name();
      }
      // VLOG(2) << "Finished obj id " << id;
    }
  }
  // VLOG(2) << "Added all keys at time " << time;

  if (objChange) {
    updateObjAnimation();
  } else if (sorted) {
    buildDisplayPacks();
    Q_EMIT keysChanged();
  } else {
    Q_EMIT keysChanged();
  }
  pushUndoSnapshotCommand("Add Key Frame", std::move(before));
  LOG(INFO) << "Added key frame at time " << time;
}

void ZAnimation::setExpanded(size_t id, bool v)
{
  AnimationObj* aniObj = findUniqueId(id);
  if (aniObj && aniObj->isExpanded != v && aniObj->boundId != 0) {
    aniObj->isExpanded = v;
    buildDisplayPacks();
    Q_EMIT expandChanged();
  }
}

void ZAnimation::toogleExpanded(size_t id)
{
  AnimationObj* aniObj = findUniqueId(id);
  if (aniObj && aniObj->boundId != 0) {
    aniObj->isExpanded = !aniObj->isExpanded;
    buildDisplayPacks();
    Q_EMIT expandChanged();
  }
}

void ZAnimation::toogleShowAll(size_t id)
{
  AnimationObj* aniObj = findUniqueId(id);
  if (aniObj && aniObj->boundId != 0) {
    aniObj->isShowAll = !aniObj->isShowAll;
    buildDisplayPacks();
    Q_EMIT expandChanged();
  }
}

void ZAnimation::setDuration(double duration)
{
  duration = std::max(1.0, duration);
  if (m_duration != duration) {
    m_undoStack.push(new ZAnimationChangeDurationCommand(this, m_duration, duration));
  }
}

void ZAnimation::setCurrentTime(double time) const
{
  time = std::max(0.0, time);
  m_currentTime = time;

  // Special-case: apply the render-backend track early (if present).
  //
  // 3D animations store the "Render Backend" parameter under the "Lighting"
  // view-setting group (boundId=3) rather than as a global track. If this track
  // is applied late, we can end up doing a large amount of OpenGL-only setup
  // work (shader/program compilation, GL resource staging, etc.) only to switch
  // to Vulkan afterwards during export. Applying it first ensures subsequent
  // per-object updates observe the final backend.
  ZParameterAnimation* renderBackendAnimation = nullptr;
  for (const auto& obj : m_objList) {
    if (obj->boundId != 3) {
      continue;
    }
    for (const auto& paPtr : obj->objParaAnimations) {
      if (!paPtr) {
        continue;
      }
      if (paPtr->name() == "Render Backend" && paPtr->type() == "StringIntOption") {
        renderBackendAnimation = paPtr.get();
        break;
      }
    }
    if (renderBackendAnimation) {
      break;
    }
  }

  // Apply global parameters first so expensive object-side effects (e.g.
  // renderer backend switch) happen before per-object track application.
  //
  // This reduces wasted work when the first time application (time=0) triggers
  // a global backend change (OpenGL -> Vulkan) during headless export: object
  // updates should observe the final backend and avoid doing OpenGL-only setup
  // that will be immediately discarded.
  const auto applyGlobals = [&]() {
    for (const auto& pa : m_globalParaAnimations) {
      pa->setCurrentTime(time);
    }
  };
  const auto applyObjects = [&]() {
    for (const auto& obj : m_objList) {
      if (obj->boundId == 0) {
        continue;
      }
      for (const auto& pa : obj->objParaAnimations) {
        if (pa.get() == renderBackendAnimation) {
          continue;
        }
        pa->setCurrentTime(time);
      }
    }
  };

  if (time == 0.0 && VLOG_IS_ON(1)) {
    ZBenchTimer bt("ZAnimation::setCurrentTime(0)");
    if (renderBackendAnimation) {
      renderBackendAnimation->setCurrentTime(time);
    }
    bt.recordEvent("renderBackend");
    applyGlobals();
    bt.recordEvent("globals");
    applyObjects();
    bt.recordEvent("objects");
    bt.stop();
    VLOG(1) << bt;
    return;
  }

  if (renderBackendAnimation) {
    renderBackendAnimation->setCurrentTime(time);
  }
  applyGlobals();
  applyObjects();
}

void ZAnimation::cancelRenderingAndSetCurrentTime(double time) const
{
  if (auto eng = dynamic_cast<Z3DRenderingEngine*>(m_engine); eng) {
    eng->cancelLongRendering();
  }

  setCurrentTime(time);
}

void ZAnimation::removeObj(size_t id)
{
  size_t idx;
  AnimationObj* aniObj = findUniqueId(id, &idx);
  if (aniObj) {
    UndoSnapshot before = captureUndoSnapshot();
    m_objList.erase(m_objList.begin() + idx);
    buildDisplayPacks();
    Q_EMIT objChanged();
    pushUndoSnapshotCommand("Remove Object From Animation", std::move(before));
  }
}

void ZAnimation::removeRedundantKeys()
{
  {
    const QSignalBlocker blocker(this);
    for (const auto& pa : m_globalParaAnimations) {
      pa->removeRedundantKeys();
    }
    for (const auto& obj : m_objList) {
      for (const auto& pa : obj->objParaAnimations) {
        pa->removeRedundantKeys();
      }
    }
  }
  Q_EMIT keysChanged();
}

void ZAnimation::removeRedundantKeysUndoable()
{
  UndoSnapshot before = captureUndoSnapshot();
  removeRedundantKeys();
  pushUndoSnapshotCommand("Remove Redundant Keys", std::move(before));
}

ZParameterAnimation* ZAnimation::parameterAnimationForBoundId(size_t boundId, const QString& jsonKey)
{
  auto* obj = findBoundId(boundId);
  if (!obj) {
    return nullptr;
  }
  for (const auto& paPtr : obj->objParaAnimations) {
    if (paPtr && paPtr->jsonKey() == jsonKey) {
      return paPtr.get();
    }
  }
  return nullptr;
}

const ZParameterAnimation* ZAnimation::parameterAnimationForBoundId(size_t boundId, const QString& jsonKey) const
{
  auto* obj = findBoundId(boundId);
  if (!obj) {
    return nullptr;
  }
  for (const auto& paPtr : obj->objParaAnimations) {
    if (paPtr && paPtr->jsonKey() == jsonKey) {
      return paPtr.get();
    }
  }
  return nullptr;
}

std::vector<ZParameterAnimation*> ZAnimation::parameterAnimationsForBoundId(size_t boundId)
{
  std::vector<ZParameterAnimation*> out;
  auto* obj = findBoundId(boundId);
  if (!obj) {
    return out;
  }
  out.reserve(obj->objParaAnimations.size());
  for (const auto& paPtr : obj->objParaAnimations) {
    if (paPtr) {
      out.push_back(paPtr.get());
    }
  }
  return out;
}

ZAnimation::EnsureParameterAnimationResult ZAnimation::ensureParameterAnimationForBoundId(size_t boundId,
                                                                                          const QString& jsonKey)
{
  EnsureParameterAnimationResult out;

  const ParsedJsonKey parsed = parseJsonKey(jsonKey);
  if (!parsed.ok) {
    out.error = QString("invalid json_key '%1'").arg(jsonKey);
    return out;
  }

  AnimationObj* aniObj = findBoundId(boundId);
  if (!aniObj) {
    QString objTypeName;
    json::value objJsonValue;
    if (boundId == 1) {
      objTypeName = "Background";
    } else if (boundId == 2) {
      objTypeName = "Axis";
    } else if (boundId == 3) {
      objTypeName = "Lighting";
    } else {
      ZObjDoc* objDoc = m_doc.idToDoc(boundId);
      if (!objDoc) {
        out.error = QString("target_id %1 not found in the current document").arg(boundId);
        return out;
      }
      objTypeName = objDoc->typeName();
      objJsonValue = objDoc->jsonValue(boundId);
      if (objTypeName.contains("Animation", Qt::CaseInsensitive)) {
        out.error = QString("target_id %1 is not a visual object").arg(boundId);
        return out;
      }
    }

    auto aO = std::make_unique<AnimationObj>(objTypeName, std::move(objJsonValue));
    aO->uniqueId = m_nextUniqueId++;
    aO->boundId = boundId;
    m_objList.push_back(std::move(aO));
    aniObj = m_objList.back().get();
    out.createdObject = true;
  }

  for (const auto& paPtr : aniObj->objParaAnimations) {
    if (paPtr && paPtr->jsonKey() == jsonKey) {
      out.animation = paPtr.get();
      return out;
    }
  }

  auto pa = std::make_unique<ZParameterAnimation>(parsed.name, parsed.type);
  pa->setParent(this);
  out.animation = pa.get();
  aniObj->objParaAnimations.push_back(std::move(pa));
  out.createdParameter = true;

  // New tracks (and possibly a new animated object) must be reflected in the
  // display packs so timeline UI and other listeners stay consistent.
  updateObjAnimation();

  return out;
}

void ZAnimation::rebindView()
{
  releaseParameters();
  if (!m_engine) {
    return;
  }

  bindGlobalParameters();

  bool sorted = false;

  if (auto engineObj = dynamic_cast<Z3DRenderingEngine*>(m_engine)) {
    connect(engineObj,
            &Z3DRenderingEngine::viewSettingWidgetsGroupChanged,
            this,
            &ZAnimation::rebindView,
            Qt::UniqueConnection);
  }

  for (const auto& obj : m_objList) {
    size_t id = obj->boundId;
    if (id == 0) {
      continue;
    }
    std::vector<ZParameter*> params;
    if (auto engObj = dynamic_cast<Z3DRenderingEngine*>(m_engine)) {
      if (engObj->thread() == QThread::currentThread()) {
        params = engObj->parametersOfViewSetting(id);
      } else {
        QMetaObject::invokeMethod(
          engObj,
          [&params, engObj, id]() {
            params = engObj->parametersOfViewSetting(id);
          },
          Qt::BlockingQueuedConnection);
      }
    }
    sorted = bind(obj->objParaAnimations, params) || sorted;
  }

  if (sorted) {
    buildDisplayPacks();
    Q_EMIT objViewChanged();
  }

  LOG(INFO) << "3D animation parameters bound";
}

void ZAnimation::releaseView()
{
  if (m_engine) {
    releaseParameters();
    m_engine = nullptr;
  }
}

void ZAnimation::exportFixedSize3DAnimation(const QString& fn,
                                            int framePerSecond,
                                            int startFrame,
                                            int endFrame,
                                            int width,
                                            int height,
                                            Z3DScreenShotType sst,
                                            int tileSize,
                                            int tileBorder)
{
  if (!m_engine) {
    QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(), QString("View not ready"));
    return;
  }
  auto engine = dynamic_cast<Z3DRenderingEngine*>(m_engine);
  CHECK(engine);

  QDir dir(QFileInfo(fn).absolutePath());
  if (!dir.exists()) {
    if (!dir.mkpath(".")) {
      QMessageBox::critical(QApplication::activeWindow(),
                            QApplication::applicationName(),
                            QString("Can not create folder %1").arg(dir.path()));
      return;
    }
  }
  if (dir.exists(fn)) {
    QMessageBox msgBox(QApplication::activeWindow());
    msgBox.setText(tr("File %1 exists, overwrite?").arg(fn));
    msgBox.setInformativeText("");
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Cancel);
    int ret = msgBox.exec();

    if (ret == QMessageBox::Cancel) {
      return;
    }
    if (!QFile::remove(dir.filePath(fn))) {
      QMessageBox::critical(QApplication::activeWindow(),
                            QApplication::applicationName(),
                            QString("Can not replace %1").arg(dir.filePath(fn)));
      return;
    }
  }

  // todo: remove these and correctly handle these in Z3DRenderingEngine
  m_doc.hideAnimation3DView();
  m_doc.deselectAllObjs();

  QString title = "Exporting 3D Animation as Video...";
  if (sst == Z3DScreenShotType::HalfSideBySideStereoView) {
    title = "Exporting 3D Animation as Half Side-By-Side Stereo Video...";
  } else if (sst == Z3DScreenShotType::FullSideBySideStereoView) {
    title = "Exporting 3D Animation as Full Side-By-Side Stereo Video...";
  }

  auto progress = new QProgressDialog(title, "Cancel", 0, 100, QApplication::activeWindow());
  progress->setAutoReset(false);
  progress->setWindowModality(Qt::WindowModal);
  progress->setAttribute(Qt::WA_DeleteOnClose);
  QObject::disconnect(progress, &QProgressDialog::canceled, progress, &QProgressDialog::cancel);
  connect(engine, &Z3DRenderingEngine::progressChanged, progress, &QProgressDialog::setValue);
  connect(engine, &Z3DRenderingEngine::renderingError, progress, &QProgressDialog::reset);
  connect(engine, &Z3DRenderingEngine::videoEncoderFinished, progress, &QProgressDialog::reset);
  connect(engine, &Z3DRenderingEngine::videoEncoderFinished, this, &ZAnimation::videoEncoderFinished);
  connect(progress, &QProgressDialog::canceled, this, &ZAnimation::cancelButtonPressed);

  QObject::disconnect(this,
                      &ZAnimation::exportFixedSize3DAnimationInEngine,
                      engine,
                      &Z3DRenderingEngine::exportFixedSize3DAnimation);
  connect(this,
          &ZAnimation::exportFixedSize3DAnimationInEngine,
          engine,
          &Z3DRenderingEngine::exportFixedSize3DAnimation);

  m_cancelFlag = false;
  Q_EMIT exportFixedSize3DAnimationInEngine(this,
                                            fn,
                                            framePerSecond,
                                            startFrame,
                                            endFrame,
                                            width,
                                            height,
                                            true,
                                            sst,
                                            &m_cancelFlag,
                                            nullptr,
                                            false,
                                            tileSize,
                                            tileBorder);

  progress->exec();
}

void ZAnimation::export3DAnimation(const QString& fn,
                                   int framePerSecond,
                                   int startFrame,
                                   int endFrame,
                                   Z3DScreenShotType sst)
{
  if (!m_engine) {
    QMessageBox::critical(QApplication::activeWindow(), QApplication::applicationName(), QString("View not ready"));
    return;
  }
  auto engine = dynamic_cast<Z3DRenderingEngine*>(m_engine);
  CHECK(engine);

  auto engineOutputSize = engine->outputSize();
  exportFixedSize3DAnimation(fn,
                             framePerSecond,
                             startFrame,
                             endFrame,
                             engineOutputSize.x,
                             engineOutputSize.y,
                             sst,
                             0,
                             0);
}

void ZAnimation::exportFixedSize2DAnimation(const QString& fn,
                                            int framePerSecond,
                                            int startFrame,
                                            int endFrame,
                                            int width,
                                            int height)
{
  int totalNumFrames = std::max(1, static_cast<int>(std::ceil(m_duration * framePerSecond)));
  if (startFrame < 0 || startFrame >= totalNumFrames) {
    QMessageBox::critical(QApplication::activeWindow(),
                          QApplication::applicationName(),
                          QString("Video start frame %1 is not correct").arg(startFrame));
  }
  if (endFrame >= 0 && endFrame <= startFrame) {
    QMessageBox::critical(QApplication::activeWindow(),
                          QApplication::applicationName(),
                          QString("Video end frame %1 is not correct").arg(endFrame));
  }
  if (endFrame < 0 || endFrame > totalNumFrames) {
    endFrame = totalNumFrames;
  }
  CHECK(m_engine);
  QDir dir(QFileInfo(fn).absolutePath());
  if (!dir.exists()) {
    if (!dir.mkpath(".")) {
      QMessageBox::critical(QApplication::activeWindow(),
                            QApplication::applicationName(),
                            QString("Can not create folder %1").arg(dir.path()));
      return;
    }
  }
  if (dir.exists(fn)) {
    QMessageBox msgBox(QApplication::activeWindow());
    msgBox.setText(tr("File %1 exists, overwrite?").arg(fn));
    msgBox.setInformativeText("");
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Cancel);
    int ret = msgBox.exec();

    if (ret == QMessageBox::Cancel) {
      return;
    }
    if (!QFile::remove(dir.filePath(fn))) {
      QMessageBox::critical(QApplication::activeWindow(),
                            QApplication::applicationName(),
                            QString("Can not replace %1").arg(dir.filePath(fn)));
      return;
    }
  }
  if (width % 2 == 1) {
    --width;
  }
  if (height % 2 == 1) {
    --height;
  }
  ZGraphicsView& canvasPainter = static_cast<ZView*>(m_engine)->graphicsView();

  QString title = "Exporting 2D Animation As Images...";
  auto progress = new QProgressDialog(title, "Cancel", 0, endFrame - startFrame, QApplication::activeWindow());
  progress->setWindowModality(Qt::WindowModal);
  progress->setAttribute(Qt::WA_DeleteOnClose);
  progress->show();
  int fieldWidth = std::max(FLAGS_output_image_name_field_width, numDigits(totalNumFrames - 1));
  QString namePrefix = QString::fromStdString(FLAGS_output_image_name_prefix);
  auto tempdir = std::make_shared<QTemporaryDir>();
  QDir tmpdir(tempdir->path());
  QString err;
  for (int i = startFrame; i < endFrame; ++i) {
    progress->setValue(i - startFrame);
    if (progress->wasCanceled()) {
      break;
    }

    setCurrentTime(static_cast<double>(i) / framePerSecond);
    QString exportErr;
    if (!static_cast<ZView*>(m_engine)->waitFor2DExportFrameReady(progress, &exportErr)) {
      if (progress->wasCanceled()) {
        break;
      }
      if (!exportErr.isEmpty()) {
        showCriticalWithDetails(QApplication::activeWindow(), tr("Can not export frame %1").arg(i), exportErr);
      }
      break;
    }

    QString filename = QString("%1%2.png").arg(namePrefix).arg(i, fieldWidth, 10, QChar('0'));
    QString filepath = tmpdir.filePath(filename);
    if (!canvasPainter.renderToImage(filepath, width, height, &err)) {
      showCriticalWithDetails(QApplication::activeWindow(), tr("Can not save frame %1").arg(filepath), err);
      break;
    }
  }

  if (!progress->wasCanceled()) {
    progress->setLabelText("Compressing Video...");
    connect(m_videoEncoder, &ZVideoEncoder::error, progress, &QProgressDialog::reset);
    connect(m_videoEncoder, &ZVideoEncoder::finished, progress, &QProgressDialog::reset);
    connect(m_videoEncoder, &ZVideoEncoder::canceled, progress, &QProgressDialog::reset);
    connect(progress, &QProgressDialog::canceled, m_videoEncoder, &ZVideoEncoder::cancel);
    m_tempDir = tempdir;
    m_videoEncoder->encode(tmpdir, namePrefix, fieldWidth, framePerSecond, dir.filePath(fn));
  }
}

void ZAnimation::export2DAnimation(const QString& fn, int framePerSecond, int startFrame, int endFrame)
{
  CHECK(m_engine);
  ZGraphicsView& canvasPainter = static_cast<ZView*>(m_engine)->graphicsView();
  exportFixedSize2DAnimation(fn,
                             framePerSecond,
                             startFrame,
                             endFrame,
                             canvasPainter.viewportSize().width(),
                             canvasPainter.viewportSize().height());
}

void ZAnimation::disableAnimationOf(size_t id)
{
  AnimationObj* aniObj = findBoundId(id);
  if (aniObj) {
    for (const auto& pa : aniObj->objParaAnimations) {
      pa->releaseParameter();
    }
    aniObj->boundId = 0;
    buildDisplayPacks();
    Q_EMIT objViewChanged();
  }
}

void ZAnimation::tryLinkAnimationWith(size_t id)
{
  ZObjDoc* doc = m_doc.idToDoc(id);
  auto jv = doc->jsonValue(id);
  for (const auto& obj : m_objList) {
    if (obj->boundId == 0 && obj->objType == doc->typeName() && doc->isSameObj(obj->objJsonValue, jv)) {
      obj->boundId = id;
      std::vector<ZParameter*> params;
      if (auto engObj = dynamic_cast<Z3DRenderingEngine*>(m_engine)) {
        if (engObj->thread() == QThread::currentThread()) {
          params = engObj->parametersOfViewSetting(id);
        } else {
          QMetaObject::invokeMethod(
            engObj,
            [&params, engObj, id]() {
              params = engObj->parametersOfViewSetting(id);
            },
            Qt::BlockingQueuedConnection);
        }
      }
      bind(obj->objParaAnimations, params);
      // Ensure newly linked object's params reflect the current animation time
      for (const auto& pa : obj->objParaAnimations) {
        pa->setCurrentTime(m_currentTime);
      }
      buildDisplayPacks();
      Q_EMIT objViewChanged();
      return;
    }
  }
}

void ZAnimation::videoEncoderError(const QString& err)
{
  QMessageBox::critical(QApplication::activeWindow(),
                        QApplication::applicationName(),
                        "Video Encoder Error.\nCan not encode video: " + err);
  m_tempDir.reset();
}

void ZAnimation::videoEncoderFinished()
{
  QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), "Finish Encoding Video.");
  m_tempDir.reset();
}

void ZAnimation::videoEncoderCanceled()
{
  QMessageBox::warning(QApplication::activeWindow(), QApplication::applicationName(), "Video Encoding was canceled.");
  m_tempDir.reset();
}

void ZAnimation::cancelButtonPressed()
{
  if (auto eng = dynamic_cast<Z3DRenderingEngine*>(m_engine); eng) {
    eng->cancelLongRendering();
    m_cancelFlag = true;
  }
}

void ZAnimation::updateObjAnimation()
{
  for (const auto& pa : m_globalParaAnimations) {
    connect(pa.get(), &ZParameterAnimation::keyChanged, this, &ZAnimation::keyChanged, Qt::UniqueConnection);
    connect(pa.get(), &ZParameterAnimation::keysChanged, this, &ZAnimation::keysChanged, Qt::UniqueConnection);
    connect(pa.get(),
            &ZParameterAnimation::keyAboutToDelete,
            this,
            &ZAnimation::keyAboutToDelete,
            Qt::UniqueConnection);
    connect(pa.get(), &ZParameterAnimation::colorChanged, this, &ZAnimation::colorChanged, Qt::UniqueConnection);
  }
  for (const auto& obj : m_objList) {
    for (const auto& pa : obj->objParaAnimations) {
      connect(pa.get(), &ZParameterAnimation::keyChanged, this, &ZAnimation::keyChanged, Qt::UniqueConnection);
      connect(pa.get(), &ZParameterAnimation::keysChanged, this, &ZAnimation::keysChanged, Qt::UniqueConnection);
      connect(pa.get(),
              &ZParameterAnimation::keyAboutToDelete,
              this,
              &ZAnimation::keyAboutToDelete,
              Qt::UniqueConnection);
      connect(pa.get(), &ZParameterAnimation::colorChanged, this, &ZAnimation::colorChanged, Qt::UniqueConnection);
    }
  }

  buildDisplayPacks();

  Q_EMIT objChanged();
}

void ZAnimation::buildDisplayPacks()
{
  int row = 0;
  m_displayPacks.clear();

  for (const auto& pa : m_globalParaAnimations) {
    ZAnimationDisplayPack pack;
    pack.name = pa->name();
    pack.id = 0;
    pack.boundId = 0;
    pack.row = row++;
    pack.type = ZAnimationDisplayPack::Type::GlobalPara;
    pack.expanded = false;
    pack.showAll = false;
    pack.paraAnimation = pa.get();
    pack.objInfo = QString("Global Parameter %1").arg(pa->name());
    m_displayPacks.push_back(pack);
  }

  for (const auto& obj : m_objList) {
    ZAnimationDisplayPack pack;
    if (obj->boundId == 0) {
      pack.name = QString("%1 not loaded").arg(obj->objType);
    } else {
      pack.name = m_doc.objName(obj->boundId);
    }
    pack.id = obj->uniqueId;
    pack.boundId = obj->boundId;
    pack.row = row++;
    pack.type = ZAnimationDisplayPack::Type::Object;
    pack.expanded = obj->isExpanded;
    pack.showAll = obj->isShowAll;
    pack.paraAnimation = nullptr;
    QString objInfo;
    if (obj->objJsonValue.is_string()) {
      objInfo = asQString(obj->objJsonValue);
    } else if (obj->objJsonValue.is_object()) {
      objInfo = jsonToFormattedQString(obj->objJsonValue);
      QStringList infoList = objInfo.split("\n");
      if (infoList.size() > 6) {
        QStringList::iterator it = infoList.begin() + 6;
        *it = "...";
        infoList.erase(it + 1, infoList.end());
        objInfo = infoList.join("\n");
      }
    } else {
      objInfo = obj->objType;
    }
    pack.objInfo = objInfo;
    m_displayPacks.push_back(pack);

    if (pack.expanded && obj->boundId != 0) {
      for (const auto& pa : obj->objParaAnimations) {
        if (pa->numKeys() > 1 || obj->isShowAll || pa->name() == "Visible") {
          ZAnimationDisplayPack pack1;
          pack1.name = pa->name();
          pack1.id = obj->uniqueId;
          pack1.boundId = obj->boundId;
          pack1.row = row++;
          pack1.type = ZAnimationDisplayPack::Type::ObjectPara;
          pack1.expanded = false;
          pack1.showAll = false;
          pack1.paraAnimation = pa.get();
          pack1.objInfo = objInfo;
          m_displayPacks.push_back(pack1);
        }
      }
      ZAnimationDisplayPack pack2;
      pack2.name = obj->isShowAll ? QString("Hide ...") : QString("Show All ...");
      pack2.id = obj->uniqueId;
      pack2.boundId = obj->boundId;
      pack2.row = row++;
      pack2.type = ZAnimationDisplayPack::Type::ShowAll;
      pack2.expanded = false;
      pack2.showAll = false;
      pack2.paraAnimation = nullptr;
      pack2.objInfo = objInfo;
      m_displayPacks.push_back(pack2);
    }
  }
}

void ZAnimation::releaseParameters()
{
  if (m_engine) {
    for (const auto& pa : m_globalParaAnimations) {
      pa->releaseParameter();
    }
    for (const auto& obj : m_objList) {
      if (obj->boundId == 0) {
        continue;
      }
      for (const auto& pa : obj->objParaAnimations) {
        pa->releaseParameter();
      }
    }
  }
}

bool ZAnimation::bind(std::vector<std::unique_ptr<ZParameterAnimation>>& paraAnimationList,
                      const std::vector<ZParameter*>& paraList)
{
  bool changed = false;
  size_t foundNum = 0;
  for (auto para : paraList) {
    if (!para) {
      continue;
    }
    for (size_t j = 0; j < paraAnimationList.size(); ++j) {
      if (!paraAnimationList[j]) {
        continue;
      }
      const QString trackJsonKey = paraAnimationList[j]->jsonKey();
      if (para->matchesJsonKey(trackJsonKey)) {
        paraAnimationList[j]->bindParameter(*para);
        if (paraAnimationList[j]->name() != para->name()) {
          // Migrate legacy parameter names to the canonical current name so
          // the UI and subsequent saves use the updated key.
          paraAnimationList[j]->setName(para->name());
          changed = true;
        }
        if (j != foundNum) {
          std::swap(paraAnimationList[j], paraAnimationList[foundNum]);
          changed = true;
        }
        foundNum++;
        break;
      }
    }
  }
  return changed;
}

void ZAnimation::readContent(const QString& fn, const QString& jsonKey)
{
  try {
    ZBenchTimer bt(fmt::format("ZAnimation::readContent('{}')", jsonKey.toStdString()));
    auto loadObj = loadJsonObject(fn);
    bt.recordEvent("loadJsonObject");
    if (!loadObj.contains(jsonKey.toStdString()) || !loadObj.at(jsonKey.toStdString()).is_object()) {
      throw ZException(tr("File is not %1 format").arg(jsonKey));
    }

    QDir::setCurrent(QFileInfo(fn).absolutePath());

    QString err;
    const auto& animationObj = loadObj.at(jsonKey.toStdString()).as_object();
    const auto& docObj = animationObj.at("Doc").as_object();
    for (const auto& [key, value] : docObj) {
      QString qkey = QString::fromUtf8(key.data(), key.size());
      if (!qkey.contains(QChar(' '))) {
        err += QString("Invalid obj key %1\n").arg(qkey);
        continue;
      }
      int spaceIdx = qkey.indexOf(QChar(' '));
      QString type = qkey.left(spaceIdx);
      QString idString = qkey.mid(spaceIdx + 1);
      if (idString.trimmed().isEmpty()) {
        err += QString("Invalid obj key %1\n").arg(qkey);
        continue;
      }
      bool ok;
      size_t id = idString.toLongLong(&ok);
      if (!ok || id == 0) {
        err += QString("Invalid obj key %1\n").arg(qkey);
        continue;
      }
      auto aniObj = std::make_unique<AnimationObj>(type, value);
      aniObj->uniqueId = id;
      m_nextUniqueId = std::max(m_nextUniqueId, aniObj->uniqueId + 1);
      m_objList.push_back(std::move(aniObj));
    }
    bt.recordEvent(fmt::format("parsed Doc keys={}", docObj.size()));

    std::vector<std::unique_ptr<ZParameterAnimation>> globalParaAnimations;
    for (const auto& [key, value] : animationObj) {
      QString qkey = QString::fromUtf8(key.data(), key.size());
      if (key == "Duration") {
        setDurationImpl(json::value_to<double>(value));
      } else if (key == "Background" || key == "Axis" || key == "Lighting") {
        auto aniObj = std::make_unique<AnimationObj>(qkey, json::value());
        aniObj->uniqueId = m_nextUniqueId++;
        if (key == "Background") {
          aniObj->boundId = 1;
        } else if (key == "Axis") {
          aniObj->boundId = 2;
        } else if (key == "Lighting") {
          aniObj->boundId = 3;
        }
        const auto& jObj = value.as_object();
        for (const auto& [pkey, pvalue] : jObj) {
          QString qpkey = QString::fromUtf8(pkey.data(), pkey.size());
          std::unique_ptr<ZParameterAnimation> pa(ZParameterAnimation::create(qpkey, pvalue, this));
          if (pa) {
            aniObj->objParaAnimations.push_back(std::move(pa));
          }
        }
        m_objList.push_back(std::move(aniObj));
      } else if (key != "Doc" && key != "Version") {
        bool isObj = !qkey.contains(' ');
        if (isObj) {
          bool ok;
          size_t objectId = qkey.toLongLong(&ok);
          if (ok) {
            AnimationObj* aniObj = findUniqueId(objectId);
            if (aniObj) {
              const auto& jObj = value.as_object();
              for (const auto& [pkey, pvalue] : jObj) {
                QString qpkey = QString::fromUtf8(pkey.data(), pkey.size());
                std::unique_ptr<ZParameterAnimation> pa(ZParameterAnimation::create(qpkey, pvalue, this));
                if (pa) {
                  aniObj->objParaAnimations.push_back(std::move(pa));
                }
              }
            }
          } else {
            err += QString("Unknown animation object %1\n").arg(qkey);
          }
        } else {
          std::unique_ptr<ZParameterAnimation> pa(ZParameterAnimation::create(qkey, value, this));
          if (pa) {
            globalParaAnimations.push_back(std::move(pa));
          } else {
            err += QString("Can not parse key %1\n").arg(qkey);
          }
        }
      }
    }
    bt.recordEvent("parsed parameter tracks");

    // match global parameters
    for (auto& gp : globalParaAnimations) {
      for (auto& globalParaAnimation : m_globalParaAnimations) {
        if (gp->name() == globalParaAnimation->name() && gp->type() == globalParaAnimation->type()) {
          globalParaAnimation = std::move(gp);
          break;
        }
      }
    }

    // read files
    std::map<size_t, size_t> idmap = m_doc.read(docObj, err);
    bt.recordEvent("doc.read");
    if (idmap.empty()) {
      // An animation file can legitimately contain zero document objects (e.g. camera/global-only timelines).
      // Only report an error when the file declares document objects but none could be loaded.
      if (!docObj.empty()) {
        err += QString("%1 %2 contains zero valid objects.\n").arg(jsonKey, fn);
      }
    } else {
      for (auto& i : m_objList) {
        if (idmap.contains(i->uniqueId)) {
          i->boundId = idmap.at(i->uniqueId);
          // original jsonvalue might be relative path, we need to convert them to absolute path (if file exist)
          i->objJsonValue = m_doc.idToDoc(i->boundId)->jsonValue(i->boundId);
        }
      }
    }

    if (!err.isEmpty()) {
      showCriticalWithDetails(QApplication::activeWindow(), tr("Can not load animation %1").arg(fn), err);
      LOG(ERROR) << err;
    }

    updateObjAnimation();
    bt.recordEvent("updateObjAnimation");

    // Reset undo state after loading: a loaded animation starts clean.
    m_undoStack.clear();
    m_undoStack.setClean();

    bt.stop();
    VLOG(1) << bt;
  }
  catch (const ZException& e) {
    throw ZException(QString("Can not load animation %1: %2").arg(fn, e.what()));
  }
}

void ZAnimation::writeContent(const QString& fn, const QString& jsonKey)
{
  try {
    // NOTE: Do not call removeRedundantKeys() implicitly on save.
    // Redundant-key cleanup is a destructive edit (it can delete keys) and must
    // remain an explicit user action via the UI button.

    QFileInfo fi(fn);
    QString nName;
    for (int i = 0; i < 10000000; ++i) {
#ifdef _MSC_VER
      nName =
        fi.absolutePath() + QString("\\") + fi.baseName() + QString("_WritingTmp%1_.").arg(i) + fi.completeSuffix();
#else
      nName =
        fi.absolutePath() + QString("/") + fi.baseName() + QString("_WritingTmp%1_.").arg(i) + fi.completeSuffix();
#endif
      if (!QFile::exists(nName)) {
        break;
      }
    }

    json::object animationObj;
    animationObj["Version"] = 1.0;

    json::object docObj;
    for (auto& i : m_objList) {
      if (i->objType == "Axis" || i->objType == "Background" || i->objType == "Lighting") {
        continue;
      }
      docObj[QString("%1 %2").arg(i->objType).arg(i->uniqueId).toStdString()] = i->objJsonValue;
    }
    animationObj["Doc"] = docObj;

    animationObj["Duration"] = m_duration;
    for (auto& globalParaAnimation : m_globalParaAnimations) {
      globalParaAnimation->write(animationObj);
    }
    for (auto& i : m_objList) {
      size_t id = i->uniqueId;
      const auto& pas = i->objParaAnimations;
      if (!pas.empty()) {
        json::object jObj;
        for (const auto& pa : pas) {
          // Backend neutrality: do not persist the render-backend selection in
          // 3D animation files. Render backend is a runtime/engine configuration
          // (and Vulkan is still experimental), so encoding it into timelines
          // makes animations fragile across machines and sessions.
          //
          // NOTE: Loading remains backward-compatible: existing .animation3d
          // files that contain this track will still be parsed and can still
          // drive backend switches during playback. We only omit it on write.
          if (pa && pa->jsonKey() == QStringLiteral("Render Backend StringIntOption")) {
            continue;
          }
          pa->write(jObj);
        }
        if (jObj.empty()) {
          continue;
        }
        QString name;
        if (i->objType == "Background") {
          name = i->objType;
        } else if (i->objType == "Axis") {
          name = i->objType;
        } else if (i->objType == "Lighting") {
          name = i->objType;
        } else {
          name = QString("%1").arg(id);
        }
        animationObj[name.toStdString()] = jObj;
      }
    }

    json::object saveObj;
    saveObj[jsonKey.toStdString()] = animationObj;

    saveJsonObject(saveObj, nName);

    if ((QFile::exists(fn) && !QFile::remove(fn)) || !QFile::rename(nName, fn)) {
      throw ZException(fmt::format("Can not replace old file {} with new file {}", nName, fn),
                       ZException::Option::CheckErrno);
    }
  }
  catch (const ZException& e) {
    throw ZException(QString("Can not save animation %1: %2").arg(fn, e.what()));
  }
}

void ZAnimation::setDurationImpl(double duration)
{
  duration = std::max(1.0, duration);
  if (m_duration != duration) {
    m_duration = duration;
    Q_EMIT durationChanged(m_duration);
  }
}

ZAnimation::UndoSnapshot ZAnimation::captureUndoSnapshot() const
{
  UndoSnapshot out;

  json::object root;
  root[kUndoDuration] = m_duration;
  root[kUndoNextUniqueId] = static_cast<uint64_t>(m_nextUniqueId);

  json::object globalTracks;
  for (const auto& pa : m_globalParaAnimations) {
    globalTracks[pa->jsonKey().toStdString()] = snapshotTrackValue(*pa);
  }
  root[kUndoGlobalTracks] = std::move(globalTracks);

  json::array objects;
  objects.reserve(m_objList.size());
  for (const auto& obj : m_objList) {
    json::object o;
    o[kUndoObjType] = obj->objType.toStdString();
    o[kUndoObjJsonValue] = obj->objJsonValue;
    o[kUndoObjExpanded] = obj->isExpanded;
    o[kUndoObjShowAll] = obj->isShowAll;
    o[kUndoObjBoundId] = static_cast<uint64_t>(obj->boundId);
    o[kUndoObjUniqueId] = static_cast<uint64_t>(obj->uniqueId);

    json::object tracks;
    for (const auto& pa : obj->objParaAnimations) {
      tracks[pa->jsonKey().toStdString()] = snapshotTrackValue(*pa);
    }
    o[kUndoObjTracks] = std::move(tracks);

    objects.push_back(std::move(o));
  }
  root[kUndoObjects] = std::move(objects);

  out.state = std::move(root);
  return out;
}

void ZAnimation::restoreFromUndoSnapshot(const UndoSnapshot& snapshot)
{
  const auto oldBlocked = signalsBlocked();
  blockSignals(true);

  QString err;

  if (!snapshot.state.contains(kUndoDuration) || !snapshot.state.at(kUndoDuration).is_number()) {
    CHECK(false) << "Undo snapshot missing duration";
  }
  if (!snapshot.state.contains(kUndoNextUniqueId) || !snapshot.state.at(kUndoNextUniqueId).is_number()) {
    CHECK(false) << "Undo snapshot missing next_unique_id";
  }
  if (!snapshot.state.contains(kUndoGlobalTracks) || !snapshot.state.at(kUndoGlobalTracks).is_object()) {
    CHECK(false) << "Undo snapshot missing global_tracks";
  }
  if (!snapshot.state.contains(kUndoObjects) || !snapshot.state.at(kUndoObjects).is_array()) {
    CHECK(false) << "Undo snapshot missing objects";
  }

  const double duration = json::value_to<double>(snapshot.state.at(kUndoDuration));
  setDurationImpl(duration);

  const auto& nextUniqueV = snapshot.state.at(kUndoNextUniqueId);
  CHECK(nextUniqueV.is_uint64()) << "Undo snapshot next_unique_id must be uint64";
  m_nextUniqueId = static_cast<size_t>(nextUniqueV.as_uint64());

  const json::object& globalTracks = snapshot.state.at(kUndoGlobalTracks).as_object();
  for (const auto& pa : m_globalParaAnimations) {
    const std::string key = pa->jsonKey().toStdString();
    auto it = globalTracks.find(key);
    CHECK(it != globalTracks.end()) << "Undo snapshot missing global track " << key;
    CHECK(restoreTrackFromSnapshot(*pa, it->value(), &err)) << err.toStdString();
  }

  m_objList.clear();
  const auto& objects = snapshot.state.at(kUndoObjects).as_array();
  m_objList.reserve(objects.size());

  for (const auto& ov : objects) {
    CHECK(ov.is_object());
    const auto& o = ov.as_object();

    CHECK(o.contains(kUndoObjType) && o.at(kUndoObjType).is_string());
    QString objType = QString::fromUtf8(o.at(kUndoObjType).get_string().data(),
                                        static_cast<int>(o.at(kUndoObjType).get_string().size()));

    json::value objJsonValue;
    if (o.contains(kUndoObjJsonValue)) {
      objJsonValue = o.at(kUndoObjJsonValue);
    }

    auto ao = std::make_unique<AnimationObj>(objType, std::move(objJsonValue));

    if (o.contains(kUndoObjExpanded) && o.at(kUndoObjExpanded).is_bool()) {
      ao->isExpanded = o.at(kUndoObjExpanded).as_bool();
    }
    if (o.contains(kUndoObjShowAll) && o.at(kUndoObjShowAll).is_bool()) {
      ao->isShowAll = o.at(kUndoObjShowAll).as_bool();
    }
    if (o.contains(kUndoObjBoundId) && o.at(kUndoObjBoundId).is_number()) {
      CHECK(o.at(kUndoObjBoundId).is_uint64()) << "Undo snapshot bound_id must be uint64";
      ao->boundId = static_cast<size_t>(o.at(kUndoObjBoundId).as_uint64());
    }
    if (o.contains(kUndoObjUniqueId) && o.at(kUndoObjUniqueId).is_number()) {
      CHECK(o.at(kUndoObjUniqueId).is_uint64()) << "Undo snapshot unique_id must be uint64";
      ao->uniqueId = static_cast<size_t>(o.at(kUndoObjUniqueId).as_uint64());
    }

    if (o.contains(kUndoObjTracks) && o.at(kUndoObjTracks).is_object()) {
      const auto& tracks = o.at(kUndoObjTracks).as_object();
      for (const auto& [k, v] : tracks) {
        const QString jsonKey = QString::fromUtf8(k.data(), static_cast<int>(k.size()));
        std::unique_ptr<ZParameterAnimation> pa(ZParameterAnimation::create(jsonKey, v, this));
        if (pa) {
          ao->objParaAnimations.push_back(std::move(pa));
        }
      }
    }

    m_objList.push_back(std::move(ao));
  }

  updateObjAnimation();

  if (m_engine) {
    rebindView();
  }
  setCurrentTime(m_currentTime);

  blockSignals(oldBlocked);

  // Emit coarse-grained signals once so listeners see a consistent snapshot.
  Q_EMIT durationChanged(m_duration);
  Q_EMIT objChanged();
  Q_EMIT keysChanged();
  if (m_engine) {
    Q_EMIT objViewChanged();
  }
}

void ZAnimation::pushUndoSnapshotCommand(const QString& text, UndoSnapshot&& beforeSnapshot)
{
  m_undoStack.push(new ZAnimationSnapshotCommand(this, text, std::move(beforeSnapshot)));
}

ZAnimation::AnimationObj* ZAnimation::findBoundId(size_t id, size_t* idx)
{
  for (size_t i = 0; i < m_objList.size(); ++i) {
    if (m_objList[i]->boundId == id) {
      if (idx) {
        *idx = i;
      }
      return m_objList[i].get();
    }
  }
  return nullptr;
}

ZAnimation::AnimationObj* ZAnimation::findUniqueId(size_t id, size_t* idx)
{
  for (size_t i = 0; i < m_objList.size(); ++i) {
    if (m_objList[i]->uniqueId == id) {
      if (idx) {
        *idx = i;
      }
      return m_objList[i].get();
    }
  }
  return nullptr;
}

const ZAnimation::AnimationObj* ZAnimation::findBoundId(size_t id, size_t* idx) const
{
  for (size_t i = 0; i < m_objList.size(); ++i) {
    if (m_objList[i]->boundId == id) {
      if (idx) {
        *idx = i;
      }
      return m_objList[i].get();
    }
  }
  return nullptr;
}

const ZAnimation::AnimationObj* ZAnimation::findUniqueId(size_t id, size_t* idx) const
{
  for (size_t i = 0; i < m_objList.size(); ++i) {
    if (m_objList[i]->uniqueId == id) {
      if (idx) {
        *idx = i;
      }
      return m_objList[i].get();
    }
  }
  return nullptr;
}

} // namespace nim
