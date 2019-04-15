#pragma once

#include "zparameter.h"

class QLineEdit;

namespace nim {

class ZStringParameter : public ZSingleValueParameter<QString>
{
Q_OBJECT
public:
  explicit ZStringParameter(const QString& name, QObject* parent = nullptr);

  explicit ZStringParameter(const QString& name, const QString& str, QObject* parent = nullptr);

  // ZParameter interface
public:
  void setSameAs(const ZParameter& rhs) override;

  bool supportInterpolation() const override
  { return false; }

  QJsonValue jsonValue() const override;

  void readValue(const QJsonValue& jsonValue) override;

signals:

  void stringChanged(QString str);

protected:
  void setContent(const QString& str);

  QWidget* actualCreateWidget(QWidget* parent) override;

  void afterChange(QString& value) override;
};

} // namespace nim

