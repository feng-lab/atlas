#ifndef ZOBJFILTER_H
#define ZOBJFILTER_H

#include <QObject>
#include <QList>
#include <QJsonObject>
#include "zview.h"

class ZParameter;

namespace nim {

class ZObjFilter : public QObject
{
  Q_OBJECT
public:
  explicit ZObjFilter(ZView &view);

  virtual void read(const QJsonObject &json);
  virtual void write(QJsonObject &json) const;

  virtual void setViewport(const QRectF &rect, double scale) { Q_UNUSED(rect); Q_UNUSED(scale); }
  virtual void deleteKeyPressed() {}
  virtual void mousePressed(const QPointF &scenePos) { Q_UNUSED(scenePos); }
  virtual void mouseReleased(const QPointF &scenePos) { Q_UNUSED(scenePos); }

  QList<ZParameter*> parameters() { return m_parameters; }

protected:
  void addParameter(ZParameter* para);
  void removeParameter(ZParameter* para);

signals:
  void boundBoxChanged();
  void objSelected(bool append);
  void objDeselected();

public slots:

protected:
  QList<ZParameter*> m_parameters;

  ZView& m_view;
};

} // namespace nim

#endif // ZOBJFILTER_H
