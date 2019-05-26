#pragma once

#include <QGraphicsItem>

namespace GraphicsItemType {

enum Type
{
  ZPunctaGraphicsItem = QGraphicsItem::UserType + 1,
  ROIGraphicsItem,
  ROICtrlPtGraphicsItem,
  ZSwcGraphicsItem,
  ParameterKeysItem,
  ZGraphicsItemGroup,
  ZImgScaleBarGraphicsItem
};

} // namespace GraphicsItemType
