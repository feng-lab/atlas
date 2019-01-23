#include "zimgfilter.h"

#include "zimg.h"
#include "zimgdoc.h"
#include "zimgview.h"
#include "zimgpackdisplay.h"
#include "zlog.h"
#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include "zlogqttypesupport.h"
#include "ztheme.h"
#include <QGraphicsPixmapItem>
#include <QStyleOption>
#include <QPainter>
#include <QPushButton>

namespace nim {

ZGraphicsItemGroup::ZGraphicsItemGroup(QGraphicsItem* parent)
  : QGraphicsItemGroup(parent)
{
  //setFlag(QGraphicsItem::ItemIsSelectable, true);
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
  const qreal pad = -itemPenWidth / 2;

  const qreal penWidth = 0; // cosmetic pen

  const QColor fgcolor = option->palette.windowText().color();
  const QColor bgcolor( // ensure good contrast against fgcolor
    fgcolor.red()   > 127 ? 0 : 255,
    fgcolor.green() > 127 ? 0 : 255,
    fgcolor.blue()  > 127 ? 0 : 255);

  painter->setPen(QPen(bgcolor, penWidth, Qt::SolidLine));
  painter->setBrush(Qt::NoBrush);
  painter->drawRect(item->boundingRect().adjusted(pad, pad, -pad, -pad));

  painter->setPen(QPen(fgcolor, penWidth, Qt::DashLine));
  painter->setBrush(Qt::NoBrush);
  painter->drawRect(item->boundingRect().adjusted(pad, pad, -pad, -pad));
}

void ZGraphicsItemGroup::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* /*widget*/)
{
  if (option->state & QStyle::State_Selected)
    qt_graphicsItem_highlightSelected(this, painter, option);
}

ZImgFilter::ZImgFilter(ZView& view)
  : ZObjFilter(view)
  , m_imgPack(nullptr)
  , m_visible("Visible", true)
  , m_hasVisibleChannel(true)
  , m_isVisible(false)
  , m_opacity(QString("Opacity"), 1.0, 0.0, 1.0)
  , m_displayValid(false)
  , m_lastDisplay(nullptr)
  , m_lastSlice(-1)
  , m_lastTime(-1)
  , m_lastScale(0)
  , m_lastViewport()
{
  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZImgFilter::visibleChanged);
  addParameter(&m_visible);
  connect(&m_opacity, &ZDoubleParameter::valueChanged, this, &ZImgFilter::opacityChanged);
  addParameter(&m_opacity);
  m_viewPrecedencePara.blockSignals(true);
  m_viewPrecedencePara.set(0);
  m_viewPrecedencePara.blockSignals(false);
  addParameter(&m_viewPrecedencePara);
  addParameter(&m_transform);
  addParameter(&m_offsetPara);
  connect(&m_view, &ZView::viewportChanged, this, &ZImgFilter::viewportChanged);
}

void ZImgFilter::setData(ZImgPack& pack)
{
  m_imgPack = &pack;
  destroyImgItems();
  m_display.reset();
  m_maxZProjDisplay.reset();

  for (size_t i = 0; i < m_channelVisibleParas.size(); ++i) {
    m_channelColorParas[i]->disconnect();
    m_channelVisibleParas[i]->disconnect();
    m_doubleChannelRangeParas[i]->disconnect();
    removeParameter(m_channelColorParas[i].get());
    removeParameter(m_channelVisibleParas[i].get());
    removeParameter(m_doubleChannelRangeParas[i].get());
  }
  m_channelVisibleParas.clear();
  m_channelColorParas.clear();
  m_doubleChannelRangeParas.clear();

  glm::dvec2 dr(m_imgPack->rangeMin(), m_imgPack->rangeMax());
  if (m_imgPack->hasMinMax() && m_imgPack->maxIntensity() > m_imgPack->minIntensity()) {
    dr = glm::dvec2(m_imgPack->minIntensity(), m_imgPack->maxIntensity());
  }

  for (size_t c = 0; c < m_imgPack->imgInfo().numChannels; ++c) {
    m_channelVisibleParas.emplace_back(
      std::make_unique<ZBoolParameter>(QString("Show %1").arg(m_imgPack->imgInfo().displayChannelName(c)), true));
    m_doubleChannelRangeParas.emplace_back(std::make_unique<ZDoubleSpanParameter>(
      QString("%1 Display Range").arg(m_imgPack->imgInfo().displayChannelName(c)),
      dr,
      m_imgPack->rangeMin(), m_imgPack->rangeMax()));
    if (m_imgPack->imgInfo().voxelFormat != VoxelFormat::Float) {
      m_doubleChannelRangeParas[m_doubleChannelRangeParas.size() - 1]->setDecimal(0);
      m_doubleChannelRangeParas[m_doubleChannelRangeParas.size() - 1]->setSingleStep(1);
    }
    m_channelColorParas.emplace_back(
      std::make_unique<ZVec3Parameter>(QString("%1 Color").arg(m_imgPack->imgInfo().displayChannelName(c)),
                                       glm::vec3(m_imgPack->imgInfo().channelColors[c].r / 255.,
                                                 m_imgPack->imgInfo().channelColors[c].g / 255.,
                                                 m_imgPack->imgInfo().channelColors[c].b / 255.),
                                       glm::vec3(0.f), glm::vec3(1.f)));
    m_channelColorParas[c]->setStyle("COLOR");
    connect(m_channelVisibleParas[c].get(), &ZBoolParameter::boolChanged, m_doubleChannelRangeParas[c].get(),
            &ZDoubleSpanParameter::setEnabled);
    connect(m_channelVisibleParas[c].get(), &ZBoolParameter::boolChanged, m_channelColorParas[c].get(),
            &ZVec3Parameter::setEnabled);
    connect(m_channelVisibleParas[c].get(), &ZBoolParameter::valueChanged, this, &ZImgFilter::channelVisibleChanged);
    connect(m_doubleChannelRangeParas[c].get(), &ZDoubleSpanParameter::valueChanged, this,
            &ZImgFilter::channelRangeChanged);
    connect(m_channelColorParas[c].get(), &ZVec3Parameter::valueChanged, this, &ZImgFilter::channelColorChanged);

    if (m_imgPack->imgInfo().isAlphaChannel(c)) {
      m_channelColorParas[c]->setVisible(false);
    }
  }

  m_display.reset(new ZImgPackDisplay(*m_imgPack, false));
  if (m_view.isMaxZProjView() && m_imgPack->imgInfo().depth > 1) {
    m_maxZProjDisplay.reset(new ZImgPackDisplay(*m_imgPack, true));
  }
  for (size_t c = 0; c < m_imgPack->imgInfo().numChannels; ++c) {
    m_display->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
    //m_display->setAlpha(m_opacity.get());
    if (m_maxZProjDisplay) {
      m_maxZProjDisplay->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
      //m_maxZProjDisplay->setAlpha(m_opacity.get());
    }

    addParameter(m_channelVisibleParas[c].get());
    addParameter(m_channelColorParas[c].get());
    addParameter(m_doubleChannelRangeParas[c].get());
  }
  if (m_view.isMaxZProjView()) {
    setMaxZProjView(m_view.currentTime());
  } else {
    setNormalView(m_view.currentSlice(), m_view.currentTime());
  }

  updateViewSettingWidgetsGroup();
}

void ZImgFilter::releaseItemsOwnership()
{
  m_item.release();
}

void ZImgFilter::setSelected(bool v)
{
  if (m_item && m_item->isSelected() != v) {
    m_item->setSelected(v);
  }
}

void ZImgFilter::setNormalView(int z, int t)
{
  bool isVisibleBefore = m_isVisible;
  int logicalZ = realZ(z);
  int logicalT = realT(t);
  m_sliceValid = logicalT >= 0 && logicalT < int(m_imgPack->imgInfo().numTimes) &&
                 logicalZ >= 0 && logicalZ < int(m_imgPack->imgInfo().depth);
  m_isVisible = m_hasVisibleChannel && m_visible.get() && m_sliceValid;

  getDisplay()->setSlice(logicalZ);
  getDisplay()->setTime(logicalT);
  if (m_isVisible) {
    updateImgItems();
  } else if (isVisibleBefore) {
    hideImgItems();
  }
}

void ZImgFilter::setMaxZProjView(int t)
{
  bool isVisibleBefore = m_isVisible;
  int logicalT = realT(t);
  m_sliceValid = logicalT >= 0 && logicalT < int(m_imgPack->imgInfo().numTimes);
  m_isVisible = m_hasVisibleChannel && m_visible.get() && m_sliceValid;

  getDisplay()->setSlice(0);
  getDisplay()->setTime(logicalT);
  if (m_isVisible) {
    updateImgItems();
  } else if (isVisibleBefore) {
    hideImgItems();
  }
}

ZBBox<glm::ivec4> ZImgFilter::boundBox() const
{
  ZBBox<glm::ivec4> res;
  res.setMinCorner(glm::ivec4(0));
  res.setMaxCorner(glm::ivec4(int(m_imgPack->imgInfo().width) - 1,
                              int(m_imgPack->imgInfo().height) - 1,
                              int(m_imgPack->imgInfo().depth) - 1,
                              int(m_imgPack->imgInfo().numTimes) - 1));
  updateBoundBoxWithOffsetPara(res);
  return res;
}

int ZImgFilter::imgSlice() const
{
  return realZ();
}

int ZImgFilter::imgTime() const
{
  return realT();
}

std::shared_ptr<ZWidgetsGroup> ZImgFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>(m_imgPack->name(), 1);
    m_widgetsGroup->addChild(m_visible, 1);

    QPushButton* pb = new QPushButton("Bring to Front");
    connect(pb, &QPushButton::clicked, this, &ZImgFilter::bringToFront);
    m_widgetsGroup->addChild(*pb, 1);

    pb = new QPushButton("Send to Back");
    connect(pb, &QPushButton::clicked, this, &ZImgFilter::sendToBack);
    m_widgetsGroup->addChild(*pb, 1);

    m_widgetsGroup->addChild(m_viewPrecedencePara, 1);
    for (size_t i = 0; i < m_channelVisibleParas.size(); ++i) {
      m_widgetsGroup->addChild(*m_channelVisibleParas[i], 1);
      m_widgetsGroup->addChild(*m_channelColorParas[i], 1);
      m_widgetsGroup->addChild(*m_doubleChannelRangeParas[i], 1);
    }

    pb = new QPushButton(ZTheme::instance().icon(ZTheme::FlipHorizontalIcon), "Flip Horizontally");
    connect(pb, &QPushButton::clicked, this, &ZImgFilter::flipHorizontally);
    m_widgetsGroup->addChild(*pb, 1);

    pb = new QPushButton(ZTheme::instance().icon(ZTheme::FlipVerticalIcon), "Flip Vertically");
    connect(pb, &QPushButton::clicked, this, &ZImgFilter::flipVertically);
    m_widgetsGroup->addChild(*pb, 1);

    m_widgetsGroup->addChild(m_transform, 1);
    m_widgetsGroup->addChild(m_offsetPara, 1);
    m_widgetsGroup->addChild(m_opacity, 1);
    m_widgetsGroup->setBasicAdvancedCutoff(5);
  }
  return m_widgetsGroup;
}

void ZImgFilter::viewPrecedenceChanged()
{
  if (m_item) {
    m_item->setZValue(m_viewPrecedencePara.get());
  }
  ZObjFilter::viewPrecedenceChanged();
}

void ZImgFilter::transformChanged()
{
  if (m_item) {
    m_item->setTransform(getQTransform());
    viewportChanged();
  }
  ZObjFilter::transformChanged();
}

void ZImgFilter::offsetChanged()
{
  m_displayValid = false;
  if (m_view.isMaxZProjView()) {
    setMaxZProjView(m_view.currentTime());
  } else {
    setNormalView(m_view.currentSlice(), m_view.currentTime());
  }
  ZObjFilter::offsetChanged();
}

void ZImgFilter::updateViewSettingWidgetsGroup()
{
  if (m_widgetsGroup) {
    m_widgetsGroup->removeAllChildren();

    m_widgetsGroup->addChild(m_visible, 1);

    QPushButton* pb = new QPushButton("Bring to Front");
    connect(pb, &QPushButton::clicked, this, &ZImgFilter::bringToFront);
    m_widgetsGroup->addChild(*pb, 1);

    pb = new QPushButton("Send to Back");
    connect(pb, &QPushButton::clicked, this, &ZImgFilter::sendToBack);
    m_widgetsGroup->addChild(*pb, 1);

    m_widgetsGroup->addChild(m_viewPrecedencePara, 1);
    for (size_t i = 0; i < m_channelVisibleParas.size(); ++i) {
      m_widgetsGroup->addChild(*m_channelVisibleParas[i], 1);
      m_widgetsGroup->addChild(*m_channelColorParas[i], 1);
      m_widgetsGroup->addChild(*m_doubleChannelRangeParas[i], 1);
    }

    pb = new QPushButton(ZTheme::instance().icon(ZTheme::FlipHorizontalIcon), "Flip Horizontally");
    connect(pb, &QPushButton::clicked, this, &ZImgFilter::flipHorizontally);
    m_widgetsGroup->addChild(*pb, 1);

    pb = new QPushButton(ZTheme::instance().icon(ZTheme::FlipVerticalIcon), "Flip Vertically");
    connect(pb, &QPushButton::clicked, this, &ZImgFilter::flipVertically);
    m_widgetsGroup->addChild(*pb, 1);

    m_widgetsGroup->addChild(m_transform, 1);
    m_widgetsGroup->addChild(m_offsetPara, 1);
    m_widgetsGroup->addChild(m_opacity, 1);
    m_widgetsGroup->setBasicAdvancedCutoff(5);

    m_widgetsGroup->emitWidgetsGroupChangedSignal();
  }
}

void ZImgFilter::channelVisibleChanged()
{
  m_hasVisibleChannel = false;
  for (size_t c = 0; c < m_channelVisibleParas.size(); ++c) {
    m_hasVisibleChannel = m_hasVisibleChannel || m_channelVisibleParas[c]->get();
  }
  m_isVisible = m_hasVisibleChannel && m_visible.get() && m_sliceValid;

  if (ZBoolParameter* para = qobject_cast<ZBoolParameter*>(sender())) {
    // find which channel send the signal
    size_t c = 0;
    for (; c < m_channelVisibleParas.size(); ++c) {
      if (m_channelVisibleParas[c].get() == para)
        break;
    }
    if (m_channelVisibleParas[c]->get()) {
      m_display->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
      if (m_maxZProjDisplay)
        m_maxZProjDisplay->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
    } else {
      m_display->hideChannel(c);
      if (m_maxZProjDisplay)
        m_maxZProjDisplay->hideChannel(c);
    }
    m_displayValid = false;
  } else {
    CHECK(false);
  }
  if (!m_isVisible) {
    destroyImgItems(); // will create new one next time
  } else {
    updateImgItems();
  }
}

void ZImgFilter::channelRangeChanged()
{
  if (ZDoubleSpanParameter* para = qobject_cast<ZDoubleSpanParameter*>(sender())) {
    // find which channel send the signal
    size_t c = 0;
    for (; c < m_doubleChannelRangeParas.size(); ++c) {
      if (m_doubleChannelRangeParas[c].get() == para)
        break;
    }
    if (m_channelVisibleParas[c]->get()) {  // only redraw if this channel is visible
      m_display->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
      if (m_maxZProjDisplay)
        m_maxZProjDisplay->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
      m_displayValid = false;
    }
  }
  if (!m_isVisible) {
    destroyImgItems(); // will create new one next time
  } else {
    updateImgItems();
  }
}

void ZImgFilter::channelColorChanged()
{
  if (ZVec3Parameter* para = qobject_cast<ZVec3Parameter*>(sender())) {
    // find which channel send the signal
    size_t c = 0;
    for (; c < m_channelColorParas.size(); ++c) {
      if (m_channelColorParas[c].get() == para)
        break;
    }
    if (m_channelVisibleParas[c]->get()) {  // only redraw if this channel is visible
      col4 col(para->get().r * 255, para->get().g * 255, para->get().b * 255);
      m_imgPack->setChannelColor(c, col);
      m_displayValid = false;
    }
  } else {
    CHECK(false);
  }
  if (!m_isVisible) {
    destroyImgItems(); // will create new one next time
  } else {
    updateImgItems();
  }
}

void ZImgFilter::opacityChanged()
{
  //for (const auto& item : m_imgItems) {
  //  item->setOpacity(m_opacity.get());
  //}
  if (m_item) {
    m_item->setOpacity(m_opacity.get());
  }
//  m_display->setAlpha(m_opacity.get());
//  if (m_maxZProjDisplay)
//    m_maxZProjDisplay->setAlpha(m_opacity.get());
//  m_displayValid = false;
//  if (!m_isVisible) {
//    destroyImgItems(); // will create new one next time
//  } else {
//    updateImgItems();
//  }
}

void ZImgFilter::visibleChanged()
{
  bool isVisibleBefore = m_isVisible;
  m_isVisible = m_hasVisibleChannel && m_visible.get() && m_sliceValid;
  if (m_isVisible && !isVisibleBefore) {
    updateImgItems();
  } else if (!m_isVisible && isVisibleBefore) {
    hideImgItems();
  }
}

ZImgPackDisplay* ZImgFilter::getDisplay() const
{
  if (m_view.isMaxZProjView() && m_imgPack->imgInfo().depth > 1) {
    if (!m_maxZProjDisplay) {
      m_maxZProjDisplay.reset(new ZImgPackDisplay(*m_imgPack, true));
      m_maxZProjDisplay->hideAllChannels();
      for (size_t c = 0; c < m_channelVisibleParas.size(); ++c) {
        if (m_channelVisibleParas[c]->get()) {
          m_maxZProjDisplay->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
        }
      }
      m_maxZProjDisplay->setSlice(0);
      m_maxZProjDisplay->setTime(realT());
    }
    return m_maxZProjDisplay.get();
  } else {
    return m_display.get();
  }
}

void ZImgFilter::hideImgItems()
{
  //for (size_t i = 0; i < m_imgItems.size(); ++i) {
  //  m_imgItems[i]->setVisible(false);
  //}
  if (m_item) {
    m_item->setVisible(false);
  }
}

void ZImgFilter::destroyImgItems()
{
  m_item.reset();
  m_imgItems.clear();
}

void ZImgFilter::updateImgItems()
{
  CHECK(m_isVisible);

  ZImgPackDisplay* curDisplay = getDisplay();
  //LOG(INFO) << curDisplay->slice() << " " << m_lastSlice << " " << m_view.currentSlice();
  if (!m_imgItems.empty() && m_displayValid &&
      curDisplay == m_lastDisplay && m_lastSlice == m_view.currentSlice() && m_lastTime == m_view.currentTime()) {
    //LOG(INFO) << "0";
    // pixmap is same, we only need to show it
    if (m_item && !m_item->isVisible()) {
      m_item->setVisible(true);
    }
  } else {
    destroyImgItems();

    curDisplay->setScale(m_view.currentScale());
    QRectF vp = m_view.currentViewport();
    curDisplay->setViewport(mapFromSceneRect(vp));

    m_item = std::make_unique<ZGraphicsItemGroup>();
    m_view.scene().addItem(m_item.get());
    const ZQImagePack& qImagePack = curDisplay->toQImagePack();
    for (size_t i = 0; i < qImagePack.numImages(); ++i) {
      m_imgItems.push_back(new QGraphicsPixmapItem(QPixmap::fromImage(qImagePack.image(i))));
      //m_imgItems[i]->setFlag(QGraphicsItem::ItemIsSelectable, true);
      m_imgItems[i]->setScale(qImagePack.scale(i));
      m_imgItems[i]->setPos(QPointF(qImagePack.location(i)));
      m_imgItems[i]->setOpacity(m_opacity.get());
      m_imgItems[i]->setVisible(m_isVisible);
      m_item->addToGroup(m_imgItems[i]);
    }
    m_item->setZValue(m_viewPrecedencePara.get());
    m_item->setTransform(getQTransform());

    m_lastDisplay = curDisplay;
    m_lastSlice = m_lastDisplay->slice();
    m_lastTime = m_lastDisplay->time();
    m_lastScale = m_lastDisplay->scale();
    m_lastViewport = m_lastDisplay->viewport();
  }
  m_displayValid = true;
}

double ZImgFilter::getLowerChannelRange(size_t c) const
{
  return m_doubleChannelRangeParas[c]->get().x;
}

double ZImgFilter::getUpperChannelRange(size_t c) const
{
  return m_doubleChannelRangeParas[c]->get().y;
}

void ZImgFilter::viewportChanged()
{
  QRectF vp = mapFromSceneRect(m_view.currentViewport());
  if (m_imgPack->needUpdate(vp, m_view.currentScale(), m_lastViewport, m_lastScale,
                            realT(), realZ(), m_view.isMaxZProjView())) {
    if (!m_isVisible) {
      destroyImgItems(); // will create new one next time
    } else {
      m_displayValid = false;
      updateImgItems();
    }
  }
}

void ZImgFilter::flipHorizontally()
{
  if (m_item && m_item->isVisible()) {
    m_transform.flipHorizontally(QRectF(0, 0, m_imgPack->imgInfo().width, m_imgPack->imgInfo().height));
  }
}

void ZImgFilter::flipVertically()
{
  if (m_item && m_item->isVisible()) {
    m_transform.flipVertically(QRectF(0, 0, m_imgPack->imgInfo().width, m_imgPack->imgInfo().height));
  }
}

} // namespace nim
