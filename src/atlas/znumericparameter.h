#pragma once

#include "zparameter.h"
#include "zglmutils.h"
#include <limits>

namespace nim {

template<class T>
class ZNumericParameter : public ZSingleValueParameter<T>
{
public:
  ZNumericParameter(const QString& name, T value, T min, T max, QObject* parent = nullptr)
    : ZSingleValueParameter<T>(name, value, parent)
    , m_min(min)
    , m_max(max)
  {
    if (std::numeric_limits<T>::is_integer) {
      m_step = 1;
      m_decimal = 0;
    } else {
      m_step = static_cast<T>(.001);
      m_decimal = 3;
    }
    m_tracking = true;
    if (this->m_value < m_min) {
      this->m_value = m_min;
    }
    if (this->m_value > m_max) {
      this->m_value = m_max;
    }
  }

  void setSingleStep(T v)
  {
    m_step = v;
  }

  void setTracking(bool t)
  {
    m_tracking = t;
  }

  void setDecimal(int d)
  {
    m_decimal = d;
  }

  void setRange(T min, T max)
  {
    if (min != m_min || max != m_max) {
      m_min = min;
      m_max = max;
      changeRange();
    }
  }

  void setPrefix(const QString& pre)
  {
    m_prefix = pre;
  }

  void setSuffix(const QString& suf)
  {
    m_suffix = suf;
  }

  [[nodiscard]] T rangeMin() const
  {
    return m_min;
  }

  [[nodiscard]] T rangeMax() const
  {
    return m_max;
  }

  // ZParameter interface

public:
  void setSameAs(const ZParameter& rhs) override
  {
    CHECK(this->isSameType(rhs));
    const auto* src = static_cast<const ZNumericParameter<T>*>(&rhs);
    setSingleStep(src->m_step);
    setDecimal(src->m_decimal);
    setTracking(src->m_tracking);
    setRange(src->m_min, src->m_max);
    this->set(src->get());
    ZParameter::setSameAs(rhs);
  }

  void interpolate(const ZParameter& prev, double progress, ZParameter& dest) override
  {
    CHECK(this->isSameType(prev) && this->isSameType(dest));
    const auto& prevPara = static_cast<const ZNumericParameter<T>&>(prev);
    auto& desPara = static_cast<ZNumericParameter<T>&>(dest);
    desPara.set(glm::mix(prevPara.get(), this->m_value, progress));
  }

  [[nodiscard]] json::value jsonValue() const override
  {
    return json::value_from(this->m_value);
  }

  void readValue(const json::value& jsonValue) override
  {
    T v;
    if (jsonValue.is_string()) {
      toVal(jsonValue.get_string(), v);
    } else {
      v = json::value_to<T>(jsonValue);
    }
    this->set(v);
  }

protected:
  void makeValid(T& value) const override
  {
    if (value < m_min) {
      value = m_min;
    }
    if (value > m_max) {
      value = m_max;
    }
  }

  // inherite this to notify associated widgets about the range change (Q_EMIT a signal)
  virtual void changeRange() {}

protected:
  T m_min;
  T m_max;
  T m_step;
  bool m_tracking;
  int m_decimal;

  QString m_prefix;
  QString m_suffix;
};

class ZIntParameter : public ZNumericParameter<int>
{
  Q_OBJECT

public:
  explicit ZIntParameter(const QString& name, QObject* parent = nullptr);

  ZIntParameter(const QString& name, int value, int min, int max, QObject* parent = nullptr);

  void setValue(int v);

Q_SIGNALS:
  void valueWillChange(int);

  void intChanged(int);

  void rangeChanged(int min, int max);

protected:
  void beforeChange(int& value) override;

  void afterChange(int& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;

  void changeRange() override;
};

class ZDoubleParameter : public ZNumericParameter<double>
{
  Q_OBJECT

public:
  explicit ZDoubleParameter(const QString& name, QObject* parent = nullptr);

  ZDoubleParameter(const QString& name, double value, double min, double max, QObject* parent = nullptr);

  void setValue(double v);

Q_SIGNALS:
  void valueWillChange(double);

  void doubleChanged(double);

  void rangeChanged(double min, double max);

protected:
  void beforeChange(double& value) override;

  void afterChange(double& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;

  void changeRange() override;
};

class ZFloatParameter : public ZNumericParameter<float>
{
  Q_OBJECT

public:
  explicit ZFloatParameter(const QString& name, QObject* parent = nullptr);

  ZFloatParameter(const QString& name, float value, float min, float max, QObject* parent = nullptr);

  void setValue(double v);

Q_SIGNALS:
  void valueWillChange(double);

  void floatChanged(double);

  void rangeChanged(double min, double max);

protected:
  void beforeChange(float& value) override;

  void afterChange(float& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;

  void changeRange() override;
};

//---------------------------------------------------------------------------------------------------

template<class T>
class ZNumericVectorParameter : public ZSingleValueParameter<T>
{
public:
  ZNumericVectorParameter(const QString& name, T value, T min, T max, QObject* parent = nullptr)
    : ZSingleValueParameter<T>(name, value, parent)
    , m_min(min)
    , m_max(max)
    , m_widgetOrientation(Qt::Vertical)
  {
    if (std::numeric_limits<typename T::value_type>::is_integer) {
      m_step = 1;
      m_decimal = 0;
    } else {
      m_step = static_cast<typename T::value_type>(.001);
      m_decimal = 3;
    }
    m_tracking = true;
    for (size_t i = 0; i < this->m_value.length(); ++i) {
      if (this->m_value[i] < m_min[i]) {
        this->m_value[i] = m_min[i];
      }
      if (this->m_value[i] > m_max[i]) {
        this->m_value[i] = m_max[i];
      }
      m_nameOfEachValue.emplace_back("");
    }
  }

  void setSingleStep(typename T::value_type v)
  {
    m_step = v;
  }

  void setTracking(bool t)
  {
    m_tracking = t;
  }

  void setDecimal(int d)
  {
    m_decimal = d;
  }

  // default is vertical
  void setWidgetOrientation(Qt::Orientation o)
  {
    m_widgetOrientation = o;
  }

  void setNameForEachValue(const std::vector<QString>& other)
  {
    CHECK(other.size() >= this->m_value.length());
    m_nameOfEachValue = other;
  }

  // for some widget style all subwidgets be bound by a groupbox
  // default is empty
  void setGroupBoxName(const QString& name)
  {
    m_groupBoxName = name;
  }

  void setRange(T min, T max)
  {
    if (min != m_min || max != m_max) {
      m_min = min;
      m_max = max;
      changeRange();
    }
  }

  [[nodiscard]] T rangeMin() const
  {
    return m_min;
  }

  [[nodiscard]] T rangeMax() const
  {
    return m_max;
  }

  void setPrefix(const QString& pre)
  {
    m_prefix = pre;
  }

  void setSuffix(const QString& suf)
  {
    m_suffix = suf;
  }

  // inherite this to notify associated widgets about the range change (Q_EMIT a signal)
  virtual void changeRange() {}

  // ZParameter interface

public:
  void setSameAs(const ZParameter& rhs) override
  {
    CHECK(this->isSameType(rhs));
    const auto* src = static_cast<const ZNumericVectorParameter<T>*>(&rhs);
    setSingleStep(src->m_step);
    setDecimal(src->m_decimal);
    setTracking(src->m_tracking);
    m_min = src->m_min;
    m_max = src->m_max;
    setWidgetOrientation(src->m_widgetOrientation);
    setGroupBoxName(src->m_groupBoxName);
    setNameForEachValue(src->m_nameOfEachValue);
    this->set(src->get());
    ZParameter::setSameAs(rhs);
  }

  void interpolate(const ZParameter& prev, double progress, ZParameter& dest) override
  {
    CHECK(this->isSameType(prev) && this->isSameType(dest));
    const auto& prevPara = static_cast<const ZNumericVectorParameter<T>&>(prev);
    auto& desPara = static_cast<ZNumericVectorParameter<T>&>(dest);
    desPara.set(glm::mix(prevPara.get(), this->m_value, progress));
  }

  [[nodiscard]] json::value jsonValue() const override
  {
    return json::value_from(this->m_value);
  }

  void readValue(const json::value& jsonValue) override
  {
    T v;
    if (jsonValue.is_string()) {
      toVal(jsonValue.get_string(), v);
    } else {
      v = json::value_to<T>(jsonValue);
    }
    this->set(v);
  }

protected:
  void makeValid(T& value) const override
  {
    for (size_t i = 0; i < this->m_value.length(); ++i) {
      if (value[i] < m_min[i]) {
        value[i] = m_min[i];
      }
      if (value[i] > m_max[i]) {
        value[i] = m_max[i];
      }
    }
  }

protected:
  T m_min;
  T m_max;
  typename T::value_type m_step;
  bool m_tracking;
  int m_decimal;
  Qt::Orientation m_widgetOrientation;
  std::vector<QString> m_nameOfEachValue; // default is empty string for each value
  QString m_groupBoxName;

  QString m_prefix;
  QString m_suffix;
};

class ZVec2Parameter : public ZNumericVectorParameter<glm::vec2>
{
  Q_OBJECT

public:
  explicit ZVec2Parameter(const QString& name, QObject* parent = nullptr);

  ZVec2Parameter(const QString& name,
                 glm::vec2 value,
                 glm::vec2 min = glm::vec2(0.f),
                 glm::vec2 max = glm::vec2(1.f),
                 QObject* parent = nullptr);

  void setValue1(double v);

  void setValue2(double v);

Q_SIGNALS:
  void value1WillChange(double);

  void value2WillChange(double);

protected:
  void beforeChange(glm::vec2& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;
};

class ZVec3Parameter : public ZNumericVectorParameter<glm::vec3>
{
  Q_OBJECT

public:
  explicit ZVec3Parameter(const QString& name, QObject* parent = nullptr);

  ZVec3Parameter(const QString& name,
                 glm::vec3 value,
                 glm::vec3 min = glm::vec3(0.f),
                 glm::vec3 max = glm::vec3(1.f),
                 QObject* parent = nullptr);

  void setValue1(double v);

  void setValue2(double v);

  void setValue3(double v);

Q_SIGNALS:
  void value1WillChange(double);

  void value2WillChange(double);

  void value3WillChange(double);

  void range1Changed(double min, double max);

  void range2Changed(double min, double max);

  void range3Changed(double min, double max);

protected:
  void beforeChange(glm::vec3& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;

  void changeRange() override;
};

class ZVec4Parameter : public ZNumericVectorParameter<glm::vec4>
{
  Q_OBJECT

public:
  explicit ZVec4Parameter(const QString& name, QObject* parent = nullptr);

  ZVec4Parameter(const QString& name,
                 glm::vec4 value,
                 glm::vec4 min = glm::vec4(0.f),
                 glm::vec4 max = glm::vec4(1.f),
                 QObject* parent = nullptr);

  void setValue1(double v);

  void setValue2(double v);

  void setValue3(double v);

  void setValue4(double v);

Q_SIGNALS:
  void value1WillChange(double);

  void value2WillChange(double);

  void value3WillChange(double);

  void value4WillChange(double);

protected:
  void beforeChange(glm::vec4& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;
};

class ZDVec2Parameter : public ZNumericVectorParameter<glm::dvec2>
{
  Q_OBJECT

public:
  explicit ZDVec2Parameter(const QString& name, QObject* parent = nullptr);

  ZDVec2Parameter(const QString& name,
                  glm::dvec2 value,
                  glm::dvec2 min = glm::dvec2(0.f),
                  glm::dvec2 max = glm::dvec2(1.f),
                  QObject* parent = nullptr);

  void setValue1(double v);

  void setValue2(double v);

Q_SIGNALS:
  void value1WillChange(double);

  void value2WillChange(double);

protected:
  void beforeChange(glm::dvec2& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;
};

class ZDVec3Parameter : public ZNumericVectorParameter<glm::dvec3>
{
  Q_OBJECT

public:
  explicit ZDVec3Parameter(const QString& name, QObject* parent = nullptr);

  ZDVec3Parameter(const QString& name,
                  glm::dvec3 value,
                  glm::dvec3 min = glm::dvec3(0.f),
                  glm::dvec3 max = glm::dvec3(1.f),
                  QObject* parent = nullptr);

  void setValue1(double v);

  void setValue2(double v);

  void setValue3(double v);

Q_SIGNALS:
  void value1WillChange(double);

  void value2WillChange(double);

  void value3WillChange(double);

protected:
  void beforeChange(glm::dvec3& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;
};

class ZDVec4Parameter : public ZNumericVectorParameter<glm::dvec4>
{
  Q_OBJECT

public:
  explicit ZDVec4Parameter(const QString& name, QObject* parent = nullptr);

  ZDVec4Parameter(const QString& name,
                  glm::dvec4 value,
                  glm::dvec4 min = glm::dvec4(0.f),
                  glm::dvec4 max = glm::dvec4(1.f),
                  QObject* parent = nullptr);

  void setValue1(double v);

  void setValue2(double v);

  void setValue3(double v);

  void setValue4(double v);

Q_SIGNALS:
  void value1WillChange(double);

  void value2WillChange(double);

  void value3WillChange(double);

  void value4WillChange(double);

protected:
  void beforeChange(glm::dvec4& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;
};

class ZIVec2Parameter : public ZNumericVectorParameter<glm::ivec2>
{
  Q_OBJECT

public:
  explicit ZIVec2Parameter(const QString& name, QObject* parent = nullptr);

  ZIVec2Parameter(const QString& name, glm::ivec2 value, glm::ivec2 min, glm::ivec2 max, QObject* parent = nullptr);

  void setValue1(int v);

  void setValue2(int v);

Q_SIGNALS:
  void value1WillChange(int);

  void value2WillChange(int);

protected:
  void beforeChange(glm::ivec2& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;
};

class ZIVec3Parameter : public ZNumericVectorParameter<glm::ivec3>
{
  Q_OBJECT

public:
  explicit ZIVec3Parameter(const QString& name, QObject* parent = nullptr);

  ZIVec3Parameter(const QString& name, glm::ivec3 value, glm::ivec3 min, glm::ivec3 max, QObject* parent = nullptr);

  void setValue1(int v);

  void setValue2(int v);

  void setValue3(int v);

Q_SIGNALS:
  void value1WillChange(int);

  void value2WillChange(int);

  void value3WillChange(int);

protected:
  void beforeChange(glm::ivec3& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;
};

//---------------------------------------------------------------------------------------------------

template<class T>
class ZNumericSpanParameter : public ZSingleValueParameter<T>
{
public:
  ZNumericSpanParameter(const QString& name,
                        T value,
                        typename T::value_type min,
                        typename T::value_type max,
                        QObject* parent = nullptr)
    : ZSingleValueParameter<T>(name, value, parent)
    , m_min(min)
    , m_max(max)
    , m_widgetOrientation(Qt::Horizontal)
  {
    if (std::numeric_limits<typename T::value_type>::is_integer) {
      m_step = 1;
      m_decimal = 0;
    } else {
      m_step = static_cast<typename T::value_type>(.01);
      m_decimal = 2;
    }
    m_tracking = true;
    for (auto i = 0; i < 2; ++i) {
      if (this->m_value[i] < m_min) {
        this->m_value[i] = m_min;
      }
      if (this->m_value[i] > m_max) {
        this->m_value[i] = m_max;
      }
      m_nameOfEachValue.emplace_back(i == 0 ? "low" : "high");
    }
    m_groupBoxName = "Range";
  }

  void setSingleStep(typename T::value_type v)
  {
    m_step = v;
  }

  void setTracking(bool t)
  {
    m_tracking = t;
  }

  void setDecimal(int d)
  {
    m_decimal = d;
  }

  // default is horizonal
  void setWidgetOrientation(Qt::Orientation o)
  {
    m_widgetOrientation = o;
  }

  void setNameForEachValue(const std::vector<QString>& other)
  {
    CHECK(other.size() >= 2);
    m_nameOfEachValue = other;
  }

  void setRange(typename T::value_type min, typename T::value_type max)
  {
    if (min <= max && (min != m_min || max != m_max)) {
      m_min = min;
      m_max = max;
      changeRange();
    }
  }

  void setRangeKeepIfMinMax(typename T::value_type min, typename T::value_type max)
  {
    if (min <= max && (min != m_min || max != m_max)) {
      // VLOG(1) << min << " " << max;
      // VLOG(1) << lowerValue() << " " << upperValue() << " " << m_min << " " << m_max;
      auto oldLowerValue = lowerValue();
      auto oldUpperValue = upperValue();
      bool keepLowerToMin = m_min == oldLowerValue;
      bool keepUpperToMax = m_max == oldUpperValue;
      // VLOG(1) << m_min << " " << m_max << " " << oldLowerValue << " " << oldUpperValue << " " << keepLowerToMin << "
      // " << keepUpperToMax;
      m_min = min;
      m_max = max;
      changeRange();
      typename T::value_type newLower = keepLowerToMin ? m_min : std::max(m_min, oldLowerValue);
      typename T::value_type newUpper = keepUpperToMax ? m_max : std::min(m_max, oldUpperValue);
      this->set(T(newLower, newUpper));
      // VLOG(1) << lowerValue() << " " << upperValue() << " " << m_min << " " << m_max;
    }
  }

  [[nodiscard]] T range() const
  {
    return T(m_min, m_max);
  }

  [[nodiscard]] typename T::value_type lowerValue() const
  {
    return this->m_value[0];
  }

  [[nodiscard]] typename T::value_type upperValue() const
  {
    return this->m_value[1];
  }

  [[nodiscard]] typename T::value_type minimum() const
  {
    return m_min;
  }

  [[nodiscard]] typename T::value_type maximum() const
  {
    return m_max;
  }

  // for some widget style all subwidgets be bound by a groupbox
  // default is empty
  void setGroupBoxName(const QString& name)
  {
    m_groupBoxName = name;
  }

  void setPrefix(const QString& pre)
  {
    m_prefix = pre;
  }

  void setSuffix(const QString& suf)
  {
    m_suffix = suf;
  }

  // ZParameter interface

public:
  void setSameAs(const ZParameter& rhs) override
  {
    CHECK(this->isSameType(rhs));
    const auto* src = static_cast<const ZNumericSpanParameter<T>*>(&rhs);
    setSingleStep(src->m_step);
    setDecimal(src->m_decimal);
    setTracking(src->m_tracking);
    setRange(src->m_min, src->m_max);
    setWidgetOrientation(src->m_widgetOrientation);
    setNameForEachValue(src->m_nameOfEachValue);
    this->set(src->get());
    ZParameter::setSameAs(rhs);
  }

  void interpolate(const ZParameter& prev, double progress, ZParameter& dest) override
  {
    CHECK(this->isSameType(prev) && this->isSameType(dest));
    const auto& prevPara = static_cast<const ZNumericSpanParameter<T>&>(prev);
    auto& desPara = static_cast<ZNumericSpanParameter<T>&>(dest);
    desPara.set(glm::mix(prevPara.get(), this->m_value, progress));
  }

  [[nodiscard]] json::value jsonValue() const override
  {
    return json::value_from(this->m_value);
  }

  void readValue(const json::value& jsonValue) override
  {
    T v;
    if (jsonValue.is_string()) {
      toVal(jsonValue.get_string(), v);
    } else {
      v = json::value_to<T>(jsonValue);
    }
    this->set(v);
  }

protected:
  void makeValid(T& value) const override
  {
    for (auto i = 0; i < 2; ++i) {
      if (value[i] < m_min) {
        value[i] = m_min;
      }
      if (value[i] > m_max) {
        value[i] = m_max;
      }
    }
    if (value[0] > value[1]) {
      std::swap(value[0], value[1]);
    }
  }

  // inherite this to notify associated widgets about the range change (Q_EMIT a signal)
  virtual void changeRange() {}

protected:
  typename T::value_type m_min;
  typename T::value_type m_max;
  typename T::value_type m_step;
  bool m_tracking;
  int m_decimal;
  Qt::Orientation m_widgetOrientation;
  std::vector<QString> m_nameOfEachValue;

  QString m_groupBoxName;

  QString m_prefix;
  QString m_suffix;
};

class ZIntSpanParameter : public ZNumericSpanParameter<glm::ivec2>
{
  Q_OBJECT

public:
  explicit ZIntSpanParameter(const QString& name, QObject* parent = nullptr);

  ZIntSpanParameter(const QString& name, glm::ivec2 value, int min, int max, QObject* parent = nullptr);

  void setLowerValue(int v);

  void setUpperValue(int v);

Q_SIGNALS:
  void lowerValueWillChange(int);

  void upperValueWillChange(int);

  void rangeChanged(int min, int max);

protected:
  void beforeChange(glm::ivec2& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;

  void changeRange() override;
};

class ZFloatSpanParameter : public ZNumericSpanParameter<glm::vec2>
{
  Q_OBJECT

public:
  explicit ZFloatSpanParameter(const QString& name, QObject* parent = nullptr);

  ZFloatSpanParameter(const QString& name, glm::vec2 value, float min, float max, QObject* parent = nullptr);

  void setLowerValue(double v);

  void setUpperValue(double v);

Q_SIGNALS:
  void lowerValueWillChange(double);

  void upperValueWillChange(double);

  void rangeChanged(double min, double max);

protected:
  void beforeChange(glm::vec2& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;

  void changeRange() override;
};

class ZDoubleSpanParameter : public ZNumericSpanParameter<glm::dvec2>
{
  Q_OBJECT

public:
  explicit ZDoubleSpanParameter(const QString& name, QObject* parent = nullptr);

  ZDoubleSpanParameter(const QString& name, glm::dvec2 value, double min, double max, QObject* parent = nullptr);

  void setLowerValue(double v);

  void setUpperValue(double v);

Q_SIGNALS:
  void lowerValueWillChange(double);

  void upperValueWillChange(double);

  void rangeChanged(double min, double max);

protected:
  void beforeChange(glm::dvec2& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;

  void changeRange() override;
};

} // namespace nim
