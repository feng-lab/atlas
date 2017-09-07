#pragma once

#include "zparameter.h"
#include <QObject>
#include <QFont>

namespace nim {

class ZFontParameter : public ZSingleValueParameter<QFont>
{
Q_OBJECT
public:
  explicit ZFontParameter(const QString& name, QObject* parent = nullptr);

  ZFontParameter(const QString& name, const QFont& font, QObject* parent = nullptr);

  // ZParameter interface
public:
  void setSameAs(const ZParameter& rhs) override;

  bool supportInterpolation() const override
  { return false; }

  QJsonValue jsonValue() const override;

  void readValue(const QJsonValue& jsonValue) override;

signals:

  void valueWillChange(QFont);

protected:
  void setValue(const QFont& v);

  void beforeChange(QFont& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;
};

} // namespace nim

