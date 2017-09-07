#pragma once

#include "zanimation.h"
#include <QWidget>
#include <QToolButton>
#include <cmath>

namespace nim {

class ZDoubleParameter;

class ZTimelineAxisView;

class ZTimelineObjView;

class ZTimelineEventView;

class ZTimelineWidget : public QWidget
{
Q_OBJECT
public:
  explicit ZTimelineWidget(ZAnimation& ani, ZDoubleParameter* currentTimePara, QWidget* parent = nullptr);

  QSize sizeHint() const override;

  ZAnimation& animation()
  { return m_animation; }

  inline int rowHeight() const
  { return 30; }

  inline double pixelsPerSecond() const
  { return m_pixelsPerSecond; }

  inline int objViewWidth() const
  { return 220; }

  inline int minViewHeight() const
  { return 10; }

  inline int eventViewWidth() const
  { return m_eventViewWidth; }

  inline double timeToX(double time) const
  { return 10 + time * pixelsPerSecond(); }

  inline double xToTime(double x) const
  { return std::max(0.0, (x - 10) / pixelsPerSecond()); }

  double currentTime() const;

  void removeSelectedKeys();

  void setCurrentTime(double t);

signals:

  void exportButtonToggled(bool);

  void eventViewWidthChanged();

  void pixelsPerSecondChagned();

  void currentTimeChanged();

protected:
  void eventViewVerticalScrollBarRangeChanged(int min, int max);

  void zoomIn();

  void zoomOut();

  void expandToFit();

  void updateEventViewWidth();

  void objViewVerticalScrollBarRangeChanged(int min, int max);

  void showValue(int i);

  void resizeEvent(QResizeEvent* event) override;

private:
  ZAnimation& m_animation;
  ZDoubleParameter* m_currentTimePara;

  ZTimelineAxisView* m_axis;
  ZTimelineObjView* m_objView;
  ZTimelineEventView* m_eventView;
  double m_pixelsPerSecond;
  int m_eventViewWidth;

  QToolButton* m_exportButton;
  QToolButton* m_cleanupButton;
  QToolButton* m_zoomInButton;
  QToolButton* m_zoomOutButton;
  QToolButton* m_expandButton;
};

} // namespace nim

