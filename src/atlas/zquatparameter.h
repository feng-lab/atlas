#pragma once

#include "znumericparameter.h"

namespace nim {

class ZQuatParameter : public ZNumericVectorParameter<glm::quat>
{
  Q_OBJECT

public:
  explicit ZQuatParameter(const QString& name, QObject* parent = nullptr);

  ZQuatParameter(const QString& name, glm::quat value, QObject* parent = nullptr);

Q_SIGNALS:
  void value1WillChange(double);

  void value2WillChange(double);

  void value3WillChange(double);

  void value4WillChange(double);

protected:
  void setValue1(double v);

  void setValue2(double v);

  void setValue3(double v);

  void setValue4(double v);

  void beforeChange(glm::quat& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;
};

} // namespace nim
