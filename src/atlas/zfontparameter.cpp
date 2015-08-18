#include "zfontparameter.h"

#include "QsLog.h"
#include "zfontwidget.h"

namespace nim {

ZFontParameter::ZFontParameter(const QString &name, QObject *parent)
  : ZSingleValueParameter<QFont>(name, parent)
{
}

ZFontParameter::ZFontParameter(const QString &name, const QFont &font, QObject *parent)
  : ZSingleValueParameter<QFont>(name, font, parent)
{
}

void ZFontParameter::setValue(const QFont &v)
{
  set(v);
}

void ZFontParameter::beforeChange(QFont &value)
{
  emit valueChanged(value);
}

QWidget *ZFontParameter::actualCreateWidget(QWidget *parent)
{
  ZFontWidget *res = new ZFontWidget(m_value, parent);
  connect(res, SIGNAL(fontChanged(QFont)), this, SLOT(setValue(QFont)));
  connect(this, SIGNAL(valueChanged(QFont)), res, SLOT(setFont(QFont)));
  return res;
}

void ZFontParameter::setSameAs(const ZParameter &rhs)
{
  assert(this->isSameType(rhs));
  set(static_cast<const ZFontParameter*>(&rhs)->get());
  ZSingleValueParameter<QFont>::setSameAs(rhs);
}

QJsonValue ZFontParameter::jsonValue() const
{
  return QJsonValue(this->m_value.toString());
}

void ZFontParameter::readValue(const QJsonValue &jsonValue)
{
  QString text = jsonValue.toString(this->m_value.toString());
  QFont font;
  if (!font.fromString(text)) {
    LWARN() << "Can not load font" << text;
  } else {
    set(font);
  }
}

} // namespace nim
