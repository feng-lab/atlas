#include "zparameter.h"

#include "zcheckbox.h"
#include "zlog.h"
#include <QWidget>
#include <QLabel>
#include <utility>

namespace nim {

ZParameter::ZParameter(QString name, QObject* parent)
  : QObject(parent)
  , m_name(std::move(name))
{
  addStyle("DEFAULT");
}

void ZParameter::setName(const QString& name)
{
  if (name != m_name) {
    m_name = name;
    Q_EMIT nameChanged(m_name);
  }
}

QString ZParameter::type() const
{
  QString className = metaObject()->className();
  if (className.startsWith("nim::")) {
    className.remove(0, 6);
  } else {
    className.remove(0, 1);
  }
  return className.left(className.length() - 9);
}

void ZParameter::setStyle(const QString& style)
{
  if (m_allStyles.contains(style)) {
    m_style = style;
  } else {
    m_style = "DEFAULT";
  }
}

QLabel* ZParameter::createNameLabel(QWidget* parent)
{
  auto label = new QLabel(m_name, parent);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  if (!m_isWidgetsVisible) {
    label->setVisible(m_isWidgetsVisible);
  }
  if (!m_isWidgetsEnabled) {
    label->setEnabled(m_isWidgetsEnabled);
  }
  connect(this, &ZParameter::setWidgetsEnabled, label, &QLabel::setEnabled);
  connect(this, &ZParameter::setWidgetsVisible, label, &QLabel::setVisible);
  connect(this, &ZParameter::nameChanged, label, &QLabel::setText);

#ifdef __APPLE__
  // QFont fnt = label->font();
  // fnt.setPointSize(11);
  // label->setFont(fnt);
#endif
  return label;
}

QWidget* ZParameter::createWidget(QWidget* parent)
{
  QWidget* widget = actualCreateWidget(parent);
  if (!m_isWidgetsVisible) {
    widget->setVisible(m_isWidgetsVisible);
  }
  if (!m_isWidgetsEnabled) {
    widget->setEnabled(m_isWidgetsEnabled);
  }
  connect(this, &ZParameter::setWidgetsEnabled, widget, &QWidget::setEnabled);
  connect(this, &ZParameter::setWidgetsVisible, widget, &QWidget::setVisible);
#ifdef __APPLE__
  widget->setAttribute(Qt::WA_LayoutUsesWidgetRect);

  // QFont fnt = widget->font();
  // fnt.setPointSize(11);
  // widget->setFont(fnt);
#endif
  return widget;
}

QString ZParameter::jsonKey() const
{
  return m_name + QString(" ") + type();
}

void ZParameter::read(const json::object& json)
{
  if (json.contains(jsonKey().toStdString())) {
    readValue(json.at(jsonKey().toStdString()));
  } else {
    LOG(WARNING) << "Parameter <" << jsonKey() << "> not found.";
  }
}

void ZParameter::write(json::object& json) const
{
  json[jsonKey().toStdString()] = jsonValue();
}

void ZParameter::setSameAs(const ZParameter& rhs)
{
  m_allStyles = rhs.m_allStyles;
  setName(rhs.m_name);
  m_description = rhs.m_description;
  setStyle(rhs.m_style);
  setEnabled(rhs.m_isWidgetsEnabled);
  setVisible(rhs.m_isWidgetsVisible);
}

void ZParameter::setVisible(bool s)
{
  if (s != m_isWidgetsVisible) {
    m_isWidgetsVisible = s;
    Q_EMIT setWidgetsVisible(m_isWidgetsVisible);
  }
}

void ZParameter::setEnabled(bool s)
{
  if (s != m_isWidgetsEnabled) {
    m_isWidgetsEnabled = s;
    Q_EMIT setWidgetsEnabled(m_isWidgetsEnabled);
  }
}

void ZParameter::updateFromSender()
{
  auto para = dynamic_cast<ZParameter*>(sender());
  CHECK(para);
  if (isSameType(*para)) {
    setValueSameAs(*para);
  } else {
    LOG(ERROR) << "can not update parameter " << name() << " with type " << type() << " from different type parameter "
               << para->name() << " type " << para->type();
  }
}

ZBoolParameter::ZBoolParameter(const QString& name, QObject* parent)
  : ZSingleValueParameter<bool>(name, parent)
{}

ZBoolParameter::ZBoolParameter(const QString& name, bool value, QObject* parent)
  : ZSingleValueParameter<bool>(name, value, parent)
{}

void ZBoolParameter::setValue(bool v)
{
  set(v);
}

void ZBoolParameter::beforeChange(bool& value)
{
  Q_EMIT valueWillChange(value);
}

void ZBoolParameter::afterChange(bool&)
{
  Q_EMIT boolChanged(m_value);
}

QWidget* ZBoolParameter::actualCreateWidget(QWidget* parent)
{
  auto cb = new ZCheckBox(parent);
  cb->setChecked(m_value);
  connect(cb, &ZCheckBox::toggled, this, &ZBoolParameter::setValue);
  connect(this, &ZBoolParameter::valueWillChange, cb, &ZCheckBox::setCheckedBlockSignals);
  return cb;
}

void ZBoolParameter::setSameAs(const ZParameter& rhs)
{
  CHECK(this->isSameType(rhs));
  set(dynamic_cast<const ZBoolParameter*>(&rhs)->get());
  ZSingleValueParameter<bool>::setSameAs(rhs);
}

json::value ZBoolParameter::jsonValue() const
{
  return this->m_value;
}

void ZBoolParameter::readValue(const json::value& jsonValue)
{
  set(jsonValue.as_bool());
}

} // namespace nim
