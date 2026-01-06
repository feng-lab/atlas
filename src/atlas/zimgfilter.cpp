#include "zimgfilter.h"

#include "zimg.h"
#include "zimgdoc.h"
#include "zimgview.h"
#include "zimgpackdisplay.h"
#include "zneuroglancerprecomputed.h"
#include "zlog.h"
#include "zmessageboxhelpers.h"
#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include "ztheme.h"
#include "zgraphicsview.h"
#include <QGraphicsPixmapItem>
#include <QGraphicsSimpleTextItem>
#include <QFutureWatcher>
#include <QApplication>
#include <QStyleOption>
#include <QPainter>
#include <QPushButton>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <QtConcurrent/QtConcurrentMap>
#include <cmath>
#include <memory>
#include <array>
#include <map>
#include <limits>
#include <vector>

namespace nim {

namespace {

// The cache-only pass is intended to be cheap and to quickly paint "something" using already-cached
// chunks. Cap the target-scale used for ratio selection so we don't try to composite a large number
// of fine tiles from cache when zoomed in.
constexpr double kNg2DCacheRenderScaleForRatioSelection = 0.5;
constexpr int kNg2DRefineDebounceMs = 150;

class ZOverwritePixmapItem : public QGraphicsPixmapItem
{
public:
  using QGraphicsPixmapItem::QGraphicsPixmapItem;

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override
  {
    CHECK(painter);
    painter->save();
    painter->setCompositionMode(QPainter::CompositionMode_Source);
    QGraphicsPixmapItem::paint(painter, option, widget);
    painter->restore();
  }
};

struct Neuroglancer2DRenderParams
{
  std::shared_ptr<ZNeuroglancerPrecomputedVolume> volume;
  ZImgInfo imgInfo;
  QRectF viewport;
  size_t z = 0;
  size_t t = 0;
  double renderScale = 1.0;
  enum class Pass
  {
    CacheOnly,
    Preview,
    Final,
  };
  Pass pass = Pass::CacheOnly;
  std::map<size_t, std::pair<double, double>> channels;
  std::map<size_t, col4> channelColors;
  double alpha = 1.0;
  size_t tileWidth = 4096;
  size_t tileHeight = 4096;
  ZImgColorizationMode colorizationMode = ZImgColorizationMode::Intensity;
};

struct Neuroglancer2DRenderResult
{
  uint64_t epoch = 0;
  Neuroglancer2DRenderParams::Pass pass = Neuroglancer2DRenderParams::Pass::CacheOnly;
  ZQImagePack qImagePack;
  QString error;
};

struct Neuroglancer2DTileSharedParams
{
  std::shared_ptr<ZNeuroglancerPrecomputedVolume> volume;
  ZImgInfo imgInfo;
  size_t z = 0;
  size_t t = 0;
  std::array<size_t, 3> targetRatio{1, 1, 1};
  int64_t ratioZ = 1;
  double tileScale = 1.0;
  uint64_t epoch = 0;
  std::shared_ptr<std::atomic<uint64_t>> epochAtomic;
  Neuroglancer2DRenderParams::Pass pass = Neuroglancer2DRenderParams::Pass::CacheOnly;
  std::map<size_t, std::pair<double, double>> channels;
  std::map<size_t, col4> channelColors;
  double alpha = 1.0;
  size_t tileWidth = 4096;
  size_t tileHeight = 4096;
  ZImgColorizationMode colorizationMode = ZImgColorizationMode::Intensity;
};

struct Neuroglancer2DTileTask
{
  std::shared_ptr<const Neuroglancer2DTileSharedParams> shared;
  ZNeuroglancerPrecomputedVolume::Chunk chunk;
  QPoint loc;
};

struct Neuroglancer2DTileResult
{
  uint64_t epoch = 0;
  Neuroglancer2DRenderParams::Pass pass = Neuroglancer2DRenderParams::Pass::CacheOnly;
  ZQImagePack qImagePack;
  QString error;
};

[[nodiscard]] double effectivePixelScaleForLod(const ZGraphicsView& view, const glm::dvec2& transformScale)
{
  const double viewScale = view.currentScale();
  const double scaleMag = std::max(std::abs(transformScale.x), std::abs(transformScale.y));

  double dpr = 1.0;
  if (const QWindow* w = view.windowHandle()) {
    dpr = static_cast<double>(w->devicePixelRatio());
  } else {
    dpr = static_cast<double>(view.devicePixelRatioF());
  }

  CHECK(viewScale > 0);
  CHECK(scaleMag > 0);
  CHECK(dpr > 0);
  return viewScale * scaleMag * dpr;
}

[[nodiscard]] int neuroglancer2DPassPriority(Neuroglancer2DRenderParams::Pass pass)
{
  switch (pass) {
  case Neuroglancer2DRenderParams::Pass::CacheOnly:
    return 0;
  case Neuroglancer2DRenderParams::Pass::Preview:
    return 1;
  case Neuroglancer2DRenderParams::Pass::Final:
    return 2;
  }
  return 0;
}

Neuroglancer2DRenderResult renderNeuroglancer2D(Neuroglancer2DRenderParams params, uint64_t epoch)
{
  Neuroglancer2DRenderResult out;
  out.epoch = epoch;
  out.pass = params.pass;

  try {
    CHECK(params.volume);
    CHECK(params.t == 0);
    CHECK(params.renderScale > 0);

    ZNeuroglancerPrecomputedVolume::SliceTilePack tiles;
    if (params.pass == Neuroglancer2DRenderParams::Pass::CacheOnly) {
      tiles = params.volume->sliceTilePackFor2DViewportCacheBestEffort(params.z, params.t, params.viewport, params.renderScale);
    } else {
      const auto policy = (params.pass == Neuroglancer2DRenderParams::Pass::Preview)
                            ? ZNeuroglancerPrecomputedVolume::Slice2DRatioPolicy::CoarsestXY
                            : ZNeuroglancerPrecomputedVolume::Slice2DRatioPolicy::BestForScale;
      tiles = params.volume->sliceTilePackFor2DViewportBlocking(params.z, params.t, params.viewport, params.renderScale, policy);
    }

    out.qImagePack = qImagePackFromZImgs(tiles.imgs,
                                         tiles.locs,
                                         tiles.scales,
                                         params.imgInfo,
                                         params.channels,
                                         params.channelColors,
                                         params.alpha,
                                         params.tileWidth,
                                         params.tileHeight,
                                         params.colorizationMode);
    return out;
  }
  catch (const std::exception& e) {
    out.error = QString::fromUtf8(e.what());
    return out;
  }
}

[[nodiscard]] bool isEpochCurrent(const std::shared_ptr<std::atomic<uint64_t>>& epochAtomic, uint64_t epoch)
{
  if (!epochAtomic) {
    return true;
  }
  return epochAtomic->load(std::memory_order_relaxed) == epoch;
}

Neuroglancer2DTileResult renderNeuroglancer2DTile(Neuroglancer2DTileTask task)
{
  Neuroglancer2DTileResult out;
  CHECK(task.shared);
  out.epoch = task.shared->epoch;
  out.pass = task.shared->pass;

  try {
    if (!isEpochCurrent(task.shared->epochAtomic, task.shared->epoch)) {
      return out;
    }

    CHECK(task.shared->volume);
    CHECK(task.shared->t == 0);
    CHECK(task.shared->ratioZ > 0);

    auto chunkImg = task.shared->volume->readChunkBlocking(task.chunk);
    if (!chunkImg) {
      return out;
    }

    if (!isEpochCurrent(task.shared->epochAtomic, task.shared->epoch)) {
      return out;
    }

    const int64_t z0 = static_cast<int64_t>(task.shared->z);
    const int64_t localZBase = z0 - task.chunk.baseStart[2];
    CHECK(localZBase >= 0);
    const int64_t localZ = localZBase / task.shared->ratioZ;
    CHECK(localZ >= 0);
    CHECK(static_cast<size_t>(localZ) < chunkImg->depth());

    ZImg sliceImg = chunkImg->crop(ZImgRegion(0,
                                              static_cast<index_t>(chunkImg->width()),
                                              0,
                                              static_cast<index_t>(chunkImg->height()),
                                              static_cast<index_t>(localZ),
                                              static_cast<index_t>(localZ + 1)));

    if (!isEpochCurrent(task.shared->epochAtomic, task.shared->epoch)) {
      return out;
    }

    std::vector<std::shared_ptr<ZImg>> imgs;
    imgs.push_back(std::make_shared<ZImg>(std::move(sliceImg)));

    std::vector<QPoint> locs;
    locs.push_back(task.loc);

    std::vector<double> scales;
    scales.push_back(task.shared->tileScale);

    out.qImagePack = qImagePackFromZImgs(imgs,
                                         locs,
                                         scales,
                                         task.shared->imgInfo,
                                         task.shared->channels,
                                         task.shared->channelColors,
                                         task.shared->alpha,
                                         task.shared->tileWidth,
                                         task.shared->tileHeight,
                                         task.shared->colorizationMode);
    return out;
  }
  catch (const std::exception& e) {
    out.error = QString::fromUtf8(e.what());
    return out;
  }
}

} // namespace

ZImgScaleBarGraphicsItem::ZImgScaleBarGraphicsItem(double length,
                                                   double height,
                                                   double voxelSizeXInUm,
                                                   double viewScale,
                                                   double transformScale,
                                                   const QRectF& viewPort,
                                                   const glm::vec3& color,
                                                   QGraphicsItem* parent)
  : QGraphicsRectItem(parent)
  , m_lengthInUm(length)
  , m_height(height)
  , m_voxelSizeXInUm(voxelSizeXInUm)
  , m_viewScale(viewScale)
  , m_transformScale(transformScale)
  , m_viewPort(viewPort)
  , m_viewPortPos(.8, .8)
{
  setFlags(QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable);

  setToolTip(QString("length: %1 µm (voxel size: %2 µm)").arg(m_lengthInUm).arg(m_voxelSizeXInUm));
  setColor(color);
  setCursor(Qt::PointingHandCursor);
  updateRectSize();
  updatePos();
}

QVariant ZImgScaleBarGraphicsItem::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value)
{
  if (change == ItemPositionChange && scene()) {
    // VLOG(1) << value.toPointF();
    QPointF newPos = value.toPointF();
    // VLOG(1) << mapFromScene(newPos);
    // auto vp = mapRectFromScene(m_viewPort);
    // VLOG(1) << m_viewPort.topLeft() << m_viewPort.bottomRight() << vp.topLeft() << vp.bottomRight();
    newPos = QPointF(qMin(m_viewPort.right(), qMax(newPos.x(), m_viewPort.left())),
                     qMin(m_viewPort.bottom(), qMax(newPos.y(), m_viewPort.top())));
    //    newPos.setX(qMin(m_viewPort.right(), qMax(newPos.x(), m_viewPort.left())));
    //    newPos.setY(qMin(m_viewPort.bottom(), qMax(newPos.y(), m_viewPort.top())));
    m_viewPortPos.x = (newPos.x() - m_viewPort.left()) / m_viewPort.width();
    m_viewPortPos.y = (newPos.y() - m_viewPort.top()) / m_viewPort.height();
    // VLOG(1) << newPos;
    return newPos;
  }
  return QGraphicsRectItem::itemChange(change, value);
}

void ZImgScaleBarGraphicsItem::updateRectSize()
{
  double height = m_height / m_viewScale;
  double width = m_lengthInUm / m_voxelSizeXInUm * m_transformScale;
  QRectF rect(0, 0, width, height);
  setRect(rect);
}

void ZImgScaleBarGraphicsItem::updatePos()
{
  setPos(m_viewPort.left() + m_viewPort.width() * m_viewPortPos.x,
         m_viewPort.top() + m_viewPort.height() * m_viewPortPos.y);
}

ZImgFilter::ZImgFilter(ZView& view)
  : ZObjFilter(view)
  , m_colorizationMode("Voxel Display")
  , m_opacity(QString("Opacity"), 1.0, 0.0, 1.0)
  , m_showScaleBar("Show Scale Bar", false)
  , m_scaleBarLengthInUm("Scale Bar Length", 10., 0.0001, 1e9)
  , m_scaleBarHeight("Scale Bar Height", 5, 1, 500)
  , m_scaleBarColor("Scale Bar Color", glm::vec3(1., 1., 1.))
  , m_ngRefineTimer(this)
{
  m_ngRenderEpochAtomic = std::make_shared<std::atomic<uint64_t>>(m_ngRenderEpoch);

  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZImgFilter::visibleChanged);
  m_colorizationMode.addOptions("Intensity", "Segmentation Labels");
  m_colorizationMode.select("Intensity");
  m_colorizationMode.setDescription(QStringLiteral(
    "Controls how voxel values are converted to colors in the 2D view:\n"
    "- 'Intensity' (default) uses per-channel range + color parameters.\n"
    "- 'Segmentation Labels' treats the volume as a label map and assigns a stable pseudo-random color to each label ID.\n"
    "  This is recommended for Neuroglancer 'type=segmentation' volumes and other label images."));
  connect(&m_colorizationMode, &ZStringIntOptionParameter::valueChanged, this, &ZImgFilter::colorizationModeChanged);
  addParameter(&m_colorizationMode);
  connect(&m_opacity, &ZDoubleParameter::valueChanged, this, &ZImgFilter::opacityChanged);
  addParameter(&m_opacity);
  connect(&m_showScaleBar, &ZBoolParameter::valueChanged, this, &ZImgFilter::showScaleBarChanged);
  addParameter(&m_showScaleBar);
  m_scaleBarLengthInUm.setSuffix(" µm");
  m_scaleBarLengthInUm.setStyle("SPINBOX");
  connect(&m_scaleBarLengthInUm, &ZDoubleParameter::valueChanged, this, &ZImgFilter::scaleBarLengthChanged);
  addParameter(&m_scaleBarLengthInUm);
  m_scaleBarHeight.setSuffix(" pixels");
  m_scaleBarHeight.setStyle("SPINBOX");
  connect(&m_scaleBarHeight, &ZIntParameter::valueChanged, this, &ZImgFilter::scaleBarHeightChanged);
  addParameter(&m_scaleBarHeight);
  m_scaleBarColor.setStyle("COLOR");
  connect(&m_scaleBarColor, &ZVec3Parameter::valueChanged, this, &ZImgFilter::scaleBarColorChanged);
  addParameter(&m_scaleBarColor);
  {
    const QSignalBlocker blocker(m_viewPrecedencePara);
    m_viewPrecedencePara.set(getViewPrecedence());
  }
  connect(&m_view, &ZView::viewportChanged, this, &ZImgFilter::viewportChanged);
  connect(&view.graphicsView(), &ZGraphicsView::scaleChanged, this, &ZImgFilter::viewScaleChanged);

  m_ngRefineTimer.setSingleShot(true);
  m_ngRefineTimer.setInterval(kNg2DRefineDebounceMs);
  connect(&m_ngRefineTimer, &QTimer::timeout, this, [this]() {
    if (!m_isVisible || !m_imgPack || !m_imgPack->isNeuroglancerPrecomputed()) {
      return;
    }
    if (m_view.isMaxZProjView()) {
      return;
    }
    startNeuroglancer2DRender(/*finalPass=*/true);
  });
}

void ZImgFilter::setData(ZImgPack& pack)
{
  m_imgPack = &pack;
  destroyImgItems();
  m_display.reset();

  // Invalidate any in-flight neuroglancer async renders from the previous dataset.
  ++m_ngRenderEpoch;
  if (m_ngRenderEpochAtomic) {
    m_ngRenderEpochAtomic->store(m_ngRenderEpoch, std::memory_order_relaxed);
  }
  m_ngCacheInFlight = false;
  m_ngCacheInFlightEpoch = 0;
  m_ngCacheDirty = false;
  m_ngPreviewInFlight = false;
  m_ngPreviewInFlightEpoch = 0;
  m_ngPreviewDirty = false;
  m_ngFinalInFlight = false;
  m_ngFinalInFlightEpoch = 0;
  m_ngFinalPending = false;
  m_ngLastAppliedFinalPass = false;
  m_ngFinalCompletedEpoch = std::numeric_limits<uint64_t>::max();
  m_ngFinalErrorEpoch = std::numeric_limits<uint64_t>::max();
  m_ngFinalError.clear();
  m_ngRefineTimer.stop();
  updateNeuroglancerLoadingIndicator();

  for (size_t i = 0; i < m_channelVisibleParas.size(); ++i) {
    m_channelColorParas[i]->disconnect();
    m_channelVisibleParas[i]->disconnect();
    m_doubleChannelRangeParas[i]->disconnect();
    removeParameter(m_channelColorParas[i].get());
    removeParameter(m_channelVisibleParas[i].get());
    removeParameter(m_doubleChannelRangeParas[i].get());
  }
  m_channelVisibleParas.clear();
  m_channelColorParas.clear();
  m_doubleChannelRangeParas.clear();

  glm::dvec2 dr(m_imgPack->rangeMin(), m_imgPack->rangeMax());
  if (m_imgPack->hasMinMax() && m_imgPack->maxIntensity() > m_imgPack->minIntensity()) {
    dr = glm::dvec2(m_imgPack->minIntensity(), m_imgPack->maxIntensity());
  }

  const bool labelCapable =
    (m_imgPack->imgInfo().voxelFormat == VoxelFormat::Unsigned && m_imgPack->imgInfo().numChannels == 1);
  {
    const QSignalBlocker blocker(m_colorizationMode);
    if (!labelCapable) {
      m_colorizationMode.select("Intensity");
    } else if (m_imgPack->isNeuroglancerPrecomputed() && m_imgPack->neuroglancerVolumeShared()->isSegmentation()) {
      m_colorizationMode.select("Segmentation Labels");
    }
    m_colorizationMode.setEnabled(labelCapable);
  }

  for (size_t c = 0; c < m_imgPack->imgInfo().numChannels; ++c) {
    const bool labelMode = currentColorizationMode() == ZImgColorizationMode::SegmentationLabels;
    m_channelVisibleParas.emplace_back(
      std::make_unique<ZBoolParameter>(QString("Show %1").arg(m_imgPack->imgInfo().displayChannelName(c)), true));
    m_doubleChannelRangeParas.emplace_back(std::make_unique<ZDoubleSpanParameter>(
      QString("%1 Display Range").arg(m_imgPack->imgInfo().displayChannelName(c)),
      dr,
      m_imgPack->rangeMin(),
      m_imgPack->rangeMax()));
    if (m_imgPack->imgInfo().voxelFormat != VoxelFormat::Float) {
      m_doubleChannelRangeParas.back()->setDecimal(0);
      m_doubleChannelRangeParas.back()->setSingleStep(1);
    }
    m_channelColorParas.emplace_back(
      std::make_unique<ZVec3Parameter>(QString("%1 Color").arg(m_imgPack->imgInfo().displayChannelName(c)),
                                       glm::vec3(m_imgPack->imgInfo().channelColors[c].r / 255.,
                                                 m_imgPack->imgInfo().channelColors[c].g / 255.,
                                                 m_imgPack->imgInfo().channelColors[c].b / 255.),
                                       glm::vec3(0.f),
                                       glm::vec3(1.f)));
    m_channelColorParas[c]->setStyle("COLOR");
    connect(m_channelVisibleParas[c].get(),
            &ZBoolParameter::boolChanged,
            m_doubleChannelRangeParas[c].get(),
            &ZDoubleSpanParameter::setEnabled);
    connect(m_channelVisibleParas[c].get(),
            &ZBoolParameter::boolChanged,
            m_channelColorParas[c].get(),
            &ZVec3Parameter::setEnabled);
    connect(m_channelVisibleParas[c].get(), &ZBoolParameter::valueChanged, this, &ZImgFilter::channelVisibleChanged);
    connect(m_doubleChannelRangeParas[c].get(),
            &ZDoubleSpanParameter::valueChanged,
            this,
            &ZImgFilter::channelRangeChanged);
    connect(m_channelColorParas[c].get(), &ZVec3Parameter::valueChanged, this, &ZImgFilter::channelColorChanged);

    m_doubleChannelRangeParas[c]->setVisible(!labelMode);
    if (labelMode || m_imgPack->imgInfo().isAlphaChannel(c)) {
      m_channelColorParas[c]->setVisible(false);
    }
  }

  if (m_mipRange) {
    m_mipRange->disconnect();
    removeParameter(m_mipRange.get());
  }
  m_mipRange = std::make_unique<ZIntSpanParameter>("Maximum Z Projection Range",
                                                   glm::ivec2(0, m_imgPack->imgInfo().depth - 1),
                                                   0,
                                                   m_imgPack->imgInfo().depth - 1);
  connect(m_mipRange.get(), &ZIntSpanParameter::valueChanged, this, &ZImgFilter::mipRangeChanged);
  addParameter(m_mipRange.get());

  m_display = std::make_unique<ZImgPackDisplay>(*m_imgPack);
  m_display->setColorizationMode(currentColorizationMode());
  if (m_view.isMaxZProjView() && m_imgPack->imgInfo().depth > 1) {
    m_display->setMIP(true);
    m_display->setMIPZRange(m_mipRange->get().x, m_mipRange->get().y);
  }
  for (size_t c = 0; c < m_imgPack->imgInfo().numChannels; ++c) {
    m_display->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
    m_display->setChannelColor(c, m_imgPack->imgInfo().channelColors[c]);
    // m_display->setAlpha(m_opacity.get());

    addParameter(m_channelVisibleParas[c].get());
    addParameter(m_channelColorParas[c].get());
    addParameter(m_doubleChannelRangeParas[c].get());
  }

  if (m_imgPack->imgInfo().voxelSizeUnit != VoxelSizeUnit::none) {
    m_showScaleBar.setEnabled(true);
    m_scaleBarItem = std::make_unique<ZImgScaleBarGraphicsItem>(m_scaleBarLengthInUm.get(),
                                                                m_scaleBarHeight.get(),
                                                                m_imgPack->imgInfo().voxelSizeXInUm(),
                                                                m_view.currentScale(),
                                                                getTransformScale().x,
                                                                mapFromSceneRect(m_view.currentViewport()),
                                                                m_scaleBarColor.get());
    m_scaleBarItem->setVisible(false);
    m_scaleBarItem->setZValue(30000);
    m_view.scene().addItem(m_scaleBarItem.get());
  } else {
    m_showScaleBar.setEnabled(false);
  }

  if (m_view.isMaxZProjView()) {
    setMaxZProjView(m_view.currentTime());
  } else {
    setNormalView(m_view.currentSlice(), m_view.currentTime());
  }

  updateViewSettingWidgetsGroup();
}

void ZImgFilter::releaseItemsOwnership()
{
  (void)m_item.release();
  (void)m_scaleBarItem.release();
}

void ZImgFilter::setSelected(bool v)
{
  if (m_item && m_item->isSelected() != v) {
    m_item->setSelected(v);
  }
}

void ZImgFilter::setNormalView(int z, int t)
{
  bool isVisibleBefore = m_isVisible;
  auto logicalZ = realZ(z);
  auto logicalT = realT(t);
  m_sliceValid = logicalT >= 0 && logicalT < int(m_imgPack->imgInfo().numTimes) && logicalZ >= 0 &&
                 logicalZ < int(m_imgPack->imgInfo().depth);
  m_isVisible = m_hasVisibleChannel && m_visible.get() && m_sliceValid;

  m_display->setSlice(logicalZ);
  m_display->setTime(logicalT);
  m_display->setMIP(false);
  if (m_isVisible) {
    updateImgItems();
  } else if (isVisibleBefore) {
    hideImgItems();
  }

  showScaleBarChanged();
}

void ZImgFilter::setMaxZProjView(int t)
{
  bool isVisibleBefore = m_isVisible;
  auto logicalT = realT(t);
  m_sliceValid = logicalT >= 0 && logicalT < int(m_imgPack->imgInfo().numTimes);
  m_isVisible = m_hasVisibleChannel && m_visible.get() && m_sliceValid;

  m_display->setSlice(0);
  m_display->setTime(logicalT);
  m_display->setMIP(true);
  m_display->setMIPZRange(m_mipRange->get().x, m_mipRange->get().y);
  if (m_isVisible) {
    updateImgItems();
  } else if (isVisibleBefore) {
    hideImgItems();
  }

  showScaleBarChanged();
}

ZBBox<glm::ivec4> ZImgFilter::boundBox() const
{
  ZBBox<glm::ivec4> res;
  res.setMinCorner(glm::ivec4(0));
  res.setMaxCorner(glm::ivec4(int(m_imgPack->imgInfo().width) - 1,
                              int(m_imgPack->imgInfo().height) - 1,
                              int(m_imgPack->imgInfo().depth) - 1,
                              int(m_imgPack->imgInfo().numTimes) - 1));
  updateBoundBoxWithOffsetPara(res);
  return res;
}

int ZImgFilter::imgSlice() const
{
  return realZ();
}

int ZImgFilter::imgTime() const
{
  return realT();
}

std::shared_ptr<ZWidgetsGroup> ZImgFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>(m_imgPack->name(), 1);
  }
  updateViewSettingWidgetsGroup();
  return m_widgetsGroup;
}

std::optional<uint64_t> ZImgFilter::cachedNeuroglancerSegmentationIdAtScenePos(const QPointF& scenePos) const
{
  if (!m_isVisible || !m_imgPack || !m_imgPack->isNeuroglancerPrecomputed()) {
    return std::nullopt;
  }
  std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol = m_imgPack->neuroglancerVolumeShared();
  if (!vol || !vol->isSegmentation() || !vol->hasMeshDirectory()) {
    return std::nullopt;
  }
  if (m_view.isMaxZProjView()) {
    return std::nullopt;
  }

  const QPointF p = mapFromScene(scenePos);
  const int64_t lx = static_cast<int64_t>(std::floor(p.x()));
  const int64_t ly = static_cast<int64_t>(std::floor(p.y()));
  const int64_t lz = static_cast<int64_t>(imgSlice());

  const auto& info = m_imgPack->imgInfo();
  const bool inBounds = (lx >= 0 && ly >= 0 && lz >= 0) && (static_cast<size_t>(lx) < info.width) &&
                        (static_cast<size_t>(ly) < info.height) && (static_cast<size_t>(lz) < info.depth);
  if (!inBounds) {
    return std::nullopt;
  }

  return m_imgPack->tryGetCachedNeuroglancerSegmentationId(static_cast<size_t>(lx),
                                                           static_cast<size_t>(ly),
                                                           static_cast<size_t>(lz));
}

void ZImgFilter::viewPrecedenceChanged()
{
  if (m_item) {
    m_item->setZValue(m_viewPrecedencePara.get());
  }
  if (m_scaleBarItem) {
    m_scaleBarItem->setZValue(30000);
  }
  ZObjFilter::viewPrecedenceChanged();
}

void ZImgFilter::transformChanged()
{
  if (m_item) {
    m_item->setTransform(getQTransform());
    if (m_scaleBarItem) {
      m_scaleBarItem->setTransformScale(getTransformScale().x);
    }
    viewportChanged();
  }
  ZObjFilter::transformChanged();
}

void ZImgFilter::offsetChanged()
{
  m_displayValid = false;
  if (m_view.isMaxZProjView()) {
    setMaxZProjView(m_view.currentTime());
  } else {
    setNormalView(m_view.currentSlice(), m_view.currentTime());
  }
  ZObjFilter::offsetChanged();
}

void ZImgFilter::updateViewSettingWidgetsGroup()
{
  if (m_widgetsGroup) {
    m_widgetsGroup->removeAllChildren();

    m_widgetsGroup->addChild(m_visible, 1);

    auto pb = new QPushButton("Bring to Front");
    connect(pb, &QPushButton::clicked, this, &ZImgFilter::bringToFront);
    m_widgetsGroup->addChild(*pb, 1);

    pb = new QPushButton("Send to Back");
    connect(pb, &QPushButton::clicked, this, &ZImgFilter::sendToBack);
    m_widgetsGroup->addChild(*pb, 1);

    m_widgetsGroup->addChild(m_viewPrecedencePara, 1);
    m_widgetsGroup->addChild(m_colorizationMode, 1);

    for (size_t i = 0; i < m_channelVisibleParas.size(); ++i) {
      m_widgetsGroup->addChild(*m_channelVisibleParas[i], 1);
      m_widgetsGroup->addChild(*m_channelColorParas[i], 1);
      m_widgetsGroup->addChild(*m_doubleChannelRangeParas[i], 1);
    }

    pb = new QPushButton(ZTheme::instance().icon(ZTheme::FlipHorizontalIcon), "Flip Horizontally");
    connect(pb, &QPushButton::clicked, this, &ZImgFilter::flipHorizontally);
    m_widgetsGroup->addChild(*pb, 1);

    pb = new QPushButton(ZTheme::instance().icon(ZTheme::FlipVerticalIcon), "Flip Vertically");
    connect(pb, &QPushButton::clicked, this, &ZImgFilter::flipVertically);
    m_widgetsGroup->addChild(*pb, 1);

    m_widgetsGroup->addChild(m_transform, 1);
    m_widgetsGroup->addChild(m_offsetPara, 1);
    m_widgetsGroup->addChild(m_opacity, 1);
    m_widgetsGroup->addChild(*m_mipRange, 1);
    m_widgetsGroup->addChild(m_showScaleBar, 2);
    m_widgetsGroup->addChild(m_scaleBarLengthInUm, 2);
    m_widgetsGroup->addChild(m_scaleBarColor, 2);
    m_widgetsGroup->addChild(m_scaleBarHeight, 2);
    m_widgetsGroup->setBasicAdvancedCutoff(5);

    m_widgetsGroup->emitWidgetsGroupChangedSignal();
  }
}

void ZImgFilter::channelVisibleChanged()
{
  m_hasVisibleChannel = false;
  for (auto& channelVisiblePara : m_channelVisibleParas) {
    m_hasVisibleChannel = m_hasVisibleChannel || channelVisiblePara->get();
  }
  m_isVisible = m_hasVisibleChannel && m_visible.get() && m_sliceValid;

  if (auto para = qobject_cast<ZBoolParameter*>(sender())) {
    // find which channel send the signal
    size_t c = 0;
    for (; c < m_channelVisibleParas.size(); ++c) {
      if (m_channelVisibleParas[c].get() == para) {
        break;
      }
    }
    if (m_channelVisibleParas[c]->get()) {
      m_display->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
    } else {
      m_display->hideChannel(c);
    }
    m_displayValid = false;
  } else {
    CHECK(false);
  }
  if (!m_isVisible) {
    destroyImgItems(); // will create new one next time
  } else {
    updateImgItems();
  }

  showScaleBarChanged();
}

void ZImgFilter::channelRangeChanged()
{
  if (auto para = qobject_cast<ZDoubleSpanParameter*>(sender())) {
    // find which channel send the signal
    size_t c = 0;
    for (; c < m_doubleChannelRangeParas.size(); ++c) {
      if (m_doubleChannelRangeParas[c].get() == para) {
        break;
      }
    }
    if (m_channelVisibleParas[c]->get()) { // only redraw if this channel is visible
      m_display->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
      m_displayValid = false;
    }
  }
  if (!m_isVisible) {
    destroyImgItems(); // will create new one next time
  } else {
    updateImgItems();
  }
}

void ZImgFilter::channelColorChanged()
{
  if (auto para = qobject_cast<ZVec3Parameter*>(sender())) {
    // find which channel send the signal
    size_t c = 0;
    for (; c < m_channelColorParas.size(); ++c) {
      if (m_channelColorParas[c].get() == para) {
        break;
      }
    }
    if (m_channelVisibleParas[c]->get()) {
      // only redraw if this channel is visible
      col4 col{static_cast<uint8_t>(para->get().r * 255),
               static_cast<uint8_t>(para->get().g * 255),
               static_cast<uint8_t>(para->get().b * 255)};
      m_display->setChannelColor(c, col);
      m_displayValid = false;
    }
  } else {
    CHECK(false);
  }
  if (!m_isVisible) {
    destroyImgItems(); // will create new one next time
  } else {
    updateImgItems();
  }
}

ZImgColorizationMode ZImgFilter::currentColorizationMode() const
{
  if (m_colorizationMode.isSelected("Segmentation Labels")) {
    return ZImgColorizationMode::SegmentationLabels;
  }
  return ZImgColorizationMode::Intensity;
}

void ZImgFilter::colorizationModeChanged()
{
  const bool labelMode = currentColorizationMode() == ZImgColorizationMode::SegmentationLabels;
  if (m_imgPack) {
    for (size_t c = 0; c < m_imgPack->imgInfo().numChannels; ++c) {
      if (c < m_doubleChannelRangeParas.size()) {
        m_doubleChannelRangeParas[c]->setVisible(!labelMode);
      }
      if (c < m_channelColorParas.size()) {
        const bool showColor = !labelMode && !m_imgPack->imgInfo().isAlphaChannel(c);
        m_channelColorParas[c]->setVisible(showColor);
      }
    }
  }
  updateViewSettingWidgetsGroup();

  if (m_display) {
    m_display->setColorizationMode(currentColorizationMode());
  }

  // Mode changes should be visually immediate; for Neuroglancer this also forces a new async render epoch.
  m_displayValid = false;
  destroyImgItems();

  if (m_isVisible) {
    updateImgItems();
  }
}

void ZImgFilter::opacityChanged()
{
  // for (const auto& item : m_imgItems) {
  //   item->setOpacity(m_opacity.get());
  // }
  if (m_item) {
    m_item->setOpacity(m_opacity.get());
  }
  //  m_display->setAlpha(m_opacity.get());
  //  if (m_maxZProjDisplay)
  //    m_maxZProjDisplay->setAlpha(m_opacity.get());
  //  m_displayValid = false;
  //  if (!m_isVisible) {
  //    destroyImgItems(); // will create new one next time
  //  } else {
  //    updateImgItems();
  //  }
}

void ZImgFilter::mipRangeChanged()
{
  m_display->setMIPZRange(m_mipRange->get().x, m_mipRange->get().y);
  if (m_display->mip()) {
    m_displayValid = false;
    if (!m_isVisible) {
      destroyImgItems(); // will create new one next time
    } else {
      updateImgItems();
    }
  }
}

void ZImgFilter::visibleChanged()
{
  bool isVisibleBefore = m_isVisible;
  m_isVisible = m_hasVisibleChannel && m_visible.get() && m_sliceValid;
  if (m_isVisible && !isVisibleBefore) {
    updateImgItems();
  } else if (!m_isVisible && isVisibleBefore) {
    hideImgItems();
  }

  showScaleBarChanged();
}

void ZImgFilter::hideImgItems()
{
  // for (size_t i = 0; i < m_imgItems.size(); ++i) {
  //   m_imgItems[i]->setVisible(false);
  // }
  if (m_item) {
    m_item->setVisible(false);
  }

  updateNeuroglancerLoadingIndicator();
}

void ZImgFilter::destroyImgItems()
{
  m_item.reset();
  m_imgItems.clear();
  m_ngTiles.clear();
  m_ngTileApplyState.clear();
  m_ngLastAppliedFinalPass = false;
}

void ZImgFilter::updateNeuroglancerLoadingIndicator()
{
  const bool ngActive = m_isVisible && m_imgPack && m_imgPack->isNeuroglancerPrecomputed() && !m_view.isMaxZProjView();
  const bool cacheBusy = m_ngCacheInFlight && m_ngCacheInFlightEpoch == m_ngRenderEpoch;
  const bool previewBusy = m_ngPreviewInFlight && m_ngPreviewInFlightEpoch == m_ngRenderEpoch;
  // Treat the debounce timer as "pending refinement" to reassure the user while interacting.
  const bool finalBusy = (m_ngFinalInFlight && m_ngFinalInFlightEpoch == m_ngRenderEpoch) || m_ngFinalPending || m_ngRefineTimer.isActive();

  // If we already finished the final pass for this epoch, the displayed imagery should be sharp and complete.
  // Hide the badge even if earlier background passes (cache/preview) are still winding down.
  const bool finalDone = m_ngFinalCompletedEpoch == m_ngRenderEpoch;

  const bool shouldShow = ngActive && !finalDone && (cacheBusy || previewBusy || finalBusy);
  if (!shouldShow) {
    if (m_ngLoadingBadgeBg) {
      m_ngLoadingBadgeBg->setVisible(false);
    }
    return;
  }

  if (!m_ngLoadingBadgeBg) {
    m_ngLoadingBadgeBg = std::make_unique<QGraphicsRectItem>();
    m_ngLoadingBadgeBg->setZValue(40000);
    m_ngLoadingBadgeBg->setPen(QPen(QColor(255, 255, 255, 60)));
    m_ngLoadingBadgeBg->setBrush(QBrush(QColor(0, 0, 0, 160)));
    m_ngLoadingBadgeBg->setAcceptedMouseButtons(Qt::NoButton);
    m_ngLoadingBadgeBg->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_view.scene().addItem(m_ngLoadingBadgeBg.get());

    m_ngLoadingBadgeText = std::make_unique<QGraphicsSimpleTextItem>(m_ngLoadingBadgeBg.get());
    m_ngLoadingBadgeText->setBrush(QBrush(QColor(255, 255, 255, 230)));
    m_ngLoadingBadgeText->setPen(Qt::NoPen);
    m_ngLoadingBadgeText->setAcceptedMouseButtons(Qt::NoButton);
    m_ngLoadingBadgeText->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
  }

  QString text;
  if (finalBusy) {
    // If we already show a coarse preview, this reassures the user that a sharper pass is coming.
    text = m_ngLastAppliedFinalPass ? QStringLiteral("Loading…") : QStringLiteral("Refining…");
  } else {
    text = QStringLiteral("Loading…");
  }

  m_ngLoadingBadgeText->setText(text);

  constexpr qreal kPadPx = 6.0;
  constexpr qreal kMarginPx = 10.0;

  const QRectF textRect = m_ngLoadingBadgeText->boundingRect();
  m_ngLoadingBadgeText->setPos(kPadPx, kPadPx);
  m_ngLoadingBadgeBg->setRect(0, 0, textRect.width() + 2 * kPadPx, textRect.height() + 2 * kPadPx);

  const double viewScale = m_view.currentScale();
  CHECK(viewScale > 0);
  const qreal marginScene = kMarginPx / static_cast<qreal>(viewScale);
  const QPointF anchor = m_view.currentViewport().topLeft() + QPointF(marginScene, marginScene);
  m_ngLoadingBadgeBg->setPos(anchor);
  m_ngLoadingBadgeBg->setVisible(true);
}

void ZImgFilter::applyQImagePack(const ZQImagePack& qImagePack, uint64_t epoch, int passPriority, bool isFinalPass)
{
  if (qImagePack.numImages() == 0) {
    return;
  }

  if (!m_imgPack || !m_imgPack->isNeuroglancerPrecomputed() || m_view.isMaxZProjView()) {
    // Non-network images (or MIP) keep the simpler synchronous path.
    destroyImgItems();
    m_item = std::make_unique<ZGraphicsItemGroup>();
    m_view.scene().addItem(m_item.get());
    for (size_t i = 0; i < qImagePack.numImages(); ++i) {
      m_imgItems.push_back(new QGraphicsPixmapItem(QPixmap::fromImage(qImagePack.image(i))));
      m_imgItems[i]->setScale(qImagePack.scale(i));
      m_imgItems[i]->setPos(QPointF(qImagePack.location(i)));
      m_imgItems[i]->setOpacity(1.0);
      m_imgItems[i]->setVisible(m_isVisible);
      m_item->addToGroup(m_imgItems[i]);
    }
    m_item->setZValue(m_viewPrecedencePara.get());
    m_item->setTransform(getQTransform());
    m_item->setOpacity(m_opacity.get());
    m_item->setVisible(m_isVisible);
    return;
  }

  // Neuroglancer: keep already-rendered tiles visible and progressively overwrite with newer tiles.
  if (!m_item) {
    m_item = std::make_unique<ZGraphicsItemGroup>();
    m_view.scene().addItem(m_item.get());
  }

  m_item->setZValue(m_viewPrecedencePara.get());
  m_item->setTransform(getQTransform());
  m_item->setOpacity(m_opacity.get());
  m_item->setVisible(m_isVisible);

  auto makeKey = [](const QPoint& loc, double scale, const QImage& img) -> ZImgFilter::NgTileKey {
    const long long scaleRounded = std::llround(scale);
    const double err = std::abs(scale - static_cast<double>(scaleRounded));
    CHECK(err <= 1e-6) << "Neuroglancer 2D tile scale is not integral: " << scale;
    if (scaleRounded < 1) {
      return {loc.x(), loc.y(), static_cast<size_t>(1), img.width(), img.height()};
    }
    return {loc.x(), loc.y(), static_cast<size_t>(scaleRounded), img.width(), img.height()};
  };

  bool appliedAny = false;
  for (size_t i = 0; i < qImagePack.numImages(); ++i) {
    const QPoint loc = qImagePack.location(i);
    const double scale = qImagePack.scale(i);
    const QImage& img = qImagePack.image(i);

    const NgTileKey key = makeKey(loc, scale, img);
    auto& st = m_ngTileApplyState[key];
    if (st.epoch != epoch) {
      st.epoch = epoch;
      st.passPriority = -1;
    }
    if (passPriority < st.passPriority) {
      continue;
    }
    st.passPriority = passPriority;
    QGraphicsPixmapItem* item = nullptr;
    if (auto it = m_ngTiles.find(key); it != m_ngTiles.end()) {
      item = it->second;
    } else {
      item = static_cast<QGraphicsPixmapItem*>(new ZOverwritePixmapItem());
      m_ngTiles.emplace(key, item);
      m_item->addToGroup(item);
    }

    item->setPixmap(QPixmap::fromImage(img));
    item->setScale(scale);
    item->setPos(QPointF(loc));
    item->setOpacity(1.0);
    item->setVisible(m_isVisible);

    const qreal subZ = static_cast<qreal>(1.0 / std::max(1e-12, scale));
    item->setZValue(subZ);
    appliedAny = true;
  }

  if (appliedAny && isFinalPass) {
    m_ngLastAppliedFinalPass = true;
  }
}

void ZImgFilter::trimNeuroglancerTilesAfterFinal(uint64_t epoch)
{
  if (!m_item) {
    return;
  }

  const int finalPrio = neuroglancer2DPassPriority(Neuroglancer2DRenderParams::Pass::Final);

  std::vector<NgTileKey> toDelete;
  toDelete.reserve(m_ngTiles.size());
  for (const auto& it : m_ngTiles) {
    const NgTileKey& key = it.first;
    auto stIt = m_ngTileApplyState.find(key);
    if (stIt == m_ngTileApplyState.end()) {
      toDelete.push_back(key);
      continue;
    }

    const NgTileApplyState& st = stIt->second;
    if (st.epoch != epoch || st.passPriority < finalPrio) {
      toDelete.push_back(key);
    }
  }

  for (const NgTileKey& key : toDelete) {
    auto it = m_ngTiles.find(key);
    if (it == m_ngTiles.end()) {
      continue;
    }
    QGraphicsPixmapItem* item = it->second;
    m_ngTiles.erase(it);
    m_ngTileApplyState.erase(key);
    delete item;
  }
}

void ZImgFilter::prepare2DExportFrame()
{
  const bool ngActive = m_isVisible && m_imgPack && m_imgPack->isNeuroglancerPrecomputed() && !m_view.isMaxZProjView();
  if (!ngActive) {
    return;
  }

  if (m_ngFinalCompletedEpoch == m_ngRenderEpoch) {
    return;
  }

  // For deterministic export we don't want to wait for the debounce; force a final pass immediately.
  m_ngRefineTimer.stop();
  if (!m_ngFinalInFlight || m_ngFinalInFlightEpoch != m_ngRenderEpoch) {
    startNeuroglancer2DRender(/*finalPass=*/true);
  }
}

bool ZImgFilter::is2DExportFrameReady(QString* errorMsg) const
{
  if (errorMsg) {
    errorMsg->clear();
  }

  const bool ngActive = m_isVisible && m_imgPack && m_imgPack->isNeuroglancerPrecomputed() && !m_view.isMaxZProjView();
  if (!ngActive) {
    return true;
  }

  if (m_ngFinalErrorEpoch == m_ngRenderEpoch) {
    if (errorMsg) {
      *errorMsg = m_ngFinalError;
    }
    return false;
  }

  if (m_ngFinalCompletedEpoch != m_ngRenderEpoch) {
    return false;
  }

  // Defensive: if something is still in-flight/pending for this epoch, don't claim readiness.
  const bool finalBusy = (m_ngFinalInFlight && m_ngFinalInFlightEpoch == m_ngRenderEpoch) || m_ngFinalPending || m_ngRefineTimer.isActive();
  return !finalBusy;
}

void ZImgFilter::requestNeuroglancer2DRender()
{
  // Bump the epoch so any in-flight renders (cache/preview/final) become stale.
  ++m_ngRenderEpoch;
  if (m_ngRenderEpochAtomic) {
    m_ngRenderEpochAtomic->store(m_ngRenderEpoch, std::memory_order_relaxed);
  }
  m_ngFinalPending = false;
  m_ngLastAppliedFinalPass = false;
  m_ngFinalCompletedEpoch = std::numeric_limits<uint64_t>::max();
  m_ngFinalErrorEpoch = std::numeric_limits<uint64_t>::max();
  m_ngFinalError.clear();

  const QRectF viewport = mapFromSceneRect(m_view.currentViewport());
  const double viewScale = effectivePixelScaleForLod(m_view.graphicsView(), getTransformScale());
  const bool finalFullyCached = isNeuroglancer2DViewportFullyCachedAtFinalLod(viewport, viewScale);

  // First, show the best already-cached coverage immediately (no network).
  m_ngCacheDirty = true;
  if (!m_ngCacheInFlight) {
    startNeuroglancer2DCacheRender();
  }

  // Schedule a coarse preview render immediately (at most one in flight).
  bool previewNeeded = false;
  if (!finalFullyCached) {
    previewNeeded = true;
    if (m_imgPack && m_imgPack->isNeuroglancerPrecomputed() && m_display) {
      previewNeeded = !m_imgPack->neuroglancerVolumeShared()->is2DViewportFullyCachedForCoarsestXY(m_display->slice(), m_display->time(),
                                                                                                   viewport);
    }
  }

  m_ngPreviewDirty = previewNeeded;
  if (previewNeeded && !m_ngPreviewInFlight) {
    startNeuroglancer2DRender(/*finalPass=*/false);
  }

  // Debounce the final render so we only sharpen once interaction settles.
  if (!finalFullyCached) {
    m_ngRefineTimer.start();
  } else {
    m_ngRefineTimer.stop();
    m_ngFinalPending = false;
  }

  updateNeuroglancerLoadingIndicator();
}

bool ZImgFilter::isNeuroglancer2DViewportFullyCachedAtFinalLod(const QRectF& viewport, double viewScale) const
{
  if (!m_imgPack || !m_imgPack->isNeuroglancerPrecomputed()) {
    return false;
  }
  if (!m_display) {
    return false;
  }

  std::shared_ptr<ZNeuroglancerPrecomputedVolume> volume = m_imgPack->neuroglancerVolumeShared();
  if (!volume) {
    return false;
  }

  try {
    const auto requests = volume->sliceChunkRequestsFor2DViewport(m_display->slice(),
                                                                  m_display->time(),
                                                                  viewport,
                                                                  viewScale,
                                                                  ZNeuroglancerPrecomputedVolume::Slice2DRatioPolicy::BestForScale);

    for (const auto& c : requests.chunks) {
      if (!volume->tryGetCachedChunk(c)) {
        return false;
      }
    }
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

void ZImgFilter::startNeuroglancer2DCacheRender()
{
  CHECK(m_imgPack);
  CHECK(m_display);
  CHECK(m_imgPack->isNeuroglancerPrecomputed());
  CHECK(!m_view.isMaxZProjView());

  if (m_ngCacheInFlight && m_ngCacheInFlightEpoch == m_ngRenderEpoch) {
    m_ngCacheDirty = true;
    return;
  }

  m_ngCacheDirty = false;
  m_ngCacheInFlight = true;
  m_ngCacheInFlightEpoch = m_ngRenderEpoch;

  updateNeuroglancerLoadingIndicator();

  const uint64_t epoch = m_ngRenderEpoch;

  // Capture all Qt/engine state on the UI thread, then build QImages from cached chunks off-thread.
  Neuroglancer2DRenderParams params;
  params.volume = m_imgPack->neuroglancerVolumeShared();
  params.imgInfo = m_imgPack->imgInfo();
  params.viewport = mapFromSceneRect(m_view.currentViewport());
  params.z = m_display->slice();
  params.t = m_display->time();

  const double viewScale = effectivePixelScaleForLod(m_view.graphicsView(), getTransformScale());
  const bool treatAsFinal = isNeuroglancer2DViewportFullyCachedAtFinalLod(params.viewport, viewScale);
  params.renderScale = treatAsFinal ? viewScale : std::min(viewScale, kNg2DCacheRenderScaleForRatioSelection);
  params.pass = Neuroglancer2DRenderParams::Pass::CacheOnly;
  params.colorizationMode = currentColorizationMode();

  // Channel configuration (copy-only types; safe to move across threads).
  for (size_t c = 0; c < m_imgPack->imgInfo().numChannels; ++c) {
    if (!m_channelVisibleParas[c]->get()) {
      continue;
    }
    params.channels.emplace(c, std::make_pair(getLowerChannelRange(c), getUpperChannelRange(c)));

    if (m_imgPack->imgInfo().isAlphaChannel(c)) {
      params.channelColors.emplace(c, col4{255, 255, 255, 255});
    } else {
      const auto rgb = m_channelColorParas[c]->get();
      params.channelColors.emplace(c,
                                  col4{static_cast<uint8_t>(rgb.r * 255),
                                       static_cast<uint8_t>(rgb.g * 255),
                                       static_cast<uint8_t>(rgb.b * 255),
                                       255});
    }
  }

  auto* watcher = new QFutureWatcher<Neuroglancer2DRenderResult>(this);
  connect(watcher, &QFutureWatcher<Neuroglancer2DRenderResult>::finished, this, [this, watcher, treatAsFinal]() {
    const Neuroglancer2DRenderResult result = watcher->result();
    watcher->deleteLater();

    if (m_ngCacheInFlight && result.epoch == m_ngCacheInFlightEpoch) {
      m_ngCacheInFlight = false;
      m_ngCacheInFlightEpoch = 0;
    }

    if (!result.error.isEmpty()) {
      LOG(ERROR) << "Neuroglancer 2D cache render failed: " << result.error.toStdString();
      if (treatAsFinal && result.epoch == m_ngRenderEpoch && m_ngFinalErrorEpoch != result.epoch) {
        m_ngFinalErrorEpoch = result.epoch;
        m_ngFinalError = QString("Neuroglancer 2D cached-final render failed: %1").arg(result.error);
      }
    } else if (m_isVisible && result.epoch == m_ngRenderEpoch) {
      const auto applyPass = treatAsFinal ? Neuroglancer2DRenderParams::Pass::Final : result.pass;
      const int prio = neuroglancer2DPassPriority(applyPass);
      applyQImagePack(result.qImagePack, result.epoch, prio, /*isFinalPass=*/treatAsFinal);
      if (treatAsFinal) {
        m_ngFinalCompletedEpoch = result.epoch;
        trimNeuroglancerTilesAfterFinal(result.epoch);
      }
    }

    updateNeuroglancerLoadingIndicator();

    if (m_ngCacheDirty && !m_ngCacheInFlight && m_isVisible) {
      startNeuroglancer2DCacheRender();
    }
  });

  watcher->setFuture(QtConcurrent::run([params = std::move(params), epoch]() mutable {
    return renderNeuroglancer2D(std::move(params), epoch);
  }));
}

void ZImgFilter::startNeuroglancer2DRender(bool finalPass)
{
  CHECK(m_imgPack);
  CHECK(m_display);
  CHECK(m_imgPack->isNeuroglancerPrecomputed());
  CHECK(!m_view.isMaxZProjView());

  const uint64_t epoch = m_ngRenderEpoch;
  const auto pass = finalPass ? Neuroglancer2DRenderParams::Pass::Final : Neuroglancer2DRenderParams::Pass::Preview;

  if (finalPass) {
    // If a final pass for the current epoch is already running, just mark it pending.
    // (If a stale epoch is running, we let a newer final render start; results are epoch-gated.)
    if (m_ngFinalInFlight && m_ngFinalInFlightEpoch == epoch) {
      m_ngFinalPending = true;
      return;
    }
    m_ngFinalInFlight = true;
    m_ngFinalInFlightEpoch = epoch;
    m_ngFinalPending = false;
  } else {
    // If a preview pass for the current epoch is already running, just mark it dirty.
    // (If a stale epoch is running, we let a newer preview render start; results are epoch-gated.)
    if (m_ngPreviewInFlight && m_ngPreviewInFlightEpoch == epoch) {
      m_ngPreviewDirty = true;
      return;
    }
    m_ngPreviewDirty = false;
    m_ngPreviewInFlight = true;
    m_ngPreviewInFlightEpoch = epoch;
  }

  updateNeuroglancerLoadingIndicator();

  const QRectF viewport = mapFromSceneRect(m_view.currentViewport());
  const double viewScale = effectivePixelScaleForLod(m_view.graphicsView(), getTransformScale());
  const auto ratioPolicy = (pass == Neuroglancer2DRenderParams::Pass::Preview)
                             ? ZNeuroglancerPrecomputedVolume::Slice2DRatioPolicy::CoarsestXY
                             : ZNeuroglancerPrecomputedVolume::Slice2DRatioPolicy::BestForScale;

  std::shared_ptr<ZNeuroglancerPrecomputedVolume> volume = m_imgPack->neuroglancerVolumeShared();
  CHECK(volume);

  ZNeuroglancerPrecomputedVolume::SliceChunkRequests requests;
  try {
    requests = volume->sliceChunkRequestsFor2DViewport(m_display->slice(), m_display->time(), viewport, viewScale, ratioPolicy);
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Neuroglancer 2D render setup failed: " << e.what();
    if (pass == Neuroglancer2DRenderParams::Pass::Final) {
      m_ngFinalErrorEpoch = epoch;
      m_ngFinalError = QString("Neuroglancer 2D final render setup failed: %1").arg(QString::fromUtf8(e.what()));
    }
    if (pass == Neuroglancer2DRenderParams::Pass::Final) {
      if (m_ngFinalInFlight && m_ngFinalInFlightEpoch == epoch) {
        m_ngFinalInFlight = false;
        m_ngFinalInFlightEpoch = 0;
      }
    } else {
      if (m_ngPreviewInFlight && m_ngPreviewInFlightEpoch == epoch) {
        m_ngPreviewInFlight = false;
        m_ngPreviewInFlightEpoch = 0;
      }
    }
    updateNeuroglancerLoadingIndicator();
    return;
  }

  if (requests.chunks.empty()) {
    if (pass == Neuroglancer2DRenderParams::Pass::Final) {
      m_ngFinalCompletedEpoch = epoch;
      if (m_ngFinalInFlight && m_ngFinalInFlightEpoch == epoch) {
        m_ngFinalInFlight = false;
        m_ngFinalInFlightEpoch = 0;
      }
    } else {
      if (m_ngPreviewInFlight && m_ngPreviewInFlightEpoch == epoch) {
        m_ngPreviewInFlight = false;
        m_ngPreviewInFlightEpoch = 0;
      }
    }
    updateNeuroglancerLoadingIndicator();
    return;
  }

  auto shared = std::make_shared<Neuroglancer2DTileSharedParams>();
  shared->volume = std::move(volume);
  shared->imgInfo = m_imgPack->imgInfo();
  shared->z = m_display->slice();
  shared->t = m_display->time();
  shared->targetRatio = requests.targetRatio;
  shared->ratioZ = static_cast<int64_t>(requests.targetRatio[2]);
  CHECK(shared->ratioZ > 0);
  shared->tileScale = static_cast<double>(requests.targetRatio[0]);
  shared->epoch = epoch;
  shared->epochAtomic = m_ngRenderEpochAtomic;
  shared->pass = pass;
  shared->colorizationMode = currentColorizationMode();

  // Channel configuration (copy-only types; safe to move across threads).
  for (size_t c = 0; c < m_imgPack->imgInfo().numChannels; ++c) {
    if (!m_channelVisibleParas[c]->get()) {
      continue;
    }
    shared->channels.emplace(c, std::make_pair(getLowerChannelRange(c), getUpperChannelRange(c)));

    if (m_imgPack->imgInfo().isAlphaChannel(c)) {
      shared->channelColors.emplace(c, col4{255, 255, 255, 255});
    } else {
      const auto rgb = m_channelColorParas[c]->get();
      shared->channelColors.emplace(c,
                                   col4{static_cast<uint8_t>(rgb.r * 255),
                                        static_cast<uint8_t>(rgb.g * 255),
                                        static_cast<uint8_t>(rgb.b * 255),
                                        255});
    }
  }

  const QPointF vpCenter = viewport.center();
  const double cx = vpCenter.x();
  const double cy = vpCenter.y();

  std::vector<ZNeuroglancerPrecomputedVolume::Chunk> chunks = std::move(requests.chunks);
  std::sort(chunks.begin(), chunks.end(), [&](const auto& a, const auto& b) {
    const double ax = 0.5 * static_cast<double>(a.baseStart[0] + a.baseEnd[0]);
    const double ay = 0.5 * static_cast<double>(a.baseStart[1] + a.baseEnd[1]);
    const double bx = 0.5 * static_cast<double>(b.baseStart[0] + b.baseEnd[0]);
    const double by = 0.5 * static_cast<double>(b.baseStart[1] + b.baseEnd[1]);
    const double da = (ax - cx) * (ax - cx) + (ay - cy) * (ay - cy);
    const double db = (bx - cx) * (bx - cx) + (by - cy) * (by - cy);
    if (std::abs(da - db) > 1e-12) {
      return da < db; // center-first
    }
    return a.baseStart < b.baseStart;
  });

  auto toIntChecked = [](int64_t v) -> int {
    if (v < static_cast<int64_t>(std::numeric_limits<int>::min()) || v > static_cast<int64_t>(std::numeric_limits<int>::max())) {
      throw ZException(fmt::format("Neuroglancer chunk coordinate {} is out of QPoint range", v));
    }
    return static_cast<int>(v);
  };

  auto tasks = std::make_shared<std::vector<Neuroglancer2DTileTask>>();
  tasks->reserve(chunks.size());
  for (const auto& chunk : chunks) {
    Neuroglancer2DTileTask t{};
    t.shared = shared;
    t.chunk = chunk;
    t.loc = QPoint(toIntChecked(chunk.baseStart[0]), toIntChecked(chunk.baseStart[1]));
    tasks->push_back(std::move(t));
  }

  auto* watcher = new QFutureWatcher<Neuroglancer2DTileResult>(this);
  connect(watcher, &QFutureWatcher<Neuroglancer2DTileResult>::resultReadyAt, this, [this, watcher](int index) {
    const Neuroglancer2DTileResult result = watcher->resultAt(index);
    if (!result.error.isEmpty()) {
      LOG(ERROR) << "Neuroglancer 2D tile render failed: " << result.error.toStdString();
      if (result.pass == Neuroglancer2DRenderParams::Pass::Final && result.epoch == m_ngRenderEpoch && m_ngFinalErrorEpoch != result.epoch) {
        m_ngFinalErrorEpoch = result.epoch;
        m_ngFinalError = QString("Neuroglancer 2D final tile render failed: %1").arg(result.error);
      }
      return;
    }
    if (!m_isVisible || result.epoch != m_ngRenderEpoch) {
      return;
    }
    const int prio = neuroglancer2DPassPriority(result.pass);
    applyQImagePack(result.qImagePack, result.epoch, prio, /*isFinalPass=*/result.pass == Neuroglancer2DRenderParams::Pass::Final);
  });

  connect(watcher, &QFutureWatcher<Neuroglancer2DTileResult>::finished, this, [this, watcher, tasks, pass, epoch]() {
    // Ensure the final on-screen state includes all results even if some resultReadyAt notifications are still queued.
    for (int i = 0; i < static_cast<int>(tasks->size()); ++i) {
      const Neuroglancer2DTileResult result = watcher->resultAt(i);
      if (!result.error.isEmpty()) {
        if (result.pass == Neuroglancer2DRenderParams::Pass::Final && result.epoch == m_ngRenderEpoch &&
            m_ngFinalErrorEpoch != result.epoch) {
          m_ngFinalErrorEpoch = result.epoch;
          m_ngFinalError = QString("Neuroglancer 2D final tile render failed: %1").arg(result.error);
        }
        continue;
      }
      if (!m_isVisible || result.epoch != m_ngRenderEpoch) {
        continue;
      }
      const int prio = neuroglancer2DPassPriority(result.pass);
      applyQImagePack(result.qImagePack, result.epoch, prio, /*isFinalPass=*/result.pass == Neuroglancer2DRenderParams::Pass::Final);
    }

    watcher->deleteLater();

    if (pass == Neuroglancer2DRenderParams::Pass::Final) {
      if (epoch == m_ngRenderEpoch) {
        m_ngFinalCompletedEpoch = epoch;
      }
      if (m_ngFinalInFlight && m_ngFinalInFlightEpoch == epoch) {
        m_ngFinalInFlight = false;
        m_ngFinalInFlightEpoch = 0;
      }
      if (epoch == m_ngRenderEpoch) {
        // Once we have a complete final pass for the current epoch, drop any cached/preview/stale tiles.
        // Keeping older passes is useful for opaque rendering, but with transparency (e.g. overlaying
        // segmentation) it causes coarse tiles to show through and appear "blocky" at chunk boundaries.
        trimNeuroglancerTilesAfterFinal(epoch);
      }
    } else if (pass == Neuroglancer2DRenderParams::Pass::Preview) {
      if (m_ngPreviewInFlight && m_ngPreviewInFlightEpoch == epoch) {
        m_ngPreviewInFlight = false;
        m_ngPreviewInFlightEpoch = 0;
      }
    }

    updateNeuroglancerLoadingIndicator();

    if (pass == Neuroglancer2DRenderParams::Pass::Preview) {
      if (m_ngPreviewDirty && (!m_ngPreviewInFlight || m_ngPreviewInFlightEpoch != m_ngRenderEpoch) && m_isVisible) {
        startNeuroglancer2DRender(/*finalPass=*/false);
      }
    } else if (pass == Neuroglancer2DRenderParams::Pass::Final) {
      if (m_ngFinalPending && (!m_ngFinalInFlight || m_ngFinalInFlightEpoch != m_ngRenderEpoch) && m_isVisible) {
        m_ngFinalPending = false;
        startNeuroglancer2DRender(/*finalPass=*/true);
      }
    }
  });

  watcher->setFuture(QtConcurrent::mapped(*tasks, renderNeuroglancer2DTile));
}

void ZImgFilter::updateImgItems()
{
  CHECK(m_isVisible);

  // VLOG(1) << curDisplay->slice() << " " << m_lastSlice << " " << m_engine.currentSlice();
  if (m_imgPack && m_imgPack->isNeuroglancerPrecomputed() && !m_view.isMaxZProjView()) {
    // Neuroglancer is network-backed; never block the UI thread for tile fetch/decode.
    // Keep the previous tiles visible while we fetch/compose in the background.

    m_display->setScale(effectivePixelScaleForLod(m_view.graphicsView(), getTransformScale()));
    QRectF vp = m_view.currentViewport();
    m_display->setViewport(mapFromSceneRect(vp));

    // If slice/time changed, the old imagery is misleading; drop it immediately.
    const bool sliceOrTimeChanged = (m_lastSlice != m_view.currentSlice()) || (m_lastTime != m_view.currentTime()) ||
                                   (m_lastMIP != m_view.isMaxZProjView());
    if (sliceOrTimeChanged) {
      destroyImgItems();
    } else if (!m_item) {
      // Ensure the group exists so transforms/opacities apply consistently.
      m_item = std::make_unique<ZGraphicsItemGroup>();
      m_view.scene().addItem(m_item.get());
      m_item->setZValue(m_viewPrecedencePara.get());
      m_item->setTransform(getQTransform());
      m_item->setOpacity(m_opacity.get());
      m_item->setVisible(m_isVisible);
    }

    m_lastMIP = m_display->mip();
    m_lastSlice = m_display->slice();
    m_lastTime = m_display->time();
    m_lastScale = m_display->scale();
    m_lastViewport = m_display->viewport();

    requestNeuroglancer2DRender();
    return;
  }

  if (!m_imgItems.empty() && m_displayValid && m_lastMIP == m_view.isMaxZProjView() &&
      m_lastSlice == m_view.currentSlice() && m_lastTime == m_view.currentTime()) {
    // VLOG(1) << "0";
    //  pixmap is same, we only need to show it
    if (m_item && !m_item->isVisible()) {
      m_item->setVisible(true);
    }
  } else {
    destroyImgItems();

    m_display->setScale(effectivePixelScaleForLod(m_view.graphicsView(), getTransformScale()));
    QRectF vp = m_view.currentViewport();
    m_display->setViewport(mapFromSceneRect(vp));

    m_item = std::make_unique<ZGraphicsItemGroup>();
    m_view.scene().addItem(m_item.get());
    const ZQImagePack& qImagePack = m_display->toQImagePack();
    for (size_t i = 0; i < qImagePack.numImages(); ++i) {
      m_imgItems.push_back(new QGraphicsPixmapItem(QPixmap::fromImage(qImagePack.image(i))));
      // m_imgItems[i]->setFlag(QGraphicsItem::ItemIsSelectable, true);
      m_imgItems[i]->setScale(qImagePack.scale(i));
      m_imgItems[i]->setPos(QPointF(qImagePack.location(i)));
      m_imgItems[i]->setOpacity(1.0);
      m_imgItems[i]->setVisible(m_isVisible);
      m_item->addToGroup(m_imgItems[i]);
    }
    m_item->setZValue(m_viewPrecedencePara.get());
    m_item->setTransform(getQTransform());
    m_item->setOpacity(m_opacity.get());

    m_lastMIP = m_display->mip();
    m_lastSlice = m_display->slice();
    m_lastTime = m_display->time();
    m_lastScale = m_display->scale();
    m_lastViewport = m_display->viewport();
  }
  m_displayValid = true;
}

double ZImgFilter::getLowerChannelRange(size_t c) const
{
  return m_doubleChannelRangeParas[c]->get().x;
}

double ZImgFilter::getUpperChannelRange(size_t c) const
{
  return m_doubleChannelRangeParas[c]->get().y;
}

void ZImgFilter::viewportChanged()
{
  if (QRectF vp = mapFromSceneRect(m_view.currentViewport());
      m_imgPack->needUpdate(vp,
                            effectivePixelScaleForLod(m_view.graphicsView(), getTransformScale()),
                            m_lastViewport,
                            m_lastScale,
                            realT(),
                            realZ(),
                            m_view.isMaxZProjView())) {
    if (!m_isVisible) {
      destroyImgItems(); // will create new one next time
    } else {
      m_displayValid = false;
      updateImgItems();
    }
  }
  if (m_scaleBarItem) {
    m_scaleBarItem->setViewPort(m_view.currentViewport());
  }

  updateNeuroglancerLoadingIndicator();
}

void ZImgFilter::flipHorizontally()
{
  if (m_item && m_item->isVisible()) {
    m_transform.flipHorizontally(QRectF(0, 0, m_imgPack->imgInfo().width, m_imgPack->imgInfo().height));
  }
}

void ZImgFilter::flipVertically()
{
  if (m_item && m_item->isVisible()) {
    m_transform.flipVertically(QRectF(0, 0, m_imgPack->imgInfo().width, m_imgPack->imgInfo().height));
  }
}

void ZImgFilter::showScaleBarChanged()
{
  if (m_scaleBarItem) {
    m_scaleBarItem->setVisible(m_isVisible & m_showScaleBar.get());
  }
}

void ZImgFilter::scaleBarLengthChanged()
{
  if (m_scaleBarItem) {
    m_scaleBarItem->setLengthInUm(m_scaleBarLengthInUm.get());
  }
}

void ZImgFilter::scaleBarHeightChanged()
{
  if (m_scaleBarItem) {
    m_scaleBarItem->setHeight(m_scaleBarHeight.get());
  }
}

void ZImgFilter::scaleBarColorChanged()
{
  if (m_scaleBarItem) {
    m_scaleBarItem->setColor(m_scaleBarColor.get());
  }
}

void ZImgFilter::viewScaleChanged(double s)
{
  if (m_scaleBarItem) {
    m_scaleBarItem->setViewScale(s);
  }

  updateNeuroglancerLoadingIndicator();
}

} // namespace nim
