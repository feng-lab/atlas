#include "zobjeditwidget.h"

#include "zobjdoc.h"
#include "zlog.h"
#include "zexception.h"
#include <QTabBar>
#include <QApplication>
#include <QScrollBar>

namespace nim {

ZObjEditWidget::ZObjEditWidget(ZDoc& doc, QWidget* mw)
  : QTabWidget(mw)
  , m_doc(doc)
  , m_logWidget(new ZLogWidget(true, this))
{
  addTab(m_logWidget, "Log Output");
  connect(&m_doc, &ZDoc::objAboutToBeRemoved, this, &ZObjEditWidget::removeObjEditWidgetOfObj);
  connect(&m_doc, &ZDoc::objInfoChanged, this, &ZObjEditWidget::updateEditWidgetTitleOfObj);
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
  for (auto& subWidget : m_subWidgets) {
    if (subWidget.id == id) {
      setCurrentWidget(subWidget.widget);
      return true;
    }
  }
  QWidget* wg = m_doc.createObjEditWidget(id);
  if (wg) {
    SubWidget sw{};
    sw.id = id;
    sw.widget = wg;
    m_subWidgets.push_back(sw);
    setCurrentIndex(addTab(sw.widget, QString("Edit %1").arg(m_doc.objNameWithModifiedMarkerAndID(id))));
    return true;
  }
  setCurrentWidget(m_logWidget);
  return false;
}

void ZObjEditWidget::updateEditWidgetTitleOfObj(size_t id)
{
  for (auto& subWidget : m_subWidgets) {
    if (subWidget.id == id) {
      setTabText(indexOf(subWidget.widget), QString("Edit %1").arg(m_doc.objNameWithModifiedMarkerAndID(id)));
      return;
    }
  }
}

void ZObjEditWidget::removeObjEditWidgetOfObj(size_t id)
{
  for (size_t i = 0; i < m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].id == id) {
      removeTab(indexOf(m_subWidgets[i].widget));
      delete m_subWidgets[i].widget;
      m_subWidgets.erase(m_subWidgets.begin() + i);
      return;
    }
  }
}

void ZObjEditWidget::closeTab(int index)
{
  if (index == 0) {
    return;
  }
  QWidget* wgt = widget(index);
  removeTab(index);
  for (size_t i = 0; i < m_subWidgets.size(); ++i) {
    if (m_subWidgets[i].widget == wgt) {
      delete m_subWidgets[i].widget;
      m_subWidgets.erase(m_subWidgets.begin() + i);
      return;
    }
  }
}

} // namespace nim
