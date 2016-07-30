#include "zimgfilter.h"

#include <QGraphicsPixmapItem>
#include "zimg.h"
#include "zimgdoc.h"
#include "zimgview.h"
#include "zimgpackdisplay.h"
#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include "zlog.h"

namespace nim {

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
  addParameter(&m_offsetPara);
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

  m_offsetPara.blockSignals(true);
  m_offsetPara.set(glm::dvec4(m_imgPack->offsetX(), m_imgPack->offsetY(),
                              m_imgPack->offsetZ(), m_imgPack->offsetT()));
  m_offsetPara.blockSignals(false);

  m_display.reset(new ZImgPackDisplay(*m_imgPack, false));
  if (m_view.isMaxZProjView() && m_imgPack->imgInfo().depth > 1) {
    m_maxZProjDisplay.reset(new ZImgPackDisplay(*m_imgPack, true));
  }
  for (size_t c = 0; c < m_imgPack->imgInfo().numChannels; ++c) {
    m_display->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
    m_display->setAlpha(m_opacity.get());
    if (m_maxZProjDisplay) {
      m_maxZProjDisplay->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
      m_maxZProjDisplay->setAlpha(m_opacity.get());
    }

    addParameter(m_channelVisibleParas[c].get());
    addParameter(m_channelColorParas[c].get());
    addParameter(m_doubleChannelRangeParas[c].get());
  }
  if (m_view.isMaxZProjView())
    setMaxZProjView(m_view.currentTime());
  else
    setNormalView(m_view.currentSlice(), m_view.currentTime());

  updateViewSettingWidgetsGroup();
}

void ZImgFilter::releaseItemsOwnership()
{
  for (size_t i = 0; i < m_imgItems.size(); ++i) {
    m_imgItems[i].release();
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

void ZImgFilter::setViewport(const QRectF& rect, double scale)
{
  QRectF vp = rect;
  vp.moveTo(vp.x() - m_offsetPara.get().x, vp.y() - m_offsetPara.get().y);
  if (m_imgPack->needUpdate(vp, scale, m_lastViewport, m_lastScale, realT(), realZ(), m_view.isMaxZProjView())) {
    if (!m_isVisible) {
      destroyImgItems(); // will create new one next time
    } else {
      m_displayValid = false;
      updateImgItems();
    }
  }
}

std::vector<int> ZImgFilter::boundBox() const
{
  std::vector<int> res(8);
  res[0] = 0;
  res[1] = int(m_imgPack->imgInfo().width) - 1;
  res[2] = 0;
  res[3] = int(m_imgPack->imgInfo().height) - 1;
  res[4] = 0;
  res[5] = int(m_imgPack->imgInfo().depth) - 1;
  res[6] = 0;
  res[7] = int(m_imgPack->imgInfo().numTimes) - 1;
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
    for (size_t i = 0; i < m_channelVisibleParas.size(); ++i) {
      m_widgetsGroup->addChild(*m_channelVisibleParas[i], 1);
      m_widgetsGroup->addChild(*m_channelColorParas[i], 1);
      m_widgetsGroup->addChild(*m_doubleChannelRangeParas[i], 1);
    }
    m_widgetsGroup->addChild(m_offsetPara, 1);
    m_widgetsGroup->addChild(m_opacity, 1);
    m_widgetsGroup->setBasicAdvancedCutoff(5);
  }
  return m_widgetsGroup;
}

void ZImgFilter::offsetChanged()
{
  m_imgPack->setOffsetX(m_offsetPara.get().x);
  m_imgPack->setOffsetY(m_offsetPara.get().y);
  m_imgPack->setOffsetZ(m_offsetPara.get().z);
  m_imgPack->setOffsetT(m_offsetPara.get().w);
  ZObjFilter::offsetChanged();
  m_displayValid = false;
  if (m_view.isMaxZProjView())
    setMaxZProjView(m_view.currentTime());
  else
    setNormalView(m_view.currentSlice(), m_view.currentTime());
}

void ZImgFilter::updateViewSettingWidgetsGroup()
{
  if (m_widgetsGroup) {
    m_widgetsGroup->removeAllChildren();

    m_widgetsGroup->addChild(m_visible, 1);
    for (size_t i = 0; i < m_channelVisibleParas.size(); ++i) {
      m_widgetsGroup->addChild(*m_channelVisibleParas[i], 1);
      m_widgetsGroup->addChild(*m_channelColorParas[i], 1);
      m_widgetsGroup->addChild(*m_doubleChannelRangeParas[i], 1);
    }
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

  ZBoolParameter* para = qobject_cast<ZBoolParameter*>(sender());
  if (para) {
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
  ZDoubleSpanParameter* para = qobject_cast<ZDoubleSpanParameter*>(sender());
  if (para) {
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
  ZVec3Parameter* para = qobject_cast<ZVec3Parameter*>(sender());
  if (para) {
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
  m_display->setAlpha(m_opacity.get());
  if (m_maxZProjDisplay)
    m_maxZProjDisplay->setAlpha(m_opacity.get());
  m_displayValid = false;
  if (!m_isVisible) {
    destroyImgItems(); // will create new one next time
  } else {
    updateImgItems();
  }
}

void ZImgFilter::visibleChanged()
{
  bool isVisibleBefore = m_isVisible;
  m_isVisible = m_hasVisibleChannel && m_visible.get() && m_sliceValid;
  if (m_isVisible && !isVisibleBefore)
    updateImgItems();
  else if (!m_isVisible && isVisibleBefore)
    hideImgItems();
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
  for (size_t i = 0; i < m_imgItems.size(); ++i) {
    m_imgItems[i]->setVisible(false);
  }
}

void ZImgFilter::destroyImgItems()
{
  for (size_t i = 0; i < m_imgItems.size(); ++i) {
    m_view.scene().removeItem(m_imgItems[i].get());
  }
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
    if (!m_imgItems[0]->isVisible()) {
      for (size_t i = 0; i < m_imgItems.size(); ++i) {
        m_imgItems[i]->setVisible(true);
      }
    }
  } else {
    destroyImgItems();

    curDisplay->setScale(m_view.currentScale());
    QRectF vp = m_view.currentViewport();
    vp.moveTo(vp.x() - m_offsetPara.get().x, vp.y() - m_offsetPara.get().y);
    curDisplay->setViewport(vp);

    const ZQImagePack& qImagePack = curDisplay->toQImagePack();
    for (size_t i = 0; i < qImagePack.numImages(); ++i) {
      m_imgItems.emplace_back(std::make_unique<QGraphicsPixmapItem>(QPixmap::fromImage(qImagePack.image(i))));
      m_imgItems[i]->setScale(qImagePack.scale(i));
      m_imgItems[i]->setPos(QPointF(qImagePack.location(i)) + QPointF(m_offsetPara.get().x, m_offsetPara.get().y));
      m_imgItems[i]->setVisible(m_isVisible);
      m_view.scene().addItem(m_imgItems[i].get());
    }

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

} // namespace nim
