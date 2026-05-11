#include "zimgview.h"

#include "zbackgroundjob.h"
#include "zbackgroundtaskmanager.h"
#include "zdoc.h"
#include "zmeshdoc.h"
#include "zpunctadoc.h"
#include "zskeletondoc.h"
#include "zneuroglancerexternalsource.h"
#include "zneuroglancerprecomputedmesh.h"
#include "zneuroglancerprecomputedannotations.h"
#include "zneuroglancerprecomputedskeleton.h"
#include "zneuroglancerprecomputedsegmentproperties.h"
#include "zneuroglancerstate.h"
#include "zneuroglancersegmentpropertiesdialog.h"
#include "zlog.h"
#include "zswcdoc.h"
#include "zswcpack.h"
#include "zfolly.h"
#include "zsysteminfo.h"
#include "zseedtrace.h"
#include "ztracesettings.h"

#include <QDir>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QInputDialog>
#include <QMessageBox>
#include <QMenu>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QUrl>

#include <folly/OperationCancelled.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Task.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace nim {

namespace {

struct NeuroglancerMeshLoadResult
{
  QString error;
  std::shared_ptr<ZMesh> mesh;
  json::value sourceJson;
  QString displayName;
  QString tooltip;
};

struct NeuroglancerSkeletonLoadResult
{
  QString error;
  std::shared_ptr<ZSkeleton> skeleton;
  json::value sourceJson;
  QString displayName;
  QString tooltip;
};

struct NeuroglancerAnnotationsSourceOpenResult
{
  QString error;
  std::shared_ptr<const ZNeuroglancerPrecomputedAnnotationsSource> source;
  QStringList relationshipIds;
};

struct NeuroglancerAnnotationsLoadResult
{
  QString error;
  json::value sourceJson;
  QString displayName;
  QString tooltip;
  std::shared_ptr<ZPuncta> puncta;
  std::shared_ptr<ZSkeleton> skeleton;
};

void showNonBlockingCriticalMessage(QWidget* parent, const QString& title, const QString& text)
{
  auto* messageBox = new QMessageBox(QMessageBox::Critical, title, text, QMessageBox::Ok, parent);
  messageBox->setAttribute(Qt::WA_DeleteOnClose);
  messageBox->setWindowModality(Qt::NonModal);
  messageBox->setModal(false);
  messageBox->show();
}

struct ClipboardNeuroglancerMeshStateResult
{
  enum class Status
  {
    NotState,
    Matched,
    Error,
  };

  Status status = Status::NotState;
  QString error;
  QString matchedLayerName;
  bool layerVisible = true;
  std::map<uint64_t, bool> desiredSegmentVisibility;
};

[[nodiscard]] QString formatAnnotationPropertyValue(
  const ZNeuroglancerPrecomputedAnnotationsSource::Annotation::PropertyValue& v)
{
  return std::visit(
    [](auto&& value) -> QString {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, std::array<uint8_t, 3>>) {
        return QString("#%1%2%3")
          .arg(value[0], 2, 16, QChar('0'))
          .arg(value[1], 2, 16, QChar('0'))
          .arg(value[2], 2, 16, QChar('0'))
          .toUpper();
      } else if constexpr (std::is_same_v<T, std::array<uint8_t, 4>>) {
        return QString("rgba(%1,%2,%3,%4)").arg(value[0]).arg(value[1]).arg(value[2]).arg(value[3]);
      } else if constexpr (std::is_same_v<T, float>) {
        return QString::number(static_cast<double>(value), 'g', 8);
      } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        return QString::number(static_cast<long long>(value));
      } else if constexpr (std::is_integral_v<T> && !std::is_signed_v<T>) {
        return QString::number(static_cast<unsigned long long>(value));
      } else {
        static_assert(std::is_same_v<T, void>, "Unhandled annotation property value type");
        return {};
      }
    },
    v);
}

[[nodiscard]] std::optional<double> numericPropertyAsDouble(
  const ZNeuroglancerPrecomputedAnnotationsSource::Annotation::PropertyValue& v)
{
  return std::visit(
    [](auto&& value) -> std::optional<double> {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, std::array<uint8_t, 3>> || std::is_same_v<T, std::array<uint8_t, 4>>) {
        return std::nullopt;
      } else {
        return static_cast<double>(value);
      }
    },
    v);
}

void applyAnnotationPropertiesToPunctum(const ZNeuroglancerPrecomputedAnnotationsSource& source,
                                       const ZNeuroglancerPrecomputedAnnotationsSource::Annotation& ann,
                                       ZPunctum& punctum)
{
  const auto& specs = source.properties();
  if (specs.empty()) {
    return;
  }
  if (ann.propertyValues.size() != specs.size()) {
    return;
  }

  QStringList props;
  props.reserve(static_cast<int>(specs.size()));
  for (size_t i = 0; i < specs.size(); ++i) {
    const QString k = specs[i].id.trimmed();
    const QString v = formatAnnotationPropertyValue(ann.propertyValues[i]);
    props.push_back(QString("%1=%2").arg(k, v));

    if (k.compare(QStringLiteral("score"), Qt::CaseInsensitive) == 0) {
      if (const auto d = numericPropertyAsDouble(ann.propertyValues[i])) {
        punctum.setScore(*d);
      }
    }
  }

  if (!props.isEmpty()) {
    punctum.comment = props.join("; ").toStdString();
    punctum.property1 = props.value(0).toStdString();
    punctum.property2 = props.value(1).toStdString();
    punctum.property3 = props.value(2).toStdString();
  }
}

[[nodiscard]] std::array<double, 3> toArray(const glm::dvec3& v)
{
  return {v.x, v.y, v.z};
}

[[nodiscard]] bool rendersSpatialAnnotationsAsPuncta(ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType type)
{
  using AnnotationType = ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType;
  return type == AnnotationType::Point || type == AnnotationType::Ellipsoid;
}

[[nodiscard]] bool rendersSpatialAnnotationsAsSkeleton(ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType type)
{
  using AnnotationType = ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType;
  return type == AnnotationType::Line || type == AnnotationType::Polyline;
}

[[nodiscard]] bool isSpatialLoadDone(const ZNeuroglancerPrecomputedAnnotationsSource::SpatialLoadProgress& progress)
{
  return progress.levelsTotal > 0 && progress.levelIndex + 1 == progress.levelsTotal &&
         progress.visitedCells >= progress.totalCells;
}

[[nodiscard]] QString
spatialLoadStatusString(const ZNeuroglancerPrecomputedAnnotationsSource::SpatialLoadProgress& progress)
{
  if (isSpatialLoadDone(progress)) {
    return QStringLiteral("done");
  }

  return QString("loading… %1/%2 cells (level %3/%4)")
    .arg(progress.visitedCells)
    .arg(progress.totalCells)
    .arg(progress.levelIndex + 1)
    .arg(progress.levelsTotal);
}

[[nodiscard]] QString makeSpatialPunctaTooltip(QStringView segRootUrl,
                                               QStringView annRootUrl,
                                               const std::array<double, 3>& qMin,
                                               const std::array<double, 3>& qMax,
                                               QStringView status,
                                               uint64_t count)
{
  return QString("Neuroglancer precomputed annotations (spatial)\n"
                 "Segmentation: %1\n"
                 "Annotations: %2\n"
                 "Voxel box: [%3,%4,%5] - [%6,%7,%8]\n"
                 "Status: %9\n"
                 "Count: %10")
    .arg(segRootUrl)
    .arg(annRootUrl)
    .arg(qMin[0], 0, 'g', 8)
    .arg(qMin[1], 0, 'g', 8)
    .arg(qMin[2], 0, 'g', 8)
    .arg(qMax[0], 0, 'g', 8)
    .arg(qMax[1], 0, 'g', 8)
    .arg(qMax[2], 0, 'g', 8)
    .arg(status)
    .arg(count);
}

[[nodiscard]] QString makeSpatialSkeletonTooltip(QStringView segRootUrl,
                                                 QStringView annRootUrl,
                                                 const std::array<double, 3>& qMin,
                                                 const std::array<double, 3>& qMax,
                                                 QStringView status,
                                                 uint64_t segmentCount)
{
  return QString("Neuroglancer precomputed annotations (spatial lines)\n"
                 "Segmentation: %1\n"
                 "Annotations: %2\n"
                 "Voxel box: [%3,%4,%5] - [%6,%7,%8]\n"
                 "Status: %9\n"
                 "Segments: %10")
    .arg(segRootUrl)
    .arg(annRootUrl)
    .arg(qMin[0], 0, 'g', 8)
    .arg(qMin[1], 0, 'g', 8)
    .arg(qMin[2], 0, 'g', 8)
    .arg(qMax[0], 0, 'g', 8)
    .arg(qMax[1], 0, 'g', 8)
    .arg(qMax[2], 0, 'g', 8)
    .arg(status)
    .arg(segmentCount);
}

[[nodiscard]] std::shared_ptr<ZPuncta>
buildSpatialPunctaBatch(const ZNeuroglancerPrecomputedAnnotationsSource& source,
                        const std::vector<ZNeuroglancerPrecomputedAnnotationsSource::Annotation>& anns)
{
  auto batch = std::make_shared<ZPuncta>();
  for (const auto& a : anns) {
    if (a.points.size() != 1) {
      continue;
    }

    const auto& p = a.points[0];
    if (source.annotationType() == ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Ellipsoid &&
        a.ellipsoidRadiiVoxel) {
      const glm::vec3 r = *a.ellipsoidRadiiVoxel;
      const double rx = static_cast<double>(r.x);
      const double ry = static_cast<double>(r.y);
      const double rz = static_cast<double>(r.z);
      const double rMax = std::max({rx, ry, rz});
      ZPunctum punctum(p.x, p.y, p.z, /*r=*/rMax);
      punctum.setRadii(rx, ry, rz);
      punctum.name = std::to_string(static_cast<unsigned long long>(a.id));
      punctum.setMaxIntensity(255.0);
      punctum.setMeanIntensity(255.0);
      applyAnnotationPropertiesToPunctum(source, a, punctum);
      if (a.rgba8) {
        const auto& c = *a.rgba8;
        punctum.setColor(col4(c[0], c[1], c[2], c[3]));
      }
      batch->data.push_back(std::move(punctum));
      continue;
    }

    ZPunctum punctum(p.x, p.y, p.z, /*r=*/2.0);
    punctum.name = std::to_string(static_cast<unsigned long long>(a.id));
    punctum.setMaxIntensity(255.0);
    punctum.setMeanIntensity(255.0);
    applyAnnotationPropertiesToPunctum(source, a, punctum);
    if (a.rgba8) {
      const auto& c = *a.rgba8;
      punctum.setColor(col4(c[0], c[1], c[2], c[3]));
    }
    batch->data.push_back(std::move(punctum));
  }

  return batch;
}

[[nodiscard]] std::shared_ptr<std::pair<std::vector<glm::vec3>, std::vector<glm::uvec2>>>
buildSpatialSkeletonBatch(const std::vector<ZNeuroglancerPrecomputedAnnotationsSource::Annotation>& anns)
{
  auto geometry = std::make_shared<std::pair<std::vector<glm::vec3>, std::vector<glm::uvec2>>>();
  auto& vertices = geometry->first;
  auto& edges = geometry->second;

  for (const auto& a : anns) {
    if (a.points.size() < 2) {
      continue;
    }

    const size_t base = vertices.size();
    vertices.insert(vertices.end(), a.points.begin(), a.points.end());
    for (size_t i = 0; i + 1 < a.points.size(); ++i) {
      edges.emplace_back(static_cast<uint32_t>(base + i), static_cast<uint32_t>(base + i + 1));
    }
  }

  return geometry;
}

[[nodiscard]] std::vector<uint64_t> parseUint64ListFromText(const QString& text)
{
  // Accept any mix of whitespace / punctuation; extract all digit runs.
  // No truncation: returns every parsed value in order (deduping can be done by caller).
  std::vector<uint64_t> out;
  const QRegularExpression re(QStringLiteral("(\\d+)"));
  auto it = re.globalMatch(text);
  while (it.hasNext()) {
    const auto m = it.next();
    const auto vOpt = parseNeuroglancerUint64Base10(m.captured(1));
    if (vOpt) {
      out.push_back(*vOpt);
    }
  }
  return out;
}

[[nodiscard]] QString normalizedDirUrlForComparison(QString url)
{
  url = url.trimmed();
  if (url.isEmpty()) {
    return {};
  }
  while (url.endsWith('/')) {
    url.chop(1);
  }
  return url + '/';
}

[[nodiscard]] ClipboardNeuroglancerMeshStateResult
parseClipboardNeuroglancerMeshStateForDataset(const QString& text, const QString& targetRootUrl)
{
  ClipboardNeuroglancerMeshStateResult out;
  constexpr std::chrono::milliseconds kDefaultTimeout{30000};

  const auto input = ZNeuroglancerState::parseInputText(text, kDefaultTimeout);
  if (input.status == ZNeuroglancerState::InputStatus::NotRecognized) {
    out.status = ClipboardNeuroglancerMeshStateResult::Status::NotState;
    return out;
  }
  if (input.status == ZNeuroglancerState::InputStatus::Error) {
    out.status = ClipboardNeuroglancerMeshStateResult::Status::Error;
    out.error = input.error;
    return out;
  }

  const ZNeuroglancerState::ParseResult parsed = ZNeuroglancerState::parse(input.stateJson);
  const QString normalizedTargetRoot = ZNeuroglancerPrecomputedVolume::normalizeRootUrl(targetRootUrl);

  std::vector<const ZNeuroglancerState::Layer*> matches;
  for (const auto& layer : parsed.layers) {
    if (layer.type != ZNeuroglancerState::LayerType::Segmentation) {
      continue;
    }
    try {
      if (ZNeuroglancerPrecomputedVolume::normalizeRootUrl(layer.volumeUrl) == normalizedTargetRoot) {
        matches.push_back(&layer);
      }
    }
    catch (const std::exception&) {
      continue;
    }
  }

  if (matches.empty()) {
    out.status = ClipboardNeuroglancerMeshStateResult::Status::Error;
    out.error = QStringLiteral("Clipboard Neuroglancer state does not contain a segmentation layer for:\n%1")
                  .arg(normalizedTargetRoot);
    return out;
  }

  const ZNeuroglancerState::Layer* matchedLayer = nullptr;
  if (matches.size() == 1) {
    matchedLayer = matches.front();
  } else if (!parsed.selectedLayerName.trimmed().isEmpty()) {
    for (const auto* layer : matches) {
      if (layer->name == parsed.selectedLayerName) {
        if (matchedLayer != nullptr) {
          matchedLayer = nullptr;
          break;
        }
        matchedLayer = layer;
      }
    }
  }

  if (matchedLayer == nullptr) {
    out.status = ClipboardNeuroglancerMeshStateResult::Status::Error;
    out.error =
      QStringLiteral("Clipboard Neuroglancer state has multiple segmentation layers for:\n%1\n\n"
                     "Select one in Neuroglancer before copying the state, or use a plain segment ID list instead.")
        .arg(normalizedTargetRoot);
    return out;
  }

  out.status = ClipboardNeuroglancerMeshStateResult::Status::Matched;
  out.matchedLayerName = matchedLayer->name;
  out.layerVisible = matchedLayer->visible;
  for (const auto& segment : matchedLayer->segments) {
    out.desiredSegmentVisibility[segment.id] = segment.visible;
  }
  return out;
}

struct CollectVisibleSegIdsResult
{
  std::vector<uint64_t> ids;
  QString error;
};

struct BatchLoadMessageResult
{
  QString message;
  QString error;
};

using BatchLoadProgressFn = std::function<void(size_t processed, size_t total, QString message)>;

template<class Result, class Finish>
struct DocOwnedSingleShotState
{
  DocOwnedSingleShotState(QPointer<QObject> guardPtrIn, Finish finishIn)
    : guardPtr(std::move(guardPtrIn))
    , finish(std::move(finishIn))
  {}

  QPointer<QObject> guardPtr;
  Finish finish;
  std::optional<Result> result;
};

template<class Result, class Finish>
void finishDocOwnedSingleShotTaskOnUi(std::shared_ptr<DocOwnedSingleShotState<Result, Finish>> state)
{
  if (state->guardPtr == nullptr || QCoreApplication::closingDown() || !state->result.has_value()) {
    return;
  }
  state->finish(std::move(*state->result));
  state->result.reset();
}

template<class Result, class Finish>
folly::coro::Task<void> runDocOwnedSingleShotTask(folly::coro::Task<Result> workTask,
                                                  std::shared_ptr<DocOwnedSingleShotState<Result, Finish>> state)
{
  state->result.emplace(co_await std::move(workTask));
  if (state->guardPtr == nullptr || QCoreApplication::closingDown()) {
    co_return;
  }

  (void)QMetaObject::invokeMethod(
    state->guardPtr,
    [state = std::move(state)]() mutable {
      try {
        finishDocOwnedSingleShotTaskOnUi(std::move(state));
      }
      catch (const std::exception& e) {
        LOG(ERROR) << "Unhandled exception in doc-owned single-shot UI callback: " << e.what();
      }
      catch (...) {
        LOG(ERROR) << "Unhandled non-std exception in doc-owned single-shot UI callback";
      }
    },
    Qt::QueuedConnection);
  co_return;
}

template<class Result, class Finish>
void startDocOwnedSingleShotTask(ZDoc& doc,
                                 QObject* guardObject,
                                 std::string_view debugLabel,
                                 folly::coro::Task<Result> workTask,
                                 Finish&& finish)
{
  using FinishFn = std::decay_t<Finish>;
  auto state = std::make_shared<DocOwnedSingleShotState<Result, FinishFn>>(QPointer<QObject>(guardObject),
                                                                           std::forward<Finish>(finish));
  doc.backgroundTaskManager().spawnDetachedTask(getAtlasBackgroundExecutor(),
                                                runDocOwnedSingleShotTask(std::move(workTask), std::move(state)),
                                                debugLabel);
}

[[nodiscard]] QString cancellableSingleShotTaskTitle(std::string_view debugLabel)
{
  if (debugLabel == "ng_mesh_load") {
    return QStringLiteral("Load Neuroglancer Mesh");
  }
  if (debugLabel == "ng_mesh_batch_load") {
    return QStringLiteral("Load Neuroglancer Meshes");
  }
  if (debugLabel == "ng_skeleton_load") {
    return QStringLiteral("Load Neuroglancer Skeleton");
  }
  if (debugLabel == "ng_skeleton_batch_load") {
    return QStringLiteral("Load Neuroglancer Skeletons");
  }
  if (debugLabel == "ng_annotations_open") {
    return QStringLiteral("Open Neuroglancer Annotations Dataset");
  }
  if (debugLabel == "ng_annotations_load_by_relationship") {
    return QStringLiteral("Load Neuroglancer Annotations");
  }
  if (debugLabel == "ng_mesh_segment_properties_ids") {
    return QStringLiteral("Load Neuroglancer Segment Properties IDs");
  }
  if (debugLabel == "ng_skeleton_segment_properties_ids") {
    return QStringLiteral("Load Neuroglancer Segment Properties IDs");
  }

  QString label = QString::fromUtf8(debugLabel.data(), static_cast<int>(debugLabel.size()));
  label.replace('_', ' ');
  if (label.startsWith(QStringLiteral("ng "))) {
    label.replace(0, 3, QStringLiteral("Neuroglancer "));
  }
  if (label.isEmpty()) {
    return QStringLiteral("Background Task");
  }
  label[0] = label[0].toUpper();
  return label;
}

template<class Result, class Finish>
struct CancellableDocOwnedSingleShotState
{
  CancellableDocOwnedSingleShotState(QPointer<QObject> guardPtrIn, Finish finishIn)
    : guardPtr(std::move(guardPtrIn))
    , finish(std::move(finishIn))
  {}

  QPointer<QObject> guardPtr;
  Finish finish;
  std::optional<Result> result;
};

template<class Result, class Finish>
void finishCancellableDocOwnedSingleShotTaskOnUi(
  std::shared_ptr<CancellableDocOwnedSingleShotState<Result, Finish>> state)
{
  if (state->guardPtr == nullptr || QCoreApplication::closingDown() || !state->result.has_value()) {
    return;
  }
  state->finish(std::move(*state->result));
  state->result.reset();
}

template<class Result, class Finish>
folly::coro::Task<ZBackgroundJobOutcome>
runCancellableDocOwnedSingleShotTask(ZBackgroundJobContext ctx,
                                     folly::coro::Task<Result> workTask,
                                     std::shared_ptr<CancellableDocOwnedSingleShotState<Result, Finish>> state)
{
  ZBackgroundJobOutcome out;
  Result result = co_await folly::coro::co_withCancellation(ctx.cancellationToken(), std::move(workTask));
  if (auto error = backgroundJobFailureMessageFromResult(result)) {
    out.state = ZBackgroundJobOutcome::State::Failed;
    out.message = *error;
  }
  state->result.emplace(std::move(result));
  out.uiCallback = [state = std::move(state)](ZDoc&, ZBackgroundTask&) mutable {
    finishCancellableDocOwnedSingleShotTaskOnUi(std::move(state));
  };
  co_return out;
}

template<class Result, class Finish>
void startDocOwnedCancellableSingleShotTask(ZDoc& doc,
                                            QObject* guardObject,
                                            std::string_view debugLabel,
                                            folly::coro::Task<Result> workTask,
                                            Finish&& finish)
{
  using FinishFn = std::decay_t<Finish>;
  auto state = std::make_shared<CancellableDocOwnedSingleShotState<Result, FinishFn>>(QPointer<QObject>(guardObject),
                                                                                      std::forward<Finish>(finish));

  ZBackgroundJobSpec spec;
  spec.title = cancellableSingleShotTaskTitle(debugLabel);
  spec.debugLabel = std::string(debugLabel);
  spec.work = [workTask = std::move(workTask), state = std::move(state)](ZBackgroundJobContext ctx) mutable {
    return runCancellableDocOwnedSingleShotTask(std::move(ctx), std::move(workTask), std::move(state));
  };
  (void)startBackgroundJob(doc, std::move(spec));
}

template<class Fn>
void postToViewThread(QPointer<ZImgView> viewPtr, Fn&& fn)
{
  if (!viewPtr || QCoreApplication::closingDown()) {
    return;
  }

  (void)QMetaObject::invokeMethod(
    viewPtr,
    [viewPtr, fn = std::forward<Fn>(fn)]() mutable {
      if (!viewPtr || QCoreApplication::closingDown()) {
        return;
      }
      try {
        fn(*viewPtr);
      }
      catch (const std::exception& e) {
        LOG(ERROR) << "Unhandled exception in posted ZImgView UI callback: " << e.what();
      }
      catch (...) {
        LOG(ERROR) << "Unhandled non-std exception in posted ZImgView UI callback";
      }
    },
    Qt::QueuedConnection);
}

[[nodiscard]] BatchLoadProgressFn makeBatchLoadProgressFn(ZBackgroundJobContext ctx)
{
  return [ctx = std::move(ctx)](size_t processed, size_t total, QString message) mutable {
    if (total > 0) {
      ctx.setProgress01(static_cast<double>(std::min(processed, total)) / static_cast<double>(total));
    }
    if (!message.isEmpty()) {
      ctx.setMessage(std::move(message));
    }
  };
}

folly::coro::Task<NeuroglancerAnnotationsSourceOpenResult>
openAnnotationsSourceTask(std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol, QString annRootUrl)
{
  NeuroglancerAnnotationsSourceOpenResult out;
  try {
    CHECK(vol);
    std::array<double, 3> baseResNm{vol->baseImgInfo().voxelSizeX,
                                    vol->baseImgInfo().voxelSizeY,
                                    vol->baseImgInfo().voxelSizeZ};
    out.source = co_await ZNeuroglancerPrecomputedAnnotationsSource::openAsync(QUrl(annRootUrl),
                                                                               baseResNm,
                                                                               vol->baseVoxelOffset(),
                                                                               vol->sharedRemoteContext());
    CHECK(out.source);
    for (const auto& rel : out.source->relationships()) {
      if (!rel.id.trimmed().isEmpty()) {
        out.relationshipIds << rel.id;
      }
    }
  }
  catch (const ZCancellationException&) {
    throw;
  }
  catch (const folly::OperationCancelled&) {
    throw;
  }
  catch (const std::exception& e) {
    out.error = QString("Failed to open neuroglancer annotations dataset:\n%1").arg(QString::fromUtf8(e.what()));
  }
  co_return out;
}

folly::coro::Task<NeuroglancerAnnotationsLoadResult>
loadAnnotationsForRelationshipTask(std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol,
                                   std::shared_ptr<const ZNeuroglancerPrecomputedAnnotationsSource> source,
                                   QString relationshipId,
                                   uint64_t segmentId,
                                   QString annRootUrl,
                                   json::value sourceJson)
{
  NeuroglancerAnnotationsLoadResult out;
  out.sourceJson = std::move(sourceJson);
  try {
    CHECK(vol);
    CHECK(source);
    const auto anns = co_await source->loadAnnotationsForRelatedObjectAsync(relationshipId, segmentId);
    if (anns.empty()) {
      out.error = QString("No annotations found for segment %1 (relationship '%2')").arg(segmentId).arg(relationshipId);
      co_return out;
    }

    if (source->annotationType() == ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Point ||
        source->annotationType() == ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Ellipsoid) {
      std::list<ZPunctum> pts;
      for (const auto& a : anns) {
        if (a.points.size() != 1) {
          continue;
        }
        const auto& p = a.points[0];
        if (source->annotationType() == ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Ellipsoid &&
            a.ellipsoidRadiiVoxel) {
          const glm::vec3 r = *a.ellipsoidRadiiVoxel;
          const double rx = static_cast<double>(r.x);
          const double ry = static_cast<double>(r.y);
          const double rz = static_cast<double>(r.z);
          const double rMax = std::max({rx, ry, rz});
          ZPunctum punctum(p.x, p.y, p.z, /*r=*/rMax);
          punctum.setRadii(rx, ry, rz);
          punctum.name = std::to_string(static_cast<unsigned long long>(a.id));
          punctum.setMaxIntensity(255.0);
          punctum.setMeanIntensity(255.0);
          applyAnnotationPropertiesToPunctum(*source, a, punctum);
          if (a.rgba8) {
            const auto& c = *a.rgba8;
            punctum.setColor(col4(c[0], c[1], c[2], c[3]));
          }
          pts.push_back(std::move(punctum));
        } else {
          ZPunctum punctum(p.x, p.y, p.z, /*r=*/2.0);
          punctum.name = std::to_string(static_cast<unsigned long long>(a.id));
          punctum.setMaxIntensity(255.0);
          punctum.setMeanIntensity(255.0);
          applyAnnotationPropertiesToPunctum(*source, a, punctum);
          if (a.rgba8) {
            const auto& c = *a.rgba8;
            punctum.setColor(col4(c[0], c[1], c[2], c[3]));
          }
          pts.push_back(std::move(punctum));
        }
      }
      if (pts.empty()) {
        out.error = QString("No POINT/ELLIPSOID annotations decoded for segment %1 (relationship '%2')")
                      .arg(segmentId)
                      .arg(relationshipId);
        co_return out;
      }
      auto p = std::make_shared<ZPuncta>();
      p->data = std::move(pts);
      out.puncta = std::move(p);
      out.displayName = QString("NG Annotations %1 (%2)").arg(segmentId).arg(relationshipId);
      out.tooltip =
        QString(
          "Neuroglancer precomputed annotations\nSegmentation: %1\nAnnotations: %2\nRelationship: %3\nObject: %4\nCount: %5")
          .arg(vol->rootUrl())
          .arg(annRootUrl)
          .arg(relationshipId)
          .arg(segmentId)
          .arg(out.puncta->data.size());
      co_return out;
    }

    if (source->annotationType() == ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Line ||
        source->annotationType() == ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Polyline) {
      std::vector<glm::vec3> vertices;
      std::vector<glm::uvec2> edges;
      size_t numSegments = 0;
      for (const auto& a : anns) {
        if (a.points.size() < 2) {
          continue;
        }
        const size_t base = vertices.size();
        vertices.insert(vertices.end(), a.points.begin(), a.points.end());
        for (size_t i = 0; i + 1 < a.points.size(); ++i) {
          edges.emplace_back(static_cast<uint32_t>(base + i), static_cast<uint32_t>(base + i + 1));
          ++numSegments;
        }
      }
      if (vertices.empty() || edges.empty()) {
        out.error = QString("No LINE/POLYLINE annotations decoded for segment %1 (relationship '%2')")
                      .arg(segmentId)
                      .arg(relationshipId);
        co_return out;
      }
      auto skel = std::make_shared<ZSkeleton>();
      skel->setVertices(std::move(vertices));
      skel->setEdges(std::move(edges));
      out.skeleton = std::move(skel);
      out.displayName = QString("NG Annotations %1 (%2)").arg(segmentId).arg(relationshipId);
      out.tooltip =
        QString(
          "Neuroglancer precomputed annotations (lines)\nSegmentation: %1\nAnnotations: %2\nRelationship: %3\nObject: %4\nSegments: %5")
          .arg(vol->rootUrl())
          .arg(annRootUrl)
          .arg(relationshipId)
          .arg(segmentId)
          .arg(numSegments);
      co_return out;
    }

    out.error =
      QString("Unsupported annotation_type for rendering: only POINT/ELLIPSOID or LINE/POLYLINE are supported.");
    co_return out;
  }
  catch (const ZCancellationException&) {
    throw;
  }
  catch (const folly::OperationCancelled&) {
    throw;
  }
  catch (const ZNotFoundException&) {
    out.error = QString("No annotations found for segment %1 (relationship '%2')").arg(segmentId).arg(relationshipId);
    co_return out;
  }
  catch (const std::exception& e) {
    out.error = QString("Failed to load neuroglancer annotations:\n%1").arg(QString::fromUtf8(e.what()));
    co_return out;
  }
}

folly::coro::Task<std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource>>
openMeshSourceTask(std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol, QString meshSourceDirUrl)
{
  CHECK(vol);
  meshSourceDirUrl = normalizedDirUrlForComparison(meshSourceDirUrl);
  CHECK(!meshSourceDirUrl.isEmpty());

  if (vol->hasMeshDirectory() && normalizedDirUrlForComparison(vol->meshDirUrl().toString()) == meshSourceDirUrl) {
    co_return co_await vol->loadMeshSourceAsync();
  }

  const ZImgInfo& info = vol->baseImgInfo();
  co_return co_await ZNeuroglancerPrecomputedMeshSource::openAsync(QUrl(meshSourceDirUrl),
                                                                   {info.voxelSizeX, info.voxelSizeY, info.voxelSizeZ},
                                                                   vol->baseVoxelOffset(),
                                                                   vol->sharedRemoteContext());
}

folly::coro::Task<NeuroglancerMeshLoadResult> loadMeshTask(std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol,
                                                           QString meshSourceDirUrl,
                                                           uint64_t segmentId,
                                                           json::value sourceJson)
{
  NeuroglancerMeshLoadResult out;
  out.sourceJson = std::move(sourceJson);
  try {
    std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source =
      co_await openMeshSourceTask(vol, meshSourceDirUrl);
    CHECK(source);
    out.mesh = co_await source->loadMeshAsync(segmentId, ZNeuroglancerPrecomputedMeshSource::LodPolicy::Coarsest);
    if (!out.mesh || out.mesh->empty()) {
      out.error = QString("Neuroglancer mesh load returned an empty mesh for segment %1").arg(segmentId);
      co_return out;
    }

    QString label;
    QString description;
    if (const auto props = vol->segmentPropertiesShared()) {
      if (const auto l = props->labelForId(segmentId)) {
        label = *l;
      }
      if (const auto d = props->descriptionForId(segmentId)) {
        description = *d;
      }
    }

    out.displayName =
      label.isEmpty() ? QString("NG Mesh %1").arg(segmentId) : QString("NG Mesh %1 (%2)").arg(segmentId).arg(label);

    out.tooltip =
      QString("Neuroglancer precomputed mesh\nSegmentation: %1\nSegment: %2").arg(vol->rootUrl()).arg(segmentId);
    if (!label.isEmpty()) {
      out.tooltip += QString("\nLabel: %1").arg(label);
    }
    if (!description.isEmpty()) {
      out.tooltip += QString("\nDescription: %1").arg(description);
    }
  }
  catch (const ZCancellationException&) {
    throw;
  }
  catch (const folly::OperationCancelled&) {
    throw;
  }
  catch (const ZNotFoundException&) {
    out.error = QString("No neuroglancer mesh found for segment %1").arg(segmentId);
  }
  catch (const ZException& e) {
    out.error = QString::fromUtf8(e.what());
  }
  catch (const std::exception& e) {
    out.error = QString::fromUtf8(e.what());
  }
  co_return out;
}

folly::coro::Task<BatchLoadMessageResult>
loadMeshBatchTask(std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol,
                  QPointer<ZMeshDoc> meshDocPtr,
                  std::vector<uint64_t> ids,
                  std::vector<ZNeuroglancerMeshExternalSourceKey> existingKeys,
                  QString meshSourceDirUrl,
                  BatchLoadProgressFn progressFn)
{
  BatchLoadMessageResult out;
  size_t loaded = 0;
  size_t missing = 0;
  size_t skipped = 0;
  size_t errors = 0;
  size_t processed = 0;
  const size_t totalIds = ids.size();
  const folly::CancellationToken cancellationToken = co_await folly::coro::co_current_cancellation_token;

  if (QCoreApplication::closingDown() || !meshDocPtr) {
    co_return out;
  }

  auto maybeCancelTask = [&]() {
    if (cancellationToken.isCancellationRequested()) {
      throw ZCancellationException();
    }
  };

  auto reportProgress = [&](size_t done, QString message) {
    if (progressFn) {
      progressFn(done, totalIds, std::move(message));
    }
  };

  auto advanceProgress = [&]() {
    ++processed;
    reportProgress(processed, QStringLiteral("loading Neuroglancer meshes… %1/%2").arg(processed).arg(totalIds));
  };

  auto enqueueToUi = [](auto fn) {
    if (QCoreApplication::closingDown()) {
      return;
    }
    if (auto* app = QCoreApplication::instance()) {
      QMetaObject::invokeMethod(
        app,
        [fn = std::move(fn)]() mutable {
          try {
            fn();
          }
          catch (const std::exception& e) {
            LOG(ERROR) << "Unhandled exception in queued Neuroglancer mesh batch UI callback: " << e.what();
          }
          catch (...) {
            LOG(ERROR) << "Unhandled non-std exception in queued Neuroglancer mesh batch UI callback";
          }
        },
        Qt::QueuedConnection);
    }
  };

  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source;
  reportProgress(0, QStringLiteral("opening Neuroglancer mesh source…"));
  try {
    source = co_await openMeshSourceTask(vol, meshSourceDirUrl);
  }
  catch (const ZCancellationException&) {
    throw;
  }
  catch (const folly::OperationCancelled&) {
    throw;
  }
  catch (const std::exception& e) {
    out.error = QString("Failed to load neuroglancer mesh source: %1").arg(QString::fromUtf8(e.what()));
    co_return out;
  }
  CHECK(source);
  reportProgress(0, QStringLiteral("loading Neuroglancer meshes… 0/%1").arg(totalIds));

  const std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> props = vol->segmentPropertiesShared();

  const QString rootUrl = vol->rootUrl();
  const QString meshSourceKey = meshSourceDirUrl;

  for (const uint64_t segmentId : ids) {
    maybeCancelTask();
    if (QCoreApplication::closingDown() || !meshDocPtr) {
      break;
    }

    const ZNeuroglancerMeshExternalSourceKey key{
      .rootUrl = rootUrl,
      .meshSourceDirUrl = meshSourceKey,
      .segmentId = segmentId,
      .baseResolutionNm = std::array<double, 3>{vol->baseImgInfo().voxelSizeX,
                                                vol->baseImgInfo().voxelSizeY,
                                                vol->baseImgInfo().voxelSizeZ},
      .baseVoxelOffset = vol->baseVoxelOffset(),
    };
    if (std::ranges::any_of(existingKeys, [&](const auto& existingKey) {
          return sameNeuroglancerMeshSourceCompat(existingKey, key);
        })) {
      ++skipped;
      advanceProgress();
      continue;
    }

    std::shared_ptr<ZMesh> coarse;
    try {
      coarse = co_await source->loadMeshAsync(segmentId, ZNeuroglancerPrecomputedMeshSource::LodPolicy::Coarsest);
    }
    catch (const ZCancellationException&) {
      throw;
    }
    catch (const folly::OperationCancelled&) {
      throw;
    }
    catch (const ZNotFoundException&) {
      ++missing;
      advanceProgress();
      continue;
    }
    catch (const std::exception& e) {
      ++errors;
      VLOG(1) << fmt::format("Failed to load neuroglancer mesh for segment {}: {}", segmentId, e.what());
      advanceProgress();
      continue;
    }

    if (!coarse || coarse->empty()) {
      ++errors;
      advanceProgress();
      continue;
    }

    QString label;
    QString description;
    if (props) {
      if (const auto l = props->labelForId(segmentId)) {
        label = *l;
      }
      if (const auto d = props->descriptionForId(segmentId)) {
        description = *d;
      }
    }

    const QString displayName =
      label.isEmpty() ? QString("NG Mesh %1").arg(segmentId) : QString("NG Mesh %1 (%2)").arg(segmentId).arg(label);

    QString tooltip =
      QString("Neuroglancer precomputed mesh\nSegmentation: %1\nSegment: %2").arg(rootUrl).arg(segmentId);
    if (!label.isEmpty()) {
      tooltip += QString("\nLabel: %1").arg(label);
    }
    if (!description.isEmpty()) {
      tooltip += QString("\nDescription: %1").arg(description);
    }

    const json::value sourceJson =
      makeNeuroglancerMeshExternalSourceJson(rootUrl,
                                             meshSourceDirUrl,
                                             segmentId,
                                             std::array<double, 3>{vol->baseImgInfo().voxelSizeX,
                                                                   vol->baseImgInfo().voxelSizeY,
                                                                   vol->baseImgInfo().voxelSizeZ},
                                             vol->baseVoxelOffset());

    maybeCancelTask();
    const auto remoteContext = vol->sharedRemoteContext();
    enqueueToUi([meshDocPtr, coarse, displayName, tooltip, sourceJson, remoteContext]() mutable {
      if (QCoreApplication::closingDown() || !meshDocPtr) {
        return;
      }
      if (meshDocPtr->findMeshByExternalSource(sourceJson)) {
        return;
      }
      (void)meshDocPtr->addMeshFromExternalSource(*coarse, displayName, tooltip, sourceJson, remoteContext);
    });

    existingKeys.push_back(key);
    ++loaded;
    advanceProgress();
  }

  if (loaded == 0 && missing == 0 && skipped == 0 && errors == 0) {
    co_return out;
  }
  out.message =
    QString(
      "Neuroglancer mesh batch load finished.\n\nLoaded: %1\nMissing mesh: %2\nSkipped (already loaded): %3\nErrors: %4")
      .arg(loaded)
      .arg(missing)
      .arg(skipped)
      .arg(errors);
  co_return out;
}

folly::coro::Task<std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource>>
openSkeletonSourceTask(std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol, QString skeletonSourceDirUrl)
{
  CHECK(vol);
  skeletonSourceDirUrl = normalizedDirUrlForComparison(skeletonSourceDirUrl);
  CHECK(!skeletonSourceDirUrl.isEmpty());

  if (vol->hasSkeletonDirectory() &&
      normalizedDirUrlForComparison(vol->skeletonDirUrl().toString()) == skeletonSourceDirUrl) {
    co_return co_await vol->loadSkeletonSourceAsync();
  }

  const ZImgInfo& info = vol->baseImgInfo();
  co_return co_await ZNeuroglancerPrecomputedSkeletonSource::openAsync(
    QUrl(skeletonSourceDirUrl),
    {info.voxelSizeX, info.voxelSizeY, info.voxelSizeZ},
    vol->baseVoxelOffset(),
    vol->sharedRemoteContext());
}

folly::coro::Task<NeuroglancerSkeletonLoadResult> loadSkeletonTask(std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol,
                                                                   QString skeletonSourceDirUrl,
                                                                   uint64_t segmentId,
                                                                   json::value sourceJson)
{
  NeuroglancerSkeletonLoadResult out;
  out.sourceJson = std::move(sourceJson);
  try {
    std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource> source =
      co_await openSkeletonSourceTask(vol, skeletonSourceDirUrl);
    CHECK(source);
    out.skeleton = co_await source->loadSkeletonAsync(segmentId);
    if (!out.skeleton || out.skeleton->empty()) {
      out.error = QString("Neuroglancer skeleton load returned an empty skeleton for segment %1").arg(segmentId);
      co_return out;
    }

    QString label;
    QString description;
    if (const auto props = vol->segmentPropertiesShared()) {
      if (const auto l = props->labelForId(segmentId)) {
        label = *l;
      }
      if (const auto d = props->descriptionForId(segmentId)) {
        description = *d;
      }
    }

    out.displayName = label.isEmpty() ? QString("NG Skeleton %1").arg(segmentId)
                                      : QString("NG Skeleton %1 (%2)").arg(segmentId).arg(label);

    out.tooltip =
      QString("Neuroglancer precomputed skeleton\nSegmentation: %1\nSegment: %2").arg(vol->rootUrl()).arg(segmentId);
    if (!label.isEmpty()) {
      out.tooltip += QString("\nLabel: %1").arg(label);
    }
    if (!description.isEmpty()) {
      out.tooltip += QString("\nDescription: %1").arg(description);
    }
  }
  catch (const ZCancellationException&) {
    throw;
  }
  catch (const folly::OperationCancelled&) {
    throw;
  }
  catch (const ZNotFoundException&) {
    out.error = QString("No neuroglancer skeleton found for segment %1").arg(segmentId);
  }
  catch (const ZException& e) {
    out.error = QString::fromUtf8(e.what());
  }
  catch (const std::exception& e) {
    out.error = QString::fromUtf8(e.what());
  }
  co_return out;
}

folly::coro::Task<BatchLoadMessageResult> loadSkeletonBatchTask(std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol,
                                                                QPointer<ZSkeletonDoc> skeletonDocPtr,
                                                                std::vector<uint64_t> ids,
                                                                std::set<QString> existingKeys,
                                                                QString skeletonSourceDirUrl,
                                                                BatchLoadProgressFn progressFn)
{
  BatchLoadMessageResult out;
  size_t loaded = 0;
  size_t missing = 0;
  size_t skipped = 0;
  size_t errors = 0;
  size_t processed = 0;
  const size_t totalIds = ids.size();
  const folly::CancellationToken cancellationToken = co_await folly::coro::co_current_cancellation_token;

  if (QCoreApplication::closingDown() || !skeletonDocPtr) {
    co_return out;
  }

  auto maybeCancelTask = [&]() {
    if (cancellationToken.isCancellationRequested()) {
      throw ZCancellationException();
    }
  };

  auto reportProgress = [&](size_t done, QString message) {
    if (progressFn) {
      progressFn(done, totalIds, std::move(message));
    }
  };

  auto advanceProgress = [&]() {
    ++processed;
    reportProgress(processed, QStringLiteral("loading Neuroglancer skeletons… %1/%2").arg(processed).arg(totalIds));
  };

  auto enqueueToUi = [](auto fn) {
    if (QCoreApplication::closingDown()) {
      return;
    }
    if (auto* app = QCoreApplication::instance()) {
      QMetaObject::invokeMethod(
        app,
        [fn = std::move(fn)]() mutable {
          try {
            fn();
          }
          catch (const std::exception& e) {
            LOG(ERROR) << "Unhandled exception in queued Neuroglancer skeleton batch UI callback: " << e.what();
          }
          catch (...) {
            LOG(ERROR) << "Unhandled non-std exception in queued Neuroglancer skeleton batch UI callback";
          }
        },
        Qt::QueuedConnection);
    }
  };

  std::shared_ptr<const ZNeuroglancerPrecomputedSkeletonSource> source;
  reportProgress(0, QStringLiteral("opening Neuroglancer skeleton source…"));
  try {
    source = co_await openSkeletonSourceTask(vol, skeletonSourceDirUrl);
  }
  catch (const ZCancellationException&) {
    throw;
  }
  catch (const folly::OperationCancelled&) {
    throw;
  }
  catch (const std::exception& e) {
    out.error = QString("Failed to load neuroglancer skeleton source: %1").arg(QString::fromUtf8(e.what()));
    co_return out;
  }
  CHECK(source);
  reportProgress(0, QStringLiteral("loading Neuroglancer skeletons… 0/%1").arg(totalIds));

  const std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> props = vol->segmentPropertiesShared();

  const QString rootUrl = vol->rootUrl();
  const QString skeletonSourceKey = skeletonSourceDirUrl;

  for (const uint64_t segmentId : ids) {
    maybeCancelTask();
    if (QCoreApplication::closingDown() || !skeletonDocPtr) {
      break;
    }

    const QString keyStr = neuroglancerSkeletonKeyString(rootUrl, skeletonSourceKey, segmentId);
    if (existingKeys.contains(keyStr)) {
      ++skipped;
      advanceProgress();
      continue;
    }

    std::shared_ptr<ZSkeleton> skel;
    try {
      skel = co_await source->loadSkeletonAsync(segmentId);
    }
    catch (const ZCancellationException&) {
      throw;
    }
    catch (const folly::OperationCancelled&) {
      throw;
    }
    catch (const ZNotFoundException&) {
      ++missing;
      advanceProgress();
      continue;
    }
    catch (const std::exception& e) {
      ++errors;
      VLOG(1) << fmt::format("Failed to load neuroglancer skeleton for segment {}: {}", segmentId, e.what());
      advanceProgress();
      continue;
    }

    if (!skel || skel->empty()) {
      ++errors;
      advanceProgress();
      continue;
    }

    QString label;
    QString description;
    if (props) {
      if (const auto l = props->labelForId(segmentId)) {
        label = *l;
      }
      if (const auto d = props->descriptionForId(segmentId)) {
        description = *d;
      }
    }

    const QString displayName = label.isEmpty() ? QString("NG Skeleton %1").arg(segmentId)
                                                : QString("NG Skeleton %1 (%2)").arg(segmentId).arg(label);

    QString tooltip =
      QString("Neuroglancer precomputed skeleton\nSegmentation: %1\nSegment: %2").arg(rootUrl).arg(segmentId);
    if (!label.isEmpty()) {
      tooltip += QString("\nLabel: %1").arg(label);
    }
    if (!description.isEmpty()) {
      tooltip += QString("\nDescription: %1").arg(description);
    }

    json::object sourceObj;
    sourceObj["type"] = "neuroglancer_precomputed_skeleton";
    sourceObj["segmentation_root_url"] = json::value_from(rootUrl);
    sourceObj["segment_id"] = json::value_from(QString::number(segmentId));
    sourceObj["skeleton_source_url"] = json::value_from(skeletonSourceDirUrl);
    const json::value sourceJson = sourceObj;

    maybeCancelTask();
    enqueueToUi([skeletonDocPtr, skel, displayName, tooltip, sourceJson]() mutable {
      if (QCoreApplication::closingDown() || !skeletonDocPtr) {
        return;
      }
      if (skeletonDocPtr->findSkeletonByExternalSource(sourceJson)) {
        return;
      }
      (void)skeletonDocPtr->addSkeletonFromExternalSource(*skel, displayName, tooltip, sourceJson);
    });

    existingKeys.insert(keyStr);
    ++loaded;
    advanceProgress();
  }

  if (loaded == 0 && missing == 0 && skipped == 0 && errors == 0) {
    co_return out;
  }
  out.message =
    QString(
      "Neuroglancer skeleton batch load finished.\n\nLoaded: %1\nMissing skeleton: %2\nSkipped (already loaded): %3\nErrors: %4")
      .arg(loaded)
      .arg(missing)
      .arg(skipped)
      .arg(errors);
  co_return out;
}

folly::coro::Task<CollectVisibleSegIdsResult>
collectVisibleSegIdsTask(const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& topVol,
                         int z,
                         const QRectF& viewport,
                         double scale)
{
  CollectVisibleSegIdsResult out;
  try {
    const auto pack =
      co_await topVol->sliceTilePackFor2DViewportCacheBestEffortAsync(static_cast<size_t>(z), /*t=*/0, viewport, scale);

    if (pack.imgs.empty()) {
      co_return out;
    }
    CHECK(pack.ratios.size() == pack.imgs.size());

    std::unordered_set<uint64_t> ids;
    for (size_t i = 0; i < pack.imgs.size(); ++i) {
      if (pack.ratios[i] != pack.targetRatio) {
        continue;
      }

      const auto& img = pack.imgs[i];
      if (!img || img->isEmpty()) {
        continue;
      }
      const ZImgInfo& info = img->info();
      if (info.numChannels != 1 || info.depth != 1) {
        out.error = QString("Unexpected neuroglancer segmentation slice type: %1 channels, depth %2")
                      .arg(info.numChannels)
                      .arg(info.depth);
        co_return out;
      }
      if (info.voxelFormat != VoxelFormat::Unsigned) {
        out.error = QString("Unsupported neuroglancer segmentation voxel format: expected unsigned, got '%1'")
                      .arg(info.voxelFormat == VoxelFormat::Signed ? "signed" : "float");
        co_return out;
      }

      const size_t n = info.width * info.height;
      switch (info.bytesPerVoxel) {
        case 1: {
          const auto* p = img->timeData<uint8_t>(0);
          for (size_t pix = 0; pix < n; ++pix) {
            ids.insert(static_cast<uint64_t>(p[pix]));
          }
          break;
        }
        case 2: {
          const auto* p = img->timeData<uint16_t>(0);
          for (size_t pix = 0; pix < n; ++pix) {
            ids.insert(static_cast<uint64_t>(p[pix]));
          }
          break;
        }
        case 4: {
          const auto* p = img->timeData<uint32_t>(0);
          for (size_t pix = 0; pix < n; ++pix) {
            ids.insert(static_cast<uint64_t>(p[pix]));
          }
          break;
        }
        case 8: {
          const auto* p = img->timeData<uint64_t>(0);
          for (size_t pix = 0; pix < n; ++pix) {
            ids.insert(p[pix]);
          }
          break;
        }
        default:
          out.error =
            QString("Unsupported neuroglancer segmentation data type: %1 bytes/voxel").arg(info.bytesPerVoxel);
          co_return out;
      }
    }

    out.ids.assign(ids.begin(), ids.end());
    std::sort(out.ids.begin(), out.ids.end());
  }
  catch (const ZException& e) {
    out.error = QString::fromUtf8(e.what());
  }
  catch (const std::exception& e) {
    out.error = QString::fromUtf8(e.what());
  }

  co_return out;
}

folly::coro::Task<CollectVisibleSegIdsResult>
collectAllSegmentPropertiesIdsTask(const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol)
{
  CollectVisibleSegIdsResult out;
  try {
    const auto props = co_await vol->loadSegmentPropertiesAsync();
    CHECK(props);
    out.ids = props->ids();
  }
  catch (const ZCancellationException&) {
    throw;
  }
  catch (const folly::OperationCancelled&) {
    throw;
  }
  catch (const ZException& e) {
    out.error = QString::fromUtf8(e.what());
  }
  catch (const std::exception& e) {
    out.error = QString::fromUtf8(e.what());
  }
  co_return out;
}

} // namespace

ZImgView::ZImgView(ZImgDoc& doc, ZView& view)
  : ZFilterView<ZImgDoc, ZImgFilter>(doc, view)
{
  docImgsAdded(m_doc.objs());
  connect(&m_doc, &ZImgDoc::objAdded, this, &ZImgView::docImgAdded);
  connect(&m_doc, &ZImgDoc::imgChanged, this, &ZImgView::docImgChanged);
}

ZImgView::~ZImgView()
{
  cancelNeuroglancerAnnotationsSpatialLoad(/*markCancelledTooltip=*/false);
}

bool ZImgView::hasActiveNeuroglancerAnnotationsSpatialRequest(uint64_t generation) const
{
  return m_ngAnnotationsSpatialRequest.has_value() && m_ngAnnotationsSpatialRequest->generation == generation;
}

void ZImgView::cancelNeuroglancerAnnotationsSpatialLoad(bool markCancelledTooltip)
{
  if (!m_ngAnnotationsSpatialRequest) {
    return;
  }

  if (m_ngAnnotationsSpatialRequest->task) {
    m_ngAnnotationsSpatialRequest->task->requestCancel();
  }

  if (!markCancelledTooltip) {
    return;
  }
  if (m_ngAnnotationsSpatialRequest->completed) {
    return;
  }

  updateNeuroglancerAnnotationsSpatialCancelledUi(QStringLiteral("cancelled (superseded by a newer request)"));
}

void ZImgView::updateNeuroglancerAnnotationsSpatialCancelledUi(QString status)
{
  if (!m_ngAnnotationsSpatialRequest) {
    return;
  }

  auto& request = *m_ngAnnotationsSpatialRequest;

  if (request.punctaObjId && m_doc.doc().punctaDoc().hasObjWithID(*request.punctaObjId)) {
    const size_t id = *request.punctaObjId;
    const uint64_t count = static_cast<uint64_t>(m_doc.doc().punctaDoc().punctaPack(id).puncta().data.size());
    m_doc.doc().punctaDoc().updateExternalPunctaMetadata(
      id,
      request.displayName,
      makeSpatialPunctaTooltip(request.segRootUrl, request.annRootUrl, request.qMin, request.qMax, status, count));
  }

  if (request.skeletonObjId && m_doc.doc().skeletonDoc().hasObjWithID(*request.skeletonObjId)) {
    const size_t id = *request.skeletonObjId;
    const uint64_t segCount = static_cast<uint64_t>(m_doc.doc().skeletonDoc().skeleton(id).numEdges());
    m_doc.doc().skeletonDoc().updateExternalSkeletonMetadata(
      id,
      request.displayName,
      makeSpatialSkeletonTooltip(request.segRootUrl, request.annRootUrl, request.qMin, request.qMax, status, segCount));
  }

  request.completed = true;
}

void ZImgView::handleNeuroglancerAnnotationsSpatialFailureOnUi(uint64_t generation, const QString& error)
{
  if (!hasActiveNeuroglancerAnnotationsSpatialRequest(generation)) {
    return;
  }

  auto& request = *m_ngAnnotationsSpatialRequest;

  if (request.punctaObjId) {
    auto& doc = m_doc.doc().punctaDoc();
    if (doc.hasObjWithID(*request.punctaObjId)) {
      const size_t id = *request.punctaObjId;
      if (doc.punctaPack(id).puncta().data.empty()) {
        m_doc.doc().removeObj(id);
        request.punctaObjId.reset();
      } else {
        const uint64_t count = static_cast<uint64_t>(doc.punctaPack(id).puncta().data.size());
        doc.updateExternalPunctaMetadata(id,
                                         request.displayName,
                                         makeSpatialPunctaTooltip(request.segRootUrl,
                                                                  request.annRootUrl,
                                                                  request.qMin,
                                                                  request.qMax,
                                                                  QStringLiteral("failed"),
                                                                  count));
      }
    }
  }

  if (request.skeletonObjId) {
    auto& doc = m_doc.doc().skeletonDoc();
    if (doc.hasObjWithID(*request.skeletonObjId)) {
      const size_t id = *request.skeletonObjId;
      if (doc.skeleton(id).empty()) {
        m_doc.doc().removeObj(id);
        request.skeletonObjId.reset();
      } else {
        const uint64_t segCount = static_cast<uint64_t>(doc.skeleton(id).numEdges());
        doc.updateExternalSkeletonMetadata(id,
                                           request.displayName,
                                           makeSpatialSkeletonTooltip(request.segRootUrl,
                                                                      request.annRootUrl,
                                                                      request.qMin,
                                                                      request.qMax,
                                                                      QStringLiteral("failed"),
                                                                      segCount));
      }
    }
  }

  request.completed = true;

  QMessageBox::information(QApplication::activeWindow(),
                           QApplication::applicationName(),
                           QString("Failed to load neuroglancer annotations (spatial):\n%1").arg(error));
}

void ZImgView::initializeNeuroglancerAnnotationsSpatialRequestOnUi(uint64_t generation,
                                                                   bool renderAsPuncta,
                                                                   json::value sourceJson)
{
  if (QCoreApplication::closingDown()) {
    return;
  }
  if (!hasActiveNeuroglancerAnnotationsSpatialRequest(generation)) {
    return;
  }

  auto& request = *m_ngAnnotationsSpatialRequest;
  if (request.task && request.task->cancelRequested()) {
    return;
  }

  if (renderAsPuncta) {
    ZPuncta empty;
    const auto id =
      m_doc.doc().punctaDoc().addPunctaFromExternalSource(std::move(empty),
                                                          request.displayName,
                                                          makeSpatialPunctaTooltip(request.segRootUrl,
                                                                                   request.annRootUrl,
                                                                                   request.qMin,
                                                                                   request.qMax,
                                                                                   QStringLiteral("loading…"),
                                                                                   0),
                                                          std::move(sourceJson));
    request.punctaObjId = id;
    request.skeletonObjId.reset();
    request.completed = false;
    return;
  }

  ZSkeleton empty;
  const auto id =
    m_doc.doc().skeletonDoc().addSkeletonFromExternalSource(empty,
                                                            request.displayName,
                                                            makeSpatialSkeletonTooltip(request.segRootUrl,
                                                                                       request.annRootUrl,
                                                                                       request.qMin,
                                                                                       request.qMax,
                                                                                       QStringLiteral("loading…"),
                                                                                       0),
                                                            std::move(sourceJson));
  request.skeletonObjId = id;
  request.punctaObjId.reset();
  request.completed = false;
  return;
}

void ZImgView::applyNeuroglancerAnnotationsSpatialPunctaUpdateOnUi(
  uint64_t generation,
  ZNeuroglancerPrecomputedAnnotationsSource::SpatialLoadProgress progress,
  std::shared_ptr<ZPuncta> batch)
{
  if (QCoreApplication::closingDown()) {
    return;
  }
  if (!hasActiveNeuroglancerAnnotationsSpatialRequest(generation)) {
    return;
  }

  auto& request = *m_ngAnnotationsSpatialRequest;
  if (request.task && request.task->cancelRequested()) {
    return;
  }
  if (!request.punctaObjId) {
    return;
  }

  const size_t id = *request.punctaObjId;
  auto& doc = m_doc.doc().punctaDoc();
  if (!doc.hasObjWithID(id)) {
    return;
  }

  if (batch && !batch->data.empty()) {
    doc.appendExternalPunctaNoUndo(id, std::move(batch->data));
  }

  const uint64_t count = static_cast<uint64_t>(doc.punctaPack(id).puncta().data.size());
  request.completed = isSpatialLoadDone(progress);
  doc.updateExternalPunctaMetadata(id,
                                   request.displayName,
                                   makeSpatialPunctaTooltip(request.segRootUrl,
                                                            request.annRootUrl,
                                                            request.qMin,
                                                            request.qMax,
                                                            spatialLoadStatusString(progress),
                                                            count));

  if (request.completed && count == 0) {
    m_doc.doc().removeObj(id);
    request.punctaObjId.reset();
    QMessageBox::information(QApplication::activeWindow(),
                             QApplication::applicationName(),
                             QStringLiteral("No annotations found intersecting the current view region."));
  }
  return;
}

void ZImgView::applyNeuroglancerAnnotationsSpatialSkeletonUpdateOnUi(
  uint64_t generation,
  ZNeuroglancerPrecomputedAnnotationsSource::SpatialLoadProgress progress,
  std::shared_ptr<std::pair<std::vector<glm::vec3>, std::vector<glm::uvec2>>> geometry)
{
  if (QCoreApplication::closingDown()) {
    return;
  }
  if (!hasActiveNeuroglancerAnnotationsSpatialRequest(generation)) {
    return;
  }

  auto& request = *m_ngAnnotationsSpatialRequest;
  if (request.task && request.task->cancelRequested()) {
    return;
  }
  if (!request.skeletonObjId) {
    return;
  }

  const size_t id = *request.skeletonObjId;
  auto& doc = m_doc.doc().skeletonDoc();
  if (!doc.hasObjWithID(id)) {
    return;
  }

  if (geometry && !geometry->first.empty() && !geometry->second.empty()) {
    doc.appendExternalSkeletonGeometryNoUndo(id, std::move(geometry->first), std::move(geometry->second));
  }

  const uint64_t segCount = static_cast<uint64_t>(doc.skeleton(id).numEdges());
  request.completed = isSpatialLoadDone(progress);
  doc.updateExternalSkeletonMetadata(id,
                                     request.displayName,
                                     makeSpatialSkeletonTooltip(request.segRootUrl,
                                                                request.annRootUrl,
                                                                request.qMin,
                                                                request.qMax,
                                                                spatialLoadStatusString(progress),
                                                                segCount));

  if (request.completed && segCount == 0) {
    m_doc.doc().removeObj(id);
    request.skeletonObjId.reset();
    QMessageBox::information(QApplication::activeWindow(),
                             QApplication::applicationName(),
                             QStringLiteral("No annotations found intersecting the current view region."));
  }
  return;
}

folly::coro::Task<ZBackgroundJobOutcome>
ZImgView::runNeuroglancerAnnotationsSpatialLoadTask(ZBackgroundJobContext ctx,
                                                    QPointer<ZImgView> viewPtr,
                                                    std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol,
                                                    QString annRootUrl,
                                                    glm::dvec3 qMin,
                                                    glm::dvec3 qMax,
                                                    json::value sourceJson,
                                                    uint64_t generation)
{
  CHECK(vol);

  auto makeCancelledOutcome = [viewPtr, generation](QString status = QStringLiteral("cancelled")) {
    ZBackgroundJobOutcome out;
    out.state = ZBackgroundJobOutcome::State::Cancelled;
    out.message = status;
    out.uiCallback = [viewPtr, generation, status = std::move(status)](ZDoc&, ZBackgroundTask&) {
      if (!viewPtr || QCoreApplication::closingDown() ||
          !viewPtr->hasActiveNeuroglancerAnnotationsSpatialRequest(generation)) {
        return;
      }
      viewPtr->updateNeuroglancerAnnotationsSpatialCancelledUi(status);
    };
    return out;
  };

  auto makeFailureOutcome = [viewPtr, generation](QString message) {
    ZBackgroundJobOutcome out;
    out.state = ZBackgroundJobOutcome::State::Failed;
    out.message = message;
    out.uiCallback = [viewPtr, generation, message = std::move(message)](ZDoc&, ZBackgroundTask&) {
      if (!viewPtr || QCoreApplication::closingDown() ||
          !viewPtr->hasActiveNeuroglancerAnnotationsSpatialRequest(generation)) {
        return;
      }
      viewPtr->handleNeuroglancerAnnotationsSpatialFailureOnUi(generation, message);
    };
    return out;
  };

  try {
    ctx.setMessage(QStringLiteral("opening annotations dataset"));

    const ZImgInfo& info = vol->baseImgInfo();
    const auto source = co_await folly::coro::co_withCancellation(
      ctx.cancellationToken(),
      ZNeuroglancerPrecomputedAnnotationsSource::openAsync(QUrl(annRootUrl),
                                                           {info.voxelSizeX, info.voxelSizeY, info.voxelSizeZ},
                                                           vol->baseVoxelOffset(),
                                                           vol->sharedRemoteContext()));
    CHECK(source);

    if (source->spatialLevels().empty()) {
      co_return makeFailureOutcome(
        QStringLiteral("This neuroglancer annotations dataset has no spatial index ('spatial' missing in info).\n\n"
                       "Use the segment-based annotations load actions instead (relationship index)."));
    }

    const auto type = source->annotationType();
    if (!rendersSpatialAnnotationsAsPuncta(type) && !rendersSpatialAnnotationsAsSkeleton(type)) {
      co_return makeFailureOutcome(QStringLiteral(
        "Unsupported annotation_type for rendering: only POINT/ELLIPSOID or LINE/POLYLINE are supported."));
    }

    const bool renderAsPuncta = rendersSpatialAnnotationsAsPuncta(type);
    ctx.maybeCancel();
    postToViewThread(viewPtr, [generation, renderAsPuncta, sourceJson = std::move(sourceJson)](ZImgView& view) mutable {
      view.initializeNeuroglancerAnnotationsSpatialRequestOnUi(generation, renderAsPuncta, std::move(sourceJson));
    });

    ctx.setMessage(QStringLiteral("loading…"));
    auto updates = source->streamAnnotationsIntersectingVoxelBoxAsync(
      qMin,
      qMax,
      ZNeuroglancerPrecomputedAnnotationsSource::SpatialStreamOptions{});
    while (auto update = co_await folly::coro::co_withCancellation(ctx.cancellationToken(), updates.next())) {
      ctx.setMessage(spatialLoadStatusString(update->progress));
      if (update->progress.totalCells > 0) {
        const double progress01 =
          static_cast<double>(std::min(update->progress.visitedCells, update->progress.totalCells)) /
          static_cast<double>(update->progress.totalCells);
        ctx.setProgress01(progress01);
      }

      if (renderAsPuncta) {
        auto batch = buildSpatialPunctaBatch(*source, update->newAnnotations);
        postToViewThread(
          viewPtr,
          [generation, progress = update->progress, batch = std::move(batch)](ZImgView& view) mutable {
            view.applyNeuroglancerAnnotationsSpatialPunctaUpdateOnUi(generation, progress, std::move(batch));
          });
        ctx.maybeCancel();
        continue;
      }

      auto geometry = buildSpatialSkeletonBatch(update->newAnnotations);
      postToViewThread(
        viewPtr,
        [generation, progress = update->progress, geometry = std::move(geometry)](ZImgView& view) mutable {
          view.applyNeuroglancerAnnotationsSpatialSkeletonUpdateOnUi(generation, progress, std::move(geometry));
        });
      ctx.maybeCancel();
    }

    ctx.maybeCancel();
  }
  catch (const ZCancellationException&) {
    co_return makeCancelledOutcome();
  }
  catch (const folly::OperationCancelled&) {
    co_return makeCancelledOutcome();
  }
  catch (const std::exception& e) {
    co_return makeFailureOutcome(QString::fromUtf8(e.what()));
  }

  ZBackgroundJobOutcome out;
  out.message = QStringLiteral("done");
  co_return out;
}

void ZImgView::appendContextMenuActions(QMenu& menu,
                                        size_t /*activeObjId*/,
                                        const QPointF& scenePos,
                                        Qt::KeyboardModifiers /*modifiers*/)
{
  if (m_view.isMaxZProjView()) {
    return;
  }

  bool hasTraceActions = false;
  {
    struct TraceCandidate
    {
      size_t imgObjId = 0;
      ZImgFilter* filter = nullptr;
      std::shared_ptr<ZImgPack> imgPack;
      std::array<double, 3> seed{};
      size_t t = 0;
    };

    std::vector<TraceCandidate> traceCandidates;
    traceCandidates.reserve(m_idToFilter.size());
    for (const auto& idFilter : m_idToFilter) {
      const size_t imgObjId = idFilter.first;
      ZImgFilter* filter = idFilter.second.get();
      if (!filter || !filter->isVisible()) {
        continue;
      }

      std::shared_ptr<ZImgPack> imgPack = m_doc.imgPackShared(imgObjId);
      if (!imgPack) {
        continue;
      }

      // Skip segmentation datasets (not traceable).
      if (imgPack->isNeuroglancerPrecomputed()) {
        const std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol = imgPack->neuroglancerVolumeShared();
        if (vol && vol->isSegmentation()) {
          continue;
        }
      }

      traceCandidates.push_back(
        {.imgObjId = imgObjId, .filter = filter, .imgPack = std::move(imgPack), .seed = {}, .t = 0});
    }

    const bool inTraceableView = m_view.viewStylePara().isSelected(QStringLiteral("Normal"));

    std::vector<TraceCandidate> cursorCandidates;
    cursorCandidates.reserve(traceCandidates.size());
    if (inTraceableView) {
      for (auto& c : traceCandidates) {
        CHECK(c.filter);
        CHECK(c.imgPack);

        const QPointF p = c.filter->mapFromScene(scenePos);
        const int64_t lx = static_cast<int64_t>(std::floor(p.x()));
        const int64_t ly = static_cast<int64_t>(std::floor(p.y()));
        const int64_t lz = static_cast<int64_t>(c.filter->imgSlice());
        const int64_t lt = static_cast<int64_t>(c.filter->imgTime());

        const ZImgInfo info = c.imgPack->imgInfo();
        const bool inBounds = (lx >= 0 && ly >= 0 && lz >= 0 && lt >= 0) && (static_cast<size_t>(lx) < info.width) &&
                              (static_cast<size_t>(ly) < info.height) && (static_cast<size_t>(lz) < info.depth) &&
                              (static_cast<size_t>(lt) < info.numTimes);
        if (!inBounds) {
          continue;
        }

        c.seed = std::array<double, 3>{p.x(), p.y(), static_cast<double>(lz)};
        c.t = static_cast<size_t>(lt);
        cursorCandidates.push_back(c);
      }
    }

    if (!traceCandidates.empty()) {
      auto* traceMenu = menu.addMenu("Trace");
      hasTraceActions = true;

      if (!inTraceableView) {
        auto* act = traceMenu->addAction("Trace is available only in Normal view.");
        act->setEnabled(false);
      } else {
        if (cursorCandidates.empty()) {
          auto* act = traceMenu->addAction("Click within an image to trace.");
          act->setEnabled(false);
          return;
        }

        const QString traceCfgPath = QDir(ZSystemInfo::jsonDirPath()).absoluteFilePath("trace_config.json");

        auto startTrace = [this](const QString& actionName,
                                 size_t sourceImgObjId,
                                 const std::shared_ptr<ZImgPack>& imgPack,
                                 size_t sc,
                                 size_t t,
                                 std::array<double, 3> seed,
                                 const QString& traceConfigPath,
                                 std::optional<std::pair<size_t, ZSwc>> hostSwcOpt,
                                 bool promoteNewSwcToExistingTarget,
                                 std::function<void(size_t newSwcId)> onNewSwcCreated) {
          startSeedTraceInteractive(m_doc.doc(),
                                    actionName,
                                    sourceImgObjId,
                                    imgPack,
                                    sc,
                                    t,
                                    seed,
                                    traceConfigPath,
                                    std::move(hostSwcOpt),
                                    promoteNewSwcToExistingTarget,
                                    std::move(onNewSwcCreated));
        };

        QAction* traceStoredAct = traceMenu->addAction("Trace");
        traceStoredAct->setEnabled(false);

        {
          const ZTraceSettings& settings = m_doc.doc().traceSettings();
          const std::optional<size_t> storedImgId = settings.sourceImageId();
          if (settings.traceInProgress()) {
            traceStoredAct->setToolTip(QStringLiteral("A trace is already running. Please wait for it to finish."));
          } else if (!storedImgId.has_value()) {
            traceStoredAct->setToolTip(QStringLiteral("Select a source image and channel in Trace Settings first."));
          } else {
            const auto it =
              std::find_if(cursorCandidates.begin(), cursorCandidates.end(), [&](const TraceCandidate& c) {
                return c.imgObjId == *storedImgId;
              });
            if (it == cursorCandidates.end()) {
              traceStoredAct->setToolTip(QStringLiteral(
                "Trace Settings source image is not under the cursor. Move the cursor over that image, or change the source image in Trace Settings."));
            } else {
              const ZImgInfo info = it->imgPack->imgInfo();
              const size_t sc = settings.sourceChannel();
              if (sc >= info.numChannels) {
                traceStoredAct->setToolTip(QStringLiteral("Trace Settings channel is out of range for this image."));
              } else {
                std::optional<std::pair<size_t, ZSwc>> hostSwcOpt;
                QString actionName = QStringLiteral("Trace");

                if (settings.swcTargetMode() == ZTraceSettings::SwcTargetMode::ExistingSwc) {
                  const std::optional<size_t> storedSwcId = settings.targetSwcId();
                  if (!storedSwcId.has_value()) {
                    traceStoredAct->setToolTip(
                      QStringLiteral("Trace Settings expects an existing SWC, but none is selected."));
                  } else {
                    ZSwcDoc& swcDoc = m_doc.doc().swcDoc();
                    if (!swcDoc.hasObjWithID(*storedSwcId)) {
                      traceStoredAct->setToolTip(
                        QStringLiteral("The SWC selected in Trace Settings no longer exists."));
                    } else {
                      hostSwcOpt =
                        std::make_optional<std::pair<size_t, ZSwc>>(*storedSwcId, swcDoc.swcPack(*storedSwcId).swc());
                      actionName = QStringLiteral("Trace (attach)");
                    }
                  }
                }

                if (settings.swcTargetMode() == ZTraceSettings::SwcTargetMode::NewSwc || hostSwcOpt.has_value()) {
                  traceStoredAct->setEnabled(true);
                  traceStoredAct->setToolTip(QStringLiteral("Trace using Trace Settings."));
                  const std::shared_ptr<ZImgPack> imgPack = it->imgPack;
                  const size_t t = it->t;
                  const std::array<double, 3> seed = it->seed;
                  const bool promoteNewSwcToExistingTarget =
                    (settings.swcTargetMode() == ZTraceSettings::SwcTargetMode::NewSwc);

                  connect(traceStoredAct,
                          &QAction::triggered,
                          this,
                          [startTrace,
                           actionName,
                           sourceImgObjId = it->imgObjId,
                           imgPack,
                           sc,
                           t,
                           seed,
                           traceCfgPath,
                           promoteNewSwcToExistingTarget,
                           hostSwcOpt = std::move(hostSwcOpt),
                           viewPtr = QPointer<ZView>(&m_view),
                           swcDocTypeName = m_doc.doc().swcDoc().typeName()]() mutable {
                            std::function<void(size_t)> onNewSwcCreated =
                              [viewPtr, sourceImgObjId, swcDocTypeName](size_t newSwcId) {
                                if (!viewPtr) {
                                  return;
                                }

                                try {
                                  json::object srcViewJson;
                                  viewPtr->write(sourceImgObjId, srcViewJson);

                                  const std::string transformKey = "Transform 2DTransform";
                                  auto it = srcViewJson.find(transformKey);
                                  if (it == srcViewJson.end()) {
                                    return;
                                  }

                                  json::object dstViewJson;
                                  viewPtr->write(newSwcId, dstViewJson);
                                  dstViewJson["ViewObjType"] = json::value_from(swcDocTypeName);
                                  dstViewJson["ViewVersion"] = 1.0;
                                  dstViewJson[transformKey] = it->value();
                                  viewPtr->read(newSwcId, dstViewJson);
                                }
                                catch (const std::exception& e) {
                                  LOG(WARNING) << "Seed trace: failed to copy 2D transform onto new SWC: " << e.what();
                                }
                              };
                            startTrace(actionName,
                                       sourceImgObjId,
                                       imgPack,
                                       sc,
                                       t,
                                       seed,
                                       traceCfgPath,
                                       std::move(hostSwcOpt),
                                       promoteNewSwcToExistingTarget,
                                       std::move(onNewSwcCreated));
                          });
                }
              }
            }
          }
        }
      }
    }
  }

  struct Candidate
  {
    size_t imgObjId = 0;
    ZImgFilter* filter = nullptr;
    std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol;
    bool hasMeshDirectory = false;
    bool hasSkeletonDirectory = false;
    bool hasAnnotationsSource = false;
    bool hasSegmentPropertiesDirectory = false;
    QString meshSourceDirUrl;
    QString skeletonSourceDirUrl;
    QString annotationsSourceRootUrl;
  };

  std::vector<Candidate> candidates;
  candidates.reserve(m_idToFilter.size());
  for (const auto& idFilter : m_idToFilter) {
    const size_t imgObjId = idFilter.first;
    ZImgFilter* filter = idFilter.second.get();
    if (!filter || !filter->isVisible()) {
      continue;
    }
    const ZImgPack& imgPack = m_doc.imgPack(imgObjId);
    if (!imgPack.isNeuroglancerPrecomputed()) {
      continue;
    }
    std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol = imgPack.neuroglancerVolumeShared();
    if (!vol || !vol->isSegmentation()) {
      continue;
    }
    Candidate c;
    c.imgObjId = imgObjId;
    c.filter = filter;
    c.vol = std::move(vol);
    c.hasSegmentPropertiesDirectory = c.vol && c.vol->hasSegmentPropertiesDirectory();

    // External sources can either be declared in the dataset info (mesh/skeletons keys),
    // or explicitly registered by the user on the dataset object (override URL).
    c.meshSourceDirUrl = imgPack.neuroglancerMeshSourceOverrideUrl();
    if (c.meshSourceDirUrl.isEmpty() && c.vol && c.vol->hasMeshDirectory()) {
      c.meshSourceDirUrl = c.vol->meshDirUrl().toString(QUrl::StripTrailingSlash) + "/";
    }
    c.skeletonSourceDirUrl = imgPack.neuroglancerSkeletonSourceOverrideUrl();
    if (c.skeletonSourceDirUrl.isEmpty() && c.vol && c.vol->hasSkeletonDirectory()) {
      c.skeletonSourceDirUrl = c.vol->skeletonDirUrl().toString(QUrl::StripTrailingSlash) + "/";
    }

    c.hasMeshDirectory = !c.meshSourceDirUrl.isEmpty();
    c.hasSkeletonDirectory = !c.skeletonSourceDirUrl.isEmpty();

    // Annotations are stored in a separate precomputed dataset, so there is no volume-info fallback.
    c.annotationsSourceRootUrl = imgPack.neuroglancerAnnotationsSourceOverrideUrl();
    c.hasAnnotationsSource = !c.annotationsSourceRootUrl.trimmed().isEmpty();
    candidates.push_back(std::move(c));
  }
  if (candidates.empty()) {
    if (hasTraceActions) {
      return;
    }
    return;
  }

  const bool hasAnyMesh = std::any_of(candidates.begin(), candidates.end(), [](const Candidate& c) {
    return c.hasMeshDirectory;
  });

  const bool hasAnySkeleton = std::any_of(candidates.begin(), candidates.end(), [](const Candidate& c) {
    return c.hasSkeletonDirectory;
  });

  const bool hasAnyAnnotations = std::any_of(candidates.begin(), candidates.end(), [](const Candidate& c) {
    return c.hasAnnotationsSource;
  });

  auto startAnnotationsLoad =
    [this](const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol, QString annRootUrl, uint64_t segmentId) {
    CHECK(vol);
    annRootUrl = annRootUrl.trimmed();
    CHECK(!annRootUrl.isEmpty());
    QPointer<ZImgView> viewPtr(this);
    startDocOwnedCancellableSingleShotTask(
      m_doc.doc(),
      this,
      "ng_annotations_open",
      openAnnotationsSourceTask(vol, annRootUrl),
      [viewPtr, vol, segmentId, annRootUrl](NeuroglancerAnnotationsSourceOpenResult r) mutable {
        if (!viewPtr) {
          return;
        }

        if (!r.error.isEmpty()) {
          QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.error);
          return;
        }
        if (!r.source) {
          QMessageBox::information(QApplication::activeWindow(),
                                   QApplication::applicationName(),
                                   QStringLiteral("Failed to open Neuroglancer annotations dataset."));
          return;
        }
        if (r.relationshipIds.isEmpty()) {
          QMessageBox::information(
            QApplication::activeWindow(),
            QApplication::applicationName(),
            QStringLiteral("This Neuroglancer annotations dataset declares no relationships.\n\n"
                           "Atlas currently supports relationship-index based loads (by segment/object ID)."));
          return;
        }

        QString relationshipId;
        if (r.relationshipIds.size() == 1) {
          relationshipId = r.relationshipIds.at(0);
        } else {
          bool ok = false;
          relationshipId =
            QInputDialog::getItem(QApplication::activeWindow(),
                                  QApplication::applicationName(),
                                  QStringLiteral("Select relationship id (used to map object IDs to annotations):"),
                                  r.relationshipIds,
                                  /*current=*/0,
                                  /*editable=*/false,
                                  &ok)
              .trimmed();
          if (!ok || relationshipId.isEmpty()) {
            return;
          }
        }

        json::object sourceObj;
        sourceObj["type"] = "neuroglancer_precomputed_annotations";
        sourceObj["segmentation_root_url"] = json::value_from(vol->rootUrl());
        sourceObj["annotation_root_url"] = json::value_from(annRootUrl);
        sourceObj["relationship_id"] = json::value_from(relationshipId);
        sourceObj["object_id"] = json::value_from(QString::number(segmentId));
        const json::value sourceJson = sourceObj;

        if (auto existing = viewPtr->m_doc.doc().punctaDoc().findPunctaByExternalSource(sourceJson)) {
          (void)existing;
          return;
        }
        if (auto existing = viewPtr->m_doc.doc().skeletonDoc().findSkeletonByExternalSource(sourceJson)) {
          (void)existing;
          return;
        }

        startDocOwnedCancellableSingleShotTask(
          viewPtr->m_doc.doc(),
          viewPtr.data(),
          "ng_annotations_load_by_relationship",
          loadAnnotationsForRelationshipTask(vol, r.source, relationshipId, segmentId, annRootUrl, sourceJson),
          [viewPtr](NeuroglancerAnnotationsLoadResult out) mutable {
            if (!viewPtr) {
              return;
            }

            if (!out.error.isEmpty()) {
              QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), out.error);
              return;
            }

            if (out.puncta) {
              if (viewPtr->m_doc.doc().punctaDoc().findPunctaByExternalSource(out.sourceJson)) {
                return;
              }
              (void)viewPtr->m_doc.doc().punctaDoc().addPunctaFromExternalSource(std::move(*out.puncta),
                                                                                 out.displayName,
                                                                                 out.tooltip,
                                                                                 out.sourceJson);
              return;
            }
            if (out.skeleton) {
              if (viewPtr->m_doc.doc().skeletonDoc().findSkeletonByExternalSource(out.sourceJson)) {
                return;
              }
              (void)viewPtr->m_doc.doc().skeletonDoc().addSkeletonFromExternalSource(*out.skeleton,
                                                                                     out.displayName,
                                                                                     out.tooltip,
                                                                                     out.sourceJson);
              return;
            }

            QMessageBox::information(QApplication::activeWindow(),
                                     QApplication::applicationName(),
                                     QStringLiteral("No renderable annotation geometry was produced."));
          });
      });
  };

  auto startAnnotationsSpatialLoad =
    [this](const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol,
           QString annRootUrl,
           glm::dvec3 voxelMin,
           glm::dvec3 voxelMax) {
      CHECK(vol);
      annRootUrl = annRootUrl.trimmed();
      CHECK(!annRootUrl.isEmpty());

      const glm::dvec3 qMin{std::min(voxelMin.x, voxelMax.x),
                            std::min(voxelMin.y, voxelMax.y),
                            std::min(voxelMin.z, voxelMax.z)};
      const glm::dvec3 qMax{std::max(voxelMin.x, voxelMax.x),
                            std::max(voxelMin.y, voxelMax.y),
                            std::max(voxelMin.z, voxelMax.z)};

      json::object sourceObj;
      sourceObj["type"] = "neuroglancer_precomputed_annotations_spatial";
      sourceObj["segmentation_root_url"] = json::value_from(vol->rootUrl());
      sourceObj["annotation_root_url"] = json::value_from(annRootUrl);
      {
        json::array a;
        a.push_back(qMin.x);
        a.push_back(qMin.y);
        a.push_back(qMin.z);
        sourceObj["voxel_box_min"] = std::move(a);
      }
      {
        json::array a;
        a.push_back(qMax.x);
        a.push_back(qMax.y);
        a.push_back(qMax.z);
        sourceObj["voxel_box_max"] = std::move(a);
      }
      const json::value sourceJson = sourceObj;

      // If already loaded, do not re-fetch.
      if (m_doc.doc().punctaDoc().findPunctaByExternalSource(sourceJson) ||
          m_doc.doc().skeletonDoc().findSkeletonByExternalSource(sourceJson)) {
        return;
      }

      // Cancel any in-flight spatial annotation load started from this view. If the prior request already
      // produced partial results, mark it as cancelled so users understand why it stopped updating.
      cancelNeuroglancerAnnotationsSpatialLoad(/*markCancelledTooltip=*/true);
      const uint64_t generation = m_nextNgAnnotationsSpatialGeneration++;
      m_ngAnnotationsSpatialRequest = NeuroglancerAnnotationsSpatialRequest{
        .generation = generation,
        .displayName = QString("NG Annotations (View z≈%1)").arg(qMin.z, 0, 'g', 6),
        .segRootUrl = vol->rootUrl(),
        .annRootUrl = annRootUrl,
        .qMin = toArray(qMin),
        .qMax = toArray(qMax),
      };

      QPointer<ZImgView> viewPtr(this);

      ZBackgroundJobSpec spec;
      spec.title = QStringLiteral("Load Neuroglancer Annotations in View");
      spec.runningMessage = QStringLiteral("opening annotations dataset");
      spec.debugLabel = "ng_annotations_spatial";
      spec.work = [viewPtr, vol, annRootUrl = std::move(annRootUrl), qMin, qMax, sourceJson, generation](
                    ZBackgroundJobContext ctx) mutable {
        return ZImgView::runNeuroglancerAnnotationsSpatialLoadTask(std::move(ctx),
                                                                   viewPtr,
                                                                   std::move(vol),
                                                                   std::move(annRootUrl),
                                                                   qMin,
                                                                   qMax,
                                                                   std::move(sourceJson),
                                                                   generation);
      };

      auto* task = startBackgroundJob(m_doc.doc(), std::move(spec));
      if (m_ngAnnotationsSpatialRequest && m_ngAnnotationsSpatialRequest->generation == generation) {
        m_ngAnnotationsSpatialRequest->task = task;
      }
      return;
    };

  auto startMeshLoad =
    [this](const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol, QString meshSourceDirUrl, uint64_t segmentId) {
    CHECK(vol);
    meshSourceDirUrl = meshSourceDirUrl.trimmed();
    CHECK(!meshSourceDirUrl.isEmpty());

    const json::value sourceJson =
      makeNeuroglancerMeshExternalSourceJson(vol->rootUrl(),
                                             meshSourceDirUrl,
                                             segmentId,
                                             std::array<double, 3>{vol->baseImgInfo().voxelSizeX,
                                                                   vol->baseImgInfo().voxelSizeY,
                                                                   vol->baseImgInfo().voxelSizeZ},
                                             vol->baseVoxelOffset());

    // If already loaded, do not re-fetch.
    if (m_doc.doc().meshDoc().findMeshByExternalSource(sourceJson)) {
      return;
    }
    QPointer<ZImgView> viewPtr(this);
    startDocOwnedCancellableSingleShotTask(
      m_doc.doc(),
      this,
      "ng_mesh_load",
      loadMeshTask(vol, meshSourceDirUrl, segmentId, sourceJson),
      [viewPtr, vol, segmentId](NeuroglancerMeshLoadResult coarse) mutable {
        if (!viewPtr) {
          return;
        }

        if (!coarse.error.isEmpty()) {
          QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), coarse.error);
          return;
        }
        if (!coarse.mesh || coarse.mesh->empty()) {
          QMessageBox::information(
            QApplication::activeWindow(),
            QApplication::applicationName(),
            QString("Neuroglancer mesh load returned an empty mesh for segment %1").arg(segmentId));
          return;
        }

        const size_t meshObjId = viewPtr->m_doc.doc().meshDoc().addMeshFromExternalSource(*coarse.mesh,
                                                                                          coarse.displayName,
                                                                                          coarse.tooltip,
                                                                                          coarse.sourceJson,
                                                                                          vol->sharedRemoteContext());
        Q_UNUSED(meshObjId);
      });
  };

  auto startMeshBatchLoad =
    [this](const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol, QString meshSourceDirUrl, std::vector<uint64_t> ids) {
    CHECK(vol);
    meshSourceDirUrl = meshSourceDirUrl.trimmed();
    CHECK(!meshSourceDirUrl.isEmpty());

    if (ids.empty()) {
      return;
    }

    // Dedup early and strip background ID 0 (it is typically "unlabeled" and rarely has a mesh).
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    ids.erase(std::remove(ids.begin(), ids.end(), 0), ids.end());
    if (ids.empty()) {
      return;
    }

    QPointer<ZMeshDoc> meshDocPtr = &m_doc.doc().meshDoc();
    CHECK(meshDocPtr);
    ZMeshDoc* meshDoc = meshDocPtr.data();

    std::vector<ZNeuroglancerMeshExternalSourceKey> existingKeys;
    for (const size_t meshId : meshDoc->objs()) {
      const auto keyOpt = parseNeuroglancerMeshExternalSourceKey(meshDoc->jsonValue(meshId));
      if (!keyOpt) {
        continue;
      }
      existingKeys.push_back(*keyOpt);
    }

    QPointer<ZImgView> viewPtr(this);
    auto finish = [viewPtr](BatchLoadMessageResult r) {
      if (!viewPtr) {
        return;
      }
      if (!r.error.isEmpty()) {
        QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.error);
        return;
      }
      if (!r.message.isEmpty()) {
        QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.message);
      }
    };

    using FinishFn = decltype(finish);
    auto state =
      std::make_shared<CancellableDocOwnedSingleShotState<BatchLoadMessageResult, FinishFn>>(QPointer<QObject>(this),
                                                                                             std::move(finish));

    ZBackgroundJobSpec spec;
    spec.title = cancellableSingleShotTaskTitle("ng_mesh_batch_load");
    spec.debugLabel = "ng_mesh_batch_load";
    spec.work = [state = std::move(state),
                 vol,
                 meshDocPtr,
                 ids = std::move(ids),
                 existingKeys = std::move(existingKeys),
                 meshSourceDirUrl = std::move(meshSourceDirUrl)](ZBackgroundJobContext ctx) mutable {
      BatchLoadProgressFn progressFn = makeBatchLoadProgressFn(ctx);
      return runCancellableDocOwnedSingleShotTask(std::move(ctx),
                                                  loadMeshBatchTask(vol,
                                                                    meshDocPtr,
                                                                    std::move(ids),
                                                                    std::move(existingKeys),
                                                                    std::move(meshSourceDirUrl),
                                                                    std::move(progressFn)),
                                                  std::move(state));
    };
    (void)startBackgroundJob(m_doc.doc(), std::move(spec));
  };

  auto startSkeletonLoad =
    [this](const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol, QString skeletonSourceDirUrl, uint64_t segmentId) {
    CHECK(vol);
    skeletonSourceDirUrl = skeletonSourceDirUrl.trimmed();
    CHECK(!skeletonSourceDirUrl.isEmpty());

    json::object sourceObj;
    sourceObj["type"] = "neuroglancer_precomputed_skeleton";
    sourceObj["segmentation_root_url"] = json::value_from(vol->rootUrl());
    sourceObj["segment_id"] = json::value_from(QString::number(segmentId));
    sourceObj["skeleton_source_url"] = json::value_from(skeletonSourceDirUrl);
    const json::value sourceJson = sourceObj;

    // If already loaded, do not re-fetch.
    if (m_doc.doc().skeletonDoc().findSkeletonByExternalSource(sourceJson)) {
      return;
    }
    QPointer<ZImgView> viewPtr(this);
    startDocOwnedCancellableSingleShotTask(
      m_doc.doc(),
      this,
      "ng_skeleton_load",
      loadSkeletonTask(vol, skeletonSourceDirUrl, segmentId, sourceJson),
      [viewPtr, segmentId](NeuroglancerSkeletonLoadResult r) mutable {
        if (!viewPtr) {
          return;
        }

        if (!r.error.isEmpty()) {
          QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.error);
          return;
        }
        if (!r.skeleton || r.skeleton->empty()) {
          QMessageBox::information(
            QApplication::activeWindow(),
            QApplication::applicationName(),
            QString("Neuroglancer skeleton load returned an empty skeleton for segment %1").arg(segmentId));
          return;
        }

        if (viewPtr->m_doc.doc().skeletonDoc().findSkeletonByExternalSource(r.sourceJson)) {
          return;
        }
        (void)viewPtr->m_doc.doc().skeletonDoc().addSkeletonFromExternalSource(*r.skeleton,
                                                                               r.displayName,
                                                                               r.tooltip,
                                                                               r.sourceJson);
      });
  };

  auto startSkeletonBatchLoad =
    [this](const std::shared_ptr<ZNeuroglancerPrecomputedVolume>& vol, QString skeletonSourceDirUrl, std::vector<uint64_t> ids) {
    CHECK(vol);
    skeletonSourceDirUrl = skeletonSourceDirUrl.trimmed();
    CHECK(!skeletonSourceDirUrl.isEmpty());

    if (ids.empty()) {
      return;
    }

    // Dedup early and strip background ID 0 (it is typically "unlabeled" and rarely has a skeleton).
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    ids.erase(std::remove(ids.begin(), ids.end(), 0), ids.end());
    if (ids.empty()) {
      return;
    }

    QPointer<ZSkeletonDoc> skeletonDocPtr = &m_doc.doc().skeletonDoc();
    CHECK(skeletonDocPtr);
    ZSkeletonDoc* skeletonDoc = skeletonDocPtr.data();

    std::set<QString> existingKeys;
    for (const size_t skelId : skeletonDoc->objs()) {
      const auto keyOpt = parseNeuroglancerSkeletonExternalSourceKey(skeletonDoc->jsonValue(skelId));
      if (!keyOpt) {
        continue;
      }
      existingKeys.insert(
        neuroglancerSkeletonKeyString(keyOpt->rootUrl, keyOpt->skeletonSourceDirUrl, keyOpt->segmentId));
    }

    QPointer<ZImgView> viewPtr(this);
    auto finish = [viewPtr](BatchLoadMessageResult r) {
      if (!viewPtr) {
        return;
      }
      if (!r.error.isEmpty()) {
        QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.error);
        return;
      }
      if (!r.message.isEmpty()) {
        QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.message);
      }
    };

    using FinishFn = decltype(finish);
    auto state =
      std::make_shared<CancellableDocOwnedSingleShotState<BatchLoadMessageResult, FinishFn>>(QPointer<QObject>(this),
                                                                                             std::move(finish));

    ZBackgroundJobSpec spec;
    spec.title = cancellableSingleShotTaskTitle("ng_skeleton_batch_load");
    spec.debugLabel = "ng_skeleton_batch_load";
    spec.work = [state = std::move(state),
                 vol,
                 skeletonDocPtr,
                 ids = std::move(ids),
                 existingKeys = std::move(existingKeys),
                 skeletonSourceDirUrl = std::move(skeletonSourceDirUrl)](ZBackgroundJobContext ctx) mutable {
      BatchLoadProgressFn progressFn = makeBatchLoadProgressFn(ctx);
      return runCancellableDocOwnedSingleShotTask(std::move(ctx),
                                                  loadSkeletonBatchTask(vol,
                                                                        skeletonDocPtr,
                                                                        std::move(ids),
                                                                        std::move(existingKeys),
                                                                        std::move(skeletonSourceDirUrl),
                                                                        std::move(progressFn)),
                                                  std::move(state));
    };
    (void)startBackgroundJob(m_doc.doc(), std::move(spec));
  };

  menu.addSeparator();

  auto* copySegIdAct = menu.addAction("Copy Neuroglancer Segment ID Under Cursor");
  connect(copySegIdAct, &QAction::triggered, this, [scenePos, candidates]() {
    int bestPrecedence = std::numeric_limits<int>::min();
    std::optional<uint64_t> bestSeg;
    for (const auto& c : candidates) {
      CHECK(c.filter);
      const std::optional<uint64_t> segOpt = c.filter->cachedNeuroglancerSegmentationIdAtScenePos(scenePos);
      if (!segOpt) {
        continue;
      }
      const int precedence = c.filter->viewPrecedence();
      if (!bestSeg || precedence > bestPrecedence) {
        bestPrecedence = precedence;
        bestSeg = segOpt;
      }
    }
    if (!bestSeg) {
      QMessageBox::information(QApplication::activeWindow(),
                               QApplication::applicationName(),
                               QStringLiteral("No cached Neuroglancer segment ID is available under the cursor yet.\n\n"
                                              "Wait for the visible tiles to finish loading, then try again."));
      return;
    }
    QApplication::clipboard()->setText(QString::number(*bestSeg));
  });

  if (!hasAnySkeleton) {
    auto* explainSkeletonAct =
      menu.addAction("Neuroglancer Skeletons: configure skeleton source in Object View Setting…");
    connect(explainSkeletonAct, &QAction::triggered, this, [candidates]() {
      std::set<QString> roots;
      for (const auto& c : candidates) {
        if (c.vol) {
          roots.insert(c.vol->rootUrl());
        }
      }

      QString rootList;
      for (const auto& r : roots) {
        rootList += QString("  %1\n").arg(r);
      }

      QMessageBox::information(
        QApplication::activeWindow(),
        QApplication::applicationName(),
        QStringLiteral("No Neuroglancer skeleton source is configured for the currently visible segmentation dataset(s).\n\n"
                       "To enable skeleton loading:\n"
                       "  1) Select the Neuroglancer segmentation object.\n"
                       "  2) Open Object View Setting → Neuroglancer Sources.\n"
                       "  3) Set \"Skeleton Source Override\" to a skeletons directory URL/path (absolute or relative).\n\n"
                       "Visible segmentation dataset(s):\n%1")
          .arg(rootList.trimmed()));
    });
  }

  if (!hasAnyAnnotations) {
    auto* explainAnnotationsAct =
      menu.addAction("Neuroglancer Annotations: configure annotations source in Object View Setting…");
    connect(explainAnnotationsAct, &QAction::triggered, this, [candidates]() {
      std::set<QString> roots;
      for (const auto& c : candidates) {
        if (c.vol) {
          roots.insert(c.vol->rootUrl());
        }
      }

      QString rootList;
      for (const auto& r : roots) {
        rootList += QString("  %1\n").arg(r);
      }

      QMessageBox::information(
        QApplication::activeWindow(),
        QApplication::applicationName(),
        QStringLiteral("No Neuroglancer annotations source is configured for the currently visible segmentation dataset(s).\n\n"
                       "To enable annotation loading:\n"
                       "  1) Select the Neuroglancer segmentation object.\n"
                       "  2) Open Object View Setting → Neuroglancer Sources.\n"
                       "  3) Set \"Annotations Source Override\" to an annotations dataset root URL/path (absolute or relative).\n\n"
                       "Visible segmentation dataset(s):\n%1")
          .arg(rootList.trimmed()));
    });
  }

  if (hasAnyMesh) {
    auto* loadMeshAct = menu.addAction("Load Neuroglancer Mesh for Segment Under Cursor");
    connect(loadMeshAct, &QAction::triggered, this, [scenePos, startMeshLoad, candidates]() {
      int bestPrecedence = std::numeric_limits<int>::min();
      std::optional<uint64_t> bestSeg;
      std::shared_ptr<ZNeuroglancerPrecomputedVolume> bestVol;
      QString bestMeshSourceDirUrl;
      for (const auto& c : candidates) {
        if (!c.hasMeshDirectory) {
          continue;
        }
        CHECK(c.filter);
        const std::optional<uint64_t> segOpt = c.filter->cachedNeuroglancerSegmentationIdAtScenePos(scenePos);
        if (!segOpt) {
          continue;
        }
        const int precedence = c.filter->viewPrecedence();
        if (!bestSeg || precedence > bestPrecedence) {
          bestPrecedence = precedence;
          bestSeg = segOpt;
          bestVol = c.vol;
          bestMeshSourceDirUrl = c.meshSourceDirUrl;
        }
      }
      if (!bestSeg || !bestVol || bestMeshSourceDirUrl.trimmed().isEmpty()) {
        QMessageBox::information(QApplication::activeWindow(),
                                 QApplication::applicationName(),
                                 QStringLiteral("No cached Neuroglancer segment ID is available under the cursor yet.\n\n"
                                                "Wait for the visible tiles to finish loading, then try again."));
        return;
      }
      startMeshLoad(bestVol, bestMeshSourceDirUrl, *bestSeg);
    });
  }

  if (hasAnySkeleton) {
    auto* loadSkeletonAct = menu.addAction("Load Neuroglancer Skeleton for Segment Under Cursor");
    connect(loadSkeletonAct, &QAction::triggered, this, [scenePos, startSkeletonLoad, candidates]() {
      int bestPrecedence = std::numeric_limits<int>::min();
      std::optional<uint64_t> bestSeg;
      std::shared_ptr<ZNeuroglancerPrecomputedVolume> bestVol;
      QString bestSkeletonSourceDirUrl;
      for (const auto& c : candidates) {
        if (!c.hasSkeletonDirectory) {
          continue;
        }
        CHECK(c.filter);
        const std::optional<uint64_t> segOpt = c.filter->cachedNeuroglancerSegmentationIdAtScenePos(scenePos);
        if (!segOpt) {
          continue;
        }
        const int precedence = c.filter->viewPrecedence();
        if (!bestSeg || precedence > bestPrecedence) {
          bestPrecedence = precedence;
          bestSeg = segOpt;
          bestVol = c.vol;
          bestSkeletonSourceDirUrl = c.skeletonSourceDirUrl;
        }
      }
      if (!bestSeg || !bestVol || bestSkeletonSourceDirUrl.trimmed().isEmpty()) {
        QMessageBox::information(QApplication::activeWindow(),
                                 QApplication::applicationName(),
                                 QStringLiteral("No cached Neuroglancer segment ID is available under the cursor yet.\n\n"
                                                "Wait for the visible tiles to finish loading, then try again."));
        return;
      }
      startSkeletonLoad(bestVol, bestSkeletonSourceDirUrl, *bestSeg);
    });
  }

  if (hasAnyAnnotations) {
    auto* loadAnnUnderCursorAct = menu.addAction("Load Neuroglancer Annotations for Segment Under Cursor");
    connect(loadAnnUnderCursorAct, &QAction::triggered, this, [scenePos, candidates, startAnnotationsLoad]() {
      int bestPrecedence = std::numeric_limits<int>::min();
      std::optional<uint64_t> bestSeg;
      std::shared_ptr<ZNeuroglancerPrecomputedVolume> bestVol;
      QString bestAnnotationsRootUrl;
      for (const auto& c : candidates) {
        if (!c.hasAnnotationsSource) {
          continue;
        }
        CHECK(c.filter);
        const std::optional<uint64_t> segOpt = c.filter->cachedNeuroglancerSegmentationIdAtScenePos(scenePos);
        if (!segOpt) {
          continue;
        }
        const int precedence = c.filter->viewPrecedence();
        if (!bestSeg || precedence > bestPrecedence) {
          bestPrecedence = precedence;
          bestSeg = segOpt;
          bestVol = c.vol;
          bestAnnotationsRootUrl = c.annotationsSourceRootUrl;
        }
      }
      if (!bestSeg || !bestVol || bestAnnotationsRootUrl.trimmed().isEmpty()) {
        QMessageBox::information(QApplication::activeWindow(),
                                 QApplication::applicationName(),
                                 QStringLiteral("No cached Neuroglancer segment ID is available under the cursor yet.\n\n"
                                                "Wait for the visible tiles to finish loading, then try again."));
        return;
      }
      startAnnotationsLoad(bestVol, bestAnnotationsRootUrl, *bestSeg);
    });
  }

  // Choose the top-most (highest view precedence) visible segmentation layer for dataset-scoped actions.
  auto pickTopmostSegmentationCandidate = [&](auto wantsCandidate) -> const Candidate* {
    int bestPrecedence = std::numeric_limits<int>::min();
    const Candidate* best = nullptr;
    for (const auto& c : candidates) {
      if (!wantsCandidate(c)) {
        continue;
      }
      CHECK(c.filter);
      const int precedence = c.filter->viewPrecedence();
      if (!best || precedence > bestPrecedence) {
        bestPrecedence = precedence;
        best = &c;
      }
    }
    return best;
  };

  const Candidate* topAnnotations = hasAnyAnnotations ? pickTopmostSegmentationCandidate([](const Candidate& c) {
    return c.hasAnnotationsSource;
  }) : nullptr;

  if (hasAnyAnnotations) {
    if (topAnnotations && topAnnotations->vol && !topAnnotations->annotationsSourceRootUrl.trimmed().isEmpty()) {
      const std::shared_ptr<ZNeuroglancerPrecomputedVolume> topVol = topAnnotations->vol;
      const QString annotationsRootUrl = topAnnotations->annotationsSourceRootUrl;
      auto* loadAnnByIdAct =
        menu.addAction(QString("Load Neuroglancer Annotations for Segment ID… (%1)").arg(topVol->rootUrl()));
      connect(loadAnnByIdAct, &QAction::triggered, this, [topVol, annotationsRootUrl, startAnnotationsLoad]() {
        QString prefill;
        if (const auto clipOpt = parseNeuroglancerUint64Base10(QApplication::clipboard()->text())) {
          prefill = QString::number(*clipOpt);
        }
        const QString s = QInputDialog::getText(QApplication::activeWindow(),
                                                QApplication::applicationName(),
                                                QStringLiteral("Neuroglancer segment/object ID (uint64, base-10):"),
                                                QLineEdit::Normal,
                                                prefill)
                            .trimmed();
        if (s.isEmpty()) {
          return;
        }
        const auto idOpt = parseNeuroglancerUint64Base10(s);
        if (!idOpt) {
          QMessageBox::information(QApplication::activeWindow(),
                                   QApplication::applicationName(),
                                   QStringLiteral("Invalid object ID. Expected a base-10 uint64 value."));
          return;
        }
        startAnnotationsLoad(topVol, annotationsRootUrl, *idOpt);
      });

      auto* loadAnnInViewAct =
        menu.addAction(QString("Load Neuroglancer Annotations in View (spatial index)… (%1)").arg(topVol->rootUrl()));
      connect(loadAnnInViewAct,
              &QAction::triggered,
              this,
              [this, topVol, annotationsRootUrl, startAnnotationsSpatialLoad]() {
                const QRectF viewport = m_view.currentViewport().normalized();
                const int z = m_view.currentSlice();

                // Query a thin slab corresponding to the current cross-section slice.
                const glm::dvec3 voxelMin{viewport.left(), viewport.top(), static_cast<double>(z)};
                const glm::dvec3 voxelMax{viewport.right(), viewport.bottom(), static_cast<double>(z + 1)};

                startAnnotationsSpatialLoad(topVol, annotationsRootUrl, voxelMin, voxelMax);
              });
    }
  }

  const Candidate* topMesh = hasAnyMesh ? pickTopmostSegmentationCandidate([](const Candidate& c) {
    return c.hasMeshDirectory;
  })
                                        : nullptr;

  const Candidate* topSkeleton = hasAnySkeleton ? pickTopmostSegmentationCandidate([](const Candidate& c) {
    return c.hasSkeletonDirectory;
  })
                                                : nullptr;

  if (hasAnyMesh) {
    if (topMesh && topMesh->vol) {
      const std::shared_ptr<ZNeuroglancerPrecomputedVolume> topVol = topMesh->vol;
      const QString meshSourceDirUrl = topMesh->meshSourceDirUrl;
      auto* loadMeshByIdAct =
        menu.addAction(QString("Load Neuroglancer Mesh for Segment ID… (%1)").arg(topVol->rootUrl()));
      connect(loadMeshByIdAct, &QAction::triggered, this, [topVol, meshSourceDirUrl, startMeshLoad]() {
        QString prefill;
        if (const auto clipOpt = parseNeuroglancerUint64Base10(QApplication::clipboard()->text())) {
          prefill = QString::number(*clipOpt);
        }
        const QString s = QInputDialog::getText(QApplication::activeWindow(),
                                                QApplication::applicationName(),
                                                QStringLiteral("Neuroglancer segment ID (uint64, base-10):"),
                                                QLineEdit::Normal,
                                                prefill)
                            .trimmed();
        if (s.isEmpty()) {
          return;
        }
        const auto idOpt = parseNeuroglancerUint64Base10(s);
        if (!idOpt) {
          QMessageBox::information(QApplication::activeWindow(),
                                   QApplication::applicationName(),
                                   QStringLiteral("Invalid segment ID. Expected a base-10 uint64 value."));
          return;
        }
        startMeshLoad(topVol, meshSourceDirUrl, *idOpt);
      });

      auto* loadMeshesFromClipboardAct =
        menu.addAction(QString("Load Neuroglancer Meshes for Segment IDs from Clipboard… (%1)").arg(topVol->rootUrl()));
      connect(loadMeshesFromClipboardAct, &QAction::triggered, this, [this, topVol, meshSourceDirUrl, startMeshBatchLoad]() {
        const QString text = QApplication::clipboard()->text();
        const auto stateResult = parseClipboardNeuroglancerMeshStateForDataset(text, topVol->rootUrl());
        if (stateResult.status == ClipboardNeuroglancerMeshStateResult::Status::Error) {
          QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), stateResult.error);
          return;
        }
        if (stateResult.status == ClipboardNeuroglancerMeshStateResult::Status::Matched) {
          CHECK(topVol);
          ZMeshDoc& meshDoc = m_doc.doc().meshDoc();
          const QString normalizedRootUrl = ZNeuroglancerPrecomputedVolume::normalizeRootUrl(topVol->rootUrl());
          const QString normalizedMeshSourceDirUrl = normalizedDirUrlForComparison(meshSourceDirUrl);

          size_t desiredVisibleCount = 0;
          if (stateResult.layerVisible) {
            for (const auto& [segmentId, visible] : stateResult.desiredSegmentVisibility) {
              if (segmentId != 0 && visible) {
                ++desiredVisibleCount;
              }
            }
          }
          const size_t desiredHiddenCount = stateResult.desiredSegmentVisibility.size() - desiredVisibleCount;

          std::set<uint64_t> alreadyLoaded;
          std::vector<std::pair<size_t, bool>> visibilityUpdates;
          size_t willShow = 0;
          size_t willHide = 0;
          for (const size_t meshId : meshDoc.objs()) {
            const auto keyOpt = parseNeuroglancerMeshExternalSourceKey(meshDoc.jsonValue(meshId));
            if (!keyOpt) {
              continue;
            }
            if (keyOpt->rootUrl != normalizedRootUrl ||
                normalizedDirUrlForComparison(keyOpt->meshSourceDirUrl) != normalizedMeshSourceDirUrl) {
              continue;
            }

            alreadyLoaded.insert(keyOpt->segmentId);
            auto it = stateResult.desiredSegmentVisibility.find(keyOpt->segmentId);
            const bool shouldBeVisible =
              stateResult.layerVisible && it != stateResult.desiredSegmentVisibility.end() && it->second;
            if (meshDoc.isObjVisible(meshId) == shouldBeVisible) {
              continue;
            }
            visibilityUpdates.emplace_back(meshId, shouldBeVisible);
            if (shouldBeVisible) {
              ++willShow;
            } else {
              ++willHide;
            }
          }

          std::vector<uint64_t> idsToLoad;
          if (stateResult.layerVisible) {
            idsToLoad.reserve(desiredVisibleCount);
            for (const auto& [segmentId, visible] : stateResult.desiredSegmentVisibility) {
              if (!visible || segmentId == 0 || alreadyLoaded.contains(segmentId)) {
                continue;
              }
              idsToLoad.push_back(segmentId);
            }
          }

          if (visibilityUpdates.empty() && idsToLoad.empty()) {
            QMessageBox::information(
              QApplication::activeWindow(),
              QApplication::applicationName(),
              QStringLiteral(
                "Atlas already matches the Neuroglancer state for this dataset. No mesh visibility changes or new mesh loads are needed."));
            return;
          }

          const QString layerName = stateResult.matchedLayerName.trimmed().isEmpty()
                                      ? QStringLiteral("<unnamed>")
                                      : stateResult.matchedLayerName.trimmed();
          QString prompt;
          if (!stateResult.layerVisible) {
            prompt = QStringLiteral(
                       "Matched Neuroglancer segmentation layer '%1' is hidden.\n\n"
                       "Atlas will hide %2 loaded mesh object(s) for this dataset and load no new meshes.\n\nContinue?")
                       .arg(layerName)
                       .arg(willHide);
          } else {
            prompt =
              QStringLiteral(
                "Apply Neuroglancer state for segmentation layer '%1'?\n\n"
                "Visible segments in state: %2\n"
                "Hidden segments in state: %3\n"
                "Loaded meshes to show: %4\n"
                "Loaded meshes to hide: %5\n"
                "New visible meshes to load: %6\n\n"
                "Atlas will update existing mesh visibility to match the state and then load any missing visible meshes.")
                .arg(layerName)
                .arg(desiredVisibleCount)
                .arg(desiredHiddenCount)
                .arg(willShow)
                .arg(willHide)
                .arg(idsToLoad.size());
          }

          const int response = QMessageBox::question(QApplication::activeWindow(),
                                                     QApplication::applicationName(),
                                                     prompt,
                                                     QMessageBox::Ok | QMessageBox::Cancel,
                                                     QMessageBox::Cancel);
          if (response != QMessageBox::Ok) {
            return;
          }

          for (const auto& [meshId, visible] : visibilityUpdates) {
            m_doc.doc().setObjVisible(meshId, visible);
          }
          if (!idsToLoad.empty()) {
            startMeshBatchLoad(topVol, meshSourceDirUrl, std::move(idsToLoad));
          }
          return;
        }

        const std::vector<uint64_t> ids = parseUint64ListFromText(text);
        if (ids.empty()) {
          QMessageBox::information(
            QApplication::activeWindow(),
            QApplication::applicationName(),
            QStringLiteral(
              "Clipboard does not contain a Neuroglancer state for this dataset or any base-10 uint64 segment IDs."));
          return;
        }

        std::set<uint64_t> unique(ids.begin(), ids.end());
        const int response = QMessageBox::question(
          QApplication::activeWindow(),
          QApplication::applicationName(),
          QString("Load meshes for %1 segment IDs from clipboard?\n\nThis will download mesh data and may take time.")
            .arg(unique.size()),
          QMessageBox::Ok | QMessageBox::Cancel,
          QMessageBox::Cancel);
        if (response != QMessageBox::Ok) {
          return;
        }

        startMeshBatchLoad(topVol, meshSourceDirUrl, std::vector<uint64_t>(unique.begin(), unique.end()));
      });

      auto* loadVisibleMeshesAct =
        menu.addAction(QString("Load Neuroglancer Meshes for Visible Segments (cached)… (%1)").arg(topVol->rootUrl()));
      connect(loadVisibleMeshesAct, &QAction::triggered, this, [this, topVol, meshSourceDirUrl, startMeshBatchLoad]() {
        CHECK(topVol);

        const QRectF viewport = m_view.currentViewport();
        const double scale = m_view.currentScale();
        const int z = m_view.currentSlice();
        QPointer<ZImgView> viewPtr(this);
        startDocOwnedSingleShotTask(
          m_doc.doc(),
          this,
          "ng_mesh_visible_ids",
          collectVisibleSegIdsTask(topVol, z, viewport, scale),
          [viewPtr, topVol, meshSourceDirUrl, startMeshBatchLoad](CollectVisibleSegIdsResult r) mutable {
            if (!viewPtr) {
              return;
            }

            if (!r.error.isEmpty()) {
              QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.error);
              return;
            }
            if (r.ids.empty()) {
              QMessageBox::information(
                QApplication::activeWindow(),
                QApplication::applicationName(),
                QStringLiteral(
                  "No cached Neuroglancer segmentation IDs are available at the current zoom level yet.\n\n"
                  "This action only considers cached tiles at the target (final) LOD for the current viewport scale "
                  "and ignores coarse fallback tiles.\n\n"
                  "Wait for refinement to complete for the current zoom level, then try again."));
              return;
            }

            const int response = QMessageBox::question(
              QApplication::activeWindow(),
              QApplication::applicationName(),
              QString("Load meshes for %1 segments visible in the current viewport (cached tiles only)?\n\n"
                      "This will download mesh data and may take time.\n"
                      "Note: segment ID 0 is treated as background and will be ignored.")
                .arg(r.ids.size()),
              QMessageBox::Ok | QMessageBox::Cancel,
              QMessageBox::Cancel);
            if (response != QMessageBox::Ok) {
              return;
            }

            startMeshBatchLoad(topVol, meshSourceDirUrl, std::move(r.ids));
          });
      });
    }
  }

  if (hasAnySkeleton) {
    if (topSkeleton && topSkeleton->vol) {
      const std::shared_ptr<ZNeuroglancerPrecomputedVolume> topVol = topSkeleton->vol;
      const QString skeletonSourceDirUrl = topSkeleton->skeletonSourceDirUrl;
      auto* loadSkeletonByIdAct =
        menu.addAction(QString("Load Neuroglancer Skeleton for Segment ID… (%1)").arg(topVol->rootUrl()));
      connect(loadSkeletonByIdAct,
              &QAction::triggered,
              this,
              [topVol, skeletonSourceDirUrl, startSkeletonLoad]() {
        QString prefill;
        if (const auto clipOpt = parseNeuroglancerUint64Base10(QApplication::clipboard()->text())) {
          prefill = QString::number(*clipOpt);
        }
        const QString s = QInputDialog::getText(QApplication::activeWindow(),
                                                QApplication::applicationName(),
                                                QStringLiteral("Neuroglancer segment ID (uint64, base-10):"),
                                                QLineEdit::Normal,
                                                prefill)
                            .trimmed();
        if (s.isEmpty()) {
          return;
        }
        const auto idOpt = parseNeuroglancerUint64Base10(s);
        if (!idOpt) {
          QMessageBox::information(QApplication::activeWindow(),
                                   QApplication::applicationName(),
                                   QStringLiteral("Invalid segment ID. Expected a base-10 uint64 value."));
          return;
        }
        startSkeletonLoad(topVol, skeletonSourceDirUrl, *idOpt);
      });

      auto* loadSkeletonsFromClipboardAct = menu.addAction(
        QString("Load Neuroglancer Skeletons for Segment IDs from Clipboard… (%1)").arg(topVol->rootUrl()));
      connect(loadSkeletonsFromClipboardAct,
              &QAction::triggered,
              this,
              [topVol, skeletonSourceDirUrl, startSkeletonBatchLoad]() {
        const QString text = QApplication::clipboard()->text();
        const std::vector<uint64_t> ids = parseUint64ListFromText(text);
        if (ids.empty()) {
          QMessageBox::information(QApplication::activeWindow(),
                                   QApplication::applicationName(),
                                   QStringLiteral("Clipboard does not contain any base-10 uint64 segment IDs."));
          return;
        }

        std::set<uint64_t> unique(ids.begin(), ids.end());
        const int response = QMessageBox::question(
          QApplication::activeWindow(),
          QApplication::applicationName(),
          QString(
            "Load skeletons for %1 segment IDs from clipboard?\n\nThis will download skeleton data and may take time.")
            .arg(unique.size()),
          QMessageBox::Ok | QMessageBox::Cancel,
          QMessageBox::Cancel);
        if (response != QMessageBox::Ok) {
          return;
        }

        startSkeletonBatchLoad(topVol, skeletonSourceDirUrl, std::vector<uint64_t>(unique.begin(), unique.end()));
      });

      auto* loadVisibleSkeletonsAct = menu.addAction(
        QString("Load Neuroglancer Skeletons for Visible Segments (cached)… (%1)").arg(topVol->rootUrl()));
      connect(loadVisibleSkeletonsAct,
              &QAction::triggered,
              this,
              [this, topVol, skeletonSourceDirUrl, startSkeletonBatchLoad]() {
        CHECK(topVol);

        const QRectF viewport = m_view.currentViewport();
        const double scale = m_view.currentScale();
        const int z = m_view.currentSlice();
        QPointer<ZImgView> viewPtr(this);
        startDocOwnedSingleShotTask(
          m_doc.doc(),
          this,
          "ng_skeleton_visible_ids",
          collectVisibleSegIdsTask(topVol, z, viewport, scale),
          [viewPtr, topVol, skeletonSourceDirUrl, startSkeletonBatchLoad](CollectVisibleSegIdsResult r) mutable {
            if (!viewPtr) {
              return;
            }

            if (!r.error.isEmpty()) {
              QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.error);
              return;
            }
            if (r.ids.empty()) {
              QMessageBox::information(
                QApplication::activeWindow(),
                QApplication::applicationName(),
                QStringLiteral("No cached Neuroglancer segmentation IDs are available at the current zoom level yet.\n\n"
                               "This action only considers cached tiles at the target (final) LOD for the current viewport "
                               "scale and ignores coarse fallback tiles.\n\n"
                               "Wait for refinement to complete for the current zoom level, then try again."));
              return;
            }

            const int response = QMessageBox::question(
              QApplication::activeWindow(),
              QApplication::applicationName(),
              QString("Load skeletons for %1 segments visible in the current viewport (cached tiles only)?\n\n"
                      "This will download skeleton data and may take time.\n"
                      "Note: segment ID 0 is treated as background and will be ignored.")
                .arg(r.ids.size()),
              QMessageBox::Ok | QMessageBox::Cancel,
              QMessageBox::Cancel);
            if (response != QMessageBox::Ok) {
              return;
            }

            startSkeletonBatchLoad(topVol, skeletonSourceDirUrl, std::move(r.ids));
          });
      });
    }
  }

  // Bulk load (segment_properties). Potentially heavy; expose per dataset.
  auto pickTopmostSourceDirUrlForRoot = [&](const QString& rootUrl, auto getUrl) -> QString {
    int bestPrecedence = std::numeric_limits<int>::min();
    QString bestUrl;
    for (const auto& c : candidates) {
      if (!c.vol || c.vol->rootUrl() != rootUrl) {
        continue;
      }
      const QString url = getUrl(c).trimmed();
      if (url.isEmpty()) {
        continue;
      }
      CHECK(c.filter);
      const int precedence = c.filter->viewPrecedence();
      if (bestUrl.isEmpty() || precedence > bestPrecedence) {
        bestPrecedence = precedence;
        bestUrl = url;
      }
    }
    return bestUrl;
  };

  std::vector<std::shared_ptr<ZNeuroglancerPrecomputedVolume>> volsWithSegProps;
  volsWithSegProps.reserve(candidates.size());
  for (const auto& c : candidates) {
    if (!c.hasSegmentPropertiesDirectory) {
      continue;
    }
    volsWithSegProps.push_back(c.vol);
  }
  std::sort(volsWithSegProps.begin(), volsWithSegProps.end(), [](const auto& a, const auto& b) {
    return a->rootUrl() < b->rootUrl();
  });
  volsWithSegProps.erase(std::unique(volsWithSegProps.begin(),
                                     volsWithSegProps.end(),
                                     [](const auto& a, const auto& b) {
                                       return a->rootUrl() == b->rootUrl();
                                     }),
                         volsWithSegProps.end());

  for (const auto& vol : volsWithSegProps) {
    const QString meshSourceDirUrl =
      pickTopmostSourceDirUrlForRoot(vol->rootUrl(), [](const Candidate& c) { return c.meshSourceDirUrl; });

    auto* showPropsAct = menu.addAction(QString("Show Neuroglancer Segment Properties… (%1)").arg(vol->rootUrl()));
    connect(
      showPropsAct,
      &QAction::triggered,
      this,
      [this,
       meshDocPtr = QPointer<ZMeshDoc>(&m_doc.doc().meshDoc()),
       skeletonDocPtr = QPointer<ZSkeletonDoc>(&m_doc.doc().skeletonDoc()),
       vol,
       meshSourceDirUrl,
       startMeshLoad]() {
        auto* dlg = new ZNeuroglancerSegmentPropertiesDialog(m_doc.doc(), vol, QApplication::activeWindow());
        dlg->setAttribute(Qt::WA_DeleteOnClose, true);
        if (!meshSourceDirUrl.isEmpty()) {
          dlg->setMeshLoadCallback([startMeshLoad, vol, meshSourceDirUrl](uint64_t id) {
            startMeshLoad(vol, meshSourceDirUrl, id);
          });
        }
        dlg->setAfterPropertiesLoadedCallback([meshDocPtr, skeletonDocPtr, propsRootUrl = vol->rootUrl()](auto props) {
          if (QCoreApplication::closingDown() || !props) {
            return;
          }

          if (meshDocPtr) {
            for (const size_t meshId : meshDocPtr->objs()) {
              const auto srcOpt = parseNeuroglancerMeshExternalSourceKey(meshDocPtr->jsonValue(meshId));
              if (!srcOpt) {
                continue;
              }
              if (srcOpt->rootUrl != propsRootUrl) {
                continue;
              }

              const uint64_t segmentId = srcOpt->segmentId;
              const auto labelOpt = props->labelForId(segmentId);
              const auto descOpt = props->descriptionForId(segmentId);
              if (!labelOpt && !descOpt) {
                continue;
              }

              const QString label = labelOpt ? *labelOpt : QString{};
              const QString description = descOpt ? *descOpt : QString{};

              const QString displayName = label.isEmpty() ? QString("NG Mesh %1").arg(segmentId)
                                                          : QString("NG Mesh %1 (%2)").arg(segmentId).arg(label);

              QString tooltip = QString("Neuroglancer precomputed mesh\nSegmentation: %1\nSegment: %2")
                                  .arg(propsRootUrl)
                                  .arg(segmentId);
              if (!label.isEmpty()) {
                tooltip += QString("\nLabel: %1").arg(label);
              }
              if (!description.isEmpty()) {
                tooltip += QString("\nDescription: %1").arg(description);
              }

              meshDocPtr->updateExternalMeshMetadata(meshId, displayName, tooltip);
            }
          }

          if (skeletonDocPtr) {
            for (const size_t skelId : skeletonDocPtr->objs()) {
              const auto srcOpt = parseNeuroglancerSkeletonExternalSourceKey(skeletonDocPtr->jsonValue(skelId));
              if (!srcOpt) {
                continue;
              }
              if (srcOpt->rootUrl != propsRootUrl) {
                continue;
              }

              const uint64_t segmentId = srcOpt->segmentId;
              const auto labelOpt = props->labelForId(segmentId);
              const auto descOpt = props->descriptionForId(segmentId);
              if (!labelOpt && !descOpt) {
                continue;
              }

              const QString label = labelOpt ? *labelOpt : QString{};
              const QString description = descOpt ? *descOpt : QString{};

              const QString displayName = label.isEmpty() ? QString("NG Skeleton %1").arg(segmentId)
                                                          : QString("NG Skeleton %1 (%2)").arg(segmentId).arg(label);

              QString tooltip = QString("Neuroglancer precomputed skeleton\nSegmentation: %1\nSegment: %2")
                                  .arg(propsRootUrl)
                                  .arg(segmentId);
              if (!label.isEmpty()) {
                tooltip += QString("\nLabel: %1").arg(label);
              }
              if (!description.isEmpty()) {
                tooltip += QString("\nDescription: %1").arg(description);
              }

              skeletonDocPtr->updateExternalSkeletonMetadata(skelId, displayName, tooltip);
            }
          }
        });
        dlg->open();
      });
  }

  for (const auto& vol : volsWithSegProps) {
    const QString meshSourceDirUrl =
      pickTopmostSourceDirUrlForRoot(vol->rootUrl(), [](const Candidate& c) { return c.meshSourceDirUrl; });
    if (meshSourceDirUrl.isEmpty()) {
      continue;
    }
    auto* loadAllMeshesAct = menu.addAction(
      QString("Load Neuroglancer Meshes for All Segments (segment_properties)… (%1)").arg(vol->rootUrl()));
    connect(loadAllMeshesAct, &QAction::triggered, this, [this, vol, meshSourceDirUrl, startMeshBatchLoad]() {
      const auto response = QMessageBox::question(
        QApplication::activeWindow(),
        QApplication::applicationName(),
        QString(
          "Load meshes for all segments listed in segment_properties?\n\nThis will download segment properties (if not cached) and may take a long time and consume significant memory."),
        QMessageBox::Ok | QMessageBox::Cancel,
        QMessageBox::Cancel);
      if (response != QMessageBox::Ok) {
        return;
      }
      QPointer<ZImgView> viewPtr(this);
      startDocOwnedCancellableSingleShotTask(
        m_doc.doc(),
        this,
        "ng_mesh_segment_properties_ids",
        collectAllSegmentPropertiesIdsTask(vol),
        [viewPtr, vol, meshSourceDirUrl, startMeshBatchLoad](CollectVisibleSegIdsResult r) mutable {
          if (!viewPtr) {
            return;
          }
          if (!r.error.isEmpty()) {
            QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.error);
            return;
          }
          if (r.ids.empty()) {
            QMessageBox::information(QApplication::activeWindow(),
                                     QApplication::applicationName(),
                                     QStringLiteral("segment_properties returned an empty ID list."));
            return;
          }
          startMeshBatchLoad(vol, meshSourceDirUrl, std::move(r.ids));
        });
    });
  }

  for (const auto& vol : volsWithSegProps) {
    const QString skeletonSourceDirUrl =
      pickTopmostSourceDirUrlForRoot(vol->rootUrl(), [](const Candidate& c) { return c.skeletonSourceDirUrl; });
    if (skeletonSourceDirUrl.isEmpty()) {
      continue;
    }
    auto* loadAllSkeletonsAct = menu.addAction(
      QString("Load Neuroglancer Skeletons for All Segments (segment_properties)… (%1)").arg(vol->rootUrl()));
    connect(loadAllSkeletonsAct,
            &QAction::triggered,
            this,
            [this, vol, skeletonSourceDirUrl, startSkeletonBatchLoad]() {
      const auto response = QMessageBox::question(
        QApplication::activeWindow(),
        QApplication::applicationName(),
        QString(
          "Load skeletons for all segments listed in segment_properties?\n\nThis will download segment properties (if not cached) and may take a long time and consume significant memory."),
        QMessageBox::Ok | QMessageBox::Cancel,
        QMessageBox::Cancel);
      if (response != QMessageBox::Ok) {
        return;
      }
      QPointer<ZImgView> viewPtr(this);
      startDocOwnedCancellableSingleShotTask(
        m_doc.doc(),
        this,
        "ng_skeleton_segment_properties_ids",
        collectAllSegmentPropertiesIdsTask(vol),
        [viewPtr, vol, skeletonSourceDirUrl, startSkeletonBatchLoad](CollectVisibleSegIdsResult r) mutable {
          if (!viewPtr) {
            return;
          }
          if (!r.error.isEmpty()) {
            QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), r.error);
            return;
          }
          if (r.ids.empty()) {
            QMessageBox::information(QApplication::activeWindow(),
                                     QApplication::applicationName(),
                                     QStringLiteral("segment_properties returned an empty ID list."));
            return;
          }
          startSkeletonBatchLoad(vol, skeletonSourceDirUrl, std::move(r.ids));
        });
    });
  }
}

QString ZImgView::infoOfPos(double x, double y)
{
  QString info;
  try {
    for (const auto& idFilter : m_idToFilter) {
      const ZImgFilter* viewControl = idFilter.second.get();
      if (!viewControl->isVisible()) {
        continue;
      }
      size_t id = idFilter.first;
      const ZImgPack& imgPack = m_doc.imgPack(id);
      QPointF p = idFilter.second->mapFromScene(QPointF(x, y));
      index_t lx = p.x();
      index_t ly = p.y();
      if (lx >= 0 && lx < imgPack.imgInfo().sWidth() && ly >= 0 && ly < imgPack.imgInfo().sHeight()) {
        auto lz = m_view.isMaxZProjView() ? 0 : viewControl->imgSlice();
        auto lt = viewControl->imgTime();
        info += imgPack.sizeInfo();
        if (imgPack.imgInfo().numTimes == 1) {
          info += QString(" Coord: (%1,%2,%3)").arg(lx).arg(ly).arg(lz);
        } else {
          info += QString(" Coord: (%1,%2,%3,%4)").arg(lx).arg(ly).arg(lz).arg(lt);
        }
        info += QString(" Intensity: (%1")
                  .arg(imgPack.displayValue(lx, ly, lz, 0, lt, m_view.isMaxZProjView()),
                       0,
                       'g',
                       QLocale::FloatingPointShortest);
        for (size_t c = 1; c < imgPack.imgInfo().numChannels; ++c) {
          info += QString(", %1").arg(imgPack.displayValue(lx, ly, lz, c, lt, m_view.isMaxZProjView()),
                                      0,
                                      'g',
                                      QLocale::FloatingPointShortest);
        }
        info += ")";

        // Neuroglancer segmentation: enrich hover readout with label/description when already available,
        // without triggering any network I/O (cache-only).
        if (!m_view.isMaxZProjView() && imgPack.isNeuroglancerPrecomputed()) {
          const std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol = imgPack.neuroglancerVolumeShared();
          if (vol && vol->isSegmentation()) {
            if (const auto props = vol->segmentPropertiesShared()) {
              const auto segOpt = imgPack.tryGetCachedNeuroglancerSegmentationId(static_cast<size_t>(lx),
                                                                                 static_cast<size_t>(ly),
                                                                                 static_cast<size_t>(lz));
              if (segOpt) {
                if (const auto labelOpt = props->labelForId(*segOpt)) {
                  info += QString(" Label: %1").arg(*labelOpt);
                }
                if (const auto descOpt = props->descriptionForId(*segOpt)) {
                  info += QString(" Desc: %1").arg(*descOpt);
                }
              }
            }
          }
        }

        info += "      ";
      }
    }
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to read image value for hover info: " << e.what();
    showNonBlockingCriticalMessage(QApplication::activeWindow(), QApplication::applicationName(), e.what());
  }
  return info;
}

void ZImgView::docImgsAdded(const std::vector<size_t>& objs)
{
  for (auto id : objs) {
    auto viewControl = new ZImgFilter(m_view);
    viewControl->setData(m_doc.imgPack(id));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[id].reset(viewControl);
    connect(viewControl, &ZImgFilter::boundBoxChanged, this, &ZImgView::updateBoundBox);
    connect(viewControl, &ZImgFilter::objDeselected, this, &ZImgView::onObjDeselectedFromView);
    connect(viewControl, &ZImgFilter::objSelected, this, &ZImgView::onObjSelectedFromView);
    connect(viewControl, &ZImgFilter::objVisibleChanged, this, &ZImgView::onObjVisibleChangedFromView);
    connect(viewControl,
            &ZImgFilter::neuroglancerDatasetUnsupported,
            this,
            [this, id](const QString& /*datasetUrl*/, const QString& /*error*/) {
              // Remove the object via ZDoc so the object manager stays consistent.
              // Use a queued connection to avoid deleting the sender while it is still processing callbacks.
              if (!m_doc.hasObjWithID(id)) {
                return;
              }
              m_doc.doc().removeObj(id);
            },
            Qt::QueuedConnection);
    Q_EMIT objViewReady(id);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZImgView::docImgAdded(size_t id)
{
  auto viewControl = new ZImgFilter(m_view);
  viewControl->setData(m_doc.imgPack(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, &ZImgFilter::boundBoxChanged, this, &ZImgView::updateBoundBox);
  connect(viewControl, &ZImgFilter::objDeselected, this, &ZImgView::onObjDeselectedFromView);
  connect(viewControl, &ZImgFilter::objSelected, this, &ZImgView::onObjSelectedFromView);
  connect(viewControl, &ZImgFilter::objVisibleChanged, this, &ZImgView::onObjVisibleChangedFromView);
  connect(viewControl,
          &ZImgFilter::neuroglancerDatasetUnsupported,
          this,
          [this, id](const QString& /*datasetUrl*/, const QString& /*error*/) {
            if (!m_doc.hasObjWithID(id)) {
              return;
            }
            m_doc.doc().removeObj(id);
          },
          Qt::QueuedConnection);
  Q_EMIT objViewReady(id);
}

void ZImgView::docImgChanged(size_t id)
{
  m_idToFilter.at(id)->setData(m_doc.imgPack(id));
}

} // namespace nim
