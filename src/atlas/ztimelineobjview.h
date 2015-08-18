#ifndef ZTIMELINEOBJVIEW_H
#define ZTIMELINEOBJVIEW_H

#include <QGraphicsView>
#include <QGraphicsItemGroup>
#include <QMouseEvent>
#include "ztimelinewidget.h"

namespace nim {

class ZTimelineObjView : public QGraphicsView
{
  Q_OBJECT
public:
  explicit ZTimelineObjView(ZTimelineWidget &parent);

  void setScrollEnabled(bool v);

signals:
  void vScrollBarValueChanged(int v);

public slots:

protected slots:
  void checkScrollBarValue(int v);

protected:
  virtual void scrollContentsBy(int dx, int dy) override;

private:
  ZTimelineWidget &m_parent;
  bool m_scrollEnabled;
};

} // namespace nim

#endif // ZTIMELINEOBJVIEW_H
