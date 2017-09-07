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

  explicit ZSwcGraphicsItem(ZSwc& swc, QGraphicsItem* parent = nullptr);

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

  const ZBBox<glm::ivec4>& boundBox() const
  { return m_boundBox; }

  // QGraphicsItem interface
public:
  QRectF boundingRect() const override;

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

protected:
  ZSwc& m_swc;
  ZBBox<glm::ivec4> m_boundBox;
  bool m_showSkeleton = true;
  QColor m_outlineColor{255, 0, 0};
  double m_opacity = 1;
  bool m_mip = false;
  int m_z = 0;
  int m_t = 0;
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

  void setSelected(bool v);

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  ZBBox<glm::ivec4> boundBox() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

protected:
  void viewPrecedenceChanged() override;

  void transformChanged() override;

  void offsetChanged() override;

private:
  void visibleChanged();

  void showSkeletonChanged();

  void outlineColorChanged();

  void opacityChanged();

private:
  std::unique_ptr<ZSwcGraphicsItem> m_item;

  ZBoolParameter m_visible;
  ZBoolParameter m_showSkeleton;
  ZVec3Parameter m_outlineColor;
  ZDoubleParameter m_opacity;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
};

} // namespace nim

