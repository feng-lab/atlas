#ifndef ZOBJDETAILEDINFOWIDGET_H
#define ZOBJDETAILEDINFOWIDGET_H

#include <QWidget>
#include <QStackedWidget>
#include <QLabel>
#include "zdoc.h"

namespace nim {

class ZObjDetailedInfoWidget : public QWidget
{
  Q_OBJECT
public:
  explicit ZObjDetailedInfoWidget(ZDoc *doc, QWidget *mw = nullptr);

signals:

public slots:
  void showDefaultWidget();
  void showWidgetOfObj(size_t id);
  void hideWidget();
  void setDefaultWidget(QWidget *widget);

private slots:
  void removeWidgetOfObj(size_t id);
  void updateWidgetLabelOfObj(size_t id);

protected:

protected:
  ZDoc *m_doc;
  QStackedWidget *m_widget;
  QWidget *m_defaultWidget;

  struct SubWidget {
    SubWidget(size_t id, QLabel *label, QWidget *wt)
      : id(id), infoLabel(label), widget(wt)
    {}

    size_t id;
    QLabel* infoLabel;
    QWidget* widget;
  };

  std::vector<SubWidget> m_subWidgets;
};

} // namespace nim

#endif // ZOBJDETAILEDINFOWIDGET_H
