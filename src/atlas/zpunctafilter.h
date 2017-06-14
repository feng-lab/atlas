#pragma once

#include "zobjfilter.h"
#include "zparameter.h"
#include "znumericparameter.h"
#include "zpuncta.h"
#include "zgraphicsitemtype.h"
#include <QList>
#include <QGraphicsEllipseItem>
#include <map>
#include <vector>

class ZWidgetsGroup;

namespace nim {

class ZPunctaGraphicsItem : public QGraphicsItem
{
public:
  enum
  {
    Type = GraphicsItemType::ZPunctaGraphicsItem
  };

  int type() const override
  { return Type; }

  explicit ZPunctaGraphicsItem(ZPuncta& puncta, double z = 1, QGraphicsItem* parent = nullptr);

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
  ZPuncta& m_puncta;
  std::array<int, 8> m_boundBox;
  QColor m_outlineColor{255, 0, 0};
  double m_opacity = 1;
  bool m_mip = false;
  int m_z = 0;
  int m_t = 0;
};

class ZPunctaFilter : public ZObjFilter
{
Q_OBJECT
public:
  explicit ZPunctaFilter(ZView& view);

  void setData(ZPuncta& puncta);

  void releaseItemsOwnership();

  void setVisible(bool v)
  { m_visible.set(v); }

  void setSelected(bool v);

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  std::array<int, 8> boundBox() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

protected:
  virtual void transformChanged() override;

  virtual void offsetChanged() override;

private:
  void visibleChanged();

  void outlineColorChanged();

  void opacityChanged();

private:
  ZPuncta* m_puncta = nullptr;
  std::unique_ptr<ZPunctaGraphicsItem> m_item;

  ZBoolParameter m_visible;
  ZVec3Parameter m_outlineColor;
  ZDoubleParameter m_opacity;
  bool m_sliceValid;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;

  std::array<int, 8> m_boundBox;
};

} // namespace nim

