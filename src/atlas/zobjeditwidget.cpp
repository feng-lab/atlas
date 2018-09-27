#include "zobjeditwidget.h"

#include "zobjdoc.h"
#include "zlog.h"
#include "zexception.h"
#include <QTabBar>
#include <QApplication>
#include <QScrollBar>
#include <iostream>

namespace nim {

ZObjEditWidget::ZObjEditWidget(ZDoc* doc, QWidget* mw)
  : QTabWidget(mw)
  , m_doc(doc)
  , m_logWidget(new ZLogWidget(true, this))
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
  }
  setCurrentWidget(m_logWidget);
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
