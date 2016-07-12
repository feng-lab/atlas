#ifndef ZOBJEDITWIDGET_H
#define ZOBJEDITWIDGET_H

#include <QStackedWidget>
#include <QPlainTextEdit>
#include <QTabWidget>
#include "zdoc.h"

#include "QsLogDest.h"

namespace QsLogging
{
class TextEditDestination : public Destination
{
public:
  explicit TextEditDestination(QPlainTextEdit& edit);

  virtual void write(const LogMessage& message) override;
  virtual bool isValid() override { return true; }

private:
  QPlainTextEdit& m_edit;
  QTextCharFormat m_normalFormat;
  QTextCharFormat m_errorFormat;
};
}

namespace nim {

class ZObjEditWidget : public QTabWidget
{
  Q_OBJECT
public:
  explicit ZObjEditWidget(ZDoc *doc, QWidget *mw = nullptr);
  ~ZObjEditWidget();

  bool showObjEditWidgetOfObj(size_t id);
  void updateEditWidgetTitleOfObj(size_t id);

private:
  void writeLogMessage(const QsLogging::LogMessage &message);
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
  QsLogging::DestinationPtr m_logOutputDestination;
};

} // namespace nim

#endif // ZOBJEDITWIDGET_H
