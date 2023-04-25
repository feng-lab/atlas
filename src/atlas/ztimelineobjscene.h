#pragma once

#include "ztimelinewidget.h"
#include <QGraphicsScene>
#include <QPixmap>
#include <QGraphicsTextItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsSvgItem>
#include <QGraphicsRectItem>
#include <QTextOption>

namespace nim {

class DiagramTextItem : public QGraphicsTextItem
{
public:
  DiagramTextItem(const ZAnimationDisplayPack& pack, QGraphicsItem* parent, Qt::Alignment align);

protected:
  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

  QRectF boundingRect() const override;

private:
  QTextOption m_textOp;
  const ZAnimationDisplayPack& m_displayPack;
};

class ZTimelineObjScene;

class ExpandArrowSvgItem : public QGraphicsSvgItem
{
public:
  ExpandArrowSvgItem(const QString& filename,
                     const ZAnimationDisplayPack& pack,
                     ZTimelineWidget& timeline,
                     QGraphicsItem* parent);

protected:
  void mousePressEvent(QGraphicsSceneMouseEvent* event) override;

private:
  const ZAnimationDisplayPack& m_displayPack;
  ZTimelineWidget& m_timeline;
};

class ObjBoundRectItem : public QGraphicsRectItem
{
public:
  ObjBoundRectItem(const ZAnimationDisplayPack& pack, ZTimelineWidget& timeline, QGraphicsItem* parent = nullptr);

protected:
  void mousePressEvent(QGraphicsSceneMouseEvent* event) override;

  void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;

  void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;

private:
  const ZAnimationDisplayPack& m_displayPack;
  ZTimelineWidget& m_timeline;
};

class ParameterAnimationColorItem : public QGraphicsRectItem
{
public:
  ParameterAnimationColorItem(const ZAnimationDisplayPack& pack,
                              ZTimelineWidget& timeline,
                              QGraphicsItem* parent = nullptr);

protected:
  void mousePressEvent(QGraphicsSceneMouseEvent* event) override;

private:
  const ZAnimationDisplayPack& m_displayPack;
  ZTimelineWidget& m_timeline;
};

class ZTimelineObjScene : public QGraphicsScene
{
  Q_OBJECT

public:
  explicit ZTimelineObjScene(ZTimelineWidget& timeline, QObject* parent = nullptr);

private:
  void updateItems();

private:
  ZTimelineWidget& m_timeline;

  std::map<ObjBoundRectItem*, const ZAnimationDisplayPack*> m_itemToDisplayPack;
};

} // namespace nim
