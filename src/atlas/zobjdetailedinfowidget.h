#pragma once

#include "zdoc.h"
#include <QWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QTextEdit>

namespace nim {

class ZObjDetailedInfoWidget : public QWidget
{
Q_OBJECT
public:
  explicit ZObjDetailedInfoWidget(ZDoc& doc, QWidget* mw = nullptr);

public:
  void showDefaultWidget();

  void showWidgetOfObj(size_t id);

  void hideWidget();

  void setDefaultWidget(QWidget* widget);

private:
  void removeWidgetOfObj(size_t id);

  void updateWidgetLabelOfObj(size_t id);

protected:
  ZDoc& m_doc;
  QStackedWidget* m_widget;
  QWidget* m_defaultWidget;

  struct SubWidget
  {
    SubWidget(size_t id_, QTextEdit* infoLabel_, QLabel* label_, QWidget* wt)
      : id(id_), infoLabel(infoLabel_), label(label_), widget(wt)
    {}

    size_t id;
    QTextEdit* infoLabel;
    QLabel* label;
    QWidget* widget;
  };

  std::vector<SubWidget> m_subWidgets;
};

} // namespace nim

