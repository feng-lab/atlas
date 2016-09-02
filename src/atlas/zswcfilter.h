#pragma once

#include "zobjfilter.h"
#include "zparameter.h"
#include "znumericparameter.h"
#include "zswcdoc.h"
#include "zgraphicsitemtype.h"
#include <QGraphicsEllipseItem>
#include <QGraphicsLineItem>
#include <QList>
#include <map>
#include <vector>

class ZWidgetsGroup;

namespace nim {

class ZSwcGraphicsItem : public QGraphicsItem
{
public:
  enum
  {
    Type = GraphicsItemType::ZSwcGraphicsItem
  };

  int type() const override
  { return Type; }

  explicit ZSwcGraphicsItem(ZSwc& swc, double z = 1, QGraphicsItem* parent = nullptr);

  void setShowSkeleton(bool v)
  {
    m_showSkeleton = v;
    update();
  }

  void setOutlineColor(const QColor& c)
  {
    m_outlineColor = c;
    update();
  }

//  void setOpacity(double v)
//  {
//    m_opacity = v;
//    update();
//  }

  void setNormalView(int z, int t)
  {
    if (m_mip || m_z != z || m_t != t) {
      m_mip = false;
      m_z = z;
      m_t = t;
      update();
    }
  }

  void setMaxZProjView(int t)
  {
    if (!m_mip || m_t != t) {
      m_mip = true;
      m_t = t;
      update();
    }
  }

  const std::array<int, 8>& boundBox() const
  { return m_boundBox; }

  // QGraphicsItem interface
public:
  virtual QRectF boundingRect() const override;

  virtual void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

protected:
  ZSwc& m_swc;
  std::array<int, 8> m_boundBox;
  bool m_showSkeleton;
  QColor m_outlineColor;
  double m_opacity;
  bool m_mip;
  int m_z;
  int m_t;
  QVector<QLineF> m_lines;
};

class ZSwcFilter : public ZObjFilter
{
Q_OBJECT
public:
  explicit ZSwcFilter(ZView& view);

  void setData(ZSwc& swc);

  void releaseItemsOwnership();

  void setVisible(bool v)
  { m_visible.set(v); }

  void setSelected(bool v)
  { Q_UNUSED(v) }

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  std::array<int, 8> boundBox() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

protected:
  virtual void offsetChanged() override;

private:
  void visibleChanged();

  void showSkeletonChanged();

  void outlineColorChanged();

  void opacityChanged();

private:
  ZSwc* m_swc;
  std::unique_ptr<ZSwcGraphicsItem> m_item;

  ZBoolParameter m_visible;
  ZBoolParameter m_showSkeleton;
  ZVec3Parameter m_outlineColor;
  ZDoubleParameter m_opacity;
  bool m_sliceValid;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
};

} // namespace nim

