#pragma once

#include "ztimelineeventscene.h"
#include "ztimelinewidget.h"
#include <QGraphicsView>

namespace nim {

class ZTimelineEventView : public QGraphicsView
{
Q_OBJECT
public:
  explicit ZTimelineEventView(ZTimelineWidget& parent);

  inline void removeSelectedKeys()
  { m_scene->removeSelectedKeys(); }

private:
  ZTimelineWidget& m_parent;

  ZTimelineEventScene* m_scene;
};

} // namespace nim


