#include "ztimelineeventview.h"

#include "ztimelineeventscene.h"

namespace nim {

ZTimelineEventView::ZTimelineEventView(ZTimelineWidget& parent)
  : QGraphicsView(&parent)
  , m_parent(parent)
{
  setAlignment(Qt::AlignLeft | Qt::AlignTop);
  m_scene = new ZTimelineEventScene(m_parent, this);
  setScene(m_scene);
  setMinimumHeight(m_parent.minViewHeight());
  setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
  setDragMode(QGraphicsView::RubberBandDrag);
}

} // namespace nim
