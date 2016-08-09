//
// Created by Linqing Feng on 8/5/16.
//

#pragma once

#include <QApplication>

namespace nim {

class ZApplication : public QApplication
{
public:
  using QApplication::QApplication;

  virtual bool notify(QObject* object, QEvent* event) override;

};

} // namespace nim

