#pragma once

#include "zfilterview.h"
#include "zimgdoc.h"
#include "zimgfilter.h"

#include <atomic>
#include <array>
#include <memory>
#include <optional>

class QMenu;

namespace nim {

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

private:
  std::shared_ptr<std::atomic_bool> m_ngAnnotationsSpatialCancel;

  // Best-effort state used to provide a nice UX when a spatial-annotations load is superseded
  // by a newer request (show "cancelled" instead of silently stopping).
  std::optional<size_t> m_ngAnnotationsSpatialPunctaId;
  std::optional<size_t> m_ngAnnotationsSpatialSkeletonId;
  QString m_ngAnnotationsSpatialDisplayName;
  QString m_ngAnnotationsSpatialSegRootUrl;
  QString m_ngAnnotationsSpatialAnnRootUrl;
  std::array<double, 3> m_ngAnnotationsSpatialQMin{0.0, 0.0, 0.0};
  std::array<double, 3> m_ngAnnotationsSpatialQMax{0.0, 0.0, 0.0};
  bool m_ngAnnotationsSpatialCompleted = false;
};

} // namespace nim
