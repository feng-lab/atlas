#include "zquatparameter.h"

#include <QGroupBox>
#include <QBoxLayout>
#include "zspinbox.h"
#include <QLabel>
#include "zspinboxwithslider.h"

namespace nim {

ZQuatParameter::ZQuatParameter(const QString &name, QObject *parent)
  : ZNumericVectorParameter<glm::quat>(name, glm::quat(), glm::quat(-std::numeric_limits<float>::max(),
                                                                    -std::numeric_limits<float>::max(),
                                                                    -std::numeric_limits<float>::max(),
                                                                    -std::numeric_limits<float>::max()),
                                       glm::quat(std::numeric_limits<float>::max(),
                                                 std::numeric_limits<float>::max(),
                                                 std::numeric_limits<float>::max(),
                                                 std::numeric_limits<float>::max()), parent)
{
  addStyle("SPINBOX");
}

ZQuatParameter::ZQuatParameter(const QString &name, glm::quat value, QObject *parent)
  : ZNumericVectorParameter<glm::quat>(name, value, glm::quat(-std::numeric_limits<float>::max(),
                                                              -std::numeric_limits<float>::max(),
                                                              -std::numeric_limits<float>::max(),
                                                              -std::numeric_limits<float>::max()),
                                 glm::quat(std::numeric_limits<float>::max(),
                                           std::numeric_limits<float>::max(),
                                           std::numeric_limits<float>::max(),
                                           std::numeric_limits<float>::max()), parent)
{
  addStyle("SPINBOX");
}

void ZQuatParameter::setValue1(double v)
{
  set(glm::quat(m_value.w, static_cast<float>(v), m_value.y, m_value.z));
}

void ZQuatParameter::setValue2(double v)
{
  set(glm::quat(m_value.w, m_value.x, static_cast<float>(v), m_value.z));
}

void ZQuatParameter::setValue3(double v)
{
  set(glm::quat(m_value.w, m_value.x, m_value.y, static_cast<float>(v)));
}

void ZQuatParameter::setValue4(double v)
{
  set(glm::quat(static_cast<float>(v), m_value.x, m_value.y, m_value.z));
}

void ZQuatParameter::beforeChange(glm::quat &value)
{
  if (value[0] != m_value[0])
    emit value1Changed(value[0]);
  if (value[1] != m_value[1])
    emit value2Changed(value[1]);
  if (value[2] != m_value[2])
    emit value3Changed(value[2]);
  if (value[3] != m_value[3])
    emit value4Changed(value[3]);
}

QWidget *ZQuatParameter::actualCreateWidget(QWidget *parent)
{
  QWidget *w;
  if (m_widgetOrientation == Qt::Horizontal)
    w = new QWidget(parent);
  else
    w = new QGroupBox(m_groupBoxName, parent);
  QBoxLayout *lo;
  if (m_widgetOrientation == Qt::Horizontal)
    lo = new QHBoxLayout();
  else
    lo = new QVBoxLayout();

  if (m_style == "SPINBOX") {
    ZDoubleSpinBox *sb1 = new ZDoubleSpinBox();
    sb1->setRange(m_min[0], m_max[0]);
    sb1->setValue(m_value[0]);
    sb1->setSingleStep(m_step);
    sb1->setDecimals(m_decimal);
    sb1->setPrefix(m_prefix);
    sb1->setSuffix(m_suffix);
    connect(sb1, SIGNAL(valueChanged(double)), this, SLOT(setValue1(double)));
    connect(this, SIGNAL(value1Changed(double)), sb1, SLOT(setValue(double)));
    if (m_nameOfEachValue.at(0).isEmpty()) {
      lo->addWidget(sb1);
    } else {
      QLabel *lb = new QLabel(m_nameOfEachValue[0]);
      lb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
      lb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      if (m_widgetOrientation == Qt::Horizontal) {
        lo->addWidget(lb);
        lo->addWidget(sb1);
      } else {
        QHBoxLayout *hlo = new QHBoxLayout();
        hlo->addWidget(lb);
        hlo->addWidget(sb1);
        lo->addLayout(hlo);
      }
    }
    ZDoubleSpinBox *sb2 = new ZDoubleSpinBox();
    sb2->setRange(m_min[1], m_max[1]);
    sb2->setValue(m_value[1]);
    sb2->setSingleStep(m_step);
    sb2->setDecimals(m_decimal);
    sb2->setPrefix(m_prefix);
    sb2->setSuffix(m_suffix);
    connect(sb2, SIGNAL(valueChanged(double)), this, SLOT(setValue2(double)));
    connect(this, SIGNAL(value2Changed(double)), sb2, SLOT(setValue(double)));
    if (m_nameOfEachValue.at(1).isEmpty()) {
      lo->addWidget(sb2);
    } else {
      QLabel *lb = new QLabel(m_nameOfEachValue[1]);
      lb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
      lb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      if (m_widgetOrientation == Qt::Horizontal) {
        lo->addWidget(lb);
        lo->addWidget(sb2);
      } else {
        QHBoxLayout *hlo = new QHBoxLayout();
        hlo->addWidget(lb);
        hlo->addWidget(sb2);
        lo->addLayout(hlo);
      }
    }
    ZDoubleSpinBox *sb3 = new ZDoubleSpinBox();
    sb3->setRange(m_min[2], m_max[2]);
    sb3->setValue(m_value[2]);
    sb3->setSingleStep(m_step);
    sb3->setDecimals(m_decimal);
    sb3->setPrefix(m_prefix);
    sb3->setSuffix(m_suffix);
    connect(sb3, SIGNAL(valueChanged(double)), this, SLOT(setValue3(double)));
    connect(this, SIGNAL(value3Changed(double)), sb3, SLOT(setValue(double)));
    if (m_nameOfEachValue.at(2).isEmpty()) {
      lo->addWidget(sb3);
    } else {
      QLabel *lb = new QLabel(m_nameOfEachValue[2]);
      lb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
      lb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      if (m_widgetOrientation == Qt::Horizontal) {
        lo->addWidget(lb);
        lo->addWidget(sb3);
      } else {
        QHBoxLayout *hlo = new QHBoxLayout();
        hlo->addWidget(lb);
        hlo->addWidget(sb3);
        lo->addLayout(hlo);
      }
    }
    ZDoubleSpinBox *sb4 = new ZDoubleSpinBox();
    sb4->setRange(m_min[3], m_max[3]);
    sb4->setValue(m_value[3]);
    sb4->setSingleStep(m_step);
    sb4->setDecimals(m_decimal);
    sb4->setPrefix(m_prefix);
    sb4->setSuffix(m_suffix);
    connect(sb4, SIGNAL(valueChanged(double)), this, SLOT(setValue4(double)));
    connect(this, SIGNAL(value4Changed(double)), sb4, SLOT(setValue(double)));
    if (m_nameOfEachValue.at(3).isEmpty()) {
      lo->addWidget(sb4);
    } else {
      QLabel *lb = new QLabel(m_nameOfEachValue[3]);
      lb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
      lb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      if (m_widgetOrientation == Qt::Horizontal) {
        lo->addWidget(lb);
        lo->addWidget(sb4);
      } else {
        QHBoxLayout *hlo = new QHBoxLayout();
        hlo->addWidget(lb);
        hlo->addWidget(sb4);
        lo->addLayout(hlo);
      }
    }
  } else {
    ZDoubleSpinBoxWithSlider *sbws1 = new ZDoubleSpinBoxWithSlider(m_value[0], m_min[0], m_max[0], m_step,
                                                                  m_decimal, m_tracking, m_prefix, m_suffix, parent);
    connect(sbws1, SIGNAL(valueChanged(double)), this, SLOT(setValue1(double)));
    connect(this, SIGNAL(value1Changed(double)), sbws1, SLOT(setValue(double)));
    if (m_nameOfEachValue.at(0).isEmpty()) {
      lo->addWidget(sbws1);
    } else {
      QLabel *lb = new QLabel(m_nameOfEachValue[0]);
      lb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
      lb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      if (m_widgetOrientation == Qt::Horizontal) {
        lo->addWidget(lb);
        lo->addWidget(sbws1);
      } else {
        QHBoxLayout *hlo = new QHBoxLayout();
        hlo->addWidget(lb);
        hlo->addWidget(sbws1);
        lo->addLayout(hlo);
      }
    }
    ZDoubleSpinBoxWithSlider *sbws2 = new ZDoubleSpinBoxWithSlider(m_value[1], m_min[1], m_max[1], m_step,
                                                                  m_decimal, m_tracking, m_prefix, m_suffix, parent);
    connect(sbws2, SIGNAL(valueChanged(double)), this, SLOT(setValue2(double)));
    connect(this, SIGNAL(value2Changed(double)), sbws2, SLOT(setValue(double)));
    if (m_nameOfEachValue.at(1).isEmpty()) {
      lo->addWidget(sbws2);
    } else {
      QLabel *lb = new QLabel(m_nameOfEachValue[1]);
      lb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
      lb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      if (m_widgetOrientation == Qt::Horizontal) {
        lo->addWidget(lb);
        lo->addWidget(sbws2);
      } else {
        QHBoxLayout *hlo = new QHBoxLayout();
        hlo->addWidget(lb);
        hlo->addWidget(sbws2);
        lo->addLayout(hlo);
      }
    }
    ZDoubleSpinBoxWithSlider *sbws3 = new ZDoubleSpinBoxWithSlider(m_value[2], m_min[2], m_max[2], m_step,
                                                                  m_decimal, m_tracking, m_prefix, m_suffix, parent);
    connect(sbws3, SIGNAL(valueChanged(double)), this, SLOT(setValue3(double)));
    connect(this, SIGNAL(value3Changed(double)), sbws3, SLOT(setValue(double)));
    if (m_nameOfEachValue.at(2).isEmpty()) {
      lo->addWidget(sbws3);
    } else {
      QLabel *lb = new QLabel(m_nameOfEachValue[2]);
      lb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
      lb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      if (m_widgetOrientation == Qt::Horizontal) {
        lo->addWidget(lb);
        lo->addWidget(sbws3);
      } else {
        QHBoxLayout *hlo = new QHBoxLayout();
        hlo->addWidget(lb);
        hlo->addWidget(sbws3);
        lo->addLayout(hlo);
      }
    }
    ZDoubleSpinBoxWithSlider *sbws4 = new ZDoubleSpinBoxWithSlider(m_value[3], m_min[3], m_max[3], m_step,
                                                                  m_decimal, m_tracking, m_prefix, m_suffix, parent);
    connect(sbws4, SIGNAL(valueChanged(double)), this, SLOT(setValue4(double)));
    connect(this, SIGNAL(value4Changed(double)), sbws4, SLOT(setValue(double)));
    if (m_nameOfEachValue.at(3).isEmpty()) {
      lo->addWidget(sbws4);
    } else {
      QLabel *lb = new QLabel(m_nameOfEachValue[3]);
      lb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
      lb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      if (m_widgetOrientation == Qt::Horizontal) {
        lo->addWidget(lb);
        lo->addWidget(sbws4);
      } else {
        QHBoxLayout *hlo = new QHBoxLayout();
        hlo->addWidget(lb);
        hlo->addWidget(sbws4);
        lo->addLayout(hlo);
      }
    }
  }

  w->setLayout(lo);
  return w;
}

} // namespace nim
