#pragma once

#include "zdoc.h"
#include "zlogwidget.h"
#include <QStackedWidget>
#include <QTabWidget>

namespace nim {

struct LogData;

class ZObjEditWidget : public QTabWidget
{
  Q_OBJECT

public:
  explicit ZObjEditWidget(ZDoc& doc, QWidget* mw = nullptr);

  bool showObjEditWidgetOfObj(size_t id);

  void updateEditWidgetTitleOfObj(size_t id);

private:
  void removeObjEditWidgetOfObj(size_t id);

  void closeTab(int index);

protected:
  ZDoc& m_doc;

  struct SubWidget
  {
    size_t id;
    QWidget* widget;
  };

  ZLogWidget* m_logWidget = nullptr;
  std::vector<SubWidget> m_subWidgets;
};

} // namespace nim
