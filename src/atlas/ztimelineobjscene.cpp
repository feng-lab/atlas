#include "ztimelineobjscene.h"

#include "zanimation.h"
#include "zbenchtimer.h"
#include "zlog.h"
#include "zparameteranimation.h"
#include "ztheme.h"
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QGraphicsPixmapItem>
#include <QTextOption>
#include <QTextDocument>
#include <QPainter>
#include <QColorDialog>
#include <QMenu>
#include <QGraphicsSceneContextMenuEvent>

namespace nim {

DiagramTextItem::DiagramTextItem(const ZAnimationDisplayPack& pack, QGraphicsItem* parent, Qt::Alignment align)
  : QGraphicsTextItem(parent)
  , m_displayPack(pack)
{
  m_textOp.setAlignment(align);
  m_textOp.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
}

void DiagramTextItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
  if (m_displayPack.type == ZAnimationDisplayPack::Type::Object && m_displayPack.boundId == 0) {
    painter->setPen(QPen(QColor(190, 190, 190)));
  }
  QRectF rect = boundingRect();
  if (m_textOp.alignment().testFlag(Qt::AlignRight)) {
    rect.setLeft(rect.left() + 30);
    rect.setWidth(rect.width() - 10);
  } else {
    rect.setLeft(rect.left() + 5);
    rect.setWidth(rect.width() - 10 - 20);
  }
  //  if (m_displayPack.name.length() >= 30) {
  //    QFont fnt = painter->font();
  //    fnt.setPointSize(10);
  //    painter->setFont(fnt);
  //  }
  painter->drawText(rect, m_displayPack.name, m_textOp);

  QGraphicsTextItem::paint(painter, option, widget);
}

QRectF DiagramTextItem::boundingRect() const
{
  return parentItem()->boundingRect();
}

ExpandArrowPixmapItem::ExpandArrowPixmapItem(const ZAnimationDisplayPack& pack, ZTimelineWidget& timeline,
                                             QGraphicsItem* parent)
  : QGraphicsPixmapItem(parent)
  , m_displayPack(pack)
  , m_timeline(timeline)
{
  setShapeMode(QGraphicsPixmapItem::BoundingRectShape);
}

void ExpandArrowPixmapItem::mousePressEvent(QGraphicsSceneMouseEvent* /*event*/)
{
  m_timeline.animation().toogleExpanded(m_displayPack.id);
}

ObjBoundRectItem::ObjBoundRectItem(const ZAnimationDisplayPack& pack, ZTimelineWidget& timeline, QGraphicsItem* parent)
  : QGraphicsRectItem(parent)
  , m_displayPack(pack)
  , m_timeline(timeline)
{
  setAcceptHoverEvents(true);
  setToolTip(m_displayPack.objInfo);
}

void ObjBoundRectItem::mousePressEvent(QGraphicsSceneMouseEvent* /*event*/)
{
  if (m_displayPack.type == ZAnimationDisplayPack::Type::ShowAll) {
    m_timeline.animation().toogleShowAll(m_displayPack.id);
  }
}

void ObjBoundRectItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* /*event*/)
{
  if (m_displayPack.type == ZAnimationDisplayPack::Type::Object) {
    m_timeline.animation().toogleExpanded(m_displayPack.id);
  }
}

void ObjBoundRectItem::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
{
  if (m_displayPack.type == ZAnimationDisplayPack::Type::Object) {
    QMenu menu;
    QAction* removeObjFromAnimation = menu.addAction(QString("Remove '%1' From Animation").arg(m_displayPack.name));
    QAction* selectedAction = menu.exec(event->screenPos());
    if (selectedAction == removeObjFromAnimation) {
      m_timeline.animation().removeObj(m_displayPack.id);
    }
  }
}

ParameterAnimationColorItem::ParameterAnimationColorItem(const ZAnimationDisplayPack& pack, ZTimelineWidget& timeline,
                                                         QGraphicsItem* parent)
  : QGraphicsRectItem(parent)
  , m_displayPack(pack)
  , m_timeline(timeline)
{
  setBrush(QBrush(m_displayPack.paraAnimation->color()));
  setRect(5, 5, m_timeline.rowHeight() - 10, m_timeline.rowHeight() - 10);
}

void ParameterAnimationColorItem::mousePressEvent(QGraphicsSceneMouseEvent* /*event*/)
{
  QColor newColor = QColorDialog::getColor(m_displayPack.paraAnimation->color(), &m_timeline,
                                           "New Color");
  if (newColor.isValid()) {
    m_displayPack.paraAnimation->setColor(newColor);
    setBrush(QBrush(m_displayPack.paraAnimation->color()));
  }
}

ZTimelineObjScene::ZTimelineObjScene(ZTimelineWidget& timeline, QObject* parent)
  : QGraphicsScene(parent)
  , m_timeline(timeline)
  , m_arrowRight(ZTheme::instance().iconFile(ZTheme::ArrowRightIcon))
  , m_arrowDown(ZTheme::instance().iconFile(ZTheme::ArrowDownIcon))
{
  updateItems();
  connect(&m_timeline.animation(), &ZAnimation::expandChanged, this, &ZTimelineObjScene::updateItems);
  connect(&m_timeline.animation(), &ZAnimation::objChanged, this, &ZTimelineObjScene::updateItems);
  connect(&m_timeline.animation(), &ZAnimation::objViewChanged, this, &ZTimelineObjScene::updateItems);
}

void ZTimelineObjScene::updateItems()
{
  //ZBenchTimer bt;
  //bt.start();
  const auto& dps = m_timeline.animation().displayPacks();
  int height = (dps.size() + 2) * m_timeline.rowHeight();  // leave space for horizonal scrollbar
  setSceneRect(0, 0, m_timeline.objViewWidth(), height);
  this->clear();
  m_itemToDisplayPack.clear();

  for (size_t dpi = 0; dpi < dps.size(); ++dpi) {
    const ZAnimationDisplayPack& pack = dps[dpi];
    if (pack.type == ZAnimationDisplayPack::Type::GlobalPara) {
      auto rect = new ObjBoundRectItem(pack, m_timeline);
      rect->setRect(0, 0, m_timeline.objViewWidth(), m_timeline.rowHeight());
      rect->setPen(QPen(QColor(133, 133, 133)));
      rect->setBrush(palette().brush(QPalette::Base));
      rect->setPos(0, pack.row * m_timeline.rowHeight());
      new DiagramTextItem(pack, rect, Qt::AlignVCenter | Qt::AlignRight);
      new ParameterAnimationColorItem(pack, m_timeline, rect);
      addItem(rect);
      m_itemToDisplayPack[rect] = &pack;
    } else if (pack.type == ZAnimationDisplayPack::Type::Object) {
      auto rect = new ObjBoundRectItem(pack, m_timeline);
      rect->setRect(0, 0, m_timeline.objViewWidth(), m_timeline.rowHeight());
      rect->setPen(QPen(QColor(133, 133, 133)));
      rect->setBrush(palette().brush(QPalette::AlternateBase));
      rect->setPos(0, pack.row * m_timeline.rowHeight());
      new DiagramTextItem(pack, rect, Qt::AlignVCenter | Qt::AlignLeft);
      ExpandArrowPixmapItem* arrow = new ExpandArrowPixmapItem(pack, m_timeline, rect);
      if (pack.expanded)
        arrow->setPixmap(m_arrowDown);
      else
        arrow->setPixmap(m_arrowRight);
      arrow->setPos(rect->rect().right() - 8 - m_arrowDown.width(),
                    (m_timeline.rowHeight() - m_arrowDown.height()) / 2.);
      addItem(rect);
      m_itemToDisplayPack[rect] = &pack;
    } else if (pack.type == ZAnimationDisplayPack::Type::ObjectPara) {
      auto rect = new ObjBoundRectItem(pack, m_timeline);
      rect->setRect(0, 0, m_timeline.objViewWidth(), m_timeline.rowHeight());
      rect->setPen(QPen(QColor(133, 133, 133)));
      rect->setBrush(palette().brush(QPalette::Base));
      rect->setPos(0, pack.row * m_timeline.rowHeight());
      new DiagramTextItem(pack, rect, Qt::AlignVCenter | Qt::AlignRight);
      new ParameterAnimationColorItem(pack, m_timeline, rect);
      addItem(rect);
      m_itemToDisplayPack[rect] = &pack;
    } else if (pack.type == ZAnimationDisplayPack::Type::ShowAll) {
      auto rect = new ObjBoundRectItem(pack, m_timeline);
      rect->setRect(0, 0, m_timeline.objViewWidth(), m_timeline.rowHeight());
      rect->setPen(QPen(QColor(133, 133, 133)));
      rect->setBrush(palette().brush(QPalette::Base));
      rect->setPos(0, pack.row * m_timeline.rowHeight());
      new DiagramTextItem(pack, rect, Qt::AlignVCenter | Qt::AlignHCenter);
      addItem(rect);
      m_itemToDisplayPack[rect] = &pack;
    }
  }

  //bt.stop();
  //LOG(INFO) << bt;
}

} // namespace nim
