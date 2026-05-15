#include "zobjdetailedinfowidget.h"

#include "zwidgetsgroup.h"
#include "zlog.h"
#include "zobjdoc.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QShowEvent>

namespace nim {

ZObjDetailedInfoWidget::ZObjDetailedInfoWidget(ZDoc& doc, QWidget* mw)
  : QWidget(mw)
  , m_doc(doc)
{
  auto layout = new QVBoxLayout;
  m_widget = new QStackedWidget;
  m_defaultWidget = new QWidget(this);
  m_widget->addWidget(m_defaultWidget);
  layout->addWidget(m_widget);
  setLayout(layout);
  connect(&m_doc, &ZDoc::showViewSetting, this, &ZObjDetailedInfoWidget::showWidgetOfObj);
  connect(&m_doc, &ZDoc::hideViewSetting, this, &ZObjDetailedInfoWidget::hideWidget);
  connect(&m_doc, &ZDoc::objAboutToBeRemoved, this, &ZObjDetailedInfoWidget::removeWidgetOfObj);
  connect(&m_doc, &ZDoc::objInfoChanged, this, &ZObjDetailedInfoWidget::updateWidgetLabelOfObj);
  setMinimumHeight(250);
}

void ZObjDetailedInfoWidget::showDefaultWidget()
{
  if (m_widget->currentWidget() != m_defaultWidget) {
    m_widget->setCurrentWidget(m_defaultWidget);
  }
}

void ZObjDetailedInfoWidget::showWidgetOfObj(size_t id)
{
  m_requestedObjId = id;
  if (!isVisible()) {
    return;
  }

  materializeWidgetOfObj(id);
}

void ZObjDetailedInfoWidget::showEvent(QShowEvent* event)
{
  QWidget::showEvent(event);
  if (m_requestedObjId > 0) {
    materializeWidgetOfObj(m_requestedObjId);
  }
}

void ZObjDetailedInfoWidget::materializeWidgetOfObj(size_t id)
{
  for (size_t i = 0; i < m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      m_widget->setCurrentWidget(m_subWidgets[i].widget);
      return;
    }
  }

  QString info = m_doc.objDetailedInfo(id);
  if (!info.isEmpty()) {
    ZWidgetsGroup wg("Info", 1);
    auto infoLabel = new QTextEdit();
    infoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoLabel->setReadOnly(true);
    infoLabel->setWordWrapMode(QTextOption::WordWrap);
    infoLabel->setPlainText(info);
    wg.addChild(*infoLabel, 1);
    QLabel* label = new QLabel(m_doc.objNameWithModifiedMarkerAndID(id));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    QWidget* wt = wg.createWidget(false, false, label, true);
    m_subWidgets.emplace_back(id, infoLabel, label, wt);
    m_widget->setCurrentIndex(m_widget->addWidget(wt));
  } else {
    m_widget->setCurrentWidget(m_defaultWidget);
  }
}

void ZObjDetailedInfoWidget::hideWidget()
{
  m_requestedObjId = 0;
  m_widget->setCurrentWidget(m_defaultWidget);
}

void ZObjDetailedInfoWidget::setDefaultWidget(QWidget* widget)
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
  if (m_requestedObjId == id) {
    m_requestedObjId = 0;
  }

  for (size_t i = 0; i < m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      if (m_widget->currentWidget() == m_subWidgets[i].widget) {
        m_widget->setCurrentWidget(m_defaultWidget);
      }
      m_widget->removeWidget(m_subWidgets[i].widget);
      delete m_subWidgets[i].widget;
      m_subWidgets.erase(m_subWidgets.begin() + i);
      return;
    }
  }
}

void ZObjDetailedInfoWidget::updateWidgetLabelOfObj(size_t id)
{
  // VLOG(1) << "..";
  for (size_t i = 0; i < m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      m_subWidgets[i].infoLabel->setPlainText(m_doc.objDetailedInfo(id));
      m_subWidgets[i].label->setText(m_doc.objNameWithModifiedMarkerAndID(id));
      break;
    }
  }
}

} // namespace nim
