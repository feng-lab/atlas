//
// Created by Linqing Feng on 8/24/16.
//

#pragma once

#include "zobjfilter.h"
#include <QList>
#include <vector>
#include "zparameter.h"
#include <map>
#include "znumericparameter.h"
#include "zsvgdoc.h"
#include <QGraphicsSvgItem>

class ZWidgetsGroup;

namespace nim {

class ZSvgFilter : public ZObjFilter
{
Q_OBJECT
public:
  ZSvgFilter(ZView& view);

  void setData(QSvgRenderer& svg);

  void releaseItemsOwnership();

  void setVisible(bool v)
  { m_visible.set(v); }

  void setSelected(bool v)
  { Q_UNUSED(v) }

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  std::vector<int> boundBox() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

protected:
  virtual void offsetChanged() override;

private:
  void visibleChanged();

  void opacityChanged();

private:
  std::unique_ptr<QGraphicsSvgItem> m_item;

  ZBoolParameter m_visible;
  ZDoubleParameter m_opacity;
  bool m_sliceValid;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
};

} // namespace nim

