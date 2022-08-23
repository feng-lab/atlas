#pragma once

#include "zgraphicsitemtype.h"
#include <QGraphicsItemGroup>

namespace nim {

class ZGraphicsItemGroup : public QGraphicsItemGroup
{
public:
  enum
  {
    Type = GraphicsItemType::ZGraphicsItemGroup
  };

  [[nodiscard]] int type() const override
  {
    return Type;
  }

  explicit ZGraphicsItemGroup(QGraphicsItem* parent = nullptr);

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
};

} // namespace nim
