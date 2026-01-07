#pragma once

#include "zobjfilter.h"
#include "zparameter.h"
#include "znumericparameter.h"
#include "zgraphicsitemtype.h"
#include "zskeletondoc.h"

#include <QGraphicsItem>

namespace nim {

class ZSkeletonGraphicsItem : public QGraphicsItem
{
public:
  enum
  {
    Type = GraphicsItemType::ZSkeletonGraphicsItem
  };

  [[nodiscard]] int type() const override
  {
    return Type;
  }

  explicit ZSkeletonGraphicsItem(ZSkeleton& skeleton, QGraphicsItem* parent = nullptr);

  void setSelected_(bool v)
  {
    m_selected = v;
    update();
  }

  void setSkeletonColor(const QColor& c)
  {
    m_skeletonColor = c;
    update();
  }

  void setSizeScale(double sizeScale)
  {
    m_sizeScale = sizeScale;
    update();
  }

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

  [[nodiscard]] QRectF boundingRect() const override;

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

private:
  ZSkeleton& m_skeleton;
  QColor m_skeletonColor{255, 0, 0};
  bool m_mip = false;
  int m_z = 0;
  int m_t = 0;
  double m_sizeScale = 1.0;
  QList<QLineF> m_lines;
  bool m_selected = false;
};

class ZSkeletonFilter : public ZObjFilter
{
  Q_OBJECT

public:
  explicit ZSkeletonFilter(ZView& view);

  static int getViewPrecedence()
  {
    static int vp = 30000;
    return vp++;
  }

  void setData(ZSkeleton& skeleton);

  void releaseItemsOwnership();

  void setSelected(bool v);

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  [[nodiscard]] ZBBox<glm::ivec4> boundBox() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

protected:
  void viewPrecedenceChanged() override;

  void transformChanged() override;

  void offsetChanged() override;

private:
  void createSkeletonItem();

  void visibleChanged();

  void skeletonColorChanged();

  void sizeScaleChanged();

  void opacityChanged();

private:
  ZVec3Parameter m_skeletonColor;
  ZFloatParameter m_sizeScale;
  ZDoubleParameter m_opacity;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;

  ZSkeleton* m_skeleton = nullptr;
  std::unique_ptr<ZSkeletonGraphicsItem> m_item;
};

} // namespace nim
