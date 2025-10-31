#include "zrpcservice.h"

#include "helloworld.grpc.pb.h"
#include "scene.grpc.pb.h"
#include "zservicemanager.h"
#include "zlog.h"
#include "zdoc.h"
#include "zmainwindow.h"
#include "../version/version.h"
#include "z3drenderingengine.h"
#include "z3danimationdoc.h"
#include "z3danimation.h"
#include "zparameteranimation.h"
#include "zcameraparameterkey.h"
#include "zcameraparameteranimation.h"
#include "zjson.h"
#include "z3dcameraparameter.h"
#include "zoptionparameter.h"
#include "znumericparameter.h"
#include <QThread>
#include <QTimer>
#include <QApplication>
#include <QtCore/QDebug>
#include <grpcpp/grpcpp.h>
#include <google/protobuf/struct.pb.h>

namespace nim {

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using helloworld::HelloRequest;
using helloworld::HelloReply;
using helloworld::Greeter;

using atlas::rpc::Scene;
using atlas::rpc::PingRequest;
using atlas::rpc::PingResponse;
using atlas::rpc::Empty;
using atlas::rpc::Bool;
using atlas::rpc::FileList;
using atlas::rpc::ListObjectsResponse;
using atlas::rpc::ObjectInfo;
using atlas::rpc::BBoxRequest;
using atlas::rpc::BBoxResponse;
using atlas::rpc::BBox;
using atlas::rpc::Vec3;
using atlas::rpc::CapabilitiesRequest;
using atlas::rpc::CapabilitiesResponse;
using atlas::rpc::Parameter;
using atlas::rpc::ParamList;
using atlas::rpc::EnsureAnimationRequest;
using atlas::rpc::SetDurationRequest;
using atlas::rpc::SetKeyRequest;
using atlas::rpc::Scope;
using atlas::rpc::CameraFitRequest;
using atlas::rpc::CameraOrbitSuggestRequest;
using atlas::rpc::CameraDollySuggestRequest;
using atlas::rpc::CameraKeysResponse;
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
using atlas::rpc::ClearKeysRequest;
using atlas::rpc::RemoveKeyRequest;
using atlas::rpc::BatchRequest;
using atlas::rpc::SetTimeRequest;
using atlas::rpc::PlayRequest;
using atlas::rpc::PauseRequest;
using atlas::rpc::SaveRequest;
using atlas::rpc::CutSetRequest;
using atlas::rpc::CutSuggestRequest;
using atlas::rpc::CutSuggestion;
using atlas::rpc::ListKeysRequest;
using atlas::rpc::KeysResponse;
using atlas::rpc::KeyInfo;
using atlas::rpc::TimeStatus;

// Logic and data behind the server's behavior.
class GreeterServiceImpl final : public Greeter::Service
{
  Status SayHello(ServerContext* context, const HelloRequest* request, HelloReply* reply) override
  {
    // Overwrite the call's compression algorithm to DEFLATE.
    context->set_compression_algorithm(GRPC_COMPRESS_DEFLATE);
    std::string prefix("Hello ");
    reply->set_message(prefix + request->name());
    return Status::OK;
  }
};

ZRPCService::ZRPCService(QObject* parent)
  : QObject(parent)
{}

ZRPCService::~ZRPCService() = default;

namespace {

template <class Func>
auto invokeOnUi(Func&& f)
{
  using R = decltype(f());
  QObject* uiObj = QCoreApplication::instance();
  // If we're already on the UI thread, invoke directly to avoid deadlock
  if (QThread::currentThread() == uiObj->thread()) {
    return f();
  }
  R result{};
  QMetaObject::invokeMethod(uiObj, [&]() { result = f(); }, Qt::BlockingQueuedConnection);
  return result;
}

// Convert google.protobuf.Value to boost::json::value
static json::value pbToJson(const google::protobuf::Value& v);
static json::value pbToJson(const google::protobuf::ListValue& lv)
{
  json::array arr;
  for (const auto& item : lv.values()) arr.push_back(pbToJson(item));
  return arr;
}
static json::value pbToJson(const google::protobuf::Struct& st)
{
  json::object obj;
  const auto& f = st.fields();
  for (const auto& [k, vv] : f) obj[k] = pbToJson(vv);
  return obj;
}
static json::value pbToJson(const google::protobuf::Value& v)
{
  using K = google::protobuf::Value::KindCase;
  switch (v.kind_case()) {
  case K::kNullValue: return nullptr;
  case K::kBoolValue: return v.bool_value();
  case K::kNumberValue: return v.number_value();
  case K::kStringValue: return json::value_from(v.string_value());
  case K::kListValue: return pbToJson(v.list_value());
  case K::kStructValue: return pbToJson(v.struct_value());
  default: return nullptr;
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
// Simple RPC-driven playback controller owned on the UI thread
struct RPCPlayback : QObject {
public:
  explicit RPCPlayback(Z3DAnimation* a, double fps, QObject* parent = nullptr)
    : QObject(parent), anim(a)
  {
    stepMs = static_cast<int>(1000.0 / std::max(1.0, fps));
    timer.setInterval(stepMs);
    QObject::connect(&timer, &QTimer::timeout, this, [this]() {
      if (!anim) return;
      double t = current;
      t += stepMs / 1000.0;
      if (t > anim->duration()) t = 0.0;
      anim->setCurrentTime(t);
      current = t;
    });
  }
  void start(double start) { current = start; timer.start(); }
  void stop() { timer.stop(); }
  QTimer timer; Z3DAnimation* anim = nullptr; int stepMs = 40; double current = 0.0;
};
static RPCPlayback* g_rpcPlayback = nullptr;

// Helpers used by camera planning/validation
static std::vector<size_t> filterVisualIds(ZRPCService& owner, const std::vector<size_t>& in)
{
  std::vector<size_t> out;
  out.reserve(in.size());
  for (auto id : in) {
    if (owner.doc()) {
      auto* od = owner.doc()->idToDoc(id);
      if (od && od->typeName() == QStringLiteral("Animation3D")) continue;
    }
    out.push_back(id);
  }
  return out;
}

static ZBBox<glm::dvec3> expandedByMarginFraction(const ZBBox<glm::dvec3>& bb, double marginFrac)
{
  if (bb.empty() || marginFrac <= 0.0) return bb;
  const glm::dvec3 half = (bb.maxCorner - bb.minCorner) * 0.5;
  const glm::dvec3 grow = half * marginFrac;
  ZBBox<glm::dvec3> out = bb;
  out.expand(bb.minCorner - grow);
  out.expand(bb.maxCorner + grow);
  return out;
}

static double bboxEnclosingSphereRadius(const ZBBox<glm::dvec3>& bb)
{
  if (bb.empty()) return 0.0;
  const glm::dvec3 sz = (bb.maxCorner - bb.minCorner);
  return 0.5 * std::sqrt(sz.x * sz.x + sz.y * sz.y + sz.z * sz.z);
}

static double requiredCenterDistanceForCoverage(const Z3DCamera& cam, double radius)
{
  if (radius <= 0.0) return 0.0;
  double angle = cam.fieldOfView();
  // Match Z3DCamera::resetCamera logic: use horizontal angle when AR < 1
  if (cam.aspectRatio() < 1.0f) {
    angle = 2.0 * std::atan(std::tan(angle * 0.5) * cam.aspectRatio());
  }
  const double s = std::sin(angle * 0.5);
  if (s <= 1e-6) return std::numeric_limits<double>::infinity();
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
        if (auto mw = qobject_cast<ZMainWindow*>(w)) { mainWin = mw; break; }
      }
      if (!mainWin) {
        LOG(WARNING) << "Ensure3DWindow: no ZMainWindow found";
        return false;
      }
      if (!mainWin->get3DWindow()) {
        QMetaObject::invokeMethod(mainWin, &ZMainWindow::ensure3DWindow, Qt::QueuedConnection);
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
      if (!m_owner.doc()) return false;
      QStringList qfiles;
      for (const auto& s : request->files()) qfiles << QString::fromStdString(s);
      m_owner.doc()->loadFileList(qfiles);
      return true;
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, "doc not ready");
    LOG(INFO) << "RPC LoadFiles done";
    return ListObjects(nullptr, nullptr, reply);
  }

  Status ListObjects(ServerContext*, const Empty*, ListObjectsResponse* reply) override
  {
    VLOG(1) << "RPC ListObjects";
    auto objs = invokeOnUi([&]() {
      std::vector<ObjectInfo> out;
      if (!m_owner.doc()) return out;
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
    if (!m_owner.engine()) return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    auto bb = invokeOnUi([&]() {
      std::vector<size_t> ids;
      ids.reserve(req->ids_size());
      for (auto v : req->ids()) {
        size_t id = static_cast<size_t>(v);
        // Filter out Animation3D objects from bbox calculations to avoid skewing camera fitting
        if (m_owner.doc()) {
          auto* od = m_owner.doc()->idToDoc(id);
          if (od && od->typeName() == QStringLiteral("Animation3D")) continue;
        }
        ids.push_back(id);
      }
      if (req->after_clipping()) {
        return m_owner.engine()->boundBoxOfObjsAfterClipping(ids);
      }
      return m_owner.engine()->boundBoxOfObjs(ids);
    });
    auto toVec = [](const glm::dvec3& v) {
      Vec3 r; r.set_x(v.x); r.set_y(v.y); r.set_z(v.z); return r; };
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
    if (!m_owner.engine()) return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    auto res = invokeOnUi([&]() {
      struct CapOut { std::vector<Parameter> bg, ax, gl; std::map<QString, std::vector<Parameter>> objs; };
      CapOut out;
      auto collect = [&](size_t id, std::vector<Parameter>& dst) {
        const auto params = m_owner.engine()->parametersOfViewSetting(id);
        for (auto* p : params) {
          Parameter meta;
          meta.set_json_key(p->jsonKey().toStdString());
          meta.set_name(p->name().toStdString());
          meta.set_type(p->type().toStdString());
          meta.set_supports_interpolation(p->supportInterpolation());
          if (auto opt = dynamic_cast<const ZStringIntOptionParameter*>(p)) {
            for (const auto& s : opt->options()) meta.add_option_names(s.toStdString());
            for (const auto& d : opt->optionAssociatedData()) meta.add_option_data(QString::number(d).toStdString());
          } else if (auto opt2 = dynamic_cast<const ZStringStringOptionParameter*>(p)) {
            for (const auto& s : opt2->options()) meta.add_option_names(s.toStdString());
            for (const auto& d : opt2->optionAssociatedData()) meta.add_option_data(d.toStdString());
          } else if (auto opt3 = dynamic_cast<const ZIntIntOptionParameter*>(p)) {
            for (const auto& s : opt3->options()) meta.add_option_names(QString::number(s).toStdString());
            for (const auto& d : opt3->optionAssociatedData()) meta.add_option_data(QString::number(d).toStdString());
          } else if (auto sp = dynamic_cast<const ZDoubleParameter*>(p)) {
            meta.set_number_min(sp->rangeMin()); meta.set_number_max(sp->rangeMax()); meta.set_number_step(sp->singleStep()); meta.set_decimal(sp->decimal());
          } else if (auto spf = dynamic_cast<const ZFloatParameter*>(p)) {
            meta.set_number_min(spf->rangeMin()); meta.set_number_max(spf->rangeMax()); meta.set_number_step(spf->singleStep()); meta.set_decimal(spf->decimal());
          } else if (auto spi = dynamic_cast<const ZIntParameter*>(p)) {
            meta.set_number_min(static_cast<double>(spi->rangeMin())); meta.set_number_max(static_cast<double>(spi->rangeMax())); meta.set_number_step(static_cast<double>(spi->singleStep())); meta.set_decimal(spi->decimal());
          } else if (auto v2 = dynamic_cast<const ZVec2Parameter*>(p)) {
            auto mn = v2->rangeMin(); auto mx = v2->rangeMax(); meta.add_vector_min(mn[0]); meta.add_vector_min(mn[1]); meta.add_vector_max(mx[0]); meta.add_vector_max(mx[1]); meta.set_decimal(v2->decimal());
          } else if (auto v3 = dynamic_cast<const ZVec3Parameter*>(p)) {
            auto mn = v3->rangeMin(); auto mx = v3->rangeMax(); meta.add_vector_min(mn[0]); meta.add_vector_min(mn[1]); meta.add_vector_min(mn[2]); meta.add_vector_max(mx[0]); meta.add_vector_max(mx[1]); meta.add_vector_max(mx[2]); meta.set_decimal(v3->decimal());
          } else if (auto v4 = dynamic_cast<const ZVec4Parameter*>(p)) {
            auto mn = v4->rangeMin(); auto mx = v4->rangeMax(); meta.add_vector_min(mn[0]); meta.add_vector_min(mn[1]); meta.add_vector_min(mn[2]); meta.add_vector_min(mn[3]); meta.add_vector_max(mx[0]); meta.add_vector_max(mx[1]); meta.add_vector_max(mx[2]); meta.add_vector_max(mx[3]); meta.set_decimal(v4->decimal());
          } else if (auto dv4 = dynamic_cast<const ZDVec4Parameter*>(p)) {
            auto mn = dv4->rangeMin(); auto mx = dv4->rangeMax(); meta.add_vector_min(mn[0]); meta.add_vector_min(mn[1]); meta.add_vector_min(mn[2]); meta.add_vector_min(mn[3]); meta.add_vector_max(mx[0]); meta.add_vector_max(mx[1]); meta.add_vector_max(mx[2]); meta.add_vector_max(mx[3]); meta.set_decimal(dv4->decimal());
          } else if (auto iv3 = dynamic_cast<const ZIVec3Parameter*>(p)) {
            auto mn = iv3->rangeMin(); auto mx = iv3->rangeMax(); meta.add_vector_min(mn[0]); meta.add_vector_min(mn[1]); meta.add_vector_min(mn[2]); meta.add_vector_max(mx[0]); meta.add_vector_max(mx[1]); meta.add_vector_max(mx[2]); meta.set_decimal(iv3->decimal());
          } else if (auto spn = dynamic_cast<const ZDoubleSpanParameter*>(p)) {
            auto mn = spn->rangeMin(); auto mx = spn->rangeMax(); meta.set_span_min(mn); meta.set_span_max(mx); meta.set_span_step(spn->singleStep()); meta.set_decimal(spn->decimal());
          } else if (auto spnf = dynamic_cast<const ZFloatSpanParameter*>(p)) {
            auto mn = spnf->rangeMin(); auto mx = spnf->rangeMax(); meta.set_span_min(mn); meta.set_span_max(mx); meta.set_span_step(spnf->singleStep()); meta.set_decimal(spnf->decimal());
          } else if (auto spni = dynamic_cast<const ZIntSpanParameter*>(p)) {
            auto mn = spni->rangeMin(); auto mx = spni->rangeMax(); meta.set_span_min(static_cast<double>(mn)); meta.set_span_max(static_cast<double>(mx)); meta.set_span_step(static_cast<double>(spni->singleStep())); meta.set_decimal(spni->decimal());
          }
          dst.push_back(std::move(meta));
        }
      };
      collect(1, out.bg); collect(2, out.ax); collect(3, out.gl);
      std::vector<size_t> ids;
      if (req->ids_size() == 0) ids = m_owner.doc() ? m_owner.doc()->objs() : std::vector<size_t>{};
      else { ids.reserve(req->ids_size()); for (auto v : req->ids()) ids.push_back(static_cast<size_t>(v)); }
      for (auto id : ids) {
        auto* od = m_owner.doc()->idToDoc(id);
        auto tn = od->typeName();
        std::vector<Parameter> pv;
        collect(id, pv);
        out.objs[tn] = std::move(pv);
      }
      return out;
    });
    for (auto& p : res.bg) *reply->add_background() = std::move(p);
    for (auto& p : res.ax) *reply->add_axis() = std::move(p);
    for (auto& p : res.gl) *reply->add_global() = std::move(p);
    for (auto& [tn, pv] : res.objs) {
      ParamList lst; for (auto& p : pv) *lst.add_params() = std::move(p);
      (*reply->mutable_objects())[tn.toStdString()] = std::move(lst);
    }
    return Status::OK;
  }

  Status EnsureAnimation(ServerContext*, const EnsureAnimationRequest*, Bool* reply) override
  {
    LOG(INFO) << "RPC EnsureAnimation";
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc()) return false;
      auto& ad = m_owner.doc()->animation3DDoc();
      if (ad.animationIds().empty()) ad.createNewAnimationAndReturnId("LLM Animation");
      return true;
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, "doc not ready");
    reply->set_ok(true);
    return Status::OK;
  }

  Status SetDuration(ServerContext*, const SetDurationRequest* req, Bool* reply) override
  {
    LOG(INFO) << "RPC SetDuration duration=" << req->duration();
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc()) return false;
      auto& ad = m_owner.doc()->animation3DDoc();
      auto ids = ad.animationIds();
      if (ids.empty()) ids.push_back(ad.createNewAnimationAndReturnId("LLM Animation"));
      if (auto* anim = ad.animationPtr(ids.front())) { anim->setDuration(req->duration()); return true; }
      return false;
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, "no animation");
    reply->set_ok(true);
    return Status::OK;
  }

  Status SetKey(ServerContext*, const SetKeyRequest* req, Bool* reply) override
  {
    LOG(INFO) << "RPC SetKey time=" << req->time() << " easing=" << req->easing()
              << (req->scope().has_camera() && req->scope().camera() ? " scope=camera"
                                                                   : (req->scope().has_object() ? " scope=object" : " scope=group"))
              << (req->json_key().empty() ? "" : (" json_key=" + req->json_key()));
    std::string errMsg;
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc() || !m_owner.engine()) { errMsg = "engine/doc not ready"; return false; }
      auto& ad = m_owner.doc()->animation3DDoc();
      auto ids = ad.animationIds();
      if (ids.empty()) ids.push_back(ad.createNewAnimationAndReturnId("LLM Animation"));
      auto* anim = ad.animationPtr(ids.front());
      if (!anim) { errMsg = "no animation available"; return false; }
      anim->rebindView();
      const double tm = req->time();
      const QString easing = QString::fromStdString(req->easing());
      // Camera
      if (req->scope().has_camera() && req->scope().camera()) {
        // Convert typed protobuf Value to boost::json object
        json::value jv = pbToJson(req->value());
        if (!jv.is_object()) { errMsg = "camera value must be an object"; return false; }
        json::object val = jv.as_object();
        json::object keyObj; keyObj["time"] = tm; keyObj["type"] = easing.toStdString(); keyObj["value"] = val;
        auto ckey = std::make_unique<ZCameraParameterKey>();
        if (!ckey->readValue(keyObj)) { errMsg = "camera value incompatible with parameter"; return false; }
        anim->cameraParameterAnimation()->addKey(std::move(ckey));
        return true;
      }
      // Group mapping 1/2/3
      size_t boundId = 0;
      if (req->scope().has_object()) {
        boundId = static_cast<size_t>(req->scope().object());
      } else if (req->scope().has_group()) {
        const auto g = QString::fromStdString(req->scope().group()).toLower();
        if (g == "background") boundId = 1; else if (g == "axis") boundId = 2; else if (g == "global") boundId = 3; else return false;
      } else {
        return false;
      }
      // Find target parameter by jsonKey
      const QString jsonKey = QString::fromStdString(req->json_key());
      ZParameter* target = nullptr;
      const auto params = m_owner.engine()->parametersOfViewSetting(boundId);
      for (auto* p : params) if (p && p->jsonKey() == jsonKey) { target = p; break; }
      if (!target) {
        LOG(WARNING) << "SetKey(param): target parameter not found boundId=" << boundId << " jsonKey=" << jsonKey.toStdString();
        errMsg = std::string("target parameter not found for json_key=") + jsonKey.toStdString();
        return false;
      }
      // Ensure parameter animations exist for this object
      anim->addKeyFrame(0.0);
      // Build key for that parameter from typed Value
      json::value v = pbToJson(req->value());

      auto jsonTypeName = [](const json::value& j) -> std::string {
        if (j.is_null()) return "null";
        if (j.is_bool()) return "bool";
        if (j.is_string()) return "string";
        if (j.is_number()) return "number";
        if (j.is_array()) return std::string("array[") + std::to_string(j.as_array().size()) + "]";
        if (j.is_object()) return "object";
        return "unknown";
      };
      auto expectArrayN = [&](size_t n) -> bool {
        if (!v.is_array() || v.as_array().size() != n) {
          errMsg = std::string("type mismatch for json_key=") + jsonKey.toStdString() +
                   ": expected array[" + std::to_string(n) + "] got " + jsonTypeName(v);
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
      const auto tstr = target->type();
      if (tstr == "Bool") {
        if (!v.is_bool()) {
          errMsg = std::string("type mismatch for json_key=") + jsonKey.toStdString() + ": expected Bool got " + jsonTypeName(v);
          return false;
        }
      } else if (tstr == "Float" || tstr == "Double" || tstr == "Int") {
        if (!v.is_number()) {
          errMsg = std::string("type mismatch for json_key=") + jsonKey.toStdString() + ": expected number got " + jsonTypeName(v);
          return false;
        }
      } else if (tstr.endsWith("Vec4")) {
        if (!expectArrayN(4)) return false;
      } else if (tstr.endsWith("Vec3")) {
        if (!expectArrayN(3)) return false;
      } else if (tstr.endsWith("Option")) {
        if (!v.is_string()) {
          errMsg = std::string("type mismatch for json_key=") + jsonKey.toStdString() + ": expected option string got " + jsonTypeName(v);
          return false;
        }
      }
      // For non-interpolatable parameters, force "Switch" to avoid invalid easing selection
      const QString keyType = target->supportInterpolation() ? easing : QStringLiteral("Switch");
      json::object keyObj; keyObj["time"] = tm; keyObj["type"] = keyType.toStdString(); keyObj["value"] = v;
      auto key = std::make_unique<ZParameterKey>(target->type());
      if (!key->readValue(keyObj)) { errMsg = "value incompatible with parameter schema"; return false; }
      // Locate the object's unique id from display packs, then access its parameter animations
      size_t uniqueId = 0;
      for (const auto& pack : anim->displayPacks()) {
        if (pack.type == ZAnimationDisplayPack::Type::Object && pack.boundId == boundId) {
          uniqueId = pack.id; break;
        }
      }
      if (uniqueId == 0) {
        LOG(WARNING) << "SetKey(param): object pack not found for boundId=" << boundId;
        errMsg = "object pack not found for boundId";
        return false;
      }
      const auto& pas = anim->paraAnimationList(uniqueId);
      for (const auto& paPtr : pas) {
        if (!paPtr) continue;
        auto* pa = paPtr.get();
        if (pa->jsonKey() == jsonKey && pa->type() == target->type()) {
          pa->addKey(std::move(key));
          return true;
        }
      }
      LOG(WARNING) << "SetKey(param): failed to locate paraAnimation for boundId=" << boundId
                   << " jsonKey=" << jsonKey.toStdString();
      errMsg = "failed to locate parameter animation for json_key";
      return false;
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, errMsg.empty() ? "set_key failed" : errMsg);
    reply->set_ok(true);
    return Status::OK;
  }

  Status CameraFit(ServerContext*, const CameraFitRequest* req, CameraKeysResponse* reply) override
  {
    VLOG(1) << "RPC CameraFit all=" << req->all() << " ids=" << req->ids_size();
    if (!m_owner.engine()) return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    auto values = invokeOnUi([&]() -> std::vector<json::value> {
      std::vector<json::value> out;
      if (!m_owner.engine()) return out;
      // Gather ids
      std::vector<size_t> ids;
      if (req->all() || req->ids_size() == 0) {
        if (m_owner.doc()) {
          ids = m_owner.doc()->objs();
          // Filter out Animation3D
          std::vector<size_t> filtered; filtered.reserve(ids.size());
          for (auto id : ids) {
            auto* od = m_owner.doc()->idToDoc(id);
            if (od && od->typeName() == QStringLiteral("Animation3D")) continue;
            filtered.push_back(id);
          }
          ids.swap(filtered);
        }
      } else {
        ids.reserve(req->ids_size());
        for (auto v : req->ids()) {
          size_t id = static_cast<size_t>(v);
          if (m_owner.doc()) {
            auto* od = m_owner.doc()->idToDoc(id);
            if (od && od->typeName() == QStringLiteral("Animation3D")) continue;
          }
          ids.push_back(id);
        }
      }
      // Compute bbox
      ZBBox<glm::dvec3> bb = req->after_clipping() ? m_owner.engine()->boundBoxOfObjsAfterClipping(ids)
                                                   : m_owner.engine()->boundBoxOfObjs(ids);
      if (bb.empty()) return out;
      // Create a temporary camera parameter and reset to bbox with optional min radius
      Z3DCameraParameter camTmp("Camera");
      camTmp.setValueSameAs(m_owner.engine()->camera());
      // Apply min radius by expanding bbox if requested
      if (req->min_radius() > 0.0) {
        const auto cent = (bb.minCorner + bb.maxCorner) * 0.5;
        const double r = req->min_radius();
        ZBBox<glm::dvec3> extra{cent - glm::dvec3(r), cent + glm::dvec3(r)};
        bb.expand(extra);
      }
      camTmp.resetCamera(bb, Z3DCamera::ResetOption::PreserveViewVector);
      const auto j = camTmp.jsonValue();
      out.emplace_back(json::value(j));
      return out;
    });
    for (const auto& jv : values) { *reply->add_values() = jsonToPb(jv); }
    return Status::OK;
  }

  Status CameraOrbitSuggest(ServerContext*, const CameraOrbitSuggestRequest* req, CameraKeysResponse* reply) override
  {
    if (!m_owner.engine()) return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    auto values = invokeOnUi([&]() -> std::vector<json::value> {
      std::vector<json::value> out;
      if (!m_owner.engine()) return out;
      // Compute bbox center
      std::vector<size_t> ids;
      if (req->ids_size() == 0) {
        if (m_owner.doc()) {
          ids = m_owner.doc()->objs();
          std::vector<size_t> filtered; filtered.reserve(ids.size());
          for (auto id : ids) {
            auto* od = m_owner.doc()->idToDoc(id);
            if (od && od->typeName() == QStringLiteral("Animation3D")) continue;
            filtered.push_back(id);
          }
          ids.swap(filtered);
        }
      } else {
        ids.reserve(req->ids_size());
        for (auto v : req->ids()) {
          size_t id = static_cast<size_t>(v);
          if (m_owner.doc()) {
            auto* od = m_owner.doc()->idToDoc(id);
            if (od && od->typeName() == QStringLiteral("Animation3D")) continue;
          }
          ids.push_back(id);
        }
      }
      ZBBox<glm::dvec3> bb = m_owner.engine()->boundBoxOfObjs(ids);
      if (bb.empty()) return out;
      const glm::vec3 center = glm::vec3((bb.minCorner + bb.maxCorner) * 0.5);
      const double angle = (req->angle_degrees() == 0.0 ? 360.0 : req->angle_degrees());
      // Axis
      glm::vec3 axis(0.f, 1.f, 0.f);
      const auto ax = QString::fromStdString(req->axis()).toLower();
      if (ax == "x") axis = glm::vec3(1.f, 0.f, 0.f);
      else if (ax == "y") axis = glm::vec3(0.f, 1.f, 0.f);
      else if (ax == "z") axis = glm::vec3(0.f, 0.f, 1.f);

      // Start: fit to bbox preserving view vector, using a temp camera
      Z3DCameraParameter startCam("Camera");
      startCam.setValueSameAs(m_owner.engine()->camera());
      startCam.resetCamera(bb, Z3DCamera::ResetOption::PreserveViewVector);
      // End: rotate around center on the selected axis
      Z3DCameraParameter endCam("Camera");
      endCam.setValueSameAs(startCam);
      endCam.rotate(static_cast<float>(angle), axis, center);

      out.emplace_back(json::value(startCam.jsonValue()));
      out.emplace_back(json::value(endCam.jsonValue()));
      return out;
    });
    for (const auto& jv : values) { *reply->add_values() = jsonToPb(jv); }
    return Status::OK;
  }

  Status CameraDollySuggest(ServerContext*, const CameraDollySuggestRequest* req, CameraKeysResponse* reply) override
  {
    if (!m_owner.engine()) return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    auto values = invokeOnUi([&]() -> std::vector<json::value> {
      std::vector<json::value> out;
      if (!m_owner.engine()) return out;
      // Determine center from bbox of ids
      std::vector<size_t> ids;
      if (req->ids_size() == 0) {
        if (m_owner.doc()) {
          ids = m_owner.doc()->objs();
          std::vector<size_t> filtered; filtered.reserve(ids.size());
          for (auto id : ids) {
            auto* od = m_owner.doc()->idToDoc(id);
            if (od && od->typeName() == QStringLiteral("Animation3D")) continue;
            filtered.push_back(id);
          }
          ids.swap(filtered);
        }
      } else {
        ids.reserve(req->ids_size());
        for (auto v : req->ids()) {
          size_t id = static_cast<size_t>(v);
          if (m_owner.doc()) {
            auto* od = m_owner.doc()->idToDoc(id);
            if (od && od->typeName() == QStringLiteral("Animation3D")) continue;
          }
          ids.push_back(id);
        }
      }
      ZBBox<glm::dvec3> bb = m_owner.engine()->boundBoxOfObjs(ids);
      if (bb.empty()) return out;
      const glm::vec3 center = glm::vec3((bb.minCorner + bb.maxCorner) * 0.5);

      Z3DCameraParameter startCam("Camera");
      startCam.setValueSameAs(m_owner.engine()->camera());
      startCam.setCenter(center);
      if (req->start_dist() > 0.0) startCam.dollyToCenterDistance(static_cast<float>(req->start_dist()));

      Z3DCameraParameter endCam("Camera");
      endCam.setValueSameAs(startCam);
      if (req->end_dist() > 0.0) endCam.dollyToCenterDistance(static_cast<float>(req->end_dist()));

      out.emplace_back(json::value(startCam.jsonValue()));
      out.emplace_back(json::value(endCam.jsonValue()));
      return out;
    });
    for (const auto& jv : values) { *reply->add_values() = jsonToPb(jv); }
    return Status::OK;
  }

  Status FitCandidates(ServerContext*, const Empty*, FitCandidatesResponse* reply) override
  {
    auto ids = invokeOnUi([&]() {
      std::vector<uint64_t> out;
      if (!m_owner.doc()) return out;
      for (auto id : m_owner.doc()->objs()) {
        auto* od = m_owner.doc()->idToDoc(id);
        if (od && od->typeName() == QStringLiteral("Animation3D")) continue;
        out.push_back(static_cast<uint64_t>(id));
      }
      return out;
    });
    for (auto id : ids) reply->add_ids(id);
    return Status::OK;
  }

  Status CameraSolve(ServerContext*, const CameraSolveRequest* req, CameraSolveResponse* reply) override
  {
    if (!m_owner.engine()) return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    auto keys = invokeOnUi([&]() {
      std::vector<CameraSolveKey> out;
      if (!m_owner.engine()) return out;
      // Collect and filter ids
      std::vector<size_t> ids;
      ids.reserve(req->ids_size());
      for (auto v : req->ids()) ids.push_back(static_cast<size_t>(v));
      ids = filterVisualIds(m_owner, ids);
      // Compute bbox
      ZBBox<glm::dvec3> bb = m_owner.engine()->boundBoxOfObjs(ids);
      const double margin = req->has_constraints() ? req->constraints().margin() : 0.0;
      if (!bb.empty() && margin > 0.0) bb = expandedByMarginFraction(bb, margin);
      const auto mode = QString::fromStdString(req->mode()).toUpper();
      const double t0 = req->t0();
      const double t1 = req->t1();
      Z3DCameraParameter base("Camera");
      base.setValueSameAs(m_owner.engine()->camera());
      if (mode == "FIT") {
        if (bb.empty()) return out; // nothing to fit
        Z3DCameraParameter cam("Camera");
        cam.setValueSameAs(base);
        cam.resetCamera(bb, Z3DCamera::ResetOption::PreserveViewVector);
        CameraSolveKey k; k.set_time(t0); *k.mutable_value() = jsonToPb(cam.jsonValue());
        out.push_back(std::move(k));
      } else if (mode == "STATIC") {
        CameraSolveKey k; k.set_time(t0); *k.mutable_value() = jsonToPb(base.jsonValue());
        out.push_back(std::move(k));
      } else if (mode == "ORBIT") {
        if (bb.empty()) return out;
        const auto params = req->params();
        std::string axis = "y";
        double angle = 360.0;
        auto itA = params.fields().find("axis");
        if (itA != params.fields().end() && itA->second.kind_case() == google::protobuf::Value::kStringValue) axis = itA->second.string_value();
        auto itAng = params.fields().find("angle_degrees");
        if (itAng != params.fields().end() && itAng->second.kind_case() == google::protobuf::Value::kNumberValue) angle = itAng->second.number_value();
        const glm::vec3 center = glm::vec3((bb.minCorner + bb.maxCorner) * 0.5);
        glm::vec3 ax(0.f, 1.f, 0.f);
        const auto axq = QString::fromStdString(axis).toLower();
        if (axq == "x") ax = glm::vec3(1.f, 0.f, 0.f);
        else if (axq == "y") ax = glm::vec3(0.f, 1.f, 0.f);
        else if (axq == "z") ax = glm::vec3(0.f, 0.f, 1.f);
        Z3DCameraParameter cam0("Camera"); cam0.setValueSameAs(base); cam0.resetCamera(bb, Z3DCamera::ResetOption::PreserveViewVector);
        Z3DCameraParameter cam1("Camera"); cam1.setValueSameAs(cam0); cam1.rotate(static_cast<float>(angle), ax, center);
        CameraSolveKey k0; k0.set_time(t0); *k0.mutable_value() = jsonToPb(cam0.jsonValue()); out.push_back(std::move(k0));
        CameraSolveKey k1; k1.set_time(t1); *k1.mutable_value() = jsonToPb(cam1.jsonValue()); out.push_back(std::move(k1));
      } else if (mode == "DOLLY") {
        if (bb.empty()) return out;
        const auto params = req->params();
        double start_dist = 0.0, end_dist = 0.0;
        auto itS = params.fields().find("start_dist");
        if (itS != params.fields().end() && itS->second.kind_case() == google::protobuf::Value::kNumberValue) start_dist = itS->second.number_value();
        auto itE = params.fields().find("end_dist");
        if (itE != params.fields().end() && itE->second.kind_case() == google::protobuf::Value::kNumberValue) end_dist = itE->second.number_value();
        const glm::vec3 center = glm::vec3((bb.minCorner + bb.maxCorner) * 0.5);
        Z3DCameraParameter cam0("Camera"); cam0.setValueSameAs(base); cam0.setCenter(center); if (start_dist > 0.0) cam0.dollyToCenterDistance(static_cast<float>(start_dist));
        Z3DCameraParameter cam1("Camera"); cam1.setValueSameAs(cam0); if (end_dist > 0.0) cam1.dollyToCenterDistance(static_cast<float>(end_dist));
        CameraSolveKey k0; k0.set_time(t0); *k0.mutable_value() = jsonToPb(cam0.jsonValue()); out.push_back(std::move(k0));
        CameraSolveKey k1; k1.set_time(t1); *k1.mutable_value() = jsonToPb(cam1.jsonValue()); out.push_back(std::move(k1));
      }
      return out;
    });
    for (auto& k : keys) { *reply->add_keys() = std::move(k); }
    if (reply->keys_size() == 0) return Status(grpc::StatusCode::FAILED_PRECONDITION, "camera_solve failed or empty");
    return Status::OK;
  }

  Status CameraValidate(ServerContext*, const CameraValidateRequest* req, CameraValidateResponse* reply) override
  {
    if (!m_owner.engine()) return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    std::string err;
    const bool allowFov = req->has_policies() ? req->policies().adjust_fov() : false;
    const bool allowDist = req->has_policies() ? req->policies().adjust_distance() : false;
    const double minCov = req->has_constraints() ? (req->constraints().min_coverage() > 0.0 ? req->constraints().min_coverage() : 0.95) : 0.95;
    const double margin = req->has_constraints() ? req->constraints().margin() : 0.0;
    auto results = invokeOnUi([&]() {
      std::vector<CameraValidateResult> out;
      if (!m_owner.engine()) return out;
      // Collect targets and bbox
      std::vector<size_t> ids; ids.reserve(req->ids_size()); for (auto v : req->ids()) ids.push_back(static_cast<size_t>(v));
      ids = filterVisualIds(m_owner, ids);
      ZBBox<glm::dvec3> bb = m_owner.engine()->boundBoxOfObjs(ids);
      if (!bb.empty() && margin > 0.0) bb = expandedByMarginFraction(bb, margin);
      const double R = bboxEnclosingSphereRadius(bb);
      // Base camera for aspect
      Z3DCameraParameter base("Camera"); base.setValueSameAs(m_owner.engine()->camera());
      // Validate each time/value
      const int n = std::min(req->times_size(), req->values_size());
      for (int i = 0; i < n; ++i) {
        double t = req->times(i);
        json::value jv = pbToJson(req->values(i));
        CameraValidateResult r; r.set_time(t);
        if (!jv.is_object()) {
          r.set_within_frame(false); r.set_coverage(0.0); r.set_adjusted(false); r.set_reason("invalid_value");
          out.push_back(std::move(r));
          continue;
        }
        Z3DCameraParameter cam("Camera"); cam.setValueSameAs(base); cam.readValue(jv);
        // Compute coverage heuristic
        const double required = (R > 0.0) ? requiredCenterDistanceForCoverage(cam.get(), R) : 0.0;
        const double current = static_cast<double>(cam.get().centerDist());
        double cov = 1.0;
        if (required > 1e-9) cov = std::min(1.0, current / required);
        bool ok = (cov + 1e-6) >= minCov;
        r.set_within_frame(ok);
        r.set_coverage(cov);
        // Adjustment policy
        bool adjusted = false;
        json::value adj = jv;
        if (!ok && R > 0.0) {
          if (allowDist && required > 0.0) {
            setCameraDistance(cam, required);
            adjusted = true;
            adj = cam.jsonValue();
            // Recompute coverage after adjustment
            const double cur2 = static_cast<double>(cam.get().centerDist());
            double cov2 = (required > 0.0) ? std::min(1.0, cur2 / required) : 1.0;
            ok = (cov2 + 1e-6) >= minCov;
            cov = cov2;
          } else if (allowFov && current > 1e-9) {
            // Solve desired FOV to achieve coverage with current distance
            double angleUsed = 2.0 * std::asin(std::min(1.0, R / current));
            double desiredFov = angleUsed;
            if (cam.get().aspectRatio() < 1.0f) {
              // angleUsed is horizontal; convert back to vertical FOV
              desiredFov = 2.0 * std::atan(std::tan(angleUsed * 0.5) / cam.get().aspectRatio());
            }
            Z3DCameraParameter cam2("Camera"); cam2.setValueSameAs(cam); cam2.setFrustum(static_cast<float>(desiredFov), cam.get().aspectRatio(), cam.get().nearDist(), cam.get().farDist());
            adjusted = true;
            adj = cam2.jsonValue();
            // Recompute coverage
            const double req2 = requiredCenterDistanceForCoverage(cam2.get(), R);
            const double cur2 = static_cast<double>(cam2.get().centerDist());
            double cov2 = (req2 > 1e-9) ? std::min(1.0, cur2 / req2) : 1.0;
            ok = (cov2 + 1e-6) >= minCov;
            cov = cov2;
          }
        }
        r.set_adjusted(adjusted);
        if (adjusted) { *r.mutable_adjusted_value() = jsonToPb(adj); }
        if (!ok) {
          if (current < required) {
            r.set_reason(allowDist ? "too_close" : (allowFov ? "fov_too_small" : "coverage_below_threshold"));
          } else {
            r.set_reason("coverage_below_threshold");
          }
        } else {
          r.set_reason("");
        }
        out.push_back(std::move(r));
      }
      return out;
    });
    bool allOk = true;
    for (auto& r : results) {
      if (!r.within_frame() || (r.coverage() + 1e-6) < (req->has_constraints() ? (req->constraints().min_coverage() > 0.0 ? req->constraints().min_coverage() : 0.95) : 0.95)) {
        allOk = false;
      }
      *reply->add_results() = std::move(r);
    }
    reply->set_ok(allOk);
    if (!allOk) return Status::OK; // strict acceptance handled client-side via revalidate
    return Status::OK;
  }

  Status SetVisibility(ServerContext*, const VisibilityRequest* req, Bool* reply) override
  {
    bool ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc()) return false;
      for (auto id : req->ids()) m_owner.doc()->setObjVisible(static_cast<size_t>(id), req->on());
      return true;
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, "doc not ready");
    reply->set_ok(true);
    return Status::OK;
  }

  Status ListParams(ServerContext*, const ListParamsRequest* req, ParamList* reply) override
  {
    if (!m_owner.engine()) return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    auto list = invokeOnUi([&]() {
      std::vector<Parameter> out;
      if (!m_owner.engine()) return out;
      // Camera: surface a synthetic entry (scope-only; SetKey ignores json_key for camera)
      if (req->scope().has_camera() && req->scope().camera()) {
        Parameter p; p.set_json_key("Camera 3DCamera"); p.set_name("Camera"); p.set_type("3DCamera"); p.set_supports_interpolation(true);
        out.push_back(std::move(p));
        return out;
      }
      size_t boundId = 0;
      if (req->scope().has_object()) {
        boundId = static_cast<size_t>(req->scope().object());
      } else if (req->scope().has_group()) {
        const auto g = QString::fromStdString(req->scope().group()).toLower();
        if (g == "background") boundId = 1; else if (g == "axis") boundId = 2; else if (g == "global") boundId = 3; else boundId = 0;
      }
      if (boundId == 0) return out;
      const auto params = m_owner.engine()->parametersOfViewSetting(boundId);
      for (auto* p : params) {
        if (!p) continue;
        Parameter meta;
        meta.set_json_key(p->jsonKey().toStdString());
        meta.set_name(p->name().toStdString());
        meta.set_type(p->type().toStdString());
        meta.set_supports_interpolation(p->supportInterpolation());
        if (auto opt = dynamic_cast<const ZStringIntOptionParameter*>(p)) {
          for (const auto& s : opt->options()) meta.add_option_names(s.toStdString());
          for (const auto& d : opt->optionAssociatedData()) meta.add_option_data(QString::number(d).toStdString());
        } else if (auto opt2 = dynamic_cast<const ZStringStringOptionParameter*>(p)) {
          for (const auto& s : opt2->options()) meta.add_option_names(s.toStdString());
          for (const auto& d : opt2->optionAssociatedData()) meta.add_option_data(d.toStdString());
        } else if (auto opt3 = dynamic_cast<const ZIntIntOptionParameter*>(p)) {
          for (const auto& s : opt3->options()) meta.add_option_names(QString::number(s).toStdString());
          for (const auto& d : opt3->optionAssociatedData()) meta.add_option_data(QString::number(d).toStdString());
        } else if (auto sp = dynamic_cast<const ZDoubleParameter*>(p)) {
          meta.set_number_min(sp->rangeMin()); meta.set_number_max(sp->rangeMax()); meta.set_number_step(sp->singleStep()); meta.set_decimal(sp->decimal());
        } else if (auto spf = dynamic_cast<const ZFloatParameter*>(p)) {
          meta.set_number_min(spf->rangeMin()); meta.set_number_max(spf->rangeMax()); meta.set_number_step(spf->singleStep()); meta.set_decimal(spf->decimal());
        } else if (auto spi = dynamic_cast<const ZIntParameter*>(p)) {
          meta.set_number_min(static_cast<double>(spi->rangeMin())); meta.set_number_max(static_cast<double>(spi->rangeMax())); meta.set_number_step(static_cast<double>(spi->singleStep())); meta.set_decimal(spi->decimal());
        } else if (auto v2 = dynamic_cast<const ZVec2Parameter*>(p)) {
          auto mn = v2->rangeMin(); auto mx = v2->rangeMax(); meta.add_vector_min(mn[0]); meta.add_vector_min(mn[1]); meta.add_vector_max(mx[0]); meta.add_vector_max(mx[1]); meta.set_decimal(v2->decimal());
        } else if (auto v3 = dynamic_cast<const ZVec3Parameter*>(p)) {
          auto mn = v3->rangeMin(); auto mx = v3->rangeMax(); meta.add_vector_min(mn[0]); meta.add_vector_min(mn[1]); meta.add_vector_min(mn[2]); meta.add_vector_max(mx[0]); meta.add_vector_max(mx[1]); meta.add_vector_max(mx[2]); meta.set_decimal(v3->decimal());
        } else if (auto v4 = dynamic_cast<const ZVec4Parameter*>(p)) {
          auto mn = v4->rangeMin(); auto mx = v4->rangeMax(); meta.add_vector_min(mn[0]); meta.add_vector_min(mn[1]); meta.add_vector_min(mn[2]); meta.add_vector_min(mn[3]); meta.add_vector_max(mx[0]); meta.add_vector_max(mx[1]); meta.add_vector_max(mx[2]); meta.add_vector_max(mx[3]); meta.set_decimal(v4->decimal());
        } else if (auto dv4 = dynamic_cast<const ZDVec4Parameter*>(p)) {
          auto mn = dv4->rangeMin(); auto mx = dv4->rangeMax(); meta.add_vector_min(mn[0]); meta.add_vector_min(mn[1]); meta.add_vector_min(mn[2]); meta.add_vector_min(mn[3]); meta.add_vector_max(mx[0]); meta.add_vector_max(mx[1]); meta.add_vector_max(mx[2]); meta.add_vector_max(mx[3]); meta.set_decimal(dv4->decimal());
        } else if (auto iv3 = dynamic_cast<const ZIVec3Parameter*>(p)) {
          auto mn = iv3->rangeMin(); auto mx = iv3->rangeMax(); meta.add_vector_min(mn[0]); meta.add_vector_min(mn[1]); meta.add_vector_min(mn[2]); meta.add_vector_max(mx[0]); meta.add_vector_max(mx[1]); meta.add_vector_max(mx[2]); meta.set_decimal(iv3->decimal());
        } else if (auto spn = dynamic_cast<const ZDoubleSpanParameter*>(p)) {
          auto mn = spn->rangeMin(); auto mx = spn->rangeMax(); meta.set_span_min(mn); meta.set_span_max(mx); meta.set_span_step(spn->singleStep()); meta.set_decimal(spn->decimal());
        } else if (auto spnf = dynamic_cast<const ZFloatSpanParameter*>(p)) {
          auto mn = spnf->rangeMin(); auto mx = spnf->rangeMax(); meta.set_span_min(mn); meta.set_span_max(mx); meta.set_span_step(spnf->singleStep()); meta.set_decimal(spnf->decimal());
        } else if (auto spni = dynamic_cast<const ZIntSpanParameter*>(p)) {
          auto mn = spni->rangeMin(); auto mx = spni->rangeMax(); meta.set_span_min(static_cast<double>(mn)); meta.set_span_max(static_cast<double>(mx)); meta.set_span_step(static_cast<double>(spni->singleStep())); meta.set_decimal(spni->decimal());
        }
        out.push_back(std::move(meta));
      }
      return out;
    });
    for (auto& p : list) *reply->add_params() = std::move(p);
    return Status::OK;
  }

  Status ClearKeys(ServerContext*, const ClearKeysRequest* req, Bool* reply) override
  {
    LOG(INFO) << "RPC ClearKeys"
              << (req->scope().has_camera() && req->scope().camera() ? " scope=camera"
                                                                   : (req->scope().has_object() ? " scope=object" : " scope=group"))
              << (req->json_key().empty() ? "" : (" json_key=" + req->json_key()));
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc() || !m_owner.engine()) return false;
      auto& ad = m_owner.doc()->animation3DDoc();
      auto ids = ad.animationIds();
      if (ids.empty()) return false;
      auto* anim = ad.animationPtr(ids.front());
      if (!anim) return false;
      anim->rebindView();
      // Camera scope: clear all camera keys
      if (req->scope().has_camera() && req->scope().camera()) {
        ZParameterAnimation* cpa = anim->cameraParameterAnimation();
        // delete keys one by one
        std::vector<ZParameterKey*> keys; keys.reserve(cpa->keys().size());
        for (const auto& k : cpa->keys()) keys.push_back(k.get());
        for (auto* k : keys) cpa->deleteKey(k);
        cpa->emitKeysChangedSignal();
        return true;
      }
      // Non-camera
      size_t boundId = 0;
      if (req->scope().has_object()) {
        boundId = static_cast<size_t>(req->scope().object());
      } else if (req->scope().has_group()) {
        const auto g = QString::fromStdString(req->scope().group()).toLower();
        if (g == "background") boundId = 1; else if (g == "axis") boundId = 2; else if (g == "global") boundId = 3; else return false;
      } else {
        return false;
      }
      const QString jsonKey = QString::fromStdString(req->json_key());
      for (const auto& pack : anim->displayPacks()) {
        if (pack.type == ZAnimationDisplayPack::Type::ObjectPara && pack.boundId == boundId && pack.paraAnimation) {
          if (pack.paraAnimation->jsonKey() == jsonKey) {
            std::vector<ZParameterKey*> keys; keys.reserve(pack.paraAnimation->keys().size());
            for (const auto& k : pack.paraAnimation->keys()) keys.push_back(k.get());
            for (auto* k : keys) pack.paraAnimation->deleteKey(k);
            pack.paraAnimation->emitKeysChangedSignal();
            return true;
          }
        }
      }
      return false;
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, "clear_keys failed");
    reply->set_ok(true);
    return Status::OK;
  }

  Status RemoveKey(ServerContext*, const RemoveKeyRequest* req, Bool* reply) override
  {
    LOG(INFO) << "RPC RemoveKey time=" << req->time()
              << (req->scope().has_camera() && req->scope().camera() ? " scope=camera"
                                                                   : (req->scope().has_object() ? " scope=object" : " scope=group"))
              << (req->json_key().empty() ? "" : (" json_key=" + req->json_key()));
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc() || !m_owner.engine()) return false;
      auto& ad = m_owner.doc()->animation3DDoc();
      auto ids = ad.animationIds();
      if (ids.empty()) return false;
      auto* anim = ad.animationPtr(ids.front());
      if (!anim) return false;
      anim->rebindView();
      // Camera
      if (req->scope().has_camera() && req->scope().camera()) {
        ZParameterAnimation* cpa = anim->cameraParameterAnimation();
        for (const auto& k : cpa->keys()) {
          if (std::abs(k->time() - req->time()) < 1e-6) { cpa->deleteKey(k.get()); cpa->emitKeysChangedSignal(); return true; }
        }
        return false;
      }
      // Non-camera
      size_t boundId = 0;
      if (req->scope().has_object()) {
        boundId = static_cast<size_t>(req->scope().object());
      } else if (req->scope().has_group()) {
        const auto g = QString::fromStdString(req->scope().group()).toLower();
        if (g == "background") boundId = 1; else if (g == "axis") boundId = 2; else if (g == "global") boundId = 3; else return false;
      } else {
        return false;
      }
      const QString jsonKey = QString::fromStdString(req->json_key());
      for (const auto& pack : anim->displayPacks()) {
        if (pack.type == ZAnimationDisplayPack::Type::ObjectPara && pack.boundId == boundId && pack.paraAnimation && pack.paraAnimation->jsonKey() == jsonKey) {
          for (const auto& k : pack.paraAnimation->keys()) {
            if (std::abs(k->time() - req->time()) < 1e-6) { pack.paraAnimation->deleteKey(k.get()); pack.paraAnimation->emitKeysChangedSignal(); return true; }
          }
        }
      }
      return false;
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, "remove_key failed");
    reply->set_ok(true);
    return Status::OK;
  }

  Status Batch(ServerContext*, const BatchRequest* req, Bool* reply) override
  {
    auto ok = invokeOnUi([&]() -> bool {
      LOG(INFO) << "RPC Batch set_keys=" << req->set_keys_size()
                << " remove_keys=" << req->remove_keys_size()
                << " commit=" << req->commit();
      if (!m_owner.doc() || !m_owner.engine()) return false;
      auto& ad = m_owner.doc()->animation3DDoc();
      auto ids = ad.animationIds();
      if (ids.empty()) ids.push_back(ad.createNewAnimationAndReturnId("LLM Animation"));
      auto* anim = ad.animationPtr(ids.front());
      if (!anim) return false;
      anim->rebindView();
      // Process removals first
      int idx = 0;
      for (const auto& r : req->remove_keys()) {
        RemoveKeyRequest tmp = r; Bool okR;
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
        SetKeyRequest tmp = s; Bool okS;
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
          if (std::abs(s.time()) < 1e-9) { hasTimeZero = true; break; }
        }
        if (hasTimeZero) {
          anim->cancelRenderingAndSetCurrentTime(0.0);
        }
      }
      return true;
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, "batch failed");
    reply->set_ok(true);
    return Status::OK;
  }

  Status SetTime(ServerContext*, const SetTimeRequest* req, Bool* reply) override
  {
    VLOG(1) << "RPC SetTime seconds=" << req->seconds() << " cancel=" << req->cancel_rendering();
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc()) return false;
      auto& ad = m_owner.doc()->animation3DDoc();
      auto ids = ad.animationIds();
      if (ids.empty()) return false;
      auto* anim = ad.animationPtr(ids.front());
      if (!anim) return false;
      if (req->cancel_rendering()) anim->cancelRenderingAndSetCurrentTime(req->seconds());
      else anim->setCurrentTime(req->seconds());
      return true;
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, "set_time failed");
    reply->set_ok(true);
    return Status::OK;
  }

  Status Play(ServerContext*, const PlayRequest* req, Bool* reply) override
  {
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc()) return false;
      auto& ad = m_owner.doc()->animation3DDoc();
      auto ids = ad.animationIds();
      if (ids.empty()) return false;
      auto* anim = ad.animationPtr(ids.front());
      if (!anim) return false;
      if (g_rpcPlayback) { g_rpcPlayback->stop(); g_rpcPlayback->deleteLater(); g_rpcPlayback = nullptr; }
      g_rpcPlayback = new RPCPlayback(anim, req->fps() <= 0.0 ? 25.0 : req->fps());
      g_rpcPlayback->start(0.0);
      return true;
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, "play failed");
    reply->set_ok(true);
    return Status::OK;
  }

  Status Pause(ServerContext*, const PauseRequest*, Bool* reply) override
  {
    invokeOnUi([&]() -> bool {
      if (g_rpcPlayback) { g_rpcPlayback->stop(); g_rpcPlayback->deleteLater(); g_rpcPlayback = nullptr; }
      return true;
    });
    reply->set_ok(true);
    return Status::OK;
  }

  Status ListKeys(ServerContext*, const ListKeysRequest* req, KeysResponse* reply) override
  {
    if (!m_owner.doc()) return Status(grpc::StatusCode::FAILED_PRECONDITION, "doc not ready");
    auto ok = invokeOnUi([&]() -> bool {
      auto& ad = m_owner.doc()->animation3DDoc();
      auto ids = ad.animationIds();
      if (ids.empty()) return false;
      auto* anim = ad.animationPtr(ids.front());
      if (!anim) return false;
      anim->rebindView();
      const bool includeValues = req->include_values();
      if (req->scope().has_camera() && req->scope().camera()) {
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
      // Non-camera: require json_key
      const QString jsonKey = QString::fromStdString(req->json_key());
      size_t boundId = 0;
      if (req->scope().has_object()) {
        boundId = static_cast<size_t>(req->scope().object());
      } else if (req->scope().has_group()) {
        const auto g = QString::fromStdString(req->scope().group()).toLower();
        if (g == "background") boundId = 1; else if (g == "axis") boundId = 2; else if (g == "global") boundId = 3; else return false;
      } else {
        return false;
      }
      for (const auto& pack : anim->displayPacks()) {
        if (pack.type == ZAnimationDisplayPack::Type::ObjectPara && pack.boundId == boundId && pack.paraAnimation && pack.paraAnimation->jsonKey() == jsonKey) {
          for (const auto& k : pack.paraAnimation->keys()) {
            KeyInfo* ki = reply->add_keys();
            ki->set_time(k->time());
            ki->set_type(pack.paraAnimation->type().toStdString());
            if (includeValues) {
              ki->set_value_json(jsonToString(k->jsonValue()));
            }
          }
          return true;
        }
      }
      return true; // no matching param; return empty list
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, "list_keys failed");
    return Status::OK;
  }

  Status GetTime(ServerContext*, const Empty*, TimeStatus* reply) override
  {
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc()) return false;
      auto& ad = m_owner.doc()->animation3DDoc();
      auto ids = ad.animationIds();
      if (ids.empty()) return false;
      auto* anim = ad.animationPtr(ids.front());
      if (!anim) return false;
      // duration is straightforward; current time is stored in animation
      reply->set_duration(anim->duration());
      reply->set_seconds(anim->currentTime());
      return true;
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, "get_time failed");
    return Status::OK;
  }

  Status Save(ServerContext*, const SaveRequest* req, Bool* reply) override
  {
    LOG(INFO) << "RPC Save path=" << req->path();
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.doc()) return false;
      auto& ad = m_owner.doc()->animation3DDoc();
      auto ids = ad.animationIds();
      if (ids.empty()) return false;
      auto* anim = ad.animationPtr(ids.front());
      if (!anim) return false;
      anim->save(QString::fromStdString(req->path()));
      return true;
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, "save failed");
    reply->set_ok(true);
    return Status::OK;
  }

  Status SaveAnimation(ServerContext*, const SaveRequest* req, Bool* reply) override
  {
    // Alias of Save for clarity in clients
    return Save(nullptr, req, reply);
  }

  Status CutSet(ServerContext*, const CutSetRequest* req, Bool* reply) override
  {
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.engine()) return false;
      auto& gp = m_owner.engine()->globalParas();
      bool applied = false;
      if (req->has_box()) {
        const auto& box = req->box();
        gp.globalXCut.setLowerValue(box.min().x()); gp.globalXCut.setUpperValue(box.max().x());
        gp.globalYCut.setLowerValue(box.min().y()); gp.globalYCut.setUpperValue(box.max().y());
        gp.globalZCut.setLowerValue(box.min().z()); gp.globalZCut.setUpperValue(box.max().z());
        applied = true;
      } else if (req->has_planes()) {
        // Support only axis-aligned planes: (1,0,0,-lower), (-1,0,0,upper), etc.
        double lx = gp.globalXCut.lowerValue(), ux = gp.globalXCut.upperValue();
        double ly = gp.globalYCut.lowerValue(), uy = gp.globalYCut.upperValue();
        double lz = gp.globalZCut.lowerValue(), uz = gp.globalZCut.upperValue();
        for (const auto& p : req->planes().planes()) {
          const double a = p.a(), b = p.b(), c = p.c(), d = p.d();
          if (a == 1.0 && b == 0.0 && c == 0.0) { lx = -d; applied = true; }
          else if (a == -1.0 && b == 0.0 && c == 0.0) { ux = d; applied = true; }
          else if (a == 0.0 && b == 1.0 && c == 0.0) { ly = -d; applied = true; }
          else if (a == 0.0 && b == -1.0 && c == 0.0) { uy = d; applied = true; }
          else if (a == 0.0 && b == 0.0 && c == 1.0) { lz = -d; applied = true; }
          else if (a == 0.0 && b == 0.0 && c == -1.0) { uz = d; applied = true; }
          else {
            // Unsupported plane orientation in current implementation
            return false;
          }
        }
        gp.globalXCut.setLowerValue(lx); gp.globalXCut.setUpperValue(ux);
        gp.globalYCut.setLowerValue(ly); gp.globalYCut.setUpperValue(uy);
        gp.globalZCut.setLowerValue(lz); gp.globalZCut.setUpperValue(uz);
      }
      if (!applied) return false;
      if (req->refit_camera()) {
        // Refit camera to visible objects after clipping
        std::vector<size_t> ids;
        if (m_owner.doc()) {
          ids = m_owner.doc()->objs();
          // Keep all ids; per-object visibility is honored by bbox-after-clipping
        }
        const auto bb = m_owner.engine()->boundBoxOfObjsAfterClipping(ids);
        if (!bb.empty()) {
          m_owner.engine()->camera().resetCamera(bb, Z3DCamera::ResetOption::PreserveViewVector);
        }
      }
      return true;
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, "cut_set failed");
    reply->set_ok(true);
    return Status::OK;
  }

  Status CutClear(ServerContext*, const Empty*, Bool* reply) override
  {
    auto ok = invokeOnUi([&]() -> bool {
      if (!m_owner.engine()) return false;
      auto& gp = m_owner.engine()->globalParas();
      gp.globalXCut.setLowerValue(gp.globalXCut.minimum()); gp.globalXCut.setUpperValue(gp.globalXCut.maximum());
      gp.globalYCut.setLowerValue(gp.globalYCut.minimum()); gp.globalYCut.setUpperValue(gp.globalYCut.maximum());
      gp.globalZCut.setLowerValue(gp.globalZCut.minimum()); gp.globalZCut.setUpperValue(gp.globalZCut.maximum());
      return true;
    });
    if (!ok) return Status(grpc::StatusCode::FAILED_PRECONDITION, "cut_clear failed");
    reply->set_ok(true);
    return Status::OK;
  }

  Status CutSuggest(ServerContext*, const CutSuggestRequest* req, CutSuggestion* reply) override
  {
    if (!m_owner.engine()) return Status(grpc::StatusCode::FAILED_PRECONDITION, "engine not ready");
    auto bbox = invokeOnUi([&]() {
      ZBBox<glm::dvec3> bb;
      if (!m_owner.engine()) return bb;
      std::vector<size_t> ids;
      if (req->ids_size() == 0) { if (m_owner.doc()) ids = m_owner.doc()->objs(); }
      else { ids.reserve(req->ids_size()); for (auto v : req->ids()) ids.push_back(static_cast<size_t>(v)); }
      bb = req->after_clipping() ? m_owner.engine()->boundBoxOfObjsAfterClipping(ids)
                                 : m_owner.engine()->boundBoxOfObjs(ids);
      if (!bb.empty() && req->margin() > 0.0) {
        const double m = req->margin();
        bb.expand(bb.minCorner - glm::dvec3(m));
        bb.expand(bb.maxCorner + glm::dvec3(m));
      }
      return bb;
    });
    if (bbox.empty()) return Status(grpc::StatusCode::FAILED_PRECONDITION, "bbox empty");
    if (req->mode().empty() || req->mode() == std::string("box")) {
      auto* out = reply->mutable_box();
      out->mutable_min()->set_x(bbox.minCorner.x); out->mutable_min()->set_y(bbox.minCorner.y); out->mutable_min()->set_z(bbox.minCorner.z);
      out->mutable_max()->set_x(bbox.maxCorner.x); out->mutable_max()->set_y(bbox.maxCorner.y); out->mutable_max()->set_z(bbox.maxCorner.z);
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
  m_greeterService.reset();
  m_sceneService.reset();
  m_grpcServer = nullptr;
}

void ZRPCService::onRPCThreadStarted()
{
  // Ensure we are on the RPC thread
  CHECK(g_sm->isCurrentOn(ZServiceManager::RPC));

  std::string server_address("0.0.0.0:50051");
  // Allocate services on the heap and keep them alive for the server lifetime.
  m_greeterService = std::unique_ptr<grpc::Service>(new GreeterServiceImpl());
  m_sceneService = std::unique_ptr<grpc::Service>(new SceneServiceImpl(*this));

  grpc::ServerBuilder builder;
  // Set the default compression algorithm for the server.
  builder.SetDefaultCompressionAlgorithm(GRPC_COMPRESS_GZIP);
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(m_greeterService.get());
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
