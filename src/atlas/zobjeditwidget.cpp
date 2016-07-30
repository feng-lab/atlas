#include "zobjeditwidget.h"

#include <QTabBar>
#include <QApplication>
#include <QScrollBar>
#include "zobjdoc.h"
#include "zlog.h"
#include "zlogmodelsink.h"

namespace nim {

ZObjEditWidget::ZObjEditWidget(ZDoc* doc, QWidget* mw)
  : QTabWidget(mw)
  , m_doc(doc)
  , m_logWidget(new QPlainTextEdit(this))
{
  addTab(m_logWidget, "Log Output");
  connect(m_doc, &ZDoc::objAboutToBeRemoved, this, &ZObjEditWidget::removeObjEditWidgetOfObj);
  connect(m_doc, &ZDoc::objInfoChanged, this, &ZObjEditWidget::updateEditWidgetTitleOfObj);
  setMinimumHeight(250);
  setTabsClosable(true);
  connect(this, &ZObjEditWidget::tabCloseRequested, this, &ZObjEditWidget::closeTab);
#ifdef __APPLE__
  tabBar()->tabButton(0, QTabBar::LeftSide)->hide();
#else
  tabBar()->tabButton(0, QTabBar::RightSide)->hide();
#endif

  m_logWidget->setCenterOnScroll(true);
  m_normalFormat = m_logWidget->currentCharFormat();
  m_errorFormat = m_normalFormat;
  m_errorFormat.setForeground(QBrush(QColor(176, 0, 0)));
  writeLogData(&logMessagesSoFar(), 0, logMessagesSoFar().size());
  receiveFutureLogMessages(this, &ZObjEditWidget::writeLogData);
}

bool ZObjEditWidget::showObjEditWidgetOfObj(size_t id)
{
  for (int i = 0; i < m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      setCurrentWidget(m_subWidgets[i].widget);
      return true;
    }
  }
  QWidget* wg = m_doc->createObjEditWidget(id);
  if (wg) {
    SubWidget sw;
    sw.id = id;
    sw.widget = wg;
    m_subWidgets.push_back(sw);
    setCurrentIndex(addTab(sw.widget, QString("Edit %1").arg(m_doc->objNameWithModifiedMarkerAndID(id))));
    return true;
  } else {
    setCurrentWidget(m_logWidget);
  }
  return false;
}

void ZObjEditWidget::updateEditWidgetTitleOfObj(size_t id)
{
  for (int i = 0; i < m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      setTabText(indexOf(m_subWidgets[i].widget), QString("Edit %1").arg(m_doc->objNameWithModifiedMarkerAndID(id)));
      return;
    }
  }
}

void ZObjEditWidget::writeLogData(const QList<LogData>* messages, int start, int end)
{
  if (end - start == 1) {
    if (messages->at(start).level <= InfoLevel) {
      m_logWidget->appendPlainText(messages->at(start).formatted);
    } else {
      m_logWidget->setCurrentCharFormat(m_errorFormat);
      m_logWidget->appendPlainText(messages->at(start).formatted);
      m_logWidget->setCurrentCharFormat(m_normalFormat);
    }
  } else {
    bool firstFormat = messages->at(start).level <= InfoLevel;
    bool lastFormat = firstFormat;
    QList<QStringList> textList;
    textList.push_back(QStringList());
    textList.back().push_back(messages->at(start).formatted);
    for (int i = start + 1; i < end; ++i) {
      if ((messages->at(i).level <= InfoLevel) != lastFormat) {
        lastFormat = !lastFormat;
        textList.push_back(QStringList());
      }
      textList.back().push_back(messages->at(i).formatted);
    }
    for (int i = 0; i < textList.size(); ++i) {
      m_logWidget->setCurrentCharFormat(firstFormat ? m_normalFormat : m_errorFormat);
      firstFormat = !firstFormat;
      m_logWidget->appendPlainText(textList[i].join("\n"));
    }
    m_logWidget->setCurrentCharFormat(m_normalFormat);
  }
}

void ZObjEditWidget::removeObjEditWidgetOfObj(size_t id)
{
  for (int i = 0; i < m_subWidgets.size(); ++i) {
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
  QWidget* wgt = widget(index);
  removeTab(index);
  for (int i = 0; i < m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].widget == wgt) {
      delete m_subWidgets[i].widget;
      m_subWidgets.removeAt(i);
      return;
    }
  }
}

} // namespace nim
