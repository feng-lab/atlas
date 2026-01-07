#pragma once

#include <QGraphicsItem>

namespace GraphicsItemType {

enum Type
{
  ZPunctaGraphicsItem = QGraphicsItem::UserType + 1,
  SliceROIGraphicsItem,
  ROIGraphicsItem,
  ROICtrlPtGraphicsItem,
  ZSwcGraphicsItem,
  ParameterKeysItem,
  ZGraphicsItemGroup,
  ZImgScaleBarGraphicsItem,
  ZPunctumGraphicsItem,
  ZSwcSkeletonGraphicsItem,
  ZSwcNodeGraphicsItem,
  ZSkeletonGraphicsItem
};

} // namespace GraphicsItemType
