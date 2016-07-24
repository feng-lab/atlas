#ifndef ZOBJEDITWIDGET_H
#define ZOBJEDITWIDGET_H

#include <QStackedWidget>
#include <QPlainTextEdit>
#include <QTabWidget>
#include "zdoc.h"
#include "zlog.h"

namespace nim {

struct LogData;

class ZObjEditWidget : public QTabWidget
{
  Q_OBJECT
public:
  explicit ZObjEditWidget(ZDoc *doc, QWidget *mw = nullptr);
  ~ZObjEditWidget();

  bool showObjEditWidgetOfObj(size_t id);
  void updateEditWidgetTitleOfObj(size_t id);

private:
  void writeLogData(const LogData &message);

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
  LogSinkPtr m_logOutputDestination;
};

} // namespace nim

#endif // ZOBJEDITWIDGET_H
