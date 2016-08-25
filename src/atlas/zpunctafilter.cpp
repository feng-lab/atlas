#include "zpunctafilter.h"

#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include "zgraphicsview.h"
#include "zsaturateoperation.h"
#include "zgraphicsscene.h"

namespace nim {

ZPunctaGraphicsItem::ZPunctaGraphicsItem(ZPuncta& puncta, double z, QGraphicsItem* parent)
  : QGraphicsItem(parent)
  , m_puncta(puncta)
  , m_boundBox(8)
  , m_outlineColor(255, 0, 0)
  , m_opacity(1)
  , m_mip(false)
  , m_z(0)
  , m_t(0)
{
  setZValue(z);

  m_boundBox[0] = m_boundBox[2] = m_boundBox[4] = m_boundBox[6] = std::numeric_limits<int>::max();
  m_boundBox[1] = m_boundBox[3] = m_boundBox[5] = m_boundBox[7] = std::numeric_limits<int>::min();
  for (ZPuncta::iterator it = m_puncta.begin(); it != m_puncta.end(); ++it) {
    int slice = roundTo<int>(it->z());
    m_boundBox[0] = std::min(roundTo<int>(it->x() - it->radius()), m_boundBox[0]);
    m_boundBox[1] = std::max(roundTo<int>(it->x() + it->radius()), m_boundBox[1]);
    m_boundBox[2] = std::min(roundTo<int>(it->y() - it->radius()), m_boundBox[2]);
    m_boundBox[3] = std::max(roundTo<int>(it->y() + it->radius()), m_boundBox[3]);
    m_boundBox[4] = std::min(slice, m_boundBox[4]);
    m_boundBox[5] = std::max(slice, m_boundBox[5]);
    m_boundBox[6] = std::min(0, m_boundBox[6]);
    m_boundBox[7] = std::max(0, m_boundBox[7]);
  }
}

QRectF ZPunctaGraphicsItem::boundingRect() const
{
  qreal penWidth = 1;
  return QRectF(m_boundBox[0] - penWidth / 2, m_boundBox[2] - penWidth / 2,
                m_boundBox[1] - m_boundBox[0] + penWidth,
                m_boundBox[3] - m_boundBox[2] + penWidth);
}

void ZPunctaGraphicsItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
  Q_UNUSED(option)
  Q_UNUSED(widget)
  m_outlineColor.setAlpha(m_opacity * 255);
  painter->setPen(QPen(m_outlineColor, 1));
  if (m_mip) {
    for (ZPuncta::iterator it = m_puncta.begin(); it != m_puncta.end(); ++it) {
      painter->drawEllipse(QRectF(it->x() - it->radius(), it->y() - it->radius(), it->radius() * 2, it->radius() * 2));
    }
  } else {
    for (ZPuncta::iterator it = m_puncta.begin(); it != m_puncta.end(); ++it) {
      int slice = roundTo<int>(it->z());
      if (slice == m_z) {
        painter->drawEllipse(
          QRectF(it->x() - it->radius(), it->y() - it->radius(), it->radius() * 2, it->radius() * 2));
      }
    }
  }
}

ZPunctaFilter::ZPunctaFilter(ZView& view)
  : ZObjFilter(view)
  , m_puncta(nullptr)
  , m_visible("Visible", true)
  , m_outlineColor("Outline Color", glm::vec3(1, 0, 1), glm::vec3(0), glm::vec3(1))
  , m_opacity("Opacity", 1, 0., 1.)
{
  m_outlineColor.setStyle("COLOR");
  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZPunctaFilter::visibleChanged);
  connect(&m_outlineColor, &ZVec3Parameter::valueChanged, this, &ZPunctaFilter::outlineColorChanged);
  connect(&m_opacity, &ZDoubleParameter::valueChanged, this, &ZPunctaFilter::opacityChanged);
  addParameter(&m_visible);
  addParameter(&m_outlineColor);
  addParameter(&m_opacity);
}

void ZPunctaFilter::setData(ZPuncta& puncta)
{
  m_puncta = &puncta;
  m_item.reset(new ZPunctaGraphicsItem(puncta));
  m_item->setOutlineColor(QColor(m_outlineColor.get().x * 255,
                                 m_outlineColor.get().y * 255,
                                 m_outlineColor.get().z * 255));
  m_item->setOpacity(m_opacity.get());
  if (m_view.isMaxZProjView()) {
    m_item->setMaxZProjView(m_view.currentTime());
  } else {
    m_item->setNormalView(m_view.currentSlice(), m_view.currentTime());
  }
  m_view.scene().addItem(m_item.get());

  //connect(m_puncta, &ZPunctaFilter::boundBoxChanged, this, &ZPunctaFilter::boundBoxChanged);
}

void ZPunctaFilter::releaseItemsOwnership()
{
  m_item.release();
}

void ZPunctaFilter::setNormalView(int z, int t)
{
  m_item->setNormalView(z, t);
}

void ZPunctaFilter::setMaxZProjView(int t)
{
  m_item->setMaxZProjView(t);
}

const std::vector<int>& ZPunctaFilter::boundBox() const
{
  return m_item->boundBox();
}

std::shared_ptr<ZWidgetsGroup> ZPunctaFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Puncta", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_outlineColor, 1);
    m_widgetsGroup->addChild(m_opacity, 1);
  }
  return m_widgetsGroup;
}

void ZPunctaFilter::visibleChanged()
{
  m_item->setVisible(m_visible.get());
}

void ZPunctaFilter::outlineColorChanged()
{
  m_item->setOutlineColor(QColor(m_outlineColor.get().x * 255,
                                 m_outlineColor.get().y * 255,
                                 m_outlineColor.get().z * 255));
}

void ZPunctaFilter::opacityChanged()
{
  m_item->setOpacity(m_opacity.get());
}

} // namespace nim
