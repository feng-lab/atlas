#include "zrpcservice.h"

#include "scene.grpc.pb.h"
#include "zservicemanager.h"
#include "zlog.h"
#include "z3dviewsettingparamops.h"
#include "zjson.h"
#include "z3dcameraplanner.h"
#include "zqobjectthreadinvoker.h"
#include "zrpcuidispatcher.h"
#include "zrpcsceneids.h"
#include "zrpctaskmanager.h"
#include <QThread>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <grpc/support/time.h>
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
using atlas::rpc::ListObjectsResponse;
using atlas::rpc::ObjectInfo;
using atlas::rpc::ObjectLoadState;
using atlas::rpc::OBJECT_LOAD_STATE_DOC_NOT_READY;
using atlas::rpc::OBJECT_LOAD_STATE_ENGINE_NOT_READY;
using atlas::rpc::OBJECT_LOAD_STATE_NOT_FOUND;
using atlas::rpc::OBJECT_LOAD_STATE_READY;
using atlas::rpc::OBJECT_LOAD_STATE_VIEW_NOT_READY;
using atlas::rpc::OBJECT_LOAD_STATE_ERROR;
using atlas::rpc::OBJECT_LOAD_STATE_UNSPECIFIED;
using atlas::rpc::ObjectStatus;
using atlas::rpc::GetStatusRequest;
using atlas::rpc::GetStatusResponse;
using atlas::rpc::WaitForObjectsReadyRequest;
using atlas::rpc::WaitForObjectsReadyResponse;
using atlas::rpc::TaskId;
using atlas::rpc::TaskStatus;
using atlas::rpc::TaskState;
using atlas::rpc::TASK_STATE_UNSPECIFIED;
using atlas::rpc::TASK_STATE_QUEUED;
using atlas::rpc::TASK_STATE_RUNNING;
using atlas::rpc::TASK_STATE_SUCCEEDED;
using atlas::rpc::TASK_STATE_FAILED;
using atlas::rpc::TASK_STATE_CANCELLED;
using atlas::rpc::LoadTaskRequest;
using atlas::rpc::LoadTaskResult;
using atlas::rpc::WaitTaskRequest;
using atlas::rpc::StartTaskResponse;
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
using atlas::rpc::CameraSampleRequest;
using atlas::rpc::CameraSampleResponse;
using atlas::rpc::CameraSampleKey;
using atlas::rpc::FitCandidatesResponse;
using atlas::rpc::VisibilityRequest;
using atlas::rpc::RemoveObjectsRequest;
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
using atlas::rpc::Set3DCanvasSizeRequest;
using atlas::rpc::Set3DCanvasSizeResponse;
using atlas::rpc::ClearKeysRequest;
using atlas::rpc::RemoveKeyRequest;
using atlas::rpc::BatchSetKey;
using atlas::rpc::BatchRemoveKey;
using atlas::rpc::BatchRequest;
using atlas::rpc::SetTimeRequest;
using atlas::rpc::AddKeyFrameRequest;
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

ZRPCService::ZRPCService(std::string_view appVersion, QObject* parent)
  : QObject(parent)
  , m_appVersion(appVersion)
{
  CHECK(!m_appVersion.empty());
}

ZRPCService::~ZRPCService() = default;

void ZRPCService::setUiDispatcher(ZRpcUiDispatcher* dispatcher)
{
  m_uiDispatcher = dispatcher;
}

namespace {
static constexpr uint32_t kWaitForObjectsReadyDefaultPollMs = 50u;
static constexpr uint32_t kWaitTaskDefaultPollMs = 50u;

template<class T>
using ZRpcInvokeResult = ZQObjectThreadInvokeResult<T>;

template<class Func>
auto invokeOnObjectThread(grpc::ServerContext* grpcContext, QObject* obj, Func&& f, std::string_view what)
  -> ZRpcInvokeResult<decltype(f())>;

[[nodiscard]] bool grpcHasFiniteDeadline(const grpc::ServerContext* grpcContext)
{
  if (!grpcContext) {
    return false;
  }
  const gpr_timespec d = grpcContext->raw_deadline();
  const gpr_timespec inf = gpr_inf_future(d.clock_type);
  return gpr_time_cmp(d, inf) != 0;
}

struct ZRpcWaitResult
{
  bool ok = false;
  std::string error;
};

[[nodiscard]] std::string formatIdList(const std::vector<uint64_t>& ids)
{
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i) {
      oss << ", ";
    }
    oss << ids[i];
  }
  oss << "]";
  return oss.str();
}

template<class Predicate>
[[nodiscard]] ZRpcWaitResult
waitUntil(grpc::ServerContext* grpcContext, uint32_t pollMs, Predicate&& predicate, std::string_view what)
{
  ZRpcWaitResult out;

  if (!grpcContext) {
    out.ok = false;
    out.error = std::string(what) + ": grpc context not provided";
    return out;
  }

  if (!grpcHasFiniteDeadline(grpcContext)) {
    // Avoid unbounded waits on server threads: require the caller to set a gRPC deadline
    // (Python: pass timeout=...).
    out.ok = false;
    out.error = std::string(what) + ": no gRPC deadline (set a client timeout to enable auto-wait)";
    return out;
  }

  const auto deadline = grpcContext->deadline();
  while (true) {
    if (grpcContext->IsCancelled()) {
      out.ok = false;
      out.error = std::string(what) + ": cancelled";
      return out;
    }

    if (predicate()) {
      out.ok = true;
      out.error.clear();
      return out;
    }

    if (std::chrono::system_clock::now() >= deadline) {
      out.ok = false;
      out.error = std::string(what) + ": timeout waiting for readiness";
      return out;
    }

    QThread::msleep(static_cast<unsigned long>(pollMs));
  }
}

[[nodiscard]] ZRpcWaitResult waitForObjectViewsReady(grpc::ServerContext* grpcContext,
                                                     ZRpcUiDispatcher* disp,
                                                     const std::vector<uint64_t>& ids,
                                                     uint32_t pollMs,
                                                     std::string_view what)
{
  CHECK(disp);

  if (ids.empty()) {
    ZRpcWaitResult out;
    out.ok = true;
    return out;
  }

  if (!grpcContext) {
    ZRpcWaitResult out;
    out.ok = false;
    out.error = std::string(what) + ": grpc context not provided";
    return out;
  }
  if (!grpcHasFiniteDeadline(grpcContext)) {
    ZRpcWaitResult out;
    out.ok = false;
    out.error = std::string(what) + ": no gRPC deadline (set a client timeout to enable auto-wait)";
    return out;
  }

  const auto deadline = grpcContext->deadline();

  while (true) {
    if (grpcContext->IsCancelled()) {
      ZRpcWaitResult out;
      out.ok = false;
      out.error = std::string(what) + ": cancelled";
      return out;
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, ids]() {
        return disp->statusSnapshot(ids, /*includeAllObjects=*/false);
      },
      what);
    if (!inv.ok) {
      ZRpcWaitResult out;
      out.ok = false;
      out.error = inv.error.empty() ? (std::string(what) + ": failed to query status") : inv.error;
      return out;
    }

    const auto& s = inv.value;
    if (s.ok && s.docReady && s.engineReady) {
      bool allReady = true;
      std::vector<uint64_t> notFound;
      std::vector<uint64_t> errors;
      for (const auto& obj : s.objects) {
        if (obj.loadState == ZRpcUiDispatcher::ObjectLoadState::NotFound) {
          notFound.push_back(obj.id);
          allReady = false;
          continue;
        }
        if (obj.loadState == ZRpcUiDispatcher::ObjectLoadState::Error) {
          errors.push_back(obj.id);
          allReady = false;
          continue;
        }
        if (obj.loadState != ZRpcUiDispatcher::ObjectLoadState::Ready) {
          allReady = false;
        }
      }

      if (!notFound.empty()) {
        ZRpcWaitResult out;
        out.ok = false;
        out.error = std::string(what) + ": object(s) not found: " + formatIdList(notFound);
        return out;
      }
      if (!errors.empty()) {
        ZRpcWaitResult out;
        out.ok = false;
        out.error = std::string(what) + ": object(s) in error state: " + formatIdList(errors);
        return out;
      }
      if (allReady) {
        ZRpcWaitResult out;
        out.ok = true;
        return out;
      }
    }

    if (std::chrono::system_clock::now() >= deadline) {
      ZRpcWaitResult out;
      out.ok = false;
      out.error = std::string(what) + ": timeout waiting for objects to become ready";
      return out;
    }

    QThread::msleep(static_cast<unsigned long>(pollMs));
  }
}

// gRPC handlers run on gRPC-managed threads. To respect Atlas' thread boundaries,
// we bounce work to the target QObject thread with a queued call and wait for
// completion, respecting gRPC cancellation/deadlines where available.
template<class Func>
auto invokeOnObjectThread(grpc::ServerContext* grpcContext, QObject* obj, Func&& f, std::string_view what)
  -> ZRpcInvokeResult<decltype(f())>
{
  std::function<bool()> shouldCancel;
  if (grpcContext) {
    const auto deadline = grpcContext->deadline();
    shouldCancel = [grpcContext, deadline]() -> bool {
      if (grpcContext->IsCancelled()) {
        return true;
      }
      return std::chrono::system_clock::now() >= deadline;
    };
  }
  return invokeOnObjectThreadWait(obj, std::forward<Func>(f), what, std::move(shouldCancel));
}

template<class RepeatedIds>
[[nodiscard]] ZRpcInvokeResult<std::vector<uint64_t>> resolveWaitIds(grpc::ServerContext* grpcContext,
                                                                     ZRpcUiDispatcher* disp,
                                                                     const RepeatedIds& idsField,
                                                                     bool emptyMeansAll,
                                                                     std::string_view what)
{
  CHECK(disp);
  ZRpcInvokeResult<std::vector<uint64_t>> out;
  out.ok = true;
  if (idsField.size() > 0) {
    out.value.reserve(static_cast<size_t>(idsField.size()));
    for (auto v : idsField) {
      out.value.push_back(static_cast<uint64_t>(v));
    }
    return out;
  }
  if (!emptyMeansAll) {
    return out;
  }

  auto inv = invokeOnObjectThread(
    grpcContext,
    disp,
    [disp]() {
      return disp->fitCandidates();
    },
    what);
  if (!inv.ok) {
    out.ok = false;
    out.error = inv.error.empty() ? (std::string(what) + ": failed to query fit candidates") : inv.error;
    return out;
  }

  out.value = std::move(inv.value);
  return out;
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

// Scene service bound to app UI context
class SceneServiceImpl final : public Scene::Service
{
public:
  explicit SceneServiceImpl(ZRpcUiDispatcher* uiDispatcher, std::string_view appVersion)
    : m_appVersion(appVersion)
    , m_uiDispatcher(uiDispatcher)
    , m_taskManager(uiDispatcher)
  {
    CHECK(!m_appVersion.empty());
  }

  Status Ping(ServerContext*, const PingRequest*, PingResponse* reply) override
  {
    LOG(INFO) << "RPC Ping";
    reply->set_ok(true);
    return Status::OK;
  }

  Status GetAppLocation(ServerContext* grpcContext,
                        const google::protobuf::Empty*,
                        google::protobuf::StringValue* reply) override
  {
    CHECK(reply);

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "get_app_location: ui dispatcher not ready");
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp]() {
        return disp->appLocation();
      },
      "get_app_location");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    const auto& r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "get_app_location failed" : r.error);
    }

    reply->set_value(r.value.toStdString());
    return Status::OK;
  }

  Status GetAppVersion(ServerContext*,
                       const google::protobuf::Empty*,
                       google::protobuf::StringValue* reply) override
  {
    CHECK(reply);
    // Build-time version string (git describe + build timestamp).
    // Provided by the app entry point to keep atlas_core independent of version.h.
    reply->set_value(m_appVersion.data(), m_appVersion.size());
    return Status::OK;
  }

  Status GetStatus(ServerContext* grpcContext, const GetStatusRequest* req, GetStatusResponse* reply) override
  {
    CHECK(reply);

    const bool includeAllObjects = (req != nullptr) ? req->include_all_objects() : false;
    std::vector<uint64_t> requestedIds;
    if (req && !includeAllObjects) {
      requestedIds.reserve(static_cast<size_t>(req->ids_size()));
      for (auto v : req->ids()) {
        requestedIds.push_back(static_cast<uint64_t>(v));
      }
    }

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      reply->set_ok(false);
      reply->set_doc_ready(false);
      reply->set_engine_ready(false);
      reply->set_has_3d_window(false);
      reply->set_error("get_status: ui dispatcher not ready");
      return Status::OK;
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, ids = std::move(requestedIds), includeAllObjects]() {
        return disp->statusSnapshot(ids, includeAllObjects);
      },
      "get_status");
    if (!inv.ok) {
      reply->set_ok(false);
      reply->set_doc_ready(false);
      reply->set_engine_ready(false);
      reply->set_has_3d_window(false);
      reply->set_error(inv.error);
      return Status::OK;
    }
    const auto& s = inv.value;

    reply->set_ok(s.ok);
    reply->set_doc_ready(s.docReady);
    reply->set_engine_ready(s.engineReady);
    reply->set_has_3d_window(s.has3DWindow);
    if (!s.error.empty()) {
      reply->set_error(s.error);
    }

    auto toProtoState = [](ZRpcUiDispatcher::ObjectLoadState st) -> atlas::rpc::ObjectLoadState {
      switch (st) {
        case ZRpcUiDispatcher::ObjectLoadState::NotFound:
          return OBJECT_LOAD_STATE_NOT_FOUND;
        case ZRpcUiDispatcher::ObjectLoadState::DocNotReady:
          return OBJECT_LOAD_STATE_DOC_NOT_READY;
        case ZRpcUiDispatcher::ObjectLoadState::EngineNotReady:
          return OBJECT_LOAD_STATE_ENGINE_NOT_READY;
        case ZRpcUiDispatcher::ObjectLoadState::ViewNotReady:
          return OBJECT_LOAD_STATE_VIEW_NOT_READY;
        case ZRpcUiDispatcher::ObjectLoadState::Ready:
          return OBJECT_LOAD_STATE_READY;
        case ZRpcUiDispatcher::ObjectLoadState::Error:
          return OBJECT_LOAD_STATE_ERROR;
      }
      return OBJECT_LOAD_STATE_UNSPECIFIED;
    };

    for (const auto& obj : s.objects) {
      ObjectStatus* st = reply->add_objects();
      st->set_id(obj.id);
      st->set_type(obj.type);
      st->set_name(obj.name);
      st->set_path(obj.path);
      st->set_visible(obj.visible);
      st->set_load_state(toProtoState(obj.loadState));
      if (obj.progress.has_value()) {
        st->mutable_progress()->set_value(*obj.progress);
      }
      if (!obj.error.empty()) {
        st->set_error(obj.error);
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

  Status StartLoadTask(ServerContext*, const LoadTaskRequest* req, StartTaskResponse* reply) override
  {
    CHECK(req);
    CHECK(reply);

    if (req->sources_size() == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "start_load_task: sources is required");
    }

    QStringList sources;
    sources.reserve(req->sources_size());
    QStringList problems;
    for (int i = 0; i < req->sources_size(); ++i) {
      const QString s = QString::fromStdString(req->sources(i)).trimmed();
      if (s.isEmpty()) {
        problems.push_back(QString("sources[%1]: empty").arg(i));
        continue;
      }
      sources.push_back(s);
    }
    if (!problems.isEmpty()) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT,
                    QString("start_load_task: invalid input: %1").arg(problems.join("; ")).toStdString());
    }

    ZRpcTaskManager::StartLoadParams params;
    params.sources = std::move(sources);
    params.setVisible = req->set_visible();
    if (req->network_timeout_ms() > 0u) {
      params.networkTimeout = std::chrono::milliseconds(req->network_timeout_ms());
    }

    const uint64_t taskId = m_taskManager.startLoadTask(std::move(params));
    reply->set_ok(true);
    reply->set_task_id(taskId);
    return Status::OK;
  }

  Status GetTaskStatus(ServerContext*, const TaskId* req, TaskStatus* reply) override
  {
    CHECK(req);
    CHECK(reply);

    const auto snapOpt = m_taskManager.taskStatus(req->id());
    if (!snapOpt.has_value()) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "get_task_status: unknown task_id");
    }
    fillTaskStatus(*snapOpt, reply);
    return Status::OK;
  }

  Status WaitTask(ServerContext* context, const WaitTaskRequest* req, TaskStatus* reply) override
  {
    CHECK(req);
    CHECK(reply);

    if (req->task_id() == 0u) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "wait_task: task_id is required");
    }

    const uint32_t timeoutMs = req->timeout_ms();
    const uint32_t pollMsRaw = req->poll_interval_ms();
    const uint32_t pollMs = (pollMsRaw > 0u) ? pollMsRaw : kWaitTaskDefaultPollMs;

    std::function<bool()> shouldCancel;
    if (context) {
      const auto deadline = context->deadline();
      shouldCancel = [context, deadline]() -> bool {
        if (context->IsCancelled()) {
          return true;
        }
        return std::chrono::system_clock::now() >= deadline;
      };
    }

    const auto snapOpt = m_taskManager.waitForCompletion(req->task_id(),
                                                         std::chrono::milliseconds(timeoutMs),
                                                         std::chrono::milliseconds(pollMs),
                                                         std::move(shouldCancel));
    if (!snapOpt.has_value()) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "wait_task: unknown task_id");
    }

    fillTaskStatus(*snapOpt, reply);

    if (context && context->IsCancelled()) {
      return Status(grpc::StatusCode::CANCELLED, "cancelled");
    }

    return Status::OK;
  }

  Status CancelTask(ServerContext*, const TaskId* req, Bool* reply) override
  {
    CHECK(req);
    CHECK(reply);

    const bool ok = m_taskManager.cancelTask(req->id());
    if (!ok) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "cancel_task: unknown task_id");
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status DeleteTask(ServerContext*, const TaskId* req, Bool* reply) override
  {
    CHECK(req);
    CHECK(reply);

    const bool ok = m_taskManager.deleteTask(req->id());
    if (!ok) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "delete_task: unknown task_id");
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status GetParamValues(ServerContext* grpcContext,
                        const GetParamValuesRequest* req,
                        GetParamValuesResponse* reply) override
  {
    CHECK(req);
    CHECK(reply);

    // Use unified parameter access for all scopes, including camera (id=0).
    const size_t boundId = static_cast<size_t>(req->id());

    std::vector<std::string> queryKeys;
    queryKeys.reserve(static_cast<size_t>(req->json_keys_size()));
    for (const auto& qk : req->json_keys()) {
      queryKeys.push_back(qk);
    }

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "get_param_values: ui dispatcher not ready");
    }

    auto invokeGet = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [disp, boundId, &queryKeys]() {
          return disp->getParamValues(boundId, queryKeys);
        },
        "get_param_values");
    };

    auto inv = invokeGet();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }

    auto r = inv.value;
    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);
    if (canAutoWait && !r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        r.error.find("target_not_ready") != std::string::npos) {
      // Make GetParamValues "UI-parity safe": if the target object isn't yet bound into a 3D view/filter,
      // wait until it becomes ready (bounded by the caller's gRPC deadline) and retry.
      if (boundId > kZRpcScopeGlobal) {
        const std::vector<uint64_t> ids = {static_cast<uint64_t>(boundId)};
        const auto wait = waitForObjectViewsReady(grpcContext,
                                                  disp,
                                                  ids,
                                                  kWaitForObjectsReadyDefaultPollMs,
                                                  "get_param_values:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
      } else {
        const auto wait = waitUntil(
          grpcContext,
          kWaitForObjectsReadyDefaultPollMs,
          [&]() -> bool {
            auto inv2 = invokeGet();
            return inv2.ok && inv2.value.ok;
          },
          "get_param_values:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
      }

      // Retry once after readiness.
      inv = invokeGet();
      if (!inv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
      }
      r = inv.value;
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "get_param_values failed" : r.error);
    }

    for (const auto& [k, v] : r.values) {
      (*reply->mutable_values())[std::string(k.data(), k.size())] = jsonToPb(v);
    }
    return Status::OK;
  }

  Status ValidateSceneParams(ServerContext* grpcContext,
                             const ValidateSceneParamsRequest* req,
                             ValidateSceneParamsResponse* reply) override
  {
    CHECK(req);
    CHECK(reply);

    std::vector<Z3DViewSettingParamOps::SetParamData> setParams;
    setParams.reserve(static_cast<size_t>(req->set_params_size()));
    for (const auto& sp : req->set_params()) {
      Z3DViewSettingParamOps::SetParamData d;
      d.id = static_cast<size_t>(sp.id());
      d.jsonKey = QString::fromStdString(sp.json_key());
      d.value = pbToJson(sp.value());
      setParams.push_back(std::move(d));
    }

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "validate_scene_params: ui dispatcher not ready");
    }

    auto invokeValidate = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [disp, &setParams]() {
          return disp->validateSceneParams(setParams);
        },
        "validate_scene_params");
    };

    auto inv = invokeValidate();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = inv.value;
    if (!r.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, r.error.empty() ? "validate_scene_params failed" : r.error);
    }

    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);
    if (canAutoWait) {
      bool hasNotReady = false;
      for (const auto& pr : r.results) {
        if (!pr.ok && pr.reason.startsWith("target_not_ready")) {
          hasNotReady = true;
          break;
        }
      }

      if (hasNotReady) {
        std::vector<uint64_t> idsToWait;
        idsToWait.reserve(setParams.size());
        for (const auto& sp : setParams) {
          if (sp.id > kZRpcScopeGlobal) {
            idsToWait.push_back(static_cast<uint64_t>(sp.id));
          }
        }
        std::sort(idsToWait.begin(), idsToWait.end());
        idsToWait.erase(std::unique(idsToWait.begin(), idsToWait.end()), idsToWait.end());

        if (!idsToWait.empty()) {
          const auto wait = waitForObjectViewsReady(grpcContext,
                                                    disp,
                                                    idsToWait,
                                                    kWaitForObjectsReadyDefaultPollMs,
                                                    "validate_scene_params:wait_ready");
          if (!wait.ok) {
            return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
          }
        } else {
          const auto wait = waitUntil(
            grpcContext,
            kWaitForObjectsReadyDefaultPollMs,
            [&]() -> bool {
              auto inv2 = invokeValidate();
              if (!inv2.ok || !inv2.value.ok) {
                return false;
              }
              for (const auto& pr2 : inv2.value.results) {
                if (!pr2.ok && pr2.reason.startsWith("target_not_ready")) {
                  return false;
                }
              }
              return true;
            },
            "validate_scene_params:wait_ready");
          if (!wait.ok) {
            return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
          }
        }

        inv = invokeValidate();
        if (!inv.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
        }
        r = inv.value;
        if (!r.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION,
                        r.error.empty() ? "validate_scene_params failed" : r.error);
        }
      }
    }

    for (const auto& pr : r.results) {
      ValidateSceneParamResult out;
      out.set_json_key(pr.jsonKey.toStdString());
      out.set_ok(pr.ok);
      if (pr.ok && pr.hasNormalizedValue) {
        *out.mutable_normalized_value() = jsonToPb(pr.normalizedValue);
      }
      if (!pr.ok && !pr.reason.isEmpty()) {
        out.set_reason(pr.reason.toStdString());
      }
      *reply->add_results() = std::move(out);
    }
    reply->set_ok(r.allOk);
    return Status::OK;
  }

  Status ApplySceneParams(ServerContext* grpcContext, const ApplySceneParamsRequest* req, Bool* reply) override
  {
    std::vector<Z3DViewSettingParamOps::SetParamData> setParams;
    setParams.reserve(static_cast<size_t>(req->set_params_size()));
    for (const auto& sp : req->set_params()) {
      Z3DViewSettingParamOps::SetParamData d;
      d.id = static_cast<size_t>(sp.id());
      d.jsonKey = QString::fromStdString(sp.json_key());
      d.value = pbToJson(sp.value());
      setParams.push_back(std::move(d));
    }

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "apply_scene_params: ui dispatcher not ready");
    }

    auto invokeApply = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [disp, &setParams]() {
          return disp->applySceneParams(setParams);
        },
        "apply_scene_params");
    };

    auto inv = invokeApply();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = inv.value;
    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);
    if (canAutoWait && !r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        r.error.find("target_not_ready") != std::string::npos) {
      std::vector<uint64_t> idsToWait;
      idsToWait.reserve(setParams.size());
      for (const auto& sp : setParams) {
        if (sp.id > kZRpcScopeGlobal) {
          idsToWait.push_back(static_cast<uint64_t>(sp.id));
        }
      }
      std::sort(idsToWait.begin(), idsToWait.end());
      idsToWait.erase(std::unique(idsToWait.begin(), idsToWait.end()), idsToWait.end());

      if (!idsToWait.empty()) {
        const auto wait = waitForObjectViewsReady(grpcContext,
                                                  disp,
                                                  idsToWait,
                                                  kWaitForObjectsReadyDefaultPollMs,
                                                  "apply_scene_params:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
      } else {
        const auto wait = waitUntil(
          grpcContext,
          kWaitForObjectsReadyDefaultPollMs,
          [&]() -> bool {
            auto inv2 = invokeApply();
            return inv2.ok && inv2.value.ok;
          },
          "apply_scene_params:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
      }

      inv = invokeApply();
      if (!inv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
      }
      r = inv.value;
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "apply_scene_params failed" : r.error);
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status EngineReady(ServerContext* grpcContext, const Empty*, Bool* reply) override
  {
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      reply->set_ok(false);
      return Status::OK;
    }

    // Wait up to ~5 seconds for engine to become ready.
    for (int i = 0; i < 100; ++i) {
      auto inv = invokeOnObjectThread(
        grpcContext,
        disp,
        [disp]() {
          return disp->engineReady();
        },
        "engine_ready");

      if (!inv.ok) {
        reply->set_ok(false);
        return Status::OK;
      }
      if (inv.value) {
        reply->set_ok(true);
        return Status::OK;
      }

      QThread::msleep(50);
    }

    reply->set_ok(false);
    return Status::OK;
  }

  Status Ensure3DWindow(ServerContext* grpcContext, const Empty*, Bool* reply) override
  {
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ensure_3d_window: ui dispatcher not ready");
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp]() {
        return disp->ensure3DWindow();
      },
      "ensure_3d_window");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    const auto& r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "ensure_3d_window failed" : r.error);
    }

    reply->set_ok(true);
    return Status::OK;
  }

  Status Set3DCanvasSize(ServerContext* grpcContext,
                         const Set3DCanvasSizeRequest* req,
                         Set3DCanvasSizeResponse* reply) override
  {
    CHECK(req);
    CHECK(reply);

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      reply->set_ok(false);
      reply->set_error("set_3d_canvas_size: ui dispatcher not ready");
      return Status::OK;
    }

    const int logicalWidth = static_cast<int>(req->logical_width());
    const int logicalHeight = static_cast<int>(req->logical_height());

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, logicalWidth, logicalHeight]() {
        return disp->set3DCanvasSize(logicalWidth, logicalHeight);
      },
      "set_3d_canvas_size");
    if (!inv.ok) {
      reply->set_ok(false);
      reply->set_error(inv.error);
      return Status::OK;
    }

    const auto& r = inv.value;
    reply->set_ok(r.ok);
    reply->set_logical_width(static_cast<uint32_t>(std::max(r.logicalWidth, 0)));
    reply->set_logical_height(static_cast<uint32_t>(std::max(r.logicalHeight, 0)));
    reply->set_physical_width(static_cast<uint32_t>(std::max(r.physicalWidth, 0)));
    reply->set_physical_height(static_cast<uint32_t>(std::max(r.physicalHeight, 0)));
    if (!r.error.empty()) {
      reply->set_error(r.error);
    }
    return Status::OK;
  }

  Status ListObjects(ServerContext* grpcContext, const Empty*, ListObjectsResponse* reply) override
  {
    VLOG(1) << "RPC ListObjects";

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "list_objects: ui dispatcher not ready");
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp]() {
        return disp->listObjects();
      },
      "list_objects");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    const auto& r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "list_objects failed" : r.error);
    }

    for (const auto& obj : r.objects) {
      ObjectInfo oi;
      oi.set_id(obj.id);
      oi.set_type(obj.type);
      oi.set_name(obj.name);
      oi.set_path(obj.path);
      oi.set_visible(obj.visible);
      *reply->add_objects() = std::move(oi);
    }
    return Status::OK;
  }

  Status BBox(ServerContext* grpcContext, const BBoxRequest* req, BBoxResponse* reply) override
  {
    CHECK(req);

    const bool afterClipping = req->after_clipping();
    std::vector<size_t> ids;
    ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      ids.push_back(static_cast<size_t>(v));
    }

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "bbox: ui dispatcher not ready");
    }

    auto invokeBBox = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [disp, &ids, afterClipping]() {
          return disp->bboxOfObjects(ids, afterClipping);
        },
        "bbox");
    };

    auto inv = invokeBBox();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = inv.value;
    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);
    if (canAutoWait && !r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        r.error.find("objects may not be ready") != std::string::npos) {
      std::vector<uint64_t> idsToWait;
      if (!ids.empty()) {
        idsToWait.reserve(ids.size());
        for (auto id : ids) {
          idsToWait.push_back(static_cast<uint64_t>(id));
        }
      } else {
        // For the "all objects" case, wait for all current fit candidates.
        auto invFit = invokeOnObjectThread(
          grpcContext,
          disp,
          [disp]() {
            return disp->fitCandidates();
          },
          "bbox:fit_candidates");
        if (!invFit.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, invFit.error);
        }
        idsToWait = std::move(invFit.value);
      }

      if (idsToWait.empty()) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, "bbox: no visual objects");
      }

      const auto wait =
        waitForObjectViewsReady(grpcContext, disp, idsToWait, kWaitForObjectsReadyDefaultPollMs, "bbox:wait_ready");
      if (!wait.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
      }

      inv = invokeBBox();
      if (!inv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
      }
      r = inv.value;
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "bbox failed" : r.error);
    }

    auto toVec = [](const glm::dvec3& v) {
      Vec3 r;
      r.set_x(v.x);
      r.set_y(v.y);
      r.set_z(v.z);
      return r;
    };
    ::atlas::rpc::BBox* b = reply->mutable_bbox();
    *b->mutable_min() = toVec(r.minCorner);
    *b->mutable_max() = toVec(r.maxCorner);
    *b->mutable_size() = toVec(r.size);
    *b->mutable_center() = toVec(r.center);
    return Status::OK;
  }

  Status Capabilities(ServerContext* grpcContext, const CapabilitiesRequest* req, CapabilitiesResponse* reply) override
  {
    CHECK(req);
    CHECK(reply);

    std::vector<uint64_t> ids;
    ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      ids.push_back(static_cast<uint64_t>(v));
    }

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "capabilities: ui dispatcher not ready");
    }

    auto invokeCaps = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [disp, &ids]() {
          return disp->capabilities(ids);
        },
        "capabilities");
    };

    auto inv = invokeCaps();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto res = inv.value;
    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);
    if (canAutoWait && !res.ok && res.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        res.error.find("target_not_ready") != std::string::npos) {
      const auto wait = waitUntil(
        grpcContext,
        kWaitForObjectsReadyDefaultPollMs,
        [&]() -> bool {
          auto inv2 = invokeCaps();
          return inv2.ok && inv2.value.ok;
        },
        "capabilities:wait_ready");
      if (!wait.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
      }

      inv = invokeCaps();
      if (!inv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
      }
      res = inv.value;
    }
    if (!res.ok) {
      const grpc::StatusCode code = (res.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, res.error.empty() ? "capabilities failed" : res.error);
    }

    auto toProto = [&](const Z3DViewSettingParamOps::ParameterMeta& meta) {
      Parameter p;
      p.set_json_key(meta.jsonKey.toStdString());
      p.set_name(meta.name.toStdString());
      p.set_type(meta.type.toStdString());
      if (!meta.description.isEmpty()) {
        p.set_description(meta.description.toStdString());
      }
      p.set_supports_interpolation(meta.supportsInterpolation);
      p.mutable_value_schema()->CopyFrom(jsonToPb(meta.valueSchema).struct_value());
      return p;
    };

    for (const auto& meta : res.camera) {
      *reply->add_camera() = toProto(meta);
    }
    for (const auto& meta : res.background) {
      *reply->add_background() = toProto(meta);
    }
    for (const auto& meta : res.axis) {
      *reply->add_axis() = toProto(meta);
    }
    for (const auto& meta : res.global) {
      *reply->add_global() = toProto(meta);
    }
    for (const auto& [tn, metas] : res.objects) {
      ParamList lst;
      for (const auto& meta : metas) {
        *lst.add_params() = toProto(meta);
      }
      (*reply->mutable_objects())[tn.toStdString()] = std::move(lst);
    }
    return Status::OK;
  }

  Status MakeAlias(ServerContext* grpcContext, const MakeAliasRequest* req, MakeAliasResponse* reply) override
  {
    CHECK(req);
    std::vector<uint64_t> srcIds;
    srcIds.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      srcIds.push_back(v);
    }

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      reply->set_ok(false);
      reply->set_error("make_alias: ui dispatcher not ready");
      return Status::OK;
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, srcIds = std::move(srcIds)]() {
        return disp->makeAliases(srcIds);
      },
      "make_alias");
    if (!inv.ok) {
      reply->set_ok(false);
      reply->set_error(inv.error);
      return Status::OK;
    }
    const auto& r = inv.value;
    for (const auto& p : r.aliases) {
      MakeAliasResult* out = reply->add_aliases();
      out->set_src_id(p.srcId);
      out->set_alias_id(p.aliasId);
    }

    reply->set_ok(r.ok);
    if (!r.error.empty()) {
      reply->set_error(r.error);
    }
    return Status::OK;
  }

  Status EnsureAnimation(ServerContext* grpcContext,
                         const EnsureAnimationRequest* req,
                         EnsureAnimationResponse* reply) override
  {
    const bool createNew = req->create_new();
    const std::string nameStr = req->name();
    LOG(INFO) << "RPC EnsureAnimation create_new=" << createNew
              << (nameStr.empty() ? "" : (" name=" + nameStr));

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    const QString name = QString::fromStdString(nameStr);
    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, createNew, name]() {
        return disp->ensureAnimation3D(createNew, name);
      },
      "ensure_animation");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    const auto r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "ensure_animation failed" : r.error);
    }

    reply->set_ok(true);
    reply->set_animation_id(r.animationId);
    reply->set_created(r.created);
    return Status::OK;
  }

  Status SetDuration(ServerContext* grpcContext, const SetDurationRequest* req, Bool* reply) override
  {
    const double duration = req->duration();
    LOG(INFO) << "RPC SetDuration duration=" << duration;
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, animationId, duration]() {
        return disp->setAnimationDuration(animationId, duration);
      },
      "set_duration");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }

    const auto r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "set_duration failed" : r.error);
    }

    reply->set_ok(true);
    return Status::OK;
  }

  Status SetKey(ServerContext* grpcContext, const SetKeyRequest* req, Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    const double timeSec = req->time();
    const std::string easingStr = req->easing();
    const uint64_t targetId = req->target_id();
    const std::string jsonKeyStr = req->json_key();
    const json::value valueJson = pbToJson(req->value());

    LOG(INFO) << "RPC SetKey anim=" << animationId << " time=" << timeSec << " easing=" << easingStr
              << " target_id=" << targetId
              << (jsonKeyStr.empty() ? "" : (" json_key=" + jsonKeyStr));

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    ZRpcUiDispatcher::SetKeyRequest dr;
    dr.animationId = animationId;
    dr.targetId = targetId;
    dr.jsonKey = QString::fromStdString(jsonKeyStr);
    dr.timeSec = timeSec;
    dr.easing = QString::fromStdString(easingStr);
    dr.value = valueJson;

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, dr = std::move(dr)]() {
        return disp->setAnimationKey(dr);
      },
      "set_key");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    const auto r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "set_key failed" : r.error);
    }

    reply->set_ok(true);
    return Status::OK;
  }

  Status CameraGet(ServerContext* grpcContext, const google::protobuf::Empty*, CameraKeysResponse* reply) override
  {
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "camera_get: ui dispatcher not ready");
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp]() {
        return disp->cameraGet();
      },
      "camera_get");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    const auto& r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "camera_get failed" : r.error);
    }
    for (const auto& v : r.values) {
      *reply->add_values() = jsonToPb(v);
    }
    return Status::OK;
  }

  Status CameraFit(ServerContext* grpcContext, const CameraFitRequest* req, CameraKeysResponse* reply) override
  {
    CHECK(req);
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "camera_fit: ui dispatcher not ready");
    }

    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);
    if (canAutoWait) {
      std::vector<uint64_t> idsToWait;
      if (req->all() || req->ids_size() == 0) {
        auto invFit = invokeOnObjectThread(
          grpcContext,
          disp,
          [disp]() {
            return disp->fitCandidates();
          },
          "camera_fit:fit_candidates");
        if (!invFit.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, invFit.error);
        }
        idsToWait = std::move(invFit.value);
      } else {
        idsToWait.reserve(static_cast<size_t>(req->ids_size()));
        for (auto v : req->ids()) {
          idsToWait.push_back(static_cast<uint64_t>(v));
        }
      }

      if (!idsToWait.empty()) {
        const auto wait = waitForObjectViewsReady(grpcContext,
                                                  disp,
                                                  idsToWait,
                                                  kWaitForObjectsReadyDefaultPollMs,
                                                  "camera_fit:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
      }
    }

    ZRpcUiDispatcher::CameraFitRequest dr;
    dr.all = req->all();
    dr.afterClipping = req->after_clipping();
    dr.minRadius = req->min_radius();
    dr.ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      dr.ids.push_back(static_cast<size_t>(v));
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, dr = std::move(dr)]() {
        return disp->cameraFit(dr);
      },
      "camera_fit");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    const auto& r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "camera_fit failed" : r.error);
    }

    for (const auto& v : r.values) {
      *reply->add_values() = jsonToPb(v);
    }
    return Status::OK;
  }

  Status CameraOrbitSuggest(ServerContext* grpcContext,
                            const CameraOrbitSuggestRequest* req,
                            CameraKeysResponse* reply) override
  {
    CHECK(req);
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "camera_orbit_suggest: ui dispatcher not ready");
    }

    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);

    ZRpcUiDispatcher::CameraOrbitSuggestRequest dr;
    dr.axis = req->axis();
    dr.degrees = req->degrees();
    dr.ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      dr.ids.push_back(static_cast<size_t>(v));
    }

    auto invokeSuggest = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [disp, &dr]() {
          return disp->cameraOrbitSuggest(dr);
        },
        "camera_orbit_suggest");
    };

    auto inv = invokeSuggest();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = std::move(inv.value);
    if (canAutoWait && !r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        r.error.find("bbox empty") != std::string::npos) {
      const auto idsInv =
        resolveWaitIds(grpcContext, disp, req->ids(), /*emptyMeansAll=*/true, "camera_orbit_suggest:fit_candidates");
      if (!idsInv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, idsInv.error);
      }
      if (!idsInv.value.empty()) {
        const auto wait = waitForObjectViewsReady(grpcContext,
                                                  disp,
                                                  idsInv.value,
                                                  kWaitForObjectsReadyDefaultPollMs,
                                                  "camera_orbit_suggest:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
        inv = invokeSuggest();
        if (!inv.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
        }
        r = std::move(inv.value);
      }
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "camera_orbit_suggest failed" : r.error);
    }

    for (const auto& v : r.values) {
      *reply->add_values() = jsonToPb(v);
    }
    return Status::OK;
  }

  Status CameraDollySuggest(ServerContext* grpcContext,
                            const CameraDollySuggestRequest* req,
                            CameraKeysResponse* reply) override
  {
    CHECK(req);
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "camera_dolly_suggest: ui dispatcher not ready");
    }

    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);

    ZRpcUiDispatcher::CameraDollySuggestRequest dr;
    dr.startDist = req->start_dist();
    dr.endDist = req->end_dist();
    dr.ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      dr.ids.push_back(static_cast<size_t>(v));
    }

    auto invokeSuggest = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [disp, &dr]() {
          return disp->cameraDollySuggest(dr);
        },
        "camera_dolly_suggest");
    };

    auto inv = invokeSuggest();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = std::move(inv.value);
    if (canAutoWait && !r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        r.error.find("bbox empty") != std::string::npos) {
      const auto idsInv =
        resolveWaitIds(grpcContext, disp, req->ids(), /*emptyMeansAll=*/true, "camera_dolly_suggest:fit_candidates");
      if (!idsInv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, idsInv.error);
      }
      if (!idsInv.value.empty()) {
        const auto wait = waitForObjectViewsReady(grpcContext,
                                                  disp,
                                                  idsInv.value,
                                                  kWaitForObjectsReadyDefaultPollMs,
                                                  "camera_dolly_suggest:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
        inv = invokeSuggest();
        if (!inv.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
        }
        r = std::move(inv.value);
      }
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "camera_dolly_suggest failed" : r.error);
    }

    for (const auto& v : r.values) {
      *reply->add_values() = jsonToPb(v);
    }
    return Status::OK;
  }

  Status CameraFocus(ServerContext* grpcContext, const CameraFocusRequest* req, CameraKeysResponse* reply) override
  {
    CHECK(req);
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "camera_focus: ui dispatcher not ready");
    }

    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);

    ZRpcUiDispatcher::CameraFocusRequest dr;
    dr.afterClipping = req->after_clipping();
    dr.minRadius = req->min_radius();
    dr.ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      dr.ids.push_back(static_cast<size_t>(v));
    }

    auto invokeFocus = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [disp, &dr]() {
          return disp->cameraFocus(dr);
        },
        "camera_focus");
    };

    auto inv = invokeFocus();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = std::move(inv.value);
    if (canAutoWait && !r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        r.error.find("bbox empty") != std::string::npos) {
      const auto idsInv =
        resolveWaitIds(grpcContext, disp, req->ids(), /*emptyMeansAll=*/true, "camera_focus:fit_candidates");
      if (!idsInv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, idsInv.error);
      }
      if (!idsInv.value.empty()) {
        const auto wait = waitForObjectViewsReady(grpcContext,
                                                  disp,
                                                  idsInv.value,
                                                  kWaitForObjectsReadyDefaultPollMs,
                                                  "camera_focus:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
        inv = invokeFocus();
        if (!inv.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
        }
        r = std::move(inv.value);
      }
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "camera_focus failed" : r.error);
    }

    for (const auto& v : r.values) {
      *reply->add_values() = jsonToPb(v);
    }
    return Status::OK;
  }

  Status CameraPointTo(ServerContext* grpcContext,
                       const CameraPointToRequest* req,
                       CameraKeysResponse* reply) override
  {
    CHECK(req);
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "camera_point_to: ui dispatcher not ready");
    }

    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);

    ZRpcUiDispatcher::CameraPointToRequest dr;
    dr.afterClipping = req->after_clipping();
    dr.ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      dr.ids.push_back(static_cast<size_t>(v));
    }

    auto invokePointTo = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [disp, &dr]() {
          return disp->cameraPointTo(dr);
        },
        "camera_point_to");
    };

    auto inv = invokePointTo();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = std::move(inv.value);
    if (canAutoWait && !r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        r.error.find("bbox empty") != std::string::npos) {
      const auto idsInv =
        resolveWaitIds(grpcContext, disp, req->ids(), /*emptyMeansAll=*/true, "camera_point_to:fit_candidates");
      if (!idsInv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, idsInv.error);
      }
      if (!idsInv.value.empty()) {
        const auto wait = waitForObjectViewsReady(grpcContext,
                                                  disp,
                                                  idsInv.value,
                                                  kWaitForObjectsReadyDefaultPollMs,
                                                  "camera_point_to:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
        inv = invokePointTo();
        if (!inv.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
        }
        r = std::move(inv.value);
      }
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "camera_point_to failed" : r.error);
    }

    for (const auto& v : r.values) {
      *reply->add_values() = jsonToPb(v);
    }
    return Status::OK;
  }

  Status CameraRotate(ServerContext* grpcContext, const CameraRotateRequest* req, CameraKeysResponse* reply) override
  {
    CHECK(req);
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "camera_rotate: ui dispatcher not ready");
    }

    ZRpcUiDispatcher::CameraRotateRequest dr;
    dr.op = req->op();
    dr.degrees = req->degrees();
    if (!req->has_base_value()) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "camera_rotate: base_value is required");
    }
    {
      json::value jv = pbToJson(req->base_value());
      if (!jv.is_object()) {
        return Status(grpc::StatusCode::INVALID_ARGUMENT, "camera_rotate: base_value must be an object");
      }
      dr.baseValueOverride = std::move(jv);
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, dr = std::move(dr)]() {
        return disp->cameraRotate(dr);
      },
      "camera_rotate");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    const auto& r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "camera_rotate failed" : r.error);
    }

    for (const auto& v : r.values) {
      *reply->add_values() = jsonToPb(v);
    }
    return Status::OK;
  }

  Status CameraResetView(ServerContext* grpcContext,
                         const CameraResetViewRequest* req,
                         CameraKeysResponse* reply) override
  {
    CHECK(req);
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "camera_reset_view: ui dispatcher not ready");
    }

    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);

    ZRpcUiDispatcher::CameraResetViewRequest dr;
    dr.mode = req->mode();
    dr.afterClipping = req->after_clipping();
    dr.minRadius = req->min_radius();
    dr.ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      dr.ids.push_back(static_cast<size_t>(v));
    }

    auto invokeReset = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [disp, &dr]() {
          return disp->cameraResetView(dr);
        },
        "camera_reset_view");
    };

    auto inv = invokeReset();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = std::move(inv.value);
    if (canAutoWait && !r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        r.error.find("bbox empty") != std::string::npos) {
      const auto idsInv =
        resolveWaitIds(grpcContext, disp, req->ids(), /*emptyMeansAll=*/true, "camera_reset_view:fit_candidates");
      if (!idsInv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, idsInv.error);
      }
      if (!idsInv.value.empty()) {
        const auto wait = waitForObjectViewsReady(grpcContext,
                                                  disp,
                                                  idsInv.value,
                                                  kWaitForObjectsReadyDefaultPollMs,
                                                  "camera_reset_view:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
        inv = invokeReset();
        if (!inv.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
        }
        r = std::move(inv.value);
      }
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "camera_reset_view failed" : r.error);
    }

    for (const auto& v : r.values) {
      *reply->add_values() = jsonToPb(v);
    }
    return Status::OK;
  }

  Status CameraMoveLocal(ServerContext* grpcContext,
                         const CameraMoveLocalRequest* req,
                         CameraKeysResponse* reply) override
  {
    CHECK(req);
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "camera_move_local: ui dispatcher not ready");
    }

    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);
    const bool wantsBBox = req->distance_is_fraction_of_bbox_radius();

    ZRpcUiDispatcher::CameraMoveLocalRequest dr;
    dr.op = req->op();
    dr.distance = req->distance();
    dr.distanceIsFractionOfBBoxRadius = req->distance_is_fraction_of_bbox_radius();
    dr.afterClipping = req->after_clipping();
    dr.moveCenter = req->move_center();
    dr.ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      dr.ids.push_back(static_cast<size_t>(v));
    }
    if (!req->has_base_value()) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "camera_move_local: base_value is required");
    }
    {
      json::value jv = pbToJson(req->base_value());
      if (!jv.is_object()) {
        return Status(grpc::StatusCode::INVALID_ARGUMENT, "camera_move_local: base_value must be an object");
      }
      dr.baseValueOverride = std::move(jv);
    }

    auto invokeMove = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [disp, &dr]() {
          return disp->cameraMoveLocal(dr);
        },
        "camera_move_local");
    };

    auto inv = invokeMove();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = std::move(inv.value);
    if (canAutoWait && wantsBBox && !r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        r.error.find("bbox empty") != std::string::npos) {
      const auto idsInv =
        resolveWaitIds(grpcContext, disp, req->ids(), /*emptyMeansAll=*/true, "camera_move_local:fit_candidates");
      if (!idsInv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, idsInv.error);
      }
      if (!idsInv.value.empty()) {
        const auto wait = waitForObjectViewsReady(grpcContext,
                                                  disp,
                                                  idsInv.value,
                                                  kWaitForObjectsReadyDefaultPollMs,
                                                  "camera_move_local:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
        inv = invokeMove();
        if (!inv.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
        }
        r = std::move(inv.value);
      }
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "camera_move_local failed" : r.error);
    }

    for (const auto& v : r.values) {
      *reply->add_values() = jsonToPb(v);
    }
    return Status::OK;
  }

  Status CameraLookAt(ServerContext* grpcContext,
                      const CameraLookAtRequest* req,
                      CameraKeysResponse* reply) override
  {
    CHECK(req);
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "camera_look_at: ui dispatcher not ready");
    }

    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);
    const bool needsBBox = (req->target_case() == CameraLookAtRequest::kTargetBboxCenter ||
                            req->target_case() == CameraLookAtRequest::kBboxFractionPoint);

    ZRpcUiDispatcher::CameraLookAtRequest dr;
    dr.afterClipping = req->after_clipping();
    dr.ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      dr.ids.push_back(static_cast<size_t>(v));
    }
    if (!req->has_base_value()) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "camera_look_at: base_value is required");
    }
    {
      json::value jv = pbToJson(req->base_value());
      if (!jv.is_object()) {
        return Status(grpc::StatusCode::INVALID_ARGUMENT, "camera_look_at: base_value must be an object");
      }
      dr.baseValueOverride = std::move(jv);
    }

    switch (req->target_case()) {
      case CameraLookAtRequest::kWorldPoint: {
        dr.target = ZRpcUiDispatcher::CameraLookAtRequest::Target::WorldPoint;
        const auto& p = req->world_point();
        dr.worldPoint =
          glm::vec3(static_cast<float>(p.x()), static_cast<float>(p.y()), static_cast<float>(p.z()));
        break;
      }
      case CameraLookAtRequest::kTargetBboxCenter: {
        dr.target = ZRpcUiDispatcher::CameraLookAtRequest::Target::TargetBBoxCenter;
        break;
      }
      case CameraLookAtRequest::kBboxFractionPoint: {
        dr.target = ZRpcUiDispatcher::CameraLookAtRequest::Target::BBoxFractionPoint;
        const auto& f = req->bbox_fraction_point();
        dr.bboxFractionPoint = glm::dvec3(f.x(), f.y(), f.z());
        break;
      }
      default: {
        return Status(grpc::StatusCode::INVALID_ARGUMENT, "camera_look_at: target is required");
      }
    }

    auto invokeLookAt = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [disp, &dr]() {
          return disp->cameraLookAt(dr);
        },
        "camera_look_at");
    };

    auto inv = invokeLookAt();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = std::move(inv.value);
    if (canAutoWait && needsBBox && !r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        r.error.find("bbox empty") != std::string::npos) {
      const auto idsInv =
        resolveWaitIds(grpcContext, disp, req->ids(), /*emptyMeansAll=*/true, "camera_look_at:fit_candidates");
      if (!idsInv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, idsInv.error);
      }
      if (!idsInv.value.empty()) {
        const auto wait = waitForObjectViewsReady(grpcContext,
                                                  disp,
                                                  idsInv.value,
                                                  kWaitForObjectsReadyDefaultPollMs,
                                                  "camera_look_at:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
        inv = invokeLookAt();
        if (!inv.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
        }
        r = std::move(inv.value);
      }
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "camera_look_at failed" : r.error);
    }

    for (const auto& v : r.values) {
      *reply->add_values() = jsonToPb(v);
    }
    return Status::OK;
  }

  Status CameraPathSolve(ServerContext* grpcContext,
                         const CameraPathSolveRequest* req,
                         CameraSolveResponse* reply) override
  {
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);

    ZRpcUiDispatcher::CameraPathSolveRequest dr;
    dr.afterClipping = req->after_clipping();
    dr.ids.reserve(req->ids_size());
    for (auto v : req->ids()) {
      dr.ids.push_back(static_cast<size_t>(v));
    }
    if (!req->has_base_value()) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "camera_path_solve: base_value is required");
    }
    {
      json::value jv = pbToJson(req->base_value());
      if (!jv.is_object()) {
        return Status(grpc::StatusCode::INVALID_ARGUMENT, "camera_path_solve: base_value must be an object");
      }
      dr.baseValueOverride = std::move(jv);
    }

    bool needsBBox = false;
    dr.waypoints.reserve(static_cast<size_t>(req->waypoints_size()));
    for (int i = 0; i < req->waypoints_size(); ++i) {
      const auto& w = req->waypoints(i);
      Z3DCameraPlannerPathWaypoint wp;
      wp.time = w.time();
      wp.index = i;
      switch (w.eye_case()) {
        case CameraWaypoint::kWorldEye: {
          wp.eyeMode = Z3DCameraPlannerPathWaypoint::EyeMode::World;
          const auto& p = w.world_eye();
          wp.eyeWorld = glm::vec3(static_cast<float>(p.x()), static_cast<float>(p.y()), static_cast<float>(p.z()));
          break;
        }
        case CameraWaypoint::kBboxFractionEye: {
          needsBBox = true;
          wp.eyeMode = Z3DCameraPlannerPathWaypoint::EyeMode::BBoxFraction;
          const auto& f = w.bbox_fraction_eye();
          wp.eyeBBoxFraction = glm::dvec3(f.x(), f.y(), f.z());
          break;
        }
        default: {
          wp.eyeMode = Z3DCameraPlannerPathWaypoint::EyeMode::Keep;
          break;
        }
      }

      switch (w.look_at_case()) {
        case CameraWaypoint::kWorldLookAt: {
          wp.lookAtMode = Z3DCameraPlannerPathWaypoint::LookAtMode::World;
          const auto& p = w.world_look_at();
          wp.lookAtWorld =
            glm::vec3(static_cast<float>(p.x()), static_cast<float>(p.y()), static_cast<float>(p.z()));
          break;
        }
        case CameraWaypoint::kLookAtBboxCenter: {
          needsBBox = true;
          wp.lookAtMode = Z3DCameraPlannerPathWaypoint::LookAtMode::BBoxCenter;
          break;
        }
        case CameraWaypoint::kBboxFractionLookAt: {
          needsBBox = true;
          wp.lookAtMode = Z3DCameraPlannerPathWaypoint::LookAtMode::BBoxFraction;
          const auto& f = w.bbox_fraction_look_at();
          wp.lookAtBBoxFraction = glm::dvec3(f.x(), f.y(), f.z());
          break;
        }
        default: {
          wp.lookAtMode = Z3DCameraPlannerPathWaypoint::LookAtMode::KeepPrevDirection;
          break;
        }
      }
      dr.waypoints.push_back(std::move(wp));
    }

    auto invokeSolve = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [&]() {
          return disp->cameraPathSolve(dr);
        },
        "camera_path_solve");
    };

    auto inv = invokeSolve();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = std::move(inv.value);
    if (canAutoWait && needsBBox && !r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        (r.error.find("bbox empty") != std::string::npos || r.error.find("bbox required") != std::string::npos)) {
      const auto idsInv =
        resolveWaitIds(grpcContext, disp, req->ids(), /*emptyMeansAll=*/true, "camera_path_solve:fit_candidates");
      if (!idsInv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, idsInv.error);
      }
      if (!idsInv.value.empty()) {
        const auto wait = waitForObjectViewsReady(grpcContext,
                                                  disp,
                                                  idsInv.value,
                                                  kWaitForObjectsReadyDefaultPollMs,
                                                  "camera_path_solve:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
        inv = invokeSolve();
        if (!inv.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
        }
        r = std::move(inv.value);
      }
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "camera_path_solve failed" : r.error);
    }

    for (const auto& k : r.keys) {
      CameraSolveKey out;
      out.set_time(k.time);
      *out.mutable_value() = jsonToPb(k.value);
      *reply->add_keys() = std::move(out);
    }
    return Status::OK;
  }

  Status FitCandidates(ServerContext* grpcContext, const Empty*, FitCandidatesResponse* reply) override
  {
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "fit_candidates: ui dispatcher not ready");
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp]() {
        return disp->fitCandidates();
      },
      "fit_candidates");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }

    for (auto id : inv.value) {
      reply->add_ids(id);
    }
    return Status::OK;
  }

  Status CameraSolve(ServerContext* grpcContext, const CameraSolveRequest* req, CameraSolveResponse* reply) override
  {
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);

    ZRpcUiDispatcher::CameraSolveRequest dr;
    dr.mode = req->mode();
    dr.t0 = req->t0();
    dr.t1 = req->t1();
    dr.margin = req->has_constraints() ? req->constraints().margin() : 0.0;
    dr.ids.reserve(req->ids_size());
    for (auto v : req->ids()) {
      dr.ids.push_back(static_cast<size_t>(v));
    }

    const auto params = req->params();
    auto itA = params.fields().find("axis");
    if (itA != params.fields().end() && itA->second.kind_case() == google::protobuf::Value::kStringValue) {
      dr.orbitAxis = itA->second.string_value();
    }
    auto itAng = params.fields().find("degrees");
    if (itAng != params.fields().end() && itAng->second.kind_case() == google::protobuf::Value::kNumberValue) {
      dr.orbitDegrees = itAng->second.number_value();
    }
    auto itStep = params.fields().find("max_step_degrees");
    if (itStep != params.fields().end() && itStep->second.kind_case() == google::protobuf::Value::kNumberValue) {
      dr.orbitMaxStepDegrees = itStep->second.number_value();
    }
    auto itS = params.fields().find("start_dist");
    if (itS != params.fields().end() && itS->second.kind_case() == google::protobuf::Value::kNumberValue) {
      dr.dollyStartDist = itS->second.number_value();
    }
    auto itE = params.fields().find("end_dist");
    if (itE != params.fields().end() && itE->second.kind_case() == google::protobuf::Value::kNumberValue) {
      dr.dollyEndDist = itE->second.number_value();
    }

    auto invokeSolve = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [&]() {
          return disp->cameraSolve(dr);
        },
        "camera_solve");
    };

    auto inv = invokeSolve();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = std::move(inv.value);

    const bool maybeNeedsWait =
      (r.ok && r.keys.empty()) ||
      (!r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
       (r.error.find("bbox") != std::string::npos || r.error.find("target_not_ready") != std::string::npos));
    if (canAutoWait && maybeNeedsWait) {
      const auto idsInv =
        resolveWaitIds(grpcContext, disp, req->ids(), /*emptyMeansAll=*/true, "camera_solve:fit_candidates");
      if (!idsInv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, idsInv.error);
      }
      if (!idsInv.value.empty()) {
        const auto wait = waitForObjectViewsReady(grpcContext,
                                                  disp,
                                                  idsInv.value,
                                                  kWaitForObjectsReadyDefaultPollMs,
                                                  "camera_solve:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
        inv = invokeSolve();
        if (!inv.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
        }
        r = std::move(inv.value);
      }
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "camera_solve failed" : r.error);
    }

    for (const auto& k : r.keys) {
      CameraSolveKey out;
      out.set_time(k.time);
      *out.mutable_value() = jsonToPb(k.value);
      *reply->add_keys() = std::move(out);
    }
    if (reply->keys_size() == 0) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION,
                    "camera_solve: no keys produced (check target ids and scene bbox)");
    }
    return Status::OK;
  }

  Status CameraValidate(ServerContext* grpcContext,
                        const CameraValidateRequest* req,
                        CameraValidateResponse* reply) override
  {
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }
    if (!req) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "request missing");
    }

    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);

    ZRpcUiDispatcher::CameraValidateRequest dr;
    dr.afterClipping = false;

    dr.ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      dr.ids.push_back(static_cast<size_t>(v));
    }

    dr.times.reserve(static_cast<size_t>(req->times_size()));
    for (auto t : req->times()) {
      dr.times.push_back(t);
    }

    dr.values.reserve(static_cast<size_t>(req->values_size()));
    for (const auto& v : req->values()) {
      dr.values.push_back(pbToJson(v));
    }

    dr.animationId = req->animation_id();

    dr.adjustDistance = req->has_policies() ? req->policies().adjust_distance() : false;

    dr.keepVisible = req->has_constraints() ? req->constraints().keep_visible() : true;
    dr.margin = req->has_constraints() ? req->constraints().margin() : 0.0;
    dr.minFrameCoverage = req->has_constraints() ? req->constraints().min_frame_coverage() : 0.0;

    auto invokeValidate = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [&]() {
          return disp->cameraValidate(dr);
        },
        "camera_validate");
    };

    auto inv = invokeValidate();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = std::move(inv.value);
    if (canAutoWait && !r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        (r.error.find("bbox") != std::string::npos || r.error.find("target_not_ready") != std::string::npos)) {
      const auto idsInv =
        resolveWaitIds(grpcContext, disp, req->ids(), /*emptyMeansAll=*/true, "camera_validate:fit_candidates");
      if (!idsInv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, idsInv.error);
      }
      if (!idsInv.value.empty()) {
        const auto wait = waitForObjectViewsReady(grpcContext,
                                                  disp,
                                                  idsInv.value,
                                                  kWaitForObjectsReadyDefaultPollMs,
                                                  "camera_validate:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
        inv = invokeValidate();
        if (!inv.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
        }
        r = std::move(inv.value);
      }
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "camera_validate failed" : r.error);
    }

    for (const auto& vr : r.results) {
      CameraValidateResult out;
      out.set_time(vr.time);
      out.set_within_frame(vr.withinFrame);
      out.set_frame_coverage(vr.frameCoverage);
      out.set_adjusted(vr.adjusted);
      out.set_reason(vr.reason);
      if (vr.adjusted && vr.adjustedValue.has_value()) {
        *out.mutable_adjusted_value() = jsonToPb(*vr.adjustedValue);
      }
      *reply->add_results() = std::move(out);
    }
    reply->set_ok(r.allOk);
    return Status::OK;
  }

  Status CameraSample(ServerContext* grpcContext, const CameraSampleRequest* req, CameraSampleResponse* reply) override
  {
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }
    if (!req) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "request missing");
    }

    ZRpcUiDispatcher::CameraSampleRequest dr;
    dr.animationId = req->animation_id();
    dr.times.reserve(static_cast<size_t>(req->times_size()));
    for (auto t : req->times()) {
      dr.times.push_back(t);
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [&]() {
        return disp->cameraSample(dr);
      },
      "camera_sample");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = std::move(inv.value);
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "camera_sample failed" : r.error);
    }

    for (const auto& s : r.samples) {
      CameraSampleKey out;
      out.set_time(s.time);
      *out.mutable_value() = jsonToPb(s.value);
      *reply->add_samples() = std::move(out);
    }
    return Status::OK;
  }

  Status SetVisibility(ServerContext* grpcContext, const VisibilityRequest* req, Bool* reply) override
  {
    CHECK(req);
    const bool on = req->on();
    std::vector<size_t> ids;
    ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      ids.push_back(static_cast<size_t>(v));
    }

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "set_visibility: ui dispatcher not ready");
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, ids = std::move(ids), on]() {
        return disp->setVisibility(ids, on);
      },
      "set_visibility");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    const auto& r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "set_visibility failed" : r.error);
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status RemoveObjects(ServerContext* grpcContext, const RemoveObjectsRequest* req, Bool* reply) override
  {
    CHECK(req);
    CHECK(reply);

    std::vector<size_t> ids;
    ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      ids.push_back(static_cast<size_t>(v));
    }
    const bool allowUnsaved = req->allow_unsaved();

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "remove_objects: ui dispatcher not ready");
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, ids = std::move(ids), allowUnsaved]() {
        return disp->removeObjects(ids, allowUnsaved);
      },
      "remove_objects");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    const auto& r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "remove_objects failed" : r.error);
    }

    reply->set_ok(true);
    return Status::OK;
  }

  Status ListParams(ServerContext* grpcContext, const ListParamsRequest* req, ParamList* reply) override
  {
    CHECK(req);
    CHECK(reply);

    const size_t boundId = static_cast<size_t>(req->id());

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "list_params: ui dispatcher not ready");
    }

    auto invokeList = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [disp, boundId]() {
          return disp->listParams(boundId);
        },
        "list_params");
    };

    auto inv = invokeList();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = inv.value;
    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);
    if (canAutoWait && !r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        r.error.find("target_not_ready") != std::string::npos) {
      // See GetParamValues: list_params should behave like the GUI and wait for the object view to exist.
      if (boundId > kZRpcScopeGlobal) {
        const std::vector<uint64_t> ids = {static_cast<uint64_t>(boundId)};
        const auto wait =
          waitForObjectViewsReady(grpcContext, disp, ids, kWaitForObjectsReadyDefaultPollMs, "list_params:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
      } else {
        const auto wait = waitUntil(
          grpcContext,
          kWaitForObjectsReadyDefaultPollMs,
          [&]() -> bool {
            auto inv2 = invokeList();
            return inv2.ok && inv2.value.ok;
          },
          "list_params:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
      }

      inv = invokeList();
      if (!inv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
      }
      r = inv.value;
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "list_params failed" : r.error);
    }

    for (const auto& meta : r.params) {
      Parameter out;
      out.set_json_key(meta.jsonKey.toStdString());
      out.set_name(meta.name.toStdString());
      out.set_type(meta.type.toStdString());
      if (!meta.description.isEmpty()) {
        out.set_description(meta.description.toStdString());
      }
      out.set_supports_interpolation(meta.supportsInterpolation);
      out.mutable_value_schema()->CopyFrom(jsonToPb(meta.valueSchema).struct_value());
      *reply->add_params() = std::move(out);
    }
    return Status::OK;
  }

  Status ClearKeys(ServerContext* grpcContext, const ClearKeysRequest* req, Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    const uint64_t targetId = req->target_id();
    const std::string jsonKeyStr = req->json_key();
    LOG(INFO) << "RPC ClearKeys anim=" << animationId << " target_id=" << targetId
              << (jsonKeyStr.empty() ? "" : (" json_key=" + jsonKeyStr));

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    ZRpcUiDispatcher::ClearKeysRequest dr;
    dr.animationId = animationId;
    dr.targetId = targetId;
    dr.jsonKey = QString::fromStdString(jsonKeyStr);

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, dr = std::move(dr)]() {
        return disp->clearAnimationKeys(dr);
      },
      "clear_keys");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }

    const auto r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "clear_keys failed" : r.error);
    }

    reply->set_ok(true);
    return Status::OK;
  }

  Status RemoveKey(ServerContext* grpcContext, const RemoveKeyRequest* req, Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    const double timeSec = req->time();
    const uint64_t targetId = req->target_id();
    const std::string jsonKeyStr = req->json_key();
    LOG(INFO) << "RPC RemoveKey anim=" << animationId << " time=" << timeSec << " target_id=" << targetId
              << (jsonKeyStr.empty() ? "" : (" json_key=" + jsonKeyStr));

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    ZRpcUiDispatcher::RemoveKeyRequest dr;
    dr.animationId = animationId;
    dr.targetId = targetId;
    dr.jsonKey = QString::fromStdString(jsonKeyStr);
    dr.timeSec = timeSec;

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, dr = std::move(dr)]() {
        return disp->removeAnimationKey(dr);
      },
      "remove_key");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }

    const auto r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "remove_key failed" : r.error);
    }

    reply->set_ok(true);
    return Status::OK;
  }

  Status Batch(ServerContext* grpcContext, const BatchRequest* req, Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    const bool commit = req->commit();

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    ZRpcUiDispatcher::BatchKeysRequest dr;
    dr.animationId = animationId;
    dr.commit = commit;
    dr.removeKeys.reserve(static_cast<size_t>(req->remove_keys_size()));
    for (const auto& r : req->remove_keys()) {
      ZRpcUiDispatcher::RemoveKeyRequest op;
      op.animationId = animationId;
      op.targetId = r.target_id();
      op.jsonKey = QString::fromStdString(r.json_key());
      op.timeSec = r.time();
      dr.removeKeys.push_back(std::move(op));
    }
    dr.setKeys.reserve(static_cast<size_t>(req->set_keys_size()));
    for (const auto& s : req->set_keys()) {
      ZRpcUiDispatcher::SetKeyRequest op;
      op.animationId = animationId;
      op.targetId = s.target_id();
      op.jsonKey = QString::fromStdString(s.json_key());
      op.timeSec = s.time();
      op.easing = QString::fromStdString(s.easing());
      op.value = pbToJson(s.value());
      dr.setKeys.push_back(std::move(op));
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, dr = std::move(dr)]() {
        return disp->batchAnimationKeys(dr);
      },
      "batch");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }

    const auto r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "batch failed" : r.error);
    }

    reply->set_ok(true);
    return Status::OK;
  }

  Status SetTime(ServerContext* grpcContext, const SetTimeRequest* req, Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    const double seconds = req->seconds();
    const bool cancelRendering = req->cancel_rendering();
    VLOG(1) << "RPC SetTime anim=" << animationId << " seconds=" << seconds << " cancel=" << cancelRendering;

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    ZRpcUiDispatcher::SetTimeRequest dr;
    dr.animationId = animationId;
    dr.seconds = seconds;
    dr.cancelRendering = cancelRendering;

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, dr = std::move(dr)]() {
        return disp->setAnimationTime(dr);
      },
      "set_time");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }

    const auto r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "set_time failed" : r.error);
    }

    reply->set_ok(true);
    return Status::OK;
  }

  Status AddKeyFrame(ServerContext* grpcContext, const AddKeyFrameRequest* req, Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    const double timeSec = req->time();
    const bool cancelRendering = req->cancel_rendering();
    VLOG(1) << "RPC AddKeyFrame anim=" << animationId << " time=" << timeSec << " cancel=" << cancelRendering;

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    ZRpcUiDispatcher::AddKeyFrameRequest dr;
    dr.animationId = animationId;
    dr.timeSec = timeSec;
    dr.cancelRendering = cancelRendering;

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, dr = std::move(dr)]() {
        return disp->addAnimationKeyFrame(dr);
      },
      "add_keyframe");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }

    const auto r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "add_keyframe failed" : r.error);
    }

    reply->set_ok(true);
    return Status::OK;
  }

  Status SetCameraInterpolationMethod(ServerContext* grpcContext,
                                      const SetCameraInterpolationMethodRequest* req,
                                      Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    const std::string methodStr = req->method();
    LOG(INFO) << "RPC SetCameraInterpolationMethod anim=" << animationId << " method=" << methodStr;

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    const QString method = QString::fromStdString(methodStr);
    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, animationId, method]() {
        return disp->setCameraInterpolationMethod(animationId, method);
      },
      "set_camera_interpolation_method");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }

    const auto r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "set_camera_interpolation_method failed" : r.error);
    }

    reply->set_ok(true);
    return Status::OK;
  }

  Status GetCameraInterpolationMethod(ServerContext* grpcContext,
                                      const GetCameraInterpolationMethodRequest* req,
                                      google::protobuf::StringValue* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, animationId]() {
        return disp->cameraInterpolationMethod(animationId);
      },
      "get_camera_interpolation_method");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }

    const auto r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "get_camera_interpolation_method failed" : r.error);
    }

    reply->set_value(r.method);
    return Status::OK;
  }

  Status ListKeys(ServerContext* grpcContext, const ListKeysRequest* req, KeysResponse* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    const bool includeValues = req->include_values();
    const uint64_t targetId = req->target_id();
    const std::string jsonKeyStr = req->json_key();

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    ZRpcUiDispatcher::ListKeysRequest dr;
    dr.animationId = animationId;
    dr.targetId = targetId;
    dr.jsonKey = QString::fromStdString(jsonKeyStr);
    dr.includeValues = includeValues;

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, dr = std::move(dr)]() {
        return disp->listAnimationKeys(dr);
      },
      "list_keys");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }

    const auto r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "list_keys failed" : r.error);
    }

    for (const auto& k : r.keys) {
      KeyInfo* ki = reply->add_keys();
      ki->set_time(k.timeSec);
      ki->set_type(k.parameterType);
      if (includeValues && !k.valueJson.empty()) {
        ki->set_value_json(k.valueJson);
      }
    }
    return Status::OK;
  }

  Status GetTime(ServerContext* grpcContext, const GetTimeRequest* req, TimeStatus* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, animationId]() {
        return disp->animationTimeStatus(animationId);
      },
      "get_time");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }

    const auto r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "get_time failed" : r.error);
    }

    reply->set_duration(r.duration);
    reply->set_seconds(r.seconds);
    return Status::OK;
  }

  Status SaveAnimation(ServerContext* grpcContext, const SaveAnimationRequest* req, Bool* reply) override
  {
    const uint64_t animationId = req->animation_id();
    if (animationId == 0) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "animation_id is required");
    }
    const std::string pathStr = req->path();
    if (pathStr.empty()) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "path is required");
    }
    LOG(INFO) << "RPC SaveAnimation anim=" << animationId << " path=" << pathStr;

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    const QString qpath = QString::fromStdString(pathStr);
    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, animationId, qpath]() {
        return disp->saveAnimationToPath(animationId, qpath);
      },
      "save_animation");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }

    const auto r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "save_animation failed" : r.error);
    }

    reply->set_ok(true);
    return Status::OK;
  }

  Status SaveScene(ServerContext* grpcContext, const SaveSceneRequest* req, Bool* reply) override
  {
    const std::string pathStr = req->path();
    LOG(INFO) << "RPC SaveScene path=" << pathStr;
    if (pathStr.empty()) {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "path is required");
    }

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "ui dispatcher not ready");
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [&]() {
        return disp->saveSceneToPath(QString::fromStdString(pathStr));
      },
      "save_scene");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    const auto& r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "save_scene failed" : r.error);
    }
    reply->set_ok(true);
    return Status::OK;
  }

  Status TakeScreenshot3D(ServerContext* grpcContext, const ScreenshotRequest* req, ScreenshotResponse* reply) override
  {
    CHECK(req);
    CHECK(reply);

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      reply->set_ok(false);
      reply->set_error("take_screenshot_3d: ui dispatcher not ready");
      return Status::OK;
    }

    ZRpcUiDispatcher::Screenshot3DRequest dr;
    dr.width = static_cast<int>(req->width());
    dr.height = static_cast<int>(req->height());
    dr.path = QString::fromStdString(req->path());
    dr.overwrite = req->overwrite();

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, dr = std::move(dr)]() {
        return disp->takeScreenshot3D(dr);
      },
      "take_screenshot_3d");
    if (!inv.ok) {
      reply->set_ok(false);
      reply->set_error(inv.error);
      return Status::OK;
    }

    const auto& r = inv.value;
    reply->set_ok(r.ok);
    reply->set_path(r.path.toStdString());
    if (!r.error.empty()) {
      reply->set_error(r.error);
    }
    return Status::OK;
  }

  Status CutSet(ServerContext* grpcContext, const CutSetRequest* req, Bool* reply) override
  {
    CHECK(req);

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "cut_set: ui dispatcher not ready");
    }

    ZRpcUiDispatcher::CutSetRequest dr;
    dr.refitCamera = req->refit_camera();
    if (req->has_box()) {
      const auto& b = req->box();
      ZRpcUiDispatcher::CutBox bd;
      bd.minCorner = glm::dvec3(b.min().x(), b.min().y(), b.min().z());
      bd.maxCorner = glm::dvec3(b.max().x(), b.max().y(), b.max().z());
      dr.box = bd;
    } else if (req->has_planes()) {
      const auto& in = req->planes().planes();
      dr.planes.reserve(static_cast<size_t>(in.size()));
      for (const auto& p : in) {
        ZRpcUiDispatcher::CutPlane pd;
        pd.a = p.a();
        pd.b = p.b();
        pd.c = p.c();
        pd.d = p.d();
        dr.planes.push_back(pd);
      }
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp, dr = std::move(dr)]() {
        return disp->cutSet(dr);
      },
      "cut_set");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }

    const auto& r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "cut_set failed" : r.error);
    }

    reply->set_ok(true);
    return Status::OK;
  }

  Status CutClear(ServerContext* grpcContext, const Empty*, Bool* reply) override
  {
    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "cut_clear: ui dispatcher not ready");
    }

    auto inv = invokeOnObjectThread(
      grpcContext,
      disp,
      [disp]() {
        return disp->cutClear();
      },
      "cut_clear");
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    const auto& r = inv.value;
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "cut_clear failed" : r.error);
    }

    reply->set_ok(true);
    return Status::OK;
  }

  Status CutSuggest(ServerContext* grpcContext, const CutSuggestRequest* req, CutSuggestion* reply) override
  {
    CHECK(req);

    ZRpcUiDispatcher* disp = uiDispatcher();
    if (!disp) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, "cut_suggest: ui dispatcher not ready");
    }

    const bool canAutoWait = grpcContext && grpcHasFiniteDeadline(grpcContext);

    ZRpcUiDispatcher::CutSuggestRequest dr;
    dr.mode = req->mode();
    dr.margin = req->margin();
    dr.afterClipping = req->after_clipping();
    dr.ids.reserve(static_cast<size_t>(req->ids_size()));
    for (auto v : req->ids()) {
      dr.ids.push_back(static_cast<size_t>(v));
    }

    auto invokeSuggest = [&]() {
      return invokeOnObjectThread(
        grpcContext,
        disp,
        [disp, &dr]() {
          return disp->cutSuggestBox(dr);
        },
        "cut_suggest");
    };

    auto inv = invokeSuggest();
    if (!inv.ok) {
      return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
    }
    auto r = std::move(inv.value);
    if (canAutoWait && !r.ok && r.errorKind == ZRpcUiDispatcher::ErrorKind::FailedPrecondition &&
        r.error.find("bbox empty") != std::string::npos) {
      const auto idsInv =
        resolveWaitIds(grpcContext, disp, req->ids(), /*emptyMeansAll=*/true, "cut_suggest:fit_candidates");
      if (!idsInv.ok) {
        return Status(grpc::StatusCode::FAILED_PRECONDITION, idsInv.error);
      }
      if (!idsInv.value.empty()) {
        const auto wait = waitForObjectViewsReady(grpcContext,
                                                  disp,
                                                  idsInv.value,
                                                  kWaitForObjectsReadyDefaultPollMs,
                                                  "cut_suggest:wait_ready");
        if (!wait.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, wait.error);
        }
        inv = invokeSuggest();
        if (!inv.ok) {
          return Status(grpc::StatusCode::FAILED_PRECONDITION, inv.error);
        }
        r = std::move(inv.value);
      }
    }
    if (!r.ok) {
      const grpc::StatusCode code = (r.errorKind == ZRpcUiDispatcher::ErrorKind::InvalidArgument)
                                      ? grpc::StatusCode::INVALID_ARGUMENT
                                      : grpc::StatusCode::FAILED_PRECONDITION;
      return Status(code, r.error.empty() ? "cut_suggest failed" : r.error);
    }

    auto* out = reply->mutable_box();
    out->mutable_min()->set_x(r.minCorner.x);
    out->mutable_min()->set_y(r.minCorner.y);
    out->mutable_min()->set_z(r.minCorner.z);
    out->mutable_max()->set_x(r.maxCorner.x);
    out->mutable_max()->set_y(r.maxCorner.y);
    out->mutable_max()->set_z(r.maxCorner.z);
    out->mutable_size()->set_x(r.size.x);
    out->mutable_size()->set_y(r.size.y);
    out->mutable_size()->set_z(r.size.z);
    out->mutable_center()->set_x(r.center.x);
    out->mutable_center()->set_y(r.center.y);
    out->mutable_center()->set_z(r.center.z);
    return Status::OK;
  }

private:
  [[nodiscard]] ZRpcUiDispatcher* uiDispatcher() const
  {
    return m_uiDispatcher;
  }

  static void fillTaskStatus(const ZRpcTaskSnapshot& snap, TaskStatus* out)
  {
    CHECK(out);

    out->set_id(snap.id);
    out->set_kind(snap.kind);

    auto toProtoState = [](ZRpcTaskState st) -> TaskState {
      switch (st) {
        case ZRpcTaskState::Queued:
          return TASK_STATE_QUEUED;
        case ZRpcTaskState::Running:
          return TASK_STATE_RUNNING;
        case ZRpcTaskState::Succeeded:
          return TASK_STATE_SUCCEEDED;
        case ZRpcTaskState::Failed:
          return TASK_STATE_FAILED;
        case ZRpcTaskState::Cancelled:
          return TASK_STATE_CANCELLED;
      }
      return TASK_STATE_UNSPECIFIED;
    };

    out->set_state(toProtoState(snap.state));
    if (snap.progress.has_value()) {
      out->mutable_progress()->set_value(*snap.progress);
    }
    if (!snap.message.empty()) {
      out->set_message(snap.message);
    }
    if (!snap.error.empty()) {
      out->set_error(snap.error);
    }

    if (snap.loadResult.has_value()) {
      const auto& lr = *snap.loadResult;
      LoadTaskResult* load = out->mutable_load();
      for (const auto id : lr.loadedIds) {
        load->add_loaded_ids(id);
      }
      for (const auto& obj : lr.objects) {
        auto* o = load->add_objects();
        o->set_id(obj.id);
        o->set_type(obj.type);
        o->set_name(obj.name);
        o->set_path(obj.path);
        o->set_visible(obj.visible);
      }
      for (const auto& w : lr.warnings) {
        load->add_warnings(w);
      }
    }
  }

  std::string_view m_appVersion;
  ZRpcUiDispatcher* m_uiDispatcher = nullptr; // non-owning
  ZRpcTaskManager m_taskManager;
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
  m_sceneService = std::unique_ptr<grpc::Service>(new SceneServiceImpl(m_uiDispatcher, m_appVersion));

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
