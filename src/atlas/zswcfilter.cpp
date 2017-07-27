#include "zswcfilter.h"

#include "zgraphicsview.h"
#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include "zsaturateoperation.h"
#include "zgraphicsscene.h"
#include "zlogqttypesupport.h"
#include <QWindow>
#include <QStyleOption>
#include <QPushButton>

namespace nim {

ZSwcGraphicsItem::ZSwcGraphicsItem(ZSwc& swc, QGraphicsItem* parent)
  : QGraphicsItem(parent)
  , m_swc(swc)
{
  m_boundBox[0] = m_boundBox[2] = m_boundBox[4] = m_boundBox[6] = std::numeric_limits<int>::max();
  m_boundBox[1] = m_boundBox[3] = m_boundBox[5] = m_boundBox[7] = std::numeric_limits<int>::min();
  for (ZSwc::Iterator it = m_swc.begin(); it != m_swc.end(); ++it) {
    int slice = roundTo<int>(it->z);
    m_boundBox[0] = std::min(roundTo<int>(it->x - std::max(it->radius, .5)), m_boundBox[0]);
    m_boundBox[1] = std::max(roundTo<int>(it->x + std::max(it->radius, .5)), m_boundBox[1]);
    m_boundBox[2] = std::min(roundTo<int>(it->y - std::max(it->radius, .5)), m_boundBox[2]);
    m_boundBox[3] = std::max(roundTo<int>(it->y + std::max(it->radius, .5)), m_boundBox[3]);
    m_boundBox[4] = std::min(slice, m_boundBox[4]);
    m_boundBox[5] = std::max(slice, m_boundBox[5]);
    m_boundBox[6] = std::min(0, m_boundBox[6]);
    m_boundBox[7] = std::max(0, m_boundBox[7]);
    if (!ZSwc::isRoot(it)) {
      ZSwc::Iterator par = ZSwc::parent(it);
      m_lines.push_back(QLineF(it->x, it->y, par->x, par->y));
    }
  }
  //  for (ZSwc::Iterator it = m_swc.begin(); it != m_swc.end(); ++it) {
  //    if (!ZSwc::isRoot(it)) {
  //      ZSwc::Iterator par = ZSwc::parent(it);
  //      m_lines.push_back(QLineF(it->x - m_boundBox[0], it->y - m_boundBox[1],
  //          par->x - m_boundBox[0], par->y - m_boundBox[1]));
  //    }
  //  }
  //  m_skeletonCache = QPixmap(m_boundBox[1] - m_boundBox[0], m_boundBox[3] - m_boundBox[2]);
  //  m_skeletonCache.fill(QColor(0,0,0,0));
  //  QPainter painter(&m_skeletonCache);
  //  m_outlineColor.setAlpha(m_opacity * 0.5 * 255);
  //  painter.setPen(QPen(m_outlineColor));
  //  painter.drawLines(m_lines);

  setFlag(QGraphicsItem::ItemIsSelectable, true);
}

QRectF ZSwcGraphicsItem::boundingRect() const
{
  qreal penWidth = 1;
  return QRectF(m_boundBox[0] - penWidth / 2, m_boundBox[2] - penWidth / 2,
                m_boundBox[1] - m_boundBox[0] + penWidth,
                m_boundBox[3] - m_boundBox[2] + penWidth);
}

/*!
    \internal
    Highlights \a item as selected.
    NOTE: This function is a duplicate of qt_graphicsItem_highlightSelected() in
          qgraphicssvgitem.cpp!
*/
static void qt_graphicsItem_highlightSelected(
  QGraphicsItem *item, QPainter *painter, const QStyleOptionGraphicsItem *option)
{
  const QRectF murect = painter->transform().mapRect(QRectF(0, 0, 1, 1));
  if (qFuzzyIsNull(qMax(murect.width(), murect.height())))
    return;

  const QRectF mbrect = painter->transform().mapRect(item->boundingRect());
  if (qMin(mbrect.width(), mbrect.height()) < qreal(1.0))
    return;

  qreal itemPenWidth;
  switch (item->type()) {
    case QGraphicsEllipseItem::Type:
      itemPenWidth = static_cast<QGraphicsEllipseItem *>(item)->pen().widthF();
      break;
    case QGraphicsPathItem::Type:
      itemPenWidth = static_cast<QGraphicsPathItem *>(item)->pen().widthF();
      break;
    case QGraphicsPolygonItem::Type:
      itemPenWidth = static_cast<QGraphicsPolygonItem *>(item)->pen().widthF();
      break;
    case QGraphicsRectItem::Type:
      itemPenWidth = static_cast<QGraphicsRectItem *>(item)->pen().widthF();
      break;
    case QGraphicsSimpleTextItem::Type:
      itemPenWidth = static_cast<QGraphicsSimpleTextItem *>(item)->pen().widthF();
      break;
    case QGraphicsLineItem::Type:
      itemPenWidth = static_cast<QGraphicsLineItem *>(item)->pen().widthF();
      break;
    default:
      itemPenWidth = 1.0;
  }
  const qreal pad = itemPenWidth / 2;

  const qreal penWidth = 0; // cosmetic pen

  const QColor fgcolor = option->palette.windowText().color();
  const QColor bgcolor( // ensure good contrast against fgcolor
    fgcolor.red()   > 127 ? 0 : 255,
    fgcolor.green() > 127 ? 0 : 255,
    fgcolor.blue()  > 127 ? 0 : 255);

  painter->setPen(QPen(bgcolor, penWidth, Qt::SolidLine));
  painter->setBrush(Qt::NoBrush);
  painter->drawRect(item->boundingRect().adjusted(pad, pad, -pad, -pad));

  painter->setPen(QPen(option->palette.windowText(), 0, Qt::DashLine));
  painter->setBrush(Qt::NoBrush);
  painter->drawRect(item->boundingRect().adjusted(pad, pad, -pad, -pad));
}

void ZSwcGraphicsItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
  if (m_t != 0)
    return;

  if (m_mip) {
    m_outlineColor.setAlpha(m_opacity * 255);
    painter->setPen(QPen(m_outlineColor, 1));
    //painter->drawLines(m_lines);
    for (const auto& node : m_swc) {
      painter->drawEllipse(
        QRectF(node.x - std::max(node.radius, .5), node.y - std::max(node.radius, .5), std::max(node.radius, .5) * 2,
               std::max(node.radius, .5) * 2));
    }
  } else {
    //if (m_showSkeleton) {
    //m_outlineColor.setAlpha(m_opacity * 0.5 * 255);
    //painter->setPen(QPen(m_outlineColor));
    //painter->drawLines(m_lines);
    //}
    m_outlineColor.setAlpha(m_opacity * 255);
    painter->setPen(QPen(m_outlineColor, 1));
    for (ZSwc::Iterator it = m_swc.begin(); it != m_swc.end(); ++it) {
      int slice = roundTo<int>(it->z);
      if (slice == m_z) {
        painter->drawEllipse(
          QRectF(it->x - std::max(it->radius, .5), it->y - std::max(it->radius, .5), std::max(it->radius, .5) * 2,
                 std::max(it->radius, .5) * 2));
        if (!ZSwc::isRoot(it)) {
          ZSwc::Iterator par = ZSwc::parent(it);
          painter->drawLine(QLineF(it->x, it->y, par->x, par->y));
        }
      }
    }
  }
  // drawLines to widget directly is very slow, don't know why
  if (widget && (m_mip || m_showSkeleton)) {
    m_outlineColor.setAlpha(m_opacity * 0.5 * 255);
    double devicePixelRatio = (widget->window() && widget->window()->windowHandle()) ?
                              widget->window()->windowHandle()->devicePixelRatio() : 1.0;
    QPixmap buffer(widget->width() * devicePixelRatio, widget->height() * devicePixelRatio);
    buffer.fill(QColor(0, 0, 0, 0));
    QPainter pnt(&buffer);
    //pnt.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    pnt.setTransform(painter->combinedTransform());
    pnt.setPen(QPen(m_outlineColor, 0));
    pnt.drawLines(m_lines);

    painter->resetTransform();
    painter->drawPixmap(0, 0, widget->width(), widget->height(), buffer);
    painter->setTransform(pnt.combinedTransform());
  } else if (m_mip || m_showSkeleton) {
    m_outlineColor.setAlpha(m_opacity * 0.5 * 255);
    painter->setPen(QPen(m_outlineColor, 0));
    painter->drawLines(m_lines);
  }

  if (option->state & QStyle::State_Selected)
    qt_graphicsItem_highlightSelected(this, painter, option);
}

ZSwcFilter::ZSwcFilter(ZView& view)
  : ZObjFilter(view)
  , m_visible("Visible", true)
  , m_showSkeleton("Show Skeleton", true)
  , m_outlineColor("Outline Color", glm::vec3(1, 0, 0), glm::vec3(0), glm::vec3(1))
  , m_opacity("Opacity", 1, 0., 1.)
{
  m_outlineColor.setStyle("COLOR");
  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZSwcFilter::visibleChanged);
  connect(&m_showSkeleton, &ZBoolParameter::valueChanged, this, &ZSwcFilter::showSkeletonChanged);
  connect(&m_outlineColor, &ZVec3Parameter::valueChanged, this, &ZSwcFilter::outlineColorChanged);
  connect(&m_opacity, &ZDoubleParameter::valueChanged, this, &ZSwcFilter::opacityChanged);
  addParameter(&m_visible);
  addParameter(&m_showSkeleton);
  addParameter(&m_outlineColor);
  m_viewPrecedencePara.blockSignals(true);
  m_viewPrecedencePara.set(1000);
  m_viewPrecedencePara.blockSignals(false);
  addParameter(&m_viewPrecedencePara);
  addParameter(&m_transform);
  addParameter(&m_offsetPara);
  addParameter(&m_opacity);
}

void ZSwcFilter::setData(ZSwc& swc)
{
  m_swc = &swc;
  m_item.reset(new ZSwcGraphicsItem(swc));
  m_item->setZValue(m_viewPrecedencePara.get());
  m_item->setShowSkeleton(m_showSkeleton.get());
  m_item->setOutlineColor(QColor(m_outlineColor.get().x * 255,
                                 m_outlineColor.get().y * 255,
                                 m_outlineColor.get().z * 255));
  m_item->setOpacity(m_opacity.get());
  m_item->setTransform(getQTransform());
  if (m_view.isMaxZProjView()) {
    m_item->setMaxZProjView(realT());
  } else {
    m_item->setNormalView(realZ(), realT());
  }
  m_view.scene().addItem(m_item.get());
}

void ZSwcFilter::releaseItemsOwnership()
{
  m_item.release();
}

void ZSwcFilter::setSelected(bool v)
{
  if (m_item->isSelected() != v) {
    m_item->setSelected(v);
  }
}

void ZSwcFilter::setNormalView(int z, int t)
{
  m_item->setNormalView(realZ(z), realT(t));
}

void ZSwcFilter::setMaxZProjView(int t)
{
  m_item->setMaxZProjView(realT(t));
}

std::array<int, 8> ZSwcFilter::boundBox() const
{
  std::array<int, 8> res = m_item->boundBox();
  updateBoundBoxWithOffsetPara(res);
  return res;
}

std::shared_ptr<ZWidgetsGroup> ZSwcFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("", 1);
    m_widgetsGroup->addChild(m_visible, 1);

    QPushButton* pb = new QPushButton("Bring to Front");
    connect(pb, &QPushButton::clicked, this, &ZSwcFilter::bringToFront);
    m_widgetsGroup->addChild(*pb, 1);

    pb = new QPushButton("Send to Back");
    connect(pb, &QPushButton::clicked, this, &ZSwcFilter::sendToBack);
    m_widgetsGroup->addChild(*pb, 1);

    m_widgetsGroup->addChild(m_viewPrecedencePara, 1);
    m_widgetsGroup->addChild(m_showSkeleton, 1);
    m_widgetsGroup->addChild(m_outlineColor, 1);
    m_widgetsGroup->addChild(m_transform, 1);
    m_widgetsGroup->addChild(m_offsetPara, 1);
    m_widgetsGroup->addChild(m_opacity, 1);
  }
  return m_widgetsGroup;
}

void ZSwcFilter::viewPrecedenceChanged()
{
  m_item->setZValue(m_viewPrecedencePara.get());
  ZObjFilter::viewPrecedenceChanged();
}

void ZSwcFilter::transformChanged()
{
  m_item->setTransform(getQTransform());
  ZObjFilter::transformChanged();
}

void ZSwcFilter::offsetChanged()
{
  if (m_view.isMaxZProjView()) {
    m_item->setMaxZProjView(realT());
  } else {
    m_item->setNormalView(realZ(), realT());
  }
  ZObjFilter::offsetChanged();
}

void ZSwcFilter::visibleChanged()
{
  m_item->setVisible(m_visible.get());
}

void ZSwcFilter::showSkeletonChanged()
{
  m_item->setShowSkeleton(m_showSkeleton.get());
}

void ZSwcFilter::outlineColorChanged()
{
  m_item->setOutlineColor(QColor(m_outlineColor.get().x * 255,
                                 m_outlineColor.get().y * 255,
                                 m_outlineColor.get().z * 255));
}

void ZSwcFilter::opacityChanged()
{
  m_item->setOpacity(m_opacity.get());
}

} // namespace nim
