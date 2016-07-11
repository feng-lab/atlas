#include "ztimelineobjview.h"

#include <QGraphicsItem>
#include <QGraphicsEllipseItem>
#include <QGraphicsTextItem>
#include "ztimelineobjscene.h"
#include "QsLog.h"
#include <QScrollBar>

namespace nim {

ZTimelineObjView::ZTimelineObjView(ZTimelineWidget &parent)
  : QGraphicsView(&parent)
  , m_parent(parent)
  , m_scrollEnabled(true)
{
  setAlignment(Qt::AlignLeft|Qt::AlignTop);
  setScene(new ZTimelineObjScene(m_parent, this));
  setMaximumWidth(m_parent.objViewWidth() + 2);
  setMinimumWidth(m_parent.objViewWidth() + 2);
  setMinimumHeight(m_parent.minViewHeight());
  setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &ZTimelineObjView::checkScrollBarValue);
}

void ZTimelineObjView::setScrollEnabled(bool v)
{
  if (!v)
    verticalScrollBar()->setValue(0);
  m_scrollEnabled = v;
}

void ZTimelineObjView::checkScrollBarValue(int v)
{
  if (m_scrollEnabled) {
    emit vScrollBarValueChanged(v);
  } else {
    verticalScrollBar()->setValue(0); // how?
  }
}

void ZTimelineObjView::scrollContentsBy(int dx, int dy)
{
  if (m_scrollEnabled)
    QGraphicsView::scrollContentsBy(dx, dy);
}

} // namespace nim

