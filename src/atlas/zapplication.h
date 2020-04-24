#pragma once

#include <QApplication>
#include <QUrl>
#include <QList>

namespace nim {

class ZApplication : public QApplication
{
Q_OBJECT
public:
  using QApplication::QApplication;

  bool notify(QObject* object, QEvent* event) override;

  bool event(QEvent *event) override;

  static QString resourcesDirPath();

signals:
  void fileOpenRequest(QList<QUrl> urlList);
};

} // namespace nim

