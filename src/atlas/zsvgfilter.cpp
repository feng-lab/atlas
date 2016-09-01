//
// Created by Linqing Feng on 8/24/16.
//

#include "zsvgfilter.h"

#include "zgraphicsview.h"
#include "znumericparameter.h"
#include "zsaturateoperation.h"
#include "zgraphicsscene.h"
#include "zwidgetsgroup.h"
#include "zlogqttypesupport.h"
#include <QWindow>

namespace nim {

ZSvgFilter::ZSvgFilter(ZView& view)
  : ZObjFilter(view)
  , m_visible("Visible", true)
  , m_opacity("Opacity", 1, 0., 1.)
{
  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZSvgFilter::visibleChanged);
  connect(&m_opacity, &ZDoubleParameter::valueChanged, this, &ZSvgFilter::opacityChanged);
  addParameter(&m_visible);
  addParameter(&m_offsetPara);
  addParameter(&m_opacity);
}

void ZSvgFilter::setData(QSvgRenderer& svg)
{
  m_item = std::make_unique<QGraphicsSvgItem>();
  m_item->setSharedRenderer(&svg);
  m_item->setOpacity(m_opacity.get());
  m_item->setPos(m_offsetPara.get().x, m_offsetPara.get().y);
  m_item->setVisible((realZ() == 0 || m_view.isMaxZProjView()) && realT() == 0 && m_visible.get());
  //if (svg.animated())
    //connect(m_item->renderer(), &QSvgRenderer::repaintNeeded, [this](){ m_item->update(); });
  m_view.scene().addItem(m_item.get());
}

void ZSvgFilter::releaseItemsOwnership()
{
  m_item.release();
}

void ZSvgFilter::setNormalView(int z, int t)
{
  m_item->setVisible(realZ(z) == 0 && realT(t) == 0 && m_visible.get());
}

void ZSvgFilter::setMaxZProjView(int t)
{
  m_item->setVisible(realT(t) == 0 && m_visible.get());
}

std::vector<int> ZSvgFilter::boundBox() const
{
  QRectF bound = m_item->boundingRect();
  std::vector<int> res(8, 0);
  res[0] = std::floor(bound.left());
  res[1] = std::ceil(bound.right());
  res[2] = std::floor(bound.top());
  res[3] = std::ceil(bound.bottom());
  updateBoundBoxWithOffsetPara(res);
  return res;
}

std::shared_ptr<ZWidgetsGroup> ZSvgFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_offsetPara, 1);
    m_widgetsGroup->addChild(m_opacity, 1);
  }
  return m_widgetsGroup;
}

void ZSvgFilter::offsetChanged()
{
  m_item->setPos(m_offsetPara.get().x, m_offsetPara.get().y);
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
