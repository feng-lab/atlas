#ifndef ZTIMELINEEVENTSCENE_H
#define ZTIMELINEEVENTSCENE_H

#include <QGraphicsScene>
#include <QPixmap>
#include <QGraphicsTextItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsPathItem>
#include <QTextOption>
#include "ztimelinewidget.h"
#include "zparameterkey.h"

namespace nim {

class ZTimelineEventScene;
class ZTimelineEventView;

class EventBoundRectItem : public QGraphicsRectItem
{
public:
  EventBoundRectItem(const ZAnimationDisplayPack &pack, ZTimelineWidget &timeline, QGraphicsItem *parent = nullptr);

protected:
  virtual void contextMenuEvent(QGraphicsSceneContextMenuEvent *event) override;

private:
  const ZAnimationDisplayPack &m_displayPack;
  ZTimelineWidget &m_timeline;
};

class ZTimelineKeyEditDialog;

class ParameterKeysItem : public QGraphicsRectItem
{
public:
  enum { Type = UserType + 4 };
  int type() const override { return Type; }
  ParameterKeysItem(ZParameterKey &paraKey, ZParameterAnimation &paraAnimation, const ZAnimationDisplayPack &pack,
                    ZTimelineWidget &timeline, QGraphicsItem *parent);
  ~ParameterKeysItem();

  void updateValue();

  ZParameterKey& paraKey() { return m_paraKey; }
  ZParameterAnimation& paraAnimation() { return m_paraAnimation; }

protected:
  virtual QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
  virtual void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;
  virtual void contextMenuEvent(QGraphicsSceneContextMenuEvent *event) override;
  virtual void hoverEnterEvent(QGraphicsSceneHoverEvent *event) override;
  virtual void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override;

private:
  ZParameterKey &m_paraKey;
  ZParameterAnimation &m_paraAnimation;
  const ZAnimationDisplayPack &m_displayPack;
  ZTimelineWidget &m_timeline;
  ZTimelineKeyEditDialog *m_editDialog;
};

class CurrentTimeLineItem : public QGraphicsLineItem
{
public:
  CurrentTimeLineItem(ZTimelineWidget &timeline, QGraphicsItem * parent = nullptr);

protected:
  virtual QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
  virtual void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

  ZTimelineWidget &m_timeline;
};

class ZTimelineEventScene : public QGraphicsScene
{
  Q_OBJECT
public:
  explicit ZTimelineEventScene(ZTimelineWidget &timeline, ZTimelineEventView *view);

signals:

public slots:
  void updateKey(ZParameterKey *paraKey);
  void updateParameterAnimation(ZParameterAnimation *pa);
  void updateItems();
  void removeSelectedKeys();

protected slots:
  void resizeRects();
  void moveKeys();
  void moveCurrentTime();
  void deleteKeyItem(ZParameterKey *paraKey);

protected:

protected:

private:
  ZTimelineWidget &m_timeline;
  ZTimelineEventView *m_view;

  std::map<EventBoundRectItem*, const ZAnimationDisplayPack*> m_itemToDisplayPack;
  CurrentTimeLineItem *m_currentTimeItem;
  std::map<ZParameterKey*,ParameterKeysItem*> m_globalParaKeyToItem;
  std::map<ZParameterKey*,ParameterKeysItem*> m_ObjKeyToItem;
  std::map<ZParameterKey*,ParameterKeysItem*> m_ObjParaKeyToItem;
};

} // namespace nim


#endif // ZTIMELINEEVENTSCENE_H
