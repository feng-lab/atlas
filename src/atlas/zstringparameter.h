#ifndef ZSTRINGPARAMETER_H
#define ZSTRINGPARAMETER_H

#include "zparameter.h"

class QLineEdit;

namespace nim {

class ZStringParameter : public ZSingleValueParameter<QString>
{
  Q_OBJECT
public:
  explicit ZStringParameter(const QString &name, QObject *parent = NULL);
  explicit ZStringParameter(const QString &name, const QString &str, QObject *parent = NULL);

signals:
  void stringChanged(QString str);
  
public slots:
  void setContent(QString str);

protected:
  virtual QWidget* actualCreateWidget(QWidget *parent) override;
  virtual void afterChange(QString &value) override;

  // ZParameter interface
public:
  virtual void setSameAs(const ZParameter &rhs) override;
  virtual bool supportInterpolation() const override { return false; }
  virtual QJsonValue jsonValue() const override;
  virtual void readValue(const QJsonValue &jsonValue) override;
};

} // namespace nim

#endif // ZSTRINGPARAMETER_H
