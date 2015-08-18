#ifndef ZQUATPARAMETER_H
#define ZQUATPARAMETER_H

#include "znumericparameter.h"

namespace nim {

class ZQuatParameter : public ZNumericVectorParameter<glm::quat>
{
  Q_OBJECT
public:
  ZQuatParameter(const QString &name, QObject *parent = NULL);
  ZQuatParameter(const QString &name, glm::quat value, QObject *parent = NULL);

signals:
  void value1Changed(double);
  void value2Changed(double);
  void value3Changed(double);
  void value4Changed(double);
public slots:
  void setValue1(double v);
  void setValue2(double v);
  void setValue3(double v);
  void setValue4(double v);

protected:
  virtual void beforeChange(glm::quat &value) override;
  virtual QWidget* actualCreateWidget(QWidget *parent) override;
};


} // namespace nim

#endif // ZQUATPARAMETER_H
