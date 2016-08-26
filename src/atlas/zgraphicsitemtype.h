//
// Created by Linqing Feng on 8/26/16.
//

#pragma once

#include <QGraphicsItem>

namespace GraphicsItemType {

enum Type
{
  ZPunctaGraphicsItem = QGraphicsItem::UserType + 1,
  ROIGraphicsItem,
  ROICtrlPtGraphicsItem,
  ZSwcGraphicsItem,
  ParameterKeysItem
};

} // namespace GraphicsItemType
