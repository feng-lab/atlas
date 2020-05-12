#pragma once

#include "zobjfilter.h"
#include "zparameter.h"
#include "znumericparameter.h"
#include "zpunctapack.h"
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

  explicit ZPunctaGraphicsItem(ZPunctaPack& p, QGraphicsItem* parent = nullptr);

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
  ZPunctaPack& m_puncta;
  ZBBox<glm::ivec4> m_boundBox;
  QColor m_outlineColor{255, 0, 0};
  double m_opacity = 1;
  bool m_mip = false;
  int m_z = 0;
  int m_t = 0;
};

class ZPunctumGraphicsItem : public QGraphicsEllipseItem
{
public:
  enum
  {
    Type = GraphicsItemType::ZPunctumGraphicsItem
  };

  int type() const override
  { return Type; }

  ZPunctumGraphicsItem(ZPunctaPack& punctaPack, const ZPunctum& punctum, const QTransform& tfm, ZView& view,
                       QGraphicsItem* parent = nullptr);

  void updateValue();

  void setTransform_(const QTransform& tfm)
  { m_transform = tfm; updateValue(); }

  // void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget = nullptr) override;

protected:
  void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;

private:
  ZPunctaPack& m_punctaPack;
  const ZPunctum& m_punctum;

  QPointF m_basePos;
  bool m_doubleClicked = false;
  QTransform m_transform;
  ZView& m_view;
};

class ZPunctaFilter : public ZObjFilter
{
Q_OBJECT
public:
  explicit ZPunctaFilter(ZView& view);

  static int getViewPrecedence()
  {
    static int vp = 20000;
    return vp++;
  }

  void setData(ZPunctaPack& puncta);

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

  void createPunctumItems();

  void updateItemSelectedState();

  void onSceneItemSelectionChanged();

private:
  void visibleChanged();

  void outlineColorChanged();

  void regionColorChanged();

  void opacityChanged();

private:
  ZPunctaPack* m_puncta = nullptr;
  // std::unique_ptr<ZPunctaGraphicsItem> m_item;
  std::vector<std::unique_ptr<ZPunctumGraphicsItem>> m_puntumItems;
  std::map<const ZPunctum*, ZPunctumGraphicsItem*> m_punctumToItem;
  std::map<QGraphicsItem*, const ZPunctum*> m_itemToPunctum;

  ZBoolParameter m_visible;
  ZVec3Parameter m_outlineColor;
  ZVec3Parameter m_regionColor;
  ZDoubleParameter m_opacity;
  bool m_sliceValid;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;

  bool m_ignoreSelectionChangedSignal = false;
  bool m_skipSelectionChangedProcessing = false;
};

} // namespace nim

