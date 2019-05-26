#pragma once

#include "zobjfilter.h"
#include "zimgpackdisplay.h"
#include "znumericparameter.h"
#include "zparameter.h"
#include "zgraphicsitemtype.h"
#include <QGraphicsPixmapItem>
#include <QGraphicsItemGroup>
#include <QPen>
#include <QList>
#include <vector>

namespace nim {

class ZDoubleSpanParameter;

class ZGraphicsItemGroup : public QGraphicsItemGroup
{
public:
  enum
  {
    Type = GraphicsItemType::ZGraphicsItemGroup
  };

  int type() const override
  { return Type; }

  explicit ZGraphicsItemGroup(QGraphicsItem* parent = nullptr);

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
};

class ZImgScaleBarGraphicsItem : public QGraphicsRectItem
{
public:
  enum
  {
    Type = GraphicsItemType::ZImgScaleBarGraphicsItem
  };

  int type() const override
  { return Type; }

  explicit ZImgScaleBarGraphicsItem(double lengthInUm, double height, double voxelSizeXInUm,
                                    double viewScale, const QRectF& viewPort, const glm::vec3& color,
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

  void setViewPort(const QRectF& viewport)
  {
    m_viewPort = viewport;
    updatePos();
  }

  void setColor(const glm::vec3& color)
  {
    setPen(QPen(QColor(color.r * 255, color.g * 255, color.b * 255), 0));
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
  QRectF m_viewPort;
  glm::vec2 m_viewPortPos;
};

class ZImgFilter : public ZObjFilter
{
Q_OBJECT
public:
  explicit ZImgFilter(ZView& view);

  void setData(ZImgPack& pack);

  void releaseItemsOwnership();

  void setVisible(bool v)
  { m_visible.set(v); }

  void setSelected(bool v);

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  inline bool isVisible() const
  { return m_isVisible; }

  ZBBox<glm::ivec4> boundBox() const;

  // location within img, can be out of img range
  int imgSlice() const;

  int imgTime() const;

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

  double getLowerChannelRange(size_t c) const;

  double getUpperChannelRange(size_t c) const;

  void viewportChanged();

  void flipHorizontally();

  void flipVertically();

  void showScaleBarChanged();

  void scaleBarLengthChanged();

  void scaleBarHeightChanged();

  void scaleBarColorChanged();

  void viewScaleChanged(double s);

private:
  ZImgPack* m_imgPack;

  std::vector<QGraphicsPixmapItem*> m_imgItems;
  std::unique_ptr<ZGraphicsItemGroup> m_item;
  std::unique_ptr<ZImgScaleBarGraphicsItem> m_scaleBarItem;

  ZBoolParameter m_visible;
  bool m_sliceValid;
  bool m_hasVisibleChannel;
  bool m_isVisible;

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
  bool m_displayValid;

  bool m_lastMIP;
  int m_lastSlice;
  int m_lastTime;
  double m_lastScale;
  QRectF m_lastViewport;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
};

} // namespace nim

