#ifndef ZFONTPARAMETER_H
#define ZFONTPARAMETER_H

#include <QObject>
#include <QFont>
#include "zparameter.h"

namespace nim {

class ZFontParameter : public ZSingleValueParameter<QFont>
{
Q_OBJECT
public:
  ZFontParameter(const QString& name, QObject* parent = nullptr);

  ZFontParameter(const QString& name, const QFont& font, QObject* parent = nullptr);

  // ZParameter interface
public:
  virtual void setSameAs(const ZParameter& rhs) override;

  virtual bool supportInterpolation() const override
  { return false; }

  virtual QJsonValue jsonValue() const override;

  virtual void readValue(const QJsonValue& jsonValue) override;

signals:

  void valueWillChange(QFont);

protected:
  void setValue(const QFont& v);

  virtual void beforeChange(QFont& value) override;

  virtual QWidget* actualCreateWidget(QWidget* parent) override;
};

} // namespace nim

#endif // ZFONTPARAMETER_H
