#include "zswcfilter.h"

#include "zgraphicsview.h"
#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include "zgraphicsscene.h"
#include <QWindow>
#include <QStyleOption>
#include <QPushButton>
#include <QGraphicsSceneMouseEvent>
#include <memory>

namespace nim {

#if 0
ZSwcGraphicsItem::ZSwcGraphicsItem(ZSwcPack& swcPack, QGraphicsItem* parent)
  : QGraphicsItem(parent)
  , m_swcPack(swcPack)
{
  for (auto it = m_swcPack.swc().begin(); it != m_swcPack.swc().end(); ++it) {
    int slice = roundTo<int>(it->z);
    m_boundBox.expand(glm::ivec4(roundTo<int>(it->x - std::max(it->radius, .5)),
                                 roundTo<int>(it->y - std::max(it->radius, .5)),
                                 slice,
                                 0));
    m_boundBox.expand(glm::ivec4(roundTo<int>(it->x + std::max(it->radius, .5)),
                                 roundTo<int>(it->y + std::max(it->radius, .5)),
                                 slice,
                                 0));
    if (!ZSwc::isRoot(it)) {
      auto par = ZSwc::parent(it);
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
  //  m_skeletonColor.setAlpha(m_opacity * 0.5 * 255);
  //  painter.setPen(QPen(m_skeletonColor));
  //  painter.drawLines(m_lines);

  setFlag(QGraphicsItem::ItemIsSelectable, true);
}

QRectF ZSwcGraphicsItem::boundingRect() const
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

void ZSwcGraphicsItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
  if (m_t != 0)
    return;

  if (m_mip) {
    m_skeletonColor.setAlpha(m_opacity * 255);
    painter->setPen(QPen(m_skeletonColor, 1));
    //painter->drawLines(m_lines);
    for (const auto& node : m_swcPack.swc()) {
      painter->drawEllipse(
        QRectF(node.x - std::max(node.radius, .5), node.y - std::max(node.radius, .5), std::max(node.radius, .5) * 2,
               std::max(node.radius, .5) * 2));
    }
  } else {
    //if (m_showSkeleton) {
    //m_skeletonColor.setAlpha(m_opacity * 0.5 * 255);
    //painter->setPen(QPen(m_skeletonColor));
    //painter->drawLines(m_lines);
    //}
    m_skeletonColor.setAlpha(m_opacity * 255);
    painter->setPen(QPen(m_skeletonColor, 1));
    for (auto it = m_swcPack.swc().begin(); it != m_swcPack.swc().end(); ++it) {
      int slice = roundTo<int>(it->z);
      if (slice == m_z) {
        painter->drawEllipse(
          QRectF(it->x - std::max(it->radius, .5), it->y - std::max(it->radius, .5), std::max(it->radius, .5) * 2,
                 std::max(it->radius, .5) * 2));
        if (!ZSwc::isRoot(it)) {
          auto par = ZSwc::parent(it);
          painter->drawLine(QLineF(it->x, it->y, par->x, par->y));
        }
      }
    }
  }
  // drawLines to widget directly is very slow, don't know why
  if (widget && (m_mip || m_showSkeleton)) {
    m_skeletonColor.setAlpha(m_opacity * 0.5 * 255);
    double devicePixelRatio = (widget->window() && widget->window()->windowHandle()) ?
                              widget->window()->windowHandle()->devicePixelRatio() : 1.0;
    QPixmap buffer(widget->width() * devicePixelRatio, widget->height() * devicePixelRatio);
    buffer.fill(QColor(0, 0, 0, 0));
    QPainter pnt(&buffer);
    //pnt.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    pnt.setTransform(painter->combinedTransform());
    pnt.setPen(QPen(m_skeletonColor, 0));
    pnt.drawLines(m_lines);

    painter->resetTransform();
    painter->drawPixmap(0, 0, widget->width(), widget->height(), buffer);
    painter->setTransform(pnt.combinedTransform());
  } else if (m_mip || m_showSkeleton) {
    m_skeletonColor.setAlpha(m_opacity * 0.5 * 255);
    painter->setPen(QPen(m_skeletonColor, 0));
    painter->drawLines(m_lines);
  }

  if (option->state & QStyle::State_Selected)
    qt_graphicsItem_highlightSelected(this, painter, option);
}
#endif

ZSwcSkeletonGraphicsItem::ZSwcSkeletonGraphicsItem(ZSwcPack& swcPack, QGraphicsItem* parent)
  : QGraphicsItem(parent)
  , m_swcPack(swcPack)
{
  for (auto& [it1, it2] : m_swcPack.decompsedNodePairs()) {
    m_lines.push_back(QLineF(it1->x, it1->y, it2->x, it2->y));
  }
}

QRectF ZSwcSkeletonGraphicsItem::boundingRect() const
{
  qreal penWidth = 1 * m_sizeScale;
  auto boundBox = m_swcPack.boundBox();
  return QRectF(boundBox.minCorner.x - penWidth / 2,
                boundBox.minCorner.y - penWidth / 2,
                boundBox.maxCorner.x - boundBox.minCorner.x + penWidth,
                boundBox.maxCorner.y - boundBox.minCorner.y + penWidth);
}

void ZSwcSkeletonGraphicsItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget* widget)
{
  if (m_t != 0)
    return;

  if (m_selected) {
    // LOG(INFO) << "here";
    QPen pen(QBrush(QColor(255, 255, 255)), 2);
    pen.setCosmetic(true);
    painter->setPen(pen);
    // LOG(INFO) << boundingRect();
    painter->drawRect(boundingRect());
  }

  if (!m_mip) {
    m_skeletonColor.setAlpha(m_opacity * 255);
    painter->setPen(QPen(m_skeletonColor, 1 * m_sizeScale));
    for (auto& [n, pn] : m_swcPack.decompsedNodePairs()) {
      if (std::abs(std::round(n->z) - m_z) > 1) {
        continue;
      }
      painter->drawLine(QLineF(n->x, n->y, pn->x, pn->y));
    }
  }
  // drawLines to widget directly is very slow, don't know why
  if (widget && (m_mip || m_showSkeleton)) {
    m_skeletonColor.setAlpha(m_opacity * 0.5 * 255);
    double devicePixelRatio = (widget->window() && widget->window()->windowHandle()) ?
                              widget->window()->windowHandle()->devicePixelRatio() : 1.0;
    QPixmap buffer(widget->width() * devicePixelRatio, widget->height() * devicePixelRatio);
    buffer.fill(QColor(0, 0, 0, 0));
    QPainter pnt(&buffer);
    //pnt.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    pnt.setTransform(painter->combinedTransform());
    pnt.setPen(QPen(m_skeletonColor, 0));
    pnt.drawLines(m_lines);

    painter->resetTransform();
    painter->drawPixmap(0, 0, widget->width(), widget->height(), buffer);
    painter->setTransform(pnt.combinedTransform());
  } else if (m_mip || m_showSkeleton) {
    m_skeletonColor.setAlpha(m_opacity * 0.5 * 255);
    painter->setPen(QPen(m_skeletonColor, 0));
    painter->drawLines(m_lines);
  }
}

ZSwcNodeGraphicsItem::ZSwcNodeGraphicsItem(ZSwcPack& swcPack, const ZSwc::SwcTreeNode& swcNode,
                                           const QTransform& tfm,
                                           QGraphicsItem* parent)
  : QGraphicsEllipseItem(parent)
  , m_swcPack(swcPack)
  , m_swcNode(swcNode)
  , m_transform(tfm)
{
  setFlags(QGraphicsItem::ItemIsSelectable);

  m_basePos = m_transform.map(QPointF(m_swcNode->x, m_swcNode->y));
  setPos(m_basePos);
  QString tooltip = QString("Swc Node:(%1,%2,%3,%4)").
    arg(m_swcNode->x).arg(m_swcNode->y).arg(m_swcNode->z).arg(m_swcNode->radius);
  setToolTip(tooltip);
  updateRectSize();
}

void ZSwcNodeGraphicsItem::updateValue()
{
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
  m_basePos = m_transform.map(QPointF(m_swcNode->x, m_swcNode->y));
  setPos(m_basePos);
  QString tooltip = QString("Swc Node:(%1,%2,%3,%4)").
    arg(m_swcNode->x).arg(m_swcNode->y).arg(m_swcNode->z).arg(m_swcNode->radius);
  setToolTip(tooltip);
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
}

void ZSwcNodeGraphicsItem::updateRectSize()
{
  double radius = std::max(0.5, m_swcNode->radius) * m_sizeScale;
  QRectF rect(-radius, -radius, radius * 2, radius * 2);
  setRect(rect);
}

void ZSwcNodeGraphicsItem::setLocked(bool l)
{
  if (l) {
    setFlags(QGraphicsItem::GraphicsItemFlags());
  } else {
    setFlags(QGraphicsItem::ItemIsSelectable);
  }
}

void ZSwcNodeGraphicsItem::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
{
  if (m_swcPack.isLocked()) {
    return;
  }
  if (!isSelected() || !isVisible() || m_swcPack.selectedNodes().empty()) { // feels weird
    return;
  }
  m_swcPack.contextMenu().popup(event->screenPos());
}

ZSwcFilter::ZSwcFilter(ZView& view)
  : ZObjFilter(view)
  , m_showSkeleton("Show Skeleton", true)
  , m_skeletonColor("Skeleton Color", glm::vec3(1, 0, 0), glm::vec3(0), glm::vec3(1))
  , m_sizeScale("Size Scale", 1.f, .01f, std::numeric_limits<float>::max())
  , m_opacity("Opacity", .5, 0., 1.)
{
  m_skeletonColor.setStyle("COLOR");
  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZSwcFilter::visibleChanged);
  connect(&m_showSkeleton, &ZBoolParameter::valueChanged, this, &ZSwcFilter::showSkeletonChanged);
  connect(&m_skeletonColor, &ZVec3Parameter::valueChanged, this, &ZSwcFilter::skeletonColorChanged);
  connect(&m_sizeScale, &ZFloatParameter::valueChanged, this, &ZSwcFilter::sizeScaleChanged);
  connect(&m_opacity, &ZDoubleParameter::valueChanged, this, &ZSwcFilter::opacityChanged);
  addParameter(&m_showSkeleton);
  addParameter(&m_skeletonColor);
  m_sizeScale.setSingleStep(0.1);
  m_sizeScale.setDecimal(1);
  m_sizeScale.setStyle("SPINBOX");
  addParameter(&m_sizeScale);
  m_viewPrecedencePara.blockSignals(true);
  m_viewPrecedencePara.set(getViewPrecedence());
  m_viewPrecedencePara.blockSignals(false);
  addParameter(&m_opacity);

  connect(&m_swcColorParameters.colorMode, &ZStringIntOptionParameter::valueChanged, this, &ZSwcFilter::updateSwcNodeColor);
  addParameter(&m_swcColorParameters.colorMode);

  connect(&m_swcColorParameters.swcTreeColor, &ZVec4Parameter::valueChanged, this, &ZSwcFilter::updateSwcNodeColor);
  addParameter(&m_swcColorParameters.swcTreeColor);

  for (const auto& color : m_swcColorParameters.colorsForDifferentType) {
    connect(color.get(), &ZVec4Parameter::valueChanged, this, &ZSwcFilter::updateSwcNodeColor);
    addParameter(color.get());
  }

  for (const auto& color : m_swcColorParameters.colorsForDifferentTopology) {
    connect(color.get(), &ZVec4Parameter::valueChanged, this, &ZSwcFilter::updateSwcNodeColor);
    addParameter(color.get());
  }

  for (const auto& color : m_swcColorParameters.colorsForSubclassType) {
    connect(color.get(), &ZVec4Parameter::valueChanged, this, &ZSwcFilter::updateSwcNodeColor);
    addParameter(color.get());
  }

  addParameter(&m_swcColorParameters.colorMapBranchType);
  connect(&m_swcColorParameters.colorMapBranchType, &ZColorMapParameter::valueChanged, this, &ZSwcFilter::updateSwcNodeColor);

  connect(&view.scene(), &ZGraphicsScene::selectionChanged, this, &ZSwcFilter::onSceneItemSelectionChanged);
}

void ZSwcFilter::setData(ZSwcPack& swcPack)
{
  m_swcPack = &swcPack;

  createSwcSkeletonItem();
  createSwcNodeItems();

  connect(m_swcPack, &ZSwcPack::selectionChanged, this, &ZSwcFilter::updateItemSelectedState);
  connect(m_swcPack, &ZSwcPack::swcChanged, this, &ZSwcFilter::onSwcChanged);
  connect(m_swcPack, &ZSwcPack::lockedStateChanged, this, &ZSwcFilter::onLockedStateChanged);
}

void ZSwcFilter::releaseItemsOwnership()
{
  static_cast<void>(m_item.release());
  for (auto& item : m_swcNodeItems) {
    static_cast<void>(item.release());
  }
}

void ZSwcFilter::setSelected(bool v)
{
  m_item->setSelected_(v);
}

void ZSwcFilter::setNormalView(int z, int t)
{
  m_item->setNormalView(realZ(z), realT(t));
  if (!m_visible.get()) {
    return;
  }
  createSwcNodeItems();
}

void ZSwcFilter::setMaxZProjView(int t)
{
  m_item->setMaxZProjView(realT(t));
  if (!m_visible.get()) {
    return;
  }
  createSwcNodeItems();
}

ZBBox<glm::ivec4> ZSwcFilter::boundBox() const
{
  ZBBox<glm::ivec4> res;
  if (m_swcPack) {
    res = m_swcPack->boundBox();
    updateBoundBoxWithOffsetPara(res);
  }
  return res;
}

std::shared_ptr<ZWidgetsGroup> ZSwcFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("", 1);
    m_widgetsGroup->addChild(m_visible, 1);

    auto pb = new QPushButton("Bring to Front");
    connect(pb, &QPushButton::clicked, this, &ZSwcFilter::bringToFront);
    m_widgetsGroup->addChild(*pb, 1);

    pb = new QPushButton("Send to Back");
    connect(pb, &QPushButton::clicked, this, &ZSwcFilter::sendToBack);
    m_widgetsGroup->addChild(*pb, 1);

    m_widgetsGroup->addChild(m_viewPrecedencePara, 1);
    m_widgetsGroup->addChild(m_showSkeleton, 1);
    m_widgetsGroup->addChild(m_skeletonColor, 1);

    m_widgetsGroup->addChild(m_swcColorParameters.colorMode, 1);
    m_widgetsGroup->addChild(m_swcColorParameters.swcTreeColor, 1);

    for (const auto& color : m_swcColorParameters.colorsForDifferentType) {
      m_widgetsGroup->addChild(*color, 1);
    }
    for (const auto& color : m_swcColorParameters.colorsForSubclassType) {
      m_widgetsGroup->addChild(*color, 1);
    }
    for (const auto& color : m_swcColorParameters.colorsForDifferentTopology) {
      m_widgetsGroup->addChild(*color, 1);
    }
    m_widgetsGroup->addChild(m_swcColorParameters.colorMapBranchType, 1);
    m_widgetsGroup->addChild(m_sizeScale, 1);

    m_widgetsGroup->addChild(m_transform, 1);
    m_widgetsGroup->addChild(m_offsetPara, 1);
    m_widgetsGroup->addChild(m_opacity, 1);
  }
  return m_widgetsGroup;
}

void ZSwcFilter::deleteKeyPressed()
{
  if (m_swcPack->isLocked()) {
    return;
  }
  m_swcPack->deleteSelectedNodes();
}

void ZSwcFilter::viewPrecedenceChanged()
{
  m_item->setZValue(m_viewPrecedencePara.get());
  for (auto& item : m_swcNodeItems) {
    item->setZValue(m_viewPrecedencePara.get());
  }
  ZObjFilter::viewPrecedenceChanged();
}

void ZSwcFilter::transformChanged()
{
  auto trans = getQTransform();
  m_item->setTransform(trans);
  for (auto& item : m_swcNodeItems) {
    item->setTransform_(trans);
  }
  ZObjFilter::transformChanged();
}

void ZSwcFilter::offsetChanged()
{
  if (m_view.isMaxZProjView()) {
    m_item->setMaxZProjView(realT());
  } else {
    m_item->setNormalView(realZ(), realT());
  }
  createSwcNodeItems();
  ZObjFilter::offsetChanged();
}

void ZSwcFilter::createSwcSkeletonItem()
{
  m_item = std::make_unique<ZSwcSkeletonGraphicsItem>(*m_swcPack);
  m_item->setZValue(m_viewPrecedencePara.get());
  m_item->setShowSkeleton(m_showSkeleton.get());
  m_item->setSkeletonColor(QColor(m_skeletonColor.get().x * 255,
                                  m_skeletonColor.get().y * 255,
                                  m_skeletonColor.get().z * 255));
  m_item->setSizeScale(m_sizeScale.get());
  m_item->setOpacity(m_opacity.get());
  m_item->setTransform(getQTransform());
  if (m_view.isMaxZProjView()) {
    m_item->setMaxZProjView(realT());
  } else {
    m_item->setNormalView(realZ(), realT());
  }
  m_view.scene().addItem(m_item.get());
}

void ZSwcFilter::createSwcNodeItems()
{
  m_swcNodeItems.clear();
  m_swcNodeToItem.clear();
  m_itemToSwcNode.clear();
  if (!m_visible.get() || !m_swcPack) {
    return;
  }

  std::vector<std::unique_ptr<ZSwcNodeGraphicsItem>> items;
  QTransform trans = getQTransform();
  for (auto p : m_swcPack->decomposedNodes()) {
    if (!m_view.isMaxZProjView() && std::abs(realZ() - std::round(p->z)) > 1.0) {
      continue;
    }
    auto item = new ZSwcNodeGraphicsItem(*m_swcPack, p, trans);
    item->setZValue(m_viewPrecedencePara.get());
    auto color = m_swcColorParameters.colorOfNode(p);
    QPen pen(QColor(color.x * 255,
                    color.y * 255,
                    color.z * 255),
             2);
    pen.setCosmetic(true);
    QBrush brush(QColor(color.x * 255,
                        color.y * 255,
                        color.z * 255,
                        m_opacity.get() * 255));
    item->setBrush(brush);
    item->setPen(pen);
    item->setLocked(m_swcPack->isLocked());
    item->setSizeScale(m_sizeScale.get());
    m_view.scene().addItem(item);
    items.emplace_back(item);
    m_swcNodeToItem[p] = item;
    m_itemToSwcNode[item] = p;
  }
  m_swcNodeItems.swap(items);

  // LOG(INFO) << "here";
  updateItemSelectedState();
}

void ZSwcFilter::updateItemSelectedState()
{
  if (m_ignoreSelectionChangedSignal) {
    return;
  }
  m_skipSelectionChangedProcessing = true;
  for (auto&[p, item] : m_swcNodeToItem) {
    item->setSelected(m_swcPack->selectedNodes().find(p) != m_swcPack->selectedNodes().end());
  }
  m_skipSelectionChangedProcessing = false;
}

void ZSwcFilter::onSwcChanged()
{
  if (!m_visible.get()) {
    return;
  }

  m_skipSelectionChangedProcessing = true;
  createSwcSkeletonItem();
  createSwcNodeItems();
  m_skipSelectionChangedProcessing = false;

  Q_EMIT boundBoxChanged();
}

void ZSwcFilter::updateSwcNodeColor()
{
  createSwcNodeItems();
}

void ZSwcFilter::onSceneItemSelectionChanged()
{
  if (m_skipSelectionChangedProcessing) {
    return;
  }
  if (!m_swcPack) {
    return;
  }
  std::set<ZSwc::SwcTreeNode> selectedNodes;
  for (auto item : m_view.scene().selectedItems()) {
    if (auto it = m_itemToSwcNode.find(item); it != m_itemToSwcNode.end()) {
      selectedNodes.insert(it->second);
    }
  }
  // LOG(INFO) << selectedPuncta.size();
  m_ignoreSelectionChangedSignal = true;
  m_swcPack->setSelectedNodes(selectedNodes);
  m_ignoreSelectionChangedSignal = false;
}

void ZSwcFilter::onLockedStateChanged(bool lock)
{
  for (auto& item : m_swcNodeItems) {
    item->setLocked(lock);
  }
}

void ZSwcFilter::visibleChanged()
{
  m_item->setVisible(m_visible.get());
  if (!m_swcPack) {
    return;
  }
  if (m_visible.get()) {
    if (m_view.isMaxZProjView()) {
      setMaxZProjView(m_view.currentTime());
    } else {
      setNormalView(m_view.currentSlice(), m_view.currentTime());
    }
  } else {
    for (auto& item : m_swcNodeItems) {
      item->setVisible(false);
    }
  }
}

void ZSwcFilter::showSkeletonChanged()
{
  m_item->setShowSkeleton(m_showSkeleton.get());
}

void ZSwcFilter::skeletonColorChanged()
{
  m_item->setSkeletonColor(QColor(m_skeletonColor.get().x * 255,
                                  m_skeletonColor.get().y * 255,
                                  m_skeletonColor.get().z * 255));
}

void ZSwcFilter::sizeScaleChanged()
{
  m_item->setSizeScale(m_sizeScale.get());
  if (!m_swcPack) {
    return;
  }
  for (auto& item : m_swcNodeItems) {
    item->setSizeScale(m_sizeScale.get());
  }
}

void ZSwcFilter::opacityChanged()
{
  m_item->setOpacity(m_opacity.get());
  if (!m_swcPack) {
    return;
  }
  for (auto& item : m_swcNodeItems) {
    item->setOpacity(m_opacity.get());
  }
}

} // namespace nim
