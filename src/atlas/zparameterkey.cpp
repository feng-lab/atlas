#include "zparameterkey.h"

#include "zparameterfactory.h"
#include "zparameter.h"
#include "zparameteranimation.h"
#include "zoptionparameter.h"

namespace {

qreal myEasingFunction(qreal progress)
{
  return progress >= 1. ? 1.0 : 0.0;
}

}

namespace nim {

ZParameterKey::ZParameterKey(double tm, const ZParameter& p)
  : m_time(std::max(tm, 0.0))
{
  m_value.reset(ZParameterFactory::instance().create(p.name(), p.type()));
  m_value->forceSetValueSameAs(p);
  setDefaultType();
}

ZParameterKey::ZParameterKey(double tm, ZParameter* p)
  : m_time(std::max(tm, 0.0))
{
  CHECK(p);
  m_value.reset(p);
  setDefaultType();
}

ZParameterKey::ZParameterKey(const QString& type)
  : m_time(0)
{
  m_value.reset(ZParameterFactory::instance().create("", type));
  CHECK(m_value);
  setDefaultType();
}

ZParameterKey::ZParameterKey(const ZParameterKey& key)
  : m_time(std::max(0., key.time()))
{
  m_value.reset(ZParameterFactory::instance().create(key.value().name(), key.value().type()));
  m_value->setSameAs(key.value());
  setDefaultType();
  setType(key.type());
}

void ZParameterKey::setTime(double t)
{
  if (t < 0)
    t = 0;
  if (m_time == t)
    return;
  m_time = t;
  if (m_paraAnimation)
    m_paraAnimation->emitKeyChangedSignal(this);
}

void ZParameterKey::setValue(const ZParameter& v)
{
  if (m_value->type().endsWith("Option"))
    m_value->forceSetValueSameAs(v);
  else
    m_value->setValueSameAs(v);
  if (m_paraAnimation)
    m_paraAnimation->emitKeyChangedSignal(this);
}

QString ZParameterKey::type() const
{
  return m_type->get();
}

void ZParameterKey::setType(const QString& t)
{
  if (m_type->isSelected(t))
    return;

  m_type->select(t);
  updateEasingCurve();
  if (m_paraAnimation)
    m_paraAnimation->emitKeyChangedSignal(this);
}

void ZParameterKey::updateEasingCurve()
{
  if (m_type->isSelected("Switch")) {
    m_curve.setCustomType(myEasingFunction);
  } else {
    m_curve.setType(static_cast<QEasingCurve::Type>(m_type->associatedData()));
  }
}

double ZParameterKey::timeToProgress(const ZParameterKey& prev, double time)
{
  return m_curve.valueForProgress((time - prev.time()) / (m_time - prev.time()));
}

void ZParameterKey::interpolate(const ZParameterKey& prev, double time, ZParameter& dest) const
{
  CHECK(time >= prev.time() && time <= m_time);
  CHECK(m_value->isSameType(prev.value()));
  CHECK(m_value->isSameType(dest));

  if (m_time - prev.time() < 0.0001) {  // less than 0.0001s, pointless, just use prev value
    dest.setValueSameAs(prev.value());
    return;
  }
  double progress = m_curve.valueForProgress((time - prev.time()) / (m_time - prev.time()));
  if (progress == 0.0) {
    dest.setValueSameAs(prev.value());
    return;
  }

  m_value->interpolate(prev.value(), progress, dest);
}

bool ZParameterKey::readValue(const QJsonValue& value)
{
  if (!value.isObject()) {
    LOG(WARNING) << "Invalid key";
    return false;
  }
  QJsonObject obj = value.toObject();
  if (!obj.contains("time") ||
      !obj.contains("type") ||
      !obj.contains("value")) {
    LOG(WARNING) << "Invalid key " << obj.keys().join("  ") << " time, type and value are required field.";
    return false;
  }
  m_time = obj.value("time").toDouble();
  m_type->select(obj.value("type").toString());
  updateEasingCurve();
  m_value->readValue(obj.value("value"));
  return true;
}

QJsonValue ZParameterKey::jsonValue() const
{
  QJsonObject obj;
  obj["time"] = m_time;
  obj["type"] = m_type->get();
  obj["value"] = m_value->jsonValue();
  return obj;
}

QString ZParameterKey::info() const
{
  QJsonDocument doc(jsonValue().toObject());
  return doc.toJson();
}

void ZParameterKey::setDefaultType()
{
  m_type = new ZStringIntOptionParameter("Type");
  if (!m_value->supportInterpolation()) {
    m_type->addOptionWithData(qMakePair<QString, int>("Switch", QEasingCurve::Custom));
    setType("Switch");
  } else {
    enum Type
    { // how to interpolate between this key and previous key
      Linear = 0,
      InQuad, OutQuad, InOutQuad, OutInQuad,
      InCubic, OutCubic, InOutCubic, OutInCubic,
      InQuart, OutQuart, InOutQuart, OutInQuart,
      InQuint, OutQuint, InOutQuint, OutInQuint,
      InSine, OutSine, InOutSine, OutInSine,
      InExpo, OutExpo, InOutExpo, OutInExpo,
      InCirc, OutCirc, InOutCirc, OutInCirc,
      InElastic, OutElastic, InOutElastic, OutInElastic,
      InBack, OutBack, InOutBack, OutInBack,
      InBounce, OutBounce, InOutBounce, OutInBounce,
      InCurve, OutCurve, SineCurve, CosineCurve,
      Switch
    };
    m_type->addOptionWithData(qMakePair<QString, int>("Linear", QEasingCurve::Linear));
    m_type->addOptionWithData(qMakePair<QString, int>("Switch", QEasingCurve::Custom));
    m_type->addOptionWithData(qMakePair<QString, int>("InQuad", QEasingCurve::InQuad));
    m_type->addOptionWithData(qMakePair<QString, int>("OutQuad", QEasingCurve::OutQuad));
    m_type->addOptionWithData(qMakePair<QString, int>("InOutQuad", QEasingCurve::InOutQuad));
    m_type->addOptionWithData(qMakePair<QString, int>("OutInQuad", QEasingCurve::OutInQuad));
    m_type->addOptionWithData(qMakePair<QString, int>("InCubic", QEasingCurve::InCubic));
    m_type->addOptionWithData(qMakePair<QString, int>("OutCubic", QEasingCurve::OutCubic));
    m_type->addOptionWithData(qMakePair<QString, int>("InOutCubic", QEasingCurve::InOutCubic));
    m_type->addOptionWithData(qMakePair<QString, int>("OutInCubic", QEasingCurve::OutInCubic));
    m_type->addOptionWithData(qMakePair<QString, int>("InElastic", QEasingCurve::InElastic));
    m_type->addOptionWithData(qMakePair<QString, int>("OutElastic", QEasingCurve::OutElastic));
    m_type->addOptionWithData(qMakePair<QString, int>("InOutElastic", QEasingCurve::InOutElastic));
    m_type->addOptionWithData(qMakePair<QString, int>("OutInElastic", QEasingCurve::OutInElastic));
    m_type->addOptionWithData(qMakePair<QString, int>("InBounce", QEasingCurve::InBounce));
    m_type->addOptionWithData(qMakePair<QString, int>("OutBounce", QEasingCurve::OutBounce));
    m_type->addOptionWithData(qMakePair<QString, int>("InOutBounce", QEasingCurve::InOutBounce));
    m_type->addOptionWithData(qMakePair<QString, int>("OutInBounce", QEasingCurve::OutInBounce));
    m_type->addOptionWithData(qMakePair<QString, int>("InBack", QEasingCurve::InBack));
    m_type->addOptionWithData(qMakePair<QString, int>("OutBack", QEasingCurve::OutBack));
    m_type->addOptionWithData(qMakePair<QString, int>("InOutBack", QEasingCurve::InOutBack));
    m_type->addOptionWithData(qMakePair<QString, int>("OutInBack", QEasingCurve::OutInBack));

    m_type->addOptionWithData(qMakePair<QString, int>("InQuart", QEasingCurve::InQuart));
    m_type->addOptionWithData(qMakePair<QString, int>("OutQuart", QEasingCurve::OutQuart));
    m_type->addOptionWithData(qMakePair<QString, int>("InOutQuart", QEasingCurve::InOutQuart));
    m_type->addOptionWithData(qMakePair<QString, int>("OutInQuart", QEasingCurve::OutInQuart));
    m_type->addOptionWithData(qMakePair<QString, int>("InQuint", QEasingCurve::InQuint));
    m_type->addOptionWithData(qMakePair<QString, int>("OutQuint", QEasingCurve::OutQuint));
    m_type->addOptionWithData(qMakePair<QString, int>("InOutQuint", QEasingCurve::InOutQuint));
    m_type->addOptionWithData(qMakePair<QString, int>("OutInQuint", QEasingCurve::OutInQuint));
    m_type->addOptionWithData(qMakePair<QString, int>("InSine", QEasingCurve::InSine));
    m_type->addOptionWithData(qMakePair<QString, int>("OutSine", QEasingCurve::OutSine));
    m_type->addOptionWithData(qMakePair<QString, int>("InOutSine", QEasingCurve::InOutSine));
    m_type->addOptionWithData(qMakePair<QString, int>("OutInSine", QEasingCurve::OutInSine));
    m_type->addOptionWithData(qMakePair<QString, int>("InExpo", QEasingCurve::InExpo));
    m_type->addOptionWithData(qMakePair<QString, int>("OutExpo", QEasingCurve::OutExpo));
    m_type->addOptionWithData(qMakePair<QString, int>("InOutExpo", QEasingCurve::InOutExpo));
    m_type->addOptionWithData(qMakePair<QString, int>("OutInExpo", QEasingCurve::OutInExpo));
    setType("Linear");
  }
}


} // namespace nim
