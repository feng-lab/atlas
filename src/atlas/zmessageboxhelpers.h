#pragma once

#include <QString>

QT_FORWARD_DECLARE_CLASS(QWidget);

namespace nim {

void showCriticalWithDetails(QWidget* parent,
                             const QString& summary,
                             const QString& details,
                             const QString& windowTitle = QString());

} // namespace nim
