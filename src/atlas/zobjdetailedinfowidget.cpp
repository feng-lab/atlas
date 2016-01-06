#include "zobjdetailedinfowidget.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include "zwidgetsgroup.h"
#include "QsLog.h"

namespace nim {

ZObjDetailedInfoWidget::ZObjDetailedInfoWidget(ZDoc *doc, QWidget *mw)
  : QWidget(mw)
  , m_doc(doc)
{
  QVBoxLayout *layout = new QVBoxLayout;
  m_widget = new QStackedWidget;
  m_defaultWidget = new QWidget(this);
  m_widget->addWidget(m_defaultWidget);
  layout->addWidget(m_widget);
  setLayout(layout);
  connect(m_doc, SIGNAL(showViewSetting(size_t)), this, SLOT(showWidgetOfObj(size_t)));
  connect(m_doc, SIGNAL(hideViewSetting()), this, SLOT(hideWidget()));
  connect(m_doc, SIGNAL(objAboutToBeRemoved(size_t,ZObjDoc*)), this, SLOT(removeWidgetOfObj(size_t)));
  connect(m_doc, SIGNAL(objInfoChanged(size_t)), this, SLOT(updateWidgetLabelOfObj(size_t)));
  setMinimumHeight(250);
}

void ZObjDetailedInfoWidget::showDefaultWidget()
{
  if (m_widget->currentWidget() != m_defaultWidget)
    m_widget->setCurrentWidget(m_defaultWidget);
}

void ZObjDetailedInfoWidget::showWidgetOfObj(size_t id)
{
  for (size_t i=0; i<m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      m_widget->setCurrentWidget(m_subWidgets[i].widget);
      return;
    }
  }

  QString info = m_doc->objDetailedInfo(id);
  if (!info.isEmpty()) {
    ZWidgetsGroup wg("Info", 1);
    QLabel *label = new QLabel(info);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    wg.addChild(*label, 1);
    QWidget *wt = wg.createWidget(false, true, QString("%1").arg(m_doc->objNameWithModifiedMarkerAndID(id)));
    m_subWidgets.emplace_back(id, label, wt);
    m_widget->setCurrentIndex(m_widget->addWidget(wt));
  } else {
    m_widget->setCurrentWidget(m_defaultWidget);
  }
}

void ZObjDetailedInfoWidget::hideWidget()
{
  m_widget->setCurrentWidget(m_defaultWidget);
}

void ZObjDetailedInfoWidget::setDefaultWidget(QWidget *widget)
{
  if (m_widget->currentWidget() == m_defaultWidget) {
    m_widget->setCurrentIndex(m_widget->addWidget(widget));
  } else {
    m_widget->addWidget(widget);
  }
  m_defaultWidget = widget;
}

void ZObjDetailedInfoWidget::removeWidgetOfObj(size_t id)
{
  for (size_t i=0; i<m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      if (m_widget->currentWidget() == m_subWidgets[i].widget)
        m_widget->setCurrentWidget(m_defaultWidget);
      m_widget->removeWidget(m_subWidgets[i].widget);
      delete m_subWidgets[i].widget;
      m_subWidgets.erase(m_subWidgets.begin() + i);
      return;
    }
  }
}

void ZObjDetailedInfoWidget::updateWidgetLabelOfObj(size_t id)
{
  //LINFO() << "..";
  for (size_t i=0; i<m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      m_subWidgets[i].infoLabel->setText(m_doc->objDetailedInfo(id));
      QScrollArea *sa = dynamic_cast<QScrollArea*>(m_subWidgets[i].widget);
      if (sa && sa->widget()) {
        QBoxLayout *lo = dynamic_cast<QBoxLayout*>(sa->widget()->layout());
        //LWARN() << lo;
        if (lo && lo->count() > 0) {
          QWidget *firstWidget = lo->itemAt(0)->widget();
          //LWARN() << firstWidget;
          if (firstWidget) {
            QLabel *label = dynamic_cast<QLabel*>(firstWidget);
            if (label) {
              label->setText(m_doc->objNameWithModifiedMarkerAndID(id));
            }
          }
        }
      }
      return;
    }
  }
}

} // namespace nim
