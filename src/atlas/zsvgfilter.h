#pragma once

#include "zobjfilter.h"
#include "zparameter.h"
#include "znumericparameter.h"
#include "zsvgdoc.h"
#include <QGraphicsSvgItem>
#include <map>
#include <vector>

class ZWidgetsGroup;

namespace nim {

class ZSvgFilter : public ZObjFilter
{
Q_OBJECT
public:
  explicit ZSvgFilter(ZView& view);

  static int getViewPrecedence()
  {
    static int vp = 10000;
    return vp++;
  }

  void setData(QSvgRenderer& svg);

  void releaseItemsOwnership();

  void setSelected(bool v);

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  ZBBox<glm::ivec4> boundBox() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

protected:
  void viewPrecedenceChanged() override;

  void transformChanged() override;

  void offsetChanged() override;

private:
  void visibleChanged();

  void opacityChanged();

private:
  std::unique_ptr<QGraphicsSvgItem> m_item;

  ZDoubleParameter m_opacity;
  bool m_sliceValid;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
};

} // namespace nim

