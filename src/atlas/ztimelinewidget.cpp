#include "ztimelinewidget.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QScrollBar>
#include "ztimelineeventview.h"
#include "ztimelineobjview.h"
#include "ztimelineaxisview.h"
#include <QToolBar>
#include <QAction>
#include "QsLog.h"
#include "znumericparameter.h"

namespace nim {

ZTimelineWidget::ZTimelineWidget(ZAnimation &ani, ZDoubleParameter *currentTimePara, QWidget *parent)
  : QWidget(parent)
  , m_animation(ani)
  , m_currentTimePara(currentTimePara)
  , m_pixelsPerSecond(10)
{
  m_eventViewWidth = 10 + int(std::ceil((m_animation.duration()  + 10) * pixelsPerSecond()));
  m_axis = new ZTimelineAxisView(*this);
  m_axis->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_axis->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_objView = new ZTimelineObjView(*this);
  m_objView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_objView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_eventView = new ZTimelineEventView(*this);
  m_eventView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  m_eventView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

  m_exportButton = new QToolButton(this);
  m_exportButton->setIcon(QIcon(":/icons/camcoder_pro-512.png"));
  m_exportButton->setStatusTip(tr("Export Animation"));
  //m_exportButton->setStyleSheet("border-style: none;");
  m_exportButton->setCheckable(true);
  connect(m_exportButton, SIGNAL(toggled(bool)), this, SIGNAL(exportButtonToggled(bool)));

  m_cleanupButton = new QToolButton(this);
  m_cleanupButton->setIcon(QIcon(":/icons/clean_up.png"));
  m_cleanupButton->setStatusTip(tr("Remove Redundant Keys"));
  m_cleanupButton->setStyleSheet("border-style: none;");
  connect(m_cleanupButton, SIGNAL(clicked()), &m_animation, SLOT(removeRedundantKeys()));

  m_zoomInButton = new QToolButton(this);
  m_zoomInButton->setIcon(QIcon(":/icons/zoom_in-512.png"));
  m_zoomInButton->setStatusTip(tr("Zoom In Timeline"));
  m_zoomInButton->setStyleSheet("border-style: none;");
  connect(m_zoomInButton, SIGNAL(clicked()), this, SLOT(zoomIn()));

  m_zoomOutButton = new QToolButton(this);
  m_zoomOutButton->setIcon(QIcon(":/icons/zoom_out-512.png"));
  m_zoomOutButton->setStatusTip(tr("Zoom Out Timeline"));
  m_zoomOutButton->setStyleSheet("border-style: none;");
  connect(m_zoomOutButton, SIGNAL(clicked()), this, SLOT(zoomOut()));

  m_expandButton = new QToolButton(this);
  m_expandButton->setIcon(QIcon(":/icons/expand-512.png"));
  m_expandButton->setStatusTip(tr("Fit Timeline to Window"));
  m_expandButton->setStyleSheet("border-style: none;");
  connect(m_expandButton, SIGNAL(clicked()), this, SLOT(expandToFit()));

  QHBoxLayout *hlo = new QHBoxLayout;
  hlo->addSpacing(objViewWidth()-125);
  hlo->addWidget(m_exportButton, 0, Qt::AlignCenter);
  hlo->addWidget(m_cleanupButton, 0, Qt::AlignCenter);
  hlo->addWidget(m_zoomInButton, 0, Qt::AlignCenter);
  hlo->addWidget(m_zoomOutButton, 0, Qt::AlignCenter);
  hlo->addWidget(m_expandButton, 0, Qt::AlignCenter);

  QGridLayout *lo = new QGridLayout;
  lo->setSpacing(0);
  lo->setMargin(0);
  lo->addLayout(hlo, 0, 0);
  lo->addWidget(m_axis, 0, 1);
  lo->addWidget(m_objView, 1, 0);
  lo->addWidget(m_eventView, 1, 1);
  setLayout(lo);

  connect(m_objView, SIGNAL(vScrollBarValueChanged(int)),
          m_eventView->verticalScrollBar(), SLOT(setValue(int)));
  connect(m_eventView->verticalScrollBar(), SIGNAL(valueChanged(int)),
          m_objView->verticalScrollBar(), SLOT(setValue(int)));

  connect(m_objView->verticalScrollBar(), SIGNAL(rangeChanged(int,int)),
          this, SLOT(objViewVerticalScrollBarRangeChanged(int,int)));
  connect(m_eventView->verticalScrollBar(), SIGNAL(rangeChanged(int,int)),
          this, SLOT(eventViewVerticalScrollBarRangeChanged(int,int)));

  connect(m_eventView->horizontalScrollBar(), SIGNAL(valueChanged(int)),
          m_axis->horizontalScrollBar(), SLOT(setValue(int)));
  connect(m_axis->horizontalScrollBar(), SIGNAL(valueChanged(int)),
          m_eventView->horizontalScrollBar(), SLOT(setValue(int)));

  connect(&m_animation, SIGNAL(durationChanged(double)), this, SLOT(updateEventViewWidth()));
  connect(m_currentTimePara, SIGNAL(valueChanged()), this, SIGNAL(currentTimeChanged()));

  eventViewVerticalScrollBarRangeChanged(m_eventView->verticalScrollBar()->minimum(),
                                         m_eventView->verticalScrollBar()->maximum());
}

QSize ZTimelineWidget::sizeHint() const
{
  return QSize(400, 180);
}

double ZTimelineWidget::currentTime() const
{
  return m_currentTimePara->get();
}

void ZTimelineWidget::removeSelectedKeys()
{
  m_eventView->removeSelectedKeys();
}

void ZTimelineWidget::setCurrentTime(double t)
{
  m_currentTimePara->set(t);
}

void ZTimelineWidget::eventViewVerticalScrollBarRangeChanged(int min, int max)
{
  m_objView->setScrollEnabled(min != 0 || max != 0);
  m_objView->verticalScrollBar()->setRange(min, max);
}

void ZTimelineWidget::zoomIn()
{
  if (m_pixelsPerSecond * 1.1 <= 15000) {
    m_pixelsPerSecond *= 1.1;
    updateEventViewWidth();
    emit pixelsPerSecondChagned();
  }
}

void ZTimelineWidget::zoomOut()
{
  if (m_pixelsPerSecond / 1.1 >= 150. / m_animation.duration() / 10) {
    m_pixelsPerSecond /= 1.1;
    updateEventViewWidth();
    emit pixelsPerSecondChagned();
  }
}

void ZTimelineWidget::expandToFit()
{
  m_pixelsPerSecond = (m_eventView->width() - 20. - m_eventView->verticalScrollBar()->width()) / m_animation.duration();
  updateEventViewWidth();
  emit pixelsPerSecondChagned();
  m_eventView->horizontalScrollBar()->setValue(0);
}

void ZTimelineWidget::updateEventViewWidth()
{
  int wth = std::max(m_eventView->width(), 10 + int(std::ceil((m_animation.duration()  + 10) * pixelsPerSecond())));
  if (wth != m_eventViewWidth) {
    m_eventViewWidth = wth;
    emit eventViewWidthChanged();
  }
}

void ZTimelineWidget::objViewVerticalScrollBarRangeChanged(int min, int max)
{
  if (min != m_eventView->verticalScrollBar()->minimum() ||
      max != m_eventView->verticalScrollBar()->maximum())
    eventViewVerticalScrollBarRangeChanged(m_eventView->verticalScrollBar()->minimum(),
                                           m_eventView->verticalScrollBar()->maximum());
}

void ZTimelineWidget::showValue(int i)
{
  LINFO() << i;
}

void ZTimelineWidget::resizeEvent(QResizeEvent *event)
{
  updateEventViewWidth();
  QWidget::resizeEvent(event);
}

} // namespace nim
