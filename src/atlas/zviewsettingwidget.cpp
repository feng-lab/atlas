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
  layout->setMargin(0);
  setLayout(layout);
  connect(m_doc, SIGNAL(showViewSetting(size_t)), this, SLOT(showViewSettingWidgetOfObj(size_t)));
  connect(m_doc, SIGNAL(hideViewSetting()), this, SLOT(hideViewSettingWidget()));
  connect(m_doc, SIGNAL(objAboutToBeRemoved(size_t,ZObjDoc*)), this, SLOT(removeViewSettingWidgetOfObj(size_t)));
  connect(m_doc, SIGNAL(objInfoChanged(size_t)), this, SLOT(updateViewSettingWidgetLabelOfObj(size_t)));
  setMinimumHeight(250);
}

void ZViewSettingWidget::showDefaultWidget()
{
  if (m_widget->currentWidget() != m_defaultWidget)
    m_widget->setCurrentWidget(m_defaultWidget);
}

void ZViewSettingWidget::showViewSettingWidgetOfObj(size_t id)
{
  for (size_t i=0; i<m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      m_widget->setCurrentWidget(m_subWidgets[i].widget);
      return;
    }
  }
  std::shared_ptr<ZWidgetsGroup> wg = m_view->viewSettingWidgetsGroupOf(id);
  if (wg) {
    QLabel* label = new QLabel(m_doc->objNameWithModifiedMarkerAndID(id));
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    QWidget *wt = wg->createWidget(false, true, label);
    connect(wg.get(), SIGNAL(widgetsGroupChanged()), this, SLOT(updateWidget()));
    m_subWidgets.emplace_back(id, wg.get(), label, wt);
    m_widget->setCurrentIndex(m_widget->addWidget(wt));
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

void ZViewSettingWidget::updateViewSettingWidgetLabelOfObj(size_t id)
{
  for (size_t i=0; i<m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      m_subWidgets[i].label->setText(m_doc->objNameWithModifiedMarkerAndID(id));
      break;
    }
  }
}

void ZViewSettingWidget::updateWidget()
{
  ZWidgetsGroup *wg = qobject_cast<ZWidgetsGroup*>(sender());
  for (size_t i=0; i<m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].widgetsGroup == wg) {
      bool current = false;
      if (m_widget->currentWidget() == m_subWidgets[i].widget) {
        current = true;
        m_widget->setCurrentWidget(m_defaultWidget);
      }
      m_widget->removeWidget(m_subWidgets[i].widget);
      delete m_subWidgets[i].widget;

      m_subWidgets[i].label = new QLabel(m_doc->objNameWithModifiedMarkerAndID(m_subWidgets[i].id));
      m_subWidgets[i].label->setTextInteractionFlags(Qt::TextSelectableByMouse);
      m_subWidgets[i].label->setWordWrap(true);
      m_subWidgets[i].widget = wg->createWidget(false, true, m_subWidgets[i].label);
      if (current) {
        m_widget->setCurrentIndex(m_widget->addWidget(m_subWidgets[i].widget));
      }
    }
  }
}

} // namespace nim
