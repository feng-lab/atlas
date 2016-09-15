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

  virtual bool notify(QObject* object, QEvent* event) override;

  virtual bool event(QEvent *event) override;

signals:
  void fileOpenRequest(QList<QUrl> urlList);
};

} // namespace nim

