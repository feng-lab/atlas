#pragma once

#include "ztimelinewidget.h"
#include "zparameterkey.h"
#include "zgraphicsitemtype.h"
#include <QGraphicsScene>
#include <QPixmap>
#include <QGraphicsTextItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsPathItem>
#include <QTextOption>

namespace nim {

class ZTimelineEventScene;

class ZTimelineEventView;

class EventBoundRectItem : public QGraphicsRectItem
{
public:
  EventBoundRectItem(const ZAnimationDisplayPack& pack, ZTimelineWidget& timeline, QGraphicsItem* parent = nullptr);

protected:
  void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;

private:
  const ZAnimationDisplayPack& m_displayPack;
  ZTimelineWidget& m_timeline;
};

class ZTimelineKeyEditDialog;

class ParameterKeysItem : public QGraphicsRectItem
{
public:
  enum
  {
    Type = GraphicsItemType::ParameterKeysItem
  };

  int type() const override
  { return Type; }

  ParameterKeysItem(ZParameterKey& paraKey, ZParameterAnimation& paraAnimation, const ZAnimationDisplayPack& pack,
                    ZTimelineWidget& timeline, QGraphicsItem* parent);

  void updateValue();

  ZParameterKey& paraKey()
  { return m_paraKey; }

  ZParameterAnimation& paraAnimation()
  { return m_paraAnimation; }

protected:
  QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

  void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;

  void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;

  void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;

  void mousePressEvent(QGraphicsSceneMouseEvent* event) override;

  void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

private:
  ZParameterKey& m_paraKey;
  ZParameterAnimation& m_paraAnimation;
  const ZAnimationDisplayPack& m_displayPack;
  ZTimelineWidget& m_timeline;
  std::unique_ptr<ZTimelineKeyEditDialog> m_editDialog;

  bool m_updateValueLock = false;
  bool m_itemMoved = false;
  double m_itemOldTime = 0.0;
};

class CurrentTimeLineItem : public QGraphicsLineItem
{
public:
  explicit CurrentTimeLineItem(ZTimelineWidget& timeline, QGraphicsItem* parent = nullptr);

protected:
  QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

  ZTimelineWidget& m_timeline;
};

class ZTimelineEventScene : public QGraphicsScene
{
Q_OBJECT
public:
  explicit ZTimelineEventScene(ZTimelineWidget& timeline, ZTimelineEventView* view);

  void updateKey(ZParameterKey* paraKey);

  void updateParameterAnimation(ZParameterAnimation* pa);

  void updateItems();

  void removeSelectedKeys();

protected:
  void resizeRects();

  void moveKeys();

  void moveCurrentTime();

  void deleteKeyItem(ZParameterKey* paraKey);

private:
  ZTimelineWidget& m_timeline;
  ZTimelineEventView* m_view;

  std::map<EventBoundRectItem*, const ZAnimationDisplayPack*> m_itemToDisplayPack;
  CurrentTimeLineItem* m_currentTimeItem;
  std::map<ZParameterKey*, ParameterKeysItem*> m_globalParaKeyToItem;
  std::map<ZParameterKey*, ParameterKeysItem*> m_ObjKeyToItem;
  std::map<ZParameterKey*, ParameterKeysItem*> m_ObjParaKeyToItem;
};

} // namespace nim


