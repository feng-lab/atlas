//
// Created by Linqing Feng on 8/5/16.
//

#ifndef ATLAS_ZAPPLICATION_H
#define ATLAS_ZAPPLICATION_H

#include <QApplication>

namespace nim {

class ZApplication : public QApplication
{
public:
  using QApplication::QApplication;

  virtual bool notify(QObject* object, QEvent* event) override;

};

} // namespace nim

#endif //ATLAS_ZAPPLICATION_H
