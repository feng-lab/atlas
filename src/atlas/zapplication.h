#pragma once

#include <QApplication>
#include <QList>
#include <QUrl>
#include <QDir>

namespace nim {

class ZApplication : public QApplication
{
  Q_OBJECT

public:
  using QApplication::QApplication;

  bool notify(QObject* object, QEvent* event) override;

  bool event(QEvent* event) override;

  static QDir resourcesDir();

  static QString resourcesDirPath();

  static QString jdkDirPath();

  static QString jarsDirPath();

  static QString applicationInstallDirPath();

Q_SIGNALS:
  void fileOpenRequest(QList<QUrl> urlList);
};

} // namespace nim
