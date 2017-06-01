#pragma once

#include "zobjfilter.h"
#include "zimgpackdisplay.h"
#include "znumericparameter.h"
#include "zparameter.h"
#include "zgraphicsitemtype.h"
#include <QGraphicsPixmapItem>
#include <QGraphicsItemGroup>
#include <QList>
#include <vector>

class ZWidgetsGroup;

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

  ZGraphicsItemGroup(double z = 2, QGraphicsItem* parent = nullptr);

  virtual void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget = nullptr) override;
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

  void setSelected(bool v)
  { Q_UNUSED(v) }

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  void setViewport(const QRectF& rect, double scale) override;

  inline bool isVisible() const
  { return m_isVisible; }

  std::array<int, 8> boundBox() const;

  // location within img, can be out of img range
  int imgSlice() const;

  int imgTime() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

protected:
  virtual void transformChanged() override;

  virtual void offsetChanged() override;

  void updateViewSettingWidgetsGroup();

private:
  void channelVisibleChanged();

  void channelRangeChanged();

  void channelColorChanged();

  void opacityChanged();

  void visibleChanged();

  ZImgPackDisplay* getDisplay() const;

  void hideImgItems();

  void destroyImgItems();

  void updateImgItems();

  double getLowerChannelRange(size_t c) const;

  double getUpperChannelRange(size_t c) const;

private:
  ZImgPack* m_imgPack;

  std::vector<QGraphicsPixmapItem*> m_imgItems;
  std::unique_ptr<ZGraphicsItemGroup> m_item;

  ZBoolParameter m_visible;
  bool m_sliceValid;
  bool m_hasVisibleChannel;
  bool m_isVisible;

  std::vector<std::unique_ptr<ZBoolParameter>> m_channelVisibleParas;
  std::vector<std::unique_ptr<ZDoubleSpanParameter>> m_doubleChannelRangeParas;
  std::vector<std::unique_ptr<ZVec3Parameter>> m_channelColorParas;
  ZDoubleParameter m_opacity;

  std::unique_ptr<ZImgPackDisplay> m_display;
  mutable std::unique_ptr<ZImgPackDisplay> m_maxZProjDisplay;
  bool m_displayValid;

  ZImgPackDisplay* m_lastDisplay;
  int m_lastSlice;
  int m_lastTime;
  double m_lastScale;
  QRectF m_lastViewport;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
};

} // namespace nim

