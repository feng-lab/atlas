#ifndef ZTIMELINEEVENTVIEW_H
#define ZTIMELINEEVENTVIEW_H

#include <QGraphicsView>
#include "ztimelinewidget.h"
#include "ztimelineeventscene.h"

namespace nim {

class ZTimelineEventView : public QGraphicsView
{
  Q_OBJECT
public:
  explicit ZTimelineEventView(ZTimelineWidget &parent);

  inline void removeSelectedKeys() { m_scene->removeSelectedKeys(); }

signals:

public slots:

protected slots:

protected:

private:
  ZTimelineWidget &m_parent;

  ZTimelineEventScene *m_scene;
};

} // namespace nim


#endif // ZTIMELINEEVENTVIEW_H
