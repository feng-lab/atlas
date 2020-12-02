#include "zviewsettingwidget.h"

#include "zlog.h"
#include "zobjdoc.h"
#include <QVBoxLayout>
#include <QLabel>

namespace nim {

ZViewSettingWidget::ZViewSettingWidget(ZDoc& doc, ZViewSettingInterface* view, QWidget* mw)
  : QWidget(mw)
  , m_doc(doc)
  , m_view(view)
{
  auto layout = new QVBoxLayout;
  m_widget = new QStackedWidget;
  m_defaultWidget = new QWidget(this);
  m_widget->addWidget(m_defaultWidget);
  layout->addWidget(m_widget);
  layout->setContentsMargins(0, 0, 0, 0);
  setLayout(layout);
  connect(&m_doc, &ZDoc::showViewSetting, this, &ZViewSettingWidget::showViewSettingWidgetOfObj);
  connect(&m_doc, &ZDoc::hideViewSetting, this, &ZViewSettingWidget::hideViewSettingWidget);
  connect(&m_doc, &ZDoc::objAboutToBeRemoved, this, &ZViewSettingWidget::removeViewSettingWidgetOfObj);
  connect(&m_doc, &ZDoc::objInfoChanged, this, &ZViewSettingWidget::updateViewSettingWidgetLabelOfObj);
  setMinimumHeight(250);
}

void ZViewSettingWidget::showDefaultWidget()
{
  if (m_widget->currentWidget() != m_defaultWidget)
    m_widget->setCurrentWidget(m_defaultWidget);
}

void ZViewSettingWidget::showViewSettingWidgetOfObj(size_t id)
{
  for (auto& m_subWidget : m_subWidgets) {
    if (m_subWidget.id == id) {
      m_widget->setCurrentWidget(m_subWidget.widget);
      return;
    }
  }
  std::shared_ptr<ZWidgetsGroup> wg = m_view->viewSettingWidgetsGroupOf(id);
  if (wg) {
    auto label = new QLabel(m_doc.objNameWithModifiedMarkerAndID(id));
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    QWidget* wt = wg->createWidget(false, true, label);
    connect(wg.get(), &ZWidgetsGroup::widgetsGroupChanged, this, &ZViewSettingWidget::updateWidget);
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

void ZViewSettingWidget::setDefaultWidget(QWidget* widget)
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
  for (size_t i = 0; i < m_subWidgets.size(); ++i) {
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
  for (auto& m_subWidget : m_subWidgets) {
    if (m_subWidget.id == id) {
      m_subWidget.label->setText(m_doc.objNameWithModifiedMarkerAndID(id));
      break;
    }
  }
}

void ZViewSettingWidget::updateWidget()
{
  if (auto wg = qobject_cast<ZWidgetsGroup*>(sender())) {
    for (auto& m_subWidget : m_subWidgets) {
      if (m_subWidget.widgetsGroup == wg) {
        bool current = false;
        if (m_widget->currentWidget() == m_subWidget.widget) {
          current = true;
          m_widget->setCurrentWidget(m_defaultWidget);
        }
        wg->protectWidgetChildren();
        m_widget->removeWidget(m_subWidget.widget);
        delete m_subWidget.widget;

        m_subWidget.label = new QLabel(m_doc.objNameWithModifiedMarkerAndID(m_subWidget.id));
        m_subWidget.label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_subWidget.label->setWordWrap(true);
        m_subWidget.widget = wg->createWidget(false, true, m_subWidget.label);
        if (current) {
          m_widget->setCurrentIndex(m_widget->addWidget(m_subWidget.widget));
        }
      }
    }
  }
}

} // namespace nim
