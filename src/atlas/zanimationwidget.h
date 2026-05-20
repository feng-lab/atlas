#pragma once

#include "zanimation.h"
#include <QWidget>

class QToolButton;

class QPushButton;

class QAction;

class QTimeLine;

namespace nim {

class ZDoubleParameter;

class ZTimelineWidget;

class ZAnimationExportWidget;

class ZAnimationWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ZAnimationWidget(ZAnimation& ani, QWidget* parent = nullptr);

protected:
  void onDurationChanged(double d);

  void onSpeedChanged(double v);

  void saveKeyFrame();

  void playPause();

  void reversePlayPause();

  void setFrame(int f);

  void timeLineFinished();

  void gotoStart();

  void gotoEnd();

  void repeatChanged(bool v);

  void keyPressEvent(QKeyEvent* event) override;

private:
  void createWidget();

  void refreshTheme();

private:
  ZAnimation& m_animation;

  ZDoubleParameter* m_duration;
  ZDoubleParameter* m_currentTime;
  ZDoubleParameter* m_playSpeed;
  bool m_isPlaying;

  QToolButton* m_gotoStartButton;
  QToolButton* m_reversePlayButton;
  QToolButton* m_playButton;
  QToolButton* m_gotoEndButton;
  QToolButton* m_repeatButton;
  QPushButton* m_saveKeyFrameButton;

  QTimeLine* m_timeLine;
  ZTimelineWidget* m_timelineWidget;

  ZAnimationExportWidget* m_exportWidget;
};

} // namespace nim
