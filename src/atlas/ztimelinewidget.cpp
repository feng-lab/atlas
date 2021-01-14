#include "ztimelinewidget.h"

#include "ztimelineeventview.h"
#include "ztimelineobjview.h"
#include "ztimelineaxisview.h"
#include "zlog.h"
#include "znumericparameter.h"
#include "ztheme.h"
#include <QHBoxLayout>
#include <QScrollBar>
#include <QAction>

namespace nim {

ZTimelineWidget::ZTimelineWidget(ZAnimation& ani, ZDoubleParameter* currentTimePara, QWidget* parent)
  : QWidget(parent)
  , m_animation(ani)
  , m_currentTimePara(currentTimePara)
  , m_pixelsPerSecond(10)
{
  m_eventViewWidth = 10 + int(std::ceil((m_animation.duration() + 10) * pixelsPerSecond()));
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
  m_exportButton->setIcon(ZTheme::instance().icon(ZTheme::CamcoderIcon));
  m_exportButton->setCheckable(true);
  m_exportButton->setStatusTip(tr("Export Animation"));
  //m_expandButton->setToolTip(tr("Export Animation")); // todo: why crash?
  //m_exportButton->setStyleSheet("border-style: none;");
  connect(m_exportButton, &QToolButton::toggled, this, &ZTimelineWidget::exportButtonToggled);

  m_cleanupButton = new QToolButton(this);
  m_cleanupButton->setIcon(ZTheme::instance().icon(ZTheme::CleanupIcon));
  m_cleanupButton->setStatusTip(tr("Remove Redundant Keys"));
  m_cleanupButton->setToolTip(tr("Remove Redundant Keys"));
  //m_cleanupButton->setStyleSheet("border-style: none;");
  connect(m_cleanupButton, &QToolButton::clicked, &m_animation, &ZAnimation::removeRedundantKeys);

  m_zoomInButton = new QToolButton(this);
  m_zoomInButton->setIcon(ZTheme::instance().icon(ZTheme::ZoomInIcon));
  m_zoomInButton->setStatusTip(tr("Zoom In Timeline"));
  m_zoomInButton->setToolTip(tr("Zoom In Timeline"));
  //m_zoomInButton->setStyleSheet("border-style: none;");
  connect(m_zoomInButton, &QToolButton::clicked, this, &ZTimelineWidget::zoomIn);

  m_zoomOutButton = new QToolButton(this);
  m_zoomOutButton->setIcon(ZTheme::instance().icon(ZTheme::ZoomOutIcon));
  m_zoomOutButton->setStatusTip(tr("Zoom Out Timeline"));
  m_zoomOutButton->setToolTip(tr("Zoom Out Timeline"));
  //m_zoomOutButton->setStyleSheet("border-style: none;");
  connect(m_zoomOutButton, &QToolButton::clicked, this, &ZTimelineWidget::zoomOut);

  m_expandButton = new QToolButton(this);
  m_expandButton->setIcon(ZTheme::instance().icon(ZTheme::ExpandIcon));
  m_expandButton->setStatusTip(tr("Fit Timeline to Window"));
  m_expandButton->setToolTip(tr("Fit Timeline to Window"));
  //m_expandButton->setStyleSheet("border-style: none;");
  connect(m_expandButton, &QToolButton::clicked, this, &ZTimelineWidget::expandToFit);

  auto hlo = new QHBoxLayout;
  hlo->addSpacing(objViewWidth() - 125);
  hlo->addWidget(m_exportButton, 0, Qt::AlignCenter);
  hlo->addWidget(m_cleanupButton, 0, Qt::AlignCenter);
  hlo->addWidget(m_zoomInButton, 0, Qt::AlignCenter);
  hlo->addWidget(m_zoomOutButton, 0, Qt::AlignCenter);
  hlo->addWidget(m_expandButton, 0, Qt::AlignCenter);

  auto lo = new QGridLayout;
  lo->setSpacing(0);
  lo->setContentsMargins(0, 0, 0, 0);
  lo->addLayout(hlo, 0, 0);
  lo->addWidget(m_axis, 0, 1);
  lo->addWidget(m_objView, 1, 0);
  lo->addWidget(m_eventView, 1, 1);
  setLayout(lo);

  connect(m_objView, &ZTimelineObjView::vScrollBarValueChanged,
          m_eventView->verticalScrollBar(), &QScrollBar::setValue);
  connect(m_eventView->verticalScrollBar(), &QScrollBar::valueChanged,
          m_objView->verticalScrollBar(), &QScrollBar::setValue);

  connect(m_objView->verticalScrollBar(), &QScrollBar::rangeChanged,
          this, &ZTimelineWidget::objViewVerticalScrollBarRangeChanged);
  connect(m_eventView->verticalScrollBar(), &QScrollBar::rangeChanged,
          this, &ZTimelineWidget::eventViewVerticalScrollBarRangeChanged);

  connect(m_eventView->horizontalScrollBar(), &QScrollBar::valueChanged,
          m_axis->horizontalScrollBar(), &QScrollBar::setValue);
  connect(m_axis->horizontalScrollBar(), &QScrollBar::valueChanged,
          m_eventView->horizontalScrollBar(), &QScrollBar::setValue);

  connect(&m_animation, &ZAnimation::durationChanged, this, &ZTimelineWidget::updateEventViewWidth);
  connect(m_currentTimePara, &ZDoubleParameter::valueChanged, this, &ZTimelineWidget::currentTimeChanged);

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
  int wth = std::max(m_eventView->width(), 10 + int(std::ceil((m_animation.duration() + 10) * pixelsPerSecond())));
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
  LOG(INFO) << i;
}

void ZTimelineWidget::resizeEvent(QResizeEvent* event)
{
  updateEventViewWidth();
  QWidget::resizeEvent(event);
}

} // namespace nim
