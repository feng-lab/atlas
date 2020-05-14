#include "zsvgfilter.h"

#include "zgraphicsview.h"
#include "znumericparameter.h"
#include "zsaturateoperation.h"
#include "zgraphicsscene.h"
#include "zwidgetsgroup.h"
#include <QWindow>
#include <QPushButton>

namespace nim {

ZSvgFilter::ZSvgFilter(ZView& view)
  : ZObjFilter(view)
  , m_opacity("Opacity", 1, 0., 1.)
{
  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZSvgFilter::visibleChanged);
  connect(&m_opacity, &ZDoubleParameter::valueChanged, this, &ZSvgFilter::opacityChanged);
  m_viewPrecedencePara.blockSignals(true);
  m_viewPrecedencePara.set(getViewPrecedence());
  m_viewPrecedencePara.blockSignals(false);
  addParameter(&m_opacity);
}

void ZSvgFilter::setData(QSvgRenderer& svg)
{
  m_item = std::make_unique<QGraphicsSvgItem>();
  m_item->setZValue(m_viewPrecedencePara.get());
  m_item->setSharedRenderer(&svg);
  m_item->setOpacity(m_opacity.get());
  m_item->setTransform(getQTransform());
  m_item->setVisible((realZ() == 0 || m_view.isMaxZProjView()) && realT() == 0 && m_visible.get());
  //if (svg.animated())
    //connect(m_item->renderer(), &QSvgRenderer::repaintNeeded, [this](){ m_item->update(); });
  m_view.scene().addItem(m_item.get());
}

void ZSvgFilter::releaseItemsOwnership()
{
  m_item.release();
}

void ZSvgFilter::setSelected(bool v)
{
  if (m_item->isSelected() != v) {
    m_item->setSelected(v);
  }
}

void ZSvgFilter::setNormalView(int z, int t)
{
  m_item->setVisible(realZ(z) == 0 && realT(t) == 0 && m_visible.get());
}

void ZSvgFilter::setMaxZProjView(int t)
{
  m_item->setVisible(realT(t) == 0 && m_visible.get());
}

ZBBox<glm::ivec4> ZSvgFilter::boundBox() const
{
  QRectF bound = m_item->boundingRect();
  ZBBox<glm::ivec4> res;
  res.setMinCorner(glm::ivec4(bound.left(), bound.top(), 0, 0));
  res.setMaxCorner(glm::ivec4(bound.right(), bound.bottom(), 0, 0));
  updateBoundBoxWithOffsetPara(res);
  return res;
}

std::shared_ptr<ZWidgetsGroup> ZSvgFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("", 1);
    m_widgetsGroup->addChild(m_visible, 1);

    QPushButton* pb = new QPushButton("Bring to Front");
    connect(pb, &QPushButton::clicked, this, &ZSvgFilter::bringToFront);
    m_widgetsGroup->addChild(*pb, 1);

    pb = new QPushButton("Send to Back");
    connect(pb, &QPushButton::clicked, this, &ZSvgFilter::sendToBack);
    m_widgetsGroup->addChild(*pb, 1);

    m_widgetsGroup->addChild(m_viewPrecedencePara, 1);
    m_widgetsGroup->addChild(m_transform, 1);
    m_widgetsGroup->addChild(m_offsetPara, 1);
    m_widgetsGroup->addChild(m_opacity, 1);
  }
  return m_widgetsGroup;
}

void ZSvgFilter::viewPrecedenceChanged()
{
  m_item->setZValue(m_viewPrecedencePara.get());
  ZObjFilter::viewPrecedenceChanged();
}

void ZSvgFilter::transformChanged()
{
  m_item->setTransform(getQTransform());
  ZObjFilter::transformChanged();
}

void ZSvgFilter::offsetChanged()
{
  m_item->setVisible((realZ() == 0 || m_view.isMaxZProjView()) && realT() == 0 && m_visible.get());
  ZObjFilter::offsetChanged();
}

void ZSvgFilter::visibleChanged()
{
  m_item->setVisible((realZ() == 0 || m_view.isMaxZProjView()) && realT() == 0 && m_visible.get());
}

void ZSvgFilter::opacityChanged()
{
  m_item->setOpacity(m_opacity.get());
}

} // namespace nim
