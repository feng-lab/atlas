#ifndef ZGRAPHICSVIEW_H
#define ZGRAPHICSVIEW_H

#include <QGraphicsView>
#include "zview.h"
#include "znumericparameter.h"

namespace nim {

class ZGraphicsView : public QGraphicsView
{
  Q_OBJECT
public:
  enum class ROIAction {
    New, Add, Subtract
  };

  explicit ZGraphicsView(QGraphicsScene *scene, ZView *parent);

  // call if scenerect changed
  void updateScaleFactorRange();

  QRectF getCurrrentlyVisibleRegion() const;

  QWidget* createScaleWidget(QWidget *parent);

  // do not use other scale functions because we need to sync scale with our widget
  inline double currentScale() const { return m_scale.get() / 100.; }
  inline void setScale(double s) { m_scale.set(s * 100.); emit scaleChanged(currentScale()); }
  void fitRect(const QRectF& rect);

  QSize viewportSize() const { return viewport()->size(); }

  bool renderToImage(const QString &filename, QString *err = nullptr);
  bool renderToImage(const QString &filename, int width, int height, QString *err = nullptr);

signals:
  void scaleChanged(double s);
  void viewportChanged();

public slots:
  void setScrollHandDragMode();
  void setRubberBandDragMode();

protected slots:
  void scaleParaChanged();

  void checkViewport();

protected:
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;

  inline bool isScenePtOverlap(const QPointF &p1, const QPointF &p2) const
  { return mapFromScene(p1) == mapFromScene(p2) || p1.toPoint() == p2.toPoint(); }

private:
  ZView *m_view;
  ZDoubleParameter m_scale;

  ROIAction m_roiAction;
  QPointF m_startScenePt;
  QGraphicsPolygonItem *m_startPtItem;
  std::vector<QGraphicsPolygonItem*> m_ctrlPtsItem;
  QGraphicsEllipseItem *m_ellipseItem;
  QGraphicsRectItem *m_rectItem;
  QPolygonF m_polygon;
  QGraphicsPathItem *m_polygonItem;
  QPolygonF m_spline;
  QGraphicsPathItem *m_splineItem;
};

} // namespace nim

#endif // ZGRAPHICSVIEW_H
