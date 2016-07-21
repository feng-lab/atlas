#ifndef ZOBJFILTER_H
#define ZOBJFILTER_H

#include <QObject>
#include <QList>
#include <QJsonObject>
#include "zview.h"
#include "znumericparameter.h"

namespace nim {

class ZObjFilter : public QObject
{
  Q_OBJECT
public:
  explicit ZObjFilter(ZView &view);

  virtual void read(const QJsonObject &json);
  virtual void write(QJsonObject &json) const;

  virtual void setNormalView(int z, int t) = 0;
  virtual void setMaxZProjView(int t) = 0;
  virtual void setViewport(const QRectF &rect, double scale) { Q_UNUSED(rect) Q_UNUSED(scale) }
  virtual void deleteKeyPressed() {}
  virtual void mousePressed(const QPointF &scenePos) { Q_UNUSED(scenePos) }
  virtual void mouseReleased(const QPointF &scenePos) { Q_UNUSED(scenePos) }
  virtual void rotateClockwise() {}
  virtual void rotateCounterclockwise() {}

  QList<ZParameter*> parameters() { return m_parameters; }

signals:
  void boundBoxChanged();
  void objSelected(bool append);
  void objDeselected();

protected:
  void addParameter(ZParameter* para);
  void removeParameter(ZParameter* para);

  void updateBoundBoxWithOffsetPara(std::vector<int> &boundBox) const;
  int realZ() const { return m_view.currentSlice() - int(m_offsetPara.get().z); }
  int realZ(int z) const { return z - int(m_offsetPara.get().z); }
  int realT() const { return m_view.currentTime() - int(m_offsetPara.get().w); }
  int realT(int t) const { return t - int(m_offsetPara.get().w); }

  virtual void offsetChanged();

protected:
  QList<ZParameter*> m_parameters;

  ZView& m_view;

  ZDVec4Parameter m_offsetPara;
};

} // namespace nim

#endif // ZOBJFILTER_H
