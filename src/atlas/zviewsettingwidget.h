#ifndef ZVIEWSETTINGWIDGET_H
#define ZVIEWSETTINGWIDGET_H

#include <QWidget>
#include <QStackedWidget>
#include "zviewsettinginterface.h"
#include "zdoc.h"

namespace nim {

class ZViewSettingWidget : public QWidget
{
  Q_OBJECT
public:
  explicit ZViewSettingWidget(ZDoc *doc, ZViewSettingInterface *view, QWidget *mw = nullptr);

signals:

public slots:
  void showDefaultWidget();
  void showViewSettingWidgetOfObj(size_t id);
  void hideViewSettingWidget();
  void setDefaultWidget(QWidget *widget);

private slots:
  void removeViewSettingWidgetOfObj(size_t id);
  void updateViewSettingWidgetLabelOfObj(size_t id);
  void updateWidget();

protected:

protected:
  ZDoc *m_doc;
  ZViewSettingInterface *m_view;
  QStackedWidget *m_widget;
  QWidget *m_defaultWidget;

  struct SubWidget {
    SubWidget(size_t id, ZWidgetsGroup *wg, QWidget *wt)
      : id(id), widgetsGroup(wg), widget(wt)
    {}

    size_t id;
    ZWidgetsGroup* widgetsGroup;
    QWidget* widget;
  };

  std::vector<SubWidget> m_subWidgets;
};

} // namespace nim

#endif // ZVIEWSETTINGWIDGET_H
