#pragma once

#include "z3dcameraplanner.h"
#include "z3dviewsettingparamops.h"
#include "zjson.h"
#include "zrpcsceneids.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <map>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nim {

class ZMainWindow;
class ZNeuroglancerPrecomputedVolume;

class ZRpcUiDispatcher : public QObject
{
  Q_OBJECT

public:
  explicit ZRpcUiDispatcher(QObject* parent = nullptr);

  // Register the app's main window. This avoids repeated top-level widget scans
  // and makes the lifetime relationship explicit: RPC UI dispatch is anchored
  // to the main window/document model.
  void setMainWindow(ZMainWindow* mainWindow);

  enum class ErrorKind
  {
    FailedPrecondition,
    InvalidArgument,
  };

  struct StringResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    QString value;
  };

  // Return the installation root of the running Atlas instance.
  [[nodiscard]] StringResult appLocation() const;

  struct BoolResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
  };

  [[nodiscard]] BoolResult ensure3DWindow();

  struct CanvasSizeResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    int logicalWidth = 0;
    int logicalHeight = 0;
    int physicalWidth = 0;
    int physicalHeight = 0;
  };

  [[nodiscard]] CanvasSizeResult set3DCanvasSize(int logicalWidth, int logicalHeight);

  // Returns true when a 3D window exists and its rendering engine thread is running.
  // This does NOT create a 3D window; use ensure3DWindow() for that.
  [[nodiscard]] bool engineReady() const;

  struct BBoxValuesResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    glm::dvec3 minCorner{0.0, 0.0, 0.0};
    glm::dvec3 maxCorner{0.0, 0.0, 0.0};
    glm::dvec3 size{0.0, 0.0, 0.0};
    glm::dvec3 center{0.0, 0.0, 0.0};
  };

  // Compute a world-space bounding box for scene objects.
  // When ids is empty, uses all visual objects (non-Animation3D).
  [[nodiscard]] BBoxValuesResult bboxOfObjects(const std::vector<size_t>& ids, bool afterClipping);

  struct CutPlane
  {
    double a = 0.0;
    double b = 0.0;
    double c = 0.0;
    double d = 0.0;
  };

  struct CutBox
  {
    glm::dvec3 minCorner{0.0, 0.0, 0.0};
    glm::dvec3 maxCorner{0.0, 0.0, 0.0};
  };

  struct CutSetRequest
  {
    bool refitCamera = false;
    std::optional<CutBox> box;
    std::vector<CutPlane> planes;
  };

  [[nodiscard]] BoolResult cutSet(const CutSetRequest& req);
  [[nodiscard]] BoolResult cutClear();

  struct CutSuggestRequest
  {
    std::string mode; // "box" (current implementation)
    double margin = 0.0;
    bool afterClipping = false;
    std::vector<size_t> ids; // empty => all visual objects
  };

  [[nodiscard]] BBoxValuesResult cutSuggestBox(const CutSuggestRequest& req);

  struct Screenshot3DRequest
  {
    int width = 0;
    int height = 0;
    QString path; // may be empty => auto-generated temp path
    bool overwrite = false;
  };

  struct Screenshot3DResult
  {
    bool ok = false;
    QString path;
    std::string error;
  };

  [[nodiscard]] Screenshot3DResult takeScreenshot3D(const Screenshot3DRequest& req);

  struct RawMIPRequest
  {
    size_t id = 0;
    QString path; // may be empty => auto-generated temp path
    bool overwrite = false;
  };

  struct RawMIPResult
  {
    bool ok = false;
    QString path;
    std::string error;
  };

  [[nodiscard]] RawMIPResult exportRawMIP3D(const RawMIPRequest& req);

  struct ScreenSpaceSufficiencyAuditResult
  {
    bool ok = false;
    uint64_t contributingSamples = 0;
    uint64_t sufficientSamples = 0;
    uint64_t level0Samples = 0;
    uint64_t level0LimitedSamples = 0;
    uint64_t contributingPixels = 0;
    uint64_t sufficientPixels = 0;
    uint64_t level0Pixels = 0;
    uint64_t level0LimitedPixels = 0;
    double sufficientSampleFraction = 1.0;
    double sufficientPixelFraction = 1.0;
    double level0SampleFraction = 0.0;
    double level0LimitedSampleFraction = 0.0;
    double level0PixelFraction = 0.0;
    double level0LimitedPixelFraction = 0.0;
    std::string error;
  };

  [[nodiscard]] ScreenSpaceSufficiencyAuditResult exportScreenSpaceSufficiencyAudit3D(size_t id);

  struct GetParamValuesResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    json::object values;
  };

  [[nodiscard]] GetParamValuesResult getParamValues(size_t id, const std::vector<std::string>& jsonKeys);

  struct ListParamsResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    std::vector<Z3DViewSettingParamOps::ParameterMeta> params;
  };

  [[nodiscard]] ListParamsResult listParams(size_t id);

  struct CapabilitiesResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    std::vector<Z3DViewSettingParamOps::ParameterMeta> camera;
    std::vector<Z3DViewSettingParamOps::ParameterMeta> background;
    std::vector<Z3DViewSettingParamOps::ParameterMeta> axis;
    std::vector<Z3DViewSettingParamOps::ParameterMeta> global;
    std::map<QString, std::vector<Z3DViewSettingParamOps::ParameterMeta>> objects;
  };

  [[nodiscard]] CapabilitiesResult capabilities(const std::vector<uint64_t>& ids);

  struct ValidateSceneParamsResult
  {
    bool ok = false; // dispatcher/engine reachable
    std::string error;
    bool allOk = false; // all assignments valid
    std::vector<Z3DViewSettingParamOps::ValidateResult> results;
  };

  [[nodiscard]] ValidateSceneParamsResult validateSceneParams(
    const std::vector<Z3DViewSettingParamOps::SetParamData>& setParams);

  [[nodiscard]] BoolResult applySceneParams(const std::vector<Z3DViewSettingParamOps::SetParamData>& setParams);

  struct ListedObject
  {
    uint64_t id = 0;
    std::string type;
    std::string name;
    std::string path;
    bool visible = false;
  };

  struct ListObjectsResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    std::vector<ListedObject> objects;
  };

  [[nodiscard]] ListObjectsResult listObjects();

  // Load local files/dirs (same semantics as the GUI file-open path) and return the updated object list.
  [[nodiscard]] ListObjectsResult loadFilesAndListObjects(const QStringList& filePaths);

  struct AddNeuroglancerPrecomputedResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    uint64_t id = 0;
    QString rootUrl;
  };

  // Register an already-open Neuroglancer precomputed volume in the current document (UI thread).
  // This is used by RPC tasks that open network datasets off the UI thread.
  [[nodiscard]] AddNeuroglancerPrecomputedResult
  addNeuroglancerPrecomputedVolume(std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol, bool setVisible);

  [[nodiscard]] BoolResult setVisibility(const std::vector<size_t>& ids, bool on);

  // Remove objects from the document without prompting to save/discard changes.
  // When allowUnsaved is false, the operation fails if any object has unsaved changes.
  [[nodiscard]] BoolResult removeObjects(const std::vector<size_t>& ids, bool allowUnsaved);

  struct MakeAliasPair
  {
    uint64_t srcId = 0;
    uint64_t aliasId = 0;
  };

  struct MakeAliasResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    std::vector<MakeAliasPair> aliases;
    bool hadInvalidId = false;
    bool hadUnsupported = false;
  };

  [[nodiscard]] MakeAliasResult makeAliases(const std::vector<uint64_t>& srcIds);

  enum class ObjectLoadState
  {
    NotFound,
    DocNotReady,
    EngineNotReady,
    ViewNotReady,
    Ready,
    Error,
  };

  struct StatusObject
  {
    uint64_t id = 0;
    std::string type;
    std::string name;
    std::string path;
    bool visible = false;
    ObjectLoadState loadState = ObjectLoadState::DocNotReady;
    std::optional<double> progress;
    std::string error;
  };

  struct StatusSnapshot
  {
    bool ok = true;
    bool docReady = false;
    bool engineReady = false;
    bool has3DWindow = false;
    std::vector<StatusObject> objects;
    std::string error;
  };

  [[nodiscard]] StatusSnapshot statusSnapshot(const std::vector<uint64_t>& ids, bool includeAllObjects);

  [[nodiscard]] std::vector<uint64_t> fitCandidates();

  struct SaveSceneResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
  };

  [[nodiscard]] SaveSceneResult saveSceneToPath(const QString& path);

  struct EnsureAnimationResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    uint64_t animationId = 0;
    bool created = false;
  };

  [[nodiscard]] EnsureAnimationResult ensureAnimation3D(bool createNew, const QString& name);

  [[nodiscard]] BoolResult setAnimationDuration(uint64_t animationId, double duration);

  struct SetKeyRequest
  {
    uint64_t animationId = 0;
    uint64_t targetId = 0;
    QString jsonKey;
    double timeSec = 0.0;
    QString easing;
    json::value value;
  };

  [[nodiscard]] BoolResult setAnimationKey(const SetKeyRequest& req);

  struct RemoveKeyRequest
  {
    uint64_t animationId = 0;
    uint64_t targetId = 0;
    QString jsonKey;
    double timeSec = 0.0;
  };

  [[nodiscard]] BoolResult removeAnimationKey(const RemoveKeyRequest& req);

  struct ClearKeysRequest
  {
    uint64_t animationId = 0;
    uint64_t targetId = 0;
    QString jsonKey; // empty => clear all tracks for that target
  };

  [[nodiscard]] BoolResult clearAnimationKeys(const ClearKeysRequest& req);

  struct BatchKeysRequest
  {
    uint64_t animationId = 0;
    bool commit = false;
    std::vector<SetKeyRequest> setKeys;
    std::vector<RemoveKeyRequest> removeKeys;
  };

  [[nodiscard]] BoolResult batchAnimationKeys(const BatchKeysRequest& req);

  struct ListKeysRequest
  {
    uint64_t animationId = 0;
    uint64_t targetId = 0;
    QString jsonKey; // ignored for camera
    bool includeValues = false;
  };

  struct ListedKey
  {
    double timeSec = 0.0;
    std::string parameterType;
    std::string valueJson;
  };

  struct ListKeysResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    std::vector<ListedKey> keys;
  };

  [[nodiscard]] ListKeysResult listAnimationKeys(const ListKeysRequest& req);

  struct TimeStatusResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    double duration = 0.0;
    double seconds = 0.0;
  };

  [[nodiscard]] TimeStatusResult animationTimeStatus(uint64_t animationId);

  struct SetTimeRequest
  {
    uint64_t animationId = 0;
    double seconds = 0.0;
    bool cancelRendering = false;
  };

  [[nodiscard]] BoolResult setAnimationTime(const SetTimeRequest& req);

  struct AddKeyFrameRequest
  {
    uint64_t animationId = 0;
    double timeSec = 0.0;
    bool cancelRendering = false;
  };

  // UI "Save Key Frame" parity: snapshot the current scene state into the
  // animation timeline for all parameters (including camera).
  [[nodiscard]] BoolResult addAnimationKeyFrame(const AddKeyFrameRequest& req);

  struct SaveAnimationResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
  };

  [[nodiscard]] SaveAnimationResult saveAnimationToPath(uint64_t animationId, const QString& path);

  [[nodiscard]] BoolResult setCameraInterpolationMethod(uint64_t animationId, const QString& method);

  struct CameraInterpolationMethodResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    std::string method;
  };

  [[nodiscard]] CameraInterpolationMethodResult cameraInterpolationMethod(uint64_t animationId);

  struct CameraValuesResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    std::vector<json::value> values;
  };

  [[nodiscard]] CameraValuesResult cameraGet();

  struct CameraFitRequest
  {
    bool all = false;
    bool afterClipping = false;
    double minRadius = 0.0;
    std::vector<size_t> ids; // empty => all visual objects (unless all=false and caller requires ids)
  };

  [[nodiscard]] CameraValuesResult cameraFit(const CameraFitRequest& req);

  struct CameraOrbitSuggestRequest
  {
    std::vector<size_t> ids; // empty => all visual objects
    std::string axis; // "x" | "y" | "z"
    double degrees = 360.0; // default 360
  };

  [[nodiscard]] CameraValuesResult cameraOrbitSuggest(const CameraOrbitSuggestRequest& req);

  struct CameraDollySuggestRequest
  {
    std::vector<size_t> ids; // empty => all visual objects
    double startDist = 0.0;
    double endDist = 0.0;
  };

  [[nodiscard]] CameraValuesResult cameraDollySuggest(const CameraDollySuggestRequest& req);

  struct CameraFocusRequest
  {
    bool afterClipping = false;
    double minRadius = 0.0;
    std::vector<size_t> ids; // required (non-empty)
  };

  [[nodiscard]] CameraValuesResult cameraFocus(const CameraFocusRequest& req);

  struct CameraPointToRequest
  {
    bool afterClipping = false;
    std::vector<size_t> ids; // required (non-empty)
  };

  [[nodiscard]] CameraValuesResult cameraPointTo(const CameraPointToRequest& req);

  struct CameraRotateRequest
  {
    std::string op; // "AZIMUTH"|"ELEVATION"|"ROLL"|"YAW"|"PITCH"|"FLIP"
    double degrees = 0.0;
    std::optional<json::value> baseValueOverride; // must be object when provided
  };

  [[nodiscard]] CameraValuesResult cameraRotate(const CameraRotateRequest& req);

  struct CameraResetViewRequest
  {
    std::string mode; // "XY"|"XZ"|"YZ"|"RESET"
    bool afterClipping = false;
    double minRadius = 0.0;
    std::vector<size_t> ids; // empty => all visual objects
  };

  [[nodiscard]] CameraValuesResult cameraResetView(const CameraResetViewRequest& req);

  struct CameraMoveLocalRequest
  {
    std::string op; // "FORWARD"|"BACK"|"RIGHT"|"LEFT"|"UP"|"DOWN"
    double distance = 0.0;
    bool distanceIsFractionOfBBoxRadius = false;
    std::vector<size_t> ids; // used only when distanceIsFractionOfBBoxRadius=true; empty => all visual objects
    bool afterClipping = false; // used only when distanceIsFractionOfBBoxRadius=true
    bool moveCenter = false;
    std::optional<json::value> baseValueOverride; // must be object when provided
  };

  [[nodiscard]] CameraValuesResult cameraMoveLocal(const CameraMoveLocalRequest& req);

  struct CameraLookAtRequest
  {
    bool afterClipping = false;
    std::vector<size_t> ids; // used only for bbox targets; empty => all visual objects
    std::optional<json::value> baseValueOverride; // must be object when provided

    enum class Target
    {
      WorldPoint,
      TargetBBoxCenter,
      BBoxFractionPoint,
    };
    Target target = Target::WorldPoint;
    glm::vec3 worldPoint{0.f, 0.f, 0.f};
    glm::dvec3 bboxFractionPoint{0.0, 0.0, 0.0};
  };

  [[nodiscard]] CameraValuesResult cameraLookAt(const CameraLookAtRequest& req);

  struct CameraSolveRequest
  {
    std::string mode;
    double t0 = 0.0;
    double t1 = 0.0;
    std::vector<size_t> ids;
    double margin = 0.0;

    // Orbit params (mode == "ORBIT")
    std::string orbitAxis = "y";
    double orbitDegrees = 360.0;
    double orbitMaxStepDegrees = 90.0;

    // Dolly params (mode == "DOLLY")
    double dollyStartDist = 0.0;
    double dollyEndDist = 0.0;
  };

  struct CameraSolveResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    std::vector<Z3DCameraPlannerSolveKey> keys;
  };

  [[nodiscard]] CameraSolveResult cameraSolve(const CameraSolveRequest& req);

  struct CameraPathSolveRequest
  {
    std::vector<Z3DCameraPlannerPathWaypoint> waypoints;
    std::vector<size_t> ids; // used only when bbox is needed; empty => all visual objects
    bool afterClipping = false;
    std::optional<json::value> baseValueOverride; // must be object when provided
  };

  struct CameraPathSolveResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    std::vector<Z3DCameraPlannerSolveKey> keys;
  };

  [[nodiscard]] CameraPathSolveResult cameraPathSolve(const CameraPathSolveRequest& req);

  struct CameraValidateRequest
  {
    std::vector<size_t> ids; // empty => all visual objects
    bool afterClipping = false;

    std::vector<double> times;
    std::vector<json::value> values; // may be shorter than times; additional values sampled from animationId
    uint64_t animationId = 0;

    bool adjustDistance = false;

    bool keepVisible = true;
    double minFrameCoverage = 0.0;
    double margin = 0.0;
  };

  struct CameraValidateResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    bool allOk = false;
    std::vector<Z3DCameraPlannerValidateResult> results;
  };

  [[nodiscard]] CameraValidateResult cameraValidate(const CameraValidateRequest& req);

  struct CameraSampleRequest
  {
    uint64_t animationId = 0;
    std::vector<double> times;
  };

  struct CameraSampleResult
  {
    bool ok = false;
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    std::string error;
    std::vector<Z3DCameraPlannerSolveKey> samples;
  };

  [[nodiscard]] CameraSampleResult cameraSample(const CameraSampleRequest& req);

private:
  [[nodiscard]] ZMainWindow* mainWindowUi() const;

  QPointer<ZMainWindow> m_mainWindow;
};

} // namespace nim
