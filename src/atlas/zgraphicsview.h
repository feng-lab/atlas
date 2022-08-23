#pragma once

#include "znumericparameter.h"
#include "zview.h"
#include <QGraphicsView>
QT_BEGIN_NAMESPACE
class QGestureEvent;
class QPanGesture;
class QPinchGesture;
QT_END_NAMESPACE

namespace nim {

class ZGraphicsView : public QGraphicsView
{
  Q_OBJECT

public:
  explicit ZGraphicsView(QGraphicsScene* scene, ZView* parent);

  // call if scenerect changed
  void updateScaleFactorRange();

  QRectF getCurrrentlyVisibleRegion() const;

  QWidget* createScaleWidget(QWidget* parent);

  // do not use other scale functions because we need to sync scale with our widget
  inline double currentScale() const
  {
    return m_scale.get() / 100.;
  }

  inline void setScale(double s)
  {
    m_scale.set(s * 100.);
    Q_EMIT scaleChanged(currentScale());
  }

  void fitRect(const QRectF& rect);

  QSize viewportSize() const
  {
    return viewport()->size();
  }

  bool renderToImage(const QString& filename, QString* err = nullptr);

  bool renderToImage(const QString& filename, int width, int height, QString* err = nullptr);

  void checkViewport();

Q_SIGNALS:

  void scaleChanged(double s);

  void viewportChanged();

protected:
  void scaleParaChanged();

  void dragEnterEvent(QDragEnterEvent* event) override;

  void dropEvent(QDropEvent* event) override;

  void wheelEvent(QWheelEvent* event) override;

  bool event(QEvent* event) override;

private:
  bool gestureEvent(QGestureEvent* event);
  void panTriggered(QPanGesture*);
  void pinchTriggered(QPinchGesture*);

private:
  ZView* m_view;
  ZDoubleParameter m_scale;

  double m_currentStepScaleFactor = 1.;
};

} // namespace nim
