#include "zdockwidget.h"

namespace nim {

ZDockWidget::ZDockWidget(const QString &title, QWidget *parent, Qt::WindowFlags flags)
  : QDockWidget(title, parent, flags)
{
}

} // namespace
