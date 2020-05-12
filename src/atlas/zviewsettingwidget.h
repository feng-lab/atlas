#pragma once

#include "zdoc.h"
#include "zviewsettinginterface.h"
#include <QWidget>
#include <QStackedWidget>
#include <QLabel>

namespace nim {

class ZViewSettingWidget : public QWidget
{
Q_OBJECT
public:
  explicit ZViewSettingWidget(ZDoc& doc, ZViewSettingInterface* view, QWidget* mw = nullptr);

  void showDefaultWidget();

  void showViewSettingWidgetOfObj(size_t id);

  void hideViewSettingWidget();

  void setDefaultWidget(QWidget* widget);

private:
  void removeViewSettingWidgetOfObj(size_t id);

  void updateViewSettingWidgetLabelOfObj(size_t id);

  void updateWidget();

protected:
  ZDoc& m_doc;
  ZViewSettingInterface* m_view;
  QStackedWidget* m_widget;
  QWidget* m_defaultWidget;

  struct SubWidget
  {
    SubWidget(size_t id_, ZWidgetsGroup* wg, QLabel* label_, QWidget* wt)
      : id(id_), widgetsGroup(wg), label(label_), widget(wt)
    {}

    size_t id;
    ZWidgetsGroup* widgetsGroup;
    QLabel* label;
    QWidget* widget;
  };

  std::vector<SubWidget> m_subWidgets;
};

} // namespace nim

