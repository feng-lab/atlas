#include "ztimelineaxisview.h"

#include <QGraphicsItem>
#include <QGraphicsEllipseItem>
#include <QGraphicsTextItem>
#include <QGraphicsRectItem>
#include <QGraphicsLineItem>
#include <QScrollBar>
#include "zlog.h"
#include "zsaturateoperation.h"

namespace nim {

CurrentTimeItem::CurrentTimeItem(ZTimelineWidget &timeline, QGraphicsItem *parent)
  : QGraphicsPathItem(parent)
  , m_timeline(timeline)
{
  setFlags(QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsMovable);
  QLinearGradient gradient(-4, 0, 7, 20);
  gradient.setColorAt(0, QColor(255,255,255));
  gradient.setColorAt(.7, QColor(194,212,248));
  gradient.setColorAt(1, QColor(155,169,198));
  gradient.setStart(-4,12);
  gradient.setFinalStop(3,13);
  gradient.setSpread(QGradient::PadSpread);
  setBrush(gradient);
  QPen pen(QColor(106,145,215));
  setPen(pen);
}

QVariant CurrentTimeItem::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant &value)
{
  if (change == ItemPositionChange) {
    double time = m_timeline.xToTime(value.toPointF().x());
    m_timeline.setCurrentTime(time);
    return QPointF(m_timeline.timeToX(m_timeline.currentTime()), 0);
  }
  return QGraphicsPathItem::itemChange(change, value);
}

void CurrentTimeItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
  painter->setRenderHint(QPainter::Antialiasing);
  QGraphicsPathItem::paint(painter, option, widget);
  painter->setRenderHint(QPainter::Antialiasing, false);
}

ZTimelineAxisView::ZTimelineAxisView(ZTimelineWidget &parent)
  : QGraphicsView(&parent)
  , m_timeline(parent)
{
  setAlignment(Qt::AlignLeft|Qt::AlignTop);
  m_scene = new QGraphicsScene(this);
  setScene(m_scene);
  setMinimumHeight(m_timeline.rowHeight()+2);
  setMaximumHeight(m_timeline.rowHeight()+2);
  setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
  //setStyleSheet("border-style: none;");

  updateAxisScene();
  connect(&m_timeline, &ZTimelineWidget::pixelsPerSecondChagned, this, &ZTimelineAxisView::updateAxisScene);
  connect(&m_timeline, &ZTimelineWidget::eventViewWidthChanged, this, &ZTimelineAxisView::updateAxisScene);
  connect(&m_timeline, &ZTimelineWidget::currentTimeChanged, this, &ZTimelineAxisView::moveCurrentTime);
}

void ZTimelineAxisView::updateAxisScene()
{
  m_scene->clear();

  m_scene->setSceneRect(0, 0, m_timeline.eventViewWidth(), m_timeline.rowHeight());
  QGraphicsRectItem* rect = new QGraphicsRectItem(nullptr);
  rect->setRect(-1, -1, m_timeline.eventViewWidth()+2, m_timeline.rowHeight()+2);
  rect->setPen(QPen(QColor(200,200,200)));
  rect->setBrush(QBrush(QColor(235+20,235+20,235+20)));

  m_scene->addItem(rect);

  int minDistBetweenTicks = 15;
  double minDistTime = minDistBetweenTicks / m_timeline.pixelsPerSecond();
  double tickTimeSpan = 0.01;
  if (minDistTime > 0.01) {
    double scale1 = 0.01;
    double scale2 = 0.05;
    double scale3 = 10 * scale1;
    while (true) {
      if (minDistTime > scale1 && minDistTime <= scale2) {
        tickTimeSpan = scale2;
        break;
      } else if (minDistTime > scale2 && minDistTime <= scale3) {
        tickTimeSpan = scale3;
        break;
      } else {
        scale1 *= 10;
        scale2 *= 10;
        scale3 *= 10;
      }
    }
  }

  for (double time = 0; time <= m_timeline.eventViewWidth() / m_timeline.pixelsPerSecond(); time += tickTimeSpan) {
    double x = m_timeline.timeToX(time);
    uint64_t count = roundTo<uint64_t>(time / tickTimeSpan);
    if (count % 10 == 0) { // big tick
      QGraphicsLineItem *line = new QGraphicsLineItem();
      line->setLine(x, m_timeline.rowHeight() * 0.2, x, m_timeline.rowHeight());
      line->setPen(QPen(QColor(100, 100, 100)));
      QGraphicsTextItem *text = new QGraphicsTextItem(timeToString(time));
      text->setPos(x, 0);
      QFont font;
      font.setPointSize(11);
      text->setFont(font);
      text->setDefaultTextColor(QColor(120, 120, 120));
      m_scene->addItem(line);
      m_scene->addItem(text);
    } else if (count % 5 == 0) { // middle tick
      QGraphicsLineItem *line = new QGraphicsLineItem();
      line->setLine(x, m_timeline.rowHeight() * 0.5, x, m_timeline.rowHeight());
      line->setPen(QPen(QColor(150, 150, 150)));
      QGraphicsTextItem *text = new QGraphicsTextItem(timeToString(time));
      text->setPos(x, 0);
      QFont font;
      font.setPointSize(11);
      text->setFont(font);
      text->setDefaultTextColor(QColor(150, 150, 150));
      m_scene->addItem(line);
      m_scene->addItem(text);
    } else { // small tick
      QGraphicsLineItem *line = new QGraphicsLineItem();
      line->setLine(x, m_timeline.rowHeight() * 0.7, x, m_timeline.rowHeight());
      line->setPen(QPen(QColor(180, 180, 180)));
      m_scene->addItem(line);
    }
  }

  m_currentTimeItem = new CurrentTimeItem(m_timeline);
  QPainterPath path;
  //path.addRect(-3,0,6,m_timeline.rowHeight()*.7);
  QPolygonF poly;
  poly << QPointF(-3,0) << QPointF(-3,m_timeline.rowHeight()*.7) << QPointF(0,m_timeline.rowHeight())
       << QPointF(3,m_timeline.rowHeight()*.7) << QPointF(3,0);
  path.addPolygon(poly);
  //path.closeSubpath();
  m_currentTimeItem->setPath(path);
  m_currentTimeItem->setPos(m_timeline.timeToX(m_timeline.currentTime()), 0);
  m_scene->addItem(m_currentTimeItem);
}

QString ZTimelineAxisView::timeToString(double time) const
{
  uint64_t tm = roundTo<uint64_t>(time / 0.001);
  uint64_t hour = tm / 3600000;
  tm -= hour * 3600000;
  uint64_t miniter = tm / 60000;
  tm -= miniter * 60000;
  if (hour > 0) {
    return QString("%1h%2m%3s").arg(hour).arg(miniter).arg(tm/1000.);
  } else if (miniter > 0) {
    return QString("%1m%2s").arg(miniter).arg(tm/1000.);
  } else {
    return QString("%1s").arg(tm/1000.);
  }
}

void ZTimelineAxisView::moveCurrentTime()
{
  m_currentTimeItem->setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
  m_currentTimeItem->setPos(m_timeline.timeToX(m_timeline.currentTime()), 0);
  m_currentTimeItem->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
  ensureVisible(m_currentTimeItem);
}



} // namespace nim

