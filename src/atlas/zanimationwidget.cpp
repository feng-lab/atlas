#include "zanimationwidget.h"

#include "znumericparameter.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QToolButton>
#include <QPushButton>
#include <QSpinBox>
#include <QLabel>
#include <QAction>
#include <QTimeLine>
#include <QApplication>
#include <QKeyEvent>
#include "ztimelinewidget.h"
#include "zanimationexportwidget.h"

namespace nim {

ZAnimationWidget::ZAnimationWidget(ZAnimation &ani, QWidget *parent)
  : QWidget(parent)
  , m_animation(ani)
  , m_duration(new ZDoubleParameter("Duration:", m_animation.duration(), 1.0, 60*60*72, this))
  , m_currentTime(new ZDoubleParameter("Current Time:", 0.0, 0.0, ani.duration(), this))
  , m_playSpeed(new ZDoubleParameter("Speed:", 1.0, 0.01, 100, this))
  , m_isPlaying(false)
{
  m_duration->setDecimal(1);
  m_duration->setSingleStep(.1);
  m_duration->setSuffix(" secs");
  m_duration->setStyle("SPINBOX");

  m_currentTime->setDecimal(3);
  m_currentTime->setSingleStep(.001);
  m_currentTime->setSuffix(" secs");
  m_playSpeed->setDecimal(2);
  m_playSpeed->setStyle("SPINBOX");
  m_playSpeed->setSuffix(" x");
  createWidget();

  m_timeLine = new QTimeLine(m_animation.duration() * 1000, this);
  m_timeLine->setFrameRange(0, m_animation.duration() * 25);
  m_timeLine->setCurveShape(QTimeLine::LinearCurve);
  connect(m_duration, SIGNAL(valueChanged(double)), &m_animation, SLOT(setDuration(double)));
  connect(m_timeLine, SIGNAL(frameChanged(int)), this, SLOT(setFrame(int)));
  connect(m_timeLine, SIGNAL(finished()), this, SLOT(timeLineFinished()));
  connect(&m_animation, SIGNAL(durationChanged(double)), this, SLOT(onDurationChanged(double)));
  connect(m_currentTime, SIGNAL(valueChanged(double)), &m_animation, SLOT(setCurrentTime(double)));
  connect(m_playSpeed, SIGNAL(valueChanged(double)), this, SLOT(onSpeedChanged(double)));
  connect(m_saveKeyFrameButton, SIGNAL(clicked()), this, SLOT(saveKeyFrame()));
  connect(m_reversePlayButton, SIGNAL(clicked()), this, SLOT(reversePlayPause()));
  connect(m_playButton, SIGNAL(clicked()), this, SLOT(playPause()));
  connect(m_gotoStartButton, SIGNAL(clicked()), this, SLOT(gotoStart()));
  connect(m_gotoEndButton, SIGNAL(clicked()), this, SLOT(gotoEnd()));
  connect(m_repeatButton, SIGNAL(toggled(bool)), this, SLOT(repeatChanged(bool)));

  m_animation.setCurrentTime(0);
}

void ZAnimationWidget::onDurationChanged(double d)
{
  m_duration->set(d);
  m_currentTime->setRange(0.0, d);
  m_timeLine->setDuration(d * 1000 / m_playSpeed->get());
  m_timeLine->setFrameRange(0, d * 25 / m_playSpeed->get());
}

void ZAnimationWidget::onSpeedChanged(double v)
{
  m_timeLine->setDuration(m_animation.duration() * 1000 / v);
  m_timeLine->setFrameRange(0, m_animation.duration() * 25 / v);
}

void ZAnimationWidget::saveKeyFrame()
{
  m_animation.addKeyFrame(m_currentTime->get());
}

void ZAnimationWidget::playPause()
{
  if (m_isPlaying) {
    m_gotoStartButton->setEnabled(true);
    m_reversePlayButton->setEnabled(true);
    m_gotoEndButton->setEnabled(true);
    m_playSpeed->setEnabled(true);

    m_isPlaying = false;
    m_playButton->setIcon(QIcon(":/icons/play-512.png"));
    m_timeLine->stop();
  } else {
    m_gotoStartButton->setEnabled(false);
    m_reversePlayButton->setEnabled(false);
    m_gotoEndButton->setEnabled(false);
    m_playSpeed->setEnabled(false);

    m_isPlaying = true;
    m_playButton->setIcon(QIcon(":/icons/pause-512.png"));
    m_timeLine->setCurrentTime(m_currentTime->get() * 1000 / m_playSpeed->get());
    m_timeLine->setDirection(QTimeLine::Forward);
    m_timeLine->resume();
  }
}

void ZAnimationWidget::reversePlayPause()
{
  if (m_isPlaying) {
    m_gotoStartButton->setEnabled(true);
    m_playButton->setEnabled(true);
    m_gotoEndButton->setEnabled(true);
    m_playSpeed->setEnabled(true);

    m_isPlaying = false;
    m_reversePlayButton->setIcon(QIcon(":/icons/reverse_play-512.png"));
    m_timeLine->stop();
  } else {
    m_gotoStartButton->setEnabled(false);
    m_playButton->setEnabled(false);
    m_gotoEndButton->setEnabled(false);
    m_playSpeed->setEnabled(false);

    m_isPlaying = true;
    m_reversePlayButton->setIcon(QIcon(":/icons/pause-512.png"));
    m_timeLine->setCurrentTime(m_currentTime->get() * 1000 / m_playSpeed->get());
    m_timeLine->setDirection(QTimeLine::Backward);
    m_timeLine->resume();
  }
}

void ZAnimationWidget::setFrame(int f)
{
  m_currentTime->set(f * m_playSpeed->get() / 25);
  QApplication::processEvents();
}

void ZAnimationWidget::timeLineFinished()
{
  m_gotoStartButton->setEnabled(true);
  m_reversePlayButton->setEnabled(true);
  m_playButton->setEnabled(true);
  m_gotoEndButton->setEnabled(true);
  m_playSpeed->setEnabled(true);

  m_isPlaying = false;
  m_playButton->setIcon(QIcon(":/icons/play-512.png"));
  m_reversePlayButton->setIcon(QIcon(":/icons/reverse_play-512.png"));
}

void ZAnimationWidget::gotoStart()
{
  m_currentTime->set(0.0);
}

void ZAnimationWidget::gotoEnd()
{
  m_currentTime->set(m_animation.duration());
}

void ZAnimationWidget::repeatChanged(bool v)
{
  if (!v && m_timeLine->state() == QTimeLine::Running) {
    m_timeLine->stop();
    m_timeLine->setLoopCount(1);
    m_timeLine->setCurrentTime(m_currentTime->get() * 1000 / m_playSpeed->get());
    m_timeLine->resume();
    return;
  }
  m_timeLine->setLoopCount(v ? 0 : 1);
}

void ZAnimationWidget::keyPressEvent(QKeyEvent *event)
{
  switch (event->key()) {
  case Qt::Key_K:
    if (event->modifiers() == Qt::ControlModifier) {
      saveKeyFrame();
      event->accept();
    }
    break;
  case Qt::Key_Backspace:
  case Qt::Key_Delete:
    if (event->modifiers() == Qt::NoModifier)
      m_timelineWidget->removeSelectedKeys();
    break;
  default:
    break;
  }
}

void ZAnimationWidget::createWidget()
{
  QHBoxLayout *hlo = new QHBoxLayout;

  hlo->addWidget(m_playSpeed->createNameLabel(this));
  hlo->addWidget(m_playSpeed->createWidget(this));
  hlo->addSpacing(5);

  m_gotoStartButton = new QToolButton(this);
  m_gotoStartButton->setIcon(QIcon(":/icons/skip_to_start-512.png"));
  hlo->addWidget(m_gotoStartButton);

  //m_rewindButton = new QToolButton(this);
  //m_rewindButton->setIcon(QIcon(":/icons/rewind-512.png"));
  //hlo->addWidget(m_rewindButton);

  m_reversePlayButton = new QToolButton(this);
  m_reversePlayButton->setIcon(QIcon(":/icons/reverse_play-512.png"));
  hlo->addWidget(m_reversePlayButton);

  m_playButton = new QToolButton(this);
  m_playButton->setIcon(QIcon(":/icons/play-512.png"));
  hlo->addWidget(m_playButton);

  //m_fastForwardButton = new QToolButton(this);
  //m_fastForwardButton->setIcon(QIcon(":/icons/fast_forward-512.png"));
  //hlo->addWidget(m_fastForwardButton);

  m_gotoEndButton = new QToolButton(this);
  m_gotoEndButton->setIcon(QIcon(":/icons/end-512.png"));
  hlo->addWidget(m_gotoEndButton);

  hlo->addSpacing(5);

  m_repeatButton = new QToolButton(this);
  m_repeatButton->setIcon(QIcon(":/icons/repeat-512.png"));
  m_repeatButton->setCheckable(true);
  hlo->addWidget(m_repeatButton);

  hlo->addSpacing(20);

  hlo->addWidget(m_duration->createNameLabel(this));
  hlo->addWidget(m_duration->createWidget(this));

  hlo->addWidget(m_currentTime->createNameLabel(this));
  hlo->addWidget(m_currentTime->createWidget(this));

  m_saveKeyFrameButton = new QPushButton("Save Key Frame", this);
  hlo->addWidget(m_saveKeyFrameButton);

  m_timelineWidget = new ZTimelineWidget(m_animation, m_currentTime, this);

  QVBoxLayout *vlo = new QVBoxLayout;
  vlo->setMargin(0);
  vlo->setSpacing(0);
  vlo->addLayout(hlo);
  vlo->addWidget(m_timelineWidget);

  m_exportWidget = new ZAnimationExportWidget(m_animation.is2DAnimation(), this);
  m_exportWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_exportWidget->setVisible(false);
  connect(m_timelineWidget, SIGNAL(exportButtonToggled(bool)), m_exportWidget, SLOT(setVisible(bool)));
  if (m_animation.is2DAnimation()) {
    connect(m_exportWidget, SIGNAL(export2DAnimation(QDir,QString,double,int,int)),
            &m_animation, SLOT(export2DAnimation(QDir,QString,double,int,int)));
    connect(m_exportWidget, SIGNAL(export2DAnimation(QDir,QString,double)),
            &m_animation, SLOT(export2DAnimation(QDir,QString,double)));
  } else {
    connect(m_exportWidget, SIGNAL(export3DAnimation(QDir,QString,double,int,int,Z3DScreenShotType)),
            &m_animation, SLOT(export3DAnimation(QDir,QString,double,int,int,Z3DScreenShotType)));
    connect(m_exportWidget, SIGNAL(export3DAnimation(QDir,QString,double,Z3DScreenShotType)),
            &m_animation, SLOT(export3DAnimation(QDir,QString,double,Z3DScreenShotType)));
  }

  hlo = new QHBoxLayout;
  hlo->addWidget(m_exportWidget);
  hlo->addLayout(vlo);
  setLayout(hlo);
}

} // namespace nim
