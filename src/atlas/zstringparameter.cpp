#include "zstringparameter.h"

#include "zlog.h"
#include <QtWidgets>

namespace nim {

ZStringParameter::ZStringParameter(const QString& name, QObject* parent)
  : ZSingleValueParameter<QString>(name, parent)
{}

ZStringParameter::ZStringParameter(const QString& name, const QString& str, QObject* parent)
  : ZSingleValueParameter<QString>(name, str, parent)
{}

void ZStringParameter::setContent(const QString& str)
{
  set(str);
}

QWidget* ZStringParameter::actualCreateWidget(QWidget* parent)
{
  auto le = new QLineEdit(parent);
  le->setText(m_value);
  connect(le, &QLineEdit::textChanged, this, &ZStringParameter::setContent);
  connect(this, &ZStringParameter::stringChanged, le, &QLineEdit::setText);
  return le;
}

void ZStringParameter::afterChange(QString& /*unused*/)
{
  Q_EMIT stringChanged(m_value);
}

void ZStringParameter::setSameAs(const ZParameter& rhs)
{
  CHECK(this->isSameType(rhs));
  const ZStringParameter* src = static_cast<const ZStringParameter*>(&rhs);
  this->set(src->get());
  ZParameter::setSameAs(rhs);
}

json::value ZStringParameter::jsonValue() const
{
  return json::value_from(this->m_value);
}

void ZStringParameter::readValue(const json::value& jsonValue)
{
  this->set(asQString(jsonValue));
}

} // namespace nim
