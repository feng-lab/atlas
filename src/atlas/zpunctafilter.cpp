#include "zpunctafilter.h"

#include "zgraphicsview.h"
#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include "zsaturateoperation.h"
#include "zgraphicsscene.h"
#include <QStyleOption>
#include <QPushButton>
#include <QGraphicsSceneMouseEvent>
#include <utility>

namespace nim {

#if 0
ZPunctaGraphicsItem::ZPunctaGraphicsItem(ZPunctaPack& puncta, QGraphicsItem* parent)
  : QGraphicsItem(parent)
  , m_puncta(puncta)
{
  for (const auto& p : m_puncta.punctaPts()) {
    int slice = roundTo<int>(p->z());
    m_boundBox.expand(glm::ivec4(roundTo<int>(p->x() - p->radius()),
                                 roundTo<int>(p->y() - p->radius()),
                                 slice,
                                 0));
    m_boundBox.expand(glm::ivec4(roundTo<int>(p->x() + p->radius()),
                                 roundTo<int>(p->y() + p->radius()),
                                 slice,
                                 0));
  }

  setFlag(QGraphicsItem::ItemIsSelectable, true);
}

QRectF ZPunctaGraphicsItem::boundingRect() const
{
  qreal penWidth = 1;
  return QRectF(m_boundBox.minCorner().x - penWidth / 2,
                m_boundBox.minCorner().y - penWidth / 2,
                m_boundBox.maxCorner().x - m_boundBox.minCorner().x + penWidth,
                m_boundBox.maxCorner().y - m_boundBox.minCorner().y + penWidth);
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

void ZPunctaGraphicsItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* /*widget*/)
{
  if (m_t != 0)
    return;
  m_outlineColor.setAlpha(m_opacity * 255);
  painter->setPen(QPen(m_outlineColor, 1));
  if (m_mip) {
    for (const auto& p : m_puncta.punctaPts()) {
      painter->drawEllipse(QRectF(p->x() - p->radius(), p->y() - p->radius(), p->radius() * 2, p->radius() * 2));
    }
  } else {
    for (const auto& p : m_puncta.punctaPts()) {
      int slice = roundTo<int>(p->z());
      if (slice == m_z) {
        painter->drawEllipse(QRectF(p->x() - p->radius(), p->y() - p->radius(), p->radius() * 2, p->radius() * 2));
      }
    }
  }

  if (option->state & QStyle::State_Selected)
    qt_graphicsItem_highlightSelected(this, painter, option);
}
#endif

ZPunctumGraphicsItem::ZPunctumGraphicsItem(ZPunctaPack& punctaPack, const ZPunctum& punctum,
                                           QTransform tfm, ZView& view,
                                           QGraphicsItem* parent)
  : QGraphicsEllipseItem(parent)
  , m_punctaPack(punctaPack)
  , m_punctum(punctum)
  , m_transform(std::move(tfm))
  , m_view(view)
{
  setFlags(QGraphicsItem::ItemIsSelectable);

  m_basePos = m_transform.map(QPointF(m_punctum.x(), m_punctum.y()));
  setPos(m_basePos);
  QString tooltip = QString("Punctum:(%1,%2,%3,%4)").
    arg(m_punctum.x()).arg(m_punctum.y()).arg(m_punctum.z()).arg(m_punctum.radius());
  setToolTip(tooltip);
  updateRectSize();
}

void ZPunctumGraphicsItem::updateValue()
{
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
  m_basePos = m_transform.map(QPointF(m_punctum.x(), m_punctum.y()));
  setPos(m_basePos);
  QString tooltip = QString("Punctum:(%1,%2,%3,%4)").
    arg(m_punctum.x()).arg(m_punctum.y()).arg(m_punctum.z()).arg(m_punctum.radius());
  setToolTip(tooltip);
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
}

void ZPunctumGraphicsItem::updateRectSize()
{
  double radius = m_punctum.radius() * m_sizeScale;
  if (m_useSameSize) {
    radius = 2.0 * m_sizeScale;
  }
  QRectF rect(-radius, -radius, radius * 2, radius * 2);
  setRect(rect);
}

void ZPunctumGraphicsItem::setLocked(bool l)
{
  if (l) {
    setFlags(0);
  } else {
    setFlags(QGraphicsItem::ItemIsSelectable);
  }
}

void ZPunctumGraphicsItem::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
{
  if (m_punctaPack.isLocked()) {
    return;
  }
  if (!isSelected() || !isVisible() || m_punctaPack.selectedPuncta().empty()) { // feels weird
    return;
  }
  m_punctaPack.contextMenu().popup(event->screenPos());
}

ZPunctaFilter::ZPunctaFilter(ZView& view)
  : ZObjFilter(view)
  , m_outlineColor("Outline Color", glm::vec3(1, 0, 1), glm::vec3(0), glm::vec3(1))
  , m_regionColor("Region Color", glm::vec3(.2, .2, .2), glm::vec3(0), glm::vec3(1))
  , m_useSameSizeForAllPuncta("Use Same Size", false)
  , m_sizeScale("Size Scale", 1.f, .01f, std::numeric_limits<float>::max())
  , m_opacity("Opacity", 1, 0., 1.)
{
  m_outlineColor.setStyle("COLOR");
  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZPunctaFilter::visibleChanged);
  connect(&m_outlineColor, &ZVec3Parameter::valueChanged, this, &ZPunctaFilter::outlineColorChanged);
  connect(&m_regionColor, &ZVec3Parameter::valueChanged, this, &ZPunctaFilter::regionColorChanged);
  connect(&m_useSameSizeForAllPuncta, &ZBoolParameter::valueChanged, this, &ZPunctaFilter::useSameSizeChanged);
  connect(&m_sizeScale, &ZFloatParameter::valueChanged, this, &ZPunctaFilter::sizeScaleChanged);
  connect(&m_opacity, &ZDoubleParameter::valueChanged, this, &ZPunctaFilter::opacityChanged);
  addParameter(&m_outlineColor);
  addParameter(&m_regionColor);
  m_viewPrecedencePara.blockSignals(true);
  m_viewPrecedencePara.set(getViewPrecedence());
  m_viewPrecedencePara.blockSignals(false);
  addParameter(&m_useSameSizeForAllPuncta);
  m_sizeScale.setSingleStep(0.1);
  m_sizeScale.setDecimal(1);
  m_sizeScale.setStyle("SPINBOX");
  addParameter(&m_sizeScale);
  addParameter(&m_opacity);

  connect(&view.scene(), &ZGraphicsScene::selectionChanged, this, &ZPunctaFilter::onSceneItemSelectionChanged);
}

void ZPunctaFilter::setData(ZPunctaPack& puncta)
{
  m_punctaPack = &puncta;
  createPunctumItems();

  connect(m_punctaPack, &ZPunctaPack::selectionChanged, this, &ZPunctaFilter::updateItemSelectedState);
  connect(m_punctaPack, &ZPunctaPack::punctaChanged, this, &ZPunctaFilter::onPunctaChanged);
  connect(m_punctaPack, &ZPunctaPack::lockedStateChanged, this, &ZPunctaFilter::onLockedStateChanged);
  //connect(m_puncta, &ZPunctaFilter::boundBoxChanged, this, &ZPunctaFilter::boundBoxChanged);
}

void ZPunctaFilter::releaseItemsOwnership()
{
  for (auto& item : m_puntumItems) {
    item.release();
  }
}

void ZPunctaFilter::setSelected(bool v)
{
  if (v) {
    for (auto& item : m_puntumItems) {
      item->setSelected(v);
    }
  } else {
    updateItemSelectedState();
  }
}

void ZPunctaFilter::setNormalView(int, int)
{
  if (!m_visible.get()) {
    return;
  }
  createPunctumItems();
}

void ZPunctaFilter::setMaxZProjView(int)
{
  if (!m_visible.get()) {
    return;
  }
  createPunctumItems();
}

ZBBox<glm::ivec4> ZPunctaFilter::boundBox() const
{
  ZBBox<glm::ivec4> res;
  if (m_punctaPack) {
    res = m_punctaPack->boundBox();
    updateBoundBoxWithOffsetPara(res);
  }
  return res;
}

std::shared_ptr<ZWidgetsGroup> ZPunctaFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Puncta", 1);
    m_widgetsGroup->addChild(m_visible, 1);

    auto pb = new QPushButton("Bring to Front");
    connect(pb, &QPushButton::clicked, this, &ZPunctaFilter::bringToFront);
    m_widgetsGroup->addChild(*pb, 1);

    pb = new QPushButton("Send to Back");
    connect(pb, &QPushButton::clicked, this, &ZPunctaFilter::sendToBack);
    m_widgetsGroup->addChild(*pb, 1);

    m_widgetsGroup->addChild(m_viewPrecedencePara, 1);
    m_widgetsGroup->addChild(m_outlineColor, 1);
    m_widgetsGroup->addChild(m_regionColor, 1);
    m_widgetsGroup->addChild(m_useSameSizeForAllPuncta, 1);
    m_widgetsGroup->addChild(m_sizeScale, 1);
    m_widgetsGroup->addChild(m_opacity, 1);
    m_widgetsGroup->addChild(m_transform, 1);
    m_widgetsGroup->addChild(m_offsetPara, 1);
  }
  return m_widgetsGroup;
}

void ZPunctaFilter::deleteKeyPressed()
{
  if (m_punctaPack->isLocked()) {
    return;
  }
  m_punctaPack->deleteSelectedPuncta();
}

void ZPunctaFilter::viewPrecedenceChanged()
{
  for (auto& item : m_puntumItems) {
    item->setZValue(m_viewPrecedencePara.get());
  }
  ZObjFilter::viewPrecedenceChanged();
}

void ZPunctaFilter::transformChanged()
{
  auto trans = getQTransform();
  for (auto& item : m_puntumItems) {
    item->setTransform_(trans);
  }
  ZObjFilter::transformChanged();
}

void ZPunctaFilter::offsetChanged()
{
  createPunctumItems();
  ZObjFilter::offsetChanged();
}

void ZPunctaFilter::createPunctumItems()
{
  m_puntumItems.clear();
  m_punctumToItem.clear();
  m_itemToPunctum.clear();
  if (!m_visible.get() || !m_punctaPack) {
    return;
  }

  std::vector<std::unique_ptr<ZPunctumGraphicsItem>> items;
  QTransform trans = getQTransform();
  QPen pen(QColor(m_outlineColor.get().x * 255,
                  m_outlineColor.get().y * 255,
                  m_outlineColor.get().z * 255),
           2);
  pen.setCosmetic(true);
  QBrush brush(QColor(m_regionColor.get().x * 255,
                      m_regionColor.get().y * 255,
                      m_regionColor.get().z * 255,
                      m_opacity.get() * 255));
  for (const auto& p : m_punctaPack->puncta()) {
    if (!m_view.isMaxZProjView() && std::abs(realZ() - std::round(p.z())) > 1.0) {
      continue;
    }
    auto item = new ZPunctumGraphicsItem(*m_punctaPack, p, trans, m_view);
    item->setZValue(m_viewPrecedencePara.get());
    item->setBrush(brush);
    item->setPen(pen);
    item->setLocked(m_punctaPack->isLocked());
    item->setUseSameSize(m_useSameSizeForAllPuncta.get());
    item->setSizeScale(m_sizeScale.get());
    m_view.scene().addItem(item);
    items.emplace_back(item);
    m_punctumToItem[&p] = item;
    m_itemToPunctum[item] = &p;
  }
  m_puntumItems.swap(items);

  // LOG(INFO) << "here";
  updateItemSelectedState();
}

void ZPunctaFilter::updateItemSelectedState()
{
  if (m_ignoreSelectionChangedSignal) {
    return;
  }
  m_skipSelectionChangedProcessing = true;
  // LOG(INFO) << m_puncta->selectedPuncta().size();
  for (auto&[p, item] : m_punctumToItem) {
    item->setSelected(m_punctaPack->selectedPuncta().find(p) != m_punctaPack->selectedPuncta().end());
  }
  m_skipSelectionChangedProcessing = false;
}

void ZPunctaFilter::onPunctaChanged()
{
  if (!m_visible.get()) {
    return;
  }
  createPunctumItems();

  emit boundBoxChanged();
}

void ZPunctaFilter::onSceneItemSelectionChanged()
{
  if (m_skipSelectionChangedProcessing) {
    return;
  }
  if (!m_punctaPack) {
    return;
  }
  std::set<const ZPunctum*> selectedPuncta;
  for (auto item : m_view.scene().selectedItems()) {
    if (auto it = m_itemToPunctum.find(item); it != m_itemToPunctum.end()) {
      selectedPuncta.insert(it->second);
    }
  }
  // LOG(INFO) << selectedPuncta.size();
  m_ignoreSelectionChangedSignal = true;
  m_punctaPack->setSelectedPuncta(selectedPuncta);
  m_ignoreSelectionChangedSignal = false;
}

void ZPunctaFilter::onLockedStateChanged(bool lock)
{
  for (auto& item : m_puntumItems) {
    item->setLocked(lock);
  }
}

void ZPunctaFilter::visibleChanged()
{
  if (!m_punctaPack) {
    return;
  }
  if (m_visible.get()) {
    if (m_view.isMaxZProjView()) {
      setMaxZProjView(m_view.currentTime());
    } else {
      setNormalView(m_view.currentSlice(), m_view.currentTime());
    }
  } else {
    for (auto& item : m_puntumItems) {
      item->setVisible(false);
    }
  }
}

void ZPunctaFilter::outlineColorChanged()
{
  if (!m_punctaPack) {
    return;
  }
  QPen pen(QColor(m_outlineColor.get().x * 255,
                  m_outlineColor.get().y * 255,
                  m_outlineColor.get().z * 255),
           2);
  pen.setCosmetic(true);
  for (auto& item : m_puntumItems) {
    item->setPen(pen);
  }
}

void ZPunctaFilter::regionColorChanged()
{
  if (!m_punctaPack) {
    return;
  }
  for (auto& item : m_puntumItems) {
    item->setBrush(QColor(m_regionColor.get().x * 255,
                          m_regionColor.get().y * 255,
                          m_regionColor.get().z * 255,
                          m_opacity.get() * 255));
  }
}

void ZPunctaFilter::useSameSizeChanged()
{
  if (!m_punctaPack) {
    return;
  }
  for (auto& item : m_puntumItems) {
    item->setUseSameSize(m_useSameSizeForAllPuncta.get());
  }
}

void ZPunctaFilter::sizeScaleChanged()
{
  if (!m_punctaPack) {
    return;
  }
  for (auto& item : m_puntumItems) {
    item->setSizeScale(m_sizeScale.get());
  }
}

void ZPunctaFilter::opacityChanged()
{
  if (!m_punctaPack) {
    return;
  }
  for (auto& item : m_puntumItems) {
    item->setBrush(QColor(m_regionColor.get().x * 255,
                          m_regionColor.get().y * 255,
                          m_regionColor.get().z * 255,
                          m_opacity.get() * 255));
  }
}

} // namespace nim
