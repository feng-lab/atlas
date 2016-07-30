#ifndef ZSTRINGPARAMETER_H
#define ZSTRINGPARAMETER_H

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
  virtual void setSameAs(const ZParameter& rhs) override;

  virtual bool supportInterpolation() const override
  { return false; }

  virtual QJsonValue jsonValue() const override;

  virtual void readValue(const QJsonValue& jsonValue) override;

signals:

  void stringChanged(QString str);

protected:
  void setContent(QString str);

  virtual QWidget* actualCreateWidget(QWidget* parent) override;

  virtual void afterChange(QString& value) override;
};

} // namespace nim

#endif // ZSTRINGPARAMETER_H
