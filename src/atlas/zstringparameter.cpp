#include "zstringparameter.h"
#include <QtGui>
#include <QtWidgets>

namespace nim {

ZStringParameter::ZStringParameter(const QString &name, QObject *parent)
  : ZSingleValueParameter<QString>(name, parent)
{
}

ZStringParameter::ZStringParameter(const QString &name, const QString &str, QObject *parent)
  : ZSingleValueParameter<QString>(name, str, parent)
{
}

void ZStringParameter::setContent(QString str)
{
  set(str);
}

QWidget *ZStringParameter::actualCreateWidget(QWidget *parent)
{
  QLineEdit* le = new QLineEdit(parent);
  le->setText(m_value);
  connect(le, &QLineEdit::textChanged, this, &ZStringParameter::setContent);
  connect(this, &ZStringParameter::stringChanged, le, &QLineEdit::setText);
  return le;
}

void ZStringParameter::afterChange(QString &)
{
  emit stringChanged(m_value);
}

void ZStringParameter::setSameAs(const ZParameter &rhs)
{
  assert(this->isSameType(rhs));
  const ZStringParameter* src = static_cast<const ZStringParameter*>(&rhs);
  this->set(src->get());
  ZParameter::setSameAs(rhs);
}

QJsonValue ZStringParameter::jsonValue() const
{
  return QJsonValue(this->m_value);
}

void ZStringParameter::readValue(const QJsonValue &jsonValue)
{
  this->set(jsonValue.toString(this->m_value));
}

} // namespace nim
