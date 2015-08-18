#include "zimgfilter.h"

#include <QGraphicsPixmapItem>
#include "zimg.h"
#include "zimgdoc.h"
#include "zimgview.h"
#include "zimgpackdisplay.h"
#include "znumericparameter.h"
#include "zwidgetsgroup.h"

namespace nim {

ZImgFilter::ZImgFilter(ZView &view)
  : ZObjFilter(view)
  , m_imgPack(nullptr)
  , m_visible("Visible", true)
  , m_hasVisibleChannel(true)
  , m_isVisible(false)
  , m_offsetPara(nullptr)
  , m_opacity(QString("Opacity"), 1.0, 0.0, 1.0)
  , m_display(nullptr)
  , m_maxZProjDisplay(nullptr)
  , m_displayValid(false)
  , m_lastDisplay(nullptr)
  , m_lastSlice(-1)
  , m_lastTime(-1)
  , m_lastScale(0)
  , m_lastViewport()
  , m_widgetsGroup(nullptr)
{
  connect(&m_visible, SIGNAL(valueChanged()), this, SLOT(visibleChanged()));
  addParameter(&m_visible);
  connect(&m_opacity, SIGNAL(valueChanged()), this, SLOT(opacityChanged()));
  addParameter(&m_opacity);
}

ZImgFilter::~ZImgFilter()
{
  destroyImgItems();
  delete m_display;
  delete m_maxZProjDisplay;
}

void ZImgFilter::setData(ZImgPack &pack)
{
  m_imgPack = &pack;
  destroyImgItems();
  delete m_display;
  delete m_maxZProjDisplay;

  for(int i=0; i<m_channelVisibleParas.size(); ++i) {
    m_channelColorParas[i]->disconnect();
    m_channelVisibleParas[i]->disconnect();
    m_doubleChannelRangeParas[i]->disconnect();
    removeParameter(m_channelColorParas[i]);
    removeParameter(m_channelVisibleParas[i]);
    removeParameter(m_doubleChannelRangeParas[i]);
    delete m_channelVisibleParas[i];
    delete m_channelColorParas[i];
    delete m_doubleChannelRangeParas[i];
  }
  m_channelVisibleParas.clear();
  m_channelColorParas.clear();
  m_doubleChannelRangeParas.clear();

  glm::dvec2 dr(m_imgPack->rangeMin(), m_imgPack->rangeMax());
  if (m_imgPack->hasMinMax() && m_imgPack->maxIntensity() > m_imgPack->minIntensity()) {
    dr = glm::dvec2(m_imgPack->minIntensity(), m_imgPack->maxIntensity());
  }

  for (size_t c=0; c<m_imgPack->imgInfo().numChannels; ++c) {
    m_channelVisibleParas.push_back(new ZBoolParameter(QString("Show %1").arg(m_imgPack->imgInfo().channelNames[c]), true, this));
    m_doubleChannelRangeParas.push_back(new ZDoubleSpanParameter(QString("%1 Display Range").arg(m_imgPack->imgInfo().channelNames[c]),
                                                                 dr,
                                                                 m_imgPack->rangeMin(), m_imgPack->rangeMax(), this));
    if (m_imgPack->imgInfo().voxelFormat != VoxelFormat::Float) {
      m_doubleChannelRangeParas[m_doubleChannelRangeParas.size()-1]->setDecimal(0);
      m_doubleChannelRangeParas[m_doubleChannelRangeParas.size()-1]->setSingleStep(1);
    }
    m_channelColorParas.push_back(new ZVec3Parameter(QString("%1 Color").arg(m_imgPack->imgInfo().channelNames[c]),
                                                     glm::vec3(m_imgPack->imgInfo().channelColors[c].r / 255.,
                                                               m_imgPack->imgInfo().channelColors[c].g / 255.,
                                                               m_imgPack->imgInfo().channelColors[c].b / 255.),
                                                     glm::vec3(0.f), glm::vec3(1.f), this));
    m_channelColorParas[c]->setStyle("COLOR");
    connect(m_channelVisibleParas[c], SIGNAL(valueChanged(bool)), m_doubleChannelRangeParas[c], SLOT(setEnabled(bool)));
    connect(m_channelVisibleParas[c], SIGNAL(valueChanged(bool)), m_channelColorParas[c], SLOT(setEnabled(bool)));
    connect(m_channelVisibleParas[c], SIGNAL(valueChanged()), this, SLOT(channelVisibleChanged()));
    connect(m_doubleChannelRangeParas[c], SIGNAL(valueChanged()), this, SLOT(channelRangeChanged()));
    connect(m_channelColorParas[c], SIGNAL(valueChanged()), this, SLOT(channelColorChanged()));

    if (m_imgPack->imgInfo().isAlphaChannel(c)) {
      m_channelColorParas[c]->setVisible(false);
    }
  }
  if (!m_offsetPara) {  // keep old offset
    m_offsetPara = new ZDVec4Parameter(QString("Offset"), glm::dvec4(m_imgPack->offsetX(), m_imgPack->offsetY(),
                                                                     m_imgPack->offsetZ(), m_imgPack->offsetT()),
                                       glm::dvec4(std::numeric_limits<int>::min()),
                                       glm::dvec4(std::numeric_limits<int>::max()),
                                       this);
    m_offsetPara->setDecimal(0);
    m_offsetPara->setSingleStep(1);
    m_offsetPara->setStyle("SPINBOX");
    connect(m_offsetPara, SIGNAL(valueChanged()), this, SLOT(offsetChanged()));
  }

  m_display = new ZImgPackDisplay(*m_imgPack, false);
  if (m_view.isMaxZProjView() && m_imgPack->imgInfo().depth > 1) {
    m_maxZProjDisplay = new ZImgPackDisplay(*m_imgPack, true);
  }
  for (size_t c=0; c<m_imgPack->imgInfo().numChannels; ++c) {
    m_display->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
    m_display->setAlpha(m_opacity.get());
    if (m_maxZProjDisplay) {
      m_maxZProjDisplay->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
      m_maxZProjDisplay->setAlpha(m_opacity.get());
    }

    addParameter(m_channelVisibleParas[c]);
    addParameter(m_channelColorParas[c]);
    addParameter(m_doubleChannelRangeParas[c]);
  }
  addParameter(m_offsetPara);
  if (m_view.isMaxZProjView())
    setMaxZProjView(m_view.currentTime());
  else
    setNormalView(m_view.currentSlice(), m_view.currentTime());

  updateViewSettingWidgetsGroup();
}

void ZImgFilter::setNormalView(int z, int t)
{
  bool isVisibleBefore = m_isVisible;
  int logicalZ = m_view.isMaxZProjView() ? 0 : int(z) - int(m_offsetPara->get().z);
  int logicalT = int(t) - int(m_offsetPara->get().w);
  m_sliceValid = logicalT >= 0 && logicalT < int(m_imgPack->imgInfo().numTimes);
  if (m_view.isNormalView())
    m_sliceValid = m_sliceValid && logicalZ >= 0 && logicalZ < int(m_imgPack->imgInfo().depth);
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
  setNormalView(0, t);
}

void ZImgFilter::setViewport(const QRectF &rect, double scale)
{
  QRectF vp = rect;
  vp.moveTo(vp.x()-m_offsetPara->get().x, vp.y()-m_offsetPara->get().y);
  if (m_imgPack->needUpdate(vp, scale, m_lastViewport, m_lastScale)) {
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
  res[0] = m_offsetPara->get().x;
  res[1] = m_offsetPara->get().x + int(m_imgPack->imgInfo().width) - 1;
  res[2] = m_offsetPara->get().y;
  res[3] = m_offsetPara->get().y + int(m_imgPack->imgInfo().height) - 1;
  res[4] = m_offsetPara->get().z;
  res[5] = m_offsetPara->get().z + int(m_imgPack->imgInfo().depth) - 1;
  res[6] = m_offsetPara->get().w;
  res[7] = m_offsetPara->get().w + int(m_imgPack->imgInfo().numTimes) - 1;
  return res;
}

int ZImgFilter::imgSlice() const
{
  return int(m_view.currentSlice()) - int(m_offsetPara->get().z);
}

int ZImgFilter::imgTime() const
{
  return int(m_view.currentTime()) - int(m_offsetPara->get().w);
}

ZWidgetsGroup *ZImgFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = new ZWidgetsGroup(m_imgPack->name(), nullptr, 1);
    new ZWidgetsGroup(&m_visible, m_widgetsGroup, 1);
    for(int i=0; i<m_channelVisibleParas.size(); ++i) {
      new ZWidgetsGroup(m_channelVisibleParas[i], m_widgetsGroup, 1);
      new ZWidgetsGroup(m_channelColorParas[i], m_widgetsGroup, 1);
      new ZWidgetsGroup(m_doubleChannelRangeParas[i], m_widgetsGroup, 1);
    }
    new ZWidgetsGroup(m_offsetPara, m_widgetsGroup, 1);
    new ZWidgetsGroup(&m_opacity, m_widgetsGroup, 1);
    m_widgetsGroup->setBasicAdvancedCutoff(5);
  }
  return m_widgetsGroup;
}

void ZImgFilter::updateViewSettingWidgetsGroup()
{
  if (m_widgetsGroup) {
    m_widgetsGroup->deleteAllChildGroups();

    new ZWidgetsGroup(&m_visible, m_widgetsGroup, 1);
    for(int i=0; i<m_channelVisibleParas.size(); ++i) {
      new ZWidgetsGroup(m_channelVisibleParas[i], m_widgetsGroup, 1);
      new ZWidgetsGroup(m_channelColorParas[i], m_widgetsGroup, 1);
      new ZWidgetsGroup(m_doubleChannelRangeParas[i], m_widgetsGroup, 1);
    }
    new ZWidgetsGroup(m_offsetPara, m_widgetsGroup, 1);
    new ZWidgetsGroup(&m_opacity, m_widgetsGroup, 1);
    m_widgetsGroup->setBasicAdvancedCutoff(5);

    m_widgetsGroup->emitWidgetsGroupChangedSignal();
  }
}

void ZImgFilter::channelVisibleChanged()
{
  m_hasVisibleChannel = false;
  for (int c=0; c<m_channelVisibleParas.size(); ++c) {
    m_hasVisibleChannel = m_hasVisibleChannel || m_channelVisibleParas[c]->get();
  }
  m_isVisible = m_hasVisibleChannel && m_visible.get() && m_sliceValid;

  ZBoolParameter *para = qobject_cast<ZBoolParameter*>(sender());
  if (para) {
    // find which channel send the signal
    int c = m_channelVisibleParas.indexOf(para);
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
    assert(false);
  }
  if (!m_isVisible) {
    destroyImgItems(); // will create new one next time
  } else {
    updateImgItems();
  }
}

void ZImgFilter::channelRangeChanged()
{
  int c = -1;
  ZDoubleSpanParameter *para = qobject_cast<ZDoubleSpanParameter*>(sender());
  if (para) {
    // find which channel send the signal
    c = m_doubleChannelRangeParas.indexOf(para);
  }
  if (c >= 0) {
    if (m_channelVisibleParas[c]->get()) {  // only redraw if this channel is visible
      m_display->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
      if (m_maxZProjDisplay)
        m_maxZProjDisplay->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
      m_displayValid = false;
    }
  } else {
    assert(false);
  }
  if (!m_isVisible) {
    destroyImgItems(); // will create new one next time
  } else {
    updateImgItems();
  }
}

void ZImgFilter::channelColorChanged()
{
  ZVec3Parameter *para = qobject_cast<ZVec3Parameter*>(sender());
  if (para) {
    // find which channel send the signal
    int c = m_channelColorParas.indexOf(para);
    if (m_channelVisibleParas[c]->get()) {  // only redraw if this channel is visible
      col4 col(para->get().r * 255, para->get().g * 255, para->get().b * 255);
      m_imgPack->imgInfoRef().channelColors[c] = col;
      m_displayValid = false;
    }
  } else {
    assert(false);
  }
  if (!m_isVisible) {
    destroyImgItems(); // will create new one next time
  } else {
    updateImgItems();
  }
}

void ZImgFilter::offsetChanged()
{
  //m_doc.setImgOffset(m_id, m_offsetPara->get().x, m_offsetPara->get().y,
  //                    m_offsetPara->get().z, m_offsetPara->get().w);
  emit boundBoxChanged();
  m_displayValid = false;
  if (m_view.isMaxZProjView())
    setMaxZProjView(m_view.currentTime());
  else
    setNormalView(m_view.currentSlice(), m_view.currentTime());
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

ZImgPackDisplay *ZImgFilter::getDisplay() const
{
  if (m_view.isMaxZProjView() && m_imgPack->imgInfo().depth > 1) {
    if (!m_maxZProjDisplay) {
      m_maxZProjDisplay = new ZImgPackDisplay(*m_imgPack, true);
      m_maxZProjDisplay->hideAllChannels();
      for (int c=0; c<m_channelVisibleParas.size(); ++c) {
        if (m_channelVisibleParas[c]->get()) {
          m_maxZProjDisplay->showChannel(c, getLowerChannelRange(c), getUpperChannelRange(c));
        }
      }
      m_maxZProjDisplay->setSlice(0);
      int logicalT = int(m_view.currentTime()) - int(m_offsetPara->get().w);
      m_maxZProjDisplay->setTime(logicalT);
    }
    return m_maxZProjDisplay;
  } else {
    return m_display;
  }
}

void ZImgFilter::hideImgItems()
{
  for (int i=0; i<m_imgItems.size(); ++i) {
    m_imgItems[i]->setVisible(false);
  }
}

void ZImgFilter::destroyImgItems()
{
  for (int i=0; i<m_imgItems.size(); ++i) {
    m_view.scene().removeItem(m_imgItems[i]);
    delete m_imgItems[i];
  }
  m_imgItems.clear();
}

void ZImgFilter::updateImgItems()
{
  assert(m_isVisible);

  ZImgPackDisplay *curDisplay = getDisplay();
  //LINFO() << curDisplay->slice() << m_lastSlice << m_view.currentSlice();
  if (!m_imgItems.empty() && m_displayValid &&
      curDisplay == m_lastDisplay && m_lastSlice == m_view.currentSlice() && m_lastTime == m_view.currentTime()) {
    //LINFO() << "0";
    // pixmap is same, we only need to show it
    if (!m_imgItems[0]->isVisible()) {
      for (int i=0; i<m_imgItems.size(); ++i) {
        m_imgItems[i]->setVisible(true);
      }
    }
  } else {
    destroyImgItems();

    curDisplay->setScale(m_view.currentScale());
    QRectF vp = m_view.currentViewport();
    vp.moveTo(vp.x()-m_offsetPara->get().x, vp.y()-m_offsetPara->get().y);
    curDisplay->setViewport(vp);

    const ZQImagePack &qImagePack = curDisplay->toQImagePack();
    for (size_t i=0; i<qImagePack.numImages(); ++i) {
      m_imgItems.push_back(new QGraphicsPixmapItem(QPixmap::fromImage(qImagePack.image(i))));
      m_imgItems[i]->setScale(qImagePack.scale(i));
      m_imgItems[i]->setPos(QPointF(qImagePack.location(i)) + QPointF(m_offsetPara->get().x, m_offsetPara->get().y));
      m_imgItems[i]->setVisible(m_isVisible);
      m_view.scene().addItem(m_imgItems[i]);
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
