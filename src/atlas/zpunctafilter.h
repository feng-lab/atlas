#pragma once

#include "zobjfilter.h"
#include <QList>
#include <vector>
#include "zparameter.h"
#include <QGraphicsEllipseItem>
#include <map>
#include "znumericparameter.h"
#include "zpuncta.h"

class ZWidgetsGroup;

namespace nim {

class ZPunctaGraphicsItem : public QGraphicsItem
{
public:
  enum
  {
    Type = UserType + 9
  };

  int type() const override
  { return Type; }

  ZPunctaGraphicsItem(ZPuncta& puncta, double z = 1, QGraphicsItem* parent = nullptr);

  void setOutlineColor(const QColor& c)
  {
    m_outlineColor = c;
    update();
  }

  void setOpacity(double v)
  {
    m_opacity = v;
    update();
  }

  void setNormalView(int z, int t)
  {
    m_mip = false;
    m_z = z;
    m_t = t;
    update();
  }

  void setMaxZProjView(int t)
  {
    m_mip = true;
    m_t = t;
    update();
  }

  const std::vector<int>& boundBox() const
  { return m_boundBox; }

  // QGraphicsItem interface
public:
  virtual QRectF boundingRect() const override;

  virtual void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

protected:
  ZPuncta& m_puncta;
  std::vector<int> m_boundBox;
  QColor m_outlineColor;
  double m_opacity;
  bool m_mip;
  int m_z;
  int m_t;
};

class ZPunctaFilter : public ZObjFilter
{
Q_OBJECT
public:
  ZPunctaFilter(ZView& view);

  void setData(ZPuncta& puncta);

  void releaseItemsOwnership();

  void setVisible(bool v)
  { m_visible.set(v); }

  void setSelected(bool v)
  { Q_UNUSED(v) }

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  const std::vector<int>& boundBox() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

private:
  void visibleChanged();

  void outlineColorChanged();

private:
  ZPuncta* m_puncta;
  std::unique_ptr<ZPunctaGraphicsItem> m_item;

  ZBoolParameter m_visible;
  ZVec3Parameter m_outlineColor;
  ZDoubleParameter m_opacity;
  bool m_sliceValid;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;

  std::vector<int> m_boundBox;
};

} // namespace nim

