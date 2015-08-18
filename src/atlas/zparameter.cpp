#include "zparameter.h"
#include <QWidget>
#include <QLabel>
#include <QCheckBox>
#include "QsLog.h"

namespace nim {

ZParameter::ZParameter(const QString& name, QObject *parent)
  : QObject(parent)
  , m_name(name)
  , m_style("DEFAULT")
  , m_isWidgetsEnabled(true)
  , m_isWidgetsVisible(true)
{
  addStyle("DEFAULT");
}

ZParameter::~ZParameter()
{
}

void ZParameter::setName(const QString &name)
{
  if (name != m_name) {
    m_name = name;
    emit nameChanged(m_name);
  }
}

QString ZParameter::type() const
{
  QString className = metaObject()->className();
  if (className.startsWith("nim::"))
    className.remove(0, 5);
  return className.remove(0, 1).left(className.length()-9);
}

void ZParameter::setStyle(const QString &style)
{
  if (m_allStyles.contains(style))
    m_style = style;
  else
    m_style = "DEFAULT";
}

QLabel *ZParameter::createNameLabel(QWidget *parent)
{
  QLabel *label = new QLabel(m_name, parent);
  if (!m_isWidgetsVisible)
    label->setVisible(m_isWidgetsVisible);
  if (!m_isWidgetsEnabled)
    label->setEnabled(m_isWidgetsEnabled);
  connect(this, SIGNAL(setWidgetsEnabled(bool)), label, SLOT(setEnabled(bool)));
  connect(this, SIGNAL(setWidgetsVisible(bool)), label, SLOT(setVisible(bool)));
  connect(this, SIGNAL(nameChanged(QString)), label, SLOT(setText(QString)));

#ifdef __APPLE__
  //QFont fnt = label->font();
  //fnt.setPointSize(11);
  //label->setFont(fnt);
#endif
  return label;
}

QWidget *ZParameter::createWidget(QWidget *parent)
{
  QWidget* widget = actualCreateWidget(parent);
  if (!m_isWidgetsVisible)
    widget->setVisible(m_isWidgetsVisible);
  if (!m_isWidgetsEnabled)
    widget->setEnabled(m_isWidgetsEnabled);
  connect(this, SIGNAL(setWidgetsEnabled(bool)), widget, SLOT(setEnabled(bool)));
  connect(this, SIGNAL(setWidgetsVisible(bool)), widget, SLOT(setVisible(bool)));
#ifdef __APPLE__
  widget->setAttribute(Qt::WA_LayoutUsesWidgetRect);

  //QFont fnt = widget->font();
  //fnt.setPointSize(11);
  //widget->setFont(fnt);
#endif
  return widget;
}

QString ZParameter::jsonKey() const
{
  return m_name + QString(" ") + type();
}

void ZParameter::read(const QJsonObject &json)
{
  if (json.contains(jsonKey())) {
    readValue(json[jsonKey()]);
  } else {
    LWARN() << "Parameter" << jsonKey() << "not found, abort reading.";
  }
}

void ZParameter::write(QJsonObject &json) const
{
  json.insert(jsonKey(), jsonValue());
}

void ZParameter::setSameAs(const ZParameter &rhs)
{
  m_allStyles = rhs.m_allStyles;
  setName(rhs.m_name);
  setStyle(rhs.m_style);
  setEnabled(rhs.m_isWidgetsEnabled);
  setVisible(rhs.m_isWidgetsVisible);
}

void ZParameter::setVisible(bool s)
{
  if (s != m_isWidgetsVisible) {
    m_isWidgetsVisible = s;
    emit setWidgetsVisible(m_isWidgetsVisible);
  }
}

void ZParameter::setEnabled(bool s)
{
  if (s != m_isWidgetsEnabled) {
    m_isWidgetsEnabled = s;
    emit setWidgetsEnabled(m_isWidgetsEnabled);
  }
}

void ZParameter::updateFromSender()
{
  ZParameter *para = qobject_cast<ZParameter*>(sender());
  if (isSameType(*para)) {
    setValueSameAs(*para);
  } else {
    LERROR() << "can not update parameter" << name() << "with type" << type()
             << "from different type parameter" << para->name() << "type" << para->type();
  }
}

ZBoolParameter::ZBoolParameter(const QString &name, QObject *parent)
  : ZSingleValueParameter<bool>(name, parent)
{
}

ZBoolParameter::ZBoolParameter(const QString &name, bool value, QObject *parent)
  : ZSingleValueParameter<bool>(name, value, parent)
{
}

void ZBoolParameter::setValue(bool v)
{
  set(v);
}

void ZBoolParameter::beforeChange(bool &value)
{
  emit valueChanged(value);
}

QWidget *ZBoolParameter::actualCreateWidget(QWidget *parent)
{
  QCheckBox* cb = new QCheckBox(parent);
  cb->setChecked(m_value);
  connect(cb, SIGNAL(toggled(bool)), this, SLOT(setValue(bool)));
  connect(this, SIGNAL(valueChanged(bool)), cb, SLOT(setChecked(bool)));
  return cb;
}

void ZBoolParameter::setSameAs(const ZParameter &rhs)
{
  assert(this->isSameType(rhs));
  set(static_cast<const ZBoolParameter*>(&rhs)->get());
  ZSingleValueParameter<bool>::setSameAs(rhs);
}

QJsonValue ZBoolParameter::jsonValue() const
{
  return QJsonValue(this->m_value);
}

void ZBoolParameter::readValue(const QJsonValue &jsonValue)
{
  set(jsonValue.toBool(this->m_value));
}

} // namespace nim
