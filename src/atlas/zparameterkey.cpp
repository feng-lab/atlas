#include "zparameterkey.h"

#include "zparameterfactory.h"
#include "zparameter.h"
#include "zoptionparameter.h"

namespace {

qreal myEasingFunction(qreal progress)
{
  return progress >= 1. ? 1.0 : 0.0;
}

} // namespace

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

ZParameterKey::~ZParameterKey() = default;

void ZParameterKey::setValue(const ZParameter& v)
{
  if (m_value->type().endsWith("Option")) {
    m_value->forceSetValueSameAs(v);
  } else {
    m_value->setValueSameAs(v);
  }
}

QString ZParameterKey::type() const
{
  return m_type->get();
}

void ZParameterKey::setType(const QString& t)
{
  if (m_type->isSelected(t)) {
    return;
  }

  m_type->select(t);
  updateEasingCurve();
}

void ZParameterKey::updateEasingCurve()
{
  if (m_type->isSelected("Switch")) {
    m_curve.setCustomType(myEasingFunction);
  } else {
    m_curve.setType(static_cast<QEasingCurve::Type>(m_type->associatedData()));
  }
}

double ZParameterKey::timeToProgress(const ZParameterKey& prev, double time) const
{
  return m_curve.valueForProgress((time - prev.time()) / (m_time - prev.time()));
}

void ZParameterKey::interpolate(const ZParameterKey& prev, double time, ZParameter& dest) const
{
  CHECK(time >= prev.time() && time <= m_time);
  CHECK(m_value->isSameType(prev.value()));
  CHECK(m_value->isSameType(dest));

  if (m_time - prev.time() < 0.0001) { // less than 0.0001s, pointless, just use prev value
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

bool ZParameterKey::readValue(const json::value& value)
{
  if (!value.is_object()) {
    LOG(WARNING) << "Invalid key";
    return false;
  }
  const auto& obj = value.as_object();
  if (!obj.contains("time") || !obj.contains("type") || !obj.contains("value")) {
    LOG(WARNING) << "Invalid key " << jsonToFormattedQString(obj) << " time, type and value are required field.";
    return false;
  }
  m_time = json::value_to<double>(obj.at("time"));
  m_type->select(json::value_to<QString>(obj.at("type")));
  updateEasingCurve();
  m_value->readValue(obj.at("value"));
  return true;
}

json::value ZParameterKey::jsonValue() const
{
  json::object obj;
  obj["time"] = m_time;
  obj["type"] = json::value_from(m_type->get());
  obj["value"] = m_value->jsonValue();
  return obj;
}

QString ZParameterKey::info() const
{
  return jsonToFormattedQString(jsonValue());
}

void ZParameterKey::setDefaultType()
{
  m_type = std::make_unique<ZStringIntOptionParameter>("Type");
  if (!m_value->supportInterpolation()) {
    m_type->addOptionWithData(std::make_pair<QString, int>("Switch", QEasingCurve::Custom));
    setType("Switch");
  } else {
    enum Type
    { // how to interpolate between this key and previous key
      Linear = 0,
      InQuad,
      OutQuad,
      InOutQuad,
      OutInQuad,
      InCubic,
      OutCubic,
      InOutCubic,
      OutInCubic,
      InQuart,
      OutQuart,
      InOutQuart,
      OutInQuart,
      InQuint,
      OutQuint,
      InOutQuint,
      OutInQuint,
      InSine,
      OutSine,
      InOutSine,
      OutInSine,
      InExpo,
      OutExpo,
      InOutExpo,
      OutInExpo,
      InCirc,
      OutCirc,
      InOutCirc,
      OutInCirc,
      InElastic,
      OutElastic,
      InOutElastic,
      OutInElastic,
      InBack,
      OutBack,
      InOutBack,
      OutInBack,
      InBounce,
      OutBounce,
      InOutBounce,
      OutInBounce,
      InCurve,
      OutCurve,
      SineCurve,
      CosineCurve,
      Switch
    };
    m_type->addOptionWithData(std::make_pair<QString, int>("Linear", QEasingCurve::Linear));
    m_type->addOptionWithData(std::make_pair<QString, int>("Switch", QEasingCurve::Custom));
    m_type->addOptionWithData(std::make_pair<QString, int>("InQuad", QEasingCurve::InQuad));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutQuad", QEasingCurve::OutQuad));
    m_type->addOptionWithData(std::make_pair<QString, int>("InOutQuad", QEasingCurve::InOutQuad));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutInQuad", QEasingCurve::OutInQuad));
    m_type->addOptionWithData(std::make_pair<QString, int>("InCubic", QEasingCurve::InCubic));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutCubic", QEasingCurve::OutCubic));
    m_type->addOptionWithData(std::make_pair<QString, int>("InOutCubic", QEasingCurve::InOutCubic));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutInCubic", QEasingCurve::OutInCubic));
    m_type->addOptionWithData(std::make_pair<QString, int>("InElastic", QEasingCurve::InElastic));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutElastic", QEasingCurve::OutElastic));
    m_type->addOptionWithData(std::make_pair<QString, int>("InOutElastic", QEasingCurve::InOutElastic));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutInElastic", QEasingCurve::OutInElastic));
    m_type->addOptionWithData(std::make_pair<QString, int>("InBounce", QEasingCurve::InBounce));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutBounce", QEasingCurve::OutBounce));
    m_type->addOptionWithData(std::make_pair<QString, int>("InOutBounce", QEasingCurve::InOutBounce));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutInBounce", QEasingCurve::OutInBounce));
    m_type->addOptionWithData(std::make_pair<QString, int>("InBack", QEasingCurve::InBack));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutBack", QEasingCurve::OutBack));
    m_type->addOptionWithData(std::make_pair<QString, int>("InOutBack", QEasingCurve::InOutBack));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutInBack", QEasingCurve::OutInBack));

    m_type->addOptionWithData(std::make_pair<QString, int>("InQuart", QEasingCurve::InQuart));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutQuart", QEasingCurve::OutQuart));
    m_type->addOptionWithData(std::make_pair<QString, int>("InOutQuart", QEasingCurve::InOutQuart));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutInQuart", QEasingCurve::OutInQuart));
    m_type->addOptionWithData(std::make_pair<QString, int>("InQuint", QEasingCurve::InQuint));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutQuint", QEasingCurve::OutQuint));
    m_type->addOptionWithData(std::make_pair<QString, int>("InOutQuint", QEasingCurve::InOutQuint));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutInQuint", QEasingCurve::OutInQuint));
    m_type->addOptionWithData(std::make_pair<QString, int>("InSine", QEasingCurve::InSine));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutSine", QEasingCurve::OutSine));
    m_type->addOptionWithData(std::make_pair<QString, int>("InOutSine", QEasingCurve::InOutSine));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutInSine", QEasingCurve::OutInSine));
    m_type->addOptionWithData(std::make_pair<QString, int>("InExpo", QEasingCurve::InExpo));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutExpo", QEasingCurve::OutExpo));
    m_type->addOptionWithData(std::make_pair<QString, int>("InOutExpo", QEasingCurve::InOutExpo));
    m_type->addOptionWithData(std::make_pair<QString, int>("OutInExpo", QEasingCurve::OutInExpo));
    setType("Linear");
  }
}

} // namespace nim
