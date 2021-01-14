#include "zimgfilter.h"

#include "zimg.h"
#include "zimgdoc.h"
#include "zimgview.h"
#include "zimgpackdisplay.h"
#include "zlog.h"
#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include "ztheme.h"
#include "zgraphicsview.h"
#include <QGraphicsPixmapItem>
#include <QStyleOption>
#include <QPainter>
#include <QPushButton>

namespace nim {

ZImgScaleBarGraphicsItem::ZImgScaleBarGraphicsItem(double length, double height, double voxelSizeXInUm,
                                                   double viewScale, double transformScale,
                                                   const QRectF& viewPort, const glm::vec3& color,
                                                   QGraphicsItem* parent)
  : QGraphicsRectItem(parent)
  , m_lengthInUm(length)
  , m_height(height)
  , m_voxelSizeXInUm(voxelSizeXInUm)
  , m_viewScale(viewScale)
  , m_transformScale(transformScale)
  , m_viewPort(viewPort)
  , m_viewPortPos(.8, .8)
{
  setFlags(QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsMovable |
           QGraphicsItem::ItemIsSelectable);

  setToolTip(QString("length: %1 µm (voxel size: %2 µm)").arg(m_lengthInUm).arg(m_voxelSizeXInUm));
  setColor(color);
  setCursor(Qt::PointingHandCursor);
  updateRectSize();
  updatePos();
}

QVariant ZImgScaleBarGraphicsItem::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value)
{
  if (change == ItemPositionChange && scene()) {
    //LOG(INFO) << value.toPointF();
    QPointF newPos = value.toPointF();
    //LOG(INFO) << mapFromScene(newPos);
    //auto vp = mapRectFromScene(m_viewPort);
    //LOG(INFO) << m_viewPort.topLeft() << m_viewPort.bottomRight() << vp.topLeft() << vp.bottomRight();
    newPos = QPointF(qMin(m_viewPort.right(), qMax(newPos.x(), m_viewPort.left())),
                     qMin(m_viewPort.bottom(), qMax(newPos.y(), m_viewPort.top())));
//    newPos.setX(qMin(m_viewPort.right(), qMax(newPos.x(), m_viewPort.left())));
//    newPos.setY(qMin(m_viewPort.bottom(), qMax(newPos.y(), m_viewPort.top())));
    m_viewPortPos.x = (newPos.x() - m_viewPort.left()) / m_viewPort.width();
    m_viewPortPos.y = (newPos.y() - m_viewPort.top()) / m_viewPort.height();
    //LOG(INFO) << newPos;
    return newPos;
  }
  return QGraphicsRectItem::itemChange(change, value);
}

void ZImgScaleBarGraphicsItem::updateRectSize()
{
  double height = m_height / m_viewScale;
  double width = m_lengthInUm / m_voxelSizeXInUm * m_transformScale;
  QRectF rect(0, 0, width, height);
  setRect(rect);
}

void ZImgScaleBarGraphicsItem::updatePos()
{
  setPos(m_viewPort.left() + m_viewPort.width() * m_viewPortPos.x,
         m_viewPort.top() + m_viewPort.height() * m_viewPortPos.y);
}

ZImgFilter::ZImgFilter(ZView& view)
  : ZObjFilter(view)
  , m_imgPack(nullptr)
  , m_hasVisibleChannel(true)
  , m_isVisible(false)
  , m_opacity(QString("Opacity"), 1.0, 0.0, 1.0)
  , m_showScaleBar("Show Scale Bar", false)
  , m_scaleBarLengthInUm("Scale Bar Length", 10., 0.0001, 1e9)
  , m_scaleBarHeight("Scale Bar Height", 5, 1, 500)
  , m_scaleBarColor("Scale Bar Color", glm::vec3(1., 1., 1.))
  , m_displayValid(false)
  , m_lastSlice(-1)
  , m_lastTime(-1)
  , m_lastScale(0)
  , m_lastViewport()
{
  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZImgFilter::visibleChanged);
  connect(&m_opacity, &ZDoubleParameter::valueChanged, this, &ZImgFilter::opacityChanged);
  addParameter(&m_opacity);
  connect(&m_showScaleBar, &ZBoolParameter::valueChanged, this, &ZImgFilter::showScaleBarChanged);
  addParameter(&m_showScaleBar);
  m_scaleBarLengthInUm.setSuffix(" µm");
  m_scaleBarLengthInUm.setStyle("SPINBOX");
  connect(&m_scaleBarLengthInUm, &ZDoubleParameter::valueChanged, this, &ZImgFilter::scaleBarLengthChanged);
  addParameter(&m_scaleBarLengthInUm);
  m_scaleBarHeight.setSuffix(" pixels");
  m_scaleBarHeight.setStyle("SPINBOX");
  connect(&m_scaleBarHeight, &ZIntParameter::valueChanged, this, &ZImgFilter::scaleBarHeightChanged);
  addParameter(&m_scaleBarHeight);
  m_scaleBarColor.setStyle("COLOR");
  connect(&m_scaleBarColor, &ZVec3Parameter::valueChanged, this, &ZImgFilter::scaleBarColorChanged);
  addParameter(&m_scaleBarColor);
  m_viewPrecedencePara.blockSignals(true);
  m_viewPrecedencePara.set(getViewPrecedence());
  m_viewPrecedencePara.blockSignals(false);
  connect(&m_view, &ZView::viewportChanged, this, &ZImgFilter::viewportChanged);
  connect(&view.graphicsView(), &ZGraphicsView::scaleChanged, this, &ZImgFilter::viewScaleChanged);
}

void ZImgFilter::setData(ZImgPack& pack)
{
  m_imgPack = &pack;
  destroyImgItems();
  m_display.reset();

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

  if (m_mipRange) {
    m_mipRange->disconnect();
    removeParameter(m_mipRange.get());
  }
  m_mipRange = std::make_unique<ZIntSpanParameter>("Maximum Z Projection Range",
                                                   glm::ivec2(0, m_imgPack->imgInfo().depth - 1),
                                                   0,
                                                   m_imgPack->imgInfo().depth - 1);
  connect(m_mipRange.get(), &ZIntSpanParameter::valueChanged, this, &ZImgFilter::mipRangeChanged);
  addParameter(m_mipRange.get());

  m_display.reset(new ZImgPackDisplay(*m_imgPack));
  if (m_view.isMaxZProjView() && m_imgPack->imgInfo().depth > 1) {
    m_display->setMIP(true);
    m_display->setMIPZRange(m_mipRange->get().x, m_mipRange->get().y);
  }
  for (size_t c = 0; c < m_imgPack->imgInfo().numChannels; ++c) {
    m_display->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
    m_display->setChannelColor(c, m_imgPack->imgInfo().channelColors[c]);
    //m_display->setAlpha(m_opacity.get());

    addParameter(m_channelVisibleParas[c].get());
    addParameter(m_channelColorParas[c].get());
    addParameter(m_doubleChannelRangeParas[c].get());
  }

  if (m_imgPack->imgInfo().voxelSizeUnit != VoxelSizeUnit::none) {
    m_showScaleBar.setEnabled(true);
    m_scaleBarItem = std::make_unique<ZImgScaleBarGraphicsItem>(m_scaleBarLengthInUm.get(),
                                                                m_scaleBarHeight.get(),
                                                                m_imgPack->imgInfo().voxelSizeXInUm(),
                                                                m_view.currentScale(),
                                                                getTransformScale().x,
                                                                mapFromSceneRect(m_view.currentViewport()),
                                                                m_scaleBarColor.get());
    m_scaleBarItem->setVisible(false);
    m_scaleBarItem->setZValue(30000);
    m_view.scene().addItem(m_scaleBarItem.get());
  } else {
    m_showScaleBar.setEnabled(false);
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
  m_scaleBarItem.release();
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
  auto logicalZ = realZ(z);
  auto logicalT = realT(t);
  m_sliceValid = logicalT >= 0 && logicalT < int(m_imgPack->imgInfo().numTimes) &&
                 logicalZ >= 0 && logicalZ < int(m_imgPack->imgInfo().depth);
  m_isVisible = m_hasVisibleChannel && m_visible.get() && m_sliceValid;

  m_display->setSlice(logicalZ);
  m_display->setTime(logicalT);
  m_display->setMIP(false);
  if (m_isVisible) {
    updateImgItems();
  } else if (isVisibleBefore) {
    hideImgItems();
  }

  showScaleBarChanged();
}

void ZImgFilter::setMaxZProjView(int t)
{
  bool isVisibleBefore = m_isVisible;
  auto logicalT = realT(t);
  m_sliceValid = logicalT >= 0 && logicalT < int(m_imgPack->imgInfo().numTimes);
  m_isVisible = m_hasVisibleChannel && m_visible.get() && m_sliceValid;

  m_display->setSlice(0);
  m_display->setTime(logicalT);
  m_display->setMIP(true);
  m_display->setMIPZRange(m_mipRange->get().x, m_mipRange->get().y);
  if (m_isVisible) {
    updateImgItems();
  } else if (isVisibleBefore) {
    hideImgItems();
  }

  showScaleBarChanged();
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

    auto pb = new QPushButton("Bring to Front");
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
    m_widgetsGroup->addChild(*m_mipRange, 1);
    m_widgetsGroup->addChild(m_showScaleBar, 2);
    m_widgetsGroup->addChild(m_scaleBarLengthInUm, 2);
    m_widgetsGroup->addChild(m_scaleBarColor, 2);
    m_widgetsGroup->addChild(m_scaleBarHeight, 2);
    m_widgetsGroup->setBasicAdvancedCutoff(5);
  }
  return m_widgetsGroup;
}

void ZImgFilter::viewPrecedenceChanged()
{
  if (m_item) {
    m_item->setZValue(m_viewPrecedencePara.get());
  }
  if (m_scaleBarItem) {
    m_scaleBarItem->setZValue(30000);
  }
  ZObjFilter::viewPrecedenceChanged();
}

void ZImgFilter::transformChanged()
{
  if (m_item) {
    m_item->setTransform(getQTransform());
    if (m_scaleBarItem) {
      m_scaleBarItem->setTransformScale(getTransformScale().x);
    }
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

    auto pb = new QPushButton("Bring to Front");
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
    m_widgetsGroup->addChild(*m_mipRange, 1);
    m_widgetsGroup->addChild(m_showScaleBar, 2);
    m_widgetsGroup->addChild(m_scaleBarLengthInUm, 2);
    m_widgetsGroup->addChild(m_scaleBarColor, 2);
    m_widgetsGroup->addChild(m_scaleBarHeight, 2);
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
    } else {
      m_display->hideChannel(c);
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

  showScaleBarChanged();
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
      m_display->setChannelColor(c, col);
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

void ZImgFilter::mipRangeChanged()
{
  m_display->setMIPZRange(m_mipRange->get().x, m_mipRange->get().y);
  if (m_display->mip()) {
    m_displayValid = false;
    if (!m_isVisible) {
      destroyImgItems(); // will create new one next time
    } else {
      updateImgItems();
    }
  }
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

  showScaleBarChanged();
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

  //LOG(INFO) << curDisplay->slice() << " " << m_lastSlice << " " << m_view.currentSlice();
  if (!m_imgItems.empty() && m_displayValid &&
      m_lastMIP == m_view.isMaxZProjView() && m_lastSlice == m_view.currentSlice() &&
      m_lastTime == m_view.currentTime()) {
    //LOG(INFO) << "0";
    // pixmap is same, we only need to show it
    if (m_item && !m_item->isVisible()) {
      m_item->setVisible(true);
    }
  } else {
    destroyImgItems();

    m_display->setScale(m_view.currentScale() * std::max(getTransformScale().x, getTransformScale().y));
    QRectF vp = m_view.currentViewport();
    m_display->setViewport(mapFromSceneRect(vp));

    m_item = std::make_unique<ZGraphicsItemGroup>();
    m_view.scene().addItem(m_item.get());
    const ZQImagePack& qImagePack = m_display->toQImagePack();
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

    m_lastMIP = m_display->mip();
    m_lastSlice = m_display->slice();
    m_lastTime = m_display->time();
    m_lastScale = m_display->scale();
    m_lastViewport = m_display->viewport();
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
  if (m_imgPack->needUpdate(vp, m_view.currentScale() * std::max(getTransformScale().x, getTransformScale().y),
                            m_lastViewport, m_lastScale,
                            realT(), realZ(), m_view.isMaxZProjView())) {
    if (!m_isVisible) {
      destroyImgItems(); // will create new one next time
    } else {
      m_displayValid = false;
      updateImgItems();
    }
  }
  if (m_scaleBarItem)
    m_scaleBarItem->setViewPort(m_view.currentViewport());
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

void ZImgFilter::showScaleBarChanged()
{
  if (m_scaleBarItem) {
    m_scaleBarItem->setVisible(m_isVisible & m_showScaleBar.get());
  }
}

void ZImgFilter::scaleBarLengthChanged()
{
  if (m_scaleBarItem) {
    m_scaleBarItem->setLengthInUm(m_scaleBarLengthInUm.get());
  }
}

void ZImgFilter::scaleBarHeightChanged()
{
  if (m_scaleBarItem) {
    m_scaleBarItem->setHeight(m_scaleBarHeight.get());
  }
}

void ZImgFilter::scaleBarColorChanged()
{
  if (m_scaleBarItem) {
    m_scaleBarItem->setColor(m_scaleBarColor.get());
  }
}

void ZImgFilter::viewScaleChanged(double s)
{
  if (m_scaleBarItem) {
    m_scaleBarItem->setViewScale(s);
  }
}

} // namespace nim
