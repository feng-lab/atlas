#pragma once

#include "zbackgroundjob.h"
#include "zfilterview.h"
#include "zimgdoc.h"
#include "zimgfilter.h"
#include "zneuroglancerprecomputedannotations.h"

#include <QPointer>

#include <array>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class QMenu;

namespace nim {

class ZBackgroundTask;
class ZNeuroglancerPrecomputedVolume;
class ZPuncta;

class ZImgView : public ZFilterView<ZImgDoc, ZImgFilter>
{
  Q_OBJECT

public:
  ZImgView(ZImgDoc& doc, ZView& view);

  ~ZImgView() override;

  // ZObjView interface

public:
  QString infoOfPos(double x, double y) override;

  void appendContextMenuActions(QMenu& menu,
                                size_t activeObjId,
                                const QPointF& scenePos,
                                Qt::KeyboardModifiers modifiers) override;

private:
  void docImgsAdded(const std::vector<size_t>& objs);

  void docImgAdded(size_t id);

  void docImgChanged(size_t id);

  void cancelNeuroglancerAnnotationsSpatialLoad(bool markCancelledTooltip);

  [[nodiscard]] bool hasActiveNeuroglancerAnnotationsSpatialRequest(uint64_t generation) const;

  void updateNeuroglancerAnnotationsSpatialCancelledUi(QString status);
  void handleNeuroglancerAnnotationsSpatialFailureOnUi(uint64_t generation, const QString& error);

  static folly::coro::Task<ZBackgroundJobOutcome>
  runNeuroglancerAnnotationsSpatialLoadTask(ZBackgroundJobContext ctx,
                                            QPointer<ZImgView> viewPtr,
                                            std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol,
                                            QString annRootUrl,
                                            glm::dvec3 qMin,
                                            glm::dvec3 qMax,
                                            json::value sourceJson,
                                            uint64_t generation);

  void
  initializeNeuroglancerAnnotationsSpatialRequestOnUi(uint64_t generation, bool renderAsPuncta, json::value sourceJson);

  void applyNeuroglancerAnnotationsSpatialPunctaUpdateOnUi(
    uint64_t generation,
    ZNeuroglancerPrecomputedAnnotationsSource::SpatialLoadProgress progress,
    std::shared_ptr<ZPuncta> batch);

  void applyNeuroglancerAnnotationsSpatialSkeletonUpdateOnUi(
    uint64_t generation,
    ZNeuroglancerPrecomputedAnnotationsSource::SpatialLoadProgress progress,
    std::shared_ptr<std::pair<std::vector<glm::vec3>, std::vector<glm::uvec2>>> geometry);

private:
  struct NeuroglancerAnnotationsSpatialRequest
  {
    uint64_t generation = 0;
    QPointer<ZBackgroundTask> task;
    std::optional<size_t> punctaObjId;
    std::optional<size_t> skeletonObjId;
    QString displayName;
    QString segRootUrl;
    QString annRootUrl;
    std::array<double, 3> qMin{0.0, 0.0, 0.0};
    std::array<double, 3> qMax{0.0, 0.0, 0.0};
    bool completed = false;
  };

  std::optional<NeuroglancerAnnotationsSpatialRequest> m_ngAnnotationsSpatialRequest;
  uint64_t m_nextNgAnnotationsSpatialGeneration = 1;
};

} // namespace nim
