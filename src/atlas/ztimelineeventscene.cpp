#include "ztimelineeventscene.h"

#include "ztimelinekeyeditdialog.h"
#include "zanimation.h"
#include "zbenchtimer.h"
#include "zlog.h"
#include "ztheme.h"
#include "ztimelineeventview.h"
#include "zparameteranimation.h"
#include <QScrollBar>
#include <QKeyEvent>
#include <QMenu>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QGraphicsPixmapItem>
#include <QTextOption>
#include <QTextDocument>
#include <QPainter>
#include <QInputDialog>
#include <cmath>

namespace nim {

EventBoundRectItem::EventBoundRectItem(const ZAnimationDisplayPack& pack,
                                       ZTimelineWidget& timeline,
                                       QGraphicsItem* parent)
  : QGraphicsRectItem(parent)
  , m_displayPack(pack)
  , m_timeline(timeline)
{}

void EventBoundRectItem::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
{
  if (m_displayPack.type == ZAnimationDisplayPack::Type::Object) {
    return;
  }
  QPointF pos = event->scenePos();
  double time = m_timeline.xToTime(pos.x());
  QMenu menu;

  QAction* addKeyHereAction = menu.addAction("Add Key Here");
  QAction* selectedAction = menu.exec(event->screenPos());

  if (selectedAction == addKeyHereAction) {
    if (!m_displayPack.paraAnimation->boundParameter()) {
      LOG(WARNING) << "Add Key Here: parameter not bound";
      return;
    }
    auto& anim = m_timeline.animation();
    auto before = anim.captureUndoSnapshot();
    m_displayPack.paraAnimation->addKey(m_displayPack.paraAnimation->createKey(time));
    anim.pushUndoSnapshotCommand("Add Key", std::move(before));
  }
}

ParameterKeysItem::ParameterKeysItem(ZParameterKey& paraKey,
                                     ZParameterAnimation& paraAnimation,
                                     const ZAnimationDisplayPack& pack,
                                     ZTimelineWidget& timeline,
                                     QGraphicsItem* parent)
  : QGraphicsRectItem(parent)
  , m_paraKey(paraKey)
  , m_paraAnimation(paraAnimation)
  , m_displayPack(pack)
  , m_timeline(timeline)
{
  if (m_paraAnimation.boundParameter()) {
    setFlags(QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable);
  } else {
    setFlags(QGraphicsItem::ItemIsSelectable);
  }
  setPos(m_timeline.timeToX(m_paraKey.time()), 0);
  setRect(-5, 5, 10, m_timeline.rowHeight() - 10);
  setAcceptHoverEvents(true);
}

void ParameterKeysItem::updateValue()
{
  if (!m_updateValueLock) {
    setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
    setPos(m_timeline.timeToX(m_paraKey.time()), 0);
    setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
  }
}

QVariant ParameterKeysItem::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value)
{
  if (change == ItemPositionChange) {
    m_pendingTime = m_timeline.xToTime(value.toPointF().x());
    m_itemMoved = true;
    return QPointF(m_timeline.timeToX(m_pendingTime), 0);
  }
  return QGraphicsRectItem::itemChange(change, value);
}

void ParameterKeysItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* /*option*/, QWidget* /*widget*/)
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

void ParameterKeysItem::hoverEnterEvent(QGraphicsSceneHoverEvent* /*event*/)
{
  setToolTip(
    QString("Parameter:%1\n%2Object:%3").arg(m_paraAnimation.name()).arg(m_paraKey.info()).arg(m_displayPack.objInfo));
}

void ParameterKeysItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* /*event*/)
{
  if (m_paraAnimation.boundParameter()) {
    if (!m_editDialog) {
      m_editDialog.reset(new ZTimelineKeyEditDialog(m_paraAnimation, m_paraKey, &m_timeline));
    }
    m_editDialog->setInitialValue();
    m_editDialog->showNormal();
    m_editDialog->raise();
    m_editDialog->activateWindow();
  }
}

void ParameterKeysItem::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
  m_itemMoved = false;
  m_itemOldTime = m_paraKey.time();
  m_pendingTime = m_itemOldTime;
  QGraphicsRectItem::mousePressEvent(event);
}

void ParameterKeysItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event)
{
  QGraphicsRectItem::mouseReleaseEvent(event);
  if (m_itemMoved && m_itemOldTime != m_pendingTime) {
    auto& anim = m_timeline.animation();
    auto before = anim.captureUndoSnapshot();

    m_updateValueLock = true;
    m_paraKey.setTime(m_pendingTime);
    m_paraAnimation.sortKeys();
    m_paraAnimation.emitKeyChangedSignal(&m_paraKey);
    m_updateValueLock = false;

    anim.pushUndoSnapshotCommand("Move Key", std::move(before));
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
  connect(&m_timeline.animation(), &ZAnimation::keyAboutToDelete, this, &ZTimelineEventScene::deleteKeyItem);
  connect(&m_timeline.animation(), &ZAnimation::colorChanged, this, &ZTimelineEventScene::updateParameterAnimation);
  connect(&ZTheme::instance(), &ZTheme::themeChanged, this, &ZTimelineEventScene::updateItems);
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
  for (const auto& key : pa->keys()) {
    updateKey(key.get());
  }
}

void ZTimelineEventScene::updateItems()
{
  // ZBenchTimer bt;
  // bt.start();
  const auto& dps = m_timeline.animation().displayPacks();
  int height = (dps.size() + 2) * m_timeline.rowHeight() -
               m_view->horizontalScrollBar()->height(); // leave space for horizonal scrollbar
  setSceneRect(0, 0, m_timeline.eventViewWidth() - m_view->verticalScrollBar()->width(), height);
  this->clear();
  m_itemToDisplayPack.clear();
  m_globalParaKeyToItem.clear();
  m_ObjKeyToItem.clear();
  m_ObjParaKeyToItem.clear();

  for (const auto& pack : dps) {
    if (pack.type == ZAnimationDisplayPack::Type::GlobalPara) {
      auto rect = new EventBoundRectItem(pack, m_timeline);
      rect->setRect(-1, 0, m_timeline.eventViewWidth() + 2, m_timeline.rowHeight());
      rect->setPen(QPen(QColor(133, 133, 133)));
      rect->setBrush(palette().brush(QPalette::Base));
      rect->setPos(0, pack.row * m_timeline.rowHeight());

      for (const auto& key : pack.paraAnimation->keys()) {
        m_globalParaKeyToItem[key.get()] = new ParameterKeysItem(*key, *pack.paraAnimation, pack, m_timeline, rect);
      }

      addItem(rect);
      m_itemToDisplayPack[rect] = &pack;
    } else if (pack.type == ZAnimationDisplayPack::Type::Object) {
      auto rect = new EventBoundRectItem(pack, m_timeline);
      rect->setRect(-1, 0, m_timeline.eventViewWidth() + 2, m_timeline.rowHeight());
      rect->setPen(QPen(QColor(133, 133, 133)));
      rect->setBrush(palette().brush(QPalette::AlternateBase));
      rect->setPos(0, pack.row * m_timeline.rowHeight());

      for (const auto& pa : m_timeline.animation().paraAnimationList(pack.id)) {
        for (const auto& key : pa->keys()) {
          ParameterKeysItem* ki = new ParameterKeysItem(*key, *pa, pack, m_timeline, rect);
          m_ObjKeyToItem[key.get()] = ki;
        }
      }

      addItem(rect);
      m_itemToDisplayPack[rect] = &pack;
    } else if (pack.type == ZAnimationDisplayPack::Type::ObjectPara) {
      auto rect = new EventBoundRectItem(pack, m_timeline);
      rect->setRect(-1, 0, m_timeline.eventViewWidth() + 2, m_timeline.rowHeight());
      rect->setPen(QPen(QColor(133, 133, 133)));
      rect->setBrush(palette().brush(QPalette::Base));
      rect->setPos(0, pack.row * m_timeline.rowHeight());

      for (const auto& key : pack.paraAnimation->keys()) {
        m_ObjParaKeyToItem[key.get()] = new ParameterKeysItem(*key, *pack.paraAnimation, pack, m_timeline, rect);
      }

      addItem(rect);
      m_itemToDisplayPack[rect] = &pack;
    }
  }

  m_currentTimeItem = new CurrentTimeLineItem(m_timeline);
  m_currentTimeItem->setLine(0, 0, 0, dps.size() * m_timeline.rowHeight());
  m_currentTimeItem->setPos(m_timeline.timeToX(m_timeline.currentTime()), 0);
  addItem(m_currentTimeItem);
  // bt.stop();
  // VLOG(1) << bt;
}

void ZTimelineEventScene::removeSelectedKeys()
{
  // make sure that the selected keys are deleted only once
  std::map<ZParameterKey*, ZParameterAnimation*> keyAniMap;
  for (auto itm : selectedItems()) {
    ParameterKeysItem* item = qgraphicsitem_cast<ParameterKeysItem*>(itm);
    if (item) {
      keyAniMap[&item->paraKey()] = &item->paraAnimation();
    }
  }
  if (keyAniMap.empty()) {
    return;
  }

  auto& anim = m_timeline.animation();
  auto before = anim.captureUndoSnapshot();
  for (const auto& keyAni : keyAniMap) {
    keyAni.second->deleteKey(keyAni.first);
  }

  anim.pushUndoSnapshotCommand("Delete Keys", std::move(before));
}

void ZTimelineEventScene::resizeRects()
{
  QRectF rect = sceneRect();
  rect.setWidth(m_timeline.eventViewWidth() - m_view->verticalScrollBar()->width());
  setSceneRect(rect);
  for (const auto& itemDisplayPack : m_itemToDisplayPack) {
    itemDisplayPack.first->setRect(-1, 0, m_timeline.eventViewWidth() + 2, m_timeline.rowHeight());
  }
}

void ZTimelineEventScene::moveKeys()
{
  for (const auto& keyItem : m_globalParaKeyToItem) {
    keyItem.second->updateValue();
  }
  for (const auto& keyItem : m_ObjKeyToItem) {
    keyItem.second->updateValue();
  }
  for (const auto& keyItem : m_ObjParaKeyToItem) {
    keyItem.second->updateValue();
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

void ZTimelineEventScene::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
{
  auto allSelectedItems = selectedItems();
  if (allSelectedItems.empty()) {
    QGraphicsScene::contextMenuEvent(event);
  } else {
    QMenu menu;

    QAction* setKeysTimeAction = menu.addAction("Set Time of Selected Keys");
    QAction* addKeysTimeAction = menu.addAction("Add Time to Selected Keys");
    QAction* subtractKeysTimeAction = menu.addAction("Subtract Time to Selected Keys");
    QAction* scaleKeysTimeAction = menu.addAction("Scale Time of Selected Keys");
    QAction* selectedAction = menu.exec(event->screenPos());

    if (selectedAction) {
      bool ok = false;
      double time = 0.0;
      if (selectedAction == setKeysTimeAction) {
        time = QInputDialog::getDouble(m_view,
                                       tr("Set Time of Selected Keys"),
                                       tr("Set Time:"),
                                       0.0,
                                       0.0,
                                       m_timeline.animation().duration(),
                                       3,
                                       &ok);
      } else if (selectedAction == addKeysTimeAction) {
        time = QInputDialog::getDouble(m_view,
                                       tr("Add Time to Selected Keys"),
                                       tr("Add Time:"),
                                       0.0,
                                       0.0,
                                       2147483647.,
                                       3,
                                       &ok);
      } else if (selectedAction == subtractKeysTimeAction) {
        time = QInputDialog::getDouble(m_view,
                                       tr("Subtract Time to Selected Keys"),
                                       tr("Subtract Time:"),
                                       0.0,
                                       0.0,
                                       2147483647.,
                                       3,
                                       &ok);
      } else if (selectedAction == scaleKeysTimeAction) {
        time = QInputDialog::getDouble(m_view,
                                       tr("Scale Time of Selected Keys"),
                                       tr("Scale Time:"),
                                       1.,
                                       0.001,
                                       2147483647.,
                                       3,
                                       &ok);
      } else {
        LOG(FATAL) << "wrong selected action";
      }
      if (ok) {
        // VLOG(1) << time;
        std::set<ZParameterKey*> keys;
        std::set<ZParameterAnimation*> anis;
        for (auto itm : allSelectedItems) {
          auto item = qgraphicsitem_cast<ParameterKeysItem*>(itm);
          if (item) {
            keys.insert(&item->paraKey());
            anis.insert(&item->paraAnimation());
          }
        }
        if (selectedAction == setKeysTimeAction) {
          for (auto key : keys) {
            key->setTime(time);
          }
        } else if (selectedAction == addKeysTimeAction) {
          for (auto key : keys) {
            key->setTime(key->time() + time);
          }
        } else if (selectedAction == subtractKeysTimeAction) {
          for (auto key : keys) {
            key->setTime(std::max(0.0, key->time() - time));
          }
        } else if (selectedAction == scaleKeysTimeAction) {
          for (auto key : keys) {
            key->setTime(std::max(0.0, key->time() * time));
          }
        }
        for (auto ani : anis) {
          ani->sortKeys();
          ani->emitKeysChangedSignal();
        }
      }
    }
  }
}

} // namespace nim
