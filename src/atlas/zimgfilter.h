#pragma once

#include "zobjfilter.h"
#include "zimgpackdisplay.h"
#include "znumericparameter.h"
#include "zparameter.h"
#include "zgraphicsitemtype.h"
#include "zgraphicsitemgroup.h"
#include <QGraphicsPixmapItem>
#include <QGraphicsSimpleTextItem>
#include <QPen>
#include <QTimer>
#include <atomic>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace nim {

class ZDoubleSpanParameter;

class ZImgScaleBarGraphicsItem : public QGraphicsRectItem
{
public:
  enum
  {
    Type = GraphicsItemType::ZImgScaleBarGraphicsItem
  };

  [[nodiscard]] int type() const override
  {
    return Type;
  }

  explicit ZImgScaleBarGraphicsItem(double lengthInUm,
                                    double height,
                                    double voxelSizeXInUm,
                                    double viewScale,
                                    double transformScale,
                                    const QRectF& viewPort,
                                    const glm::vec3& color,
                                    QGraphicsItem* parent = nullptr);

  void setLengthInUm(double l)
  {
    m_lengthInUm = l;
    setToolTip(QString("length: %1 µm (voxel size: %2 µm)").arg(m_lengthInUm).arg(m_voxelSizeXInUm));
    updateRectSize();
  }

  void setHeight(double h)
  {
    m_height = h;
    updateRectSize();
  }

  void setViewScale(double s)
  {
    m_viewScale = s;
    updateRectSize();
  }

  void setTransformScale(double s)
  {
    m_transformScale = s;
    updateRectSize();
  }

  void setViewPort(const QRectF& viewport)
  {
    m_viewPort = viewport;
    updatePos();
  }

  void setColor(const glm::vec3& color)
  {
    setPen(Qt::NoPen);
    setBrush(QBrush(QColor(color.r * 255, color.g * 255, color.b * 255)));
  }

protected:
  QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

  void updateRectSize();

  void updatePos();

private:
  double m_lengthInUm;
  double m_height;
  double m_voxelSizeXInUm;
  double m_viewScale;
  double m_transformScale;
  QRectF m_viewPort;
  glm::vec2 m_viewPortPos;
};

class ZImgFilter : public ZObjFilter
{
  Q_OBJECT

public:
  explicit ZImgFilter(ZView& view);

  static int getViewPrecedence()
  {
    static int vp = 0;
    return vp++;
  }

  void setData(ZImgPack& pack);

  void releaseItemsOwnership();

  void setSelected(bool v);

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  [[nodiscard]] bool isVisible() const override
  {
    return m_isVisible;
  }

  [[nodiscard]] ZBBox<glm::ivec4> boundBox() const;

  // location within img, can be out of img range
  [[nodiscard]] int imgSlice() const;

  [[nodiscard]] int imgTime() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

protected:
  void viewPrecedenceChanged() override;

  void transformChanged() override;

  void offsetChanged() override;

  void updateViewSettingWidgetsGroup();

private:
  void channelVisibleChanged();

  void channelRangeChanged();

  void channelColorChanged();

  void opacityChanged();

  void mipRangeChanged();

  void visibleChanged();

  void hideImgItems();

  void destroyImgItems();

  void updateImgItems();

  void requestNeuroglancer2DRender();

  void startNeuroglancer2DCacheRender();

  void startNeuroglancer2DRender(bool finalPass);

  void applyQImagePack(const ZQImagePack& qImagePack, uint64_t epoch, int passPriority, bool isFinalPass);

  void updateNeuroglancerLoadingIndicator();

  [[nodiscard]] double getLowerChannelRange(size_t c) const;

  [[nodiscard]] double getUpperChannelRange(size_t c) const;

  void viewportChanged();

  void flipHorizontally();

  void flipVertically();

  void showScaleBarChanged();

  void scaleBarLengthChanged();

  void scaleBarHeightChanged();

  void scaleBarColorChanged();

  void viewScaleChanged(double s);

private:
  ZImgPack* m_imgPack = nullptr;

  std::vector<QGraphicsPixmapItem*> m_imgItems;
  std::unique_ptr<ZGraphicsItemGroup> m_item;
  std::unique_ptr<ZImgScaleBarGraphicsItem> m_scaleBarItem;

  bool m_sliceValid = false;
  bool m_hasVisibleChannel = true;
  bool m_isVisible = false;

  std::vector<std::unique_ptr<ZBoolParameter>> m_channelVisibleParas;
  std::vector<std::unique_ptr<ZDoubleSpanParameter>> m_doubleChannelRangeParas;
  std::vector<std::unique_ptr<ZVec3Parameter>> m_channelColorParas;
  ZDoubleParameter m_opacity;
  std::unique_ptr<ZIntSpanParameter> m_mipRange;
  ZBoolParameter m_showScaleBar;
  ZDoubleParameter m_scaleBarLengthInUm;
  ZIntParameter m_scaleBarHeight;
  ZVec3Parameter m_scaleBarColor;

  std::unique_ptr<ZImgPackDisplay> m_display;
  bool m_displayValid = false;

  bool m_lastMIP = false;
  int m_lastSlice = -1;
  int m_lastTime = -1;
  double m_lastScale = 0;
  QRectF m_lastViewport;

  // Neuroglancer: keep the UI responsive by fetching chunks off the UI thread,
  // and progressively refining from coarse -> sharp after interaction settles.
  QTimer m_ngRefineTimer;
  uint64_t m_ngRenderEpoch = 0;
  std::shared_ptr<std::atomic<uint64_t>> m_ngRenderEpochAtomic;
  bool m_ngCacheInFlight = false;
  uint64_t m_ngCacheInFlightEpoch = 0;
  bool m_ngCacheDirty = false;
  bool m_ngPreviewInFlight = false;
  uint64_t m_ngPreviewInFlightEpoch = 0;
  bool m_ngPreviewDirty = false;
  bool m_ngFinalInFlight = false;
  uint64_t m_ngFinalInFlightEpoch = 0;
  bool m_ngFinalPending = false;

  // Neuroglancer 2D: a small overlay badge to indicate progressive loading/refinement.
  std::unique_ptr<QGraphicsRectItem> m_ngLoadingBadgeBg;
  std::unique_ptr<QGraphicsSimpleTextItem> m_ngLoadingBadgeText;
  bool m_ngLastAppliedFinalPass = false;

  using NgTileKey = std::tuple<int, int, size_t, int, int>;

  struct NgTileApplyState
  {
    uint64_t epoch = 0;
    int passPriority = -1;
  };

  std::map<NgTileKey, QGraphicsPixmapItem*> m_ngTiles;
  std::map<NgTileKey, NgTileApplyState> m_ngTileApplyState;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
};

} // namespace nim
