#pragma once

#include <QApplication>
#include <QList>
#include <QUrl>

namespace nim {

class ZApplication : public QApplication
{
  Q_OBJECT

public:
  using QApplication::QApplication;

  bool notify(QObject* object, QEvent* event) override;

  bool event(QEvent* event) override;

Q_SIGNALS:
  void fileOpenRequest(QList<QUrl> urlList);
};

} // namespace nim
