#pragma once

#include "ztimelinewidget.h"
#include <QGraphicsView>
#include <QGraphicsItemGroup>
#include <QMouseEvent>

namespace nim {

class ZTimelineObjView : public QGraphicsView
{
Q_OBJECT
public:
  explicit ZTimelineObjView(ZTimelineWidget& parent);

  void setScrollEnabled(bool v);

signals:

  void vScrollBarValueChanged(int v);

protected:
  void checkScrollBarValue(int v);

  virtual void scrollContentsBy(int dx, int dy) override;

private:
  ZTimelineWidget& m_parent;
  bool m_scrollEnabled;
};

} // namespace nim

