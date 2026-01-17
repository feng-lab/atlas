#include "zrpcservice.h"

#include "scene.grpc.pb.h"
#include "zservicemanager.h"
#include "zlog.h"
#include "zdoc.h"
#include "zmainwindow.h"
#include "zview.h"
#include "z3drenderingengine.h"
#include "z3danimationdoc.h"
#include "z3danimation.h"
#include "zparameteranimation.h"
#include "zcameraparameterkey.h"
#include "zcameraparameteranimation.h"
#include "zjson.h"
#include "z3dcameraparameter.h"
#include "z3dobjview.h"
#include "zoptionparameter.h"
#include "znumericparameter.h"
#include "z3dtransformparameter.h"
#include "../version/version.h"
#include <QThread>
#include <QTimer>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QtCore/QDebug>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <grpcpp/grpcpp.h>
#include <google/protobuf/empty.pb.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/wrappers.pb.h>

namespace nim {

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

using atlas::rpc::Scene;
using atlas::rpc::PingRequest;
using atlas::rpc::PingResponse;
using atlas::rpc::Empty;
using atlas::rpc::Bool;
using atlas::rpc::FileList;
using atlas::rpc::ListObjectsResponse;
using atlas::rpc::ObjectInfo;
using atlas::rpc::ObjectLoadState;
using atlas::rpc::OBJECT_LOAD_STATE_DOC_NOT_READY;
using atlas::rpc::OBJECT_LOAD_STATE_ENGINE_NOT_READY;
using atlas::rpc::OBJECT_LOAD_STATE_NOT_FOUND;
using atlas::rpc::OBJECT_LOAD_STATE_READY;
using atlas::rpc::OBJECT_LOAD_STATE_VIEW_NOT_READY;
using atlas::rpc::ObjectStatus;
using atlas::rpc::GetStatusRequest;
using atlas::rpc::GetStatusResponse;
using atlas::rpc::WaitForObjectsReadyRequest;
using atlas::rpc::WaitForObjectsReadyResponse;
using atlas::rpc::BBoxRequest;
using atlas::rpc::BBoxResponse;
using atlas::rpc::BBox;
using atlas::rpc::Vec3;
using atlas::rpc::CapabilitiesRequest;
using atlas::rpc::CapabilitiesResponse;
using atlas::rpc::Parameter;
using atlas::rpc::ParamList;
using atlas::rpc::EnsureAnimationRequest;
using atlas::rpc::EnsureAnimationResponse;
using atlas::rpc::SetDurationRequest;
using atlas::rpc::SetKeyRequest;
using atlas::rpc::CameraFitRequest;
using atlas::rpc::CameraOrbitSuggestRequest;
using atlas::rpc::CameraDollySuggestRequest;
using atlas::rpc::CameraMoveLocalRequest;
using atlas::rpc::CameraLookAtRequest;
using atlas::rpc::CameraPathSolveRequest;
using atlas::rpc::CameraWaypoint;
using atlas::rpc::CameraKeysResponse;
using atlas::rpc::CameraFocusRequest;
using atlas::rpc::CameraPointToRequest;
using atlas::rpc::CameraRotateRequest;
using atlas::rpc::CameraResetViewRequest;
using atlas::rpc::CameraSolveRequest;
using atlas::rpc::CameraSolveResponse;
using atlas::rpc::CameraSolveKey;
using atlas::rpc::CameraConstraints;
using atlas::rpc::CameraPolicies;
using atlas::rpc::CameraValidateRequest;
using atlas::rpc::CameraValidateResponse;
using atlas::rpc::CameraValidateResult;
using atlas::rpc::FitCandidatesResponse;
using atlas::rpc::VisibilityRequest;
using atlas::rpc::ListParamsRequest;
using atlas::rpc::SetParam;
using atlas::rpc::ApplySceneParamsRequest;
using atlas::rpc::MakeAliasRequest;
using atlas::rpc::MakeAliasResponse;
using atlas::rpc::MakeAliasResult;
using atlas::rpc::ValidateSceneParamsRequest;
using atlas::rpc::ValidateSceneParamsResponse;
using atlas::rpc::ValidateSceneParamResult;
using atlas::rpc::GetParamValuesRequest;
using atlas::rpc::GetParamValuesResponse;
using atlas::rpc::SaveSceneRequest;
using atlas::rpc::ScreenshotRequest;
using atlas::rpc::ScreenshotResponse;
using atlas::rpc::ClearKeysRequest;
using atlas::rpc::RemoveKeyRequest;
using atlas::rpc::BatchSetKey;
using atlas::rpc::BatchRemoveKey;
using atlas::rpc::BatchRequest;
using atlas::rpc::SetTimeRequest;
using atlas::rpc::SetCameraInterpolationMethodRequest;
using atlas::rpc::GetCameraInterpolationMethodRequest;
using atlas::rpc::SaveAnimationRequest;
using atlas::rpc::CutSetRequest;
using atlas::rpc::CutSuggestRequest;
using atlas::rpc::CutSuggestion;
using atlas::rpc::ListKeysRequest;
using atlas::rpc::KeysResponse;
using atlas::rpc::KeyInfo;
using atlas::rpc::TimeStatus;
using atlas::rpc::GetTimeRequest;

ZRPCService::ZRPCService(QObject* parent)
  : QObject(parent)
{}

ZRPCService::~ZRPCService() = default;

namespace {

// RPC scope ids for view-setting/parameter operations.
// 0 = camera; 1 = background; 2 = axis; 3 = global/lighting; >=4 = object id
static constexpr uint64_t kScopeCamera = 0;
[[maybe_unused]] static constexpr uint64_t kScopeBackground = 1;
[[maybe_unused]] static constexpr uint64_t kScopeAxis = 2;
[[maybe_unused]] static constexpr uint64_t kScopeGlobal = 3;
static constexpr uint32_t kWaitForObjectsReadyDefaultPollMs = 50u;

template<class Func>
auto invokeOnUi(Func&& f)
{
  using R = decltype(f());
  QObject* uiObj = QCoreApplication::instance();
  // If we're already on the UI thread, invoke directly to avoid deadlock
  if (QThread::currentThread() == uiObj->thread()) {
    return f();
  }
  R result{};
  QMetaObject::invokeMethod(
    uiObj,
    [&]() {
      result = f();
    },
    Qt::BlockingQueuedConnection);
  return result;
}

template<class Func>
auto invokeOnObjectThread(QObject* obj, Func&& f)
{
  using R = decltype(f());
  CHECK(obj);
  // If we're already on the object's thread, invoke directly to avoid deadlock.
  if (QThread::currentThread() == obj->thread()) {
    return f();
  }
  R result{};
  QMetaObject::invokeMethod(
    obj,
    [&]() {
      result = f();
    },
    Qt::BlockingQueuedConnection);
  return result;
}

// Convert google.protobuf.Value to boost::json::value
static json::value pbToJson(const google::protobuf::Value& v);
static json::value pbToJson(const google::protobuf::ListValue& lv)
{
  json::array arr;
  for (const auto& item : lv.values()) {
    arr.push_back(pbToJson(item));
  }
  return arr;
}
static json::value pbToJson(const google::protobuf::Struct& st)
{
  json::object obj;
  const auto& f = st.fields();
  for (const auto& [k, vv] : f) {
    obj[k] = pbToJson(vv);
  }
  return obj;
}
static json::value pbToJson(const google::protobuf::Value& v)
{
  using K = google::protobuf::Value::KindCase;
  switch (v.kind_case()) {
    case K::kNullValue:
      return nullptr;
    case K::kBoolValue:
      return v.bool_value();
    case K::kNumberValue:
      return v.number_value();
    case K::kStringValue:
      return json::value_from(v.string_value());
    case K::kListValue:
      return pbToJson(v.list_value());
    case K::kStructValue:
      return pbToJson(v.struct_value());
    default:
      return nullptr;
  }
}

// Convert boost::json to google.protobuf.Value
static google::protobuf::Value jsonToPb(const json::value& jv);
static google::protobuf::Value jsonToPb(const json::array& arr)
{
  google::protobuf::Value v;
  auto* lv = v.mutable_list_value();
  for (const auto& it : arr) {
    *lv->add_values() = jsonToPb(it);
  }
  return v;
}
static google::protobuf::Value jsonToPb(const json::object& obj)
{
  google::protobuf::Value v;
  auto* st = v.mutable_struct_value();
  for (const auto& kv : obj) {
    (*st->mutable_fields())[kv.key_c_str()] = jsonToPb(kv.value());
  }
  return v;
}
static google::protobuf::Value jsonToPb(const json::value& jv)
{
  google::protobuf::Value v;
  if (jv.is_null()) {
    v.set_null_value(google::protobuf::NullValue::NULL_VALUE);
  } else if (jv.is_bool()) {
    v.set_bool_value(jv.as_bool());
  } else if (jv.is_double()) {
    v.set_number_value(jv.as_double());
  } else if (jv.is_int64()) {
    v.set_number_value(static_cast<double>(jv.as_int64()));
  } else if (jv.is_uint64()) {
    v.set_number_value(static_cast<double>(jv.as_uint64()));
  } else if (jv.is_string()) {
    v.set_string_value(std::string(jv.as_string()));
  } else if (jv.is_array()) {
    v = jsonToPb(jv.as_array());
  } else if (jv.is_object()) {
    v = jsonToPb(jv.as_object());
  } else {
    v.set_null_value(google::protobuf::NullValue::NULL_VALUE);
  }
  return v;
}

} // namespace

namespace {

// Helpers used by camera planning/validation
static std::vector<size_t> filterVisualIds(ZRPCService& owner, const std::vector<size_t>& in)
{
  std::vector<size_t> out;
  out.reserve(in.size());
  for (auto id : in) {
    if (owner.doc()) {
      auto* od = owner.doc()->idToDoc(id);
      if (od && od->typeName() == QStringLiteral("Animation3D")) {
        continue;
      }
    }
    out.push_back(id);
  }
  return out;
}

static ZBBox<glm::dvec3> expandedByMarginFraction(const ZBBox<glm::dvec3>& bb, double marginFrac)
{
  if (bb.empty() || marginFrac <= 0.0) {
    return bb;
  }
  const glm::dvec3 half = (bb.maxCorner - bb.minCorner) * 0.5;
  const glm::dvec3 grow = half * marginFrac;
  ZBBox<glm::dvec3> out = bb;
  out.expand(bb.minCorner - grow);
  out.expand(bb.maxCorner + grow);
  return out;
}

static double bboxEnclosingSphereRadius(const ZBBox<glm::dvec3>& bb)
{
  if (bb.empty()) {
    return 0.0;
  }
  const glm::dvec3 sz = (bb.maxCorner - bb.minCorner);
  return 0.5 * std::sqrt(sz.x * sz.x + sz.y * sz.y + sz.z * sz.z);
}

static double requiredCenterDistanceForCoverage(const Z3DCamera& cam, double radius)
{
  if (radius <= 0.0) {
    return 0.0;
  }
  double angle = cam.fieldOfView();
  // Match Z3DCamera::resetCamera logic: use horizontal angle when AR < 1
  if (cam.aspectRatio() < 1.0f) {
    angle = 2.0 * std::atan(std::tan(angle * 0.5) * cam.aspectRatio());
  }
  const double s = std::sin(angle * 0.5);
  if (s <= 1e-6) {
    return std::numeric_limits<double>::infinity();
  }
  return radius / s;
}

static void setCameraDistance(Z3DCameraParameter& cam, double centerDist)
{
  const glm::vec3 c = cam.get().center();
  const glm::vec3 v = cam.get().viewVector();
  const glm::vec3 eye = c - static_cast<float>(centerDist) * v;
  const glm::vec3 up = cam.get().upVector();
  cam.setCamera(eye, c, up);
}

static bool hasVisualObjects(ZRPCService& owner)
{
  if (!owner.doc()) {
    return false;
  }
  for (auto id : owner.doc()->objs()) {
    auto* od = owner.doc()->idToDoc(id);
    if (od && od->typeName() == QStringLiteral("Animation3D")) {
      continue;
    }
    return true;
  }
  return false;
}

struct EngineCameraAndBBoxSnapshot
{
  json::value cameraJson;
  ZBBox<glm::dvec3> bbox;
};

[[nodiscard]] EngineCameraAndBBoxSnapshot snapshotEngineCameraAndBBox(Z3DRenderingEngine* engine,
                                                                      const std::vector<size_t>& ids,
                                                                      bool afterClipping)
{
  CHECK(engine);
  return invokeOnObjectThread(engine, [&]() {
    EngineCameraAndBBoxSnapshot out{};
    out.cameraJson = engine->camera().jsonValue();
    out.bbox = afterClipping ? engine->boundBoxOfObjsAfterClipping(ids) : engine->boundBoxOfObjs(ids);
    return out;
  });
}

[[nodiscard]] json::value snapshotEngineCameraJson(Z3DRenderingEngine* engine)
{
  CHECK(engine);
  return invokeOnObjectThread(engine, [&]() -> json::value {
    return engine->camera().jsonValue();
  });
}
} // namespace

// Scene service bound to app UI context
class SceneServiceImpl final : public Scene::Service
{
public:
  explicit SceneServiceImpl(ZRPCService& owner)
    : m_owner(owner)
  {}

  Status Ping(ServerContext*, const PingRequest*, PingResponse* reply) override
  {
    LOG(INFO) << "RPC Ping";
    reply->set_ok(true);
    return Status::OK;
  }

  Status GetAppLocation(ServerContext*,
                        const google::protobuf::Empty*,
                        google::protobuf::StringValue* reply) override
  {
    // This must be UI-thread safe on all platforms, but it does not touch
    // rendering/engine state. We still compute it on the UI thread so it is
    // consistent with Qt's application/runtime state.
    const QString atlasDir = invokeOnUi([&]() -> QString {
      // applicationDirPath:
      // - macOS:   .../Atlas.app/Contents/MacOS
      // - Windows: <install>/ (contains Atlas.exe)
      // - Linux:   <install>/ (contains Atlas)
      const QString appDir = QCoreApplication::applicationDirPath();
      if (appDir.isEmpty()) {
        return QString{};
      }

#if defined(Q_OS_MAC)
      // Convert .../Atlas.app/Contents/MacOS -> .../Atlas.app
      QDir d(appDir);
      if (!d.cdUp()) { // Contents
        return appDir;
      }
      if (!d.cdUp()) { // .app
        return d.absolutePath();
      }
      return d.absolutePath();
#else
      return QDir(appDir).absolutePath();
#endif
    });
    reply->set_value(atlasDir.toStdString());
    return Status::OK;
  }

  Status GetAppVersion(ServerContext*,
                       const google::protobuf::Empty*,
                       google::protobuf::StringValue* reply) override
  {
    // Build-time version string (git describe + build timestamp).
    reply->set_value(std::string(GIT_VERSION));
    return Status::OK;
  }

  Status GetStatus(ServerContext*, const GetStatusRequest* req, GetStatusResponse* reply) override
  {
    // Collect UI-side object info (doc/type/path/visible) and determine whether a 3D window exists.
    struct UiInfo {
      uint64_t id = 0;
      bool docHasObj = false;
      std::string type;
      std::string name;
      std::string path;
      bool visible = false;
    };

    const auto ui = invokeOnUi([&]() {
      struct UiOut {
        bool docReady = false;
        bool has3dWindow = false;
        std::vector<UiInfo> objs;
      };
      UiOut out;
      out.docReady = (m_owner.doc() != nullptr);

      // Detect 3D window presence (without creating one).
      ZMainWindow* mainWin = nullptr;
      for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto mw = qobject_cast<ZMainWindow*>(w)) {
          mainWin = mw;
          break;
        }
      }
      out.has3dWindow = (mainWin && mainWin->get3DWindow() != nullptr);

      // Resolve which ids to report.
      std::vector<size_t> ids;
      if (req && req->include_all_objects()) {
        if (m_owner.doc()) {
          ids = m_owner.doc()->objs();
        }
      } else if (req) {
        ids.reserve(static_cast<size_t>(req->ids_size()));
        for (auto v : req->ids()) {
          ids.push_back(static_cast<size_t>(v));
        }
      }

      out.objs.reserve(ids.size());
      for (auto id : ids) {
        UiInfo info;
        info.id = static_cast<uint64_t>(id);
        if (m_owner.doc()) {
          if (auto* od = m_owner.doc()->idToDoc(id)) {
            info.docHasObj = true;
            info.type = od->typeName().toStdString();
            info.name = m_owner.doc()->objName(id).toStdString();
            info.path = od->objPath(id).toStdString();
            info.visible = m_owner.doc()->isObjVisible(id);
          }
        }
        out.objs.push_back(std::move(info));
      }
      return out;
    });

    reply->set_ok(true);
    reply->set_doc_ready(ui.docReady);
    reply->set_has_3d_window(ui.has3dWindow);

    Z3DRenderingEngine* eng = m_owner.engine();
    const bool engineReady = (eng != nullptr);
    reply->set_engine_ready(engineReady);

    // Determine per-object view readiness on the engine's thread (single-GL-context assumption).
    std::vector<bool> viewReady(ui.objs.size(), false);
    if (engineReady) {
      viewReady = invokeOnObjectThread(eng, [&]() {
        std::vector<bool> r;
        r.reserve(ui.objs.size());
        for (const auto& info : ui.objs) {
          const size_t id = static_cast<size_t>(info.id);
          if (id <= kScopeGlobal) {
            r.push_back(true);
            continue;
          }
          bool found = false;
          for (const auto& ov : eng->objViews()) {
            if (ov && ov->hasObj(id)) {
              found = true;
              break;
            }
          }
          r.push_back(found);
        }
        return r;
      });
    }

    for (size_t i = 0; i < ui.objs.size(); ++i) {
      const auto& info = ui.objs[i];
      ObjectStatus* st = reply->add_objects();
      st->set_id(info.id);
      st->set_type(info.type);
      st->set_name(info.name);
      st->set_path(info.path);
      st->set_visible(info.visible);

      if (!ui.docReady) {
        st->set_load_state(OBJECT_LOAD_STATE_DOC_NOT_READY);
      } else if (!info.docHasObj && static_cast<size_t>(info.id) > kScopeGlobal) {
        st->set_load_state(OBJECT_LOAD_STATE_NOT_FOUND);
      } else if (!engineReady) {
        st->set_load_state(OBJECT_LOAD_STATE_ENGINE_NOT_READY);
      } else if (i < viewReady.size() && viewReady[i]) {
        st->set_load_state(OBJECT_LOAD_STATE_READY);
      } else if (static_cast<size_t>(info.id) <= kScopeGlobal) {
        st->set_load_state(OBJECT_LOAD_STATE_READY);
      } else {
        st->set_load_state(OBJECT_LOAD_STATE_VIEW_NOT_READY);
      }
    }

    return Status::OK;
  }

  Status WaitForObjectsReady(ServerContext* context,
                             const WaitForObjectsReadyRequest* req,
                             WaitForObjectsReadyResponse* reply) override
  {
    if (!req || req->ids_size() == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "ids is required");
    }

    const uint32_t timeoutMs = req->timeout_ms();
    const uint32_t pollMsRaw = req->poll_interval_ms();
    const uint32_t pollMs = (pollMsRaw > 0u) ? pollMsRaw : kWaitForObjectsReadyDefaultPollMs;

    std::vector<uint64_t> ids;
    ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      ids.push_back(static_cast<uint64_t>(v));
    }

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeoutMs);

    auto fillReply = [&](bool doneOk, const std::string& err) {
      // Reuse GetStatus logic for consistent per-id state computation.
      GetStatusRequest statusReq;
      for (auto id : ids) {
        statusReq.add_ids(id);
      }
      GetStatusResponse statusResp;
      GetStatus(nullptr, &statusReq, &statusResp);

      reply->set_ok(doneOk);
      reply->set_doc_ready(statusResp.doc_ready());
      reply->set_engine_ready(statusResp.engine_ready());
      reply->set_has_3d_window(statusResp.has_3d_window());
      for (const auto& obj : statusResp.objects()) {
        *reply->add_objects() = obj;
      }
      if (!err.empty()) {
        reply->set_error(err);
      }
    };

    while (true) {
      if (context && context->IsCancelled()) {
        fillReply(false, "cancelled");
        return Status(grpc::StatusCode::CANCELLED, "cancelled");
      }

      // Compute readiness from a fresh status snapshot.
      GetStatusRequest statusReq;
      for (auto id : ids) {
        statusReq.add_ids(id);
      }
      GetStatusResponse statusResp;
      GetStatus(nullptr, &statusReq, &statusResp);

      bool allReady = true;
      if (!statusResp.doc_ready() || !statusResp.engine_ready()) {
        allReady = false;
      } else {
        for (const auto& obj : statusResp.objects()) {
          if (obj.load_state() != OBJECT_LOAD_STATE_READY) {
            allReady = false;
            break;
          }
        }
      }

      if (allReady) {
        fillReply(true, "");
        return Status::OK;
      }

      if (timeoutMs == 0u || std::chrono::steady_clock::now() >= deadline) {
        fillReply(false, "timeout waiting for objects to become ready");
        return Status::OK;
      }

      QThread::msleep(static_cast<unsigned long>(pollMs));
    }
  }

  Status GetParamValues(ServerContext*, const GetParamValuesRequest* req, GetParamValuesResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    // Use unified parameter access for all scopes, including camera (id=0).
    const size_t boundId = static_cast<size_t>(req->id());

    std::vector<std::string> queryKeys;
    queryKeys.reserve(static_cast<size_t>(req->json_keys_size()));
    for (const auto& qk : req->json_keys()) {
      queryKeys.push_back(qk);
    }

    const json::object j = invokeOnObjectThread(engine, [&]() {
      const auto params = engine->parametersOfViewSetting(boundId);
      json::object out;
      for (auto* p : params) {
        if (p) {
          p->write(out);
        }
      }
      return out;
    });

    if (queryKeys.empty()) {
      for (const auto& [k, v] : j) {
        (*reply->mutable_values())[std::string(k.data(), k.size())] = jsonToPb(v);
      }
    } else {
      for (const auto& qk : queryKeys) {
        if (auto* pv = j.if_contains(qk)) {
          (*reply->mutable_values())[qk] = jsonToPb(*pv);
        }
      }
    }
    return Status::OK;
  }

  Status ValidateSceneParams(ServerContext*,
                             const ValidateSceneParamsRequest* req,
                             ValidateSceneParamsResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);
    bool allOk = true;
    auto results = invokeOnObjectThread(engine, [&]() {
      std::vector<ValidateSceneParamResult> out;
      if (!m_owner.engine()) {
        return out;
      }
      auto validateOne = [&](const SetParam& sp, ValidateSceneParamResult& r) {
        r.set_json_key(sp.json_key());
        // Resolve scope via unified parameter list (id=0 => camera)
        const size_t boundId = static_cast<size_t>(sp.id());
        const auto params = engine->parametersOfViewSetting(boundId);
        // Compatibility: allow empty json_key for camera scope (id=0) and map to the single camera parameter key
        QString jsonKey = QString::fromStdString(sp.json_key());
        if (boundId == kScopeCamera && jsonKey.isEmpty()) {
          if (!params.empty() && params.front()) {
            jsonKey = params.front()->jsonKey();
          }
        }
        ZParameter* target = nullptr;
        for (auto* p : params) {
          if (p && p->jsonKey() == jsonKey) {
            target = p;
            break;
          }
        }
        if (!target) {
          r.set_ok(false);
          r.set_reason("json_key_not_found");
          return;
        }

        // Convert to json and perform light type/range checks
        json::value v = pbToJson(sp.value());
        auto tstr = target->type();

        // Strict validation for option parameters (system boundary: reject early with a soft error)
        if (auto optSI = dynamic_cast<const ZStringIntOptionParameter*>(target)) {
          if (!v.is_string()) {
            r.set_ok(false);
            r.set_reason("type_mismatch"); // expected string label
            return;
          }
          const auto& bs = v.as_string();
          const QString label = QString::fromUtf8(bs.data(), bs.size());
          if (!optSI->hasOption(label)) {
            r.set_ok(false);
            r.set_reason("option_invalid");
            return;
          }
          *r.mutable_normalized_value() = jsonToPb(json::value_from(label));
          r.set_ok(true);
          return;
        }
        if (auto optSS = dynamic_cast<const ZStringStringOptionParameter*>(target)) {
          if (!v.is_string()) {
            r.set_ok(false);
            r.set_reason("type_mismatch"); // expected string label
            return;
          }
          const auto& bs = v.as_string();
          const QString label = QString::fromUtf8(bs.data(), bs.size());
          if (!optSS->hasOption(label)) {
            r.set_ok(false);
            r.set_reason("option_invalid");
            return;
          }
          *r.mutable_normalized_value() = jsonToPb(json::value_from(label));
          r.set_ok(true);
          return;
        }
        if (auto optII = dynamic_cast<const ZIntIntOptionParameter*>(target)) {
          if (!v.is_number()) {
            r.set_ok(false);
            r.set_reason("type_mismatch"); // expected integer option
            return;
          }
          // Treat numeric JSON as integer option (clamp to nearest int)
          const int ival = static_cast<int>(std::floor(v.as_double() + 0.5));
          if (!optII->hasOption(ival)) {
            r.set_ok(false);
            r.set_reason("option_invalid");
            return;
          }
          *r.mutable_normalized_value() = jsonToPb(json::value_from(ival));
          r.set_ok(true);
          return;
        }

        // Non-option types
        auto okType = [&]() -> bool {
          if (tstr == "Bool") {
            return v.is_bool();
          }
          if (tstr == "Int") {
            return v.is_number();
          }
          if (tstr == "Float" || tstr == "Double") {
            return v.is_number();
          }
          if (tstr == "Vec2" || tstr == "DVec2") {
            return v.is_array() && v.as_array().size() == 2;
          }
          if (tstr == "Vec3" || tstr == "DVec3") {
            return v.is_array() && v.as_array().size() == 3;
          }
          if (tstr == "Vec4" || tstr == "DVec4") {
            return v.is_array() && v.as_array().size() == 4;
          }
          // Other custom types: accept and defer to target->read during apply
          return true;
        }();
        if (!okType) {
          r.set_ok(false);
          r.set_reason("type_mismatch");
          return;
        }

        // Range clamp for numeric/vector when metadata is available
        auto setNormalized = [&](const json::value& nv) {
          *r.mutable_normalized_value() = jsonToPb(nv);
        };
        bool normalized = false;
        if (auto dp = dynamic_cast<ZDoubleParameter*>(target)) {
          if (v.is_number()) {
            double x = v.as_double();
            x = std::clamp(x, dp->rangeMin(), dp->rangeMax());
            setNormalized(x);
            normalized = true;
          }
        } else if (auto fp = dynamic_cast<ZFloatParameter*>(target)) {
          if (v.is_number()) {
            double x = v.as_double();
            x = std::clamp(x, static_cast<double>(fp->rangeMin()), static_cast<double>(fp->rangeMax()));
            setNormalized(x);
            normalized = true;
          }
        } else if (auto ip = dynamic_cast<ZIntParameter*>(target)) {
          if (v.is_number()) {
            double x = v.as_double();
            x = std::clamp(x, static_cast<double>(ip->rangeMin()), static_cast<double>(ip->rangeMax()));
            setNormalized(std::floor(x + 0.5));
            normalized = true;
          }
        } else if (auto v2 = dynamic_cast<ZVec2Parameter*>(target)) {
          if (v.is_array() && v.as_array().size() == 2) {
            auto mn = v2->rangeMin();
            auto mx = v2->rangeMax();
            json::array arr;
            for (size_t i = 0; i < 2; ++i) {
              double x = v.as_array()[i].is_number() ? v.as_array()[i].as_double() : 0.0;
              x = std::clamp(x, static_cast<double>(mn[i]), static_cast<double>(mx[i]));
              arr.emplace_back(x);
            }
            setNormalized(arr);
            normalized = true;
          }
        } else if (auto v3 = dynamic_cast<ZVec3Parameter*>(target)) {
          if (v.is_array() && v.as_array().size() == 3) {
            auto mn = v3->rangeMin();
            auto mx = v3->rangeMax();
            json::array arr;
            for (size_t i = 0; i < 3; ++i) {
              double x = v.as_array()[i].is_number() ? v.as_array()[i].as_double() : 0.0;
              x = std::clamp(x, static_cast<double>(mn[i]), static_cast<double>(mx[i]));
              arr.emplace_back(x);
            }
            setNormalized(arr);
            normalized = true;
          }
        } else if (auto v4 = dynamic_cast<ZVec4Parameter*>(target)) {
          if (v.is_array() && v.as_array().size() == 4) {
            auto mn = v4->rangeMin();
            auto mx = v4->rangeMax();
            json::array arr;
            for (size_t i = 0; i < 4; ++i) {
              double x = v.as_array()[i].is_number() ? v.as_array()[i].as_double() : 0.0;
              x = std::clamp(x, static_cast<double>(mn[i]), static_cast<double>(mx[i]));
              arr.emplace_back(x);
            }
            setNormalized(arr);
            normalized = true;
          }
        }
        // Special handling for 3DTransform: accept plain field names and normalize to child jsonKeys
        if (!normalized) {
          if (auto tp = dynamic_cast<const Z3DTransformParameter*>(target)) {
            if (v.is_object()) {
              const auto& obj = v.as_object();
              json::object out;
              auto getVec = [&](const char* key1, const char* key2, size_t n) -> std::optional<json::array> {
                const json::value* pv = nullptr;
                if (auto it = obj.if_contains(key1)) {
                  pv = &*it;
                } else if (auto it2 = obj.if_contains(key2)) {
                  pv = &*it2;
                } else {
                  return std::nullopt;
                }
                if (!pv->is_array() || pv->as_array().size() != n) {
                  return std::nullopt;
                }
                json::array arr;
                for (size_t i = 0; i < n; ++i) {
                  const auto& el = pv->as_array()[i];
                  if (!el.is_number()) {
                    return std::nullopt;
                  }
                  arr.emplace_back(el.as_double());
                }
                return arr;
              };
              if (auto a = getVec("Scale", "Scale Vec3", 3)) {
                out["Scale Vec3"] = *a;
              }
              if (auto a = getVec("Translation", "Translation Vec3", 3)) {
                out["Translation Vec3"] = *a;
              }
              if (auto a = getVec("Rotation", "Rotation Vec4", 4)) {
                out["Rotation Vec4"] = *a;
              }
              // Prefer canonical "Rotation Center Vec3"; accept synonyms
              if (auto a = getVec("Rotation Center", "Rotation Center Vec3", 3)) {
                out["Rotation Center Vec3"] = *a;
              } else if (auto a2 = getVec("Center", "Center Vec3", 3)) {
                out["Rotation Center Vec3"] = *a2;
              }
              *r.mutable_normalized_value() = jsonToPb(out);
            } else {
              *r.mutable_normalized_value() = jsonToPb(v);
            }
          } else {
            *r.mutable_normalized_value() = jsonToPb(v);
          }
        }
        r.set_ok(true);
      };

      for (const auto& sp : req->set_params()) {
        ValidateSceneParamResult r;
        validateOne(sp, r);
        if (!r.ok()) {
          allOk = false;
        }
        out.push_back(std::move(r));
      }
      return out;
    });
    for (auto& r : results) {
      *reply->add_results() = std::move(r);
    }
    reply->set_ok(allOk);
    return Status::OK;
  }

  Status ApplySceneParams(ServerContext*, const ApplySceneParamsRequest* req, Bool* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);
    // Validate first to avoid partial apply
    ValidateSceneParamsRequest vreq;
    for (const auto& sp : req->set_params()) {
      *vreq.add_set_params() = sp;
    }
    ValidateSceneParamsResponse vresp;
    auto vst = ValidateSceneParams(nullptr, &vreq, &vresp);
    if (vst.error_code() != grpc::StatusCode::OK || !vresp.ok()) {
      // Build a compact reason string to aid diagnostics
      int bad = 0;
      std::string firstReason;
      std::string firstKey;
      for (const auto& r : vresp.results()) {
        if (!r.ok()) {
          ++bad;
          if (firstReason.empty()) {
            firstReason = r.reason();
            firstKey = r.json_key();
          }
        }
      }
      std::ostringstream oss;
      oss << "validate failed";
      if (bad > 0) {
        oss << ": bad=" << bad;
        if (!firstReason.empty()) {
          oss << ", first_reason=" << firstReason;
        }
        if (!firstKey.empty()) {
          oss << ", first_key=" << firstKey;
        }
      }
      return Status(grpc::StatusCode::FAILED_PRECONDITION, oss.str());
    }
    std::string applyErr;
    bool ok = invokeOnObjectThread(engine, [&]() -> bool {
      if (!m_owner.engine()) {
        return false;
      }
      for (const auto& sp : req->set_params()) {
        const size_t boundId = static_cast<size_t>(sp.id());
        // Apply using unified parameter list (id=0 => camera)
        const auto params = engine->parametersOfViewSetting(boundId);
        // Compatibility for camera: empty json_key maps to the camera parameter jsonKey
        QString jsonKey = QString::fromStdString(sp.json_key());
        if (boundId == kScopeCamera && jsonKey.isEmpty()) {
          if (!params.empty() && params.front()) {
            jsonKey = params.front()->jsonKey();
          }
        }
        ZParameter* target = nullptr;
        for (auto* p : params) {
          if (p && p->jsonKey() == jsonKey) {
            target = p;
            break;
          }
        }
        if (!target) {
          return false;
        }
        // Prefer normalized value from validation when available
        json::value v = pbToJson(sp.value());
        // Apply with normalization for composite types
        json::object j;
        if (auto tp = dynamic_cast<const Z3DTransformParameter*>(target)) {
          if (v.is_object()) {
            const auto& obj = v.as_object();
            json::object out;
            auto mapField = [&](const char* key1, const char* key2) {
              if (auto it = obj.if_contains(key1)) {
                out[key2] = *it;
              } else if (auto it2 = obj.if_contains(key2)) {
                out[key2] = *it2;
              }
            };
            mapField("Scale", "Scale Vec3");
            mapField("Translation", "Translation Vec3");
            mapField("Rotation", "Rotation Vec4");
            // Canonical center key is "Rotation Center Vec3"; accept synonyms
            mapField("Rotation Center", "Rotation Center Vec3");
            mapField("Center", "Rotation Center Vec3");
            mapField("Center Vec3", "Rotation Center Vec3");
            j[sp.json_key()] = out;
          } else {
            j[sp.json_key()] = v;
          }
        } else {
          j[sp.json_key()] = v;
        }
        try {
          target->read(j);
        }
        catch (const std::exception& e) {
          applyErr = std::string("apply_scene_params: exception while reading json_key=") + sp.json_key() + " (" +
                     target->type().toStdString() + "): " + e.what();
          LOG(WARNING) << applyErr;
          return false;
        }
        catch (...) {
          applyErr = std::string("apply_scene_params: unknown exception while reading json_key=") + sp.json_key() +
                     " (" + target->type().toStdString() + ")";
          LOG(WARNING) << applyErr;
          return false;
        }
      }
      return true;
    });
    if (!ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, applyErr.empty() ? "apply_scene_params failed" : applyErr);
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status EngineReady(ServerContext*, const Empty*, Bool* reply) override
  {
    // Wait up to ~5 seconds for engine to become ready
    for (int i = 0; i < 100; ++i) {
      if (m_owner.engine()) {
        reply->set_ok(true);
        return Status::OK;
      }
      QThread::msleep(50);
    }
    reply->set_ok(false);
    return Status::OK;
  }

  Status Ensure3DWindow(ServerContext*, const Empty*, Bool* reply) override
  {
    auto ok = invokeOnUi([&]() -> bool {
      // Find the main 2D window and ask it to open the 3D window/canvas.
      ZMainWindow* mainWin = nullptr;
      for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto mw = qobject_cast<ZMainWindow*>(w)) {
          mainWin = mw;
          break;
        }
      }
      if (!mainWin) {
        LOG(WARNING) << "Ensure3DWindow: no ZMainWindow found";
        return false;
      }
      if (!mainWin->get3DWindow()) {
        // We are already executing on the UI thread (invokeOnUi). Open synchronously
        // so clients can immediately poll EngineReady() without racing a queued call.
        mainWin->ensure3DWindow();
      }
      // if (!mainWin->isVisible()) mainWin->show();
      // mainWin->raise();
      // mainWin->activateWindow();
      return true;
    });
    reply->set_ok(ok);
    return Status::OK;
  }

  Status LoadFiles(ServerContext*, const FileList* request, ListObjectsResponse* reply) override
  {
    LOG(INFO) << "RPC LoadFiles count=" << request->files_size();
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc()) {
        return false;
      }
      QStringList qfiles;
      for (const auto& s : request->files()) {
        qfiles << QString::fromStdString(s);
      }
      m_owner.doc()->loadFileList(qfiles);
      // If an animation already exists, rebind to include newly loaded objects
      // so subsequent key writes/queries see the latest parameters.
      auto& ad = m_owner.doc()->animation3DDoc();
      auto ids = ad.animationIds();
      for (auto animId : ids) {
        if (auto* anim = ad.animationPtr(animId)) {
          anim->rebindView();
        }
      }
      return true;
    });
    if (!ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "doc not ready");
    }
    LOG(INFO) << "RPC LoadFiles done";
    return ListObjects(nullptr, nullptr, reply);
  }

  Status ListObjects(ServerContext*, const Empty*, ListObjectsResponse* reply) override
  {
    VLOG(1) << "RPC ListObjects";
    auto objs = invokeOnUi([&]() {
      std::vector<ObjectInfo> out;
      if (!m_owner.doc()) {
        return out;
      }
      for (auto id : m_owner.doc()->objs()) {
        ObjectInfo oi;
        oi.set_id(id);
        auto* od = m_owner.doc()->idToDoc(id);
        oi.set_type(od->typeName().toStdString());
        oi.set_name(m_owner.doc()->objName(id).toStdString());
        oi.set_path(od->objPath(id).toStdString());
        oi.set_visible(m_owner.doc()->isObjVisible(id));
        out.push_back(std::move(oi));
      }
      return out;
    });
    for (auto& oi : objs) {
      *reply->add_objects() = std::move(oi);
    }
    return Status::OK;
  }

  Status BBox(ServerContext*, const BBoxRequest* req, BBoxResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    const std::vector<size_t> ids = invokeOnUi([&]() {
      std::vector<size_t> ids;
      ids.reserve(req->ids_size());
      for (auto v : req->ids()) {
        ids.push_back(static_cast<size_t>(v));
      }
      return filterVisualIds(m_owner, ids);
    });

    const auto bb = invokeOnObjectThread(engine, [&]() {
      return req->after_clipping() ? engine->boundBoxOfObjsAfterClipping(ids) : engine->boundBoxOfObjs(ids);
    });
    auto toVec = [](const glm::dvec3& v) {
      Vec3 r;
      r.set_x(v.x);
      r.set_y(v.y);
      r.set_z(v.z);
      return r;
    };
    ::atlas::rpc::BBox* b = reply->mutable_bbox();
    const auto& minC = bb.minCorner;
    const auto& maxC = bb.maxCorner;
    *b->mutable_min() = toVec(minC);
    *b->mutable_max() = toVec(maxC);
    *b->mutable_size() = toVec(maxC - minC);
    *b->mutable_center() = toVec((maxC + minC) * 0.5);
    return Status::OK;
  }

  Status Capabilities(ServerContext*, const CapabilitiesRequest* req, CapabilitiesResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    struct ObjTypeId
    {
      QString typeName;
      size_t id = 0;
    };

    const std::vector<ObjTypeId> objTypeIds = invokeOnUi([&]() {
      std::vector<ObjTypeId> out;
      if (!m_owner.doc()) {
        return out;
      }
      std::vector<size_t> ids;
      if (req->ids_size() == 0) {
        ids = m_owner.doc()->objs();
      } else {
        ids.reserve(req->ids_size());
        for (auto v : req->ids()) {
          ids.push_back(static_cast<size_t>(v));
        }
      }
      out.reserve(ids.size());
      for (auto id : ids) {
        if (auto* od = m_owner.doc()->idToDoc(id)) {
          out.push_back(ObjTypeId{od->typeName(), id});
        }
      }
      return out;
    });

    auto res = invokeOnObjectThread(engine, [&]() {
      struct CapOut
      {
        std::vector<Parameter> cam, bg, ax, gl;
        std::map<QString, std::vector<Parameter>> objs;
      };
      CapOut out;
      auto collect = [&](size_t id, std::vector<Parameter>& dst) {
        const auto params = engine->parametersOfViewSetting(id);
        for (auto* p : params) {
          Parameter meta;
          meta.set_json_key(p->jsonKey().toStdString());
          meta.set_name(p->name().toStdString());
          meta.set_type(p->type().toStdString());
          if (!p->description().isEmpty()) {
            meta.set_description(p->description().toStdString());
          }
          meta.set_supports_interpolation(p->supportInterpolation());
          // Enumerated options represented in value_schema via enum; no separate fields.
          // Attach canonical value schema from parameter.
          {
            const json::object schema = p->valueSchema();
            // Dev guard: ensure schema has a recognizable 'type' for easier diagnostics.
            try {
              auto it = schema.if_contains("type");
              if (!it || !it->is_string()) {
                LOG(WARNING) << "valueSchema missing/invalid 'type' for jsonKey=" << p->jsonKey().toStdString()
                             << ", type=" << p->type().toStdString();
              }
            }
            catch (...) {
            }
            meta.mutable_value_schema()->CopyFrom(jsonToPb(schema).struct_value());
          }
          dst.push_back(std::move(meta));
        }
      };
      // Include camera (id=0) to let clients discover its schema.
      collect(0, out.cam);
      collect(1, out.bg);
      collect(2, out.ax);
      collect(3, out.gl);

      for (const auto& it : objTypeIds) {
        std::vector<Parameter> pv;
        collect(it.id, pv);
        out.objs[it.typeName] = std::move(pv);
      }
      return out;
    });
    for (auto& p : res.cam) {
      *reply->add_camera() = std::move(p);
    }
    for (auto& p : res.bg) {
      *reply->add_background() = std::move(p);
    }
    for (auto& p : res.ax) {
      *reply->add_axis() = std::move(p);
    }
    for (auto& p : res.gl) {
      *reply->add_global() = std::move(p);
    }
    for (auto& [tn, pv] : res.objs) {
      ParamList lst;
      for (auto& p : pv) {
        *lst.add_params() = std::move(p);
      }
      (*reply->mutable_objects())[tn.toStdString()] = std::move(lst);
    }
    return Status::OK;
  }

  Status MakeAlias(ServerContext*, const MakeAliasRequest* req, MakeAliasResponse* reply) override
  {
    if (!m_owner.doc()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "doc not ready");
    }
    bool hadInvalidId = false;
    bool hadUnsupported = false;
    auto pairs = invokeOnUi([&]() {
      std::vector<std::pair<uint64_t, uint64_t>> out;
      if (!m_owner.doc()) {
        hadInvalidId = true;
        return out;
      }
      for (auto srcId64 : req->ids()) {
        const size_t srcId = static_cast<size_t>(srcId64);
        ZObjDoc* od = m_owner.doc()->idToDoc(srcId);
        if (!od) {
          // System boundary: record invalid id and continue.
          hadInvalidId = true;
          continue;
        }
        const size_t aliasId = od->makeAlias(srcId);
        if (aliasId == 0) {
          // Type does not support aliasing or alias creation failed.
          hadUnsupported = true;
          continue;
        }
        out.emplace_back(srcId64, static_cast<uint64_t>(aliasId));
      }
      return out;
    });
    for (const auto& p : pairs) {
      MakeAliasResult* r = reply->add_aliases();
      r->set_src_id(p.first);
      r->set_alias_id(p.second);
    }
    const bool ok = !pairs.empty() && !hadInvalidId && !hadUnsupported;
    reply->set_ok(ok);
    if (!ok) {
      if (hadInvalidId) {
        reply->set_error("one or more ids were not found in the current document");
      } else if (hadUnsupported) {
        reply->set_error("one or more ids do not support aliasing");
      }
    }
    return Status::OK;
  }

  Status EnsureAnimation(ServerContext*, const EnsureAnimationRequest* req, EnsureAnimationResponse* reply) override
  {
    LOG(INFO) << "RPC EnsureAnimation create_new=" << req->create_new()
              << (req->name().empty() ? "" : (" name=" + req->name()));
    uint64_t animationId = 0;
    bool created = false;
    std::string errMsg;
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc()) {
        errMsg = "doc not ready";
        return false;
      }
      auto& ad = m_owner.doc()->animation3DDoc();
      auto ids = ad.animationIds();

      if (!req->create_new() && !ids.empty()) {
        animationId = static_cast<uint64_t>(ids.front());
        return true;
      }

      if (!hasVisualObjects(m_owner)) {
        errMsg = "no visual objects loaded";
        return false;
      }

      const QString rawName = QString::fromStdString(req->name()).trimmed();
      const QString name = rawName.isEmpty() ? QStringLiteral("LLM Animation") : rawName;
      animationId = static_cast<uint64_t>(ad.createNewAnimationAndReturnId(name));
      created = true;
      return animationId != 0;
    });
    if (!ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, errMsg.empty() ? "ensure_animation failed" : errMsg);
    }
    reply->set_ok(true);
    reply->set_animation_id(animationId);
    reply->set_created(created);
    return Status::OK;
  }

  Status SetDuration(ServerContext*, const SetDurationRequest* req, Bool* reply) override
  {
    LOG(INFO) << "RPC SetDuration duration=" << req->duration();
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc()) {
        return false;
      }
      auto& ad = m_owner.doc()->animation3DDoc();
      if (auto* anim = ad.animationPtr(static_cast<size_t>(animationId))) {
        anim->setDuration(req->duration());
        return true;
      }
      return false;
    });
    if (!ok) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id not found");
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status SetKey(ServerContext*, const SetKeyRequest* req, Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    LOG(INFO) << "RPC SetKey anim=" << animationId << " time=" << req->time() << " easing=" << req->easing()
              << " target_id=" << req->target_id()
              << (req->json_key().empty() ? "" : (" json_key=" + req->json_key()));

    struct ParamMeta
    {
      bool found = false;
      QString type;
      bool supportsInterpolation = false;
    };

    std::string errMsg;
    // Camera (id=0) does not require engine parameter lookup.
    if (req->target_id() == kScopeCamera) {
      auto ok = invokeOnUi([&]() -> bool {
        if (!m_owner.doc() || !m_owner.engine()) {
          errMsg = "engine/doc not ready";
          return false;
        }
        auto& ad = m_owner.doc()->animation3DDoc();
        auto* anim = ad.animationPtr(static_cast<size_t>(animationId));
        if (!anim) {
          errMsg = "animation_id not found";
          return false;
        }
        anim->rebindView();
        const double tm = req->time();
        const QString easing = QString::fromStdString(req->easing());
        json::value jv = pbToJson(req->value());
        if (!jv.is_object()) {
          errMsg = "camera value must be an object";
          return false;
        }
        json::object keyObj;
        keyObj["time"] = tm;
        keyObj["type"] = easing.toStdString();
        keyObj["value"] = jv.as_object();
        auto ckey = std::make_unique<ZCameraParameterKey>();
        if (!ckey->readValue(keyObj)) {
          errMsg = "camera value incompatible with parameter";
          return false;
        }
        anim->cameraParameterAnimation()->addKey(std::move(ckey));
        return true;
      });
      if (!ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, errMsg.empty() ? "set_key failed" : errMsg);
      }
      reply->set_ok(true);
      return Status::OK;
    }

    // Non-camera: validate animation on the UI thread first (preserve existing error precedence).
    auto preOk = invokeOnUi([&]() -> bool {
      if (!m_owner.doc() || !m_owner.engine()) {
        errMsg = "engine/doc not ready";
        return false;
      }
      auto& ad = m_owner.doc()->animation3DDoc();
      auto* anim = ad.animationPtr(static_cast<size_t>(animationId));
      if (!anim) {
        errMsg = "animation_id not found";
        return false;
      }
      return true;
    });
    if (!preOk) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, errMsg.empty() ? "set_key failed" : errMsg);
    }

    // Snapshot parameter metadata on the engine's thread.
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);
    const size_t boundId = static_cast<size_t>(req->target_id());
    const QString jsonKey = QString::fromStdString(req->json_key());

    const ParamMeta meta = invokeOnObjectThread(engine, [&]() {
      ParamMeta out;
      const auto params = engine->parametersOfViewSetting(boundId);
      for (auto* p : params) {
        if (p && p->jsonKey() == jsonKey) {
          out.found = true;
          out.type = p->type();
          out.supportsInterpolation = p->supportInterpolation();
          break;
        }
      }
      return out;
    });

    if (!meta.found) {
      LOG(WARNING) << "SetKey(param): target parameter not found boundId=" << boundId
                   << " jsonKey=" << jsonKey.toStdString();
      return Status(grpc::StatusCode::FAILED_PRECONDITION,
                    std::string("target parameter not found for json_key=") + jsonKey.toStdString());
    }

    const double tm = req->time();
    const QString easing = QString::fromStdString(req->easing());
    const json::value v = pbToJson(req->value());

    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc() || !m_owner.engine()) {
        errMsg = "engine/doc not ready";
        return false;
      }
      auto& ad = m_owner.doc()->animation3DDoc();
      auto* anim = ad.animationPtr(static_cast<size_t>(animationId));
      if (!anim) {
        errMsg = "animation_id not found";
        return false;
      }
      anim->rebindView();

      // Ensure parameter animations exist for this object.
      anim->addKeyFrame(0.0);

      auto jsonTypeName = [](const json::value& j) -> std::string {
        if (j.is_null()) {
          return "null";
        }
        if (j.is_bool()) {
          return "bool";
        }
        if (j.is_string()) {
          return "string";
        }
        if (j.is_number()) {
          return "number";
        }
        if (j.is_array()) {
          return std::string("array[") + std::to_string(j.as_array().size()) + "]";
        }
        if (j.is_object()) {
          return "object";
        }
        return "unknown";
      };
      auto expectArrayN = [&](size_t n) -> bool {
        if (!v.is_array() || v.as_array().size() != n) {
          errMsg = std::string("type mismatch for json_key=") + jsonKey.toStdString() + ": expected array[" +
                   std::to_string(n) + "] got " + jsonTypeName(v);
          return false;
        }
        for (const auto& el : v.as_array()) {
          if (!el.is_number()) {
            errMsg = std::string("type mismatch for json_key=") + jsonKey.toStdString() + ": expected number elements";
            return false;
          }
        }
        return true;
      };

      const QString tstr = meta.type;
      if (tstr == "Bool") {
        if (!v.is_bool()) {
          errMsg = std::string("type mismatch for json_key=") + jsonKey.toStdString() + ": expected Bool got " +
                   jsonTypeName(v);
          return false;
        }
      } else if (tstr == "Float" || tstr == "Double" || tstr == "Int") {
        if (!v.is_number()) {
          errMsg = std::string("type mismatch for json_key=") + jsonKey.toStdString() + ": expected number got " +
                   jsonTypeName(v);
          return false;
        }
      } else if (tstr.endsWith("Vec4")) {
        if (!expectArrayN(4)) {
          return false;
        }
      } else if (tstr.endsWith("Vec3")) {
        if (!expectArrayN(3)) {
          return false;
        }
      } else if (tstr.endsWith("Option")) {
        if (!v.is_string()) {
          errMsg = std::string("type mismatch for json_key=") + jsonKey.toStdString() +
                   ": expected option string got " + jsonTypeName(v);
          return false;
        }
      }

      // For non-interpolatable parameters, force "Switch" to avoid invalid easing selection.
      const QString keyType = meta.supportsInterpolation ? easing : QStringLiteral("Switch");
      json::object keyObj;
      keyObj["time"] = tm;
      keyObj["type"] = keyType.toStdString();
      keyObj["value"] = v;
      auto key = std::make_unique<ZParameterKey>(meta.type);
      if (!key->readValue(keyObj)) {
        errMsg = "value incompatible with parameter schema";
        return false;
      }

      // Locate the object's unique id from display packs, then access its parameter animations.
      size_t uniqueId = 0;
      for (const auto& pack : anim->displayPacks()) {
        if (pack.type == ZAnimationDisplayPack::Type::Object && pack.boundId == boundId) {
          uniqueId = pack.id;
          break;
        }
      }
      if (uniqueId == 0) {
        LOG(WARNING) << "SetKey(param): object pack not found for boundId=" << boundId;
        errMsg = "object pack not found for boundId";
        return false;
      }

      const auto& pas = anim->paraAnimationList(uniqueId);
      for (const auto& paPtr : pas) {
        if (!paPtr) {
          continue;
        }
        auto* pa = paPtr.get();
        if (pa->jsonKey() == jsonKey && pa->type() == meta.type) {
          pa->addKey(std::move(key));
          return true;
        }
      }
      LOG(WARNING) << "SetKey(param): failed to locate paraAnimation for boundId=" << boundId
                   << " jsonKey=" << jsonKey.toStdString();
      errMsg = "failed to locate parameter animation for json_key";
      return false;
    });
    if (!ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, errMsg.empty() ? "set_key failed" : errMsg);
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status CameraGet(ServerContext*, const google::protobuf::Empty*, CameraKeysResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);
    *reply->add_values() = jsonToPb(snapshotEngineCameraJson(engine));
    return Status::OK;
  }

  Status CameraFit(ServerContext*, const CameraFitRequest* req, CameraKeysResponse* reply) override
  {
    VLOG(1) << "RPC CameraFit all=" << req->all() << " ids=" << req->ids_size();
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    // Gather and filter ids on the UI thread (doc lives there).
    const std::vector<size_t> ids = invokeOnUi([&]() {
      std::vector<size_t> ids;
      if (req->all() || req->ids_size() == 0) {
        if (m_owner.doc()) {
          ids = filterVisualIds(m_owner, m_owner.doc()->objs());
        }
      } else {
        ids.reserve(req->ids_size());
        for (auto v : req->ids()) {
          ids.push_back(static_cast<size_t>(v));
        }
        ids = filterVisualIds(m_owner, ids);
      }
      return ids;
    });

    // Snapshot engine-side data on the rendering thread.
    EngineCameraAndBBoxSnapshot snap = snapshotEngineCameraAndBBox(engine, ids, req->after_clipping());
    ZBBox<glm::dvec3> bb = snap.bbox;
    if (bb.empty()) {
      return Status::OK;
    }

    // Apply min radius by expanding bbox if requested.
    if (req->min_radius() > 0.0) {
      const auto cent = (bb.minCorner + bb.maxCorner) * 0.5;
      const double r = req->min_radius();
      bb.expand(ZBBox<glm::dvec3>{cent - glm::dvec3(r), cent + glm::dvec3(r)});
    }

    CHECK(snap.cameraJson.is_object());
    Z3DCameraParameter camTmp("Camera");
    camTmp.readValue(snap.cameraJson);
    camTmp.resetCamera(bb, Z3DCamera::ResetOption::PreserveViewVector);

    *reply->add_values() = jsonToPb(camTmp.jsonValue());
    return Status::OK;
  }

  Status CameraOrbitSuggest(ServerContext*, const CameraOrbitSuggestRequest* req, CameraKeysResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    const std::vector<size_t> ids = invokeOnUi([&]() {
      std::vector<size_t> ids;
      if (req->ids_size() == 0) {
        if (m_owner.doc()) {
          ids = filterVisualIds(m_owner, m_owner.doc()->objs());
        }
      } else {
        ids.reserve(req->ids_size());
        for (auto v : req->ids()) {
          ids.push_back(static_cast<size_t>(v));
        }
        ids = filterVisualIds(m_owner, ids);
      }
      return ids;
    });

    EngineCameraAndBBoxSnapshot snap = snapshotEngineCameraAndBBox(engine, ids, /*afterClipping=*/false);
    const ZBBox<glm::dvec3> bb = snap.bbox;
    if (bb.empty()) {
      return Status::OK;
    }

    const glm::vec3 center = glm::vec3((bb.minCorner + bb.maxCorner) * 0.5);
    const double angle = (req->degrees() == 0.0 ? 360.0 : req->degrees());
    // Axis
    glm::vec3 axis(0.f, 1.f, 0.f);
    const auto ax = QString::fromStdString(req->axis()).toLower();
    if (ax == "x") {
      axis = glm::vec3(1.f, 0.f, 0.f);
    } else if (ax == "y") {
      axis = glm::vec3(0.f, 1.f, 0.f);
    } else if (ax == "z") {
      axis = glm::vec3(0.f, 0.f, 1.f);
    }

    CHECK(snap.cameraJson.is_object());
    // Start: fit to bbox preserving view vector, using a temp camera seeded with the engine camera.
    Z3DCameraParameter startCam("Camera");
    startCam.readValue(snap.cameraJson);
    startCam.resetCamera(bb, Z3DCamera::ResetOption::PreserveViewVector);
    // End: rotate around center on the selected axis.
    Z3DCameraParameter endCam("Camera");
    endCam.setValueSameAs(startCam);
    // Z3DCamera expects radians for rotation angles.
    endCam.rotate(glm::radians(static_cast<float>(angle)), axis, center);

    *reply->add_values() = jsonToPb(startCam.jsonValue());
    *reply->add_values() = jsonToPb(endCam.jsonValue());
    return Status::OK;
  }

  Status CameraDollySuggest(ServerContext*, const CameraDollySuggestRequest* req, CameraKeysResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    const std::vector<size_t> ids = invokeOnUi([&]() {
      std::vector<size_t> ids;
      if (req->ids_size() == 0) {
        if (m_owner.doc()) {
          ids = filterVisualIds(m_owner, m_owner.doc()->objs());
        }
      } else {
        ids.reserve(req->ids_size());
        for (auto v : req->ids()) {
          ids.push_back(static_cast<size_t>(v));
        }
        ids = filterVisualIds(m_owner, ids);
      }
      return ids;
    });

    EngineCameraAndBBoxSnapshot snap = snapshotEngineCameraAndBBox(engine, ids, /*afterClipping=*/false);
    const ZBBox<glm::dvec3> bb = snap.bbox;
    if (bb.empty()) {
      return Status::OK;
    }
    const glm::vec3 center = glm::vec3((bb.minCorner + bb.maxCorner) * 0.5);

    CHECK(snap.cameraJson.is_object());
    Z3DCameraParameter startCam("Camera");
    startCam.readValue(snap.cameraJson);
    startCam.setCenter(center);
    if (req->start_dist() > 0.0) {
      startCam.dollyToCenterDistance(static_cast<float>(req->start_dist()));
    }

    Z3DCameraParameter endCam("Camera");
    endCam.setValueSameAs(startCam);
    if (req->end_dist() > 0.0) {
      endCam.dollyToCenterDistance(static_cast<float>(req->end_dist()));
    }

    *reply->add_values() = jsonToPb(startCam.jsonValue());
    *reply->add_values() = jsonToPb(endCam.jsonValue());
    return Status::OK;
  }

  Status CameraFocus(ServerContext*, const CameraFocusRequest* req, CameraKeysResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    const std::vector<size_t> ids = invokeOnUi([&]() {
      std::vector<size_t> ids;
      ids.reserve(req->ids_size());
      for (auto v : req->ids()) {
        ids.push_back(static_cast<size_t>(v));
      }
      return filterVisualIds(m_owner, ids);
    });

    EngineCameraAndBBoxSnapshot snap = snapshotEngineCameraAndBBox(engine, ids, req->after_clipping());
    ZBBox<glm::dvec3> bb = snap.bbox;
    if (bb.empty()) {
      return Status::OK;
    }

    if (req->min_radius() > 0.0) {
      const auto cent = (bb.minCorner + bb.maxCorner) * 0.5;
      bb.expand(ZBBox<glm::dvec3>{cent - glm::dvec3(req->min_radius()), cent + glm::dvec3(req->min_radius())});
    }

    CHECK(snap.cameraJson.is_object());
    Z3DCameraParameter cam("Camera");
    cam.readValue(snap.cameraJson);
    cam.resetCamera(bb, Z3DCamera::ResetOption::PreserveViewVector);

    *reply->add_values() = jsonToPb(cam.jsonValue());
    return Status::OK;
  }

  Status CameraPointTo(ServerContext*, const CameraPointToRequest* req, CameraKeysResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    const std::vector<size_t> ids = invokeOnUi([&]() {
      std::vector<size_t> ids;
      ids.reserve(req->ids_size());
      for (auto v : req->ids()) {
        ids.push_back(static_cast<size_t>(v));
      }
      return filterVisualIds(m_owner, ids);
    });

    EngineCameraAndBBoxSnapshot snap = snapshotEngineCameraAndBBox(engine, ids, req->after_clipping());
    const ZBBox<glm::dvec3> bb = snap.bbox;
    if (bb.empty()) {
      return Status::OK;
    }

    CHECK(snap.cameraJson.is_object());
    Z3DCameraParameter cam("Camera");
    cam.readValue(snap.cameraJson);
    cam.setCenter(glm::vec3((bb.minCorner + bb.maxCorner) * 0.5));

    *reply->add_values() = jsonToPb(cam.jsonValue());
    return Status::OK;
  }

  Status CameraRotate(ServerContext*, const CameraRotateRequest* req, CameraKeysResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    const json::value baseJson = snapshotEngineCameraJson(engine);
    CHECK(baseJson.is_object());

    Z3DCameraParameter cam("Camera");
    cam.readValue(baseJson);
    if (req->has_base_value()) {
      json::value jv = pbToJson(req->base_value());
      if (jv.is_object()) {
        cam.readValue(jv);
      }
    }
    const auto op = QString::fromStdString(req->op()).toUpper();
    const float angle = glm::radians(static_cast<float>(req->degrees()));
    if (op == "AZIMUTH") {
      cam.azimuth(angle);
    } else if (op == "ELEVATION") {
      cam.elevation(angle);
    } else if (op == "ROLL") {
      cam.roll(angle);
    } else if (op == "YAW") {
      cam.yaw(angle);
    } else if (op == "PITCH") {
      cam.pitch(angle);
    } else if (op == "FLIP") {
      cam.flipViewDirection();
    }

    *reply->add_values() = jsonToPb(cam.jsonValue());
    return Status::OK;
  }

  Status CameraResetView(ServerContext*, const CameraResetViewRequest* req, CameraKeysResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    const std::vector<size_t> ids = invokeOnUi([&]() {
      // Determine bbox (ids → filtered; empty → all visual).
      std::vector<size_t> ids;
      ids.reserve(req->ids_size());
      for (auto v : req->ids()) {
        ids.push_back(static_cast<size_t>(v));
      }
      if (ids.empty()) {
        if (m_owner.doc()) {
          ids = filterVisualIds(m_owner, m_owner.doc()->objs());
        }
      } else {
        ids = filterVisualIds(m_owner, ids);
      }
      return ids;
    });

    EngineCameraAndBBoxSnapshot snap = snapshotEngineCameraAndBBox(engine, ids, req->after_clipping());
    ZBBox<glm::dvec3> bb = snap.bbox;
    if (bb.empty()) {
      return Status::OK;
    }
    if (req->min_radius() > 0.0 && (req->mode() == std::string("XY") || req->mode() == std::string("RESET"))) {
      const auto cent = (bb.minCorner + bb.maxCorner) * 0.5;
      bb.expand(ZBBox<glm::dvec3>(cent - glm::dvec3(req->min_radius()), cent + glm::dvec3(req->min_radius())));
    }

    CHECK(snap.cameraJson.is_object());
    Z3DCameraParameter cam("Camera");
    cam.readValue(snap.cameraJson);
    const auto mode = QString::fromStdString(req->mode()).toUpper();
    if (mode == "XY" || mode == "RESET" || mode.isEmpty()) {
      cam.resetCamera(bb, Z3DCamera::ResetOption::ResetAll);
    } else if (mode == "XZ") {
      cam.resetCamera(bb, Z3DCamera::ResetOption::ResetAll);
      cam.rotate90X();
    } else if (mode == "YZ") {
      cam.resetCamera(bb, Z3DCamera::ResetOption::ResetAll);
      cam.rotate90XZ();
    } else {
      cam.resetCamera(bb, Z3DCamera::ResetOption::ResetAll);
    }

    *reply->add_values() = jsonToPb(cam.jsonValue());
    return Status::OK;
  }

  Status CameraMoveLocal(ServerContext*, const CameraMoveLocalRequest* req, CameraKeysResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    const QString op = QString::fromStdString(req->op()).toUpper().trimmed();
    if (op.isEmpty()) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "op is required");
    }
    if (!(op == "FORWARD" || op == "BACK" || op == "RIGHT" || op == "LEFT" || op == "UP" || op == "DOWN")) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid op (expected FORWARD|BACK|RIGHT|LEFT|UP|DOWN)");
    }
    if (!std::isfinite(req->distance())) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "distance must be finite");
    }

    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    const json::value baseEngineCamera = snapshotEngineCameraJson(engine);
    CHECK(baseEngineCamera.is_object());

    // Base camera (current engine camera, optionally overridden by base_value).
    Z3DCameraParameter cam("Camera");
    cam.readValue(baseEngineCamera);
    if (req->has_base_value()) {
      json::value jv = pbToJson(req->base_value());
      if (!jv.is_object()) {
        return Status(grpc::StatusCode::INVALID_ARGUMENT, "base_value must be an object");
      }
      cam.readValue(jv);
    }

    double distWorld = req->distance();
    if (req->distance_is_fraction_of_bbox_radius()) {
      // Determine bbox radius from ids (or all visual objects when ids is empty).
      bool docReady = false;
      const std::vector<size_t> ids = invokeOnUi([&]() {
        std::vector<size_t> ids;
        if (!m_owner.doc()) {
          docReady = false;
          return ids;
        }
        docReady = true;
        ids.reserve(req->ids_size());
        for (auto v : req->ids()) {
          ids.push_back(static_cast<size_t>(v));
        }
        if (ids.empty()) {
          ids = filterVisualIds(m_owner, m_owner.doc()->objs());
        } else {
          ids = filterVisualIds(m_owner, ids);
        }
        return ids;
      });
      if (!docReady) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, "doc not ready");
      }

      const ZBBox<glm::dvec3> bb = invokeOnObjectThread(engine, [&]() {
        return req->after_clipping() ? engine->boundBoxOfObjsAfterClipping(ids) : engine->boundBoxOfObjs(ids);
      });
      if (bb.empty()) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, "bbox empty");
      }
      const double r = bboxEnclosingSphereRadius(bb);
      if (!(r > 0.0)) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, "bbox radius is zero");
      }
      distWorld = distWorld * r;
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
    // move_center=true implements "fly": translate eye+center together.
    // move_center=false implements "boom/dolly": translate eye only.
    eye += delta;
    if (req->move_center()) {
      center += delta;
    }
    cam.setCamera(eye, center, up);

    *reply->add_values() = jsonToPb(cam.jsonValue());
    return Status::OK;
  }

  Status CameraLookAt(ServerContext*, const CameraLookAtRequest* req, CameraKeysResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    if (req->target_case() == CameraLookAtRequest::TARGET_NOT_SET) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "target is required");
    }

    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    const json::value baseEngineCamera = snapshotEngineCameraJson(engine);
    CHECK(baseEngineCamera.is_object());

    // Base camera
    Z3DCameraParameter cam("Camera");
    cam.readValue(baseEngineCamera);
    if (req->has_base_value()) {
      json::value jv = pbToJson(req->base_value());
      if (!jv.is_object()) {
        return Status(grpc::StatusCode::INVALID_ARGUMENT, "base_value must be an object");
      }
      cam.readValue(jv);
    }

    auto bboxForReq = [&]() -> std::optional<ZBBox<glm::dvec3>> {
      bool docReady = false;
      const std::vector<size_t> ids = invokeOnUi([&]() {
        std::vector<size_t> ids;
        if (!m_owner.doc()) {
          docReady = false;
          return ids;
        }
        docReady = true;
        ids.reserve(req->ids_size());
        for (auto v : req->ids()) {
          ids.push_back(static_cast<size_t>(v));
        }
        if (ids.empty()) {
          ids = filterVisualIds(m_owner, m_owner.doc()->objs());
        } else {
          ids = filterVisualIds(m_owner, ids);
        }
        return ids;
      });
      if (!docReady) {
        return std::nullopt;
      }
      ZBBox<glm::dvec3> bb = invokeOnObjectThread(engine, [&]() {
        return req->after_clipping() ? engine->boundBoxOfObjsAfterClipping(ids) : engine->boundBoxOfObjs(ids);
      });
      return bb;
    };

    glm::vec3 target(0.f, 0.f, 0.f);
    switch (req->target_case()) {
      case CameraLookAtRequest::kWorldPoint: {
        const auto& p = req->world_point();
        target = glm::vec3(static_cast<float>(p.x()), static_cast<float>(p.y()), static_cast<float>(p.z()));
        break;
      }
      case CameraLookAtRequest::kTargetBboxCenter: {
        auto bbOpt = bboxForReq();
        if (!bbOpt.has_value()) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, "doc not ready");
        }
        const auto& bb = *bbOpt;
        if (bb.empty()) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, "bbox empty");
        }
        const glm::dvec3 c = (bb.minCorner + bb.maxCorner) * 0.5;
        target = glm::vec3(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z));
        break;
      }
      case CameraLookAtRequest::kBboxFractionPoint: {
        auto bbOpt = bboxForReq();
        if (!bbOpt.has_value()) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, "doc not ready");
        }
        const auto& bb = *bbOpt;
        if (bb.empty()) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, "bbox empty");
        }
        const auto& f = req->bbox_fraction_point();
        const auto clamp01 = [](double v) { return std::max(0.0, std::min(1.0, v)); };
        const double fx = clamp01(f.x());
        const double fy = clamp01(f.y());
        const double fz = clamp01(f.z());
        const glm::dvec3 sz = (bb.maxCorner - bb.minCorner);
        const glm::dvec3 p = bb.minCorner + glm::dvec3(fx * sz.x, fy * sz.y, fz * sz.z);
        target = glm::vec3(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
        break;
      }
      default: {
        return Status(grpc::StatusCode::INVALID_ARGUMENT, "unsupported target");
      }
    }

    // Aim: keep eye and up vector; set center to target.
    cam.setCamera(cam.get().eye(), target, cam.get().upVector());
    *reply->add_values() = jsonToPb(cam.jsonValue());
    return Status::OK;
  }

  Status CameraPathSolve(ServerContext*, const CameraPathSolveRequest* req, CameraSolveResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    if (req->waypoints_size() == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "waypoints must be non-empty");
    }

    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    const json::value baseEngineCamera = snapshotEngineCameraJson(engine);
    CHECK(baseEngineCamera.is_object());

    // Base camera for defaults (projection/fov/up); can be overridden.
    Z3DCameraParameter base("Camera");
    base.readValue(baseEngineCamera);
    if (req->has_base_value()) {
      json::value jv = pbToJson(req->base_value());
      if (!jv.is_object()) {
        return Status(grpc::StatusCode::INVALID_ARGUMENT, "base_value must be an object");
      }
      base.readValue(jv);
    }

    const auto clamp01 = [](double v) { return std::max(0.0, std::min(1.0, v)); };
    auto fracToWorld = [&](const ZBBox<glm::dvec3>& bb, const Vec3& f) -> glm::vec3 {
      const double fx = clamp01(f.x());
      const double fy = clamp01(f.y());
      const double fz = clamp01(f.z());
      const glm::dvec3 sz = (bb.maxCorner - bb.minCorner);
      const glm::dvec3 p = bb.minCorner + glm::dvec3(fx * sz.x, fy * sz.y, fz * sz.z);
      return glm::vec3(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
    };

    // Determine whether bbox is needed (fractions or bbox-center look-at).
    bool needBBox = false;
    for (int i = 0; i < req->waypoints_size(); ++i) {
      const auto& w = req->waypoints(i);
      if (w.eye_case() == CameraWaypoint::kBboxFractionEye || w.look_at_case() == CameraWaypoint::kLookAtBboxCenter ||
          w.look_at_case() == CameraWaypoint::kBboxFractionLookAt) {
        needBBox = true;
        break;
      }
    }

    ZBBox<glm::dvec3> bb;
    if (needBBox) {
      bool docReady = false;
      const std::vector<size_t> ids = invokeOnUi([&]() {
        std::vector<size_t> ids;
        if (!m_owner.doc()) {
          docReady = false;
          return ids;
        }
        docReady = true;
        ids.reserve(req->ids_size());
        for (auto v : req->ids()) {
          ids.push_back(static_cast<size_t>(v));
        }
        if (ids.empty()) {
          ids = filterVisualIds(m_owner, m_owner.doc()->objs());
        } else {
          ids = filterVisualIds(m_owner, ids);
        }
        return ids;
      });
      if (!docReady) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, "doc not ready");
      }
      bb = invokeOnObjectThread(engine, [&]() {
        return req->after_clipping() ? engine->boundBoxOfObjsAfterClipping(ids) : engine->boundBoxOfObjs(ids);
      });
      if (bb.empty()) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, "bbox empty");
      }
    }

    struct WpRef
    {
      const CameraWaypoint* wp = nullptr;
      double time = 0.0;
      int index = 0;
    };
    std::vector<WpRef> wps;
    wps.reserve(static_cast<size_t>(req->waypoints_size()));
    for (int i = 0; i < req->waypoints_size(); ++i) {
      wps.push_back(WpRef{&req->waypoints(i), req->waypoints(i).time(), i});
    }
    std::stable_sort(wps.begin(), wps.end(), [](const WpRef& a, const WpRef& b) { return a.time < b.time; });

    std::vector<CameraSolveKey> keys;
    keys.reserve(wps.size());

    Z3DCameraParameter prev("Camera");
    prev.setValueSameAs(base);
    bool havePrev = false;

    for (const auto& wr : wps) {
      const auto* w = wr.wp;
      if (!w) {
        continue;
      }
      const double t = w->time();
      if (!(t >= 0.0) || !std::isfinite(t)) {
        return Status(grpc::StatusCode::INVALID_ARGUMENT, "waypoint time must be finite and >= 0");
      }

      glm::vec3 eye = havePrev ? prev.get().eye() : base.get().eye();
      if (w->eye_case() == CameraWaypoint::kWorldEye) {
        const auto& p = w->world_eye();
        eye = glm::vec3(static_cast<float>(p.x()), static_cast<float>(p.y()), static_cast<float>(p.z()));
      } else if (w->eye_case() == CameraWaypoint::kBboxFractionEye) {
        if (bb.empty()) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, "bbox required for bbox_fraction_eye");
        }
        eye = fracToWorld(bb, w->bbox_fraction_eye());
      }

      glm::vec3 center;
      if (w->look_at_case() == CameraWaypoint::kWorldLookAt) {
        const auto& p = w->world_look_at();
        center = glm::vec3(static_cast<float>(p.x()), static_cast<float>(p.y()), static_cast<float>(p.z()));
      } else if (w->look_at_case() == CameraWaypoint::kLookAtBboxCenter) {
        if (bb.empty()) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, "bbox required for look_at_bbox_center");
        }
        const glm::dvec3 c = (bb.minCorner + bb.maxCorner) * 0.5;
        center = glm::vec3(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z));
      } else if (w->look_at_case() == CameraWaypoint::kBboxFractionLookAt) {
        if (bb.empty()) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, "bbox required for bbox_fraction_look_at");
        }
        center = fracToWorld(bb, w->bbox_fraction_look_at());
      } else {
        // Default: keep previous view direction and center distance (first key uses base).
        const glm::vec3 view = havePrev ? prev.get().viewVector() : base.get().viewVector();
        const float dist = havePrev ? prev.get().centerDist() : base.get().centerDist();
        center = eye + view * dist;
      }

      const glm::vec3 up = havePrev ? prev.get().upVector() : base.get().upVector();

      Z3DCameraParameter cam("Camera");
      cam.setValueSameAs(base);
      cam.setCamera(eye, center, up);

      CameraSolveKey k;
      k.set_time(t);
      *k.mutable_value() = jsonToPb(cam.jsonValue());
      keys.push_back(std::move(k));

      prev.setValueSameAs(cam);
      havePrev = true;
    }

    for (auto& k : keys) {
      *reply->add_keys() = std::move(k);
    }
    return Status::OK;
  }

  Status FitCandidates(ServerContext*, const Empty*, FitCandidatesResponse* reply) override
  {
    auto ids = invokeOnUi([&]() {
      std::vector<uint64_t> out;
      if (!m_owner.doc()) {
        return out;
      }
      auto filtered = filterVisualIds(m_owner, m_owner.doc()->objs());
      out.reserve(filtered.size());
      for (auto id : filtered) {
        out.push_back(static_cast<uint64_t>(id));
      }
      return out;
    });
    for (auto id : ids) {
      reply->add_ids(id);
    }
    return Status::OK;
  }

  Status CameraSolve(ServerContext*, const CameraSolveRequest* req, CameraSolveResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    // Collect and filter ids on the UI thread (doc lives there).
    const std::vector<size_t> ids = invokeOnUi([&]() {
      std::vector<size_t> ids;
      ids.reserve(req->ids_size());
      for (auto v : req->ids()) {
        ids.push_back(static_cast<size_t>(v));
      }
      return filterVisualIds(m_owner, ids);
    });

    EngineCameraAndBBoxSnapshot snap = snapshotEngineCameraAndBBox(engine, ids, /*afterClipping=*/false);
    ZBBox<glm::dvec3> bb = snap.bbox;
    const double margin = req->has_constraints() ? req->constraints().margin() : 0.0;
    if (!bb.empty() && margin > 0.0) {
      bb = expandedByMarginFraction(bb, margin);
    }

    const auto mode = QString::fromStdString(req->mode()).toUpper();
    const double t0 = req->t0();
    const double t1 = req->t1();

    CHECK(snap.cameraJson.is_object());
    Z3DCameraParameter base("Camera");
    base.readValue(snap.cameraJson);

    std::vector<CameraSolveKey> keys;

    if (mode == "FIT") {
      if (bb.empty()) {
        // Nothing to fit; leave keys empty so the RPC reports an empty solve.
      } else {
        Z3DCameraParameter cam("Camera");
        cam.setValueSameAs(base);
        cam.resetCamera(bb, Z3DCamera::ResetOption::PreserveViewVector);
        CameraSolveKey k;
        k.set_time(t0);
        *k.mutable_value() = jsonToPb(cam.jsonValue());
        keys.push_back(std::move(k));
      }
    } else if (mode == "STATIC") {
      CameraSolveKey k;
      k.set_time(t0);
      *k.mutable_value() = jsonToPb(base.jsonValue());
      keys.push_back(std::move(k));
    } else if (mode == "ORBIT") {
      if (bb.empty()) {
        // No bbox; leave keys empty so the RPC reports an empty solve.
      } else {
        const auto params = req->params();
        std::string axis = "y";
        double angle = 360.0;
        auto itA = params.fields().find("axis");
        if (itA != params.fields().end() && itA->second.kind_case() == google::protobuf::Value::kStringValue) {
          axis = itA->second.string_value();
        }
        auto itAng = params.fields().find("degrees");
        if (itAng != params.fields().end() && itAng->second.kind_case() == google::protobuf::Value::kNumberValue) {
          angle = itAng->second.number_value();
        }
        const glm::vec3 center = glm::vec3((bb.minCorner + bb.maxCorner) * 0.5);
        glm::vec3 ax(0.f, 1.f, 0.f);
        const auto axq = QString::fromStdString(axis).toLower();
        if (axq == "x") {
          ax = glm::vec3(1.f, 0.f, 0.f);
        } else if (axq == "y") {
          ax = glm::vec3(0.f, 1.f, 0.f);
        } else if (axq == "z") {
          ax = glm::vec3(0.f, 0.f, 1.f);
        }
        // Build a segmented orbit to avoid the 360° wrap producing identical endpoints.
        // Normalize 0/±360° (or multiples) to a full 360° sweep with segments.
        double angDeg = angle;
        const double sign = (angDeg >= 0.0 ? 1.0 : -1.0);
        double aabs = std::abs(angDeg);
        const double eps = 1e-6;
        // Treat multiples of 360 as 360 to ensure motion.
        if (std::abs(std::fmod(aabs, 360.0)) < eps) {
          aabs = 360.0;
          angDeg = sign * aabs;
        }
        // Segment into <=90° steps for stable interpolation.
        int segments = std::max(1, static_cast<int>(std::ceil(aabs / 90.0)));
        const double stepDeg = angDeg / static_cast<double>(segments);
        const double dt = (t1 - t0) / static_cast<double>(segments);

        Z3DCameraParameter cam("Camera");
        cam.setValueSameAs(base);
        cam.resetCamera(bb, Z3DCamera::ResetOption::PreserveViewVector);
        // Emit first key (start).
        {
          CameraSolveKey k;
          k.set_time(t0);
          *k.mutable_value() = jsonToPb(cam.jsonValue());
          keys.push_back(std::move(k));
        }
        // Apply incremental rotations and emit intermediate keys.
        for (int i = 1; i <= segments; ++i) {
          cam.rotate(glm::radians(static_cast<float>(stepDeg)), ax, center);
          CameraSolveKey k;
          k.set_time(t0 + dt * static_cast<double>(i));
          *k.mutable_value() = jsonToPb(cam.jsonValue());
          keys.push_back(std::move(k));
        }
      }
    } else if (mode == "DOLLY") {
      if (bb.empty()) {
        // No bbox; leave keys empty so the RPC reports an empty solve.
      } else {
        const auto params = req->params();
        double start_dist = 0.0, end_dist = 0.0;
        auto itS = params.fields().find("start_dist");
        if (itS != params.fields().end() && itS->second.kind_case() == google::protobuf::Value::kNumberValue) {
          start_dist = itS->second.number_value();
        }
        auto itE = params.fields().find("end_dist");
        if (itE != params.fields().end() && itE->second.kind_case() == google::protobuf::Value::kNumberValue) {
          end_dist = itE->second.number_value();
        }
        const glm::vec3 center = glm::vec3((bb.minCorner + bb.maxCorner) * 0.5);
        Z3DCameraParameter cam0("Camera");
        cam0.setValueSameAs(base);
        cam0.setCenter(center);
        if (start_dist > 0.0) {
          cam0.dollyToCenterDistance(static_cast<float>(start_dist));
        }
        Z3DCameraParameter cam1("Camera");
        cam1.setValueSameAs(cam0);
        if (end_dist > 0.0) {
          cam1.dollyToCenterDistance(static_cast<float>(end_dist));
        }
        CameraSolveKey k0;
        k0.set_time(t0);
        *k0.mutable_value() = jsonToPb(cam0.jsonValue());
        keys.push_back(std::move(k0));
        CameraSolveKey k1;
        k1.set_time(t1);
        *k1.mutable_value() = jsonToPb(cam1.jsonValue());
        keys.push_back(std::move(k1));
      }
    }

    for (auto& k : keys) {
      *reply->add_keys() = std::move(k);
    }
    if (reply->keys_size() == 0) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "camera_solve failed or empty");
    }
    return Status::OK;
  }

  Status CameraValidate(ServerContext*, const CameraValidateRequest* req, CameraValidateResponse* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    if (req->times_size() == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "times must be non-empty");
    }
    const uint64_t sampleAnimationId = req->animation_id();
    if (req->values_size() < req->times_size() && sampleAnimationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT,
                    "animation_id is required when values are omitted (values_size < times_size)");
    }
    if (sampleAnimationId != 0) {
      bool docReady = false;
      const bool animExists = invokeOnUi([&]() -> bool {
        if (!m_owner.doc()) {
          docReady = false;
          return false;
        }
        docReady = true;
        auto& ad = m_owner.doc()->animation3DDoc();
        return ad.animationPtr(static_cast<size_t>(sampleAnimationId)) != nullptr;
      });
      if (!docReady) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, "doc not ready");
      }
      if (!animExists) {
        return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id not found");
      }
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);
    const bool allowFov = req->has_policies() ? req->policies().adjust_fov() : false;
    const bool allowDist = req->has_policies() ? req->policies().adjust_distance() : false;
    const bool keepVisible = req->has_constraints() ? req->constraints().keep_visible() : true;
    const double minCov = keepVisible
                            ? (req->has_constraints()
                                 ? (req->constraints().min_coverage() > 0.0 ? req->constraints().min_coverage() : 0.95)
                                 : 0.95)
                            : 0.0;
    const double margin = req->has_constraints() ? req->constraints().margin() : 0.0;
    // Collect targets on the UI thread (doc lives there).
    const std::vector<size_t> ids = invokeOnUi([&]() {
      // Default to all visual objects when ids is empty.
      std::vector<size_t> ids;
      ids.reserve(req->ids_size());
      for (auto v : req->ids()) {
        ids.push_back(static_cast<size_t>(v));
      }
      if (ids.empty()) {
        if (m_owner.doc()) {
          ids = filterVisualIds(m_owner, m_owner.doc()->objs());
        }
      } else {
        ids = filterVisualIds(m_owner, ids);
      }
      return ids;
    });

    // Snapshot engine-side data on the rendering thread.
    EngineCameraAndBBoxSnapshot snap = snapshotEngineCameraAndBBox(engine, ids, /*afterClipping=*/false);
    ZBBox<glm::dvec3> bb = snap.bbox;
    if (!bb.empty() && margin > 0.0) {
      bb = expandedByMarginFraction(bb, margin);
    }
    const double R = bboxEnclosingSphereRadius(bb);

    CHECK(snap.cameraJson.is_object());
    Z3DCameraParameter base("Camera");
    base.readValue(snap.cameraJson);

    // Prepare times and values pairs. Be permissive:
    // - If values are missing (size==0) but times are provided, sample values from the specified animation camera track.
    // - If counts mismatch, pair up to times.size(); fill missing values by sampling, and ignore extras.
    std::vector<double> times;
    times.reserve(req->times_size());
    for (int i = 0; i < req->times_size(); ++i) {
      times.push_back(req->times(i));
    }

    std::vector<json::value> values;
    values.reserve(static_cast<size_t>(std::max(req->values_size(), req->times_size())));
    for (int i = 0; i < req->values_size(); ++i) {
      values.push_back(pbToJson(req->values(i)));
    }

    const size_t provided = values.size();
    if (provided < times.size()) {
      const auto sampled = invokeOnUi([&]() {
        std::vector<json::value> out;
        out.reserve(times.size() - provided);

        // Default to the current engine camera when no animation/doc is available.
        if (!m_owner.doc()) {
          for (size_t i = provided; i < times.size(); ++i) {
            out.push_back(snap.cameraJson);
          }
          return out;
        }

        auto& ad = m_owner.doc()->animation3DDoc();
        const size_t animId = static_cast<size_t>(sampleAnimationId);
        auto* anim = ad.animationPtr(animId);
        if (!anim) {
          LOG(WARNING) << "CameraValidate: animation_id=" << animId << " not found while sampling";
          for (size_t i = provided; i < times.size(); ++i) {
            out.push_back(snap.cameraJson);
          }
          return out;
        }

        ZCameraParameterAnimation* cpa = anim->cameraParameterAnimation();
        if (!cpa) {
          for (size_t i = provided; i < times.size(); ++i) {
            out.push_back(snap.cameraJson);
          }
          return out;
        }

        for (size_t i = provided; i < times.size(); ++i) {
          Z3DCameraParameter tmp("Camera");
          tmp.readValue(snap.cameraJson);
          cpa->updateParaToTime(times[i], &tmp);
          out.push_back(tmp.jsonValue());
        }
        return out;
      });
      values.insert(values.end(), sampled.begin(), sampled.end());
    }

    std::vector<CameraValidateResult> results;
    const int n = static_cast<int>(std::min(times.size(), values.size()));
    results.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const double t = times[i];
      const json::value& jv = values[i];
      CameraValidateResult r;
      r.set_time(t);
      if (!jv.is_object()) {
        r.set_within_frame(false);
        r.set_coverage(0.0);
        r.set_adjusted(false);
        r.set_reason("invalid_value");
        results.push_back(std::move(r));
        continue;
      }
      Z3DCameraParameter cam("Camera");
      cam.setValueSameAs(base);
      try {
        cam.readValue(jv);
      }
      catch (const std::exception& e) {
        LOG(WARNING) << "CameraValidate: exception while parsing camera value at t=" << t << ": " << e.what();
        r.set_within_frame(false);
        r.set_coverage(0.0);
        r.set_adjusted(false);
        r.set_reason("invalid_value");
        results.push_back(std::move(r));
        continue;
      }
      catch (...) {
        LOG(WARNING) << "CameraValidate: unknown exception while parsing camera value at t=" << t;
        r.set_within_frame(false);
        r.set_coverage(0.0);
        r.set_adjusted(false);
        r.set_reason("invalid_value");
        results.push_back(std::move(r));
        continue;
      }
      // Compute coverage heuristic.
      const double required = (R > 0.0) ? requiredCenterDistanceForCoverage(cam.get(), R) : 0.0;
      const double current = static_cast<double>(cam.get().centerDist());
      double cov = 1.0;
      if (required > 1e-9) {
        cov = std::min(1.0, current / required);
      }
      bool ok = (cov + 1e-6) >= minCov;
      r.set_within_frame(ok);
      r.set_coverage(cov);

      // Adjustment policy.
      bool adjusted = false;
      json::value adj = jv;
      if (!ok && R > 0.0) {
        if (allowDist && required > 0.0) {
          setCameraDistance(cam, required);
          adjusted = true;
          adj = cam.jsonValue();
          // Recompute coverage after adjustment.
          const double cur2 = static_cast<double>(cam.get().centerDist());
          const double cov2 = (required > 0.0) ? std::min(1.0, cur2 / required) : 1.0;
          ok = (cov2 + 1e-6) >= minCov;
          cov = cov2;
        } else if (allowFov && current > 1e-9) {
          // Solve desired FOV to achieve coverage with current distance.
          double angleUsed = 2.0 * std::asin(std::min(1.0, R / current));
          double desiredFov = angleUsed;
          if (cam.get().aspectRatio() < 1.0f) {
            // angleUsed is horizontal; convert back to vertical FOV.
            desiredFov = 2.0 * std::atan(std::tan(angleUsed * 0.5) / cam.get().aspectRatio());
          }
          Z3DCameraParameter cam2("Camera");
          cam2.setValueSameAs(cam);
          cam2.setFrustum(static_cast<float>(desiredFov),
                          cam.get().aspectRatio(),
                          cam.get().nearDist(),
                          cam.get().farDist());
          adjusted = true;
          adj = cam2.jsonValue();
          // Recompute coverage.
          const double req2 = requiredCenterDistanceForCoverage(cam2.get(), R);
          const double cur2 = static_cast<double>(cam2.get().centerDist());
          const double cov2 = (req2 > 1e-9) ? std::min(1.0, cur2 / req2) : 1.0;
          ok = (cov2 + 1e-6) >= minCov;
          cov = cov2;
        }
      }
      r.set_adjusted(adjusted);
      if (adjusted) {
        *r.mutable_adjusted_value() = jsonToPb(adj);
      }
      if (!ok) {
        if (current < required) {
          r.set_reason(allowDist ? "too_close" : (allowFov ? "fov_too_small" : "coverage_below_threshold"));
        } else {
          r.set_reason("coverage_below_threshold");
        }
      } else {
        r.set_reason("");
      }
      results.push_back(std::move(r));
    }
    bool allOk = true;
    for (auto& r : results) {
      if (!r.within_frame() || (keepVisible && (r.coverage() + 1e-6) < minCov)) {
        allOk = false;
      }
      *reply->add_results() = std::move(r);
    }
    reply->set_ok(allOk);
    if (!allOk) {
      return Status::OK; // strict acceptance handled client-side via revalidate
    }
    return Status::OK;
  }

  Status SetVisibility(ServerContext*, const VisibilityRequest* req, Bool* reply) override
  {
    bool ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc()) {
        return false;
      }
      for (auto id : req->ids()) {
        m_owner.doc()->setObjVisible(static_cast<size_t>(id), req->on());
      }
      return true;
    });
    if (!ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "doc not ready");
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status ListParams(ServerContext*, const ListParamsRequest* req, ParamList* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);
    auto list = invokeOnObjectThread(engine, [&]() {
      std::vector<Parameter> out;
      if (!m_owner.engine()) {
        return out;
      }
      size_t boundId = static_cast<size_t>(req->id());
      const auto params = engine->parametersOfViewSetting(boundId);
      for (auto* p : params) {
        if (!p) {
          continue;
        }
        Parameter meta;
        meta.set_json_key(p->jsonKey().toStdString());
        meta.set_name(p->name().toStdString());
        meta.set_type(p->type().toStdString());
        if (!p->description().isEmpty()) {
          meta.set_description(p->description().toStdString());
        }
        meta.set_supports_interpolation(p->supportInterpolation());
        // Enumerated options represented in value_schema via enum; no separate fields.
        // Value schema from parameter (fail-fast if invariant breaks)
        {
          const json::object schema = p->valueSchema();
          // Dev guard: ensure schema has a recognizable 'type'
          try {
            auto it = schema.if_contains("type");
            if (!it || !it->is_string()) {
              LOG(WARNING) << "valueSchema missing/invalid 'type' for jsonKey=" << p->jsonKey().toStdString()
                           << ", type=" << p->type().toStdString();
            }
          }
          catch (...) {
          }
          meta.mutable_value_schema()->CopyFrom(jsonToPb(schema).struct_value());
        }
        out.push_back(std::move(meta));
      }
      return out;
    });
    for (auto& p : list) {
      *reply->add_params() = std::move(p);
    }
    return Status::OK;
  }

  Status ClearKeys(ServerContext*, const ClearKeysRequest* req, Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    LOG(INFO) << "RPC ClearKeys anim=" << animationId << " target_id=" << req->target_id()
              << (req->json_key().empty() ? "" : (" json_key=" + req->json_key()));
    std::string errMsg;
    grpc::StatusCode errCode = grpc::StatusCode::FAILED_PRECONDITION;
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc() || !m_owner.engine()) {
        errMsg = "engine/doc not ready";
        return false;
      }
      auto& ad = m_owner.doc()->animation3DDoc();
      auto* anim = ad.animationPtr(static_cast<size_t>(animationId));
      if (!anim) {
        errCode = grpc::StatusCode::INVALID_ARGUMENT;
        errMsg = "animation_id not found";
        return false;
      }
      anim->rebindView();
      // Camera (id=0): clear all camera keys
      if (req->target_id() == kScopeCamera) {
        ZParameterAnimation* cpa = anim->cameraParameterAnimation();
        // delete keys one by one
        std::vector<ZParameterKey*> keys;
        keys.reserve(cpa->keys().size());
        for (const auto& k : cpa->keys()) {
          keys.push_back(k.get());
        }
        for (auto* k : keys) {
          cpa->deleteKey(k);
        }
        cpa->emitKeysChangedSignal();
        return true;
      }
      // Non-camera: resolve the object's uniqueId, then operate on its parameter animations
      size_t boundId = static_cast<size_t>(req->target_id());
      const QString jsonKey = QString::fromStdString(req->json_key());
      size_t uniqueId = 0;
      for (const auto& pack : anim->displayPacks()) {
        if (pack.type == ZAnimationDisplayPack::Type::Object && pack.boundId == boundId) {
          uniqueId = pack.id;
          break;
        }
      }
      bool clearedAny = false;
      if (uniqueId != 0) {
        const auto& pas = anim->paraAnimationList(uniqueId);
        for (const auto& paPtr : pas) {
          if (!paPtr) {
            continue;
          }
          auto* pa = paPtr.get();
          if (jsonKey.isEmpty() || pa->jsonKey() == jsonKey) {
            std::vector<ZParameterKey*> keys;
            keys.reserve(pa->keys().size());
            for (const auto& k : pa->keys()) {
              keys.push_back(k.get());
            }
            for (auto* k : keys) {
              pa->deleteKey(k);
            }
            pa->emitKeysChangedSignal();
            clearedAny = true;
            if (!jsonKey.isEmpty()) {
              // When a specific param was requested, stop after clearing its keys
              break;
            }
          }
        }
      }
      // Benign no-op: if no keys existed for the target, report success to simplify client logic
      if (!clearedAny) {
        if (jsonKey.isEmpty()) {
          LOG(INFO) << "RPC ClearKeys: no param tracks for id=" << boundId << "; nothing to clear";
        } else {
          LOG(INFO) << "RPC ClearKeys: no matching track for id=" << boundId << " json_key=" << req->json_key();
        }
      }
      return true;
    });
    if (!ok) {
      return Status(errCode, errMsg.empty() ? "clear_keys failed" : errMsg);
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status RemoveKey(ServerContext*, const RemoveKeyRequest* req, Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    LOG(INFO) << "RPC RemoveKey anim=" << animationId << " time=" << req->time() << " target_id=" << req->target_id()
              << (req->json_key().empty() ? "" : (" json_key=" + req->json_key()));
    std::string errMsg;
    grpc::StatusCode errCode = grpc::StatusCode::FAILED_PRECONDITION;
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc() || !m_owner.engine()) {
        errMsg = "engine/doc not ready";
        return false;
      }
      auto& ad = m_owner.doc()->animation3DDoc();
      auto* anim = ad.animationPtr(static_cast<size_t>(animationId));
      if (!anim) {
        errCode = grpc::StatusCode::INVALID_ARGUMENT;
        errMsg = "animation_id not found";
        return false;
      }
      anim->rebindView();
      // Camera (id=0)
      if (req->target_id() == kScopeCamera) {
        ZParameterAnimation* cpa = anim->cameraParameterAnimation();
        for (const auto& k : cpa->keys()) {
          if (std::abs(k->time() - req->time()) < 1e-6) {
            cpa->deleteKey(k.get());
            cpa->emitKeysChangedSignal();
            return true;
          }
        }
        return false;
      }
      // Non-camera
      size_t boundId = static_cast<size_t>(req->target_id());
      const QString jsonKey = QString::fromStdString(req->json_key());
      for (const auto& pack : anim->displayPacks()) {
        if (pack.type == ZAnimationDisplayPack::Type::ObjectPara && pack.boundId == boundId && pack.paraAnimation &&
            pack.paraAnimation->jsonKey() == jsonKey) {
          for (const auto& k : pack.paraAnimation->keys()) {
            if (std::abs(k->time() - req->time()) < 1e-6) {
              pack.paraAnimation->deleteKey(k.get());
              pack.paraAnimation->emitKeysChangedSignal();
              return true;
            }
          }
        }
      }
      return false;
    });
    if (!ok) {
      return Status(errCode, errMsg.empty() ? "remove_key failed" : errMsg);
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status Batch(ServerContext*, const BatchRequest* req, Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    std::string errMsg;
    grpc::StatusCode errCode = grpc::StatusCode::FAILED_PRECONDITION;
    auto ok = invokeOnUi([&]() -> bool {
      LOG(INFO) << "RPC Batch anim=" << animationId << " set_keys=" << req->set_keys_size()
                << " remove_keys=" << req->remove_keys_size()
                << " commit=" << req->commit();
      if (!m_owner.doc() || !m_owner.engine()) {
        errMsg = "engine/doc not ready";
        return false;
      }
      auto& ad = m_owner.doc()->animation3DDoc();
      auto* anim = ad.animationPtr(static_cast<size_t>(animationId));
      if (!anim) {
        errCode = grpc::StatusCode::INVALID_ARGUMENT;
        errMsg = "animation_id not found";
        return false;
      }
      anim->rebindView();
      // Process removals first
      int idx = 0;
      for (const auto& r : req->remove_keys()) {
        RemoveKeyRequest tmp;
        tmp.set_animation_id(animationId);
        tmp.set_target_id(r.target_id());
        tmp.set_json_key(r.json_key());
        tmp.set_time(r.time());
        Bool okR;
        auto st = RemoveKey(nullptr, &tmp, &okR);
        if (st.error_code() != grpc::StatusCode::OK || !okR.ok()) {
          LOG(WARNING) << "Batch: RemoveKey failed at index=" << idx << " code=" << st.error_code();
          return false;
        }
        ++idx;
      }
      // Then sets
      idx = 0;
      for (const auto& s : req->set_keys()) {
        SetKeyRequest tmp;
        tmp.set_animation_id(animationId);
        tmp.set_target_id(s.target_id());
        tmp.set_json_key(s.json_key());
        tmp.set_time(s.time());
        tmp.set_easing(s.easing());
        tmp.mutable_value()->CopyFrom(s.value());
        Bool okS;
        auto st = SetKey(nullptr, &tmp, &okS);
        if (st.error_code() != grpc::StatusCode::OK || !okS.ok()) {
          LOG(WARNING) << "Batch: SetKey failed at index=" << idx << " code=" << st.error_code();
          return false;
        }
        ++idx;
      }

      // Commit semantics: if requested, immediately apply t=0 keys so UI reflects
      // initial state without requiring a separate SetTime call. We intentionally
      // do not jump the timeline for non-zero keys.
      if (req->commit()) {
        bool hasTimeZero = false;
        for (const auto& s : req->set_keys()) {
          if (std::abs(s.time()) < 1e-9) {
            hasTimeZero = true;
            break;
          }
        }
        if (hasTimeZero) {
          anim->cancelRenderingAndSetCurrentTime(0.0);
        }
      }
      return true;
    });
    if (!ok) {
      return Status(errCode, errMsg.empty() ? "batch failed" : errMsg);
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status SetTime(ServerContext*, const SetTimeRequest* req, Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    VLOG(1) << "RPC SetTime anim=" << animationId << " seconds=" << req->seconds() << " cancel=" << req->cancel_rendering();
    std::string errMsg;
    grpc::StatusCode errCode = grpc::StatusCode::FAILED_PRECONDITION;
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc()) {
        errMsg = "doc not ready";
        return false;
      }
      auto& ad = m_owner.doc()->animation3DDoc();
      auto* anim = ad.animationPtr(static_cast<size_t>(animationId));
      if (!anim) {
        errCode = grpc::StatusCode::INVALID_ARGUMENT;
        errMsg = "animation_id not found";
        return false;
      }
      if (req->cancel_rendering()) {
        anim->cancelRenderingAndSetCurrentTime(req->seconds());
      } else {
        anim->setCurrentTime(req->seconds());
      }
      return true;
    });
    if (!ok) {
      return Status(errCode, errMsg.empty() ? "set_time failed" : errMsg);
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status SetCameraInterpolationMethod(ServerContext*,
                                      const SetCameraInterpolationMethodRequest* req,
                                      Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    LOG(INFO) << "RPC SetCameraInterpolationMethod anim=" << animationId << " method=" << req->method();
    std::string errMsg;
    grpc::StatusCode errCode = grpc::StatusCode::FAILED_PRECONDITION;
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc() || !m_owner.engine()) {
        errMsg = "engine/doc not ready";
        return false;
      }
      auto& ad = m_owner.doc()->animation3DDoc();
      auto* anim = ad.animationPtr(static_cast<size_t>(animationId));
      if (!anim) {
        errCode = grpc::StatusCode::INVALID_ARGUMENT;
        errMsg = "animation_id not found";
        return false;
      }
      anim->rebindView();
      auto* cpa = anim->cameraParameterAnimation();
      if (!cpa) {
        errMsg = "cameraParameterAnimation not available";
        return false;
      }

      const QString raw = QString::fromStdString(req->method()).trimmed();
      if (raw.isEmpty()) {
        errCode = grpc::StatusCode::INVALID_ARGUMENT;
        errMsg = "method is required";
        return false;
      }
      const QString m = raw.toLower();
      QString want;
      if (m == "center") {
        want = QStringLiteral("Center");
      } else {
        // Temporarily disable spline-based interpolation modes. They are known to
        // be unstable in some workflows and are not exposed via the agent tool
        // surface. Fail fast with a clear message rather than silently accepting.
        errCode = grpc::StatusCode::FAILED_PRECONDITION;
        errMsg = "camera interpolation methods other than Center are temporarily disabled";
        return false;
      }

      auto& para = cpa->interpolationMethodPara();
      if (!para.hasOption(want)) {
        errCode = grpc::StatusCode::FAILED_PRECONDITION;
        errMsg = "interpolation method option unavailable";
        return false;
      }
      para.select(want);
      return true;
    });
    if (!ok) {
      return Status(errCode, errMsg.empty() ? "set_camera_interpolation_method failed" : errMsg);
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status GetCameraInterpolationMethod(ServerContext*,
                                      const GetCameraInterpolationMethodRequest* req,
                                      google::protobuf::StringValue* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    std::string errMsg;
    grpc::StatusCode errCode = grpc::StatusCode::FAILED_PRECONDITION;
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc() || !m_owner.engine()) {
        errMsg = "engine/doc not ready";
        return false;
      }
      auto& ad = m_owner.doc()->animation3DDoc();
      auto* anim = ad.animationPtr(static_cast<size_t>(animationId));
      if (!anim) {
        errCode = grpc::StatusCode::INVALID_ARGUMENT;
        errMsg = "animation_id not found";
        return false;
      }
      anim->rebindView();
      auto* cpa = anim->cameraParameterAnimation();
      if (!cpa) {
        errMsg = "cameraParameterAnimation not available";
        return false;
      }
      const QString sel = cpa->interpolationMethodPara().get();
      reply->set_value(sel.toStdString());
      return true;
    });
    if (!ok) {
      return Status(errCode, errMsg.empty() ? "get_camera_interpolation_method failed" : errMsg);
    }
    return Status::OK;
  }

  Status ListKeys(ServerContext*, const ListKeysRequest* req, KeysResponse* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    if (!m_owner.doc()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "doc not ready");
    }
    grpc::StatusCode errCode = grpc::StatusCode::FAILED_PRECONDITION;
    std::string errMsg;
    auto ok = invokeOnUi([&]() -> bool {
      auto& ad = m_owner.doc()->animation3DDoc();
      auto* anim = ad.animationPtr(static_cast<size_t>(animationId));
      if (!anim) {
        errCode = grpc::StatusCode::INVALID_ARGUMENT;
        errMsg = "animation_id not found";
        return false;
      }
      anim->rebindView();
      const bool includeValues = req->include_values();
      if (req->target_id() == 0) {
        ZParameterAnimation* cpa = anim->cameraParameterAnimation();
        for (const auto& k : cpa->keys()) {
          KeyInfo* ki = reply->add_keys();
          ki->set_time(k->time());
          ki->set_type("3DCamera");
          if (includeValues) {
            ki->set_value_json(jsonToString(k->jsonValue()));
          }
        }
        return true;
      }
      // Non-camera: resolve uniqueId for the object, then fetch parameter animation by json_key
      const QString jsonKey = QString::fromStdString(req->json_key());
      const size_t boundId = static_cast<size_t>(req->target_id());
      size_t uniqueId = 0;
      for (const auto& pack : anim->displayPacks()) {
        if (pack.type == ZAnimationDisplayPack::Type::Object && pack.boundId == boundId) {
          uniqueId = pack.id;
          break;
        }
      }
      if (uniqueId == 0) {
        // Object not bound; return empty list
        return true;
      }
      const auto& pas = anim->paraAnimationList(uniqueId);
      for (const auto& paPtr : pas) {
        if (!paPtr) {
          continue;
        }
        auto* pa = paPtr.get();
        if (pa->jsonKey() == jsonKey) {
          for (const auto& k : pa->keys()) {
            KeyInfo* ki = reply->add_keys();
            ki->set_time(k->time());
            ki->set_type(pa->type().toStdString());
            if (includeValues) {
              ki->set_value_json(jsonToString(k->jsonValue()));
            }
          }
          break;
        }
      }
      return true; // return empty when no matching param track
    });
    if (!ok) {
      return Status(errCode, errMsg.empty() ? "list_keys failed" : errMsg);
    }
    return Status::OK;
  }

  Status GetTime(ServerContext*, const GetTimeRequest* req, TimeStatus* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    std::string error;
    grpc::StatusCode errCode = grpc::StatusCode::FAILED_PRECONDITION;
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc()) {
        error = "doc not ready";
        return false;
      }
      auto& ad = m_owner.doc()->animation3DDoc();
      auto* anim = ad.animationPtr(static_cast<size_t>(animationId));
      if (!anim) {
        errCode = grpc::StatusCode::INVALID_ARGUMENT;
        error = "animation_id not found";
        return false;
      }
      // duration is straightforward; current time is stored in animation
      reply->set_duration(anim->duration());
      reply->set_seconds(anim->currentTime());
      return true;
    });
    if (!ok) {
      if (error.empty()) {
        error = "get_time failed";
      }
      return Status(errCode, error);
    }
    return Status::OK;
  }

  Status SaveAnimation(ServerContext*, const SaveAnimationRequest* req, Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    if (req->path().empty()) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "path is required");
    }
    LOG(INFO) << "RPC SaveAnimation anim=" << animationId << " path=" << req->path();
    std::string errMsg;
    grpc::StatusCode errCode = grpc::StatusCode::FAILED_PRECONDITION;
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc()) {
        errMsg = "doc not ready";
        return false;
      }
      auto& ad = m_owner.doc()->animation3DDoc();
      auto* anim = ad.animationPtr(static_cast<size_t>(animationId));
      if (!anim) {
        errCode = grpc::StatusCode::INVALID_ARGUMENT;
        errMsg = "animation_id not found";
        return false;
      }
      // Mirror UI behavior: use Z3DAnimationDoc save to update name/path state
      const QString qpath = QString::fromStdString(req->path());
      if (qpath.trimmed().isEmpty()) {
        errCode = grpc::StatusCode::INVALID_ARGUMENT;
        errMsg = "path is empty";
        return false;
      }
      return ad.saveToPath(static_cast<size_t>(animationId), qpath);
    });
    if (!ok) {
      return Status(errCode, errMsg.empty() ? "save_animation failed" : errMsg);
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status SaveScene(ServerContext*, const SaveSceneRequest* req, Bool* reply) override
  {
    LOG(INFO) << "RPC SaveScene path=" << req->path();
    auto ok = invokeOnUi([&]() -> bool {
      // Find the main window and gather state similar to ZMainWindow::saveJsonSceneImpl
      ZMainWindow* mainWin = nullptr;
      for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto mw = qobject_cast<ZMainWindow*>(w)) {
          mainWin = mw;
          break;
        }
      }
      if (!mainWin) {
        return false;
      }
      if (!m_owner.doc()) {
        return false;
      }
      json::object sceneObj;
      sceneObj["Version"] = 1.0;

      // Doc
      json::object docObj;
      m_owner.doc()->write(docObj, true);
      sceneObj["Doc"] = docObj;

      // Per-object views
      auto objs = m_owner.doc()->objs();
      for (auto id : objs) {
        json::object jObj;
        json::object view2DObj;
        if (auto* v = mainWin->view()) {
          v->write(id, view2DObj);
        }
        jObj["View2D"] = view2DObj;

        if (m_owner.engine()) {
          json::object view3DObj;
          auto* eng = m_owner.engine();
          QMetaObject::invokeMethod(
            eng,
            [eng, id, &view3DObj]() {
              eng->write(id, view3DObj);
            },
            Qt::BlockingQueuedConnection);
          jObj["View3D"] = view3DObj;
        }
        sceneObj[QString("%1").arg(id).toStdString()] = jObj;
      }

      // General views
      if (auto* v = mainWin->view()) {
        json::object view2DGeneralObj;
        v->write(view2DGeneralObj);
        sceneObj["View2DGeneral"] = view2DGeneralObj;
      }
      if (m_owner.engine()) {
        json::object view3DGeneralObj;
        auto* eng = m_owner.engine();
        QMetaObject::invokeMethod(
          eng,
          [eng, &view3DGeneralObj]() {
            eng->write(view3DGeneralObj);
          },
          Qt::BlockingQueuedConnection);
        sceneObj["View3DGeneral"] = view3DGeneralObj;
      }

      json::object saveObj;
      saveObj["Scene"] = sceneObj;
      try {
        saveJsonObject(saveObj, QString::fromStdString(req->path()));
      }
      catch (const std::exception&) {
        return false;
      }
      return true;
    });
    if (!ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "save_scene failed");
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status TakeScreenshot3D(ServerContext*, const ScreenshotRequest* req, ScreenshotResponse* reply) override
  {
    // Note: this intentionally does NOT create an animation or write keyframes.
    // It renders the current scene state to a single image file.

    const int width = static_cast<int>(req->width());
    const int height = static_cast<int>(req->height());
    if (width <= 0 || height <= 0) {
      reply->set_ok(false);
      reply->set_error("width and height must be > 0");
      return Status::OK;
    }

    struct Result
    {
      bool ok = false;
      QString path;
      QString error;
    };

    auto res = invokeOnUi([&]() -> Result {
      Result r;
      if (!m_owner.engine()) {
        r.ok = false;
        r.error = "engine not ready";
        return r;
      }

      auto* eng = m_owner.engine();
      CHECK(eng);

      QString outPath = QString::fromStdString(req->path()).trimmed();
      const bool overwrite = req->overwrite();
      if (outPath.isEmpty()) {
        const QString ts = QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz");
        const QString uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString fn = QString("atlas_scene_screenshot3d_%1_%2.png").arg(ts, uid);
        outPath = QDir(QDir::tempPath()).filePath(fn);
      } else {
        // We always write PNG for screenshot RPC calls (LLM-compatible and deterministic).
        // Do not silently rewrite user paths: fail fast if the extension is unexpected.
        if (!outPath.endsWith(".png", Qt::CaseInsensitive)) {
          r.ok = false;
          r.path = outPath;
          r.error = QString("TakeScreenshot3D only supports .png output paths: %1").arg(outPath);
          return r;
        }
        QFileInfo fi(outPath);
        if (fi.isRelative()) {
          outPath = QDir::current().filePath(outPath);
          fi = QFileInfo(outPath);
        }
        if (fi.exists() && !overwrite) {
          r.ok = false;
          r.path = outPath;
          r.error = QString("file already exists (overwrite=false): %1").arg(outPath);
          return r;
        }
      }

      // Ensure output directory exists.
      QDir dir(QFileInfo(outPath).absolutePath());
      if (!dir.exists()) {
        if (!dir.mkpath(".")) {
          r.ok = false;
          r.path = outPath;
          r.error = QString("failed to create output folder: %1").arg(dir.absolutePath());
          return r;
        }
      }

      if (QFileInfo(outPath).exists() && overwrite) {
        // Best-effort: remove first so failures surface earlier.
        QFile::remove(outPath);
      }

      r.path = outPath;

      // Run the actual render on the engine's thread (single-GL-context assumption).
      r = invokeOnObjectThread(eng, [&]() -> Result {
        Result rr;
        rr.path = outPath;

        QString err;
        auto conn = QObject::connect(
          eng,
          &Z3DRenderingEngine::renderingError,
          eng,
          [&](const QString& e) {
            if (err.isEmpty()) {
              err = e;
            }
          },
          Qt::DirectConnection);

        eng->takeFixedSizeScreenShot(outPath, width, height, Z3DScreenShotType::MonoView);

        QObject::disconnect(conn);

        if (QFileInfo(outPath).exists()) {
          rr.ok = true;
          return rr;
        }
        rr.ok = false;
        rr.error = err.isEmpty() ? QString("screenshot failed (no output file): %1").arg(outPath) : err;
        return rr;
      });

      return r;
    });

    reply->set_ok(res.ok);
    reply->set_path(res.path.toStdString());
    if (!res.error.isEmpty()) {
      reply->set_error(res.error.toStdString());
    }
    return Status::OK;
  }

  Status CutSet(ServerContext*, const CutSetRequest* req, Bool* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    const std::vector<size_t> ids = req->refit_camera() ? invokeOnUi([&]() {
                                   return m_owner.doc() ? m_owner.doc()->objs() : std::vector<size_t>{};
                                 })
                                 : std::vector<size_t>{};

    auto ok = invokeOnObjectThread(engine, [&]() -> bool {
      auto& gp = engine->globalParas();
      bool applied = false;
      if (req->has_box()) {
        const auto& box = req->box();
        gp.globalXCut.setLowerValue(box.min().x());
        gp.globalXCut.setUpperValue(box.max().x());
        gp.globalYCut.setLowerValue(box.min().y());
        gp.globalYCut.setUpperValue(box.max().y());
        gp.globalZCut.setLowerValue(box.min().z());
        gp.globalZCut.setUpperValue(box.max().z());
        applied = true;
      } else if (req->has_planes()) {
        // Support only axis-aligned planes: (1,0,0,-lower), (-1,0,0,upper), etc.
        double lx = gp.globalXCut.lowerValue(), ux = gp.globalXCut.upperValue();
        double ly = gp.globalYCut.lowerValue(), uy = gp.globalYCut.upperValue();
        double lz = gp.globalZCut.lowerValue(), uz = gp.globalZCut.upperValue();
        for (const auto& p : req->planes().planes()) {
          const double a = p.a(), b = p.b(), c = p.c(), d = p.d();
          if (a == 1.0 && b == 0.0 && c == 0.0) {
            lx = -d;
            applied = true;
          } else if (a == -1.0 && b == 0.0 && c == 0.0) {
            ux = d;
            applied = true;
          } else if (a == 0.0 && b == 1.0 && c == 0.0) {
            ly = -d;
            applied = true;
          } else if (a == 0.0 && b == -1.0 && c == 0.0) {
            uy = d;
            applied = true;
          } else if (a == 0.0 && b == 0.0 && c == 1.0) {
            lz = -d;
            applied = true;
          } else if (a == 0.0 && b == 0.0 && c == -1.0) {
            uz = d;
            applied = true;
          } else {
            // Unsupported plane orientation in current implementation.
            return false;
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
      if (req->refit_camera() && !ids.empty()) {
        // Refit camera to visible objects after clipping.
        const auto bb = engine->boundBoxOfObjsAfterClipping(ids);
        if (!bb.empty()) {
          engine->camera().resetCamera(bb, Z3DCamera::ResetOption::PreserveViewVector);
        }
      }
      return true;
    });
    if (!ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "cut_set failed");
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status CutClear(ServerContext*, const Empty*, Bool* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);
    auto ok = invokeOnObjectThread(engine, [&]() -> bool {
      auto& gp = engine->globalParas();
      gp.globalXCut.setLowerValue(gp.globalXCut.minimum());
      gp.globalXCut.setUpperValue(gp.globalXCut.maximum());
      gp.globalYCut.setLowerValue(gp.globalYCut.minimum());
      gp.globalYCut.setUpperValue(gp.globalYCut.maximum());
      gp.globalZCut.setLowerValue(gp.globalZCut.minimum());
      gp.globalZCut.setUpperValue(gp.globalZCut.maximum());
      return true;
    });
    if (!ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "cut_clear failed");
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status CutSuggest(ServerContext*, const CutSuggestRequest* req, CutSuggestion* reply) override
  {
    if (!m_owner.engine()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    }
    Z3DRenderingEngine* engine = m_owner.engine();
    CHECK(engine);

    const std::vector<size_t> ids = invokeOnUi([&]() {
      std::vector<size_t> ids;
      if (req->ids_size() == 0) {
        if (m_owner.doc()) {
          ids = filterVisualIds(m_owner, m_owner.doc()->objs());
        }
      } else {
        ids.reserve(req->ids_size());
        for (auto v : req->ids()) {
          ids.push_back(static_cast<size_t>(v));
        }
        ids = filterVisualIds(m_owner, ids);
      }
      return ids;
    });

    auto bbox = invokeOnObjectThread(engine, [&]() {
      return req->after_clipping() ? engine->boundBoxOfObjsAfterClipping(ids) : engine->boundBoxOfObjs(ids);
    });

    if (!bbox.empty() && req->margin() > 0.0) {
      const double m = req->margin();
      bbox.expand(bbox.minCorner - glm::dvec3(m));
      bbox.expand(bbox.maxCorner + glm::dvec3(m));
    }
    if (bbox.empty()) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "bbox empty");
    }
    if (req->mode().empty() || req->mode() == std::string("box")) {
      auto* out = reply->mutable_box();
      out->mutable_min()->set_x(bbox.minCorner.x);
      out->mutable_min()->set_y(bbox.minCorner.y);
      out->mutable_min()->set_z(bbox.minCorner.z);
      out->mutable_max()->set_x(bbox.maxCorner.x);
      out->mutable_max()->set_y(bbox.maxCorner.y);
      out->mutable_max()->set_z(bbox.maxCorner.z);
      out->mutable_size()->set_x(bbox.maxCorner.x - bbox.minCorner.x);
      out->mutable_size()->set_y(bbox.maxCorner.y - bbox.minCorner.y);
      out->mutable_size()->set_z(bbox.maxCorner.z - bbox.minCorner.z);
      out->mutable_center()->set_x((bbox.maxCorner.x + bbox.minCorner.x) * 0.5);
      out->mutable_center()->set_y((bbox.maxCorner.y + bbox.minCorner.y) * 0.5);
      out->mutable_center()->set_z((bbox.maxCorner.z + bbox.minCorner.z) * 0.5);
      return Status::OK;
    }
    return Status(grpc::StatusCode::INVALID_ARGUMENT, "unsupported mode");
  }

private:
  ZRPCService& m_owner;
};

void ZRPCService::init()
{
  g_sm->checkCurrentOn(ZServiceManager::RPC);
  // We are already running on the dedicated RPC thread managed by ZServiceManager.
  // Start the gRPC server on this thread, but run the blocking Wait in a
  // dedicated std::thread so the Qt event loop remains responsive for
  // shutdown signals.
  onRPCThreadStarted();
}

void ZRPCService::shutdown()
{
  g_sm->checkCurrentOn(ZServiceManager::RPC);
  if (m_grpcServer) {
    m_grpcServer->Shutdown();
  }
  if (m_waitThread.joinable()) {
    m_waitThread.join();
  }
  m_serverOwned.reset();
  // Release services after server is fully stopped
  m_sceneService.reset();
  m_grpcServer = nullptr;
}

void ZRPCService::onRPCThreadStarted()
{
  // Ensure we are on the RPC thread
  CHECK(g_sm->isCurrentOn(ZServiceManager::RPC));

  std::string server_address("0.0.0.0:50051");
  // Allocate services on the heap and keep them alive for the server lifetime.
  m_sceneService = std::unique_ptr<grpc::Service>(new SceneServiceImpl(*this));

  grpc::ServerBuilder builder;
  // Set the default compression algorithm for the server.
  builder.SetDefaultCompressionAlgorithm(GRPC_COMPRESS_GZIP);
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(m_sceneService.get());
  // Finally assemble the server.
  m_serverOwned = builder.BuildAndStart();
  LOG(INFO) << "Server listening on " << server_address;
  m_grpcServer = m_serverOwned.get();
  // Block in a background thread so the RPC QThread event loop remains free
  // to accept a BlockingQueuedConnection for shutdown.
  m_waitThread = std::thread([server = m_serverOwned.get()]() {
    server->Wait();
    LOG(INFO) << "Server shutdown";
  });
}

} // namespace nim
