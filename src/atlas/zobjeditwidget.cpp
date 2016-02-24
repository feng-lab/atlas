#include "zobjeditwidget.h"

#include <QTabBar>
#include "QsLog.h"
#include <QApplication>
#include <QScrollBar>

QsLogging::TextEditDestination::TextEditDestination(QPlainTextEdit &edit)
  : m_edit(edit)
{
  m_normalFormat = m_edit.currentCharFormat();
  m_errorFormat = m_normalFormat;
  m_errorFormat.setForeground(QBrush(QColor(176,0,0)));
}

void QsLogging::TextEditDestination::write(const LogMessage &message)
{
  bool atBottom = m_edit.verticalScrollBar()->value() == m_edit.verticalScrollBar()->maximum();
  if (message.level <= QsLogging::InfoLevel) {
    m_edit.appendPlainText(message.formatted);
  } else {
    m_edit.setCurrentCharFormat(m_errorFormat);
    m_edit.appendPlainText(message.formatted);
    m_edit.setCurrentCharFormat(m_normalFormat);
  }
  if (atBottom) {
    m_edit.verticalScrollBar()->setValue(m_edit.verticalScrollBar()->maximum());
    m_edit.verticalScrollBar()->setValue(m_edit.verticalScrollBar()->maximum());
  }
}

namespace nim {

ZObjEditWidget::ZObjEditWidget(ZDoc *doc, QWidget *mw)
  : QTabWidget(mw)
  , m_doc(doc)
  , m_logWidget(new QPlainTextEdit(this))
  //, m_logOutputDestination(new QsLogging::TextEditDestination(*m_logWidget))
  , m_logOutputDestination(QsLogging::DestinationFactory::MakeFunctorDestination(this, SLOT(writeLogMessage(QsLogging::LogMessage))))
{
  addTab(m_logWidget, "Log Output");
  connect(m_doc, SIGNAL(objAboutToBeRemoved(size_t,ZObjDoc*)), this, SLOT(removeObjEditWidgetOfObj(size_t)));
  connect(m_doc, SIGNAL(objInfoChanged(size_t)), this, SLOT(updateEditWidgetTitleOfObj(size_t)));
  setMinimumHeight(250);
  setTabsClosable(true);
  connect(this, SIGNAL(tabCloseRequested(int)), this, SLOT(closeTab(int)));
#ifdef __APPLE__
  tabBar()->tabButton(0, QTabBar::LeftSide)->hide();
#else
  tabBar()->tabButton(0, QTabBar::RightSide)->hide();
#endif

  m_normalFormat = m_logWidget->currentCharFormat();
  m_errorFormat = m_normalFormat;
  m_errorFormat.setForeground(QBrush(QColor(176,0,0)));
  for (auto const &lm : m_doc->logMessages()) {
    writeLogMessage(lm);
  }
  QsLogging::Logger::instance().addDestination(m_logOutputDestination);
}

ZObjEditWidget::~ZObjEditWidget()
{
  QsLogging::Logger::instance().removeDestination(m_logOutputDestination);
}

void ZObjEditWidget::showObjEditWidgetOfObj(size_t id)
{
  for (int i=0; i<m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      setCurrentWidget(m_subWidgets[i].widget);
      return;
    }
  }
  QWidget* wg = m_doc->createObjEditWidget(id);
  if (wg) {
    SubWidget sw;
    sw.id = id;
    sw.widget = wg;
    m_subWidgets.push_back(sw);
    setCurrentIndex(addTab(sw.widget, QString("Edit %1").arg(m_doc->objNameWithModifiedMarkerAndID(id))));
  } else {
    setCurrentWidget(m_logWidget);
  }
}

void ZObjEditWidget::updateEditWidgetTitleOfObj(size_t id)
{
  for (int i=0; i<m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      setTabText(indexOf(m_subWidgets[i].widget), QString("Edit %1").arg(m_doc->objNameWithModifiedMarkerAndID(id)));
      return;
    }
  }
}

void ZObjEditWidget::writeLogMessage(const QsLogging::LogMessage &message)
{
  bool atBottom = m_logWidget->verticalScrollBar()->value() == m_logWidget->verticalScrollBar()->maximum();
  if (message.level <= QsLogging::InfoLevel) {
    m_logWidget->appendPlainText(message.formatted);
  } else {
    m_logWidget->setCurrentCharFormat(m_errorFormat);
    m_logWidget->appendPlainText(message.formatted);
    m_logWidget->setCurrentCharFormat(m_normalFormat);
  }
  if (atBottom) {
    m_logWidget->verticalScrollBar()->setValue(m_logWidget->verticalScrollBar()->maximum());
    m_logWidget->verticalScrollBar()->setValue(m_logWidget->verticalScrollBar()->maximum());
  }
}

void ZObjEditWidget::removeObjEditWidgetOfObj(size_t id)
{
  for (int i=0; i<m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      removeTab(indexOf(m_subWidgets[i].widget));
      delete m_subWidgets[i].widget;
      m_subWidgets.removeAt(i);
      return;
    }
  }
}

void ZObjEditWidget::closeTab(int index)
{
  if (index == 0)
    return;
  QWidget *wgt = widget(index);
  removeTab(index);
  for (int i=0; i<m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].widget == wgt) {
      delete m_subWidgets[i].widget;
      m_subWidgets.removeAt(i);
      return;
    }
  }
}

} // namespace nim
