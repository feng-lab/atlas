#include "zviewsettingwidget.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include "QsLog.h"

namespace nim {

ZViewSettingWidget::ZViewSettingWidget(ZDoc *doc, ZViewSettingInterface *view, QWidget *mw)
  : QWidget(mw)
  , m_doc(doc)
  , m_view(view)
{
  QVBoxLayout *layout = new QVBoxLayout;
  m_widget = new QStackedWidget;
  m_defaultWidget = new QWidget(this);
  m_widget->addWidget(m_defaultWidget);
  layout->addWidget(m_widget);
  setLayout(layout);
  connect(m_doc, SIGNAL(showViewSetting(size_t)), this, SLOT(showViewSettingWidgetOfObj(size_t)));
  connect(m_doc, SIGNAL(hideViewSetting()), this, SLOT(hideViewSettingWidget()));
  connect(m_doc, SIGNAL(objAboutToBeRemoved(size_t,ZObjDoc*)), this, SLOT(removeViewSettingWidgetOfObj(size_t)));
  connect(m_doc, SIGNAL(objInfoChanged(size_t)), this, SLOT(updateViewSettingWidgetLabelOfObj(size_t)));
  setMinimumHeight(250);
}

ZViewSettingWidget::~ZViewSettingWidget()
{
  for (int i=0; i<m_subWidgets.size(); ++i) {
    delete m_subWidgets[i].widgetsGroup;
  }
}

void ZViewSettingWidget::showDefaultWidget()
{
  if (m_widget->currentWidget() != m_defaultWidget)
    m_widget->setCurrentWidget(m_defaultWidget);
}

void ZViewSettingWidget::showViewSettingWidgetOfObj(size_t id)
{
  for (int i=0; i<m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      m_widget->setCurrentWidget(m_subWidgets[i].widget);
      return;
    }
  }
  ZWidgetsGroup* wg = m_view->viewSettingWidgetsGroupOf(id);
  if (wg) {
    SubWidget sw;
    sw.id = id;
    sw.widgetsGroup = wg;
    sw.widget = wg->createWidget(false, true, QString("%1").arg(m_doc->objNameWithModifiedMarkerAndID(id)));
    connect(wg, SIGNAL(widgetsGroupChanged()), this, SLOT(updateWidget()));
    m_subWidgets.push_back(sw);
    m_widget->setCurrentIndex(m_widget->addWidget(sw.widget));
  } else {
    m_widget->setCurrentWidget(m_defaultWidget);
  }
}

void ZViewSettingWidget::hideViewSettingWidget()
{
  m_widget->setCurrentWidget(m_defaultWidget);
}

void ZViewSettingWidget::setDefaultWidget(QWidget *widget)
{
  if (m_widget->currentWidget() == m_defaultWidget) {
    m_widget->setCurrentIndex(m_widget->addWidget(widget));
  } else {
    m_widget->addWidget(widget);
  }
  m_defaultWidget = widget;
}

void ZViewSettingWidget::removeViewSettingWidgetOfObj(size_t id)
{
  for (int i=0; i<m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      if (m_widget->currentWidget() == m_subWidgets[i].widget)
        m_widget->setCurrentWidget(m_defaultWidget);
      m_widget->removeWidget(m_subWidgets[i].widget);
      delete m_subWidgets[i].widget;
      delete m_subWidgets[i].widgetsGroup;
      m_subWidgets.removeAt(i);
      return;
    }
  }
}

void ZViewSettingWidget::updateViewSettingWidgetLabelOfObj(size_t id)
{
  //LINFO() << "..";
  for (int i=0; i<m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
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

void ZViewSettingWidget::updateWidget()
{
  ZWidgetsGroup *wg = qobject_cast<ZWidgetsGroup*>(sender());
  for (int i=0; i<m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].widgetsGroup == wg) {
      bool current = false;
      if (m_widget->currentWidget() == m_subWidgets[i].widget) {
        current = true;
        m_widget->setCurrentWidget(m_defaultWidget);
      }
      m_widget->removeWidget(m_subWidgets[i].widget);
      delete m_subWidgets[i].widget;
      m_subWidgets[i].widget = wg->createWidget(false, true, QString("%1").arg(m_doc->objNameWithModifiedMarkerAndID(m_subWidgets[i].id)));
      if (current) {
        m_widget->setCurrentIndex(m_widget->addWidget(m_subWidgets[i].widget));
      }
    }
  }
}

} // namespace nim
