#pragma once

#include "znumericparameter.h"
#include "z2dtransformparameter.h"
#include "zview.h"
#include <QObject>
#include <QList>
#include <QTransform>

namespace nim {

class ZObjFilter : public QObject
{
Q_OBJECT
public:
  explicit ZObjFilter(ZView& view);

  virtual void read(const QJsonObject& json);

  virtual void write(QJsonObject& json) const;

  virtual void setNormalView(int z, int t) = 0;

  virtual void setMaxZProjView(int t) = 0;

  virtual void deleteKeyPressed()
  {}

  virtual void mousePressed(const QPointF& scenePos)
  { Q_UNUSED(scenePos) }

  virtual void mouseReleased(const QPointF& scenePos)
  { Q_UNUSED(scenePos) }

  virtual void rotateClockwise()
  {}

  virtual void rotateCounterclockwise()
  {}

  QList<ZParameter*> parameters()
  { return m_parameters; }

  QPointF mapFromScene(QPointF p) const;

signals:

  void boundBoxChanged();

  void objSelected(bool append);

  void objDeselected();

protected:
  void addParameter(ZParameter* para);

  void removeParameter(ZParameter* para);

  void updateBoundBoxWithOffsetPara(std::array<int, 8>& boundBox) const;

  QTransform getQTransform() const;

  QRectF mapToSceneRect(const QRectF& rect) const;

  QRectF mapFromSceneRect(const QRectF& rect) const;

  int realZ() const
  { return m_view.currentSlice() - int(m_offsetPara.get().x); }

  int realZ(int z) const
  { return z - int(m_offsetPara.get().x); }

  int realT() const
  { return m_view.currentTime() - int(m_offsetPara.get().y); }

  int realT(int t) const
  { return t - int(m_offsetPara.get().y); }

  virtual void transformChanged();

  virtual void offsetChanged();

protected:
  QList<ZParameter*> m_parameters;

  ZView& m_view;

  Z2DTransformParameter m_transform;
  ZDVec2Parameter m_offsetPara;
};

} // namespace nim

