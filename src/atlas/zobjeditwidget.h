#pragma once

#include "zdoc.h"
#include <QStackedWidget>
#include <QPlainTextEdit>
#include <QTabWidget>

namespace nim {

struct LogData;

class ZObjEditWidget : public QTabWidget
{
Q_OBJECT
public:
  explicit ZObjEditWidget(ZDoc* doc, QWidget* mw = nullptr);

  bool showObjEditWidgetOfObj(size_t id);

  void updateEditWidgetTitleOfObj(size_t id);

private:
  void writeLogData(const QList<LogData>* messages, int start, int end);

  void removeObjEditWidgetOfObj(size_t id);

  void closeTab(int index);

protected:
  ZDoc* m_doc;

  struct SubWidget
  {
    size_t id;
    QWidget* widget;
  };

  QPlainTextEdit* m_logWidget;
  QTextCharFormat m_normalFormat;
  QTextCharFormat m_errorFormat;
  QList<SubWidget> m_subWidgets;
};

} // namespace nim

