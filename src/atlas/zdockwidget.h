#pragma once

#include <QDockWidget>

namespace nim {

class ZDockWidget : public QDockWidget
{
Q_OBJECT
public:
  explicit ZDockWidget(const QString& title, QWidget* parent = nullptr, Qt::WindowFlags flags = nullptr);
};

} // namespace

