#ifndef ZSWCFILTER_H
#define ZSWCFILTER_H

#include "zobjfilter.h"
#include <QList>
#include <vector>
#include "zparameter.h"
#include <QGraphicsEllipseItem>
#include <QGraphicsLineItem>
#include <map>
#include "znumericparameter.h"
#include "zswcdoc.h"

class ZWidgetsGroup;

namespace nim {

class ZSwcGraphicsItem : public QGraphicsItem
{
public:
  enum { Type = UserType + 7 };
  int type() const { return Type; }

  ZSwcGraphicsItem(ZSwc &swc, double z = 1, QGraphicsItem *parent = nullptr);

  void setShowSkeleton(bool v) { m_showSkeleton = v; update(); }
  void setOutlineColor(const QColor &c) { m_outlineColor = c; update(); }
  void setOpacity(double v) { m_opacity = v; update(); }
  void setNormalView(int z, int t) { m_mip = false; m_z = z; m_t = t; update(); }
  void setMaxZProjView(int t) { m_mip = true; m_t = t; update(); }

  const std::vector<int>& boundBox() const { return m_boundBox; }

  // QGraphicsItem interface
public:
  virtual QRectF boundingRect() const override;
  virtual void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

protected:
  ZSwc& m_swc;
  std::vector<int> m_boundBox;
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
  ZSwcFilter(ZView &view);
  ~ZSwcFilter();

  void setData(ZSwc &swc);

  void releaseItemsOwnership();

  void setVisible(bool v) { m_visible.set(v); }
  void setSelected(bool v) { Q_UNUSED(v); }
  void setNormalView(int z, int t) override;
  void setMaxZProjView(int t) override;

  const std::vector<int>& boundBox() const;

  ZWidgetsGroup* viewSettingWidgetsGroup();

signals:

public slots:

protected:

private slots:
  void visibleChanged();
  void showSkeletonChanged();
  void outlineColorChanged();

private:
  ZSwc *m_swc;
  std::unique_ptr<ZSwcGraphicsItem> m_item;

  ZBoolParameter m_visible;
  ZBoolParameter m_showSkeleton;
  ZVec3Parameter m_outlineColor;
  ZDoubleParameter m_opacity;
  bool m_sliceValid;

  ZWidgetsGroup *m_widgetsGroup;
};

} // namespace nim

#endif // ZSWCFILTER_H
