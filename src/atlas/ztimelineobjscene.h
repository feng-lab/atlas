#pragma once

#include <QGraphicsScene>
#include <QPixmap>
#include <QGraphicsTextItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QTextOption>
#include "ztimelinewidget.h"

namespace nim {

class DiagramTextItem : public QGraphicsTextItem
{
public:
  DiagramTextItem(const ZAnimationDisplayPack& pack, QGraphicsItem* parent, Qt::Alignment align);

protected:
  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget = nullptr);

  QRectF boundingRect() const;

private:
  QTextOption m_textOp;
  const ZAnimationDisplayPack& m_displayPack;
};

class ZTimelineObjScene;

class ExpandArrowPixmapItem : public QGraphicsPixmapItem
{
public:
  ExpandArrowPixmapItem(const ZAnimationDisplayPack& pack, ZTimelineWidget& timeline, QGraphicsItem* parent);

protected:
  virtual void mousePressEvent(QGraphicsSceneMouseEvent* event) override;

private:
  const ZAnimationDisplayPack& m_displayPack;
  ZTimelineWidget& m_timeline;
};

class ObjBoundRectItem : public QGraphicsRectItem
{
public:
  ObjBoundRectItem(const ZAnimationDisplayPack& pack, ZTimelineWidget& timeline, QGraphicsItem* parent = nullptr);

protected:
  virtual void mousePressEvent(QGraphicsSceneMouseEvent* event) override;

  virtual void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;

  virtual void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;

private:
  const ZAnimationDisplayPack& m_displayPack;
  ZTimelineWidget& m_timeline;
};

class ParameterAnimationColorItem : public QGraphicsRectItem
{
public:
  ParameterAnimationColorItem(const ZAnimationDisplayPack& pack, ZTimelineWidget& timeline,
                              QGraphicsItem* parent = nullptr);

protected:
  virtual void mousePressEvent(QGraphicsSceneMouseEvent* event) override;

private:
  const ZAnimationDisplayPack& m_displayPack;
  ZTimelineWidget& m_timeline;
};

class ZTimelineObjScene : public QGraphicsScene
{
Q_OBJECT
public:
  explicit ZTimelineObjScene(ZTimelineWidget& timeline, QObject* parent = nullptr);

signals:

private:
  void updateItems();

private:
  ZTimelineWidget& m_timeline;

  QPixmap m_arrowRight;
  QPixmap m_arrowDown;
  std::map<ObjBoundRectItem*, const ZAnimationDisplayPack*> m_itemToDisplayPack;
};

} // namespace nim

