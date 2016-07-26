#ifndef ZOBJEDITWIDGET_H
#define ZOBJEDITWIDGET_H

#include <QStackedWidget>
#include <QPlainTextEdit>
#include <QTabWidget>
#include "zdoc.h"

namespace nim {

#ifndef _USE_QSLOG_
struct LogData;
#endif

class ZObjEditWidget : public QTabWidget
{
  Q_OBJECT
public:
  explicit ZObjEditWidget(ZDoc *doc, QWidget *mw = nullptr);

  bool showObjEditWidgetOfObj(size_t id);
  void updateEditWidgetTitleOfObj(size_t id);

private:
  void writeLogData(const LogData *message);

  void removeObjEditWidgetOfObj(size_t id);
  void closeTab(int index);

protected:
  ZDoc *m_doc;

  struct SubWidget {
    size_t id;
    QWidget* widget;
  };

  QPlainTextEdit* m_logWidget;
  QTextCharFormat m_normalFormat;
  QTextCharFormat m_errorFormat;
  QList<SubWidget> m_subWidgets;
};

} // namespace nim

#endif // ZOBJEDITWIDGET_H
