#pragma once

#include "zfilterview.h"
#include "zimgdoc.h"
#include "zimgfilter.h"

class QMenu;

namespace nim {

class ZImgView : public ZFilterView<ZImgDoc, ZImgFilter>
{
  Q_OBJECT

public:
  ZImgView(ZImgDoc& doc, ZView& view);

  // ZObjView interface

public:
  QString infoOfPos(double x, double y) override;

  void appendContextMenuActions(QMenu& menu,
                                size_t activeObjId,
                                const QPointF& scenePos,
                                Qt::KeyboardModifiers modifiers) override;

private:
  void docImgsAdded(const std::vector<size_t>& objs);

  void docImgAdded(size_t id);

  void docImgChanged(size_t id);
};

} // namespace nim
