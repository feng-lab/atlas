#include "zquatparameter.h"

#include <QGroupBox>
#include <QBoxLayout>
#include "zspinbox.h"
#include <QLabel>
#include "zspinboxwithslider.h"

namespace nim {

ZQuatParameter::ZQuatParameter(const QString &name, QObject *parent)
  : ZNumericVectorParameter<glm::quat>(name, glm::quat(), glm::quat(std::numeric_limits<float>::lowest(),
                                                                    std::numeric_limits<float>::lowest(),
                                                                    std::numeric_limits<float>::lowest(),
                                                                    std::numeric_limits<float>::lowest()),
                                       glm::quat(std::numeric_limits<float>::max(),
                                                 std::numeric_limits<float>::max(),
                                                 std::numeric_limits<float>::max(),
                                                 std::numeric_limits<float>::max()), parent)
{
  addStyle("SPINBOX");
}

ZQuatParameter::ZQuatParameter(const QString &name, glm::quat value, QObject *parent)
  : ZNumericVectorParameter<glm::quat>(name, value, glm::quat(std::numeric_limits<float>::lowest(),
                                                              std::numeric_limits<float>::lowest(),
                                                              std::numeric_limits<float>::lowest(),
                                                              std::numeric_limits<float>::lowest()),
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
    emit value1WillChange(value[0]);
  if (value[1] != m_value[1])
    emit value2WillChange(value[1]);
  if (value[2] != m_value[2])
    emit value3WillChange(value[2]);
  if (value[3] != m_value[3])
    emit value4WillChange(value[3]);
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
    {
      ZDoubleSpinBox *sb1 = new ZDoubleSpinBox();
      sb1->setRange(m_min[0], m_max[0]);
      sb1->setValue(m_value[0]);
      sb1->setSingleStep(m_step);
      sb1->setDecimals(m_decimal);
      sb1->setPrefix(m_prefix);
      sb1->setSuffix(m_suffix);
      connect(sb1, qOverload<double>(&ZDoubleSpinBox::valueChanged), this, &ZQuatParameter::setValue1);
      connect(this, &ZQuatParameter::value1WillChange, sb1, &ZDoubleSpinBox::setValue);
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
    }
    {
      ZDoubleSpinBox *sb2 = new ZDoubleSpinBox();
      sb2->setRange(m_min[1], m_max[1]);
      sb2->setValue(m_value[1]);
      sb2->setSingleStep(m_step);
      sb2->setDecimals(m_decimal);
      sb2->setPrefix(m_prefix);
      sb2->setSuffix(m_suffix);
      connect(sb2, qOverload<double>(&ZDoubleSpinBox::valueChanged), this, &ZQuatParameter::setValue2);
      connect(this, &ZQuatParameter::value2WillChange, sb2, &ZDoubleSpinBox::setValue);
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
    }
    {
      ZDoubleSpinBox *sb3 = new ZDoubleSpinBox();
      sb3->setRange(m_min[2], m_max[2]);
      sb3->setValue(m_value[2]);
      sb3->setSingleStep(m_step);
      sb3->setDecimals(m_decimal);
      sb3->setPrefix(m_prefix);
      sb3->setSuffix(m_suffix);
      connect(sb3, qOverload<double>(&ZDoubleSpinBox::valueChanged), this, &ZQuatParameter::setValue3);
      connect(this, &ZQuatParameter::value3WillChange, sb3, &ZDoubleSpinBox::setValue);
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
    }
    {
      ZDoubleSpinBox *sb4 = new ZDoubleSpinBox();
      sb4->setRange(m_min[3], m_max[3]);
      sb4->setValue(m_value[3]);
      sb4->setSingleStep(m_step);
      sb4->setDecimals(m_decimal);
      sb4->setPrefix(m_prefix);
      sb4->setSuffix(m_suffix);
      connect(sb4, qOverload<double>(&ZDoubleSpinBox::valueChanged), this, &ZQuatParameter::setValue4);
      connect(this, &ZQuatParameter::value4WillChange, sb4, &ZDoubleSpinBox::setValue);
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
    }
  } else {
    {
      ZDoubleSpinBoxWithSlider *sbws1 = new ZDoubleSpinBoxWithSlider(m_value[0], m_min[0], m_max[0], m_step,
          m_decimal, m_tracking, m_prefix, m_suffix, parent);
      connect(sbws1, &ZDoubleSpinBoxWithSlider::valueChanged, this, &ZQuatParameter::setValue1);
      connect(this, &ZQuatParameter::value1WillChange, sbws1, &ZDoubleSpinBoxWithSlider::setValue);
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
    }
    {
      ZDoubleSpinBoxWithSlider *sbws2 = new ZDoubleSpinBoxWithSlider(m_value[1], m_min[1], m_max[1], m_step,
          m_decimal, m_tracking, m_prefix, m_suffix, parent);
      connect(sbws2, &ZDoubleSpinBoxWithSlider::valueChanged, this, &ZQuatParameter::setValue2);
      connect(this, &ZQuatParameter::value2WillChange, sbws2, &ZDoubleSpinBoxWithSlider::setValue);
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
    }
    {
      ZDoubleSpinBoxWithSlider *sbws3 = new ZDoubleSpinBoxWithSlider(m_value[2], m_min[2], m_max[2], m_step,
          m_decimal, m_tracking, m_prefix, m_suffix, parent);
      connect(sbws3, &ZDoubleSpinBoxWithSlider::valueChanged, this, &ZQuatParameter::setValue3);
      connect(this, &ZQuatParameter::value3WillChange, sbws3, &ZDoubleSpinBoxWithSlider::setValue);
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
    }
    {
      ZDoubleSpinBoxWithSlider *sbws4 = new ZDoubleSpinBoxWithSlider(m_value[3], m_min[3], m_max[3], m_step,
          m_decimal, m_tracking, m_prefix, m_suffix, parent);
      connect(sbws4, &ZDoubleSpinBoxWithSlider::valueChanged, this, &ZQuatParameter::setValue4);
      connect(this, &ZQuatParameter::value4WillChange, sbws4, &ZDoubleSpinBoxWithSlider::setValue);
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
  }

  w->setLayout(lo);
  return w;
}

} // namespace nim
