#pragma once

#include "znumericparameter.h"
#include "z2dtransformparameter.h"
#include "zview.h"
#include <QObject>
#include <QTransform>

namespace nim {

class ZObjFilter : public QObject
{
Q_OBJECT
public:
  explicit ZObjFilter(ZView& view);

  virtual void read(const json::object& json);

  virtual void write(json::object& json) const;

  void setVisible(bool v)
  { m_visible.set(v); }

  [[nodiscard]] virtual bool isVisible() const
  { return m_visible.get(); }

  ZBoolParameter& visiblePara()
  { return m_visible; }

  virtual void setNormalView(int z, int t) = 0;

  virtual void setMaxZProjView(int t) = 0;

  virtual void deleteKeyPressed()
  {}

  virtual void copyKeyPressed()
  {}

  virtual void pasteKeyPressed(int slice, QPointF point, bool hFlip, bool vFlip)
  {
    Q_UNUSED(slice)
    Q_UNUSED(point)
    Q_UNUSED(hFlip)
    Q_UNUSED(vFlip)
  }

  virtual void mousePressed(const QPointF& scenePos, Qt::KeyboardModifiers modifiers)
  {
    Q_UNUSED(scenePos)
    Q_UNUSED(modifiers)
  }

  virtual void mouseMoved(const QPointF& scenePos, Qt::KeyboardModifiers modifiers)
  {
    Q_UNUSED(scenePos)
    Q_UNUSED(modifiers)
  }

  virtual void mouseReleased(const QPointF& scenePos)
  { Q_UNUSED(scenePos) }

  virtual void rotateClockwise(double, double)
  {}

  virtual void rotateCounterclockwise(double, double)
  {}

  std::vector<ZParameter*> parameters()
  { return m_parameters; }

  [[nodiscard]] int viewPrecedence() const
  { return m_viewPrecedencePara.get(); }

  [[nodiscard]] QPointF mapFromScene(QPointF p) const;

Q_SIGNALS:

  void boundBoxChanged();

  void objSelected(bool append);

  void objDeselected();

  void objVisibleChanged(bool v);

protected:
  void addParameter(ZParameter* para);

  void removeParameter(ZParameter* para);

  void updateBoundBoxWithOffsetPara(ZBBox<glm::ivec4>& boundBox) const;

  QTransform getQTransform() const;

  [[nodiscard]] glm::dvec2 getTransformScale() const
  { return m_transform.scale(); }

  [[nodiscard]] QRectF mapToSceneRect(const QRectF& rect) const;

  [[nodiscard]] QRectF mapFromSceneRect(const QRectF& rect) const;

  [[nodiscard]] int realZ() const
  { return m_view.currentSlice() - int(m_offsetPara.get().x); }

  [[nodiscard]] int realZ(int z) const
  { return z - int(m_offsetPara.get().x); }

  [[nodiscard]] int realT() const
  { return m_view.currentTime() - int(m_offsetPara.get().y); }

  [[nodiscard]] int realT(int t) const
  { return t - int(m_offsetPara.get().y); }

  virtual void viewPrecedenceChanged();

  virtual void transformChanged();

  virtual void offsetChanged();

  // set view precedence to highest value in the scene
  void bringToFront();

  // set view precedence to lowest value in the scene
  void sendToBack();

protected:
  std::vector<ZParameter*> m_parameters;

  ZView& m_view;

  ZBoolParameter m_visible;
  ZIntParameter m_viewPrecedencePara;
  Z2DTransformParameter m_transform;
  ZDVec2Parameter m_offsetPara;
};

} // namespace nim

