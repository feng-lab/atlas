#pragma once

#include "zjson.h"
#include <QEasingCurve>
#include <memory>

namespace nim {

class ZParameter;

class ZStringIntOptionParameter;

class ZParameterAnimation;

class ZParameterKey
{
public:
  ZParameterKey(double tm, const ZParameter& p);

  ZParameterKey(double tm, ZParameter* p);

  explicit ZParameterKey(const QString& type);

  ZParameterKey(const ZParameterKey& key);

  virtual ~ZParameterKey();

  double time() const
  { return m_time; }

  void setTime(double t)
  { m_time = t; }

  ZParameter& value() const
  { return *m_value; }

  void setValue(const ZParameter& v);

  QString type() const;

  void setType(const QString& t);

  void updateEasingCurve();

  const ZStringIntOptionParameter& typePara() const
  { return *m_type; }

  // convert time to progress between prev key and current key
  double timeToProgress(const ZParameterKey& prev, double time) const;

  // interpolate value of time and put result to dest
  virtual void interpolate(const ZParameterKey& prev, double time, ZParameter& dest) const;

  virtual bool readValue(const QJsonValue& value);

  virtual QJsonValue jsonValue() const;

  QString info() const;

protected:
  void setDefaultType();

protected:
  double m_time;
  std::unique_ptr<ZParameter> m_value;
  std::unique_ptr<ZStringIntOptionParameter> m_type;

  QEasingCurve m_curve;
};

} // namespace nim

