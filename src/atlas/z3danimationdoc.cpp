#include "z3danimationdoc.h"

#include "zanimationwidget.h"
#include "z3drenderingengine.h"
#include "zcameraparameterkey.h"
#include "zcameraparameteranimation.h"
#include "zbenchtimer.h"
#include "zexception.h"
#include "zlog.h"
#include "zmessageboxhelpers.h"
#include "zoptionparameter.h"
#include "zparameterkey.h"
#include "zqobjectthreadinvoker.h"
#include "ztheme.h"
#include "zjson.h"
#include <QFileDialog>
#include <QSettings>
#include <QApplication>
#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace nim {

Z3DAnimationDoc::Z3DAnimationDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

void Z3DAnimationDoc::bindView(Z3DRenderingEngine* v)
{
  if (m_view == v) {
    return;
  }
  if (m_view && m_view != v) {
    // Avoid stale connections when rebinding to a new engine instance.
    // (E.g. when the 3D window is recreated.) Otherwise a later destroyed()
    // signal from the old engine could release the new binding unexpectedly.
    disconnect(m_view, nullptr, this, nullptr);
    releaseView();
  }
  m_view = v;
  if (!m_view) {
    return;
  }

  ZBenchTimer bt(fmt::format("Z3DAnimationDoc::bindView ({} animations)", m_idToAnimationPacks.size()));
  connect(m_view, &Z3DRenderingEngine::destroyed, this, &Z3DAnimationDoc::releaseView, Qt::UniqueConnection);
  bt.recordEvent("connected engine");
  for (const auto& idPack : m_idToAnimationPacks) {
    idPack.second->animation->bindView(m_view);
  }
  bt.recordEvent("animations bound");
  for (const auto& idPack : m_idToAnimationPacks) {
    idPack.second->animation->setCurrentTime(0);
  }
  bt.recordEvent("setCurrentTime(0)");
  bt.stop();
  VLOG(1) << bt;
  LOG(INFO) << "bind 3d view done";
}

void Z3DAnimationDoc::createNewAnimation(const QString& name)
{
  auto animation = new Z3DAnimation(m_doc, this);
  addAnimation(animation, "", name);
  // Only add a default keyframe if a 3D view/engine is already bound.
  // When created before the 3D window is opened, defer key creation until
  // the engine exists to avoid touching rendering state off-thread.
  if (m_view) {
    animation->addKeyFrame(0);
  } else {
    VLOG(1) << "CreateNewAnimation: engine not ready; skipping default keyframe";
  }
}

std::vector<size_t> Z3DAnimationDoc::animationIds() const
{
  std::vector<size_t> ids;
  ids.reserve(m_idToAnimationPacks.size());
  for (const auto& idPack : m_idToAnimationPacks) {
    ids.push_back(idPack.first);
  }
  return ids;
}

Z3DAnimation* Z3DAnimationDoc::animationPtr(size_t id)
{
  auto it = m_idToAnimationPacks.find(id);
  if (it == m_idToAnimationPacks.end()) {
    return nullptr;
  }
  return it->second->animation.get();
}

const Z3DAnimation* Z3DAnimationDoc::animationPtr(size_t id) const
{
  auto it = m_idToAnimationPacks.find(id);
  if (it == m_idToAnimationPacks.end()) {
    return nullptr;
  }
  return it->second->animation.get();
}

size_t Z3DAnimationDoc::createNewAnimationAndReturnId(const QString& name)
{
  auto animation = new Z3DAnimation(m_doc, this);
  size_t id = addAnimation(animation, "", name);
  if (m_view) {
    animation->addKeyFrame(0);
  } else {
    VLOG(1) << "CreateNewAnimationAndReturnId: engine not ready; skipping default keyframe";
  }
  return id;
}

namespace {

// Timeline editing uses floating seconds; match the existing RPC tolerance.
static constexpr double kKeyTimeEpsSec = 1e-6;

struct EngineParamSpec
{
  bool found = false;
  QString type;
  bool supportsInterpolation = false;
  bool isStringOption = false;
  bool isIntOption = false;
  std::vector<QString> stringOptions;
  std::vector<int> intOptions;
};

[[nodiscard]] QString normalizeInterpolationMethod(QString s)
{
  s = s.toLower().trimmed();
  QString out;
  out.reserve(s.size());
  for (QChar c : s) {
    if (c.isSpace() || c == QChar('_') || c == QChar('-')) {
      continue;
    }
    out.append(c);
  }
  return out;
}

} // namespace

Z3DAnimationDoc::KeyOpResult Z3DAnimationDoc::setKey(size_t animationId,
                                                     size_t targetId,
                                                     const QString& jsonKey,
                                                     double timeSec,
                                                     const QString& easing,
                                                     const json::value& value)
{
  KeyOpResult out;
  const bool recordUndo = (m_undoSuppressionDepth == 0);

  if (animationId == 0) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id is required";
    return out;
  }
  if (timeSec < 0.0) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "time must be >= 0";
    return out;
  }

  Z3DAnimation* anim = animationPtr(animationId);
  if (!anim) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id not found";
    return out;
  }
  if (!m_view || !m_view->thread() || !m_view->thread()->isRunning() || m_view->thread()->isFinished()) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::FailedPrecondition;
    out.error = "engine not ready";
    return out;
  }

  // Camera (id=0) is stored as a dedicated track.
  if (targetId == 0) {
    if (!value.is_object()) {
      out.ok = false;
      out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
      out.error = "camera value must be an object";
      return out;
    }
    auto ckey = std::make_unique<ZCameraParameterKey>();
    json::object keyObj;
    keyObj["time"] = timeSec;
    keyObj["type"] = easing.toStdString();
    keyObj["value"] = value;
    try {
      if (!ckey->readValue(keyObj)) {
        out.ok = false;
        out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
        out.error = "camera key value incompatible with parameter";
        return out;
      }
    }
    catch (const std::exception& e) {
      out.ok = false;
      out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
      out.error = QString("camera key read failed: %1").arg(e.what());
      return out;
    }
    catch (...) {
      out.ok = false;
      out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
      out.error = "camera key read failed";
      return out;
    }
    ZAnimation::UndoSnapshot before;
    if (recordUndo) {
      before = anim->captureUndoSnapshot();
    }
    anim->cameraParameterAnimation()->addKey(std::move(ckey));
    if (recordUndo) {
      anim->pushUndoSnapshotCommand("Set Camera Key", std::move(before));
    }
    out.ok = true;
    return out;
  }

  if (jsonKey.trimmed().isEmpty()) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "json_key is required for non-camera";
    return out;
  }

  const size_t boundId = targetId;
  const QString jsonKeyTrim = jsonKey.trimmed();

  Z3DRenderingEngine* view = m_view;
  auto inv = invokeOnObjectThreadWait(
    view,
    [view, boundId, jsonKeyTrim]() {
      EngineParamSpec spec;
      const auto params = view->parametersOfViewSetting(boundId);
      ZParameter* target = nullptr;
      for (auto* p : params) {
        if (p && p->jsonKey() == jsonKeyTrim) {
          target = p;
          break;
        }
      }
      if (!target) {
        return spec;
      }
      spec.found = true;
      spec.type = target->type();
      spec.supportsInterpolation = target->supportInterpolation();

      if (auto optSI = dynamic_cast<const ZStringIntOptionParameter*>(target)) {
        spec.isStringOption = true;
        spec.stringOptions = optSI->options();
      } else if (auto optSS = dynamic_cast<const ZStringStringOptionParameter*>(target)) {
        spec.isStringOption = true;
        spec.stringOptions = optSS->options();
      } else if (auto optII = dynamic_cast<const ZIntIntOptionParameter*>(target)) {
        spec.isIntOption = true;
        spec.intOptions = optII->options();
      }

      return spec;
    },
    "animation_set_key:param_spec");
  if (!inv.ok) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::FailedPrecondition;
    out.error = QString::fromStdString(inv.error);
    return out;
  }
  const EngineParamSpec spec = inv.value;
  if (!spec.found) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = QString("target parameter not found for json_key=%1").arg(jsonKeyTrim);
    return out;
  }

  // Validate value against parameter type/options (mirror UI constraints).
  const QString tstr = spec.type;
  auto expectArrayN = [&](size_t n) -> bool {
    if (!value.is_array()) {
      return false;
    }
    return value.as_array().size() == n;
  };

  if (tstr == "Bool") {
    if (!value.is_bool()) {
      out.ok = false;
      out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
      out.error =
        QString("type mismatch for json_key=%1: expected Bool got %2").arg(jsonKeyTrim, QString::fromStdString(jsonTypeName(value)));
      return out;
    }
  } else if (tstr == "Float" || tstr == "Double" || tstr == "Int") {
    if (!value.is_number()) {
      out.ok = false;
      out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
      out.error =
        QString("type mismatch for json_key=%1: expected number got %2").arg(jsonKeyTrim, QString::fromStdString(jsonTypeName(value)));
      return out;
    }
  } else if (tstr.endsWith("Vec4")) {
    if (!expectArrayN(4)) {
      out.ok = false;
      out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
      out.error =
        QString("type mismatch for json_key=%1: expected array[4] got %2").arg(jsonKeyTrim, QString::fromStdString(jsonTypeName(value)));
      return out;
    }
  } else if (tstr.endsWith("Vec3")) {
    if (!expectArrayN(3)) {
      out.ok = false;
      out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
      out.error =
        QString("type mismatch for json_key=%1: expected array[3] got %2").arg(jsonKeyTrim, QString::fromStdString(jsonTypeName(value)));
      return out;
    }
  } else if (spec.isStringOption) {
    if (!value.is_string()) {
      out.ok = false;
      out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
      out.error =
        QString("type mismatch for json_key=%1: expected option string got %2").arg(jsonKeyTrim, QString::fromStdString(jsonTypeName(value)));
      return out;
    }
    const auto& bs = value.as_string();
    const QString label = QString::fromUtf8(bs.data(), static_cast<int>(bs.size()));
    const bool allowed = std::ranges::any_of(spec.stringOptions, [&](const QString& o) { return o == label; });
    if (!allowed) {
      out.ok = false;
      out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
      out.error = QString("invalid option for json_key=%1: %2").arg(jsonKeyTrim, label);
      return out;
    }
  } else if (spec.isIntOption) {
    if (!value.is_number()) {
      out.ok = false;
      out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
      out.error =
        QString("type mismatch for json_key=%1: expected integer option got %2").arg(jsonKeyTrim, QString::fromStdString(jsonTypeName(value)));
      return out;
    }
    int ival = 0;
    if (value.is_int64()) {
      ival = static_cast<int>(value.as_int64());
    } else if (value.is_uint64()) {
      ival = static_cast<int>(value.as_uint64());
    } else {
      ival = static_cast<int>(std::floor(value.as_double() + 0.5));
    }
    const bool allowed = std::ranges::any_of(spec.intOptions, [&](int o) { return o == ival; });
    if (!allowed) {
      out.ok = false;
      out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
      out.error = QString("invalid option for json_key=%1: %2").arg(jsonKeyTrim).arg(ival);
      return out;
    }
  } else if (tstr == "3DTransform") {
    if (!value.is_object()) {
      out.ok = false;
      out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
      out.error =
        QString("type mismatch for json_key=%1: expected object got %2").arg(jsonKeyTrim, QString::fromStdString(jsonTypeName(value)));
      return out;
    }
  }

  ZAnimation::UndoSnapshot before;
  if (recordUndo) {
    before = anim->captureUndoSnapshot();
  }

  // Ensure the target track exists without creating a full global keyframe.
  auto ensure = anim->ensureParameterAnimationForBoundId(boundId, jsonKeyTrim);
  if (!ensure.error.isEmpty()) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = ensure.error;
    return out;
  }
  if (ensure.createdObject || ensure.createdParameter) {
    // Bind the newly created track to live parameters so UI playback/editing works immediately.
    anim->rebindView();
  }

  auto* pa = anim->parameterAnimationForBoundId(boundId, jsonKeyTrim);
  if (!pa) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::FailedPrecondition;
    out.error = "failed to locate parameter animation track";
    return out;
  }

  const QString keyType = spec.supportsInterpolation ? easing : QStringLiteral("Switch");
  json::object keyObj;
  keyObj["time"] = timeSec;
  keyObj["type"] = keyType.toStdString();
  keyObj["value"] = value;

  auto key = std::make_unique<ZParameterKey>(spec.type);
  try {
    if (!key->readValue(keyObj)) {
      out.ok = false;
      out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
      out.error = "key value incompatible with parameter";
      return out;
    }
  }
  catch (const std::exception& e) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = QString("key read failed: %1").arg(e.what());
    return out;
  }
  catch (...) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "key read failed";
    return out;
  }

  pa->addKey(std::move(key));
  if (recordUndo) {
    anim->pushUndoSnapshotCommand("Set Key", std::move(before));
  }
  out.ok = true;
  return out;
}

Z3DAnimationDoc::KeyOpResult Z3DAnimationDoc::removeKey(size_t animationId,
                                                        size_t targetId,
                                                        const QString& jsonKey,
                                                        double timeSec)
{
  KeyOpResult out;
  const bool recordUndo = (m_undoSuppressionDepth == 0);
  if (animationId == 0) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id is required";
    return out;
  }
  if (timeSec < 0.0) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "time must be >= 0";
    return out;
  }

  Z3DAnimation* anim = animationPtr(animationId);
  if (!anim) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id not found";
    return out;
  }

  if (targetId == 0) {
    auto* cpa = anim->cameraParameterAnimation();
    for (const auto& k : cpa->keys()) {
      if (std::abs(k->time() - timeSec) < kKeyTimeEpsSec) {
        ZAnimation::UndoSnapshot before;
        if (recordUndo) {
          before = anim->captureUndoSnapshot();
        }
        cpa->deleteKey(k.get());
        cpa->emitKeysChangedSignal();
        if (recordUndo) {
          anim->pushUndoSnapshotCommand("Remove Camera Key", std::move(before));
        }
        out.ok = true;
        return out;
      }
    }
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "camera key not found at requested time";
    return out;
  }

  const QString jsonKeyTrim = jsonKey.trimmed();
  if (jsonKeyTrim.isEmpty()) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "json_key is required for non-camera removal";
    return out;
  }

  ZParameterAnimation* pa = anim->parameterAnimationForBoundId(targetId, jsonKeyTrim);
  if (!pa) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "json_key not found for target";
    return out;
  }

  for (const auto& k : pa->keys()) {
    if (std::abs(k->time() - timeSec) < kKeyTimeEpsSec) {
      ZAnimation::UndoSnapshot before;
      if (recordUndo) {
        before = anim->captureUndoSnapshot();
      }
      pa->deleteKey(k.get());
      pa->emitKeysChangedSignal();
      if (recordUndo) {
        anim->pushUndoSnapshotCommand("Remove Key", std::move(before));
      }
      out.ok = true;
      return out;
    }
  }
  out.ok = false;
  out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
  out.error = "key not found at requested time";
  return out;
}

Z3DAnimationDoc::KeyOpResult Z3DAnimationDoc::clearKeys(size_t animationId, size_t targetId, const QString& jsonKey)
{
  KeyOpResult out;
  const bool recordUndo = (m_undoSuppressionDepth == 0);
  if (animationId == 0) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id is required";
    return out;
  }
  Z3DAnimation* anim = animationPtr(animationId);
  if (!anim) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id not found";
    return out;
  }

  if (targetId == 0) {
    auto* cpa = anim->cameraParameterAnimation();
    if (cpa->keys().empty()) {
      out.ok = true;
      return out;
    }
    ZAnimation::UndoSnapshot before;
    if (recordUndo) {
      before = anim->captureUndoSnapshot();
    }
    std::vector<ZParameterKey*> keys;
    keys.reserve(cpa->keys().size());
    for (const auto& k : cpa->keys()) {
      keys.push_back(k.get());
    }
    for (auto* k : keys) {
      cpa->deleteKey(k);
    }
    cpa->emitKeysChangedSignal();
    if (recordUndo) {
      anim->pushUndoSnapshotCommand("Clear Camera Keys", std::move(before));
    }
    out.ok = true;
    return out;
  }

  const QString jsonKeyTrim = jsonKey.trimmed();
  if (jsonKeyTrim.isEmpty()) {
    bool hasAnyKeys = false;
    for (auto* pa : anim->parameterAnimationsForBoundId(targetId)) {
      if (pa && !pa->keys().empty()) {
        hasAnyKeys = true;
        break;
      }
    }
    if (!hasAnyKeys) {
      out.ok = true;
      return out;
    }
    ZAnimation::UndoSnapshot before;
    if (recordUndo) {
      before = anim->captureUndoSnapshot();
    }
    // Clear all tracks for this target id (benign no-op when no tracks exist).
    for (auto* pa : anim->parameterAnimationsForBoundId(targetId)) {
      std::vector<ZParameterKey*> keys;
      keys.reserve(pa->keys().size());
      for (const auto& k : pa->keys()) {
        keys.push_back(k.get());
      }
      for (auto* k : keys) {
        pa->deleteKey(k);
      }
      pa->emitKeysChangedSignal();
    }
    if (recordUndo) {
      anim->pushUndoSnapshotCommand("Clear Keys", std::move(before));
    }
    out.ok = true;
    return out;
  }

  ZParameterAnimation* pa = anim->parameterAnimationForBoundId(targetId, jsonKeyTrim);
  if (!pa) {
    // Preserve existing RPC behavior: clearing a missing track is a benign no-op.
    out.ok = true;
    return out;
  }
  if (pa->keys().empty()) {
    out.ok = true;
    return out;
  }
  ZAnimation::UndoSnapshot before;
  if (recordUndo) {
    before = anim->captureUndoSnapshot();
  }
  std::vector<ZParameterKey*> keys;
  keys.reserve(pa->keys().size());
  for (const auto& k : pa->keys()) {
    keys.push_back(k.get());
  }
  for (auto* k : keys) {
    pa->deleteKey(k);
  }
  pa->emitKeysChangedSignal();
  if (recordUndo) {
    anim->pushUndoSnapshotCommand("Clear Keys", std::move(before));
  }
  out.ok = true;
  return out;
}

Z3DAnimationDoc::ListKeysResult Z3DAnimationDoc::listKeys(size_t animationId,
                                                          size_t targetId,
                                                          const QString& jsonKey,
                                                          bool includeValues)
{
  ListKeysResult out;
  if (animationId == 0) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id is required";
    return out;
  }

  Z3DAnimation* anim = animationPtr(animationId);
  if (!anim) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id not found";
    return out;
  }

  if (targetId == 0) {
    auto* cpa = anim->cameraParameterAnimation();
    out.keys.reserve(cpa->keys().size());
    for (const auto& k : cpa->keys()) {
      ListedKey ki;
      ki.timeSec = k->time();
      ki.parameterType = "3DCamera";
      if (includeValues) {
        ki.keyJson = QString::fromStdString(jsonToString(k->jsonValue()));
      }
      out.keys.push_back(std::move(ki));
    }
    out.ok = true;
    return out;
  }

  const QString jsonKeyTrim = jsonKey.trimmed();
  ZParameterAnimation* pa = anim->parameterAnimationForBoundId(targetId, jsonKeyTrim);
  if (!pa) {
    // Missing tracks are treated as empty (benign) to simplify client logic.
    out.ok = true;
    return out;
  }
  out.keys.reserve(pa->keys().size());
  for (const auto& k : pa->keys()) {
    ListedKey ki;
    ki.timeSec = k->time();
    ki.parameterType = pa->type();
    if (includeValues) {
      ki.keyJson = QString::fromStdString(jsonToString(k->jsonValue()));
    }
    out.keys.push_back(std::move(ki));
  }
  out.ok = true;
  return out;
}

Z3DAnimationDoc::KeyOpResult Z3DAnimationDoc::batchKeys(size_t animationId,
                                                        const std::vector<BatchRemoveKeyOp>& removeOps,
                                                        const std::vector<BatchSetKeyOp>& setOps,
                                                        bool commit)
{
  KeyOpResult out;
  const bool recordUndo = (m_undoSuppressionDepth == 0);
  if (animationId == 0) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id is required";
    return out;
  }

  Z3DAnimation* anim = animationPtr(animationId);
  if (!anim) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id not found";
    return out;
  }
  if (!setOps.empty()) {
    if (!m_view || !m_view->thread() || !m_view->thread()->isRunning() || m_view->thread()->isFinished()) {
      out.ok = false;
      out.errorKind = KeyOpResult::ErrorKind::FailedPrecondition;
      out.error = "engine not ready";
      return out;
    }
  }

  auto before = anim->captureUndoSnapshot();
  struct UndoSuppressionGuard
  {
    explicit UndoSuppressionGuard(Z3DAnimationDoc& doc)
      : m_doc(doc)
    {
      ++m_doc.m_undoSuppressionDepth;
    }
    ~UndoSuppressionGuard()
    {
      CHECK(m_doc.m_undoSuppressionDepth > 0);
      --m_doc.m_undoSuppressionDepth;
    }
    Z3DAnimationDoc& m_doc;
  } guard(*this);

  for (size_t idx = 0; idx < removeOps.size(); ++idx) {
    const auto& r = removeOps[idx];
    auto res = removeKey(animationId, r.targetId, r.jsonKey, r.timeSec);
    if (!res.ok) {
      anim->restoreFromUndoSnapshot(before);
      out.ok = false;
      out.errorKind = res.errorKind;
      const QString detail = res.error.isEmpty() ? "remove_key failed" : res.error;
      out.error = QString("remove_key failed at index=%1: %2").arg(idx).arg(detail);
      return out;
    }
  }

  bool hasTimeZero = false;
  for (size_t idx = 0; idx < setOps.size(); ++idx) {
    const auto& s = setOps[idx];
    auto res = setKey(animationId, s.targetId, s.jsonKey, s.timeSec, s.easing, s.value);
    if (!res.ok) {
      anim->restoreFromUndoSnapshot(before);
      out.ok = false;
      out.errorKind = res.errorKind;
      const QString detail = res.error.isEmpty() ? "set_key failed" : res.error;
      out.error = QString("set_key failed at index=%1: %2").arg(idx).arg(detail);
      return out;
    }
    if (std::abs(s.timeSec) < kKeyTimeEpsSec) {
      hasTimeZero = true;
    }
  }

  if (commit && hasTimeZero) {
    anim->cancelRenderingAndSetCurrentTime(0.0);
  }

  if (recordUndo && (!removeOps.empty() || !setOps.empty())) {
    anim->pushUndoSnapshotCommand("Batch Keys", std::move(before));
  }

  out.ok = true;
  return out;
}

Z3DAnimationDoc::TimeStatusResult Z3DAnimationDoc::timeStatus(size_t animationId) const
{
  TimeStatusResult out;
  if (animationId == 0) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id is required";
    return out;
  }
  const Z3DAnimation* anim = animationPtr(animationId);
  if (!anim) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id not found";
    return out;
  }
  out.ok = true;
  out.duration = anim->duration();
  out.seconds = anim->currentTime();
  return out;
}

Z3DAnimationDoc::KeyOpResult Z3DAnimationDoc::setTime(size_t animationId, double seconds, bool cancelRendering)
{
  KeyOpResult out;
  if (animationId == 0) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id is required";
    return out;
  }
  Z3DAnimation* anim = animationPtr(animationId);
  if (!anim) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id not found";
    return out;
  }
  if (cancelRendering) {
    anim->cancelRenderingAndSetCurrentTime(seconds);
  } else {
    anim->setCurrentTime(seconds);
  }
  out.ok = true;
  return out;
}

Z3DAnimationDoc::KeyOpResult Z3DAnimationDoc::setCameraInterpolationMethod(size_t animationId, const QString& method)
{
  KeyOpResult out;
  const bool recordUndo = (m_undoSuppressionDepth == 0);
  if (animationId == 0) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id is required";
    return out;
  }
  Z3DAnimation* anim = animationPtr(animationId);
  if (!anim) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id not found";
    return out;
  }

  const QString raw = method.trimmed();
  if (raw.isEmpty()) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "method is required";
    return out;
  }

  auto* cpa = anim->cameraParameterAnimation();
  CHECK(cpa);

  const QString wantNorm = normalizeInterpolationMethod(raw);
  QString matched;
  QString joined;
  for (const auto& opt : cpa->interpolationMethodPara().options()) {
    if (!joined.isEmpty()) {
      joined += ", ";
    }
    joined += opt;
    if (matched.isEmpty() && normalizeInterpolationMethod(opt) == wantNorm) {
      matched = opt;
    }
  }
  if (matched.isEmpty()) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = QString("unsupported camera interpolation method: %1 (available: %2)").arg(raw, joined);
    return out;
  }

  if (cpa->interpolationMethodPara().get() == matched) {
    out.ok = true;
    return out;
  }

  ZAnimation::UndoSnapshot before;
  if (recordUndo) {
    before = anim->captureUndoSnapshot();
  }
  cpa->interpolationMethodPara().select(matched);
  if (recordUndo) {
    anim->pushUndoSnapshotCommand("Set Camera Interpolation Method", std::move(before));
  }
  out.ok = true;
  return out;
}

Z3DAnimationDoc::CameraInterpolationMethodResult Z3DAnimationDoc::cameraInterpolationMethod(size_t animationId) const
{
  CameraInterpolationMethodResult out;
  if (animationId == 0) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id is required";
    return out;
  }
  const Z3DAnimation* anim = animationPtr(animationId);
  if (!anim) {
    out.ok = false;
    out.errorKind = KeyOpResult::ErrorKind::InvalidArgument;
    out.error = "animation_id not found";
    return out;
  }
  const auto* cpa = anim->cameraParameterAnimation();
  CHECK(cpa);
  out.ok = true;
  out.method = cpa->interpolationMethodPara().get();
  return out;
}

bool Z3DAnimationDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id)) {
    return true;
  }

  auto& pack = m_idToAnimationPacks.at(id);
  if (pack->path.endsWith(".animation3d", Qt::CaseInsensitive)) {
    QString err;
    if (saveAnimation(pack.get(), pack->path, err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not save animation %1").arg(pack->path), err);
    return false;
  }
  return saveAs(id);
}

bool Z3DAnimationDoc::saveAs(size_t id)
{
  QStringList filters;
  filters << "3D Animation files (*.animation3d)";

  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilters(filters);
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle(tr("Save 3D Animation %1 As").arg(objName(id)));
  if (dialog.exec()) {
    QString err;
    auto& pack = m_idToAnimationPacks.at(id);
    const QString targetPath = dialog.selectedFiles().at(0);
    if (saveAnimation(pack.get(), targetPath, err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not save animation %1").arg(targetPath), err);
  }
  return false;
}

bool Z3DAnimationDoc::saveToPath(size_t id, const QString& filePath)
{
  try {
    auto& pack = m_idToAnimationPacks.at(id);
    QString err;
    if (saveAnimation(pack.get(), filePath, err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    LOG(ERROR) << "saveToPath failed: " << err;
    return false;
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "saveToPath exception: " << e.what();
    return false;
  }
}

bool Z3DAnimationDoc::canReadFile(const QString& fileName) const
{
  return fileName.endsWith(".animation3d", Qt::CaseInsensitive);
}

size_t Z3DAnimationDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (const auto& idPack : m_idToAnimationPacks) {
    if (idPack.second->path == fileName) {
      return idPack.first;
    }
  }
  size_t id;
  try {
    auto animation = std::make_unique<Z3DAnimation>(m_doc);
    animation->load(fileName, m_showLoadIssueDialogs);
    id = addAnimation(animation.release(), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
  catch (const std::exception& e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t Z3DAnimationDoc::loadFile(const json::value& jValue, QString& errorMsg)
{
  try {
    if (asQString(jValue).trimmed().isEmpty()) {
      errorMsg = QString("File path is not string or is empty");
      return 0;
    }
    for (const auto& idPack : m_idToAnimationPacks) {
      if (isSameObj(jValue, jsonValue(idPack.first))) {
        return idPack.first;
      }
    }
    size_t id;
    QString fileName = asQString(jValue);

    auto animation = std::make_unique<Z3DAnimation>(m_doc);
    animation->load(fileName, m_showLoadIssueDialogs);
    id = addAnimation(animation.release(), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

std::vector<QAction*> Z3DAnimationDoc::loadFileActions() const
{
  std::vector<QAction*> res;
  res.push_back(m_loadAnimationsAction);
  return res;
}

void Z3DAnimationDoc::removeObj(size_t id)
{
  auto it = m_idToAnimationPacks.find(id);
  Q_EMIT objAboutToBeRemoved(it->first, this);
  m_idToAnimationPacks.erase(it);
  Q_EMIT objRemoved(id, this);
}

QString Z3DAnimationDoc::objName(size_t id) const
{
  return m_idToAnimationPacks.at(id)->name();
}

QString Z3DAnimationDoc::objPath(size_t id) const
{
  return m_idToAnimationPacks.at(id)->path;
}

bool Z3DAnimationDoc::objHasUnsavedChange(size_t id) const
{
  const auto& pack = m_idToAnimationPacks.at(id);
  return !(canReadFile(pack->path) && objUndoStack(id)->isClean());
}

QString Z3DAnimationDoc::objInfo(size_t id) const
{
  return m_idToAnimationPacks.at(id)->info();
}

QString Z3DAnimationDoc::objTooltip(size_t id) const
{
  return m_idToAnimationPacks.at(id)->tooltip();
}

const QUndoStack* Z3DAnimationDoc::objUndoStack(size_t id) const
{
  return m_idToAnimationPacks.at(id)->animation->undoStack();
}

json::value Z3DAnimationDoc::jsonValue(size_t id) const
{
  return json::value_from(m_idToAnimationPacks.at(id)->path);
}

bool Z3DAnimationDoc::isSameObj(const json::value& v1, const json::value& v2) const
{
  CHECK(v1.is_string() && v2.is_string());
  if (v1 == v2) {
    return true;
  }
  QString f1 = asQString(v1);
  QString f2 = asQString(v2);
  if (!QFile::exists(f1) || !QFile::exists(f2)) {
    return false;
  }
  return QFileInfo(f1).canonicalFilePath() == QFileInfo(f2).canonicalFilePath();
}

size_t Z3DAnimationDoc::makeAlias(size_t /*id*/)
{
  return 0;
}

bool Z3DAnimationDoc::isAlias(size_t id) const
{
  CHECK(m_idToAnimationPacks.contains(id));

  return std::ranges::any_of(m_idToAnimationPacks, [&, this](const auto& idPack) {
    return idPack.first != id && idPack.second == m_idToAnimationPacks.at(id);
  });
}

QWidget* Z3DAnimationDoc::createObjEditWidget(size_t id)
{
  CHECK(m_idToAnimationPacks.contains(id));

  auto& pack = m_idToAnimationPacks.at(id);
  return new ZAnimationWidget(*pack->animation);
}

void Z3DAnimationDoc::loadAnimation()
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilter("*.animation3d");
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load 3D Animation File");
  if (dialog.exec()) {
    QString errorMsg;
    // int fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    for (index_t i = 0; i < dialog.selectedFiles().size(); ++i) {
      const QString filePath = dialog.selectedFiles().at(i);
      if (!loadFile(filePath, errorMsg)) {
        showCriticalWithDetails(QApplication::activeWindow(), tr("Can not load animation %1").arg(filePath), errorMsg);
      }
    }
  }
}

void Z3DAnimationDoc::setModified()
{
  if (auto animation = qobject_cast<Z3DAnimation*>(sender())) {
    for (const auto& idPack : m_idToAnimationPacks) {
      if (idPack.second->animation.get() == animation) {
        idPack.second->updateDerivedData();
        m_doc.updateObjInfo(idPack.first);
        return;
      }
    }
  }
}

void Z3DAnimationDoc::releaseView()
{
  for (const auto& idPack : m_idToAnimationPacks) {
    idPack.second->animation->releaseView();
  }
  m_view = nullptr;
}

size_t Z3DAnimationDoc::addAnimation(Z3DAnimation* animation, const QString& path, const QString& name)
{
  size_t id = m_doc.getNewObjId();
  m_idToAnimationPacks[id] = std::make_shared<AnimationPack>(animation, path, name);
  m_doc.registerNewObj(id, *this);
  // Keep Animation3D overlays hidden by default. This aligns the document/model state (object manager checkbox)
  // with the filter's default visibility, and avoids surprise overlays when loading or creating animations.
  m_doc.setObjVisible(id, false);
  animation->bindView(m_view);

  Q_EMIT objAdded(id, this);
  connect(animation, &Z3DAnimation::durationChanged, this, &Z3DAnimationDoc::setModified);
  connect(animation, &Z3DAnimation::keysChanged, this, &Z3DAnimationDoc::setModified);
  connect(animation, &Z3DAnimation::objChanged, this, &Z3DAnimationDoc::setModified);
  connect(animation, &Z3DAnimation::keyChanged, this, &Z3DAnimationDoc::setModified);
  connect(animation, &Z3DAnimation::colorChanged, this, &Z3DAnimationDoc::setModified);
  connect(animation, &Z3DAnimation::keyAboutToDelete, this, &Z3DAnimationDoc::setModified);
  return id;
}

Z3DAnimationDoc::AnimationPack::AnimationPack(Z3DAnimation* animation_, const QString& path_, QString name)
  : animation(animation_)
  , path(QFileInfo(path_).canonicalFilePath())
  , m_tmpName(std::move(name))
{
  updateDerivedData();
}

void Z3DAnimationDoc::AnimationPack::updateDerivedData()
{
  m_info.clear();
  m_name = path.isEmpty() ? m_tmpName : QFileInfo(path).fileName();
  m_tooltip = path;
}

const QString& Z3DAnimationDoc::AnimationPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("%1 secs").arg(animation->duration());
  }
  return m_info;
}

void Z3DAnimationDoc::createActions()
{
  m_loadAnimationsAction =
    new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("&Load 3D Animations..."), this);
  m_loadAnimationsAction->setStatusTip(tr("Load one or more existing Animation files"));
  connect(m_loadAnimationsAction, &QAction::triggered, this, &Z3DAnimationDoc::loadAnimation);
}

bool Z3DAnimationDoc::saveAnimation(AnimationPack* pack, const QString& fileName, QString& errorMsg)
{
  try {
    pack->animation->save(fileName);
    pack->path = QFileInfo(fileName).canonicalFilePath();
    pack->animation->undoStack()->setClean();
    pack->updateDerivedData();

    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return true;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return false;
  }
}

} // namespace nim
