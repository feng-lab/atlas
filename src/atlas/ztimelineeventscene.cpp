#include "ztimelineeventscene.h"

#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QGraphicsPixmapItem>
#include <QTextOption>
#include <QTextDocument>
#include <QPainter>
#include "zlog.h"
#include <cmath>
#include "zanimation.h"
#include "zbenchtimer.h"
#include "ztimelineeventview.h"
#include "zparameteranimation.h"
#include <QScrollBar>
#include <QKeyEvent>
#include <QMenu>
#include <QGraphicsSceneContextMenuEvent>
#include "ztimelinekeyeditdialog.h"

namespace nim {

EventBoundRectItem::EventBoundRectItem(const ZAnimationDisplayPack& pack, ZTimelineWidget& timeline,
                                       QGraphicsItem* parent)
  : QGraphicsRectItem(parent)
  , m_displayPack(pack)
  , m_timeline(timeline)
{
}

void EventBoundRectItem::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
{
  if (m_displayPack.type == ZAnimationDisplayPack::Type::Object)
    return;
  QPointF pos = event->scenePos();
  double time = m_timeline.xToTime(pos.x());
  if (time < 0)
    time = 0;
  QMenu menu;
  //QAction *addKeyAction = menu.addAction("Add Key...");
  QAction* addKeyHereAction = menu.addAction("Add Key Here");
  QAction* selectedAction = menu.exec(event->screenPos());
  /*if (selectedAction == addKeyAction) {
    ZParameterKey *key = m_displayPack.paraAnimation->createKey(time);
    ZTimelineKeyEditDialog dlg(*m_displayPack.paraAnimation, *key, &m_timeline);
    if (dlg.exec() == QDialog::Accepted) {
      m_displayPack.paraAnimation->addKey(key);
    } else {
      delete key;
    }
  } else */
  if (selectedAction == addKeyHereAction) {
    m_displayPack.paraAnimation->addKey(m_displayPack.paraAnimation->createKey(time));
  }
}

ParameterKeysItem::ParameterKeysItem(ZParameterKey& paraKey, ZParameterAnimation& paraAnimation,
                                     const ZAnimationDisplayPack& pack,
                                     ZTimelineWidget& timeline, QGraphicsItem* parent)
  : QGraphicsRectItem(parent)
  , m_paraKey(paraKey)
  , m_paraAnimation(paraAnimation)
  , m_displayPack(pack)
  , m_timeline(timeline)
{
  if (m_paraAnimation.boundParameter()) {
    setFlags(QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsMovable |
             QGraphicsItem::ItemIsSelectable);
  } else {
    setFlags(QGraphicsItem::ItemIsSelectable);
  }
  setPos(m_timeline.timeToX(m_paraKey.time()), 0);
  setRect(-5, 5, 10, m_timeline.rowHeight() - 10);
  setAcceptHoverEvents(true);
}

void ParameterKeysItem::updateValue()
{
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
  setPos(m_timeline.timeToX(m_paraKey.time()), 0);
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
}

QVariant ParameterKeysItem::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value)
{
  if (change == ItemPositionChange) {
    double time = m_timeline.xToTime(value.toPointF().x());
    m_paraKey.setTime(time);
    return QPointF(m_timeline.timeToX(m_paraKey.time()), 0);
  }
  return QGraphicsRectItem::itemChange(change, value);
}

void ParameterKeysItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*)
{
  painter->setRenderHint(QPainter::Antialiasing);
  QPen pen(m_paraAnimation.color());
  QBrush brush(m_paraAnimation.color());
  if (this->isSelected()) {
    pen.setStyle(Qt::DashLine);
    painter->setPen(pen);
    QRectF rct = this->rect();
    rct.adjust(-2, -2, 2, 2);
    painter->drawRect(rct);
    pen.setStyle(Qt::SolidLine);
  }
  painter->setPen(pen);
  painter->setBrush(brush);
  painter->drawRoundedRect(this->rect(), 5, 5);
  painter->setRenderHint(QPainter::Antialiasing, false);
}

void ParameterKeysItem::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
{
  event->accept();
}

void ParameterKeysItem::hoverEnterEvent(QGraphicsSceneHoverEvent*)
{
  setToolTip(
    QString("Parameter:%1\n%2Object:%3").arg(m_paraAnimation.name()).arg(m_paraKey.info()).arg(m_displayPack.objInfo));
}

void ParameterKeysItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent*)
{
  if (m_paraAnimation.boundParameter()) {
    if (!m_editDialog)
      m_editDialog.reset(new ZTimelineKeyEditDialog(m_paraAnimation, m_paraKey, &m_timeline));
    m_editDialog->setInitialValue();
    m_editDialog->show();
    m_editDialog->raise();
  }
}

CurrentTimeLineItem::CurrentTimeLineItem(ZTimelineWidget& timeline, QGraphicsItem* parent)
  : QGraphicsLineItem(parent)
  , m_timeline(timeline)
{
  setFlags(QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsMovable);
  QPen pen(QColor(25, 147, 255));
  setPen(pen);
}

QVariant CurrentTimeLineItem::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value)
{
  if (change == ItemPositionChange) {
    double time = m_timeline.xToTime(value.toPointF().x());
    m_timeline.setCurrentTime(time);
    return QPointF(m_timeline.timeToX(m_timeline.currentTime()), 0);
  }
  return QGraphicsLineItem::itemChange(change, value);
}

void CurrentTimeLineItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
  painter->setRenderHint(QPainter::Antialiasing);
  QGraphicsLineItem::paint(painter, option, widget);
  painter->setRenderHint(QPainter::Antialiasing, false);
}

ZTimelineEventScene::ZTimelineEventScene(ZTimelineWidget& timeline, ZTimelineEventView* view)
  : QGraphicsScene(view)
  , m_timeline(timeline)
  , m_view(view)
{
  updateItems();
  connect(&m_timeline.animation(), &ZAnimation::expandChanged, this, &ZTimelineEventScene::updateItems);
  connect(&m_timeline.animation(), &ZAnimation::objChanged, this, &ZTimelineEventScene::updateItems);
  connect(&m_timeline.animation(), &ZAnimation::objViewChanged, this, &ZTimelineEventScene::updateItems);
  connect(&m_timeline.animation(), &ZAnimation::keysChanged, this, &ZTimelineEventScene::updateItems);
  connect(&m_timeline, &ZTimelineWidget::eventViewWidthChanged, this, &ZTimelineEventScene::resizeRects);
  connect(&m_timeline, &ZTimelineWidget::pixelsPerSecondChagned, this, &ZTimelineEventScene::moveKeys);
  connect(&m_timeline, &ZTimelineWidget::pixelsPerSecondChagned, this, &ZTimelineEventScene::moveCurrentTime);
  connect(&m_timeline, &ZTimelineWidget::currentTimeChanged, this, &ZTimelineEventScene::moveCurrentTime);
  connect(&m_timeline.animation(), &ZAnimation::keyChanged, this, &ZTimelineEventScene::updateKey);
  connect(&m_timeline.animation(), &ZAnimation::keyAboutToDelete,
          this, &ZTimelineEventScene::deleteKeyItem);
  connect(&m_timeline.animation(), &ZAnimation::colorChanged,
          this, &ZTimelineEventScene::updateParameterAnimation);
}

void ZTimelineEventScene::updateKey(ZParameterKey* paraKey)
{
  std::map<ZParameterKey*, ParameterKeysItem*>::iterator it = m_globalParaKeyToItem.find(paraKey);
  if (it != m_globalParaKeyToItem.end()) {
    it->second->updateValue();
    return;
  }
  it = m_ObjKeyToItem.find(paraKey);
  if (it != m_ObjKeyToItem.end()) {
    it->second->updateValue();
    it = m_ObjParaKeyToItem.find(paraKey);
    if (it != m_ObjParaKeyToItem.end()) {
      it->second->updateValue();
    }
  }
}

void ZTimelineEventScene::updateParameterAnimation(ZParameterAnimation* pa)
{
  const auto& keys = pa->keys();
  for (size_t i = 0; i < keys.size(); ++i) {
    updateKey(keys[i].get());
  }
}

void ZTimelineEventScene::updateItems()
{
  //ZBenchTimer bt;
  //bt.start();
  const auto& dps = m_timeline.animation().displayPacks();
  int height = (dps.size() + 2) * m_timeline.rowHeight() -
               m_view->horizontalScrollBar()->height();  // leave space for horizonal scrollbar
  setSceneRect(0, 0, m_timeline.eventViewWidth() - m_view->verticalScrollBar()->width(), height);
  this->clear();
  m_itemToDisplayPack.clear();
  m_globalParaKeyToItem.clear();
  m_ObjKeyToItem.clear();
  m_ObjParaKeyToItem.clear();

  for (size_t dpi = 0; dpi < dps.size(); ++dpi) {
    const ZAnimationDisplayPack& pack = dps[dpi];
    if (pack.type == ZAnimationDisplayPack::Type::GlobalPara) {
      EventBoundRectItem* rect = new EventBoundRectItem(pack, m_timeline);
      rect->setRect(-1, 0, m_timeline.eventViewWidth() + 2, m_timeline.rowHeight());
      rect->setPen(QPen(QColor(133, 133, 133)));
      rect->setBrush(QBrush(QColor(235 + 20, 235 + 20, 235 + 20)));
      rect->setPos(0, pack.row * m_timeline.rowHeight());

      const auto& keys = pack.paraAnimation->keys();
      for (size_t i = 0; i < keys.size(); ++i) {
        m_globalParaKeyToItem[keys[i].get()] = new ParameterKeysItem(*keys[i], *pack.paraAnimation, pack, m_timeline,
                                                                     rect);
      }

      addItem(rect);
      m_itemToDisplayPack[rect] = &pack;
    } else if (pack.type == ZAnimationDisplayPack::Type::Object) {
      EventBoundRectItem* rect = new EventBoundRectItem(pack, m_timeline);
      rect->setRect(-1, 0, m_timeline.eventViewWidth() + 2, m_timeline.rowHeight());
      rect->setPen(QPen(QColor(133, 133, 133)));
      rect->setBrush(QBrush(QColor(220 + 20, 220 + 20, 220 + 20)));
      rect->setPos(0, pack.row * m_timeline.rowHeight());

      const auto& paraAnimationList = m_timeline.animation().paraAnimationList(pack.id);
      for (size_t j = 0; j < paraAnimationList.size(); ++j) {
        ZParameterAnimation* paraAnimation = paraAnimationList[j].get();
        const auto& keys = paraAnimation->keys();
        for (size_t i = 0; i < keys.size(); ++i) {
          ParameterKeysItem* ki = new ParameterKeysItem(*keys[i], *paraAnimation, pack, m_timeline, rect);
          m_ObjKeyToItem[keys[i].get()] = ki;
        }
      }

      addItem(rect);
      m_itemToDisplayPack[rect] = &pack;
    } else if (pack.type == ZAnimationDisplayPack::Type::ObjectPara) {
      EventBoundRectItem* rect = new EventBoundRectItem(pack, m_timeline);
      rect->setRect(-1, 0, m_timeline.eventViewWidth() + 2, m_timeline.rowHeight());
      rect->setPen(QPen(QColor(200, 200, 200)));
      rect->setBrush(QBrush(QColor(235 + 20, 235 + 20, 235 + 20)));
      rect->setPos(0, pack.row * m_timeline.rowHeight());

      const auto& keys = pack.paraAnimation->keys();
      for (size_t i = 0; i < keys.size(); ++i) {
        m_ObjParaKeyToItem[keys[i].get()] = new ParameterKeysItem(*keys[i], *pack.paraAnimation, pack, m_timeline,
                                                                  rect);
      }

      addItem(rect);
      m_itemToDisplayPack[rect] = &pack;
    }
  }

  m_currentTimeItem = new CurrentTimeLineItem(m_timeline);
  m_currentTimeItem->setLine(0, 0, 0, dps.size() * m_timeline.rowHeight());
  m_currentTimeItem->setPos(m_timeline.timeToX(m_timeline.currentTime()), 0);
  addItem(m_currentTimeItem);
  //bt.stop();
  //LOG(INFO) << bt;
}

void ZTimelineEventScene::removeSelectedKeys()
{
  QList<QGraphicsItem*> items = selectedItems();
  // make sure key delete only once
  std::map<ZParameterKey*, ZParameterAnimation*> keyAniMap;
  for (int i = 0; i < items.size(); ++i) {
    ParameterKeysItem* item = qgraphicsitem_cast<ParameterKeysItem*>(items[i]);
    if (item) {
      keyAniMap[&item->paraKey()] = &item->paraAnimation();
    }
  }
  for (std::map<ZParameterKey*, ZParameterAnimation*>::iterator it = keyAniMap.begin();
       it != keyAniMap.end(); ++it) {
    it->second->deleteKey(it->first);
  }
}

void ZTimelineEventScene::resizeRects()
{
  QRectF rect = sceneRect();
  rect.setWidth(m_timeline.eventViewWidth() - m_view->verticalScrollBar()->width());
  setSceneRect(rect);
  for (std::map<EventBoundRectItem*, const ZAnimationDisplayPack*>::iterator it = m_itemToDisplayPack.begin();
       it != m_itemToDisplayPack.end(); ++it) {
    it->first->setRect(-1, 0, m_timeline.eventViewWidth() + 2, m_timeline.rowHeight());
  }
}

void ZTimelineEventScene::moveKeys()
{
  for (std::map<ZParameterKey*, ParameterKeysItem*>::iterator it = m_globalParaKeyToItem.begin();
       it != m_globalParaKeyToItem.end(); ++it) {
    it->second->updateValue();
  }
  for (std::map<ZParameterKey*, ParameterKeysItem*>::iterator it = m_ObjKeyToItem.begin();
       it != m_ObjKeyToItem.end(); ++it) {
    it->second->updateValue();
  }
  for (std::map<ZParameterKey*, ParameterKeysItem*>::iterator it = m_ObjParaKeyToItem.begin();
       it != m_ObjParaKeyToItem.end(); ++it) {
    it->second->updateValue();
  }
}

void ZTimelineEventScene::moveCurrentTime()
{
  m_currentTimeItem->setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
  m_currentTimeItem->setPos(m_timeline.timeToX(m_timeline.currentTime()), 0);
  m_currentTimeItem->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
}

void ZTimelineEventScene::deleteKeyItem(ZParameterKey* paraKey)
{
  std::map<ZParameterKey*, ParameterKeysItem*>::iterator it = m_globalParaKeyToItem.find(paraKey);
  if (it != m_globalParaKeyToItem.end()) {
    removeItem(it->second);
    delete it->second;
    m_globalParaKeyToItem.erase(it);
    return;
  }
  it = m_ObjKeyToItem.find(paraKey);
  if (it != m_ObjKeyToItem.end()) {
    removeItem(it->second);
    delete it->second;
    m_ObjKeyToItem.erase(it);
    it = m_ObjParaKeyToItem.find(paraKey);
    if (it != m_ObjParaKeyToItem.end()) {
      removeItem(it->second);
      delete it->second;
      m_ObjParaKeyToItem.erase(it);
    }
  }
}

} // namespace nim
