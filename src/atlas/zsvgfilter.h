#pragma once

#include "zobjfilter.h"
#include "zparameter.h"
#include "znumericparameter.h"
#include "zsvgdoc.h"
#include <QGraphicsSvgItem>
#include <QList>
#include <map>
#include <vector>

class ZWidgetsGroup;

namespace nim {

class ZSvgFilter : public ZObjFilter
{
Q_OBJECT
public:
  explicit ZSvgFilter(ZView& view);

  void setData(QSvgRenderer& svg);

  void releaseItemsOwnership();

  void setVisible(bool v)
  { m_visible.set(v); }

  void setSelected(bool v)
  { Q_UNUSED(v) }

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  std::array<int, 8> boundBox() const;

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

