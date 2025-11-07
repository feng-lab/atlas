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
  {
    return false;
  }

  json::value jsonValue() const override;

  void readValue(const json::value& jsonValue) override;

  [[nodiscard]] json::object valueSchema() const override
  {
    json::object o;
    o["type"] = "string"; // serialized via QFont::toString()
    return o;
  }

Q_SIGNALS:
  void valueWillChange(QFont);

protected:
  void setValue(const QFont& v);

  void beforeChange(const QFont& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;
};

} // namespace nim
